#include "camsource.h"

#include <QDebug>
#include <QTimer>
#include <QImage>
#include <QPainter>
#include <QDateTime>
#include <QElapsedTimer>
#include <QByteArray>

#ifdef HAVE_GST
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

// =====================================================================
//  CPU 预算（2026-11-20 按线程实测，RK3566 + 640x480 子码流）
// ---------------------------------------------------------------------
//  myui 总 ~50%，两条热点：
//    mppvideodec12:s  24%   解码输出 + 每帧 buffer 拷贝 + NV12→RGB 转换
//    主线程            24%   Qt 绘制（rebuildScaled 的缩放+裁切 + blit）
//  其它线程（rtspsrc / rtcp / jitterbuffer / mpp 内部）合计 <2%。
//
//  所以优化只有两个方向，本文件负责第一条：
//   A. 减少 streaming 线程的「转换 + 拷贝」→ 先尝试让 mppvideodec 直出
//      BGRx（硬件转色，m_setPixelFormat），不成再退回 NV12 软件转；
//      同时让解码器直接输出更小的尺寸（m_decW/H）来减少拷贝字节数。
//   B. 减少 UI 线程的「缩放 + 裁切」→ 见 tabpages.cpp 的 paintEvent：
//      已改成 drawImage(目标矩形, 源图, 源矩形) 一步到位。
//
//  限帧（m_fps）两边都受益，默认 15fps：监看够用，比 25fps 省 40%。
// =====================================================================

// ---------------------------------------------------------------------
//  NV12 → RGB32（顺带 step×step 下采样）
//  板上这个 videoconvert 只会「直通 NV12」：实测 RGB / BGRx / RGBx / RGBA /
//  I420 / YUY2 全部协商失败（字节数 0），videoscale 也缩不了 NV12；
//  所以软件兜底这条路必须留着。
//  下采样和转换合并成一次遍历，1280x720 只算 640x360 个点（1/4）。
// ---------------------------------------------------------------------
static QImage nv12ToRgb(const uchar *data, int w, int h, int step)
{
    if (step < 1)
        step = 1;
    const int W = w / step;
    const int H = h / step;
    if (W <= 0 || H <= 0)
        return QImage();

    QImage img(W, H, QImage::Format_RGB32);
    const uchar *Y  = data;
    const uchar *UV = data + w * h;      // NV12：Y 平面之后是交错的 UV，行宽 = w

    for (int j = 0; j < H; ++j) {
        const int sy = j * step;
        const uchar *yp  = Y + sy * w;
        const uchar *uvp = UV + (sy / 2) * w;
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(j));
        for (int i = 0; i < W; ++i) {
            const int sx = i * step;
            const int yv = yp[sx];
            // 2x2 块共用一个 UV 对；& ~1 把 sx 对齐到偶数
            const int u = int(uvp[sx & ~1])     - 128;
            const int v = int(uvp[(sx & ~1) + 1]) - 128;
            int r = yv + ((1436 * v) >> 10);
            int g = yv - ((352 * u + 731 * v) >> 10);
            int b = yv + ((1814 * u) >> 10);
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            line[i] = qRgb(r, g, b);
        }
    }
    return img;
}

// =====================================================================
//  GStreamer 回调（跑在 streaming 线程，绝不能碰 UI）
// =====================================================================
struct CamSourceCallbacks
{
    static GstFlowReturn onSample(GstAppSink *sink, gpointer user)
    {
        CamSource *self = static_cast<CamSource *>(user);

        // ⚠ 一次回调把队列抽干，只保留**最新**那帧。
        //   起因（2026-11-20 实测）：源稳定 20fps，但 appsink 只吐出 5.8fps，
        //   同一条链路换成 filesink 就是 19fps。原因是 new_sample 只在「有新
        //   buffer 入队」时触发，队列里残留的旧帧一直占着位置 → 新帧被 drop
        //   且不再回调。而 TCP 侧仍按 20fps 往里灌，多出来的全堆在内核 socket
        //   缓冲里 —— 画面因此倒退好几秒（用户说的「延时特别高」）。
        //   抽干 = 队列长度恒为 0，每个新帧都会触发回调，且永远只画最新的。
        GstSample *cur = gst_app_sink_try_pull_sample(sink, GST_SECOND / 10);
        if (!cur)
            return GST_FLOW_OK;
        GstSample *nx = nullptr;
        while ((nx = gst_app_sink_try_pull_sample(sink, 0)) != nullptr) {
            gst_sample_unref(cur);
            cur = nx;
        }

        GstBuffer *buf  = gst_sample_get_buffer(cur);
        GstCaps   *caps = gst_sample_get_caps(cur);
        int w = 0, h = 0, stride = 0;
        QString fmt;
        if (caps) {
            GstStructure *s = gst_caps_get_structure(caps, 0);
            gst_structure_get_int(s, "width",  &w);
            gst_structure_get_int(s, "height", &h);
            const gchar *f = gst_structure_get_string(s, "format");
            if (f)
                fmt = QString::fromLatin1(f);
            // 解码器输出的行跨度可能带对齐填充，必须按 caps 里的 stride 走，
            // 不能想当然用 w*4（一旦 width 不是 16 的倍数就会整幅错位）
            GstVideoInfo vi;
            if (gst_video_info_from_caps(&vi, caps))
                stride = GST_VIDEO_INFO_PLANE_STRIDE(&vi, 0);
        }

        GstMapInfo map;
        QElapsedTimer et;
        et.start();
        if (w > 0 && h > 0 && gst_buffer_map(buf, &map, GST_MAP_READ)) {
            self->m_msMap += et.nsecsElapsed() / 1e6;
            et.restart();
            // 限帧：先去重/限频再转换。屏幕上的画面区只有 434x384 物理像素，
            // 25fps 纯属浪费；这是省 CPU 最直接的一刀。
            const int period = self->m_fps > 0 ? (1000 / self->m_fps) : 0;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            ++self->m_nIn;
            if (period > 0 && now - self->m_lastMs < period) {
                gst_buffer_unmap(buf, &map);
                gst_sample_unref(cur);
                self->reportFps(now);
                return GST_FLOW_OK;
            }
            self->m_lastMs = now;

            QImage out;
            if (fmt == QLatin1String("NV12")) {
                // ⚠⚠ 必须先把整帧 memcpy 到普通内存再转色。
                //   mppvideodec 输出的 NV12 是 dma-buf 映射（不可缓存），逐字节读
                //   每个字节都要走一次慢速事务：实测 800x448 直接转色 **171ms/帧**
                //   （480ns/像素），整条链路被拖到 6fps、TCP 侧跟着积压 → 画面
                //   倒退好几秒。整块 memcpy（大块顺序拷贝，硬件能跑满带宽）只要
                //   几毫秒，之后从缓存里转就恢复正常的几毫秒。
                const size_t need = size_t(w) * h * 3 / 2;
                if (size_t(self->m_nv12.size()) < need)
                    self->m_nv12.resize(int(need));
                memcpy(self->m_nv12.data(), map.data, need);
                self->m_msCopy += et.nsecsElapsed() / 1e6;
                et.restart();

                // 下采样倍数按源宽度自适应：>1000 就取 2×2（1080p/720p），
                // 否则 1:1（640x480 子码流本来就只比画面区略大）。
                const int step = (w > 1000) ? 2 : 1;
                out = nv12ToRgb(reinterpret_cast<const uchar *>(self->m_nv12.constData()),
                                w, h, step);
            } else if (fmt == QLatin1String("BGRx") || fmt == QLatin1String("BGRA")) {
                // 内存里是 B,G,R,X → 小端 32 位读出来就是 0xXXRRGGBB，
                // 正是 Format_RGB32 的排布（最高字节被当不透明忽略）。
                if (stride <= 0)
                    stride = w * 4;
                out = QImage(map.data, w, h, stride, QImage::Format_RGB32).copy();
            } else if (fmt == QLatin1String("RGB")) {
                if (stride <= 0)
                    stride = w * 3;
                out = QImage(map.data, w, h, stride, QImage::Format_RGB888).copy();
            }
            self->m_msConv += et.nsecsElapsed() / 1e6;
            et.restart();
            gst_buffer_unmap(buf, &map);
            self->m_msUnmap += et.nsecsElapsed() / 1e6;

            // 节流：UI 还没画完上一帧就丢掉这一帧（否则 queued signal 会堆积）
            if (!out.isNull() && self->m_pending == 0) {
                self->m_pending = 1;
                ++self->m_nOut;
                emit self->frameReady(out);
            }
        }
        gst_sample_unref(cur);
        self->reportFps(QDateTime::currentMSecsSinceEpoch());
        return GST_FLOW_OK;
    }
};

// 每 5 秒报一次实际进/出帧率。CPU 异常时先看这一行：
//   in 变大 → 源帧率变高（解码/拷贝量按比例涨）
//   in 正常但 CPU 高 → 每帧变贵了（转换/绘制/协商降级）
void CamSource::reportFps(qint64 now)
{
    if (m_statMs == 0) {
        m_statMs = now;
        return;
    }
    if (now - m_statMs < 5000)
        return;
    const int n = qMax(1, m_nIn);
    qDebug("[cam] fps in=%d out=%d /%lldms | map=%.1f conv=%.1f unmap=%.1f ms/帧",
           m_nIn, m_nOut, now - m_statMs,
           m_msMap / n, m_msConv / n, m_msUnmap / n);
    qDebug("[cam] copy=%.1f ms/帧", m_msCopy / n);
    m_nIn = m_nOut = 0;
    m_msMap = m_msConv = m_msUnmap = m_msCopy = 0.0;
    m_statMs = now;
}

static bool g_gstReady = false;

#endif // HAVE_GST

// =====================================================================

CamSource::CamSource(QObject *parent) : QObject(parent)
{
#ifdef HAVE_GST
    if (!g_gstReady) {
        gst_init(nullptr, nullptr);
        g_gstReady = true;
    }
#endif

    // ⚠ 这里不能用 gst_bus_add_watch：它挂在 GLib 的 default main context 上，
    //   Qt 的事件循环不会去跑那个 loop，ERROR/EOS 永远收不到。改成轮询。
    m_busTimer = new QTimer(this);
    m_busTimer->setInterval(250);
    connect(m_busTimer, &QTimer::timeout, this, &CamSource::pollBus);

    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, [this] {
        if (!m_url.isEmpty())
            start(m_url, m_w, m_h);
    });
}

CamSource::~CamSource()
{
    teardown();
}

void CamSource::setPixelFormat(const QString &fmt)
{
    // 只认这几种；写错的按 NV12 处理（软件兜底永远能出图）
    const QString f = fmt.trimmed();
    if (f.compare(QLatin1String("BGRx"), Qt::CaseInsensitive) == 0)
        m_fmt = QStringLiteral("BGRx");
    else if (f.compare(QLatin1String("BGRA"), Qt::CaseInsensitive) == 0)
        m_fmt = QStringLiteral("BGRA");
    else if (f.compare(QLatin1String("RGB"), Qt::CaseInsensitive) == 0)
        m_fmt = QStringLiteral("RGB");
    else
        m_fmt = QStringLiteral("NV12");
}

void CamSource::setTransport(const QString &t)
{
    const QString v = t.trimmed().toLower();
    if (v == QLatin1String("udp") || v == QLatin1String("tcp"))
        m_transport = v;
    else
        m_transport = QStringLiteral("auto");
}

void CamSource::teardown()
{
    m_running = false;
    m_busTimer->stop();

#ifdef HAVE_GST
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_appsink = nullptr;
    m_bus = nullptr;
#endif
}

bool CamSource::start(const QString &url, int outW, int outH)
{
    teardown();
    m_url = url;
    m_w = outW;
    m_h = outH;
    m_pending = 0;
    m_lastMs = 0;

#ifdef HAVE_GST
    if (url.isEmpty())
        return false;

    QString safe = url;
    safe.replace(QLatin1Char('"'), QStringLiteral("\\\""));

    // 解码器参数：
    //   m_fmt  = NV12 时让它原样出 YUV，转色/缩放在 Qt 侧做；
    //          = BGRx 等时让 mpp 内部用硬件转色，省掉整帧软件转换。
    //   m_decW/H 让解码器直接输出小尺寸（mppvideodec 的 width/height 属性，
    //   0 = 原始），这样「解码→内存」的拷贝字节数按比例下降。
    QString dec = QStringLiteral("mppvideodec");
    if (m_decW > 0 && m_decH > 0)
        dec += QStringLiteral(" width=%1 height=%2").arg(m_decW).arg(m_decH);

    // 没有 videoconvert：实测 mppvideodec 直出就能被 appsink map，
    // 省掉一次整帧拷贝（640x480 NV12 一帧 460KB，25fps 就是 11MB/s 白搬）。
    // 传输/缓冲：这四项全是「延迟」而不是「CPU」的开关，见下面注释
    //   latency            —— rtspsrc 默认 **2000ms**，不显式压到 0 就是 2 秒延迟
    //   protocols          —— 强制 tcp 会因丢包重传 + 队头阻塞把延迟堆到秒级；
    //                         auto 让 rtspsrc 自己按 udp→tcp 回退（默认就是 udp 优先）
    //   do-retransmission  —— 走 udp 时向摄像头要重传包，等于主动等一个 RTT
    //   drop-on-latency    —— jitterbuffer 超过 latency 就丢旧包，绝不排队积压
    QString tr;
    if (m_transport != QLatin1String("auto"))
        tr = QStringLiteral(" protocols=") + m_transport;
    const QString dpl = m_dropOnLat ? QStringLiteral(" drop-on-latency=true")
                                    : QString();
    // ⚠ 队列只留最新一帧：源 20fps、我们消费多少就吐多少，队列一积压
    //   延迟立刻从「一帧」涨到「几秒」（TCP 侧还会跟着把内核缓冲塞满）。
    const QString sinkDesc = QStringLiteral(
        "appsink name=camsink max-buffers=%1 drop=%2 sync=false")
        .arg(m_sinkBufs)
        .arg(m_sinkDrop ? QStringLiteral("true") : QStringLiteral("false"));

    const QString desc = QStringLiteral(
        "rtspsrc location=\"%1\" latency=%2%3%4 "
        "do-retransmission=false "
        "! rtph264depay ! h264parse ! %5 "
        "! video/x-raw,format=%6 "
        "! %7")
        .arg(safe)
        .arg(m_latencyMs)
        .arg(tr)
        .arg(dpl)
        .arg(dec)
        .arg(m_fmt)
        .arg(sinkDesc);

    GError *err = nullptr;
    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!m_pipeline || err) {
        const QString why = err ? QString::fromUtf8(err->message)
                                : QStringLiteral("pipeline build failed");
        if (err)
            g_error_free(err);
        qWarning("[cam] pipeline 失败: %s", qPrintable(why));
        teardown();
        emit failed(why);
        return false;
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "camsink");
    if (!sink) {
        qWarning("[cam] 找不到 appsink");
        teardown();
        emit failed(QStringLiteral("no appsink"));
        return false;
    }
    m_appsink = GST_APP_SINK(sink);

    GstAppSinkCallbacks cbs = {};
    cbs.new_sample = CamSourceCallbacks::onSample;
    gst_app_sink_set_callbacks(m_appsink, &cbs, this, nullptr);

    m_bus = gst_element_get_bus(m_pipeline);

    const GstStateChangeReturn st =
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (st == GST_STATE_CHANGE_FAILURE) {
        qWarning("[cam] 无法进入 PLAYING");
        teardown();
        emit failed(QStringLiteral("state change failed"));
        return false;
    }

    m_running = true;
    m_busTimer->start();
    qDebug("[cam] start %s -> fmt=%s fps=%d dec=%dx%d %s lat=%d",
           qPrintable(url), qPrintable(m_fmt), m_fps, m_decW, m_decH,
           qPrintable(m_transport), m_latencyMs);
    return true;

#else
    Q_UNUSED(url)
    Q_UNUSED(outW)
    Q_UNUSED(outH)

    // host 版没有 GStreamer 开发包：生成一张会动的测试图，
    // 用于在虚拟机上验证「画面是否落在设计稿矩形内、cover 裁剪是否正确」。
    m_running = true;
    qDebug("[cam] host 模式：假帧 %dx%d", outW, outH);
    QTimer *fake = new QTimer(this);
    fake->setInterval(33);
    connect(fake, &QTimer::timeout, this, [this, fake] {
        static int n = 0;
        QImage img(m_w, m_h, QImage::Format_RGB888);
        img.fill(QColor(0x1c, 0x2a, 0x33));

        QPainter p(&img);
        p.setPen(QPen(QColor(0x33, 0x7f, 0xbf), 2));
        for (int x = 0; x < m_w; x += 40)
            p.drawLine(x, 0, x, m_h);
        for (int y = 0; y < m_h; y += 40)
            p.drawLine(0, y, m_w, y);
        p.setPen(QPen(QColor(0xe6, 0xe6, 0xe6), 3));
        p.drawLine(m_w / 2, 0, m_w / 2, m_h);
        p.drawLine(0, m_h / 2, m_w, m_h / 2);
        p.drawRect(0, 0, m_w - 1, m_h - 1);

        QFont f = p.font();
        f.setPixelSize(m_h / 8);
        p.setFont(f);
        p.setPen(QColor(0xff, 0xd2, 0x66));
        p.drawText(QRect(0, m_h / 2 + 8, m_w, m_h / 2 - 8),
                   Qt::AlignHCenter | Qt::AlignTop,
                   QStringLiteral("CAM TEST %1").arg(n++));
        p.end();

        if (m_pending == 0) {
            m_pending = 1;
            emit frameReady(img);
        }
    });
    fake->start();
    Q_UNUSED(fake)
    return true;
#endif
}

void CamSource::stop()
{
    if (!m_running)
        return;
#ifdef HAVE_GST
    qDebug("[cam] stop");
#endif
    m_retry->stop();
    m_url.clear();
    teardown();
}

void CamSource::ackFrame()
{
    m_pending = 0;
}

void CamSource::pollBus()
{
#ifdef HAVE_GST
    if (!m_bus || !m_running)
        return;

    GstMessage *msg = gst_bus_timed_pop_filtered(
        m_bus, 0, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    if (!msg)
        return;

    QString why;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *err = nullptr;
        gchar  *dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        if (err) {
            why = QString::fromUtf8(err->message);
            g_error_free(err);
        }
        qWarning("[cam] bus error: %s (%s)", qPrintable(why),
                 dbg ? dbg : "");
        g_free(dbg);
    } else {
        why = QStringLiteral("stream ended");
        qWarning("[cam] EOS");
    }
    gst_message_unref(msg);

    const QString keepUrl = m_url;
    const int     kw = m_w, kh = m_h;
    teardown();
    m_url = keepUrl;
    emit failed(why);
    scheduleRetry();
    Q_UNUSED(kw)
    Q_UNUSED(kh)
#else
    // 假帧模式不需要
#endif
}

void CamSource::scheduleRetry()
{
    if (m_url.isEmpty())
        return;
    // 1s → 2s → 5s 退避；摄像头重启、网线插拔都会走到这里
    const int steps[3] = {1000, 2000, 5000};
    const int ms = steps[qMin(m_failCount, 2)];
    ++m_failCount;
    qDebug("[cam] %d ms 后重连（第 %d 次）", ms, m_failCount);
    m_retry->start(ms);
}

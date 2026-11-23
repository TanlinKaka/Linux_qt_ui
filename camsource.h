#ifndef CAMSOURCE_H
#define CAMSOURCE_H

// =====================================================================
//  CamSource —— IP 摄像头取帧（RTSP / H.264 → 硬解 → RGB QImage）
//
//  为什么不用 QtMultimedia：板上 /usr/lib/qt/plugins/videonode/ 是空的，
//  QCameraViewfinder / QGraphicsVideoItem 拿不到渲染节点，出不了图；况且
//  原生视频窗口不参与 QGraphicsView 的 rotate270 + scale 变换。所以这里
//  直接把帧解成 QImage，交给页面 paintEvent 画，翻转/缩放/截图全都自动对。
//
//  ⚠ 两个 GStreamer 坑（都已规避）：
//   1. gst_bus_add_watch 依赖 GLib main loop，Qt 的事件循环不跑它 → 永远收不到
//      ERROR/EOS。这里改用 QTimer 轮询 gst_bus_timed_pop_filtered。
//   2. appsink 回调跑在 GStreamer 的 streaming 线程，不能碰 UI。帧用 queued
//      signal 投递，并用 m_pending 节流：UI 没处理完就丢新帧，避免事件队列
//      堆几百个 QImage 把内存吃爆。
//
//  三个可调项（都是为了压 CPU，见 camsource.cpp 顶部说明）：
//     setFps()          —— 限帧，UI 侧只转换/投递这么多次
//     setPixelFormat()  —— 让 mppvideodec 直出 RGB 系，省掉软件 NV12→RGB
//     setDecodeSize()   —— 让解码器直接输出小尺寸，减少每帧拷贝量
// =====================================================================

#include <QObject>
#include <QImage>
#include <QString>

class QTimer;

typedef struct _GstElement GstElement;
typedef struct _GstAppSink GstAppSink;
typedef struct _GstBus GstBus;

class CamSource : public QObject
{
    Q_OBJECT
public:
    explicit CamSource(QObject *parent = nullptr);
    ~CamSource() override;

    // 启动取流。outW/outH 留作兼容（host 假帧尺寸）。
    // 返回 false 表示环境不支持（host 版没有 GStreamer）或参数不合法。
    bool start(const QString &url, int outW = 640, int outH = 360);
    void stop();
    bool running() const { return m_running; }

    QString url() const { return m_url; }

    // ---- 调优项（必须在 start() 之前设置，改完要重启流）----
    // 目标帧率：<=0 表示不限（跟源一样）。默认 15。
    void setFps(int fps) { m_fps = fps > 0 ? fps : 0; }
    int  fps() const { return m_fps; }

    // 解码输出像素格式："NV12"（默认，软件转色）/ "BGRx" / "BGRA" / "RGB"
    // 板端 mppvideodec 的 SRC pad 标称支持 BGRx/RGBA/RGB 等，能用就省掉
    // 一次整帧的软件色彩转换。
    void setPixelFormat(const QString &fmt);
    QString pixelFormat() const { return m_fmt; }

    // 让 mppvideodec 直接输出这个尺寸（0 = 原始分辨率）。
    // 只影响「解码→内存」这一段的拷贝量，不改变画面比例。
    void setDecodeSize(int w, int h) { m_decW = w > 0 ? w : 0; m_decH = h > 0 ? h : 0; }

    // 传输方式："auto"（默认：rtspsrc 自己按 udp→mcast→tcp 回退）/ "udp" / "tcp"
    // tcp 会因丢包重传 + 队头阻塞把延迟堆到秒级，正常情况下别强制 tcp。
    void setTransport(const QString &t);
    QString transport() const { return m_transport; }

    // jitterbuffer 缓冲毫秒数，默认 0（不缓冲，延迟最低）。
    void setLatencyMs(int ms) { m_latencyMs = ms > 0 ? ms : 0; }

    // jitterbuffer 超过 latency 就丢旧包。⚠ 实测开着它解码开销会涨 4~5 倍
    // （乱序包被立刻吐出 → 废 AU 反复重解），默认关。
    void setDropOnLatency(bool on) { m_dropOnLat = on; }

    // appsink 队列：max-buffers=0 表示不限（不丢帧，但队列会积压 → 延迟涨）
    void setSinkBuffers(int n, bool drop) { m_sinkBufs = n; m_sinkDrop = drop; }

signals:
    void frameReady(const QImage &img);   // UI 线程收到
    void failed(const QString &why);      // 断流 / 打不开

public slots:
    // UI 侧处理完一帧后调用，放行下一帧（节流阀）
    void ackFrame();

private slots:
    void pollBus();                       // 轮询 ERROR / EOS
    void scheduleRetry();

private:
    void teardown();
    void reportFps(qint64 now);          // 每 5s 打一次实际进/出帧率（诊断用）

    GstElement *m_pipeline = nullptr;
    GstAppSink *m_appsink  = nullptr;
    GstBus     *m_bus      = nullptr;
    QTimer     *m_busTimer = nullptr;
    QTimer     *m_retry    = nullptr;

    QString m_url;
    int     m_w = 640, m_h = 360;
    bool    m_running = false;
    int     m_pending = 0;               // 0 = UI 空闲，可投递新帧
    int     m_failCount = 0;
    qint64  m_lastMs = 0;                // 上一帧投递时刻（限帧用）

    int     m_fps  = 15;                 // 0 = 不限帧
    QString m_fmt  = QStringLiteral("NV12");
    int     m_decW = 0, m_decH = 0;      // mppvideodec 输出尺寸（0 = 原始）
    QString m_transport = QStringLiteral("auto");
    int     m_latencyMs = 0;             // rtspsrc jitterbuffer（默认 2000，必须显式压到 0）
    bool    m_dropOnLat = false;
    int     m_sinkBufs  = 1;             // appsink max-buffers（1 = 只留最新一帧）
    bool    m_sinkDrop  = true;
    int     m_nIn = 0, m_nOut = 0;       // 5s 窗口内的进/出帧数
    qint64  m_statMs = 0;
    double  m_msMap = 0, m_msConv = 0, m_msUnmap = 0, m_msCopy = 0;
    QByteArray m_nv12;              // NV12 中转缓冲（dma-buf → 普通内存）

    friend struct CamSourceCallbacks;
};

#endif // CAMSOURCE_H

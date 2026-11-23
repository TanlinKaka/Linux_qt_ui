#include "tabpages.h"

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QPixmap>
#include <QHash>
#include <QDebug>
#include <QDateTime>

// =====================================================================
//  所有数字都来自设计稿量测（tools/land_measure.py）
// ---------------------------------------------------------------------
//  竖屏 1200x1920（摄像机关闭2.png / 一键位2.png / 拼接2备份.png）
//    页面骨架：左右各 8px #050505 黑边，页签行 y588..683，内容区 y684..1743，
//              内容区与底栏之间 y1744..1751 那 8px 黑缝由底图提供，不覆盖。
//    页内容区 = (0,684) 1200x1060
//
//  横屏 1920x1200（摄像机关闭.png / 一键位.png / 拼接备份.png）
//    骨架：x0..7 黑边 | 左常驻栏 x8..831 | 8px 黑缝 832..839 | 页内容区 840..1911
//          | 8px 黑边 1912..1919
//          y0..111 顶栏 | 112..119 黑缝 | 120..1031 页内容区 | 1032..1039 黑缝
//          | 1040..1199 底栏
//    页内容区 = (840,120) 1072x912  ← **横屏的页控件只盖这一块**
//    左侧患者栏（缩略图 / M 卡 / Req.SID 读数）切页时一直保留，不归页控件管。
//    页签行横屏也在这块里：y120..215，4 格 × 268，从 x840 起。
// =====================================================================

namespace {

const QColor kOuter   (0x05, 0x05, 0x05);   // 页面左右 8px 黑边
const QColor kPageBg  (0x17, 0x17, 0x17);
const QColor kPanelBg (0x21, 0x21, 0x21);
const QColor kSep     (0x45, 0x45, 0x45);
const QColor kInkActive(0xE0, 0xE0, 0xE0);
const QColor kInkIdle  (0xD1, 0xD1, 0xD1);
const QColor kInkOff   (0x78, 0x78, 0x78);   // Stitching：未启用
const QColor kBlue     (0x33, 0x7F, 0xBF);
const QColor kInkBody  (0xA6, 0xA6, 0xA6);
const QColor kBox      (0x09, 0x09, 0x09);
const QColor kBtnLine  (0x8C, 0x8C, 0x8C);   // 角度按钮 1px 描边（原稿不是 #575757）
const QColor kBtnLineOff(0x56, 0x56, 0x56);  // 运动中：其余按钮描边（设计稿 12）
const QColor kSelFill  (0x24, 0x33, 0x40);
const QColor kSelLine  (0x2E, 0x4A, 0x63);
const QColor kSelDoneFill(0x23, 0x2B, 0x31); // 到位后：选中格变暗蓝（设计稿 13）
const QColor kSelDoneLine(0x2A, 0x50, 0x70);
const QColor kStripSel (0x1C, 0x2B, 0x38);
const QColor kGoToBg   (0x40, 0x40, 0x40);   // 不可点：底 #404040 + 字 #7C7C7C
const QColor kGoToOnBg (0x69, 0x69, 0x69);   // 可点：底 #696969 + 字 #E1E1E1
const QColor kGoToOffInk(0x7C, 0x7C, 0x7C);
const QColor kGoToOnInk(0xE1, 0xE1, 0xE1);
const QColor kNumInk   (0xE8, 0xE8, 0xE8);
const QColor kNumInkOff(0x84, 0x84, 0x84);   // 运动中/到位后：数字压暗
const QColor kUnitInk  (0x77, 0x77, 0x77);
const QColor kStatusInk(0xE8, 0xE8, 0xE8);
// Stitching 页（拼接备份.png 采样）
const QColor kStTitle (0xD1, 0xD1, 0xD1);
const QColor kStLabel (0x9A, 0x9A, 0x9A);
const QColor kStValue (0xD1, 0xD1, 0xD1);
const QColor kStInfoInk(0x8A, 0x8A, 0x8A);
const QColor kStBtnBg (0x2A, 0x2A, 0x2A);
const QColor kStBtnInk(0x7F, 0x7F, 0x7F);
const QColor kStDot   (0x8C, 0x8C, 0x8C);
// View 页相机开启态（摄像机2.png / 摄像机.png 采样）：按钮底 #11202D + 蓝框 #337FBF
const QColor kCamOnBg (0x11, 0x20, 0x2D);

// 页面骨架：左右 8px 黑边 + 中间内容底色
void paintShell(QPainter &p, const QRect &r, const QColor &fill)
{
    p.fillRect(r, kOuter);
    p.fillRect(QRect(8, 0, r.width() - 16, r.height()), fill);
}

QFont uiFont(int px, QFont::Weight w = QFont::Normal)
{
    static const QString fam = QStringLiteral("Ubuntu");
    QFont f(fam);
    f.setPixelSize(px);
    f.setWeight(w);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

// 按「墨迹水平中心 + 基线」落字：设计稿是按基线对齐的，用
// AlignHCenter 会因为 Qt 的 side bearing 差 1~2px，这里统一手算。
void drawInk(QPainter &p, int centerX, int baseline, const QString &text,
             const QFont &f, const QColor &c)
{
    const QFontMetricsF fm(f);
    const qreal w = fm.horizontalAdvance(text);
    p.setFont(f);
    p.setPen(c);
    p.drawText(QPointF(centerX - w / 2.0, baseline), text);
}

// 按「墨迹左端 + 基线」落字（Stitching 页的标签都是左对齐）
void drawLeft(QPainter &p, int leftX, int baseline, const QString &text,
              const QFont &f, const QColor &c)
{
    p.setFont(f);
    p.setPen(c);
    p.drawText(QPointF(leftX, baseline), text);
}

// 圆角描边按钮
void drawBox(QPainter &p, const QRectF &r, qreal radius,
             const QColor &fill, const QColor &line, qreal lw = 2.0)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(r.adjusted(lw / 2, lw / 2, -lw / 2, -lw / 2), radius, radius);
    p.fillPath(path, fill);
    // ⚠ drawPath 会同时用「当前画刷」填充 —— 描边前必须清画刷，
    //   否则前面留下的浅色画刷会把整块按钮刷亮（20 个角度钮全变浅灰的老坑）。
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(line, lw));
    p.drawPath(path);
    p.setRenderHint(QPainter::Antialiasing, false);
}

// 图标素材（qrc:/mselect/...），加载失败时返回空图（drawPixmap 空图不画，不崩）
QPixmap res(const QString &name)
{
    static QHash<QString, QPixmap> cache;
    auto it = cache.constFind(name);
    if (it != cache.constEnd())
        return it.value();
    QPixmap pm(QStringLiteral(":/mselect/%1").arg(name));
    if (pm.isNull())
        qWarning("[tabpages] 素材缺失: %s", qPrintable(name));
    cache.insert(name, pm);
    return pm;
}

} // namespace

// =====================================================================
//  TabBarPage
// =====================================================================
TabBarPage::TabBarPage(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
}

void TabBarPage::applyProfile(bool landscape)
{
    m_land = landscape;
    if (landscape) {
        // 横屏页签行量自 曝光备份.png / 摄像机关闭.png：
        //   x840..1911（4 格 × 268），y120..215（96 高，蓝线 y212..215）
        // 左侧 832..839 是底图自带的 8px 黑缝，所以本控件从 840 起、不画左黑边。
        setGeometry(QRect(840, 120, 1072, 96));
        show();
        raise();
        update();
        return;
    }
    // 全宽：左右 8px 黑边也要盖住（底图是 Exposure 那一页，页签行同样带黑边，
    // 但换页后内容区会重画，页签行必须自己负责整条，避免露缝）
    setGeometry(QRect(0, 588, 1200, 96));
    show();
    raise();
    update();
}

void TabBarPage::setActive(int i)
{
    i = qBound(0, i, 3);
    if (i == m_active)
        return;
    m_active = i;
    update();
}

int TabBarPage::indexAt(const QPoint &pt) const
{
    if (!rect().contains(pt))
        return -1;
    // 竖屏 4 格 × 296 从 x8 起；横屏 4 格 × 268，控件本身已定位到 x840
    const int cellW = m_land ? 268 : 296;
    const int x0    = m_land ? 0 : 8;
    return qBound(0, (pt.x() - x0) / cellW, 3);
}

void TabBarPage::mousePressEvent(QMouseEvent *e)
{
    m_press = indexAt(e->pos());
    qDebug("[tabbar] press (%d,%d) -> cell %d", e->pos().x(), e->pos().y(), m_press);
    if (m_press >= 0) {
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void TabBarPage::mouseReleaseEvent(QMouseEvent *e)
{
    const int i = indexAt(e->pos());
    const bool hit = (i >= 0 && i == m_press);
    m_press = -1;
    qDebug("[tabbar] release (%d,%d) -> cell %d  %s",
           e->pos().x(), e->pos().y(), i, hit ? "CLICK" : "ignored");
    if (hit) {
        emit tabClicked(i);
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void TabBarPage::paintEvent(QPaintEvent *)
{
    static const char *kNames[4] = {"Exposure", "View", "Position", "Stitching"};
    QPainter p(this);

    // 两种版式的差别只有「起点 / 格宽 / 右边留不留 8px 黑边」。
    //  竖屏：scrW 1200，格宽 296，从 x8 起，右侧 8px 黑边由 paintShell 画
    //  横屏：scrW 1920，格宽 268，控件已定位在 x840，整块都是内容（左黑缝在控件外）
    const int cellW = m_land ? 268 : 296;
    const int x0    = m_land ? 0 : 8;
    const int barX  = x0;
    const int barW  = m_land ? 1072 : 1184;

    if (m_land)
        p.fillRect(rect(), kPageBg);
    else
        paintShell(p, rect(), kPageBg);

    // 整条底线：非活动格是 #454545，活动格由蓝线覆盖（设计稿 y680..683 / 横屏 y212..215）
    p.fillRect(QRect(barX, 92, barW, 4), kSep);
    p.fillRect(QRect(barX + m_active * cellW, 92, cellW, 4), kBlue);

    // 页签文字：40px + 1.1 字距。设计稿 "Exposure" 墨迹 174px（横竖屏同宽）、
    // 40px 裸排只有 165px，而字高又确实是 40px 的（29~35）→ 差的是字距，不是字号。
    QFont tf = uiFont(40);
    tf.setLetterSpacing(QFont::AbsoluteSpacing, 1.1);
    for (int i = 0; i < 4; ++i) {
        // Stitching 在设计稿里是「未启用」：字更暗，且不参与下划线
        const bool enabled = (i != 3);
        const QColor c = (i == m_active) ? kInkActive : (enabled ? kInkIdle : kInkOff);
        drawInk(p, barX + i * cellW + cellW / 2, 63, QLatin1String(kNames[i]), tf, c);
    }
}

// =====================================================================
//  CameraOffPage（View）
// =====================================================================
CameraOffPage::CameraOffPage(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::ArrowCursor);
}

void CameraOffPage::applyProfile(bool landscape)
{
    m_land = landscape;
    if (landscape) {
        // 横屏页内容区只有右侧 1072x912（左侧患者栏不归本控件管）
        setGeometry(QRect(840, 120, 1072, 912));
        // 设计稿：按钮 (848,568) 112x112 → 相对本控件 (8,448)
        m_btn   = QRect(8, 448, 112, 112);
        // 画面矩形绝对 (969,240) 815x768 → 相对 (129,120)
        m_photo = QRect(129, 120, 815, 768);
    } else {
        setGeometry(QRect(0, 684, 1200, 1060));
        m_btn   = QRect(16, 1158 - 684, 112, 112);
        // 画面矩形绝对 (84,710) 1032x1008 → 相对 (84,26)
        m_photo = QRect(84, 26, 1032, 1008);
    }
    // ⚠ 命中区外扩 32 设计 px：按钮可视 112x112，落到这块屏上只有 60x56 物理像素
    //   （约 11x10mm），手指一偏就落空 —— 用户反馈的「点了没反应/有点飘」就是它。
    //   外扩后 176x176 设计 ≈ 94x88 物理像素，符合触摸最小目标。
    m_btnHit = m_btn.adjusted(-32, -32, 32, 32).intersected(rect());
    rebuildScaled();
    show();
    update();
}

void CameraOffPage::setCameraState(CamState s)
{
    if (m_state == s)
        return;
    m_state = s;
    update();
}

void CameraOffPage::setCameraFrame(const QImage &img)
{
    // 只有 ON 态才收帧：关掉之后管线的「在途帧」还会到一两次，收下它们就会
    // 把状态从 OFF 顶回 ON（用户看到的「点关闭又自己开回来」）。
    if (m_state != CamOn)
        return;

    m_frame = img;
    rebuildScaled();
    // ⚠ 竖屏时按钮压在画面左边缘 44px 上，只 update(m_photo) 会把按钮一起擦掉，
    //   下一帧才补回来 → 视觉上按钮一直在闪。必须把按钮区域并进来。
    update(m_photo.united(m_btn));
}

void CameraOffPage::clearFrame()
{
    m_frame = QImage();
    m_src = QRect();
    m_dst = QRect();
    update();
}

// 只算「从源图哪一块取、画到哪个矩形」，不再生成中间 QImage。
// ⚠ 原来这里是 m_frame.scaled(...) 再 .copy(居中裁)，每帧要：分配 815x768x4
//   ≈2.5MB + 一次全图插值 + 一次全图拷贝，A55 上约 10ms/帧。现在合并成
//   paintEvent 里的 drawImage(目标, 源图, 源矩形)，Qt 一步写后备缓冲，
//   裁剪和缩放都在同一次遍历里完成，中间图彻底不要了。
void CameraOffPage::rebuildScaled()
{
    m_src = QRect();
    m_dst = QRect();
    if (m_frame.isNull() || m_photo.isEmpty())
        return;

    const QSize src = m_frame.size();
    const QSize box = m_photo.size();

    if (m_cover) {
        // 铺满：源图按「盖住」算出缩放比 k，反推要裁出的源矩形（等比、居中）
        const qreal k = qMax(qreal(box.width())  / src.width(),
                             qreal(box.height()) / src.height());
        const int cw = qMin(src.width(),  qRound(box.width()  / k));
        const int ch = qMin(src.height(), qRound(box.height() / k));
        m_src = QRect((src.width() - cw) / 2, (src.height() - ch) / 2, cw, ch);
        m_dst = m_photo;
    } else {
        // 内切：整幅都画，等比缩到完全放得下，四周居中留边
        const QSize s = src.scaled(box, Qt::KeepAspectRatio);
        m_src = QRect(QPoint(0, 0), src);
        m_dst = QRect(m_photo.x() + (m_photo.width()  - s.width())  / 2,
                      m_photo.y() + (m_photo.height() - s.height()) / 2,
                      s.width(), s.height());
    }
}

// 一次手指点击 → 多组 press/release 的原因（2026-11-20 板上实证）：
//   ILITEK 触摸屏在内核里注册了**两个**输入设备：
//     event5  "ILITEK ILITEK-TP"        真多点触摸
//     event6  "ILITEK ILITEK-TP Mouse"  同一块屏的单点兼容鼠标
//   weston 把两个都转给 Qt，于是同一次按下先来一组触摸（Qt 合成鼠标事件），
//   再来一组鼠标事件 —— 坐标差几像素、间隔只有几毫秒。
//
// 原来用「处理时刻」做 400ms 冷却，被下面这条链打败：
//   第 1 组 → OFF → stop() 阻塞主线程几百 ms → 队列里第 2 组这时才被处理，
//   早已超过冷却窗口 → 又被当成新点击 → 又开回去 → 用户看到「点关闭没反应」。
//
// 改用**事件自带的时间戳**（QInputEvent::timestamp）：重复组的时间戳相差只有
// 几毫秒，与主线程卡多久完全无关；而真正的第二次点击在事件时间上也至少差 100ms。
// 事件没带时间戳（比如 --sim-click 手工构造的）时才退回系统时钟。
bool CameraOffPage::gestureAccepted(qint64 evtTs, qint64 now)
{
    const qint64 kDupMs = 260;
    const bool evtKnown = (evtTs > 0);
    bool dup = false;

    if (evtKnown && m_lastEvtTs > 0)
        dup = (evtTs - m_lastEvtTs) < kDupMs;    // 权威判据：与主线程卡多久无关
    else if (m_lastWallMs > 0)
        dup = (now - m_lastWallMs) < kDupMs;     // 时间戳不可用时的兜底

    if (dup)
        return false;

    m_lastEvtTs  = evtKnown ? evtTs : 0;
    m_lastWallMs = now;
    return true;
}

void CameraOffPage::mousePressEvent(QMouseEvent *e)
{
    // 命中用外扩后的 m_btnHit，不是可视的 m_btn（见 applyProfile 里的说明）
    m_pressHit = m_btnHit.contains(e->pos());
    if (m_pressHit) {
        m_pressed = true;          // 按下立刻给视觉反馈，否则用户以为没点上会反复点
        update(m_btnHit);
        qDebug("[cam] btn press (%d,%d) hit=1 ts=%lu", e->pos().x(), e->pos().y(),
               (unsigned long)e->timestamp());
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void CameraOffPage::mouseReleaseEvent(QMouseEvent *e)
{
    // 和翻转热区同一套：press 上膛，release 无条件触发（手指飘出框外也算）
    const bool hit = m_pressHit;
    m_pressHit = false;
    if (m_pressed) {
        m_pressed = false;
        update(m_btnHit);
    }
    if (hit) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!gestureAccepted(qint64(e->timestamp()), now)) {
            qDebug("[cam] btn release dup ts=%lu (same tap, ignored)",
                   (unsigned long)e->timestamp());
            e->accept();
            return;
        }
        qDebug("[cam] btn release -> toggle ts=%lu", (unsigned long)e->timestamp());
        emit cameraToggle();
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void CameraOffPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);

    // 页底色：横屏纯色，竖屏带左右 8px 黑边
    if (m_land)
        p.fillRect(rect(), kPageBg);
    else
        paintShell(p, rect(), kPageBg);

    // 左下角相机按钮：OFF 灰框 / ON&ERROR 蓝框（素材都是 96x96，居中在 112x112 里）
    auto drawBtn = [&](bool onLook) {
        p.fillRect(m_btn, onLook ? kCamOnBg : kBox);
        p.drawPixmap(m_btn.topLeft() + QPoint(8, 8),
                     res(onLook ? "tab_cam_btn_on.png" : "tab_cam_btn.png"));
        // 按下反馈：触摸屏没有 hover，不给反馈用户就会「反复点」并把慢关机当成没点上
        if (m_pressed)
            p.fillRect(m_btn, QColor(255, 255, 255, 40));
    };

    // ---------------- 开启：整块画面，没有图标和文字 ----------------
    if (m_state == CamOn) {
        if (m_frame.isNull() || m_dst.isEmpty()) {
            p.fillRect(m_photo, kBox);          // 首帧之前填 #090909
        } else {
            // ⚠ 关掉 Smooth（默认双三次）：A55 上十几 ms/帧，25fps 直接把核吃满。
            //   最近邻 + 一步 crop/scale，画完就是最终像素，不再留中间图。
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawImage(m_dst, m_frame, m_src);
        }
        drawBtn(true);
        return;
    }

    // ---------------- 关闭 / 不可用 ----------------
    // 两态的元素坐标（都取自设计稿墨迹包围盒，绝对坐标 → 减控件原点）
    int camX, camY, cx, line1Bas, line2Bas;
    if (m_land) {
        cx = 536;  line1Bas = 589;  line2Bas = 651;
        camX = (m_state == CamError) ? 467 : 466;
        camY = (m_state == CamError) ? 379 : 389;
    } else {
        cx = 598;  line1Bas = 1301 - 684;  line2Bas = 1363 - 684;
        camX = (m_state == CamError) ? 531 : 530;
        camY = (m_state == CamError) ? 1089 - 684 : 1099 - 684;
    }

    // 设计稿这两行比 Ubuntu 40px 的默认字距宽 ~10%，实测「字宽差/字符数」
    // 两行都落在 2.0~2.2px，所以是加字距而不是换字号（换字号会把字高顶上去）。
    QFont vf = uiFont(40);
    vf.setLetterSpacing(QFont::AbsoluteSpacing, 2.1);

    if (m_state == CamError) {
        p.drawPixmap(QPoint(camX, camY), res("tab_cam_err.png"));
        drawInk(p, cx, line1Bas, QStringLiteral("Camera is not working, restart the system."),
                vf, kInkBody);
        drawInk(p, cx, line2Bas, QStringLiteral("If the issue persists, contact service."),
                vf, kInkBody);
        drawBtn(true);
        return;
    }

    // 中央灰色相机图标（素材 140x94，含底 #171717，与原稿逐像素一致）
    p.drawPixmap(QPoint(camX, camY), res("tab_cam_off.png"));
    drawInk(p, cx, line1Bas, QStringLiteral("The live camera is turned OFF"),
            vf, kInkBody);
    drawInk(p, cx, line2Bas, QStringLiteral("Tap the camera icon to turn ON the live camera"),
            vf, kInkBody);
    drawBtn(false);
}

// =====================================================================
//  PositionPage
// =====================================================================
PositionPage::PositionPage(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::ArrowCursor);
    rebuildCells();
}

void PositionPage::applyProfile(bool landscape)
{
    m_land = landscape;
    if (landscape) {
        // 横屏页内容区 (840,120) 1072x912；控件内顶部 96px 是页签行（页签行后 raise）
        //   左竖条 x8..135，4 格，格高 128、步进 129，首格 y98
        //   Go to position (1554,920) 334x94 → 相对 (714,800)
        setGeometry(QRect(840, 120, 1072, 912));
        m_goTo = QRect(714, 800, 334, 94);
        for (int i = 0; i < 4; ++i)
            m_strip[i] = QRect(8, 98 + i * 129, 128, 128);
        rebuildCells();
        show();
        update();
        return;
    }
    setGeometry(QRect(0, 684, 1200, 1060));
    m_goTo = QRect(834, 1632 - 684, 334, 96);
    for (int i = 0; i < 4; ++i)
        m_strip[i] = QRect(8, 686 - 684 + i * 129, 128, 128);
    rebuildCells();
    show();
    update();
}

// 四个模式的角度矩阵（以实机 2026-11-20 照片为准，设计稿只覆盖了模式 0）：
//   模式 0  探测器倾斜      两栏 110/180  正角 10 格（0 5 10 15 20 25 30 35 40 45）
//   模式 1  球管 + 探测器   两栏 110/180  负角 10 格（0 −5 −10 … −45）
//   模式 2  球管倾斜        单栏 110      正角  9 格（右下角 45° 那格没有）
//   模式 3  球管倾斜（反向）单栏 110      负角  9 格（右下角 −45° 那格没有）
// 两栏 = 110/180 两个 SID 各一组；单栏只有 110，右栏留空面板。
// 列优先：列 1 = 0/10/20/30/40，列 2 = 5/15/25/35/45（设计稿如此，不是行优先）。
void PositionPage::rebuildCells()
{
    m_cells.clear();
    // 行坐标：竖屏面板就在控件顶，首行 96（表头占 96）；横屏面板顶在 96，
    // 表头再占 98 → 首行 194。行步进 118、按钮高 94（两套设计稿一致）。
    const int row0 = m_land ? 194 : 96;
    const int rowY[5] = {row0, row0 + 118, row0 + 236, row0 + 354, row0 + 472};
    const bool neg  = (m_mode == 1 || m_mode == 3);   // 负角：第 2、4 个按钮
    const bool dual = (m_mode <= 1);                  // 两栏：第 1、2 个按钮

    // 列坐标（相对控件左上）：
    //   竖屏 栏 (138,0)/(669,0) 宽 523，按钮 200，列 188/412 与 719/943
    //   横屏 栏 (130,96)/(605,96) 宽 467，按钮仍是 200，列 151/375 与 626/850
    const int cellW    = 200;
    const int colX[2]  = { m_land ? 151 : 188, m_land ? 375 : 412 };
    const int colX2[2] = { m_land ? 626 : 719, m_land ? 850 : 943 };

    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 2; ++c) {
            // 单栏模式右下角那格实机上是空的（照片 2/3 都没有边框）
            if (!dual && r == 4 && c == 1)
                continue;
            const int v = r * 10 + c * 5;
            m_cells << Cell{0, neg ? -v : v, QRect(colX[c], rowY[r], cellW, 94)};
            if (dual)
                m_cells << Cell{1, neg ? -v : v, QRect(colX2[c], rowY[r], cellW, 94)};
        }
    }
}

int PositionPage::cellAt(const QPoint &pt) const
{
    for (int i = 0; i < m_cells.size(); ++i)
        if (m_cells[i].r.contains(pt))
            return i;
    return -1;
}

void PositionPage::setTiltMode(int i)
{
    i = qBound(0, i, 3);
    if (i == m_mode)
        return;
    m_mode = i;
    m_angA = m_angB = -1;                    // 换模式 = 选择作废
    m_status.clear();
    m_statusKind = 0;
    rebuildCells();
    update();
    emit tiltModeChanged(i);
}

void PositionPage::setAngle(int panel, int value)
{
    int *slot = (panel == 0) ? &m_angA : &m_angB;
    // 负数 = 清空该栏选择（调试/复位用：--set angle=none）
    if (value < 0) {
        *slot = -1;
        update();
        return;
    }
    for (const Cell &c : m_cells) {
        if (c.panel == panel && c.value == value) {
            *slot = value;
            // 一次只能亮一个按钮 → 另一栏必须清掉
            if (panel == 0) m_angB = -1; else m_angA = -1;
            update();
            return;
        }
    }
}

bool PositionPage::goToEnabled() const
{
    // 有选中才可点；运动中（1）与已到位（2）都是不可点态 —— 设计稿三张稿如此
    return (m_angA >= 0 || m_angB >= 0) && m_statusKind == 0;
}

void PositionPage::setStatus(const QString &text, int kind)
{
    m_status = text;
    m_statusKind = kind;
    update();
}

// 命中目标编码：0..3 左侧竖条 / 100+i 角度按钮 / -2 Go to position / -1 空白
int PositionPage::targetAt(const QPoint &pt) const
{
    for (int i = 0; i < 4; ++i)
        if (m_strip[i].contains(pt))
            return i;
    const int ci = cellAt(pt);
    if (ci >= 0)
        return 100 + ci;
    if (m_goTo.contains(pt))
        return -2;
    return -1;
}

void PositionPage::mousePressEvent(QMouseEvent *e)
{
    m_press = targetAt(e->pos());
    qDebug("[pospage] press (%d,%d) -> target %d%s", e->pos().x(), e->pos().y(), m_press,
           moving() ? " (moving: locked)" : "");
    // 运动中整页锁住：不 accept，松手也不会回投 → 天然点不动
    if (m_press != -1 && !moving()) {
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void PositionPage::mouseReleaseEvent(QMouseEvent *e)
{
    const int t = targetAt(e->pos());
    const int pressed = m_press;
    m_press = -1;
    if (t == -1 || t != pressed) {
        qDebug("[pospage] release -> target %d (press %d) ignored", t, pressed);
        QWidget::mouseReleaseEvent(e);
        return;
    }
    e->accept();
    if (moving()) {                      // 运动中：所有按钮都不可选
        qDebug("[pospage] release ignored (moving)");
        return;
    }

    if (t >= 100) {
        // 角度按钮：只点亮/取消点亮，不出任何提示信息
        const Cell &c = m_cells[t - 100];
        int *slot  = (c.panel == 0) ? &m_angA : &m_angB;
        int *other = (c.panel == 0) ? &m_angB : &m_angA;
        if (*slot == c.value) {          // 再点同一个 → 取消点亮
            *slot = -1;
            qDebug("[pospage] angle deselect panel %d %d", c.panel, c.value);
        } else {                         // 一次只能亮一个
            *slot = c.value;
            *other = -1;
            qDebug("[pospage] angle panel %d -> %d", c.panel, c.value);
        }
        m_status.clear();                // 选/取消都不提示
        m_statusKind = 0;
        update();
        emit anglePicked(c.panel, *slot);
    } else if (t == -2) {
        if (!goToEnabled()) {
            qDebug("[pospage] go to position ignored (no selection / busy)");
            return;
        }
        qDebug("[pospage] go to position A=%d B=%d", m_angA, m_angB);
        emit goToPosition();
    } else {
        qDebug("[pospage] tilt mode -> %d", t);
        setTiltMode(t);
    }
}

void PositionPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);

    if (m_land)
        p.fillRect(rect(), kPageBg);
    else
        paintShell(p, rect(), kPageBg);

    // 骨架（两套设计稿量测值，全部相对控件左上）：
    //            竖屏            横屏
    //  栏 1 x    138             130
    //  栏 2 x    669             605
    //  栏宽      523             467
    //  面板顶    0               96
    //  面板高    932             688
    //  表头基线  56              154
    //  底部条    932 高128       784 高128
    const int footY   = m_land ? 784 : 932;
    const int footH   = 128;
    const int footW   = m_land ? 1072 : 1184;
    const int footDivX= m_land ? 128 : 136;
    const int panelY  = m_land ? 96 : 0;
    const int panelH  = m_land ? 688 : 932;
    const int panel1X = m_land ? 130 : 138;
    const int panel2X = m_land ? 605 : 669;
    const int panelW  = m_land ? 467 : 523;
    const int headY   = panelY + (m_land ? 24 : 22);     // 表头图标顶
    const int headBas = panelY + (m_land ? 58 : 56);     // 表头基线
    // 表头「图标 + 数值 + 单位」相对**栏左**的偏移（两套稿子同结构）
    const int headIconDx = m_land ? 136 : 164;
    const int headNumDx  = m_land ? 236 : 254;
    const int headUnitDx = m_land ? 297 : 321;

    // ---- 左竖条：4 格 ----
    for (int i = 0; i < 4; ++i) {
        if (i == m_mode)
            p.fillRect(m_strip[i], kStripSel);
        // 图标是「白 + alpha」素材（128x128），灰底/选中底上都能直接用
        p.drawPixmap(m_strip[i].topLeft(),
                     res(QStringLiteral("tab_strip_%1.png").arg(i)));
        if (i < 3)
            p.fillRect(QRect(8, m_strip[i].bottom() + 1, 128, 1), kSep);
    }

    // ---- 两栏面板：单栏模式（模式 2/3）右栏只有底、没有按钮 ----
    const bool dual = (m_mode <= 1);
    p.fillRect(QRect(panel1X, panelY, panelW, panelH), kPanelBg);
    p.fillRect(QRect(panel2X, panelY, panelW, panelH), kPanelBg);

    // 表头：人形图标 + 数值 + 单位（设计稿里是「图标 + 数值」整组，不是各自居中）
    p.drawPixmap(QPoint(panel1X + headIconDx, headY), res("tab_head_person.png"));
    const QFont nf = uiFont(40);
    drawInk(p, panel1X + headNumDx,  headBas, QStringLiteral("110"), nf, kNumInk);
    drawInk(p, panel1X + headUnitDx, headBas, QStringLiteral("cm"),  nf, kUnitInk);
    if (dual) {
        p.drawPixmap(QPoint(panel2X + headIconDx, headY), res("tab_head_person.png"));
        drawInk(p, panel2X + headNumDx,  headBas, QStringLiteral("180"), nf, kNumInk);
        drawInk(p, panel2X + headUnitDx, headBas, QStringLiteral("cm"),  nf, kUnitInk);
    }

    // ---- 角度按钮：1px 描边；三态配色取自设计稿 11/12/13 ----
    //   常态  : 底 #212121 / 边 #8C8C8C / 字 #E8E8E8
    //   选中  : 底 #243340 / 边 #337FBF / 字 #E8E8E8
    //   运动中: 其余按钮 边 #565656 / 字 #848484（不可选）
    //   到位后: 选中格 底 #232B31 / 边 #2A5070 / 字 #848484
    // 数字实测：墨迹高 30、每行基线在 top+59（设计稿 40px 会高 3px 且压低 4px）
    const QFont bf = uiFont(38, QFont::Bold);
    for (const Cell &c : m_cells) {
        const bool sel  = (c.panel == 0) ? (m_angA == c.value) : (m_angB == c.value);
        const bool busy = moving();
        const bool done = (m_statusKind == 2) && sel;

        QColor fill = sel ? kSelFill : kPanelBg;
        QColor line = sel ? kBlue    : kBtnLine;
        QColor ink  = kNumInk;
        if (busy && !sel) { line = kBtnLineOff; ink = kNumInkOff; }
        if (done)         { fill = kSelDoneFill; line = kSelDoneLine; ink = kNumInkOff; }

        drawBox(p, QRectF(c.r), 4, fill, line, 1.0);
        // 负角模式整栏都带负号，但 0° 那格实机印的是 "0°"（不是 "−0°"）。
        // 所以负号看「值」而不是看模式：-0 == 0，用值的符号刚好。负号用长 minus U+2212。
        QString num = QString::number(c.value);
        if (c.value < 0)
            num = QString(QChar(0x2212)) + QString::number(qAbs(c.value));
        drawInk(p, c.r.center().x(), c.r.top() + 59,
                num + QStringLiteral("\u00B0"), bf, ink);
    }

    // ---- 底部条（竖屏 y932..1059，横屏 y784..911；分隔线 x136 / x128）----
    p.fillRect(QRect(m_land ? 0 : 8, footY, footW, footH), kPageBg);
    p.fillRect(QRect(footDivX, footY, 1, footH), kSep);
    p.drawPixmap(QPoint(m_land ? 32 : 40, footY + 32), res("tab_wrench.png"));

    // 状态行：⚠ 三角 / ⓘ 圆 + 斜体文字（设计稿墨迹 x227..523、y1665..1693）
    // 40px 会宽 47px、压低 7px → 34px 斜体，起点 x=226。
    if (!m_status.isEmpty()) {
        p.drawPixmap(QPoint(160, footY + 36),
                     res(m_statusKind == 1 ? "tab_warn.png" : "tab_info.png"));
        QFont sf = uiFont(34);
        sf.setItalic(true);
        p.setFont(sf);
        p.setPen(kStatusInk);
        p.drawText(QPointF(226, footY + 73), m_status);
    }

    // Go to position：设计稿三态 —— 无选中 #404040/#7C7C7C（不可点），
    // 有选中 #696969/#E1E1E1（可点），运动中与到位后都回到不可点态。
    // 字实测墨迹 264x36（869..1131，y1659..1694）→ 42px Regular、基线 top+56。
    const bool ready = goToEnabled();
    drawBox(p, QRectF(m_goTo), 4,
            ready ? kGoToOnBg : kGoToBg, ready ? kGoToOnBg : kGoToBg, 2.0);
    drawInk(p, m_goTo.center().x(), m_goTo.top() + 56,
            QStringLiteral("Go to position"), uiFont(42),
            ready ? kGoToOnInk : kGoToOffInk);
}

// =====================================================================
//  StitchingPage
// ---------------------------------------------------------------------
//  横屏设计稿 拼接备份.png（1920x1200），全部数值相对页内容区左上 (840,120)：
//    Stitching       墨迹左 26、基线 150（40px）
//    Set positions   墨迹左 26、基线 207（40px，#9A9A9A）
//    Top   按钮      (403,175) 311x127，1px #8C8C8C 描边
//    Bottom 按钮     (737,175) 311x127
//    # Images 标签   左 26、基线 393；破折号 x408..447（y380）
//    Coverage 标签   左 26、基线 481；破折号 + cm x408..508（y459）
//    提示行 ⓘ        (307,707) 42x42；文字左 378、基线 741（32px Bold）
//    Reset           (676,792) 174x96 底 #2A2A2A
//    Apply           (874,792) 174x96
// =====================================================================
StitchingPage::StitchingPage(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::ArrowCursor);
}

void StitchingPage::applyProfile(bool landscape)
{
    m_land = landscape;
    if (landscape) {
        setGeometry(QRect(840, 120, 1072, 912));
        m_top    = QRect(403, 175, 311, 127);
        m_bot    = QRect(737, 175, 311, 127);
        m_reset  = QRect(676, 792, 174, 96);
        m_apply  = QRect(874, 792, 174, 96);
    } else {
        // 竖屏还是空页（骨架与别的页一致）
        setGeometry(QRect(0, 684, 1200, 1060));
    }
    show();
    update();
}

void StitchingPage::setStartPosition(int i)
{
    i = qBound(0, i, 1);
    if (i == m_start)
        return;
    m_start = i;
    update();
}

int StitchingPage::targetAt(const QPoint &pt) const
{
    if (m_top.contains(pt))   return 0;
    if (m_bot.contains(pt))   return 1;
    if (m_reset.contains(pt)) return 2;
    if (m_apply.contains(pt)) return 3;
    return -1;
}

void StitchingPage::mousePressEvent(QMouseEvent *e)
{
    m_press = targetAt(e->pos());
    qDebug("[stitchpage] press (%d,%d) -> target %d", e->pos().x(), e->pos().y(), m_press);
    if (m_press >= 0) {
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void StitchingPage::mouseReleaseEvent(QMouseEvent *e)
{
    const int t = targetAt(e->pos());
    const bool hit = (t >= 0 && t == m_press);
    m_press = -1;
    if (!hit) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    e->accept();
    if (t <= 1) {
        qDebug("[stitchpage] start position -> %s", t == 0 ? "Top" : "Bottom");
        setStartPosition(t);
        emit startPositionChanged(t);
    } else if (t == 2) {
        qDebug("[stitchpage] reset");
        emit resetClicked();
    } else {
        qDebug("[stitchpage] apply");
        emit applyClicked();
    }
}

void StitchingPage::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (m_land)
        p.fillRect(rect(), kPageBg);
    else
        paintShell(p, rect(), kPageBg);

    // 竖屏设计稿也有（拼接2备份.png），但目前只做横屏版式
    if (!m_land)
        return;

    // ---- 左侧标签 ----
    drawLeft(p, 26, 150, QStringLiteral("Stitching"),     uiFont(40), kStTitle);
    drawLeft(p, 26, 207, QStringLiteral("Set positions"), uiFont(40), kStLabel);

    // ---- Top / Bottom 按钮 ----
    for (int i = 0; i < 2; ++i) {
        const QRect r = (i == 0) ? m_top : m_bot;
        drawBox(p, QRectF(r), 4, kPageBg, kBtnLine, 2.0);
        // 右上角状态点
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(kStDot);
        p.drawEllipse(QPointF(r.right() - 17, r.top() + 20), 5, 5);
        p.setRenderHint(QPainter::Antialiasing, false);

        // 图标用设计稿裁下来的素材（床架 + 人形 + 方向小方块），56x56。
        // 设计稿里「图标+文字」整组在按钮内居中（组中心 = 按钮中心 1398.5 / 1732.5）。
        if (i == 0)
            p.drawPixmap(QPoint(486, 211), res("st_top.png"));
        else
            p.drawPixmap(QPoint(786, 211), res("st_bot.png"));
        drawInk(p, (i == 0) ? 599 : 932, 248,
                i == 0 ? QStringLiteral("Top") : QStringLiteral("Bottom"),
                uiFont(42), kStTitle);
    }

    // ---- # Images / Coverage ----
    drawLeft(p, 26, 391, QStringLiteral("# Images"), uiFont(40), kStLabel);
    drawLeft(p, 26, 479, QStringLiteral("Coverage"), uiFont(40), kStLabel);
    p.fillRect(QRect(408, 380, 40, 3), kStValue);           // 破折号
    p.fillRect(QRect(408, 459, 40, 3), kStValue);
    drawLeft(p, 456, 479, QStringLiteral("cm"), uiFont(40), kStValue);

    // ---- 提示行 ----
    p.drawPixmap(QPoint(307, 707), res("st_info.png"));
    QFont inf = uiFont(28, QFont::Bold);
    drawLeft(p, 378, 736,
             QStringLiteral("Image acquisition direction is from top to bottom"),
             inf, kStInfoInk);

    // ---- Reset / Apply ----
    for (int i = 0; i < 2; ++i) {
        const QRect r = (i == 0) ? m_reset : m_apply;
        drawBox(p, QRectF(r), 4, kStBtnBg, kStBtnBg, 1.0);
        drawInk(p, r.center().x(), r.top() + 57,
                i == 0 ? QStringLiteral("Reset") : QStringLiteral("Apply"),
                uiFont(40), kStBtnInk);
    }
}

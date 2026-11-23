// ============================================================================
//  DropPopup / DropCardBase / PatientCard / WorkspotCard 实现
//  量测值全部来自设计稿（1200x1920 画布）逐像素反推，见 _probe/layout/README.md。
// ============================================================================
#include "patientcard.h"
#include "layout_profile.h"

#include <QPainter>
#include <QPainterPath>
#include <QFontMetricsF>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QHideEvent>
#include <QEvent>
#include <QTimer>
#include <QDateTime>
#include <QDebug>
#include <cmath>

// ---------------------------- 弹层量测常量（设计坐标） ------------------------
// 6 个弹层共用一套视觉，只有尺寸/图标框/滚动条不同（见 patientcard.h 的表）：
//   Patient type     abs (287,267) 514x450
//   Workspot         abs (663,267) 514x450
//   Focal Spot       abs (661,999) 508x338
//   Target exp.index abs (661,847) 508x658
//   Exposure params  abs ( 31,999) 508x450
static const QSize  kPopSize(514, 450);   // 默认尺寸（Patient / Workspot）
static const int    kPopRadius    = 4;
static const QColor kPopBg        (0x21, 0x21, 0x21);
static const QColor kPopBorder    (0x59, 0x59, 0x59);
static const QColor kRowHi        (0x4D, 0x4D, 0x4D);
// 「待确认行」：手指点过、还没落定的那一行。用和 Position 选中态一致的蓝调，
// 和普通 hover 灰底明确区分 —— 否则用户看不出「已经点上了，还差一下」。
static const QColor kRowPend      (0x24, 0x33, 0x40);
static const QColor kRowPendBar   (0x33, 0x7F, 0xBF);
static const QColor kInk          (0xE8, 0xE8, 0xE8);
static const QColor kHeaderInk    (0x9E, 0x9E, 0x9E);
static const QColor kSbTrack      (0x17, 0x17, 0x17);
static const QColor kSbThumb      (0x76, 0x76, 0x76);

static const int    kHdrClipBottom = 74;   // 标题下伸部分裁平（设计稿 y=341）
static const int    kRowTopRel     = 97;   // 首行顶（364-267）
static const int    kRowH          = 112;
static const QRect  kIconBoxDef    (25, 24, 64, 64);   // 行内图标框（相对行顶）

// 弹层相对卡片：纵向 = 卡片底 + 3（竖屏 264 → 267）；
// 横向 = 贴卡片某一条边再向外 1px，见 layoutPopup()
static const int    kPopGapY       = 3;

// ---------------------------- 卡片量测常量 ----------------------------------
static const QRect  kCardRect(288, 136, 432, 128);      // 竖屏兜底，构造时被 profile 覆盖
static const int    kCardRadius   = 8;
static const QColor kCardBg       (0x3B, 0x3B, 0x3B);
static const QColor kCardBgActive (0x24, 0x24, 0x24);   // 展开中：卡片压暗

// 字体参数（calib.cpp 对设计稿穷举得出）
static const int    kLetterPx    = 41;     // Patient type 行内字母
static const int    kHeaderPx    = 42;     // 两个弹层的标题
static const int    kHeaderTrack = -0.4;
static const int    kPopRowPx    = 42;     // Workspot 行内文字

// ---------------------------------------------------------------------------
//  字体：设计稿是 Ubuntu。板子的 buildroot 里没有这个字体（只有 DejaVu /
//  Liberation / Cantarell），字宽会变，所以把 Ubuntu-R.ttf 打进 Qt 资源，
//  运行时注册。VM 上系统装的也是同一个文件，两边渲染一致。
// ---------------------------------------------------------------------------
static QString g_family;

static const QString &uiFontFamily()
{
    if (g_family.isEmpty()) {
        // 两种字重都嵌进来：板上没有系统 Ubuntu，只嵌 Regular 的话
        // setWeight(Bold) 会走 Qt 的「伪粗」（描边加粗），和 VM 上的真 Bold 不一样。
        const int id = QFontDatabase::addApplicationFont(
                    QStringLiteral(":/mselect/Ubuntu-R.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/mselect/Ubuntu-B.ttf"));
        const QStringList fams = QFontDatabase::applicationFontFamilies(id);
        if (fams.isEmpty()) {
            g_family = QStringLiteral("Ubuntu");
            qWarning("!! 内嵌字体注册失败，退回系统 Ubuntu（板上会 fallback）");
        } else {
            g_family = fams.first();
            qDebug("ui font: '%s' (id=%d)", qPrintable(g_family), id);
        }
    }
    return g_family;
}

static QFont makeFontW(int px, QFont::Weight w = QFont::Normal)
{
    QFont f(uiFontFamily());
    f.setPixelSize(px);
    f.setWeight(w);
    f.setHintingPreference(QFont::PreferFullHinting);
    return f;
}

static QFont makeFont(int px) { return makeFontW(px); }

// 按「墨迹左上角」对齐画字：反推基线原点。
// 必须用 tightBoundingRect（真正的墨迹框），boundingRect 给的是行框，
// 它的 top == -ascent，比墨迹顶高 (ascent-capHeight) ≈ 11px。
// pen 再 snap 到整像素，否则整行抗锯齿都发糊。
static void drawInkText(QPainter &p, const QPointF &inkTopLeft, const QString &s,
                        const QFont &f, const QColor &c, qreal letterSpacing = 0.0)
{
    QFont ff = f;
    if (letterSpacing != 0.0)
        ff.setLetterSpacing(QFont::AbsoluteSpacing, letterSpacing);
    QFontMetricsF fm(ff);
    const QRectF br = fm.tightBoundingRect(s);
    p.setFont(ff);
    p.setPen(c);
    p.drawText(QPointF(qRound(inkTopLeft.x() - br.left()),
                       qRound(inkTopLeft.y() - br.top())), s);
}

// probe 决定「绘制原点」：同一弹层里所有行共用 probe 时 = 原点对齐，
// 各行的墨迹左边界差异来自字符自带的左边距（设计稿即如此）。
static QPointF inkOriginFor(const QFont &f, const QString &probe, const QPointF &inkTopLeft)
{
    QFontMetricsF fm(f);
    const QRectF br = fm.tightBoundingRect(probe);
    return QPointF(qRound(inkTopLeft.x() - br.left()),
                   qRound(inkTopLeft.y() - br.top()));
}

// ---------------------------------------------------------------------------
//  AEC 人体图（矢量）
// ---------------------------------------------------------------------------
//  量自设计稿（367x367 标准尺寸，相对图形框左上角）：
//    方框 367x367、线宽 24（外框 0..367，内框 24..343）
//    领口 = 圆心在框顶上方 34px、(183,-34)、半径 70 / 93 的圆环，被框裁掉上半；
//           环内再填回面板底色 → 得到设计稿那个「凹口 + 项圈」
//    三个场域 75x100、线宽 6：左 (58,83) 右 (233,83) 下 (146,208)
//  卡片上的 58x58 小图就是同一函数，只是 frame 宽 58 —— 但直接缩放会让
//  24*0.158=3.79 / 6*0.158=0.95 这两条线宽落在半像素上，整个图标发糊，
//  所以 <=72 时走一套整数坐标（实测：框线 4、场域 12x16 在 (9,13)/(37,13)/(23,34)、线宽 1）。
//  bg = 领口里要填的底色：弹层里是 #212121，卡片上是 #3B3B3B / #242424（展开态）。
// ---------------------------------------------------------------------------
static void paintAecFigure(QPainter &p, const QRectF &frame, int mask, const QColor &bg)
{
    const int w = qRound(frame.width());
    if (w <= 0)
        return;

    if (w <= 72) {                      // 卡片上的小图：整数坐标，不加抗锯齿
        const qreal bx = frame.x(), by = frame.y();
        const int F = 4;                // 24 * 58/367 = 3.79 → 4

        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        QPainterPath ring;
        ring.setFillRule(Qt::OddEvenFill);
        ring.addRect(QRectF(bx, by, w, w));
        ring.addRect(QRectF(bx + F, by + F, w - 2 * F, w - 2 * F));
        p.fillPath(ring, kInk);

        // 领口（弧线只能抗锯齿）
        p.setRenderHint(QPainter::Antialiasing, true);
        p.save();
        p.setClipRect(QRectF(bx, by, w, w));
        const QPointF c(bx + 29, by - 5.4);
        QPainterPath co, ho;
        co.addEllipse(c, 14.7, 14.7);   // 93 * 0.158
        ho.addEllipse(c, 11.1, 11.1);   // 70 * 0.158
        p.fillPath(co.subtracted(ho), kInk);
        p.fillPath(ho, bg);
        p.restore();

        p.setRenderHint(QPainter::Antialiasing, false);
        const QRect fr[3] = { QRect( 9, 13, 12, 16),
                              QRect(37, 13, 12, 16),
                              QRect(23, 34, 12, 16) };
        for (int i = 0; i < 3; ++i) {
            const QRect r = fr[i].translated(qRound(bx), qRound(by));
            if (mask & (1 << i)) {
                p.fillRect(r, kInk);
                continue;
            }
            // 先拿底色把整块抹掉再描边：选中→取消 时这块本来是实心的，
            // 只描边会留下中间那一坨旧墨迹。
            // ⚠ 线宽必须 2 而不是 1：整个界面在屏上是按 0.6 倍缩放显示的
            //   （1152x720 窗口 / 板上 1280x800），1px 线缩完只剩 0.6px，
            //   Qt 取整后某些边直接消失 —— 表现就是「取消选择后图标边缘残缺」。
            p.fillRect(r, bg);
            const int L = 2;
            p.fillRect(QRect(r.x(),          r.y(),           r.width(), L), kInk);
            p.fillRect(QRect(r.x(),          r.bottom() - L + 1, r.width(), L), kInk);
            p.fillRect(QRect(r.x(),          r.y(),           L, r.height()), kInk);
            p.fillRect(QRect(r.right() - L + 1, r.y(),        L, r.height()), kInk);
        }
        p.restore();
        return;
    }

    const qreal s = frame.width() / 367.0;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(frame.topLeft());
    p.scale(s, s);

    const QRectF outer(0, 0, 367, 367);
    const QRectF inner(24, 24, 319, 319);

    // 方框环（外框 + 内框，奇偶填充）
    QPainterPath ring;
    ring.setFillRule(Qt::OddEvenFill);
    ring.addRect(outer);
    ring.addRect(inner);
    p.fillPath(ring, kInk);

    // 领口
    p.save();
    p.setClipRect(outer);
    QPainterPath collar;
    collar.addEllipse(QPointF(183, -34), 93, 93);
    QPainterPath hole;
    hole.addEllipse(QPointF(183, -34), 70, 70);
    p.fillPath(collar.subtracted(hole), kInk);
    p.fillPath(hole, bg);                  // 环内 = 面板/卡片底色（都是不透明的）
    p.restore();

    // 三个场域：选中的整块填，没选中的描边（线宽 6，路径内缩 3）
    const QRectF fr[3] = { QRectF( 58,  83, 75, 100),
                           QRectF(233,  83, 75, 100),
                           QRectF(146, 208, 75, 100) };
    for (int i = 0; i < 3; ++i) {
        if (mask & (1 << i)) {
            p.fillRect(fr[i], kInk);
        } else {
            // 同上：先抹底再描边，避免「选中→取消」留下实心残影
            p.fillRect(fr[i], bg);
            p.setPen(QPen(kInk, 6));
            p.setBrush(Qt::NoBrush);
            p.drawRect(fr[i].adjusted(3, 3, -3, -3));
        }
    }
    p.restore();
}

// ================================ 下拉弹层 ==================================
DropPopup::DropPopup(QWidget *parent)
    : QWidget(parent)
    , m_size(kPopSize)
    , m_rowTop(kRowTopRel)
    , m_rowH(kRowH)
    , m_iconBox(kIconBoxDef)
{
    // 注意：这里 **不能** 用 Qt::Popup —— 本工程整页被 QGraphicsView 旋转缩放，
    // 顶层窗口不参与该变换。做成普通子控件，跟着页面一起变换。
    setAttribute(Qt::WA_NoMousePropagation, false);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setFixedSize(m_size);

    // 默认 = Patient type 弹层的版式（其余弹层用 setter 覆盖）
    setHeader(QStringLiteral("Patient type"), kHeaderPx, kHeaderTrack, QPoint(29, 39));
    setRowStyle(kLetterPx, QPoint(117, 42), QStringLiteral("M"));

    // 惯性 / 吸附动画：16ms ≈ 60fps。只在松手或点轨道时启动，
    // 停下后立刻 stop()，不占 CPU（板上 RK3566 单核跑 UI，空闲必须为 0）。
    m_anim = new QTimer(this);
    m_anim->setTimerType(Qt::PreciseTimer);
    m_anim->setInterval(16);
    connect(m_anim, &QTimer::timeout, this, [this] { onAnimTick(); });
}

void DropPopup::setPanelSize(const QSize &sz)
{
    m_size = sz;
    setFixedSize(sz);
    update();
}

void DropPopup::setRowMetrics(int firstRowTop, int rowHeight)
{
    m_rowTop = firstRowTop;
    m_rowH   = rowHeight;
    update();
}

void DropPopup::setIconBox(const QRect &r)
{
    m_iconBox = r;
    update();
}

void DropPopup::setListRight(int x)
{
    m_listRight = x;
    update();
}

void DropPopup::setScrollbar(const QRect &track, const QRect &thumb)
{
    m_sbTrack = track;
    m_sbThumb = thumb;
    update();
}

void DropPopup::setHeader(const QString &text, int px, qreal track, const QPoint &inkTopLeft)
{
    m_headerText  = text;
    m_headerFont  = makeFont(px);
    m_headerTrack = track;
    m_hdrInk      = inkTopLeft;
    update();
}

void DropPopup::setRowStyle(int px, const QPoint &inkTopLeft, const QString &probe)
{
    m_rowFont  = makeFont(px);
    m_rowInk   = inkTopLeft;
    m_rowProbe = probe;
    update();
}

void DropPopup::addItem(const QString &text, const QString &iconPath)
{
    Item it;
    it.text = text;
    if (!iconPath.isEmpty()) {           // 空 = 这一列没有图标，别报警
        it.icon.load(iconPath);
        if (it.icon.isNull())
            qWarning("!! 图标加载失败: %s", qPrintable(iconPath));
    }
    m_items.append(it);
}

void DropPopup::setCurrent(int idx)
{
    m_current = idx;
    m_hover   = idx;          // 展开时高亮「当前值」那一行（与设计稿一致）
    m_pending = -1;           // 新一轮打开：上一轮没确认的待选行作废
    ensureVisible(idx, false); // 当前值在第 6 行以后时，光高亮看不到 —— 直接就位
    update();
}

// ------------------------------ 滚动实现 -----------------------------------
//  只有「行数 > visibleRows」时才真的滚；否则 m_scroll 恒为 0、滑块退回设计稿
//  那条 156px 的固定长度，像素与改动前完全一致。
void DropPopup::setVisibleRows(int n)
{
    m_visibleRows = qMax(0, n);
    m_scroll      = 0;
    m_scrollPx    = 0.0;
    stopAnim();
    update();
}

bool DropPopup::scrolling() const
{
    return m_visibleRows > 0 && m_items.size() > m_visibleRows;
}

int DropPopup::maxScroll() const
{
    if (!scrolling())
        return 0;
    return m_items.size() - m_visibleRows;
}

// 内容能滚多远（像素）。滚动条比例、边界判断都用它。
qreal DropPopup::maxScrollPx() const
{
    return qreal(maxScroll()) * qMax(1, m_rowH);
}

// 行级入口（键盘上下、ensureVisible、旧调用）。内部统一折算成像素，
// 保证 m_scrollPx 永远是唯一真身，m_scroll 只是派生值。
void DropPopup::setScroll(int s)
{
    setScrollPx(qreal(qBound(0, s, maxScroll())) * m_rowH);
}

// ★ 像素级滚动（丝滑的核心）：手指划多少就滚多少，不再按行取整。
//   之前是按行 round，手指慢速滑动时会「攒够一行才跳一下」，看起来
//   就是用户说的「瞬间翻页」。改成像素后跟手连续，配合松手惯性才顺。
//
//   elastic=true 时允许越界（阻尼 0.35、上限 140 设计 px）：
//   这是给「内容不足一屏」的弹层（EL 5 档、焦点 2 项）用的橡皮筋 ——
//   没内容可翻也有物理反馈，而不是干脆不动（用户会觉得"划了没反应"）。
//   松手由 springBack() 弹回。
void DropPopup::setScrollPx(qreal px, bool elastic)
{
    const qreal mx = maxScrollPx();
    qreal np = px;
    if (elastic) {
        if (np < 0.0)
            np = -qMin(-np * 0.35, 140.0);
        else if (np > mx)
            np = mx + qMin((np - mx) * 0.35, 140.0);
    } else {
        np = qBound(0.0, np, mx);
    }

    const int ns = qBound(0, int(np / qMax(1, m_rowH)), maxScroll());
    if (std::fabs(np - m_scrollPx) < 0.01) {
        m_scroll = ns;                  // 位置没变，派生值也保持同步
        return;
    }
    m_scrollPx = np;
    m_scroll   = ns;
    update();
}

void DropPopup::startAnim()
{
    if (!m_anim)
        return;
    m_tickLastMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_anim->isActive())
        m_anim->start();
}

void DropPopup::stopAnim()
{
    if (m_animMode != 0)          // 只在真的在动画时打一行，方便核对惯性行程
        qDebug("[popup] anim stop px=%.1f (mode=%d)", m_scrollPx, m_animMode);
    m_animMode = 0;
    m_vel      = 0.0;
    if (m_anim && m_anim->isActive())
        m_anim->stop();
}

// 平滑滚到某个像素位置：点轨道翻页、惯性停下后对齐整行都走这里。
void DropPopup::animateTo(qreal target)
{
    m_targetPx = qBound(0.0, target, maxScrollPx());
    if (std::fabs(m_targetPx - m_scrollPx) < 0.8) {
        setScrollPx(m_targetPx);
        return;
    }
    m_animMode = 2;
    startAnim();
}

// 橡皮筋回弹：不可滚弹层被拉出去后收回原位（指数衰减，两端都管）
void DropPopup::springBack()
{
    const qreal mx = maxScrollPx();
    if (m_scrollPx >= 0.0 && m_scrollPx <= mx) {
        stopAnim();
        return;
    }
    m_animMode = 3;
    startAnim();
}

void DropPopup::onAnimTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // dt 上限 48ms：板子上万一被抢占卡了一帧，不要让内容「瞬移」一大段
    const qreal dt = qBound(1.0, qreal(now - m_tickLastMs), 48.0);
    m_tickLastMs = now;
    const qreal maxPx = maxScrollPx();

    if (m_animMode == 1) {                      // 松手惯性滑行
        // 摩擦：0.93 时约 180ms 才减半，滑行太远；0.90 约 105ms 减半，
        // 松手后滑一小段就停，像手机原生列表的手感（2026-11-16 手感调整）
        m_vel *= std::pow(0.90, dt / 16.0);
        if (std::fabs(m_vel) < 0.05) {          // 太慢了就直接停，避免长时间微抖
            stopAnim();
            snapToRow();
            return;
        }
        const qreal np = m_scrollPx + m_vel * dt;
        if (np <= 0.0 || np >= maxPx) {         // 撞到两端：就地停，不回弹（下拉框不弹）
            setScrollPx(np);
            stopAnim();
            return;
        }
        setScrollPx(np);
        return;
    }

    if (m_animMode == 2) {                      // 趋向目标（翻页 / 吸附）
        const qreal d = m_targetPx - m_scrollPx;
        if (std::fabs(d) < 0.8) {
            setScrollPx(m_targetPx);
            stopAnim();
            return;
        }
        // 指数逼近：每帧吃掉剩余距离的 ~22%，约 150ms 到位，两端都平滑
        // （收到 150ms 后「对齐整行」那一下几乎看不出来，又不至于拖沓）
        setScrollPx(m_scrollPx + d * qMin(1.0, dt / 65.0));
        return;
    }

    if (m_animMode == 3) {                      // 橡皮筋回弹
        const qreal mx = maxScrollPx();
        const bool  lo = (m_scrollPx < 0.0);
        const bool  hi = (m_scrollPx > mx);
        if (!lo && !hi) {                       // 已经被别的路径收回去了
            stopAnim();
            return;
        }
        qreal e = lo ? m_scrollPx : (m_scrollPx - mx);
        e *= std::pow(0.80, dt / 16.0);
        if (std::fabs(e) < 0.6) {
            setScrollPx(lo ? 0.0 : mx);
            stopAnim();
            return;
        }
        setScrollPx(lo ? e : mx + e, true);      // 继续保持弹性坐标
        return;
    }

    stopAnim();
}

// 惯性停下后对齐到整行：否则列表会长期停在「第一行被切一半」的位置，
// 视觉上不像下拉框（设计稿的行是完整块）。只在小差距内吸附，避免突然跳。
void DropPopup::snapToRow()
{
    if (!scrolling())
        return;
    const qreal rowH = qMax(1, m_rowH);
    qreal t = qRound(m_scrollPx / rowH) * rowH;
    t = qBound(0.0, t, maxScrollPx());
    if (std::fabs(t - m_scrollPx) < 1.0) {
        setScrollPx(t);
        return;
    }
    animateTo(t);
}

void DropPopup::ensureVisible(int idx, bool animate)
{
    if (!scrolling() || idx < 0)
        return;
    const qreal rowH  = qMax(1, m_rowH);
    const qreal top   = idx * rowH;
    const qreal viewH = m_visibleRows * rowH;
    qreal t = m_scrollPx;
    if (top < t)
        t = top;
    else if (top + rowH > t + viewH)
        t = top + rowH - viewH;
    t = qBound(0.0, t, maxScrollPx());
    if (animate)
        animateTo(t);
    else
        setScrollPx(t);          // 展开时直接就位，别让用户看到「自己滚过去」
}

// 滑块几何。不滚动时直接用设计稿量出来的 m_sbThumb（16x156 @ (483,105)）；
// 滚动时按「可见行数 / 总行数」算长度、按 m_scroll 比例算位置。
QRect DropPopup::thumbRect() const
{
    if (m_sbTrack.isNull())
        return QRect();
    if (!scrolling())
        return m_sbThumb;

    const int pad = m_sbThumb.isNull() ? 8 : (m_sbThumb.x() - m_sbTrack.x());
    const int w   = m_sbThumb.isNull() ? qMax(8, m_sbTrack.width() - 2 * pad)
                                       : m_sbThumb.width();
    const int y0  = m_sbThumb.isNull() ? (m_sbTrack.y() + pad) : m_sbThumb.y();
    const int span = m_sbTrack.height() - 2 * (y0 - m_sbTrack.y());

    const int n = m_items.size();
    int h = (n > 0) ? int(qint64(span) * m_visibleRows / n) : span;
    // 设计稿那条 156 是观感基准：内容超出不多时保持同样长度、只挪位置。
    // 若纯按比例算，5 项→6 项时滑块会从 156 突然跳到满轨（544），
    // 与设计稿差得太远；行数很多时才让比例接管。
    const int designH = m_sbThumb.isNull() ? span : m_sbThumb.height();
    h = qBound(qMin(48, span), h, designH);     // 太短就不像滑块了

    // 位置按像素偏移占比算（不是行下标），拖动过程中滑块才会连续跟手
    const qreal mx   = maxScrollPx();
    const qreal frac = (mx > 0.0) ? qBound(0.0, m_scrollPx / mx, 1.0) : 0.0;
    const int   y    = y0 + int(qRound((span - h) * frac));
    return QRect(m_sbTrack.x() + pad, y, w, h);
}

int DropPopup::indexOfText(const QString &t) const
{
    // 手写配置时最常错的是空格、大小写，以及「长 minus(U+2212) / 短连字符」——
    // 设计稿的弹层行用 U+2212、按钮上印的是 '-'，两边都当同一个字看待。
    auto squash = [](const QString &s) {
        QString r;
        for (const QChar c : s) {
            if (c.isSpace()) continue;
            r.append(c == QChar(0x2212) ? QLatin1Char('-') : c);
        }
        return r;
    };
    const QString k = squash(t);
    for (int i = 0; i < m_items.size(); ++i)
        if (squash(m_items[i].text).compare(k, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

void DropPopup::setHoverIndex(int idx) { m_hover = idx; update(); }

// 「待确认行」只在弹层真正收起时才提交。
// 点选现在是「立即 emit + 立即 close」，那时 m_current 已经同步，
// 这里会走 idx == m_current 的跳过分支（不重复 emit）。
// 保留它是为了兜底：面板被其它路径收起时（点空白、再点卡片），
// 还没落定的那一行不会丢。
void DropPopup::commitPending()
{
    if (m_pending < 0)
        return;
    const int idx = m_pending;
    m_pending = -1;
    if (idx == m_current) {
        // 点行时已经实时 emit 过（选中即生效），收起面板时不要再发一次，
        // 否则卡片会重复 setIndex、联动重绘两遍
        m_hover = idx;
        return;
    }
    m_current = idx;
    m_hover   = idx;
    qDebug("[popup] commit idx=%d '%s'", idx, qPrintable(itemText(idx)));
    emit picked(idx);
}

void DropPopup::hideEvent(QHideEvent *e)
{
    stopAnim();               // 收起时掐掉惯性/回弹定时器，别在隐藏控件上空转
    setScrollPx(0.0);         // 下次打开从头显示（越界值也一并清掉）
    commitPending();          // 先落定，再广播「已关闭」（卡片据此恢复常态底色）
    emit closed();
    QWidget::hideEvent(e);
}

void DropPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 不铺满整个窗口：圆角外的像素留空，让父页面的底图透出来（子控件能做到，
    // 顶层窗口做不到）。文字/图标都画在面板内部，天然不越界。
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                        kPopRadius, kPopRadius);
    p.fillPath(path, kPopBg);
    p.setPen(QPen(kPopBorder, 1));
    p.drawPath(path);

    // 标题：设计稿把下伸部分裁平在 y=340（'y' 尾巴和 'p' 竖干一起被切齐）
    p.save();
    p.setClipRect(QRectF(0, 0, width(), kHdrClipBottom));
    drawInkText(p, QPointF(m_hdrInk), m_headerText, m_headerFont, kHeaderInk, m_headerTrack);
    p.restore();

    // 列表区右边界：有滚动条时高亮要让出轨道那一条
    const int listRight = (m_listRight > 0) ? m_listRight : (width() - 2);

    // 行。滚动时按**像素偏移**画（丝滑滚动的绘制端）：
    //   第一行可能是「半行」，所以按 m_scrollPx 反算绝对下标，
    //   再把整个列表区裁一刀 —— 不让半行画到标题上、也不让它溢出面板底边。
    //   不滚动（m_visibleRows==0）时 off 恒为 0，绘制结果与改动前逐像素一致。
    const int  offI     = int(qRound(m_scrollPx));
    const int  firstRow = (m_visibleRows > 0)
                          ? qMax(0, int(std::floor(m_scrollPx / qMax(1, m_rowH))))
                          : 0;
    const int  lastRow  = (m_visibleRows > 0)
                          ? qMin(m_items.size(), firstRow + m_visibleRows + 1)
                          : m_items.size();

    p.save();
    if (m_visibleRows > 0 || std::fabs(m_scrollPx) > 0.01) {
        // 不可滚的弹层（m_visibleRows==0）在橡皮筋拉动时也要裁，
        // 否则行会画到标题上去；静态时（scrollPx==0）不裁，逐像素不变。
        const int clipRows = (m_visibleRows > 0) ? m_visibleRows : m_items.size();
        // IntersectClip：与已建立的裁剪求交，不要替换（否则高亮会溢出）
        p.setClipRect(QRectF(0, m_rowTop, width(), clipRows * m_rowH),
                      Qt::IntersectClip);
    }
    for (int i = firstRow; i < lastRow; ++i) {
        const int top = m_rowTop + i * m_rowH - offI;
        if (i == m_hover) {
            // 整行高亮，左右顶到描边内侧
            p.save();
            QPainterPath rp;
            rp.addRoundedRect(QRectF(0.5, 0.5, width() - 1, height() - 1),
                              kPopRadius, kPopRadius);
            p.setClipPath(rp, Qt::IntersectClip);
            if (i == m_pending) {
                // 待确认：蓝底 + 左侧竖条，一眼看出"再点一下就生效"
                p.fillRect(QRect(1, top, listRight - 1, m_rowH), kRowPend);
                p.fillRect(QRect(1, top, 6, m_rowH),             kRowPendBar);
            } else {
                p.fillRect(QRect(1, top, listRight - 1, m_rowH), kRowHi);
            }
            p.restore();
        }
        if (!m_iconBox.isNull() && !m_items[i].icon.isNull())
            p.drawPixmap(m_iconBox.translated(0, top), m_items[i].icon);

        const QString &probe = m_rowProbe.isEmpty() ? m_items[i].text : m_rowProbe;
        const QPointF origin = inkOriginFor(m_rowFont, probe,
                                            QPointF(m_rowInk.x(), top + m_rowInk.y()));
        p.setFont(m_rowFont);
        p.setPen(kInk);
        p.drawText(origin, m_items[i].text);
    }
    p.restore();

    // 滚动条：轨道先铺（会盖住整行高亮），滑块再叠上去。
    // 设计稿里轨道是列表区高度的一条 #171717 暗带，滑块是 16 宽的圆头胶囊。
    // 行数超出可见区时滑块改成动态（长度/位置跟内容走），否则保持设计稿那一条。
    if (!m_sbTrack.isNull()) {
        p.fillRect(m_sbTrack, kSbTrack);
        const QRect thumb = thumbRect();
        if (!thumb.isNull()) {
            const qreal r = thumb.width() / 2.0;
            QPainterPath tp;
            tp.addRoundedRect(QRectF(thumb), r, r);
            p.fillPath(tp, kSbThumb);
        }
    }
}

void DropPopup::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->pos();

    if (m_dragThumb) {
        // 拖滑块：滑块行程 ↔ 内容像素一一映射，拖动过程连续跟手
        const QRect th  = thumbRect();
        const int   y0  = m_sbThumb.isNull() ? (m_sbTrack.y() + 8) : m_sbThumb.y();
        const int   span = m_sbTrack.height() - 2 * (y0 - m_sbTrack.y());
        const int   travel = qMax(1, span - th.height());
        const qreal dy = pos.y() - m_dragPos0.y();
        setScrollPx(m_dragScroll0Px + dy * maxScrollPx() / qMax(1, travel));
        e->accept();
        return;
    }
    if (m_dragList) {
        const int dx = pos.x() - m_dragPos0.x();
        const int dy = pos.y() - m_dragPos0.y();
        // 死区：手指点按时会有几像素抖动，超过这个才算「划动」。
        // 用 dx/dy 的合成距离，不只看 dy —— 面板旋转 270° 后，手指的竖向
        // 划动在控件局部坐标里可能落到 x 轴上，只看 dy 会漏判成「点击」。
        // 一旦判定为划动，本次手势就**不会**再选中任何行（回不到 tap）。
        //
        // ⚠ 这里用的是**设计坐标**：1400 归一到 1024 屏是 0.5 倍，12 设计 px
        //    只有 6 屏幕 px —— 手指点按的正常抖动就会超，会把「点击」误判成
        //    「划动」导致点不中。取 24（≈12 屏幕 px），比一行（112 设计 px /
        //    ≈56 屏幕 px）小得多，真划动照样能识别。
        if (!m_dragMoved && (qAbs(dx) > 24 || qAbs(dy) > 24)) {
            m_dragMoved = true;
            m_vel       = 0.0;          // 刚进入划动，速度重新采样
            m_velLastMs = 0;
        }

        if (m_dragMoved) {
            if (scrolling()) {
                // ★ 像素级跟手：手指划多少、内容滚多少。
                //   向上划（dy<0）= 看后面的行，所以是「减 dy」。
                //   之前是 round(dy/行高) 扔给 setScroll，手指慢划时会攒够
                //   一行才跳一格 —— 那就是「瞬间翻页」的观感来源。
                const qreal before = m_scrollPx;
                setScrollPx(m_dragScroll0Px - dy);

                // 采样速度（px/ms）给松手惯性用。用「实际位移」而不是手指
                // 位移：撞到两端时实际位移为 0，速度自然归零，松手不会继续甩。
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (m_velLastMs > 0) {
                    const qreal dtms = qMax(1.0, qreal(now - m_velLastMs));
                    const qreal inst = (m_scrollPx - before) / dtms;
                    m_vel = m_vel * 0.55 + inst * 0.45;   // 指数平滑，抗单帧抖动
                    // 钳到 ±4 设计 px/ms：异常帧（掉帧后补发一串 move）不会
                    // 算出天文速度把列表一下甩到尽头
                    m_vel = qBound(-4.0, m_vel, 4.0);
                }
                m_velLastMs = now;
            } else {
                // 一屏就放得下（EL 5 档 / 焦点 2 项 / 曝光参数 3 项）：
                // 没有下一页可翻，但也不能「划了完全不动」—— 给橡皮筋阻尼：
                // 手指拉得动、松手弹回，物理上明确告诉用户「已经到底了」。
                //
                // ⚠ 这里不再让高亮跟着手指走。高亮逐行跳变看起来就像在
                //    「翻页选中」，正是用户说的「瞬间翻页」；而且松手回弹
                //    时高亮又会跳一次，更容易被误认成误选。
                setScrollPx(m_dragScroll0Px - dy, true);
            }
        }
        e->accept();
        return;
    }

    const int idx = indexAt(pos);
    if (idx != m_hover) { m_hover = idx; update(); }
}

void DropPopup::mousePressEvent(QMouseEvent *e)
{
    const QPoint pos = e->pos();

    // 任何新按下都先掐掉正在跑的惯性/吸附：手指摸上来就要立刻「抓住」列表，
    // 不能还在自己滑（这是丝滑手感的关键细节）。
    stopAnim();
    m_vel       = 0.0;
    m_velLastMs = 0;

    // 滚动条优先（只存在于「行数 > 可见行数」的弹层）
    if (scrolling() && m_sbTrack.contains(pos)) {
        const QRect th = thumbRect();
        if (th.contains(pos)) {
            m_dragThumb     = true;
            m_dragPos0      = pos;
            m_dragScroll0   = m_scroll;
            m_dragScroll0Px = m_scrollPx;
        } else {
            // 点轨道空白 = 翻一屏（平滑滚过去，不瞬移）
            const int dir = (pos.y() < th.center().y()) ? -1 : 1;
            animateTo(m_scrollPx + dir * m_visibleRows * m_rowH);
        }
        e->accept();
        return;
    }

    // 列表：按下只记账，**什么都不提交**。
    // 触屏上 press 之后可能是「划动翻页」，也可能是「点选参数」，
    // 只有松手那一刻（且全程没划过 12px）才分得出来 —— 见 mouseReleaseEvent。
    m_dragList      = true;
    m_dragMoved     = false;
    m_pressIdx      = indexAt(pos);
    m_dragPos0      = pos;
    m_dragScroll0   = m_scroll;
    m_dragScroll0Px = m_scrollPx;
    m_hover         = m_pressIdx;    // 按下先点亮，给个即时反馈；划走会跟着手指走
    update();
    e->accept();
}

void DropPopup::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_dragThumb) { m_dragThumb = false; e->accept(); return; }
    if (m_dragList) {
        // 全程没划动（<12px）才算「点选」；划过一律只当翻页/浏览。
        // 这样「按住划下去翻参数、松手」永远不会把值改掉。
        const int  idx = m_pressIdx;
        const bool tap = !m_dragMoved && idx >= 0;
        m_dragList = false;
        m_pressIdx = -1;

        if (tap) {
            // 点一下某行 = 立即生效并收起面板（普通单选语义）。
            // 中途有一版改成了「点亮即选中、点卡片才关」（AEC 场域那套），
            // 但 AEC 场域弹层自己重写了 press/release、根本不走这里，
            // 基类没必要跟着不关 —— 2026-11-20 按用户要求恢复成点完就关。
            if (idx != m_current) {
                m_pending = idx;
                m_hover   = idx;
                m_current = idx;       // 已生效，hideEvent 的提交会跳过、不重复 emit
                emit picked(idx);
                qDebug("[popup] pick idx=%d '%s' (applied, closing)",
                       idx, qPrintable(itemText(idx)));
                update();
            }
            close();                   // hideEvent → commitPending(跳过) → closed
        } else {
            // 划动结束：不选中任何行，高亮回弹到「待确认 / 当前值」
            const int back = (m_pending >= 0) ? m_pending : m_current;
            if (m_hover != back) { m_hover = back; update(); }

            if (scrolling()) {
                // ★ 松手惯性：手指甩出去的速度继续滑，摩擦衰减后对齐整行。
                //   有这一下才叫「丝滑」——只看位移、松手即停，手感是硬的。
                if (std::fabs(m_vel) > 0.06) {
                    m_animMode = 1;
                    startAnim();
                } else {
                    snapToRow();
                }
                qDebug("[popup] drag end px=%.0f/%.0f v=%.2f no pick",
                       m_scrollPx, maxScrollPx(), m_vel);
            } else {
                springBack();          // 不可滚：橡皮筋弹回原位
                qDebug("[popup] drag end (not scrollable) rubber px=%.0f, no pick",
                       m_scrollPx);
            }
        }
        e->accept();
        return;
    }
    e->accept();
}

void DropPopup::wheelEvent(QWheelEvent *e)
{
    if (!scrolling()) { e->ignore(); return; }
    const int dy = e->angleDelta().y();
    if (dy == 0) { e->ignore(); return; }
    animateTo(m_scrollPx + (dy > 0 ? -m_rowH : m_rowH));   // 向上滚 = 看前面的行
    e->accept();
}

void DropPopup::keyPressEvent(QKeyEvent *e)
{
    // Esc = 取消：把待确认行扔掉，再收面板（否则 hideEvent 会把它提交掉）
    if (e->key() == Qt::Key_Escape) { m_pending = -1; close(); return; }
    if (e->key() == Qt::Key_Down)   { m_hover = qMin(m_hover + 1, m_items.size() - 1); ensureVisible(m_hover); update(); return; }
    if (e->key() == Qt::Key_Up)     { m_hover = qMax(m_hover - 1, 0); ensureVisible(m_hover); update(); return; }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        m_pending = -1;                 // 直接提交，别让 hideEvent 再来一次
        m_current = m_hover; emit picked(m_current); close(); return;
    }
    QWidget::keyPressEvent(e);
}

int DropPopup::indexAt(const QPoint &pt) const
{
    const int listRight = (m_listRight > 0) ? m_listRight : (width() - 2);
    if (pt.x() >= listRight) return -1;       // 滚动条那一条不算行
    if (pt.y() < m_rowTop)   return -1;
    const int   vis  = (m_visibleRows > 0) ? m_visibleRows : m_items.size();
    const qreal rowH = qMax(1, m_rowH);

    // ① 先按「屏幕内的第几行」判断是否落在列表区（不能拿绝对索引去比 vis ——
    //    那是滚到底页时点不中后几行的根因：8 档滚到 px=336 后，屏幕第 2 行
    //    的绝对索引是 4，`4 >= vis(5)` 不成立但第 5 行是 7，`7 >= 5` 就被
    //    当成「列表下方空白」直接丢掉，表现就是「划到下一页后选不了」。
    const int visRow = int((pt.y() - m_rowTop) / rowH);
    if (visRow < 0 || visRow >= vis) return -1;

    // ② 再叠上像素滚动偏移，算出手指真正落在哪一条绝对行。
    //    用 floor 而不是 round：行顶被裁掉一半时，落在可见那半边上就算选中它。
    const int i = int(std::floor((qreal(pt.y() - m_rowTop) + m_scrollPx) / rowH));
    return (i >= 0 && i < m_items.size()) ? i : -1;
}

// ================================ 卡片基类 ==================================
DropCardBase::DropCardBase(QWidget *cardParent)
    : QToolButton(cardParent)
{
    setGeometry(kCardRect);
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:0;}"));

    m_tri.load(QStringLiteral(":/mselect/tri.png"));
    if (m_tri.isNull())
        qWarning("!! 三角切图加载失败");

    // 弹层建在「卡片所在的页面」上：它要比卡片大、还要盖住下面的其它卡片，
    // 所以父控件必须是整页而不能是卡片本身（子控件会被父控件裁剪）。
    QWidget *page = cardParent ? cardParent : this;
    m_popup = new DropPopup(page);
    m_popup->hide();

    connect(this, &QToolButton::clicked, this, &DropCardBase::togglePopup);
    connect(m_popup, &DropPopup::closed, this, [this] {
        m_popupOpen = false;
        update();
        emit popupClosed();
    });

    // 点弹层以外的地方关闭：装在页面上，凡冒泡到页面的鼠标按下就是「点空白」
    if (cardParent)
        cardParent->installEventFilter(this);
}

// 弹层跟着卡片走，横向自动选边：
//   卡片右边剩下的空间 < 弹层比卡片多出来的宽度 ⇒ 弹层贴卡片右边，否则贴左边，
//   两侧都再向外 1px（设计稿：竖屏 Patient 左外 1px、Workspot 右外 1px）。
// 弹层宽度取它自己的（不再强制 ≥ 卡片宽）：Exposure parameters 的卡片 749 宽、
// 弹层只有 508，必须按 508 摆，否则会贴错边。
void DropCardBase::layoutPopup()
{
    const int popW = m_popup->width();
    QWidget *page = parentWidget();
    const int rightGap = page ? (page->width() - (m_cardRect.x() + m_cardRect.width())) : 0;

    const int x = (rightGap < popW - m_cardRect.width())
                  ? (m_cardRect.x() + m_cardRect.width() + 1 - popW)   // 贴右边
                  : (m_cardRect.x() - 1);                              // 贴左边
    m_popup->setGeometry(x + m_popOff.x(),
                         m_cardRect.y() + m_cardRect.height() + kPopGapY + m_popOff.y(),
                         popW, m_popup->height());
}

// 构造时建的是普通 DropPopup；AEC 场域弹层的版式完全不是行列表，需要换掉。
// 复用构造函数里那套接线（closed → 恢复卡片常态底色）。
void DropCardBase::replacePopup(DropPopup *p)
{
    if (!p || p == m_popup)
        return;
    DropPopup *old = m_popup;
    m_popup = p;
    m_popup->hide();
    connect(m_popup, &DropPopup::closed, this, [this] {
        m_popupOpen = false;
        update();
        emit popupClosed();
    });
    if (old) {
        old->hide();
        old->deleteLater();
    }
}

void DropCardBase::openPopupAt()
{
    // 同一页面上其它卡片的下拉先收起来（两个卡片并存时才有的问题）
    if (QWidget *page = parentWidget()) {
        const QList<DropCardBase *> others = page->findChildren<DropCardBase *>();
        for (DropCardBase *o : others)
            if (o != this && o->isPopupOpen())
                o->closePopup();
    }

    syncPopupCurrent();
    m_popupOpen = true;
    update();

    layoutPopup();          // 位置按当前版式重算（换过朝向也不会跑位）
    m_popup->raise();
    m_popup->show();
    m_popup->setFocus();
}

void DropCardBase::closePopup()
{
    if (m_popup->isVisible())
        m_popup->hide();          // hideEvent → closed → 恢复常态底色
    else {
        m_popupOpen = false;
        update();
    }
}

void DropCardBase::togglePopup()
{
    if (m_popupOpen) closePopup();
    else             openPopupAt();
}

bool DropCardBase::eventFilter(QObject *obj, QEvent *e)
{
    if (m_popupOpen && (e->type() == QEvent::MouseButtonPress
                        || e->type() == QEvent::TouchBegin)) {
        // 能冒泡到这里说明按下的点在弹层/卡片之外
        closePopup();
    }
    return QToolButton::eventFilter(obj, e);
}

void DropCardBase::paintEvent(QPaintEvent *)
{
    // ChoiceCard（m_paintCard=false）：卡片图形在底图 PNG 里，关闭态不用重绘；
    // 但展开态设计稿会把卡片压暗（#3B3B3B → #242424），那时候必须自己铺底色
    // 再把内容层叠回去，底图改不了。
    // m_forceDraw：用户已经选了别的值，烘焙的那份不成立了，关闭态也得自己画。
    if (!m_popupOpen && !m_paintCard && !m_forceDraw)
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 展开态一律要铺底色（设计稿把卡片压暗成 #242424）；
    // ChoiceCard 的内容层只有图形（图标/文字/三角），底色是靠这一层填出来的。
    // 关闭态只有 m_fillBg=true 的卡片才铺（ChoiceCard 不铺，让底图原样透出）。
    if (m_fillBg || m_popupOpen) {
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()), kCardRadius, kCardRadius);
        p.fillPath(path, m_popupOpen ? kCardBgActive : kCardBg);
    }

    paintContent(p);

    if (!m_tri.isNull())
        p.drawPixmap(m_triRel, m_tri);
}

// ================================ 体型卡片 ==================================
// 卡片图标随体型切换。三张图都是 64x64，alpha 墨迹中心实测都在 (31.5,31.5)，
// 所以直接叠在同一个图标框里，切换时不会左右跳。
// M 用 icon_card.png 而不是 icon_M.png：前者是设计稿卡片区域的原样抠图，
// 与已建立的逐像素校验基线（关闭态 diff 427）对齐；icon_M 是弹层行内版本，
// 与 icon_card 差半个像素的整体位移，换上去等于把基线推翻重来。
static const char *kBodyIconPath[3] = {
    ":/mselect/icon_L.png",
    ":/mselect/icon_card.png",
    ":/mselect/icon_S.png",
};

static int letterIndex(const QString &v)
{
    const int i = QStringLiteral("LMS").indexOf(v);
    return i < 0 ? 1 : i;          // 认不出的值一律当 M
}

PatientCard::PatientCard(QWidget *cardParent)
    : DropCardBase(cardParent)
{
    for (int i = 0; i < 3; ++i) {
        m_iconSet[i].load(QString::fromLatin1(kBodyIconPath[i]));
        if (m_iconSet[i].isNull())
            qWarning("!! 卡片图标加载失败: %s", kBodyIconPath[i]);
    }
    m_font = makeFont(kLetterPx);

    m_popup->addItem(QStringLiteral("L"), QStringLiteral(":/mselect/icon_L.png"));
    m_popup->addItem(QStringLiteral("M"), QStringLiteral(":/mselect/icon_M.png"));
    m_popup->addItem(QStringLiteral("S"), QStringLiteral(":/mselect/icon_S.png"));

    connect(m_popup, &DropPopup::picked, this, [this](int idx) {
        setValue(QStringLiteral("LMS").mid(idx, 1));
    });

    // 兜底套一遍竖屏版式（main() 按实际朝向还会再调一次 applyProfile）
    applyProfile(dr::portraitProfile());
}

// 换版式：卡片位置、卡片内三个元素、弹层位置一起换。
// 弹层尺寸不变 —— 「Patient type + 三行」的内容与朝向无关：
//   竖屏卡片 432 / 横屏 512，都 ≤ 弹层 514，视觉就是对牢的。
void PatientCard::applyProfile(const dr::LayoutProfile &p)
{
    const dr::Hotspot *h = p.spot(QStringLiteral("bodyPart"));
    if (!h) {
        qWarning("!! profile '%s' 里没有 bodyPart 热区", qPrintable(p.name));
        return;
    }

    m_iconRel = p.cardIcon;
    m_inkRel  = p.cardInk;
    m_triRel  = p.cardTri;

    setCardRect(h->r);
    layoutPopup();
    update();
}

void PatientCard::setValue(const QString &v)
{
    if (m_value == v) return;
    m_value = v;
    update();
    emit valueChanged(v);
}

void PatientCard::syncPopupCurrent()
{
    m_popup->setCurrent(letterIndex(m_value));
}

void PatientCard::paintContent(QPainter &p)
{
    const QPixmap &ic = m_iconSet[letterIndex(m_value)];
    if (!ic.isNull())
        p.drawPixmap(m_iconRel, ic);
    drawInkText(p, QPointF(m_inkRel), m_value, m_font, kInk);
}

// ================================ 站位卡片 ==================================
// 三个图标都由设计稿反解 alpha 得到（64x64，墨迹 56x56 居中），
// 与卡片上的图标是同一份切图 —— 选 Table 后卡片立刻显示桌子那个图形。
static const char *kSpotIconPath[3] = {
    ":/mselect/icon_ws_wallstand.png",
    ":/mselect/icon_ws_table.png",
    ":/mselect/icon_ws_free.png",
};

WorkspotCard::WorkspotCard(QWidget *cardParent)
    : DropCardBase(cardParent)
{
    for (int i = 0; i < 3; ++i) {
        m_iconSet[i].load(QString::fromLatin1(kSpotIconPath[i]));
        if (m_iconSet[i].isNull())
            qWarning("!! 站位图标加载失败: %s", kSpotIconPath[i]);
    }

    // Workspot 弹层：标题墨迹 (26,39) 42px/-0.4；
    // 行文字 42px，墨迹锚点 (114,37)，probe 固定 "Wallstand" ⇒ 原点对齐，
    // Table / Free detector 的左边界差异自动由字符左边距产生（与设计稿一致）。
    m_popup->setHeader(QStringLiteral("Workspot"), kHeaderPx, kHeaderTrack, QPoint(26, 39));
    m_popup->setRowStyle(kPopRowPx, QPoint(114, 37), QStringLiteral("Wallstand"));

    m_popup->addItem(QStringLiteral("Wallstand"),     QString::fromLatin1(kSpotIconPath[0]));
    m_popup->addItem(QStringLiteral("Table"),         QString::fromLatin1(kSpotIconPath[1]));
    m_popup->addItem(QStringLiteral("Free detector"), QString::fromLatin1(kSpotIconPath[2]));

    connect(m_popup, &DropPopup::picked, this, &WorkspotCard::setIndex);

    applyProfile(dr::portraitProfile());
}

void WorkspotCard::applyProfile(const dr::LayoutProfile &p)
{
    const dr::Hotspot *h = p.spot(QStringLiteral("lamp"));
    if (!h) {
        qWarning("!! profile '%s' 里没有 lamp 热区", qPrintable(p.name));
        return;
    }

    m_iconRel = p.wsIcon;
    m_triRel  = p.cardTri;

    setCardRect(h->r);
    layoutPopup();
    update();
}

void WorkspotCard::setIndex(int idx)
{
    if (idx < 0 || idx > 2 || m_index == idx) return;
    m_index = idx;
    update();
    emit indexChanged(idx);
}

void WorkspotCard::syncPopupCurrent()
{
    m_popup->setCurrent(m_index);
}

void WorkspotCard::paintContent(QPainter &p)
{
    const QPixmap &ic = m_iconSet[m_index];
    if (!ic.isNull())
        p.drawPixmap(m_iconRel, ic);
}

// =========================== 底图卡片的纯下拉 ===============================
// Focal Spot / Target exposure index / Exposure parameters 这三张卡片在底图
// PNG 里已经画好了，所以这里只占命中区：关闭态不绘制，展开态才铺压暗底色 +
// 把抠出来的内容层叠回去（设计稿：卡片压暗但图标/文字不变）。
ChoiceCard::ChoiceCard(QWidget *cardParent, const QString &spotId,
                       const QString &contentRes, const QString &contentResLand)
    : DropCardBase(cardParent)
    , m_spotId(spotId)
{
    m_res[0] = contentRes;
    m_res[1] = contentResLand;

    setPaintCard(false);
    // 卡片底色 / 圆角 / 右下角三角都由底图（关闭态）或内容层（展开态）提供，
    // 不再整卡填充：整卡填充会把底图上的 EI_T 标签和三角一起抹掉。
    setFillBg(false);
    m_tri = QPixmap();          // 三角在底图/内容层里已经有了，基类不要重复画

    connect(m_popup, &DropPopup::picked, this, &ChoiceCard::setIndex);

    applyProfile(dr::portraitProfile());
}

void ChoiceCard::applyProfile(const dr::LayoutProfile &p)
{
    const dr::Hotspot *h = p.spot(m_spotId);
    if (!h) {
        qWarning("!! profile '%s' 里没有热区 '%s'",
                 qPrintable(p.name), qPrintable(m_spotId));
        return;
    }

    const bool land = (p.name == QLatin1String("landscape"));
    m_land = land ? 1 : 0;
    m_content.load(m_res[land ? 1 : 0]);
    if (m_content.isNull())
        qWarning("!! 卡片内容层加载失败: %s", qPrintable(m_res[land ? 1 : 0]));
    else if (m_content.size() != h->r.size())
        qWarning("!! 内容层 %s 尺寸 %dx%d 与热区 %s 的 %dx%d 不匹配",
                 qPrintable(m_res[land ? 1 : 0]),
                 m_content.width(), m_content.height(),
                 qPrintable(m_spotId), h->r.width(), h->r.height());

    setCardRect(h->r);
    layoutPopup();
    update();
}

void ChoiceCard::setIndex(int idx)
{
    if (idx < 0 || idx >= m_popup->count())
        return;

    // 注意：即使选的是「当前这一项」也要重画卡片。
    // 底图烘焙的值和弹层里的当前项不一定是同一个数（EL 卡片烘焙 320、默认项是 50），
    // 这里若提前 return，用户点了 50 卡片还是 320，看起来就是「没同步」。
    const bool changed = (m_index != idx);
    m_index = idx;

    if (!m_iconRes.isEmpty() || m_valPx > 0) {
        m_valText = m_cardLabels.value(idx, m_popup->itemText(idx));
        setForceDraw(true);
        update();
    }
    if (changed)
        emit indexChanged(idx, m_popup->itemText(idx));
}

void ChoiceCard::setCardIconItems(const QStringList &resPaths,
                                  const QRect &boxPortrait, const QRect &boxLand)
{
    m_iconRes  = resPaths;
    m_iconBox[0] = boxPortrait;
    m_iconBox[1] = boxLand;
    // 盖掉的范围比图标框大 3px：图标边缘有抗锯齿，留 1px 会看到一圈残影
    m_cover[0] = boxPortrait.adjusted(-3, -3, 3, 3);
    m_cover[1] = boxLand.adjusted(-3, -3, 3, 3);
    m_icons.clear();
    m_icons.resize(resPaths.size());
}

void ChoiceCard::setCardValueText(int px, qreal track, const QColor &c,
                                  int cxPortrait, int topPortrait,
                                  int cxLand, int topLand,
                                  const QRect &coverPortrait, const QRect &coverLand,
                                  QFont::Weight w)
{
    m_valPx       = px;
    m_valTrack    = track;
    m_valWeight   = w;
    m_valColor    = c;
    m_valCx[0]    = cxPortrait;
    m_valCx[1]    = cxLand;
    m_valTop[0]   = topPortrait;
    m_valTop[1]   = topLand;
    m_cover[0]    = coverPortrait;
    m_cover[1]    = coverLand;
}

QPixmap ChoiceCard::iconAt(int idx)
{
    if (idx < 0 || idx >= m_iconRes.size())
        return QPixmap();
    if (m_icons[idx].isNull())
        m_icons[idx] = QPixmap(m_iconRes[idx]);
    return m_icons[idx];
}


QString ChoiceCard::text() const
{
    return m_popup->itemText(m_index);
}

void ChoiceCard::paintContent(QPainter &p)
{
    // 展开态：先把「压暗后的卡片」内容层叠回去（底图改不了，只能盖）
    if (m_popupOpen && !m_content.isNull())
        p.drawPixmap(0, 0, m_content);

    if (!m_forceDraw)
        return;                     // 还没选过：保持底图烘焙的样子

    // 把烘焙的图标/数值盖掉，换成当前选中的
    p.fillRect(m_cover[m_land], m_popupOpen ? kCardBgActive : kCardBg);

    if (!m_iconRes.isEmpty()) {
        const QPixmap ic = iconAt(m_index);
        if (!ic.isNull())
            p.drawPixmap(m_iconBox[m_land], ic);
        return;
    }
    if (m_valPx > 0 && !m_valText.isEmpty()) {
        QFont f = makeFontW(m_valPx, m_valWeight);
        if (m_valTrack != 0.0)
            f.setLetterSpacing(QFont::AbsoluteSpacing, m_valTrack);
        QFontMetricsF fm(f);
        const QRectF br = fm.tightBoundingRect(m_valText);
        // tightBoundingRect 不含 letterSpacing 的额外增量，水平居中要自己补上
        const qreal extra = (m_valText.length() - 1) * m_valTrack;
        p.setFont(f);
        p.setPen(m_valColor);
        p.drawText(QPointF(qRound(m_valCx[m_land] - (br.width() + extra) / 2.0 - br.left()),
                           qRound(m_valTop[m_land] - br.top())), m_valText);
    }
}

// ============================ 常亮开关卡片 ==================================
//  关闭态什么都不画（底图里的常态图形原样透出），点亮态铺一张整卡图。
//  整卡图是从设计稿按卡片矩形原样切下来的，四角是页面底色 #171717，
//  所以圆角处不会露出一圈硬边。
ToggleCard::ToggleCard(QWidget *page, const QString &spotId,
                       const QString &onRes, const QString &onResLand)
    : QToolButton(page)
    , m_spotId(spotId)
{
    m_res[0] = onRes;
    m_res[1] = onResLand;

    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:0;}"));
    // 换态必须重绘：底图和点亮图是两套，状态没变时 setChecked 不会发信号
    connect(this, &QToolButton::toggled, this, [this](bool) { update(); });

    applyProfile(dr::portraitProfile());
}

void ToggleCard::applyProfile(const dr::LayoutProfile &p)
{
    const dr::Hotspot *h = p.spot(m_spotId);
    if (!h) {
        qWarning("!! profile '%s' 里没有热区 '%s'",
                 qPrintable(p.name), qPrintable(m_spotId));
        return;
    }

    const bool land = (p.name == QLatin1String("landscape"));
    m_land = land ? 1 : 0;
    m_on.load(m_res[m_land]);
    if (m_on.isNull())
        qWarning("!! 点亮态图加载失败: %s", qPrintable(m_res[m_land]));
    else if (m_on.size() != h->r.size())
        qWarning("!! 点亮态图 %s 尺寸 %dx%d 与热区 %s 的 %dx%d 不匹配",
                 qPrintable(m_res[m_land]),
                 m_on.width(), m_on.height(),
                 qPrintable(m_spotId), h->r.width(), h->r.height());

    setGeometry(h->r);
    update();
}

void ToggleCard::paintEvent(QPaintEvent *)
{
    if (!isChecked() || m_on.isNull())
        return;                     // 关闭态：让底图原样透出

    QPainter p(this);
    p.drawPixmap(0, 0, m_on);
}

// ============================== 参数步进器 ==================================
//  两种状态（使能/置灰）都是量测值，全是中性灰，直接取一个通道当灰度用：
//    使能  填充 33  外框 111/87  分隔线 87  ± 232  单位 140  值 211
//    置灰  填充 28  外框  67/55  分隔线 55  ± 127  单位  81  值 116
static const QColor kStFillOn  (33, 33, 33);
static const QColor kStFillOff (28, 28, 28);
static const QColor kStSymOn   (232, 232, 232);
static const QColor kStSymOff  (127, 127, 127);
static const QColor kStUnitOn  (140, 140, 140);
static const QColor kStUnitOff (81, 81, 81);
static const QColor kStValOn   (211, 211, 211);
static const QColor kStValOff  (116, 116, 116);

// 卡内固定量（两种朝向完全一样，只取值格宽度不同）
static const int kStValInkLeft  = 48;    // 数值墨迹左端 = 值格左 + 48
static const int kStBaseline    = 62;    // 数值基线 = 卡顶 + 62
static const int kStUnitInkPad  = 24;    // 单位墨迹右端 = 值格右 − 24
static const int kStFontPx      = 40;    // 数值 Ubuntu **Bold** 40px（墨迹 43x28，墨量对得上）
static const int kStUnitPx      = 40;    // 单位 Regular 40px（量测「kV」墨迹 44x30）

// 按「基线 + 墨迹左端」对齐画字：数值里可能只有「–」（DEN 关态），
// 这时墨迹顶不在数字的顶线上，用 ink-top 对齐会跑偏，所以统一用基线。
static void drawBaselineText(QPainter &p, int inkLeft, int baseline,
                             const QString &s, const QFont &f, const QColor &c)
{
    QFontMetricsF fm(f);
    const QRectF br = fm.tightBoundingRect(s);
    p.setFont(f);
    p.setPen(c);
    p.drawText(QPointF(qRound(inkLeft - br.left()), baseline), s);
}

StepperCard::StepperCard(QWidget *page, const QString &spotId, const QString &unit,
                         double lo, double hi, double step, int decimals, double init)
    : QToolButton(page)
    , m_spotId(spotId)
    , m_unit(unit)
    , m_lo(lo), m_hi(hi), m_step(step), m_value(init)
    , m_decimals(decimals)
{
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:0;}"));

    // 长按连发：按住 300ms 后每 150ms 走一格
    //   150ms 来自抓包实测（触控屏长按 130~180ms，多数 ~150ms）。
    //   起步留 300ms 而不是跟连发一样快，是为了「点一下」不被判成长按。
    m_delay  = new QTimer(this);
    m_repeat = new QTimer(this);
    m_delay->setSingleShot(true);
    m_delay->setInterval(300);
    m_repeat->setInterval(150);
    connect(m_delay, &QTimer::timeout, this, [this] {
        if (m_repeatDir) m_repeat->start();
    });
    connect(m_repeat, &QTimer::timeout, this, [this] {
        if (m_repeatDir) applyStep(m_repeatDir);
    });

    applyProfile(dr::portraitProfile());

    // 初始使能态直接取版式表：竖屏设计稿里 kV/mA 亮、mAs/ms/DEN 灰
    // （DEN 之后由 AEC 开关接管，这里只是开机第一帧）
    if (const dr::StepperSpot *s = dr::portraitProfile().stepper(m_spotId))
        m_on = s->enabled;
}

void StepperCard::applyProfile(const dr::LayoutProfile &p)
{
    const dr::StepperSpot *s = p.stepper(m_spotId);
    if (!s) {
        qWarning("!! profile '%s' 里没有步进器 '%s'",
                 qPrintable(p.name), qPrintable(m_spotId));
        return;
    }

    const bool land = (p.name == QLatin1String("landscape"));
    m_land = land ? 1 : 0;

    static const char *kRes[4] = {
        ":/mselect/st_chrome_on.png",       ":/mselect/st_chrome_off.png",
        ":/mselect/st_chrome_on_land.png",  ":/mselect/st_chrome_off_land.png"
    };
    for (int i = 0; i < 2; ++i) {
        const int idx = m_land * 2 + i;
        if (!m_chrome[idx].isNull())
            continue;
        m_chrome[idx].load(QLatin1String(kRes[idx]));
        if (m_chrome[idx].isNull()) {
            qWarning("!! 步进器底纹加载失败: %s", kRes[idx]);
        } else if (m_chrome[idx].size() != s->outer.size()) {
            qWarning("!! 步进器底纹 %s 尺寸 %dx%d 与热区 %s 的 %dx%d 不匹配",
                     kRes[idx], m_chrome[idx].width(), m_chrome[idx].height(),
                     qPrintable(m_spotId), s->outer.width(), s->outer.height());
        }
    }

    m_dec = s->dec.translated(-s->outer.topLeft());
    m_val = s->val.translated(-s->outer.topLeft());
    m_inc = s->inc.translated(-s->outer.topLeft());

    setGeometry(s->outer);
    update();
}

void StepperCard::setValue(double v, bool snapGrid)
{
    // 夹范围 → 对齐步长网格 → 按小数位收敛（不然 0.1 累加会出 0.30000000000000004）
    v = qBound(m_lo, v, m_hi);
    if (snapGrid) {
        const double grid = m_lo + qRound((v - m_lo) / m_step) * m_step;
        v = qBound(m_lo, grid, m_hi);
    }
    const double p10 = std::pow(10.0, m_decimals);
    v = qRound(v * p10) / p10;

    if (qAbs(v - m_value) < 1e-9)
        return;
    m_value = v;
    m_limitDir = 0;          // 值真的动了 → 之前判的「到底」作废，两侧重新点亮
    update();
    emit valueChanged(m_value);
}

void StepperCard::setOn(bool on)
{
    if (m_on == on)
        return;
    m_on = on;
    if (!on)
        stopRepeat();
    update();
    emit onChanged(on);
}

// 边界判断用「离 lo/hi 不足半格」——浮点累加后 75.999999 也要认成 76
void StepperCard::applyStep(int dir)
{
    if (!m_on)
        return;

    // ---- 远程驱动：只发意图，数值等主控广播回来 ----------------------
    //  不能在这里做 m_value ± step 的预测：mA/ms/mAs 的档位是主控按机型参数表
    //  跳的（实测 180→200→220→250…），本地 +5 只会先跳到一个错值。
    if (m_remote) {
        if (m_limitDir == dir) {         // 这个方向已经被判到底了
            stopRepeat();
            return;
        }
        emit stepped(dir, m_value);
        return;
    }

    if (dir < 0 && m_value <= m_lo + 1e-9) {
        stopRepeat();
        return;
    }
    if (dir > 0 && m_value >= m_hi - 1e-9) {
        stopRepeat();
        return;
    }
    const double before = m_value;
    setValue(m_value + dir * m_step);
    if (qAbs(m_value - before) > 1e-9)
        emit stepped(dir, m_value);      // 本地模式：值真变了才发（撞边界不发）
    if ((dir < 0 && m_value <= m_lo + 1e-9) || (dir > 0 && m_value >= m_hi - 1e-9))
        stopRepeat();                    // 到顶就把连发停掉，省得一直空转
}

// 主控没反应 → 该侧到底。只有远程驱动才用得上：
// 本地模式的两个边界在 applyStep 里用 m_lo/m_hi 就判完了。
void StepperCard::noteLimit(int dir)
{
    if (!m_remote)
        return;
    m_limitDir = dir > 0 ? 1 : -1;
    stopRepeat();
    update();
}

void StepperCard::stopRepeat()
{
    m_repeatDir = 0;
    if (m_delay)  m_delay->stop();
    if (m_repeat) m_repeat->stop();
}

// − 是 36x6 一条；+ 是 48x6 横条 + 6x48 竖条，都居中在格子正中
void StepperCard::paintSymbol(QPainter &p, const QRect &cell, bool plus, const QColor &c)
{
    const int cx = (cell.left() + cell.right() + 1) / 2;
    const int cy = (cell.top()  + cell.bottom() + 1) / 2;
    if (!plus) {
        p.fillRect(QRect(cx - 18, cy - 3, 36, 6), c);
    } else {
        p.fillRect(QRect(cx - 24, cy - 3, 48, 6), c);
        p.fillRect(QRect(cx - 3, cy - 24, 6, 48), c);
    }
}

QString StepperCard::valueText() const
{
    if (!m_on && m_dashOff)
        return QString::fromUtf8("–");     // U+2013，和设计稿同一个破折号
    if (m_decimals <= 0)
        return QString::number(qRound(m_value));
    QString s = QString::number(m_value, 'f', m_decimals);
    if (s.endsWith(QLatin1String(".0")))
        s.chop(2);                         // 设计稿写「5」不写「5.0」
    return s;
}

void StepperCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    // 符号是整像素矩形，关掉抗锯齿才不会发糊
    p.setRenderHint(QPainter::Antialiasing, false);

    const QPixmap &chrome = m_chrome[m_land * 2 + (m_on ? 0 : 1)];
    if (!chrome.isNull())
        p.drawPixmap(0, 0, chrome);
    else
        p.fillRect(rect(), m_on ? kStFillOn : kStFillOff);

    // 到边界只把「那一侧」变灰，另一侧还是亮的。
    // 远程驱动时 m_value 是在 [lo,hi] 中间的正常值，判断不出边界，
    // 于是靠 noteLimit() 记下的「主控没反应的方向」来变灰（抓包：到底时主控静默忽略）。
    const bool decOk = m_on && (m_value > m_lo + 1e-9) && m_limitDir != -1;
    const bool incOk = m_on && (m_value < m_hi - 1e-9) && m_limitDir != +1;
    paintSymbol(p, m_dec, false, decOk ? kStSymOn : kStSymOff);
    paintSymbol(p, m_inc, true,  incOk ? kStSymOn : kStSymOff);

    const QFont f = makeFontW(kStFontPx, QFont::Bold);
    const QFont fu = makeFont(kStUnitPx);
    const QColor unitC = m_on ? kStUnitOn : kStUnitOff;
    const QColor valC  = m_on ? kStValOn  : kStValOff;

    // 单位：墨迹右端贴着值格右 − 24；基线比数值低 1px（设计稿里单位墨迹底正好低 1）
    {
        QFontMetricsF fm(fu);
        const qreal w = fm.tightBoundingRect(m_unit).width();
        drawBaselineText(p, qRound(m_val.right() - kStUnitInkPad - w + 1),
                         kStBaseline + 1, m_unit, fu, unitC);
    }

    // 数值：墨迹左端 = 值格左 + 48
    drawBaselineText(p, m_val.left() + kStValInkLeft, kStBaseline,
                     valueText(), f, valC);
}

void StepperCard::mousePressEvent(QMouseEvent *e)
{
    e->accept();
    if (!m_on)
        return;                          // 置灰时点了没反应

    const QPoint pt = e->pos();
    int dir = 0;
    if (m_dec.contains(pt))      dir = -1;
    else if (m_inc.contains(pt)) dir = +1;
    if (!dir)
        return;                          // 点在数值区：暂不处理（以后弹数字键盘）

    applyStep(dir);                      // 先走一格，按下就有反馈（远程模式下=发第一帧意图）
    m_repeatDir = dir;
    m_delay->start();                    // 300ms 后才开始连发
}

void StepperCard::mouseReleaseEvent(QMouseEvent *e)
{
    e->accept();
    stopRepeat();
}

void StepperCard::leaveEvent(QEvent *e)
{
    stopRepeat();                        // 手指滑出格子就停，别一直加
    QToolButton::leaveEvent(e);
}

// ============================== AEC 场域弹层 ================================
AecFieldPopup::AecFieldPopup(QWidget *parent)
    : DropPopup(parent)
{
    // 不 addItem 任何行：这一层壳只负责「面板底/描边/圆角 + 标题 + hideEvent→closed」，
    // 行列表那套（indexAt / 高亮 / 滚动条）在这里全部空转。
}

void AecFieldPopup::setFigureLayout(const QRect &frame,
                                    const QRect &left, const QRect &right,
                                    const QRect &bottom)
{
    m_frame    = frame;
    m_field[0] = left;
    m_field[1] = right;
    m_field[2] = bottom;
    update();
}

void AecFieldPopup::setZones(const QRect &left, const QRect &right, const QRect &bottom)
{
    m_zone[0] = left;
    m_zone[1] = right;
    m_zone[2] = bottom;
}

int AecFieldPopup::zoneAt(const QPoint &pt) const
{
    for (int i = 0; i < 3; ++i)
        if (m_zone[i].contains(pt))
            return i;
    return -1;
}

void AecFieldPopup::paintEvent(QPaintEvent *e)
{
    DropPopup::paintEvent(e);       // 面板底 / 1px 描边 / 圆角 / 标题

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 触摸屏没有真正的 hover，这里只给一点点按下反馈（设计稿没有这个状态）
    if (m_hoverZone >= 0) {
        p.fillRect(m_zone[m_hoverZone].intersected(m_frame),
                   QColor(255, 255, 255, 12));
    }

    paintAecFigure(p, QRectF(m_frame), m_mask, kPopBg);
}

void AecFieldPopup::mouseMoveEvent(QMouseEvent *e)
{
    const int z = zoneAt(e->pos());
    if (z != m_hoverZone) { m_hoverZone = z; update(); }
    e->accept();
}

void AecFieldPopup::leaveEvent(QEvent *e)
{
    if (m_hoverZone != -1) { m_hoverZone = -1; update(); }
    QWidget::leaveEvent(e);
}

void AecFieldPopup::mousePressEvent(QMouseEvent *e)
{
    // 多选：点一下只切换该场域，**不关弹层**（关 = 点空白 / 再点卡片）。
    // 基类在这里是「选中即 close」，所以必须整个覆盖掉。
    const int z = zoneAt(e->pos());
    if (z >= 0) {
        m_mask ^= (1 << z);
        update();
        emit toggled(z);
    }
    e->accept();
}

void AecFieldPopup::mouseReleaseEvent(QMouseEvent *e)
{
    e->accept();
}

// ============================== AEC 场域卡片 ================================
AecFieldCard::AecFieldCard(QWidget *cardParent)
    : DropCardBase(cardParent)
{
    // 关闭态也要画：底图烘焙的小图是「左野实心」，而选定的是「右野实心」，
    // 所以必须把那一小块盖掉重画；但整卡底色仍让底图透出（fillBg=false）。
    setPaintCard(true);
    setFillBg(false);

    m_aec = new AecFieldPopup(cardParent ? cardParent : this);
    replacePopup(m_aec);

    connect(m_aec, &AecFieldPopup::toggled, this, [this](int idx) {
        setMask(m_mask ^ (1 << idx));
    });
}

void AecFieldCard::applyProfile(const dr::LayoutProfile &p)
{
    const dr::Hotspot *h = p.spot(QStringLiteral("view"));
    if (!h) {
        qWarning("!! profile '%s' 里没有热区 'view'", qPrintable(p.name));
        return;
    }
    const bool land = (p.name == QLatin1String("landscape"));

    setCardRect(h->r);
    m_triRel  = QRect(width() - 32, height() - 32, 32, 32);
    // 小图 58x58 水平居中、距卡顶 35（两种朝向实测都是 35）
    m_iconBox = QRect((width() - 58) / 2, 35, 58, 58);

    // 弹层锚定：左沿 = 卡片左沿、顶边 = 卡片底 + 5。
    // 基类给的是 (card.x-1, card.bottom+3)，两种朝向各差 1~2px，这里补掉。
    m_popOff = land ? QPoint(1, 0) : QPoint(0, 1);

    m_aec->setPanelSize(QSize(582, 561));
    m_aec->setHeader(QStringLiteral("Automatic exposure control"),
                     42, -0.3, QPoint(28, 36));
    // 人体图 abs：竖屏 (526,985) → 弹层内 (108,137)；横屏 (1321,517) → (108,138)
    m_aec->setFigureLayout(land ? QRect(108, 138, 367, 367)
                                : QRect(108, 137, 367, 367),
                           QRect( 58,  83, 75, 100),    // 左野
                           QRect(233,  83, 75, 100),    // 右野
                           QRect(146, 208, 75, 100));   // 下野
    // 热区放大到区块：上排左右各半 + 下带整宽（人体图内沿 132..450 / 161..479）
    m_aec->setZones(QRect(132, 161, 159, 184),
                    QRect(291, 161, 159, 184),
                    QRect(132, 345, 318, 134));
    m_aec->setMask(m_mask);

    layoutPopup();
    update();
}

void AecFieldCard::setMask(int mask)
{
    mask &= 0x7;
    if (m_aec)
        m_aec->setMask(mask);
    if (m_mask == mask) {
        update();
        return;
    }
    m_mask = mask;
    update();
    emit maskChanged(mask);
}

void AecFieldCard::syncPopupCurrent()
{
    if (m_aec)
        m_aec->setMask(m_mask);
}

void AecFieldCard::paintContent(QPainter &p)
{
    // 两种状态都必须清底：
    //   关闭态 —— 盖掉底图里烘焙的原始小图；
    //   展开态 —— 盖掉上一次绘制留下的场域实心块。少了这一步，从「选中」
    //             切成「未选中」时线框描边盖不住中间的旧墨迹，看着就是
    //             「AEC 人体场域图标边缘残缺」。
    const QColor bg = m_popupOpen ? kCardBgActive : kCardBg;
    p.fillRect(m_iconBox.adjusted(-3, -3, 3, 3), bg);
    paintAecFigure(p, QRectF(m_iconBox), m_mask, bg);
}

// ===========================================================================
//  PatientInfoPanel —— 顶部患者信息面板
// ===========================================================================
//  面板几何两种朝向一致：y=112 起、高 312、整宽（1200 / 1920）。
//  关闭态 hide()，既不绘制也不吃事件 —— thumb 卡片 (24,136,240,328) 的下半截
//  露在面板底边（424..463）之外，必须保持可点。
static const int kPatPanelH = 312;

PatientInfoPanel::PatientInfoPanel(QWidget *pageParent)
    : QWidget(pageParent)
{
    setObjectName(QStringLiteral("patInfoPanel"));
    m_res[0] = QStringLiteral(":/mselect/pat_panel.png");
    m_res[1] = QStringLiteral(":/mselect/pat_panel_land.png");
    hide();
}

void PatientInfoPanel::applyProfile(const dr::LayoutProfile &p)
{
    m_land = (p.name == QLatin1String("landscape")) ? 1 : 0;
    m_img[m_land].load(m_res[m_land]);
    if (m_img[m_land].isNull())
        qDebug("patient info: 面板图加载失败 %s", qPrintable(m_res[m_land]));

    m_rect = QRect(0, p.header.bottom() + 1, p.design.width(), kPatPanelH);
    setGeometry(m_rect);
    if (m_open)
        raise();
    update();
}

void PatientInfoPanel::openPanel()
{
    if (m_open)
        return;
    m_open = true;
    if (QWidget *page = parentWidget()) {
        // 页面上已经有展开的下拉弹层时先收掉，避免两个层同时挂着；
        // 同时给每张卡片装过滤器 —— 面板开着的时候点任何卡片都先关面板，
        // 再把这次按下放行给卡片自己（见 eventFilter）。
        const QList<DropCardBase *> cards = page->findChildren<DropCardBase *>();
        for (DropCardBase *c : cards) {
            c->closePopup();
            c->installEventFilter(this);
        }
        page->installEventFilter(this);
    }
    raise();                 // 盖住 thumb / bodyPart 等先建的同级卡片
    show();
    emit opened();
}

void PatientInfoPanel::closePanel()
{
    if (!m_open)
        return;
    m_open = false;
    if (QWidget *page = parentWidget()) {
        page->removeEventFilter(this);
        const QList<DropCardBase *> cards = page->findChildren<DropCardBase *>();
        for (DropCardBase *c : cards)
            c->removeEventFilter(this);
    }
    hide();
    emit closed();
}

void PatientInfoPanel::togglePanel()
{
    if (m_open)
        closePanel();
    else
        openPanel();
}

//  面板外（冒泡到页面的）鼠标按下就关。顶栏热区是独立控件、也没装过滤器，
//  所以「点热区」不会走到这里，由热区自己的 clicked 做 toggle。
//  注意：点到卡片时只能先关面板、不能吃掉事件（closePopup 之后卡片还要正常响应）。
bool PatientInfoPanel::eventFilter(QObject *obj, QEvent *e)
{
    if (m_open && e->type() == QEvent::MouseButtonPress) {
        if (obj == parentWidget())
            closePanel();
        else if (qobject_cast<DropCardBase *>(obj))
            closePanel();
    }
    return QWidget::eventFilter(obj, e);
}

void PatientInfoPanel::paintEvent(QPaintEvent *)
{
    QPainter g(this);
    const QPixmap &img = m_img[m_land];
    if (img.isNull())
        g.fillRect(rect(), QColor(0x34, 0x34, 0x34));
    else
        g.drawPixmap(rect(), img);
}

// 面板内点击一律吃掉：QWidget 默认实现在未处理时 ignore，事件会冒泡到页面，
// 立刻触发「点空白关闭」——面板里点哪儿都被关掉。
void PatientInfoPanel::mousePressEvent(QMouseEvent *e)
{
    e->accept();
}

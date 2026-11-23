// ============================================================================
//  首页的「下拉卡片」+ 通用下拉弹层
// ----------------------------------------------------------------------------
//  1) DropPopup    —— 通用下拉弹层（标题 + N 行「图标 + 文字」）
//                     所有弹层共用一套视觉：圆角 4、描边 #595959、底 #212121、
//                     整行高亮 #4D4D4D、行高 112、首行顶 97、标题墨迹 y=39。
//                     可变的部分全部由 setter 给（尺寸、图标框、行字号/墨迹、
//                     滚动条），因为设计稿里一共 7 种弹层、8 套参数：
//
//                       弹层                尺寸      行  图标        备注
//                       Patient type     514x450   3   64x64@(25,24)
//                       Workspot         514x450   3   64x64@(25,24)
//                       Focal Spot       508x338   2   59x59@(28,27) checkbox
//                       Target exp.index 508x658   5   —           右侧滚动条
//                       Exposure params  508x450   3   —
//                       AEC 场域         582x561   —   —           人体图（见下）
//                       （横屏另有 6 套）
//                     第 7 种（AEC 场域）结构完全不同，是 AecFieldPopup，
//                     只是继承 DropPopup 借它的面板底/描边/标题绘制。
//
//  2) PatientCard  —— 「L / M / S」体型卡片（图标随值换）
//  3) WorkspotCard —— 「Wallstand / Table / Free detector」站位卡片
//  4) ChoiceCard   —— 底图上已有图形的卡片（Focal Spot / Target EI /
//                     Exposure params）的下拉，只做命中区 + 弹层，自己不绘制
//
//  行文字的绘制用「原点对齐」：同一弹层里所有行共用 probe 的字形算绘制原点，
//  各行的墨迹差异来自字符自带的边距（设计稿就是这样 —— 例如 Large 比
//  Small 右上 2px，因为 L 的左边距比 S 小）。
//
//  ⚠️ 与早期探针 demo.cpp 的唯一结构性差别：
//   demo 里弹层是 Qt::Popup 独立顶层窗口；本工程整页被 QGraphicsView
//   rotate(270) + fitInView 缩放，独立顶层窗口不参与这个变换（会跑到错误的
//   位置、而且不跟随旋转），所以这里把弹层做成 **页面内的子控件**，
//   由父页面统一绘制/变换。
//   前提：弹层矩形完全落在页面内（设计稿是 287,267 514x450 ⊂ 1200x1920，满足）。
//
//  点击弹层以外的地方关闭：卡片在父页面上装了一个事件过滤器，
//  凡是冒泡到页面的鼠标按下（说明没被子控件吃掉）就关掉弹层。
// ============================================================================
#ifndef PATIENTCARD_H
#define PATIENTCARD_H

#include <QWidget>
#include <QToolButton>
#include <QPixmap>
#include <QFont>
#include <QList>
#include <QRect>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QColor>

class QTimer;

namespace dr { struct LayoutProfile; }

// ---------------------------------------------------------------------------
//  通用下拉弹层
// ---------------------------------------------------------------------------
class DropPopup : public QWidget
{
    Q_OBJECT
public:
    explicit DropPopup(QWidget *parent = nullptr);

    // 弹层尺寸。默认 514x450（Patient type / Workspot 那套）。
    // 行数变了高度就要跟着改：height = 首行顶 97 + 行高 112 * 行数 + 17（底边距）。
    void setPanelSize(const QSize &sz);

    // 标题：文字 / 字号 / 字距 / 墨迹左上角（弹层内相对坐标）
    void setHeader(const QString &text, int px, qreal track, const QPoint &inkTopLeft);

    // 行文字样式：字号 / 墨迹锚点（弹层内相对，y 相对行顶）/ 对齐探针。
    //   probe 非空 = 「原点对齐」：所有行都用 probe 的字形算绘制原点，
    //                各行的左边界差异来自字符自带的左边距（设计稿就是这样）；
    //   probe 为空 = 「墨迹对齐」：每行按自己的墨迹左边界对齐到同一 x。
    void setRowStyle(int px, const QPoint &inkTopLeft, const QString &probe);

    // 行几何：首行顶 / 行高。默认 97 / 112（所有已知弹层都一样，一般不用改）
    void setRowMetrics(int firstRowTop, int rowHeight);

    // 行内图标框（弹层内相对坐标；y 相对行顶）。QRect() = 这个弹层没有图标。
    void setIconBox(const QRect &r);

    // 列表区右边界（不含）。整行高亮画到 listRight-1。
    // <=0 表示用 width()-2。有滚动条时要设成轨道左边界，否则高亮会盖住轨道。
    void setListRight(int x);

    // 滚动条：轨道矩形 / 滑块矩形（都是弹层内相对坐标）。QRect() = 没有滚动条。
    // 轨道色 #171717，滑块色 #767676、圆角取滑块宽的一半（胶囊形）。
    void setScrollbar(const QRect &track, const QRect &thumb);

    // iconPath 传空串 = 该行没有图标（Target EI / Exposure params 这种纯文字列）
    void addItem(const QString &text, const QString &iconPath);

    // ---- 滚动（行数比设计稿多时用）-----------------------------------------
    //  设计稿的弹层高度是按「固定几行」量出来的（EL 5 行 = 658），行数一变就
    //  要么跑出屏幕、要么把行画到面板圆角外。档位改由 cards.json 配置后行数会变，
    //  所以这里补真实滚动：面板高度不变，只显示前 n 行，其余靠
    //  滑滑块 / 点轨道 / 拖列表 / 滚轮翻。
    //  n = 0 ⇒ 不滚（所有行都画，高度由调用方按行数给）—— 焦点卡、曝光参数卡走这条，
    //  行为与改动前完全一致（含设计稿那条 156px 的固定滑块）。
    void setVisibleRows(int n);
    int  visibleRows() const { return m_visibleRows; }
    bool scrolling() const;

    // 按行文字查下标。配置驱动之后「换档位」不能再用一份写死的数组，
    // setVar(el=…) / dbg.sh 都走这里，保证与弹层里真正显示的一致。
    int  indexOfText(const QString &t) const;

    void setCurrent(int idx);
    void setHoverIndex(int idx);
    int  current() const { return m_current; }
    int  count() const { return m_items.size(); }
    QString itemText(int idx) const
    { return (idx >= 0 && idx < m_items.size()) ? m_items[idx].text : QString(); }

signals:
    void picked(int index);
    void closed();

protected:
    void hideEvent(QHideEvent *e) override;
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    int indexAt(const QPoint &pt) const;

    // 滚动的内部实现
    //  ⚠ 滚动状态的真身是 m_scrollPx（像素，qreal），m_scroll 只是它的
    //     整数派生值（floor(px/rowH)），给键盘 / ensureVisible / 日志用。
    //     原因：按行取整会让手指划动「一格一格跳」（用户原话：瞬间翻页），
    //     触屏要的是像素级跟手 + 松手惯性（2026-11-16）。
    int   maxScroll() const;
    qreal maxScrollPx() const;
    void  setScroll(int s);        // 行级（键盘 / 兼容旧调用），内部转像素
    // elastic=true：允许越界（阻尼拉伸），给「内容不足一屏」的弹层做
    // 橡皮筋手感；松手后 springBack() 弹回。
    void  setScrollPx(qreal px, bool elastic = false);
    void  animateTo(qreal target); // 平滑滚到某像素位置（点轨道翻页 / 吸附）
    void  springBack();            // 越界回弹（不可滚弹层的划动反馈）
    void  startAnim();
    void  stopAnim();
    void  onAnimTick();            // 16ms 定时器：惯性衰减 / 趋向目标 / 回弹
    void  snapToRow();             // 惯性停下后平滑对齐到整行
    void  ensureVisible(int idx, bool animate = true);
    QRect thumbRect() const;       // 滚动中按内容算滑块；不滚动时就是设计稿那个
    void  commitPending();         // 把「待确认」那一行真正提交（picked）

    struct Item { QString text; QPixmap icon; };
    QList<Item> m_items;
    int m_current = 0;
    int m_hover   = 0;

    int m_visibleRows = 0;         // 0 = 不滚
    int m_scroll      = 0;         // 首个可见行的下标（= m_scrollPx / rowH 的派生值）
    qreal m_scrollPx  = 0.0;       // ★ 真正的内容偏移（像素），丝滑滚动的唯一状态
    int m_pending     = -1;        // 手指划出来的「待确认行」：只高亮，关弹层时才提交
    bool m_dragThumb  = false;     // 正在拖滑块
    bool m_dragList   = false;     // 正在拖列表（触屏滑动）
    bool m_dragMoved  = false;     // 位移超过阈值 ⇒ 算滑动，松手不选中
    int  m_pressIdx   = -1;
    QPoint m_dragPos0;             // 按下点
    int   m_dragScroll0 = 0;       // 按下时的 m_scroll
    qreal m_dragScroll0Px = 0.0;   // 按下时的 m_scrollPx（像素级跟手用）

    // 惯性 / 平滑动画（一个 16ms 定时器同时管「松手滑行」和「对齐整行」）
    QTimer *m_anim      = nullptr;
    int     m_animMode  = 0;       // 0=停 1=惯性 2=趋向目标
    qreal   m_vel       = 0.0;     // 惯性速度，单位 px/ms（正 = 往后翻）
    qreal   m_targetPx  = 0.0;     // m_animMode==2 时的目标像素位置
    qint64  m_velLastMs = 0;       // 上一次 move 的时间戳（算瞬时速度）
    qint64  m_tickLastMs = 0;      // 上一次 tick 的时间戳（算 dt）

    QSize   m_size;
    int     m_rowTop  = 97;
    int     m_rowH    = 112;
    int     m_listRight = 0;
    QRect   m_iconBox;
    QRect   m_sbTrack;
    QRect   m_sbThumb;

    QString m_headerText;
    QFont   m_headerFont;
    qreal   m_headerTrack = 0.0;
    QPoint  m_hdrInk;

    QFont   m_rowFont;
    QPoint  m_rowInk;
    QString m_rowProbe;
};

// ---------------------------------------------------------------------------
//  下拉卡片基类：底色 + 右下角三角 + 弹层开合 + 点空白关闭
// ---------------------------------------------------------------------------
class DropCardBase : public QToolButton
{
    Q_OBJECT
public:
    // cardParent = 卡片所在页面（弹层也建在它上面，才能跟着页面一起变换）
    explicit DropCardBase(QWidget *cardParent);

    DropPopup *popup() const { return m_popup; }
    bool       isPopupOpen() const { return m_popupOpen; }

public slots:
    void togglePopup();
    void openPopupAt();
    void closePopup();

signals:
    void popupClosed();

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;
    void paintEvent(QPaintEvent *) override;

    // 子类画「卡片内容」（图标 / 字母），基类负责底色与三角
    virtual void paintContent(QPainter &p) = 0;
    // 子类在 openPopupAt() 里把「当前值」同步给弹层
    virtual void syncPopupCurrent() = 0;

    // false = 卡片图形已经在底图 PNG 里了，这个控件只当命中区，什么都不画
    void setPaintCard(bool on) { m_paintCard = on; }

    // 底图卡片（ChoiceCard）用：底图烘焙的是「某个具体值」，
    // 用户选了别的值以后必须自己重画卡片，这时把它置 true 来解除 paintEvent 的提前返回
    void setForceDraw(bool on) { m_forceDraw = on; }

    // false = 不铺整卡底色（ChoiceCard 用：卡片本身、圆角、右下角三角都由底图/内容层
    //         提供，整卡填充反而会把底图上「EI_T」这种标签抹掉）
    void setFillBg(bool on) { m_fillBg = on; }

    void setCardRect(const QRect &r) { m_cardRect = r; setGeometry(r); }
    void layoutPopup();

    // 换掉构造时默认建的那个 DropPopup（AEC 场域弹层的版式完全不是行列表）
    void replacePopup(DropPopup *p);

    QRect   m_cardRect;
    QRect   m_triRel;
    QPoint  m_popOff;          // 弹层微调（设计稿各朝向的锚定差 1~2px）
    QPixmap m_tri;
    DropPopup *m_popup = nullptr;
    bool    m_popupOpen = false;
    bool    m_paintCard = true;
    bool    m_forceDraw = false;
    bool    m_fillBg    = true;
};

// ---------------------------------------------------------------------------
//  体型卡片 L / M / S
// ---------------------------------------------------------------------------
class PatientCard : public DropCardBase
{
    Q_OBJECT
public:
    explicit PatientCard(QWidget *cardParent = nullptr);

    // 换版式：卡片位置 + 卡片内图标/字母/三角的相对坐标 + 弹层位置，一次全换。
    // 竖↔横切换 = 只调这一个函数，控件不重建（弹层的当前值/展开状态都保留）。
    void applyProfile(const dr::LayoutProfile &p);

    void setValue(const QString &v);
    QString value() const { return m_value; }

signals:
    void valueChanged(const QString &v);

protected:
    void paintContent(QPainter &p) override;
    void syncPopupCurrent() override;

private:
    // 卡片上那个「人形 + 条形图」图标，跟着体型走：下标与 "LMS".indexOf() 一致
    //   [0]=L [1]=M [2]=S
    QPixmap m_iconSet[3];
    QFont   m_font;
    QString m_value = "M";

    QRect  m_iconRel;      // 相对卡片左上角
    QPoint m_inkRel;
};

// ---------------------------------------------------------------------------
//  站位卡片 Wallstand / Table / Free detector
// ---------------------------------------------------------------------------
class WorkspotCard : public DropCardBase
{
    Q_OBJECT
public:
    explicit WorkspotCard(QWidget *cardParent = nullptr);

    void applyProfile(const dr::LayoutProfile &p);

    void setIndex(int idx);
    int  index() const { return m_index; }

signals:
    void indexChanged(int idx);

protected:
    void paintContent(QPainter &p) override;
    void syncPopupCurrent() override;

private:
    QPixmap m_iconSet[3];
    int     m_index = 0;
    QRect   m_iconRel;
};

// ---------------------------------------------------------------------------
//  底图卡片的纯下拉（Focal Spot / Target exposure index / Exposure parameters）
// ---------------------------------------------------------------------------
//  这三张卡片在底图 PNG 里已经画好了（方形图标 / EL 320 / kV-mAs），所以：
//    · 关闭态 —— 什么都不画，让底图原样透出
//    · 展开态 —— 设计稿会把卡片压暗（#3B3B3B → #242424）但内容不变，
//                底图改不了，于是铺 #242424 + 叠一张「内容层」alpha 图
//  （内容层由 tools/_cut_card_content.py 用「关闭态 / 展开态双底色联立」抠出，
//    竖屏横屏各一套 —— 横屏卡片更窄，不能缩放复用）
class ChoiceCard : public DropCardBase
{
    Q_OBJECT
public:
    // spotId     = layout_profile.h 里 spots 的 id
    // contentRes = 竖屏 / 横屏两套内容层资源路径
    explicit ChoiceCard(QWidget *cardParent, const QString &spotId,
                        const QString &contentRes,
                        const QString &contentResLand);

    void applyProfile(const dr::LayoutProfile &p);

    void setIndex(int idx);
    int  index() const { return m_index; }
    QString text() const;

    // ---- 卡片上「当前值」的显示 -----------------------------------------
    // 底图 PNG 里烘焙的是设计稿那一刻的值（Focal=Small / EL=320 / kV-mAs）。
    // 首屏保持不画（透出底图，与设计稿逐像素一致），用户一旦选了别的值，
    // 就把烘焙那块盖掉、按当前值重画。
    //
    // 图标型（Focal Spot）：每个选项一张图 + 卡片上的图标框（竖屏 / 横屏各一套）
    void setCardIconItems(const QStringList &resPaths,
                          const QRect &boxPortrait, const QRect &boxLand);
    // 文字型（EL 320 / kV-mAs）：字号 / 字重 / 字距 / 颜色 + 竖屏、横屏各自的
    // 墨迹水平中心、墨迹顶、以及要盖掉的矩形（要能容下最长那个值）
    void setCardValueText(int px, qreal track, const QColor &c,
                          int cxPortrait, int topPortrait,
                          int cxLand, int topLand,
                          const QRect &coverPortrait, const QRect &coverLand,
                          QFont::Weight w = QFont::Normal);

    // 卡片上显示的文字可以和弹层行文字不同（设计稿就是这么画的：
    //   弹层行用长minus「kV−mAs」，卡片上是短连字符「kV-mAs」，而且卡片是粗体）。
    // 传空 = 全部沿用弹层行文字。
    void setCardLabels(const QStringList &labels) { m_cardLabels = labels; }

    // 回到「没选过」的样子：烘焙的那块重新透出底图（自检导出时用来还原现场）
    void resetToBaked() { setForceDraw(false); update(); }

signals:
    void indexChanged(int idx, const QString &text);

protected:
    void paintContent(QPainter &p) override;      // 关闭态：仅在接管后画；展开态：先叠内容层
    void syncPopupCurrent() override { m_popup->setCurrent(m_index); }

private:
    QPixmap iconAt(int idx);

    QString m_spotId;
    QString m_res[2];        // [0]=竖屏 [1]=横屏
    QPixmap m_content;
    int     m_index = 0;
    int     m_land  = 0;     // applyProfile 里定的：0 竖屏 / 1 横屏

    QStringList        m_iconRes;     // 图标型：每个选项一张
    QVector<QPixmap>   m_icons;
    QRect              m_iconBox[2];
    int                m_valPx = 0;   // 文字型：0 = 不是文字型
    qreal              m_valTrack = 0.0;
    QFont::Weight      m_valWeight = QFont::Normal;
    QStringList        m_cardLabels;  // 卡片专属文案（空 = 用弹层行文字）
    QColor             m_valColor;
    int                m_valCx[2]  = {0, 0};
    int                m_valTop[2] = {0, 0};
    QRect              m_cover[2];
    QString            m_valText;
};

// ---------------------------------------------------------------------------
//  常亮开关卡片（准星对中 / 跟随）
// ---------------------------------------------------------------------------
//  设计稿：对中2.png（都灭）/ 对中2备份 2.png（对中亮）/ 对中2备份 4.png（跟随亮）
//  横屏等价：对中.png / 对中备份 11.png / 对中备份 9.png
//    · 对中亮 —— 卡片压暗 #3B3B3B→#292929，准星由 #E8E8E8 变绿 #00BD5D
//    · 跟随亮 —— 底色换深蓝 #1C2B38 + 1px 蓝边 #337FBF，右上圆点由灰变亮绿 #44DE85
//  两张都是「单击点亮、再单击熄灭」。点亮态是「底色 + 图形颜色」同时变，
//  而图形本身是抗锯齿叠在旧底色上的，反解 alpha 再上色不如直接切图稳 ——
//  所以点亮态 = 一张整卡烘焙图（含底色/描边/圆角），关闭态不画、让底图透出。
//
//  状态由谁驱动：现在先本地点击切换；以后要挂数据变量，外部状态一变就调
//  setOn()（QAbstractButton::setChecked 的包装，值没变就不发信号，不会打环）。
//  信号直接用 QAbstractButton::toggled(bool)。
class ToggleCard : public QToolButton
{
    Q_OBJECT
public:
    // spotId = layout_profile.h 里 spots 的 id（laser / collim）
    ToggleCard(QWidget *page, const QString &spotId,
               const QString &onRes, const QString &onResLand);

    void applyProfile(const dr::LayoutProfile &p);

    bool isOn() const { return isChecked(); }

public slots:
    void setOn(bool on)  { setChecked(on); }
    void toggleOn()      { setChecked(!isChecked()); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_spotId;
    QString m_res[2];        // [0]=竖屏 [1]=横屏
    QPixmap m_on;
    int     m_land = 0;
};

// ---------------------------------------------------------------------------
//  参数步进器（kV / mA / mAs / ms / DEN）—— 第 10 类控件，数据驱动
// ---------------------------------------------------------------------------
//  设计稿 曝光2备份.png（AEC 关）/ 曝光备份.png（横屏）
//    外框 竖屏 558x98（−/值/+ = 92/366/92）、横屏 502x98（92/310/92）
//    量测（全是中性灰）：
//      使能  填充 #212121  外框 #6F6F6F/#575757  分隔线 #575757  ± #E8E8E8  单位 #8C8C8C  值 #D3D3D3
//      置灰  填充 #1C1C1C  外框 #434343/#373737  分隔线 #373737  ± #7F7F7F  单位 #515151  值 #747474
//    数值墨迹左端 = 值格左 + 48，基线 = 卡顶 + 61；单位墨迹右端 = 值格右 − 24
//    − 横条 36x6；+ 横条 48x6 / 竖条 6x48，都居中于各自格子
//
//  画法分三层（原来的「整卡切图 + 灰度重映射」撑不住动态数值了）：
//    1. 底纹 st_chrome_{on,off}[_land].png —— 从设计稿切，只留 2px 双色外框 + 两条
//       分隔线，内部符号/文字全填平成底色。圆角与描边保持设计稿原样。
//    2. −/+ 符号矢量画：到边界时**单侧**变灰（另一侧仍是亮的），切图做不到。
//    3. 单位 + 数值用 Ubuntu 42px 现画（ink 对齐）。
//
//  交互：单击 ±1 格；长按 500ms 后每 80ms 连发。到 min/max 时该侧符号变灰、
//        点了也不再变值（值本身夹在 [lo,hi]）。
//  DEN 在 AEC 关时显示「–」而不是 0 —— setDashWhenOff(true)。
class StepperCard : public QToolButton
{
    Q_OBJECT
public:
    // spotId   = layout_profile.h 里 steppers 的 id
    // unit     = 值格右侧的单位字（kV / mA / mAs / ms / DEN）
    // lo/hi/step/decimals = 范围与步长；init = 初始值
    StepperCard(QWidget *page, const QString &spotId, const QString &unit,
                double lo, double hi, double step, int decimals, double init);

    void applyProfile(const dr::LayoutProfile &p);

    // 关态显示「–」而不是数值（只有 DEN 用）
    void setDashWhenOff(bool d) { m_dashOff = d; update(); }

    double value() const { return m_value; }
    bool   isOn()  const { return m_on; }

    // ---- 接主控后的两种驱动方式 ----------------------------------------
    //  本地驱动（默认）：点 ± 立刻改 m_value，用于没有主控的演示/自检。
    //  远程驱动：点 ± **只发意图**，本地数值不动，等主控广播回来再 setValue。
    //    原因（抓包实测）：mA/ms/mAs 是主控按参数表跳档（180→200→220…），
    //    本地按 +5 预测会先跳到一个错的值、再被广播纠正，视觉上跳两次。
    void setRemoteDriven(bool r) { m_remote = r; }
    bool isRemoteDriven() const  { return m_remote; }

    // 主控对某方向没反应（300ms 没广播）→ 该侧到底：变灰 + 停连发。
    // 反向再点一次会清掉，因为说明还能往回走。
    void noteLimit(int dir);

public slots:
    // snapGrid = true：夹到 [lo,hi]、**对齐步长网格**、按小数位收敛（本地设定用）
    // snapGrid = false：只夹范围 + 小数位收敛，不对齐网格。
    //   主控广播的 ms 是 5.6 / 12.5 这种，若按 step=1 的网格对齐会被吸成 6，
    //   和真机平板显示对不上 —— 所以接主控时走 false。
    void setValue(double v, bool snapGrid = true);
    void setOn(bool on);

signals:
    void valueChanged(double v);
    void onChanged(bool on);
    // 只有「用户点 ± / 长按连发」才发，程序 setValue 不发 → 接主控无回环
    void stepped(int dir, double v);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    void applyStep(int dir);
    void stopRepeat();
    void paintSymbol(QPainter &p, const QRect &cell, bool plus, const QColor &c);
    QString valueText() const;

    QString m_spotId;
    QString m_unit;
    double  m_lo = 0.0, m_hi = 100.0, m_step = 1.0, m_value = 0.0;
    int     m_decimals = 0;
    bool    m_on      = true;
    bool    m_dashOff = false;        // 关态是否显示「–」
    QRect   m_dec;
    QRect   m_val;
    QRect   m_inc;
    QPixmap m_chrome[4];              // [0]竖亮 [1]竖灰 [2]横亮 [3]横灰
    int     m_land = 0;
    QTimer *m_delay  = nullptr;       // 长按起步
    QTimer *m_repeat = nullptr;       // 连发
    int     m_repeatDir = 0;
    bool    m_remote    = false;      // 远程驱动：点 ± 只发意图
    int     m_limitDir  = 0;          // 主控没反应的那个方向（-1/+1，0=无）
};

// ---------------------------------------------------------------------------
//  AEC 场域弹层（第 7 种弹层，结构完全不是行列表）
// ---------------------------------------------------------------------------
//  设计稿 下拉弹窗2备份 7.png / 下拉弹窗备份 7.png（横屏）
//    弹层 582x561，只有三样东西：标题 / 人体图 / 图里三个可点亮的场域
//    标题墨迹 (28,36) 523x37  ⇒ 42px + 字距 -0.3
//    人体图 (108,137) 367x367（矢量画，见 paintAecFigure）
//    三个场域（图内相对）：左 (58,83) / 右 (233,83) / 下 (146,208)，各 75x100、线宽 6
//    选中的整块填 #E8E8E8，没选中的只描边
//  多选：点一下切换该场域，弹层不关；点空白或再点卡片才关。
//  热区放大到「上排左半 / 上排右半 / 下带」而不是 75x100 —— 后者到屏上只有 23x31。
// ---------------------------------------------------------------------------
class AecFieldPopup : public DropPopup
{
    Q_OBJECT
public:
    explicit AecFieldPopup(QWidget *parent = nullptr);

    // 人体图在弹层内的矩形（367x367 设计尺寸）+ 三个场域在图内的矩形
    void setFigureLayout(const QRect &frame,
                         const QRect &left, const QRect &right, const QRect &bottom);
    // 三个热区（弹层内相对坐标）
    void setZones(const QRect &left, const QRect &right, const QRect &bottom);

    void setMask(int mask) { if (m_mask != mask) { m_mask = mask; update(); } }
    int  mask() const { return m_mask; }

signals:
    void toggled(int idx);          // 0=左 1=右 2=下

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    int zoneAt(const QPoint &pt) const;

    QRect m_frame;
    QRect m_field[3];
    QRect m_zone[3];
    int   m_mask      = 0x2;        // 默认只有右野（与设计稿弹层一致）
    int   m_hoverZone = -1;
};

// ---------------------------------------------------------------------------
//  人体场域卡片（触发卡，layout_profile 里的 spotId = "view"）
// ---------------------------------------------------------------------------
//  底图已经把这张卡连同 58x58 的小人体画好了（小图是同一图形的 0.158 倍缩放），
//  但底图烘焙的是「左野实心」，而最终选定的是「以弹层为准 = 右野实心」，
//  所以关闭态也要把那一小块盖掉重画；展开态整卡压暗成 #242424 后整块重画。
class AecFieldCard : public DropCardBase
{
    Q_OBJECT
public:
    explicit AecFieldCard(QWidget *cardParent = nullptr);

    void applyProfile(const dr::LayoutProfile &p);

    void setMask(int mask);
    int  mask() const { return m_mask; }

signals:
    void maskChanged(int mask);

protected:
    void paintContent(QPainter &p) override;
    void syncPopupCurrent() override;

private:
    AecFieldPopup *m_aec = nullptr;
    QRect m_iconBox;
    int   m_mask = 0x2;
};

// ---------------------------------------------------------------------------
//  顶部患者信息面板（点顶栏的患者名展开，整块盖住患者卡区）
// ---------------------------------------------------------------------------
//  设计稿 患者信息2.png（竖屏 1200x1920）/ 患者信息.png（横屏 1920x1200）
//    面板 = (0,112) 整宽 x 312；外圈 8px #323232、内部 #343434、方角
//    顶栏 0..111 完全不变 —— 它就是触发区（人像图标 + 姓名 + ⌄ 三角）
//    面板以下（Req.SID 行起）完全不变，包括 thumb 卡片露在面板外的下半截
//
//  ⚠️ 展开态不是「半透明遮罩压在上面」：原卡片 #3B3B3B→#383838、
//     卡片间隙 #171717→#343434、部位图标/准星以更暗的形态继续透出。
//     这种「整体换一套暗色并保留残影」的结果用色块拼不出来，所以整块
//     从设计稿烘焙成一张图（两种朝向各一张，横屏只是更宽）。
//
//  第 4 行 Chest PA 在面板底边被裁断（设计稿如此，照抄）；
//  它的亮度只有 54、底色 52，肉眼几乎看不见，是「下一行滚入」的残影。
// ---------------------------------------------------------------------------
class PatientInfoPanel : public QWidget
{
    Q_OBJECT
public:
    // pageParent = 整页（面板与其它卡片同级，才能盖住它们）
    explicit PatientInfoPanel(QWidget *pageParent);

    void applyProfile(const dr::LayoutProfile &p);

    bool isOpen() const { return m_open; }

    // 顶栏热区（页面坐标）。点它 toggle；点页面其它空白关闭。
    void setHotspot(const QRect &r) { m_hot = r; }
    QRect hotspot() const { return m_hot; }

public slots:
    void openPanel();
    void closePanel();
    void togglePanel();

signals:
    void opened();
    void closed();

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;

private:
    QString m_res[2];        // [0]=竖屏 [1]=横屏
    QPixmap m_img[2];
    int     m_land = 0;
    QRect   m_rect;
    QRect   m_hot;
    bool    m_open = false;
};

#endif // PATIENTCARD_H

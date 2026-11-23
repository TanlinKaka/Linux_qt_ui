#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "patientcard.h"
#include "statusstrip.h"
#include "layout_profile.h"
#include "drlink.h"
#include "uicards.h"
#include "tabpages.h"
#include "camsource.h"

#include <QDebug>
#include <QFile>
#include <QDateTime>
#include <QElapsedTimer>
#include <QWidget>
#include <QMouseEvent>
#include <QApplication>
#include <QTimer>
#include <QPainter>
#include <functional>

// 弹层行几何（设计稿 1200x1920 量测）：首行顶 97 / 行高 112 / 底边距 17。
// 面板高度 = 行顶 + 行高×行数 + 底边距，所以行数改由 cards.json 决定后
// 高度也得跟着算 —— 5 行时正好 658，与设计稿原值一致。
// EL 卡例外：那张的滚动条位置全按 5 行量的，高度锁死、多了就滚。
static const int kPopRowTop    = 97;
static const int kPopRowH      = 112;
static const int kPopBottomPad = 17;

/*
 * 设计期 / 运行期的图片路径
 * ------------------------------------------------------------------
 * .ui 里的 background-image 写的是磁盘绝对路径，Qt Designer 才能把底图
 * 画出来（Designer 不认 :/xxx，除非 qrc 参与编译）。
 * 程序启动时统一把 kDesignRoot 前缀换成 kResRoot，图片改由 res.rcc 提供。
 * 换工程目录 / 换用户名时，只改下面第 1 行。
 */
static const char kDesignRoot[] = "/home/neo/UI/";
static const char kResRoot[]    = ":/new/prefix1/";

static void fixImagePaths(QWidget *w)
{
    const QString css = w->styleSheet();
    if (!css.isEmpty()) {
        const QString fixed = QString(css).replace(QLatin1String(kDesignRoot),
                                                   QLatin1String(kResRoot));
        if (fixed != css)
            w->setStyleSheet(fixed);
    }
    const QObjectList kids = w->children();
    for (QObject *o : kids)
        if (QWidget *cw = qobject_cast<QWidget *>(o))
            fixImagePaths(cw);
}

// setupUi() 会先用设计期绝对路径把样式表应用到 MainWindow 及所有子控件，
// 真机上没有 /home/neo/UI 目录，于是每个控件各报一条
// "Could not create pixmap from /home/neo/UI/..."。这属于预期噪音，滤掉；
// 其它任何图片加载失败仍然照常输出。
static QtMessageHandler g_prevHandler = nullptr;

static void designPathFilter(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (msg.startsWith(QLatin1String("Could not create pixmap from "))
        && msg.contains(QLatin1String(kDesignRoot)))
        return;
    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
}

// =====================================================================
//  顶栏右上角的隐形热区（临时调试用）
// ---------------------------------------------------------------------
//  完全透明、不画任何像素，靠 setGeometry 落在设计坐标的顶栏右端那块空白上。
//  2026-11-20 用户圈的位置换算到设计坐标是 (813, 8, 354, 97) —— 也就是竖屏
//  header 的右端（1200-33-354 起）。点一下请 main.cpp 换成另一套版式，再点换回来。
//
//  为什么不用 QToolButton：QToolButton 在透明背景 + 无图标时，某些平台风格
//  仍会画一个 hover 方框；这里要的是「完全看不见」。也不用 Q_OBJECT，
//  用一个 std::function 回调，省掉 .cpp 里 moc 的那套 include。
//  ⚠ press 必须 accept()，否则 QGraphicsProxyWidget 不设抓取者，release 收不到。
//
//  2026-11-20 下午「有时候点了没反应」的三条实测原因（都已修）：
//   ① 松手点必须还在矩形内（`rect().contains(pos)`）→ 贴着屏边点，手指稍微
//      一滚就出界，release 被丢掉 = 没反应。改成 press 时上膛、release 无条件触发。
//   ② 命中区太小：354x97 设计像素，1024x600 屏上竖屏只有 **52 宽 x 178 高**的
//      贴边竖条（rot270 把宽高对调了），比一根手指还窄。已按版式各放宽。
//   ③ 完全透明、无任何反馈 → 点中了也看不出来，误判成「失灵」。
//      现在按下 / 命中后 300ms 会闪一层很淡的白（alpha 46），只在命中时出现。
//  另外 main.cpp 里加了 700ms 冷却，连点两下不会翻两次又翻回来（净效果=没动）。
// =====================================================================
class FlipHotspot : public QWidget
{
public:
    explicit FlipHotspot(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setCursor(Qt::PointingHandCursor);
        setObjectName(QStringLiteral("flipHotspot"));
    }
    std::function<void()> onClick;

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        m_armed = true;
        m_flash = true;
        update();
        qInfo("[hotspot] press (%d,%d)  area %dx%d",
              e->pos().x(), e->pos().y(), width(), height());
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        e->accept();
        const bool armed = m_armed;
        m_armed = false;
        // 按一下就几十毫秒，只在按住时高亮根本看不见；命中后留 300ms 余光
        if (armed) {
            m_flash = true;
            update();
            QTimer::singleShot(300, this, [this]() { m_flash = false; update(); });
        } else {
            m_flash = false;
            update();
        }
        qInfo("[hotspot] release (%d,%d)  inside=%d  armed=%d",
              e->pos().x(), e->pos().y(), rect().contains(e->pos()) ? 1 : 0, armed ? 1 : 0);
        // ⚠ 不再判 rect().contains()：press 已经在框内了，松手飘出去不算取消
        if (armed && onClick)
            onClick();
    }
    void paintEvent(QPaintEvent *) override
    {
        if (!m_flash)
            return;
        QPainter p(this);
        p.fillRect(rect(), QColor(255, 255, 255, 46));   // 很近的「按到了」提示
    }

private:
    bool m_armed = false;
    bool m_flash = false;
};

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    if (!g_prevHandler) {
        g_prevHandler = qInstallMessageHandler(designPathFilter);
        if (!g_prevHandler)                     // 之前没有处理器时返回值是空
            g_prevHandler = [](QtMsgType, const QMessageLogContext &, const QString &) {};
    }

    ui->setupUi(this);

    fixImagePaths(this);

    // 首页「M」卡片 + Patient type 下拉弹层。
    // 父控件给 centralwidget（整页），弹层才能盖住卡片下面的其它内容。
    m_card = new PatientCard(ui->centralwidget);
    connect(m_card, &PatientCard::valueChanged, this, [](const QString &v) {
        qDebug("patient type -> %s", qPrintable(v));
    });

    // 同排右侧的「站位」卡片 + Workspot 下拉弹层（Wallstand/Table/Free detector）
    m_ws = new WorkspotCard(ui->centralwidget);
    connect(m_ws, &WorkspotCard::indexChanged, this, [](int i) {
        static const char *kName[3] = {"Wallstand", "Table", "Free detector"};
        qDebug("workspot -> %s", kName[qBound(0, i, 2)]);
    });

    // ------------------------------------------------------------------
    //  底图上已经画好的三张卡片，各挂一个下拉
    //  ------------------------------------------------------------------
    //  三张卡片（焦点 / EL 320 / kV-mAs）在底图 PNG 里就是静态图形，
    //  控件只当命中区，所以卡片本身不需要跟着选中值重绘 —— 选中结果只
    //  更新内部状态 + 发信号，等以后要把值回写到卡片上再说。
    //
    //  弹层参数的来历（设计稿 1200x1920，见 patientcard.h 顶部的对照表）：
    //    尺寸 = 首行顶 97 + 行高 112*行数 + 底边距 17
    //    标题墨迹 y 恒为 39；行文字用「原点对齐」，墨迹 x 见各自注释
    //  横屏（1920x1200）三个弹层尺寸完全相同，只是跟着卡片换位置，
    //  所以这里配一次就够，切换朝向不用重配。

    // ---- 焦点卡片（X 射线管大/小焦点） → Focal Spot（Small / Large，行首是 checkbox 图标）----
    m_focal = new ChoiceCard(ui->centralwidget, QStringLiteral("square"),
                             QStringLiteral(":/mselect/card_square_content.png"),
                             QStringLiteral(":/mselect/card_square_content_land.png"));
    m_focal->setObjectName(QStringLiteral("cardFocal"));   // --shot 自检按名字找
    // 列表项可被 cards.json 覆盖；没配置就是上面注释里的设计稿默认值。
    const uicards::Resolved focalCfg = uicards::resolve(
                QStringLiteral("focal"),
                { { QStringLiteral("Small"), QStringLiteral(":/mselect/icon_chk_small.png") },
                  { QStringLiteral("Large"), QStringLiteral(":/mselect/icon_chk_large.png") } },
                QStringLiteral("Focal Spot"));
    {
        DropPopup *pp = m_focal->popup();
        // 面板高度跟行数走：2 行 = 97 + 112*2 + 17 = 338（设计稿原值）。
        // 这张卡没有滚动条，行数多了就往下长 —— 位置由 layoutPopup() 兜着。
        pp->setPanelSize(QSize(508, kPopRowTop + kPopRowH * focalCfg.items.size() + 17));
        pp->setHeader(focalCfg.title, 42, -0.4, QPoint(29, 39));
        pp->setRowStyle(42, QPoint(115, 36), QStringLiteral("Small"));
        pp->setIconBox(QRect(28, 27, 59, 59));      // 比另外两个弹层的 64x64 小
        for (const uicards::Item &it : focalCfg.items)
            pp->addItem(it.text, it.icon);
    }
    // 选中后卡片上的图标要跟着换（底图烘焙的是 Small，inner 18px 那个）
    {
        QStringList icons;
        for (const uicards::Item &it : focalCfg.items)
            icons << it.icon;
        m_focal->setCardIconItems(
                    icons,
                    QRect(152, 35, 58, 58),     // 竖屏：设计稿墨迹框，卡片中心 x=181
                    QRect(134, 35, 58, 58));    // 横屏：同上，中心 x=163
        // 设计稿的默认项就是第 0 项（Small）；配置指定了别的才需要动
        if (focalCfg.defIndex > 0)
            m_focal->setIndex(focalCfg.defIndex);
    }
    connect(m_focal, &ChoiceCard::indexChanged, this, [](int i, const QString &t) {
        qDebug("focal spot -> %s (%d)", qPrintable(t), i);
    });

    // ---- EL 320 卡片 → Target exposure index（纯文字列 + 右侧滚动条）----
    m_tgtEi = new ChoiceCard(ui->centralwidget, QStringLiteral("el"),
                             QStringLiteral(":/mselect/card_el_content.png"),
                             QStringLiteral(":/mselect/card_el_content_land.png"));
    m_tgtEi->setObjectName(QStringLiteral("cardTgtEi"));
    // 档位由 cards.json 的 targetEi.items 决定（缺省 = 设计稿那 5 档）。
    const uicards::Resolved eiCfg = uicards::resolve(
                QStringLiteral("targetEi"),
                { { QStringLiteral("50"),  QString() },
                  { QStringLiteral("64"),  QString() },
                  { QStringLiteral("80"),  QString() },
                  { QStringLiteral("100"), QString() },
                  { QStringLiteral("125"), QString() } },
                QStringLiteral("Target exposure index"));
    {
        DropPopup *pp = m_tgtEi->popup();
        // 高度锁死 658（= 97 + 112*5 + 1）：轨道 32x560 和滑块 16x156 都是按
        // 5 行量出来的，面板一变长这两条就得跟着重算、还容易顶出屏幕。
        // 所以行数超过 5 时不增高，改成滚动（拖滑块 / 点轨道 / 拖列表）。
        pp->setPanelSize(QSize(508, 658));
        pp->setHeader(eiCfg.title, 41, -0.4, QPoint(26, 39));
        pp->setRowStyle(41, QPoint(27, 38), QStringLiteral("100"));
        pp->setIconBox(QRect());                    // 无图标
        // 滚动条占了右侧 32px，所以高亮要让到 x=475（否则盖住轨道）
        pp->setListRight(475);
        pp->setScrollbar(QRect(475, 97, 32, 560),   // 轨道：列表区整高
                         QRect(483, 105, 16, 156)); // 滑块：16x156 胶囊
        for (const uicards::Item &it : eiCfg.items)
            pp->addItem(it.text, it.icon);

        // 可见行数：配置可改（visibleRows），默认 5。行数不超过它时
        // scrolling() 为假，滑块退回设计稿那条固定的 156px，像素不变。
        pp->setVisibleRows(eiCfg.visibleRows > 0 ? eiCfg.visibleRows : 5);
    }
    // 选中后卡片上的数值要跟着换（底图烘焙的是 320）
    // 字号定法：设计稿墨迹 84x34；Ubuntu 49px 是 80x34（高度精确），
    // 差的 4px 来自字距 —— 3 个字 2 个间隙各 +2px 正好 84，故 track=+2.0。
    m_tgtEi->setCardValueText(49, 2.0, QColor(0xE8, 0xE8, 0xE8),
                              181, 67,                  // 竖屏：墨迹 (139,67) w84
                              162, 68,                  // 横屏：墨迹 (120,68) 中心 161.5
                              QRect(123, 60, 116, 48),  // 盖掉区（含 125 这种 3 位数）
                              QRect(104, 61, 116, 48));
    connect(m_tgtEi, &ChoiceCard::indexChanged, this, [](int i, const QString &t) {
        qDebug("target exposure index -> %s (%d)", qPrintable(t), i);
    });
    // 只有配置里明确写了 default 才选中它；不写就保持「透出底图烘焙的 320」——
    // 与改动前一致，逐像素自检的基线也就不会因为加了配置层而漂。
    if (eiCfg.defIndex >= 0)
        m_tgtEi->setIndex(eiCfg.defIndex);

    // ---- kV-mAs 卡片 → Exposure parameters（kV-mAs / kV-mAs-ms / kV-mA-ms）----
    //  首行以实机为准是 "kV-mAs"（设计稿那版图里写的是占位的 "kV"，
    //  但实机三项就是 kV-mAs / kV-mAs-ms / kV-mA-ms，没有「只有 kV」这一档）。
    //  卡片（按钮）上的文案也必须跟着是 kV-mAs —— 见下面的 setCardValueText。
    m_expP = new ChoiceCard(ui->centralwidget, QStringLiteral("kvma"),
                            QStringLiteral(":/mselect/card_kvma_content.png"),
                            QStringLiteral(":/mselect/card_kvma_content_land.png"));
    m_expP->setObjectName(QStringLiteral("cardExpP"));
    // 三项可被 cards.json 的 exposure.items 覆盖（缺省 = 下面这组设计稿文案）。
    const uicards::Resolved expCfg = uicards::resolve(
                QStringLiteral("exposure"),
                { { QStringLiteral("kV\u2212mAs"),        QString() },
                  { QStringLiteral("kV\u2212mAs\u2212ms"), QString() },
                  { QStringLiteral("kV\u2212mA\u2212ms"),  QString() } },
                QStringLiteral("Exposure parameters"));
    {
        DropPopup *pp = m_expP->popup();
        // 3 行 = 97 + 112*3 + 17 = 450（设计稿原值）；行数变了就跟着长
        pp->setPanelSize(QSize(508, kPopRowTop + kPopRowH * expCfg.items.size() + kPopBottomPad));
        pp->setHeader(expCfg.title, 42, -0.4, QPoint(29, 39));
        // 这个弹层的行字号比另两个小 1~2px（实测墨迹宽度：
        //   "kV−mAs−ms" 221 / "kV−mA−ms" 202  ⇒ 40px 全部吻合）
        //   probe 仍用 "kV"：只用来定基线，与实际行文案无关。
        pp->setRowStyle(40, QPoint(28, 37), QStringLiteral("kV"));
        pp->setIconBox(QRect());                    // 无图标
        for (const uicards::Item &it : expCfg.items)
            pp->addItem(it.text, it.icon);
    }
    // 选中后卡片上的文字要跟着换。设计稿上这张卡片（按钮）的字和弹层行不是同一套：
    //   弹层行 = 常规体 40px + 长 minus「kV−mAs」（墨迹 221 / 202，量测吻合）
    //   卡片上 = 粗体 46px + 短连字符「kV-mA」（墨迹 160x36，质量 1982，
    //            常规体只有 1296 —— 明显是 Bold，不是 Regular 描粗）
    // 所以卡片另给一套文案（连字符），字重也单独给 Bold；字距 3.8 是反算的：
    //   Bold 46px 的「kV-mA」无字距 145 宽，设计稿 160，4 个间隙各补 3.75。
    // 注意中心不是卡片几何中心：竖屏卡片 749 宽而墨迹中心在 338，
    // 横屏卡片 674 宽、墨迹中心在 337 —— 设计稿在两种朝向下都把它放在左侧 674 区域内居中。
    // 卡片文案不另写一份：直接拿弹层行文字把长 minus「−」(U+2212) 换成短连字符，
    // 配置改了 items 之后按钮上的字也自动跟着变（设计稿本来就是这两套字形）。
    {
        QStringList cardLabels;
        for (const uicards::Item &it : expCfg.items) {
            QString s = it.text;
            s.replace(QChar(0x2212), QLatin1Char('-'));
            cardLabels << s;
        }
        m_expP->setCardLabels(cardLabels);
    }
    m_expP->setCardValueText(46, 3.8, QColor(0xE8, 0xE8, 0xE8),
                             338, 44,
                             337, 44,
                             QRect(198, 36, 280, 50),
                             QRect(197, 36, 280, 50),
                             QFont::Bold);
    // ⚠ 顺序不能反：setIndex() 里「要不要自己接管绘制」判的是 m_valPx > 0。
    //   先 setIndex 再 setCardValueText，第一次就会落到「还没接管」的早退分支，
    //   forceDraw 一直是 false，按钮上透出的就永远是底图烘焙的 kV-mA。
    //   这不是设计稿的问题，是接线顺序问题 —— 改按钮文案必须在这里生效。
    // 默认项由配置给（缺省 = 第 0 项，即按钮上印的 kV−mAs）。
    m_expP->setIndex(expCfg.defIndex > 0 ? expCfg.defIndex : 0);
    connect(m_expP, &ChoiceCard::indexChanged, this, [](int i, const QString &t) {
        qDebug("exposure parameters -> %s (%d)", qPrintable(t), i);
    });

    // ---- 人体场域卡片 → AEC 场域（第 7 种弹层，结构不是行列表）----
    //  触发卡不是左边那张 "AEC" 文字卡，而是中间这张画着小人体的卡：
    //  设计稿里只有它展开时被压暗、也只有它带右下角三角。
    //  它的 spotId 历史上叫 "view"（当时把图标误认成「视图」），沿用不改。
    //  默认选中 = 右野（以弹层为准；底图烘焙的是左野，关闭态会盖掉重画）。
    m_aecField = new AecFieldCard(ui->centralwidget);
    m_aecField->setObjectName(QStringLiteral("cardAecField"));
    m_aecField->setMask(0x2);
    connect(m_aecField, &AecFieldCard::maskChanged, this, [](int m) {
        qDebug("aec field mask -> 0x%x", m);
    });

    // ---- 准星对中 / 跟随：两张常亮开关卡 ----
    //  都是「单击点亮 → 再单击熄灭」。点亮态整卡换一套配色（对中：压暗 + 准星变绿；
    //  跟随：深蓝底 + 蓝边 + 右上圆点亮绿），所以点亮图直接从设计稿切，
    //  关闭态不画、让底图透出 —— 两种朝向各一张，尺寸 = 热区尺寸。
    //  以后要挂数据变量：外部状态一变就 m_center->setOn(v)/m_follow->setOn(v)，
    //  想拿状态就读 isOn()，信号是 QAbstractButton::toggled(bool)。
    m_center = new ToggleCard(ui->centralwidget, QStringLiteral("laser"),
                              QStringLiteral(":/mselect/card_center_on.png"),
                              QStringLiteral(":/mselect/card_center_on_land.png"));
    m_center->setObjectName(QStringLiteral("cardCenter"));
    connect(m_center, &ToggleCard::toggled, this, [](bool on) {
        qDebug("centering -> %s", on ? "on" : "off");
    });

    m_follow = new ToggleCard(ui->centralwidget, QStringLiteral("collim"),
                              QStringLiteral(":/mselect/card_follow_on.png"),
                              QStringLiteral(":/mselect/card_follow_on_land.png"));
    m_follow->setObjectName(QStringLiteral("cardFollow"));
    connect(m_follow, &ToggleCard::toggled, this, [](bool on) {
        qDebug("follow -> %s", on ? "on" : "off");
    });

    // ---- AEC 开关 + 五个参数步进器 ----
    //  AEC 点亮后 DEN 才可调（设计稿 曝光2备份 3.png 里 DEN 那行由灰变亮）；
    //  kV / mA / mAs / ms 的可调性由「Exposure parameters」选了哪一项决定
    //  （见 applyExposureMode）。底纹/符号/文字都是运行期画的，见 StepperCard。
    m_aecBtn = new ToggleCard(ui->centralwidget, QStringLiteral("aec"),
                              QStringLiteral(":/mselect/card_aec_on.png"),
                              QStringLiteral(":/mselect/card_aec_on_land.png"));
    m_aecBtn->setObjectName(QStringLiteral("cardAec"));

    // 范围/步长按真机取值：kV 1、mA 5、mAs 0.1（一位小数）、ms 1、DEN 1
    // ⚠ ms 小数位设 1：主控广播的 ms 是 5.6 / 12.5 / 2.2 这种（抓包实测），
    //   小数位给 0 会被四舍五入成 6，与平板显示对不上。
    auto mkStepper = [this](const char *spotId, const char *objName, const char *unit,
                            double lo, double hi, double step, int dec, double init)
            -> StepperCard * {
        StepperCard *s = new StepperCard(ui->centralwidget, QLatin1String(spotId),
                                         QString::fromUtf8(unit),
                                         lo, hi, step, dec, init);
        s->setObjectName(QLatin1String(objName));
        return s;
    };
    m_stKv  = mkStepper("stKv",  "stKv",  "kV",   40.0, 150.0, 1.0, 0,  75.0);
    m_stMa  = mkStepper("stMa",  "stMa",  "mA",   10.0, 500.0, 5.0, 0,  80.0);
    m_stMas = mkStepper("stMas", "stMas", "mAs",   0.1, 500.0, 0.1, 1,   5.0);
    m_stMs  = mkStepper("stMs",  "stMs",  "ms",    1.0,1000.0, 1.0, 1,   2.0);
    m_stDen = mkStepper("stDen", "stDen", "DEN",  -8.0,   8.0, 1.0, 0,   0.0);
    // DEN 关态在设计稿里印的是「–」不是 0
    m_stDen->setDashWhenOff(true);

    // mA / ms / mAs 三者满足 mAs = mA × ms / 1000。
    //   改 mA 或 ms → 重算 mAs；
    //   改 mAs     → mA 不动，反算 ms（mA 当主参数，和真机习惯一致）。
    // m_syncing 防打环：联动出来的 setValue 也会发 valueChanged。
    //
    // ⚠ 接主控后整段联动都关掉（m_link != nullptr）：mA / ms / mAs 三个值是
    //   主控分三帧广播的，各自都是权威值，本地反算只会打架。
    //   实测例子：主控给 MA=200 / MS=5.6 / MAS=1.2（四舍五入前 1.12），
    //   若按 mA×ms 反算 ms，会把 5.6 改成 6.0，与平板显示不一致。
    connect(m_stMa, &StepperCard::valueChanged, this, [this](double v) {
        if (m_syncing || m_link) return;
        m_syncing = true;
        m_stMas->setValue(v * m_stMs->value() / 1000.0);
        m_syncing = false;
    });
    connect(m_stMs, &StepperCard::valueChanged, this, [this](double v) {
        if (m_syncing || m_link) return;
        m_syncing = true;
        m_stMas->setValue(m_stMa->value() * v / 1000.0);
        m_syncing = false;
    });
    connect(m_stMas, &StepperCard::valueChanged, this, [this](double v) {
        if (m_syncing || m_link) return;
        m_syncing = true;
        const double ma = m_stMa->value();
        if (ma > 0.0)
            m_stMs->setValue(v * 1000.0 / ma);
        m_syncing = false;
    });
    // 启动归一化：设计稿的 75/80/5/2 本身不满足公式，按 mA×ms 重算一次 mAs
    m_syncing = true;
    m_stMas->setValue(m_stMa->value() * m_stMs->value() / 1000.0);
    m_syncing = false;
    qDebug("steppers: kV=%g mA=%g mAs=%g ms=%g DEN=%g",
           m_stKv->value(), m_stMa->value(), m_stMas->value(),
           m_stMs->value(), m_stDen->value());

    connect(m_aecBtn, &ToggleCard::toggled, this, [this](bool on) {
        qDebug("aec -> %s", on ? "on" : "off");
        if (m_stDen) m_stDen->setOn(on);        // AEC 开 → DEN 可调
    });
    connect(m_expP, &ChoiceCard::indexChanged, this, [this](int i, const QString &) {
        applyExposureMode(i);
    });
    // 初始态跟着卡片自己的选中项走（= 0，kV−mAs），别再写死 -1 ——
    // 写死 -1 会让「按钮写 kV−mAs、可调的却是 mA」这种自相矛盾的状态留在首屏上。
    applyExposureMode(m_expP->index());

    // ---- 顶栏患者信息：点「人像 + 姓名 + 三角」展开整块的详情面板 ----
    //  热区 544x96 盖住 (16,8)..(559,103)。设计稿两种朝向里这三样东西的
    //  包围盒都是 (28,28)..(535,83)：图标 28..83 / 姓名 100..475 / 三角 500..535。
    //  面板本身是整宽 x 312 的烘焙图（见 patientcard.h 的 PatientInfoPanel），
    //  它盖住整个患者卡区，所以必须最后创建 + openPanel() 里 raise()。
    // ---- 读数区 / 底栏：盖色块重画 ----
    //  这两块在底图里是烘焙死的像素，没有控件。这里各起一个 QWidget，
    //  先铺 #171717 把旧内容整个盖掉，再按量测值重画分隔线/图标/文字。
    //  底图一个字节都不动，所以设计稿仍是像素回归基准。
    //  不重叠于任何卡片（读数区 y425..553、底栏 y1755..1920），z-order 无所谓。
    m_info = new InfoStrip(ui->centralwidget);
    m_info->setObjectName(QStringLiteral("infoStrip"));
    m_footer = new FooterBar(ui->centralwidget);
    m_footer->setObjectName(QStringLiteral("footerBar"));

    //  顶栏姓名 + 缩略图部位名：同样盖色块重画（底图里也是烘焙死的文字）
    m_header = new HeaderBar(ui->centralwidget);
    m_header->setObjectName(QStringLiteral("headerBar"));
    m_caption = new ThumbCaption(ui->centralwidget);
    m_caption->setObjectName(QStringLiteral("thumbCaption"));

    // ---- 四个页签：Exposure / View / Position / Stitching ----
    //  底图只烘焙了 Exposure 这一页，页签行也烘焙着「Exposure 亮 + 蓝色下划线」，
    //  所以切页必须把「页签行」和「内容区」两块都盖掉重画。
    //  放在卡片之后创建 → 天然压在卡片之上；患者信息面板再压一层（它更晚建）。
    m_tabs = new TabBarPage(ui->centralwidget);
    m_tabs->setObjectName(QStringLiteral("tabBar"));
    m_viewPage = new CameraOffPage(ui->centralwidget);
    m_viewPage->setObjectName(QStringLiteral("pageView"));
    m_posPage = new PositionPage(ui->centralwidget);
    m_posPage->setObjectName(QStringLiteral("pagePosition"));
    m_stitchPage = new StitchingPage(ui->centralwidget);
    m_stitchPage->setObjectName(QStringLiteral("pageStitching"));
    wireTabPage();

    m_patHot = new QToolButton(ui->centralwidget);
    m_patHot->setObjectName(QStringLiteral("patHot"));
    m_patHot->setCursor(Qt::PointingHandCursor);
    m_patHot->setAutoRaise(true);
    m_patHot->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;}"));
    m_patHot->setGeometry(QRect(16, 8, 544, 96));

    m_patInfo = new PatientInfoPanel(ui->centralwidget);
    connect(m_patHot, &QToolButton::clicked, m_patInfo, &PatientInfoPanel::togglePanel);
    connect(m_patInfo, &PatientInfoPanel::opened, this, [] { qDebug("patient info -> open"); });
    connect(m_patInfo, &PatientInfoPanel::closed, this, [] { qDebug("patient info -> close"); });

    // 临时调试热区：顶栏右端，点一下切横屏、再点切回竖屏。
    // 几何在 applyProfile() 里按设计稿宽度靠右摆（两套版式共用一个尺寸）。
    {
        FlipHotspot *hot = new FlipHotspot(ui->centralwidget);
        hot->onClick = [this]() { emit orientationToggleRequested(); };
        m_flipBtn = hot;
    }

    // 自检：真机上应为 1；为 0 说明 res.rcc 没加载或路径写错
    qDebug("bg in resources: %d",
           QFile::exists(":/new/prefix1/Resource/PHILIPS_TUI/对中2.png") ? 1 : 0);
    qDebug("mselect res: card=%d L=%d ws=%d chk=%d font=%d",
           QFile::exists(":/mselect/icon_card.png") ? 1 : 0,
           QFile::exists(":/mselect/icon_L.png") ? 1 : 0,
           QFile::exists(":/mselect/icon_ws_wallstand.png") ? 1 : 0,
           QFile::exists(":/mselect/icon_chk_small.png") ? 1 : 0,
           QFile::exists(":/mselect/Ubuntu-R.ttf") ? 1 : 0);
}

MainWindow::~MainWindow()
{
    delete ui;
}

// =====================================================================
//  四个页签：Exposure / View / Position / Stitching
// ---------------------------------------------------------------------
//  Exposure 页的控件（AEC/视图/EL/参数卡/步进器）在底图上并不是像素，
//  而是真控件，所以切到别的页时必须整体 setVisible(false)；切回来再显示，
//  并重跑一次 applyExposureMode() —— 否则「mAs/ms 该灰的格子」会保持上次的
//  明暗（步进器的两态是控件内部状态，不是靠父控件可见性恢复的）。
// =====================================================================
void MainWindow::showTab(int i)
{
    m_tab = qBound(0, i, 3);
    const bool land = (m_tabs && m_tabs->landscape());
    // 2026-11-20：横屏也按真页签切了（之前没有横屏设计稿，恒显 Exposure）。
    //  横屏的页签行/内容区坐标已按 曝光备份.png 量出，两个页控件各自有横屏分支。
    const int eff = m_tab;

    QWidget *exposure[] = {
        reinterpret_cast<QWidget *>(m_aecField), reinterpret_cast<QWidget *>(m_center),
        reinterpret_cast<QWidget *>(m_follow),   reinterpret_cast<QWidget *>(m_aecBtn),
        reinterpret_cast<QWidget *>(m_stKv),     reinterpret_cast<QWidget *>(m_stMa),
        reinterpret_cast<QWidget *>(m_stMas),    reinterpret_cast<QWidget *>(m_stMs),
        reinterpret_cast<QWidget *>(m_stDen),    reinterpret_cast<QWidget *>(m_tgtEi),
        reinterpret_cast<QWidget *>(m_expP),     reinterpret_cast<QWidget *>(m_focal),
    };
    for (QWidget *w : exposure)
        if (w) w->setVisible(eff == 0);
    // 缩略图 / 左上卡片区 / Req.SID 读数：**两种朝向都常驻**。
    // 竖屏页内容区从 y684 起碰不到它们；横屏的页内容区只有右侧 (840,120)-(1912,1032)，
    // 左边 840px 是常驻患者栏（摄像机关闭.png / 一键位.png / 拼接备份.png 三张横屏
    // 设计稿里都保留着缩略图 + M 卡 + 115cm/75cm 读数）→ 切页时不能收掉。
    if (eff == 0 && m_expP)
        applyExposureMode(m_expP->index());

    if (m_viewPage)   m_viewPage->setVisible(eff == 1);
    if (m_posPage)    m_posPage->setVisible(eff == 2);
    if (m_stitchPage) m_stitchPage->setVisible(eff == 3);

    // 摄像头只在 View 页跑：切走停流省 CPU（画面保留最后一帧），切回来续上。
    // 摄像头重连要 1~2s，所以不能停 pipeline —— 这里只是 stop，状态还是 ON，
    // 回来时 start 一次即可（同一路 RTSP，摄像头端会立刻重推关键帧）。
    if (m_camOn && m_camSource) {
        if (eff == 1) {
            if (!m_camSource->running())
                m_camSource->start(m_camUrl, 640, 360);
        } else if (m_camSource->running()) {
            m_camSource->stop();
        }
    }

    qDebug("tab -> %d (%s)", eff, land ? "landscape" : "portrait");

    // 活动页签必须有下划线跟着走：漏掉这句的话，蓝色下划线会一直停在
    // 底图烘焙的 Exposure 下面（切了页也看不出来）。
    if (m_tabs) m_tabs->setActive(eff);
    // 层序：先当前页控件，再页签行 —— 横屏的页控件从 y120 起（要盖底图残留），
    // 与页签行 y120..215 重叠，所以页签行必须最后 raise，否则点不到页签。
    if (eff == 1 && m_viewPage)   m_viewPage->raise();
    if (eff == 2 && m_posPage)    m_posPage->raise();
    if (eff == 3 && m_stitchPage) m_stitchPage->raise();
    if (m_tabs) m_tabs->raise();
}

void MainWindow::onGoToPosition()
{
    if (!m_posPage || m_posPage->moving())
        return;
    qDebug("go to position: A=%d B=%d", m_posPage->angleA(), m_posPage->angleB());
    // 设计稿 12/13 的顺序：点 Go to position → 支架开始动（⚠ System is moving.....）
    // → 到位（ⓘ System reached position）。运动中所有角度按钮由 PositionPage 自己锁掉。
    // 这里仍用定时器演，接主控后换成真实事件（收到位码再置 reached）。
    m_posPage->setStatus(QStringLiteral("System is moving....."), 1);
    QTimer::singleShot(2500, this, [this] {
        if (m_posPage && m_posPage->moving())
            m_posPage->setStatus(QStringLiteral("System reached position"), 2);
    });
}

// =====================================================================
//  摄像头（IP 摄像头 → GStreamer → QImage → View 页）
// ---------------------------------------------------------------------
//  startCamera 是「先切面、后起流」：立刻把页面切到 ON（画面区先填 #090909），
//  再去连 RTSP。反过来会让用户点完按钮盯着 OFF 界面等 1~2 秒，像没反应。
// =====================================================================
void MainWindow::setCameraUrl(const QString &url)
{
    m_camUrl = url;
    qDebug("[cam] url = %s", qPrintable(url));
}

void MainWindow::setCameraFit(bool cover)
{
    m_camFitCover = cover;
    if (m_viewPage)
        m_viewPage->setFitCover(cover);
}

// 取流调优项：限帧 / 解码输出格式 / 解码输出尺寸。
// 这三项是压 CPU 的全部杠杆（实测见 camsource.cpp 顶部），改了要在
// 下一次 start() 才生效 —— 所以统一在 startCamera() 里下发。
void MainWindow::setCameraTuning(int fps, const QString &fmt, int decW, int decH)
{
    m_camFps  = fps;
    m_camFmt  = fmt;
    m_camDecW = decW;
    m_camDecH = decH;
    qDebug("[cam] tuning fps=%d fmt=%s dec=%dx%d",
           m_camFps, qPrintable(m_camFmt), m_camDecW, m_camDecH);
}

void MainWindow::setCameraNet(const QString &transport, int latencyMs, bool dropOnLat)
{
    m_camTransport = transport;
    m_camLatencyMs = latencyMs;
    m_camDropOnLat = dropOnLat;
    qDebug("[cam] net transport=%s latency=%dms droplat=%d",
           qPrintable(m_camTransport), m_camLatencyMs, int(m_camDropOnLat));
}

void MainWindow::setCameraSinkBufs(int n)
{
    m_camSinkBufs = n >= 0 ? n : 1;
    qDebug("[cam] sink max-buffers=%d", m_camSinkBufs);
}

void MainWindow::applyCameraTuning()
{
    if (!m_camSource)
        return;
    m_camSource->setFps(m_camFps);
    m_camSource->setPixelFormat(m_camFmt);
    m_camSource->setDecodeSize(m_camDecW, m_camDecH);
    m_camSource->setTransport(m_camTransport);
    m_camSource->setLatencyMs(m_camLatencyMs);
    m_camSource->setDropOnLatency(m_camDropOnLat);
    m_camSource->setSinkBuffers(m_camSinkBufs, true);
}

void MainWindow::startCamera()
{
    if (!m_viewPage)
        return;

    m_viewPage->setFitCover(m_camFitCover);
    m_viewPage->clearFrame();
    m_viewPage->setCameraState(CameraOffPage::CamOn);
    m_camOn = true;

    if (m_camSource) {
        applyCameraTuning();
        m_camSource->start(m_camUrl, 640, 360);   // 异步，帧到了自然上屏
    }

    qDebug("[cam] -> ON   url=%s", qPrintable(m_camUrl));
}

void MainWindow::stopCamera()
{
    m_camOn = false;
    // ⚠ 顺序很关键：先让界面切到 OFF，再销毁管线。
    //   gst_element_set_state(NULL) 会阻塞主线程（rtspsrc 关 TCP、清 jitterbuffer，
    //   实测几百毫秒）。原来先 stop 再切界面，用户看不到任何反应就会再点一下，
    //   这一下的事件排在队列里，等阻塞结束立刻被处理 → 又开回去了，
    //   表现出来就是「点关闭却没关」。
    if (m_viewPage) {
        m_viewPage->clearFrame();
        m_viewPage->setCameraState(CameraOffPage::CamOff);
        // 同步重画一次再进 stop()：否则 update() 只是排进事件队列，紧接着被
        // 176ms 的 stop() 挡住，用户看到的「关闭」要等两百毫秒才出现。
        m_viewPage->repaint();
    }
    if (m_camSource) {
        CamSource *src = m_camSource;
        QTimer::singleShot(0, this, [src] {
            QElapsedTimer t; t.start();
            src->stop();
            // 这个数是主线程被卡住的时长：>300ms 用户就会觉得「点了没反应」而补点。
            qDebug("[cam] stop took %lld ms", (long long)t.elapsed());
        });
    }
    qDebug("[cam] -> OFF");
}

void MainWindow::wireTabPage()
{
    connect(m_tabs, &TabBarPage::tabClicked, this, [this](int i) { showTab(i); });

    // ⚠ 这里**不做**任何冷却。一次物理点击会派生出多组 press/release（ILITEK 触摸屏
    //   在内核里同时注册了「触摸」和「兼容鼠标」两个设备），去重必须在页面里按
    //   **事件时间戳**做（见 CameraOffPage::gestureAccepted）。
    //   原来这里用「处理时刻」做 400ms 冷却，一旦 stop() 阻塞主线程，积压的重复
    //   事件被处理时早就超出窗口 → 又翻一次 → 「点关闭却是开着的」。
    connect(m_viewPage, &CameraOffPage::cameraToggle, this, [this] {
        if (m_camOn)
            stopCamera();
        else
            startCamera();
    });

    // ---- 摄像头取帧源（帧 → QImage → 页面 paintEvent）----
    if (!m_camSource) {
        m_camSource = new CamSource(this);
        connect(m_camSource, &CamSource::frameReady, this, [this](const QImage &img) {
            // 关掉之后管线是「延迟一拍」才销毁的，这期间还会送帧上来。
            // 不拦住的话会把界面从 OFF 顶回 ON（正是「点关闭没关掉」的观感）。
            if (!m_camOn) {
                m_camSource->ackFrame();
                return;
            }
            if (m_viewPage) {
                m_viewPage->setCameraFrame(img);
                // 重连成功后第一帧到达：从 Error 自动切回 On
                if (m_viewPage->cameraState() != CameraOffPage::CamOn)
                    m_viewPage->setCameraState(CameraOffPage::CamOn);
            }
            m_camSource->ackFrame();      // 放行下一帧（节流阀）
        });
        connect(m_camSource, &CamSource::failed, this, [this](const QString &why) {
            qWarning("[cam] failed: %s", qPrintable(why));
            // 没配置 URL 的时候不算故障，停在「连接中」的 #090909；
            // 正在关闭（m_camOn=false）时的 BUS 错误（EOS/关流报错）也不算故障，
            // 否则会把界面画成「摄像机不可使用」而不是「已关闭」。
            if (m_camOn && !m_camUrl.isEmpty() && m_viewPage)
                m_viewPage->setCameraState(CameraOffPage::CamError);
        });
    }

    connect(m_posPage, &PositionPage::tiltModeChanged, this, [](int m) {
        qDebug("tilt mode -> %d", m);
    });
    // 点角度按钮只点亮/取消点亮，不出提示 —— 提示只在「已选中 + Go to position」后出现
    connect(m_posPage, &PositionPage::anglePicked, this, [](int panel, int value) {
        qDebug("position: panel %d -> %d deg", panel, value);
    });
    connect(m_posPage, &PositionPage::goToPosition, this, &MainWindow::onGoToPosition);
}

// 竖屏 / 横屏是两张独立的整页设计稿（1200x1920 与 1920x1200），
// 布局是「三列重排」而不是把竖屏转 90° —— 所以这里一次换三样：
//   页面尺寸、底图、卡片（含弹层）的坐标。
// 底图走资源：res.qrc 里两张图都在（对中2.png / 常规状态.png）。
void MainWindow::applyProfile(const dr::LayoutProfile &p)
{
    setFixedSize(p.design);
    setStyleSheet(QStringLiteral("background-image: url(%1Resource/PHILIPS_TUI/%2);")
                  .arg(QLatin1String(kResRoot), p.bg));

    if (m_card)
        m_card->applyProfile(p);
    if (m_ws)
        m_ws->applyProfile(p);
    // 这三个只换命中区的位置；弹层尺寸/内容两种朝向完全一样，不用重配
    if (m_focal) m_focal->applyProfile(p);
    if (m_tgtEi) m_tgtEi->applyProfile(p);
    if (m_expP)  m_expP->applyProfile(p);
    if (m_aecField) m_aecField->applyProfile(p);
    if (m_center)   m_center->applyProfile(p);
    if (m_follow)   m_follow->applyProfile(p);
    if (m_aecBtn)   m_aecBtn->applyProfile(p);
    for (StepperCard *s : {m_stKv, m_stMa, m_stMas, m_stMs, m_stDen})
        if (s) s->applyProfile(p);
    // 患者信息面板：矩形按 header 下沿 + 整宽算，两种朝向热区完全一样
    if (m_patInfo) m_patInfo->applyProfile(p);
    if (m_patHot)  m_patHot->setGeometry(QRect(16, 8, 544, 96));
    // 读数区 / 底栏：目前只有竖屏版实现（横屏 2×2 的读数区还要另排）
    const bool land = (p.name == QLatin1String("landscape"));
    if (m_info)   m_info->applyProfile(land);
    if (m_footer) m_footer->applyProfile(land);
    if (m_header) m_header->applyProfile(land);
    if (m_caption) m_caption->applyProfile(land);

    // 四个页签：横屏版式 2026-11-20 补齐（页签行量自 曝光备份.png，
    // 内容区 y216..1031）。两个页控件在 applyProfile(true) 里自己摆横屏几何。
    if (m_tabs)       m_tabs->applyProfile(land);
    if (m_viewPage)   m_viewPage->applyProfile(land);
    if (m_posPage)    m_posPage->applyProfile(land);
    if (m_stitchPage) m_stitchPage->applyProfile(land);
    showTab(m_tab);

    // 隐形翻转热区：钉在**设计稿右上角**，两套版式用同一块相对位置。
    // 必须放在 showTab() 之后 raise，否则会被页签行/页控件盖住点不到。
    //
    // ⚠ 命中区不能只按「视觉角」给 354x97：1024x600 屏上
    //    竖屏 rot270 会把宽高对调 → 实际只有 52 宽 x 178 高的贴边竖条，
    //    比一根手指还窄，竖着蹭一下就漏。这里按版式分别放宽，但要避开
    //    header 下方的第一排卡片和横屏的页签行（y120..215）：
    //      竖屏：header 0..111，下方 112..135 是空隙，卡片从 y136 起 → 取 y0..130
    //      横屏：header 0..111，页签行 y120 起但只在 x840..1911 → 取 y0..112、x 从 1360 起
    if (m_flipBtn) {
        m_flipBtn->setGeometry(land ? QRect(1360, 0, 560, 112)
                                    : QRect(730, 0, 470, 130));
        m_flipBtn->raise();
    }

    qDebug("profile '%s'  %dx%d  rot=%d  bg=%s",
           qPrintable(p.name), p.design.width(), p.design.height(),
           p.rot, qPrintable(p.bg));
}

// =====================================================================
//  调试变量通道
// ---------------------------------------------------------------------
//  key 一律小写；value 不区分大小写（除体型 L/M/S 统一转大写）。
//  返回 false = key 不认识或值非法，调用方会打一行警告，方便查错别字。
//  以后接网口：收到协议帧 → 解析成 (key,value) → 逐条调 setVar()，
//  界面代码一行都不用改。
// =====================================================================
static bool varTruthy(const QString &v)
{
    const QString s = v.trimmed().toLower();
    return s == "1" || s == "on" || s == "true" || s == "yes";
}

bool MainWindow::setVar(const QString &keyIn, const QString &valueIn)
{
    const QString k = keyIn.trimmed().toLower();
    const QString v = valueIn.trimmed();
    if (k.isEmpty())
        return false;

    // ---------------- 四个页签 ----------------
    if (k == "tab" || k == "page") {
        const QString t = v.toLower();
        int i = -1;
        if      (t == "0" || t == "exposure" || t == "exp")  i = 0;
        else if (t == "1" || t == "view")                    i = 1;
        else if (t == "2" || t == "position" || t == "pos")  i = 2;
        else if (t == "3" || t == "stitching")               i = 3;
        if (i < 0)
            return false;
        showTab(i);
        return true;
    }

    // ---------------- 摄像头（View 页）自检 ----------------
    //   --set cam=on     开流并切到 ON
    //   --set cam=off    回到 OFF
    //   --set cam=error  强制「不可用」态（截图比对用，不起流）
    if (k == "cam" || k == "camera") {
        if (!m_viewPage)
            return false;
        const QString c = v.toLower();
        if (c == "on" || c == "1" || c == "true") {
            startCamera();
            return true;
        }
        if (c == "off" || c == "0" || c == "false") {
            stopCamera();
            return true;
        }
        if (c == "error" || c == "err") {
            stopCamera();
            m_viewPage->setCameraState(CameraOffPage::CamError);
            return true;
        }
        return false;
    }

    // Position 页自检：左侧竖条模式 + 角度选中（"angle=30" / "angle=B45"）
    if (k == "tilt" || k == "posmode") {
        if (!m_posPage)
            return false;
        m_posPage->setTiltMode(v.toInt());
        return true;
    }
    if (k == "angle" || k == "posangle") {
        if (!m_posPage)
            return false;
        QString s = v.trimmed();
        bool panelGiven = false;
        int panel = 0;
        if (s.startsWith(QLatin1Char('B'), Qt::CaseInsensitive)) { panel = 1; s = s.mid(1); panelGiven = true; }
        else if (s.startsWith(QLatin1Char('A'), Qt::CaseInsensitive)) { s = s.mid(1); panelGiven = true; }
        // 清空选择（设计稿初始态就是「都不选」）：angle=none 两栏都清，angle=Bnone 只清右栏
        const QString t = s.trimmed().toLower();
        if (t == "none" || t == "off" || t == "clear" || t == "empty" || t == "-1") {
            if (panelGiven) {
                m_posPage->setAngle(panel, -1);
            } else {
                m_posPage->setAngle(0, -1);
                m_posPage->setAngle(1, -1);
            }
            return true;
        }
        bool ok = false;
        const int deg = s.toInt(&ok);
        if (!ok)
            return false;
        m_posPage->setAngle(panel, deg);
        return true;
    }

    // Position 页自检：状态行三态（status=none / moving / reached）
    if (k == "status" || k == "posstatus") {
        if (!m_posPage)
            return false;
        const QString t = v.trimmed().toLower();
        if (t == "none" || t == "off" || t == "clear" || t == "idle" || t == "0") {
            m_posPage->setStatus(QString(), 0);
        } else if (t == "moving" || t == "move" || t == "1") {
            m_posPage->setStatus(QStringLiteral("System is moving....."), 1);
        } else if (t == "reached" || t == "done" || t == "2") {
            m_posPage->setStatus(QStringLiteral("System reached position"), 2);
        } else {
            return false;
        }
        return true;
    }

    if (k == "reset" || k == "default") {          // 全部回设计稿初值
        resetVars();
        return true;
    }

    // ---------------- 顶栏患者卡：体型 ----------------
    if (k == "patient") {
        const QString t = v.toUpper();
        if (t != "L" && t != "M" && t != "S")
            return false;
        if (m_card) m_card->setValue(t);
        return true;
    }

    // ---------------- 站位卡 ----------------
    if (k == "workspot") {
        const QString s = v.toLower().remove(QLatin1Char(' '));
        int idx = -1;
        if (s == "0" || s == "wallstand")                    idx = 0;
        else if (s == "1" || s == "table")                   idx = 1;
        else if (s == "2" || s == "free" || s == "freedetector") idx = 2;
        if (idx < 0)
            return false;
        if (m_ws) m_ws->setIndex(idx);
        return true;
    }

    // ---------------- 焦点卡（X 射线管大/小焦点）----------------
    //  底图烘焙的是小焦点；选大焦点后卡片上那个方形图标跟着换。
    if (k == "focal") {
        // 配置可能把行文案换掉，所以先按弹层里的实际文字查；查不到再走下面这套别名
        if (m_focal) {
            const int i = m_focal->popup()->indexOfText(v);
            if (i >= 0) { m_focal->setIndex(i); return true; }
        }
        const QString s = v.toLower().remove(QLatin1Char(' '));
        int idx = -1;
        if (s == "0" || s == "small" || s == "s"
            || s == QStringLiteral("小") || s == QStringLiteral("小焦点"))
            idx = 0;
        else if (s == "1" || s == "large" || s == "l"
                 || s == QStringLiteral("大") || s == QStringLiteral("大焦点"))
            idx = 1;
        if (idx < 0)
            return false;
        if (m_focal) m_focal->setIndex(idx);
        return true;
    }

    // ---------------- EL 320（Target exposure index）----------------
    //  档位表由 cards.json 的 targetEi.items 决定，这里必须**按弹层当前内容**查，
    //  不能再写死一份 int 数组 —— 否则配置一改，调试通道/主控下发的值和
    //  弹层里真正显示的就不是同一套（上一版就是两处各写一遍，改档位要改两个地方）。
    //  320 = 底图烘焙的那个值：只有配置里确实没有 320 时才当作「没选过」。
    if (k == "el" || k == "targetei") {
        if (!m_tgtEi)
            return false;
        DropPopup *pp = m_tgtEi->popup();

        if (v == QLatin1String("baked")) {       // 显式还原成底图那一帧
            m_tgtEi->resetToBaked();
            return true;
        }
        int idx = pp->indexOfText(v);            // 先按文字（"50" / "100" …）
        if (idx < 0) {                           // 再按数值（容忍 "050"、"64.0"）
            bool ok = false;
            const double d = v.toDouble(&ok);
            if (ok) {
                for (int i = 0; i < pp->count(); ++i) {
                    bool ok2 = false;
                    if (qFuzzyCompare(pp->itemText(i).toDouble(&ok2) + 1.0, d + 1.0) && ok2) {
                        idx = i;
                        break;
                    }
                }
            }
        }
        if (idx >= 0) {
            m_tgtEi->setIndex(idx);
            return true;
        }
        if (v == QLatin1String("320") || v == QLatin1String("-1")) {
            m_tgtEi->resetToBaked();
            return true;
        }
        return false;
    }

    // ---------------- 曝光参数组合 ----------------
    //  0 = kV−mAs（按钮印的就是它，也是首屏默认）/ 1 = kV−mAs−ms / 2 = kV−mA−ms
    //  baked = 回到设计稿那帧（卡片透出底图烘焙的 kV-mA，只给逐像素自检用）
    if (k == "exp" || k == "exposure") {
        const QString s = v.toLower();
        if (!m_expP)
            return false;
        DropPopup *pp = m_expP->popup();

        if (s == "-1" || s == "baked" || s == "init" || s == "kvma") {
            m_expP->setIndex(0);
            m_expP->resetToBaked();
            applyExposureMode(-1);
            return true;
        }

        // 先按「模式名 → 行文字」查：档位已由 cards.json 配置，顺序/数量都可能变，
        // 写死 0/1/2 会在用户重排后张冠李戴（比如把 kV−mA 挪到第 4 项）。
        int idx = -1;
        if      (s == "kvmas"    || s == "kv")  idx = pp->indexOfText(QStringLiteral("kV-mAs"));
        else if (s == "kvmas_ms" || s == "mas") idx = pp->indexOfText(QStringLiteral("kV-mAs-ms"));
        else if (s == "kvma_ms"  || s == "ma")  idx = pp->indexOfText(QStringLiteral("kV-mA-ms"));
        // 再退化到位置（0/1/2），兼容以前的脚本
        if (idx < 0 && (s == "0" || s == "1" || s == "2"))
            idx = s.toInt();
        if (idx < 0 && s == "kv")               // 配置里没有 kV−mAs 时按首项兜底
            idx = 0;
        if (idx < 0 || idx >= pp->count())
            return false;

        m_expP->setIndex(idx);              // 联动使能由信号走
        return true;
    }

    // ---------------- 三张开关卡 ----------------
    if (k == "aec") {                                // 同时决定 DEN 可不可调
        if (m_aecBtn) m_aecBtn->setOn(varTruthy(v));
        return true;
    }
    if (k == "center" || k == "centering") {          // 准星对中
        if (m_center) m_center->setOn(varTruthy(v));
        return true;
    }
    if (k == "follow") {                             // 跟随
        if (m_follow) m_follow->setOn(varTruthy(v));
        return true;
    }

    // ---------------- AEC 场域（可多选）----------------
    //  0x1 左野 / 0x2 右野 / 0x4 下野；也接受 left,right 这种写法，
    //  多个用 + 或 | 连起来；给 none 表示一个都不选。
    if (k == "aecfield" || k == "field") {
        const QString s = v.toLower().remove(QLatin1Char(' '));
        int mask = 0;
        if (s == "none" || s == "0") {
            mask = 0;
        } else if (s.startsWith(QLatin1String("0x"))
                   || (!s.contains(QLatin1Char('+'))
                       && !s.contains(QLatin1Char('|'))
                       && s.toInt() > 0)) {
            bool ok = false;
            mask = s.startsWith(QLatin1String("0x")) ? s.mid(2).toInt(&ok, 16)
                                                     : s.toInt(&ok, 10);
            if (!ok) return false;
        } else {
            const QStringList parts = QString(s).replace(QLatin1Char('|'),
                                                         QLatin1Char('+'))
                                      .split(QLatin1Char('+'), Qt::SkipEmptyParts);
            for (const QString &p : parts) {
                if      (p == "left"   || p == "l") mask |= 0x1;
                else if (p == "right"  || p == "r") mask |= 0x2;
                else if (p == "bottom" || p == "b") mask |= 0x4;
                else return false;
            }
        }
        if (m_aecField) m_aecField->setMask(mask);
        return true;
    }

    // ---------------- 五个参数步进器 ----------------
    //  联动照旧：mA/ms → 重算 mAs；mAs → 反算 ms（mA 优先）。
    {
        StepperCard *st = nullptr;
        if      (k == "kv")  st = m_stKv;
        else if (k == "ma")  st = m_stMa;
        else if (k == "mas") st = m_stMas;
        else if (k == "ms")  st = m_stMs;
        else if (k == "den") st = m_stDen;
        if (st) {
            bool ok = false;
            const double d = v.toDouble(&ok);
            if (!ok)
                return false;
            // 不对齐步长网格：主控给的是权威值（ms 可能是 5.6），
            // 按 step=1 对齐会变成 6，与真机平板显示不一致。
            st->setValue(d, /*snapGrid=*/false);
            return true;
        }
    }

    // ---------------- 顶栏 / 缩略图 ----------------
    //  姓名可以不带逗号；部位名直接给整串（"Chest PA" / "Knee AP"）
    if (k == "name") {
        if (m_header) m_header->setName(v);
        return true;
    }
    if (k == "part") {
        if (m_caption) m_caption->setPart(v);
        return true;
    }

    // ---------------- 读数区（Req.SID / 身高 / 角度 / 支架高）----------------
    //  只改数字，单位固定；以后网口下发的是这几个量。
    if (k == "sid" || k == "reqsid") {
        if (m_info) m_info->setNum(0, v);
        return true;
    }
    if (k == "pheight" || k == "height") {
        if (m_info) m_info->setNum(1, v);
        return true;
    }
    if (k == "angle") {
        if (m_info) m_info->setNum(2, v);
        return true;
    }
    if (k == "standh" || k == "standheight") {
        if (m_info) m_info->setNum(3, v);
        return true;
    }

    // ---------------- 底栏 ----------------
    if (!m_footer)
        return false;

    //  探测器/限束器开口尺寸：横向 × 纵向，两个数可以不同。
    //    det=35        两个方向都设成 35（正方形，"29 x 29 cm"）
    //    det=35x43     一次给横向 35、纵向 43
    //    detw=35       只改横向，纵向不动
    //    deth=43       只改纵向，横向不动
    if (k == "det" || k == "detsize" || k == "detw" || k == "deth") {
        const QStringList parts = v.split(QLatin1Char('x'), Qt::SkipEmptyParts);
        bool ok = false;
        const int a = parts.value(0).toInt(&ok);
        if (!ok || a <= 0) return false;
        int b = a;
        if (parts.size() > 1) {
            b = parts.value(1).toInt(&ok);
            if (!ok || b <= 0) return false;
        }
        if (k == "detw")      m_footer->setDetW(a);
        else if (k == "deth") m_footer->setDetH(a);
        else                  m_footer->setDetSize(a, parts.size() > 1 ? b : a);
        return true;
    }
    //  滤过：整串文字，"0.9 Cu + 1 Al" / "none" 之类
    if (k == "filt" || k == "filter") {
        m_footer->setFilter(v);
        return true;
    }
    //  温度：≥70 自动转警示色（阈值见 statusstrip.cpp 的 kTempWarn）
    if (k == "tempa" || k == "temp") {
        bool ok = false; const int n = v.toInt(&ok);
        if (!ok) return false;
        m_footer->setTemp(0, n);
        return true;
    }
    if (k == "tempb") {
        bool ok = false; const int n = v.toInt(&ok);
        if (!ok) return false;
        m_footer->setTemp(1, n);
        return true;
    }
    if (k == "hu") {
        bool ok = false; const int n = v.toInt(&ok);
        if (!ok) return false;
        m_footer->setHu(n);
        return true;
    }
    //  电池：0..4 = 电量格数（0 空 / 1 / 2 / 3 / 4 满）；chg = 充电（闪电）；
    //        low = 低电（橙色 1 格）；empty = 0%
    if (k == "bata" || k == "bat" || k == "batb") {
        const int idx = (k == "batb") ? 1 : 0;
        const QString s = v.toLower();
        if (s == "chg" || s == "charging") { m_footer->setBattery(idx, 4, true);  return true; }
        if (s == "low")                    { m_footer->setBatteryLow(idx);       return true; }
        if (s == "empty")                  { m_footer->setBattery(idx, 0, false); return true; }
        bool ok = false; const int n = v.toInt(&ok);
        if (!ok) return false;
        m_footer->setBattery(idx, n, false);
        return true;
    }
    //  哪只球管在用：A / B（另一个的方块与箭头置灰）
    if (k == "tube") {
        const QString s = v.toUpper();
        if (s != "A" && s != "B") return false;
        m_footer->setTubeActive(s == "B" ? 1 : 0);
        return true;
    }
    //  栅格：0 正常 / 2 告警（图标右边换感叹号）
    if (k == "grid" || k == "grida" || k == "gridb") {
        const int idx = (k == "gridb") ? 1 : 0;
        bool ok = false; const int n = v.toInt(&ok);
        if (!ok) return false;
        m_footer->setGrid(idx, n);
        return true;
    }
    //  探测器 A/B：ready（蓝，就绪）/ off（灰，未就绪）
    if (k == "deta" || k == "detb") {
        const int idx = (k == "detb") ? 1 : 0;
        const QString s = v.toLower();
        const bool ready = (s == "ready" || s == "1" || s == "on");
        const bool off   = (s == "off" || s == "0" || s == "none" || s == "no");
        if (!ready && !off) return false;
        m_footer->setDetector(idx, ready ? 0 : 1);
        return true;
    }
    //  准直器：on（白，正常）/ off（灰，关）/ warn（橙，告警）
    if (k == "collim" || k == "coll") {
        const QString s = v.toLower();
        if (s == "on" || s == "1" || s == "ok")        m_footer->setCollim(0);
        else if (s == "off" || s == "0" || s == "close") m_footer->setCollim(1);
        else if (s == "warn" || s == "2" || s == "err") m_footer->setCollim(2);
        else return false;
        return true;
    }
    //  滤过状态（只换图标色，文字走 filt）：ok（白）/ warn（橙）
    if (k == "filts" || k == "filtst") {
        const QString s = v.toLower();
        if (s == "ok" || s == "0" || s == "on")         m_footer->setFiltState(0);
        else if (s == "warn" || s == "1" || s == "2")  m_footer->setFiltState(2);
        else return false;
        return true;
    }
    //  HU 左侧温度计（探测器温度）：数字进，≥70 转红
    if (k == "hutemp" || k == "dettemp" || k == "tempd") {
        bool ok = false; const int n = v.toInt(&ok);
        if (!ok) return false;
        m_footer->setDetTemp(n);
        return true;
    }
    //  剂量环：ok（绿）/ warn（红）
    if (k == "ring" || k == "dose") {
        const QString s = v.toLower();
        if (s == "ok" || s == "0" || s == "on")         m_footer->setRing(0);
        else if (s == "warn" || s == "1" || s == "2")  m_footer->setRing(2);
        else return false;
        return true;
    }

    // ---------------- 患者信息面板 ----------------
    if (k == "patinfo") {
        const QString s = v.toLower();
        if (!m_patInfo) return false;
        if (s == "open" || s == "1" || s == "on")        m_patInfo->openPanel();
        else if (s == "close" || s == "0" || s == "off") m_patInfo->closePanel();
        else if (s == "toggle")                          m_patInfo->togglePanel();
        else return false;
        return true;
    }

    return false;
}

// 全部回到「设计稿第一帧」：卡片上的值、开关、场域、步进器、面板一次复位。
// 用途：调试时改了十几个变量看不清了，发一条 reset 就回到原样。
void MainWindow::resetVars()
{
    if (m_patInfo) m_patInfo->closePanel();
    if (m_card)    m_card->setValue(QStringLiteral("M"));
    if (m_ws)      m_ws->setIndex(0);
    if (m_info)    m_info->resetAll();
    if (m_footer)  m_footer->resetAll();
    if (m_header)  m_header->resetAll();
    if (m_caption) m_caption->resetAll();
    if (m_center)  m_center->setOn(false);
    if (m_follow)  m_follow->setOn(false);
    if (m_aecBtn)  m_aecBtn->setOn(false);
    if (m_aecField) m_aecField->setMask(0x2);        // 右野，与弹层一致
    if (m_focal) { m_focal->setIndex(0); m_focal->resetToBaked(); }
    if (m_tgtEi) { m_tgtEi->setIndex(0); m_tgtEi->resetToBaked(); }
    if (m_expP)  { m_expP->setIndex(0); }           // 首屏模式 kV−mAs（按钮印的就是它）
    // 设计稿的 Position 页所有角度按钮都是「未选中」态 → 复位要清掉选择与状态行
    if (m_posPage) {
        m_posPage->setAngle(0, -1);
        m_posPage->setAngle(1, -1);
        m_posPage->setStatus(QString(), 0);
    }
    applyExposureMode(0);

    m_syncing = true;                                // 别让 setValue 互相打环
    if (m_stKv)  m_stKv->setValue(75.0);
    if (m_stMa)  m_stMa->setValue(80.0);
    if (m_stMs)  m_stMs->setValue(2.0);
    if (m_stMas) m_stMas->setValue(80.0 * 2.0 / 1000.0);   // = 0.16 → 对齐 0.1 → 0.2
    if (m_stDen) { m_stDen->setValue(0.0); m_stDen->setOn(false); }
    m_syncing = false;
    qDebug("vars reset -> 设计稿初值");
}

QStringList MainWindow::varKeys()
{
    return QStringList {
        QStringLiteral("patient   = L | M | S                        体型（顶栏 M 卡）"),
        QStringLiteral("workspot  = wallstand | table | free          站位卡"),
        QStringLiteral("focal     = small | large                     焦点：小焦点 / 大焦点"),
        QStringLiteral("el        = 档位值（默认 50|64|80|100|125）| 320(baked)  EL 曝光指数卡"),
        QStringLiteral("exp       = kvmas | kvmas_ms | kvma_ms | baked    曝光参数组合（首屏 kV−mAs）"),
        QStringLiteral("aec       = 0 | 1                             AEC 开关（连带 DEN 使能）"),
        QStringLiteral("center    = 0 | 1                             准星对中"),
        QStringLiteral("follow    = 0 | 1                             跟随"),
        QStringLiteral("aecfield  = left+right | right | 0x1..0x7      AEC 场域（可多选）"),
        QStringLiteral("kv        = 40..150                           步进器 kV"),
        QStringLiteral("ma        = 10..500                           步进器 mA（联动 mAs）"),
        QStringLiteral("mas       = 0.1..500                          步进器 mAs（反算 ms）"),
        QStringLiteral("ms        = 1..1000                           步进器 ms（联动 mAs）"),
        QStringLiteral("den       = -8..8                            步进器 DEN（有符号档位，0 为默认）"),
        QStringLiteral("----- 顶栏 / 缩略图 -----"),
        QStringLiteral("name      = \"Ahmed, Ayesha\"                  患者姓名（过长自动省略）"),
        QStringLiteral("part      = \"Chest PA\"                      缩略图部位名（居中）"),
        QStringLiteral("----- 读数区 -----"),
        QStringLiteral("sid       = 115                                Req.SID（cm）"),
        QStringLiteral("pheight   = 75                                 患者身高（cm）"),
        QStringLiteral("angle     = 110                                投照角度（°）"),
        QStringLiteral("standh    = 175                                支架高度（cm）"),
        QStringLiteral("----- 底栏 -----"),
        QStringLiteral("det       = 35 | 35x43                     开口尺寸（横向 × 纵向，cm）"),
        QStringLiteral("detw/deth = 35 | 43                         只改横向 / 只改纵向"),
        QStringLiteral("filt      = \"0.9 Cu + 1 Al\"                    滤过组合"),
        QStringLiteral("tempa/tempb = 48                               球管温度（≥70 转警示色）"),
        QStringLiteral("hu        = 0                                  HU 百分比"),
        QStringLiteral("bata/batb = 0|1|2|3|4 | chg | low | empty      电池：电量格 / 充电 / 低电 / 空"),
        QStringLiteral("tube      = A | B                              当前球管（另一只置灰）"),
        QStringLiteral("grid/gridb= 0 | 2                              栅格状态（2 = 告警感叹号）"),
        QStringLiteral("deta/detb = ready | off                       探测器 A/B（蓝=就绪 灰=未就绪）"),
        QStringLiteral("collim    = on | off | warn                   准直器（白=开 灰=关 橙=告警）"),
        QStringLiteral("filts     = ok | warn                         滤过图标（橙=告警，文字走 filt）"),
        QStringLiteral("hutemp    = 48                                HU 左侧温度计（≥70 转红）"),
        QStringLiteral("ring      = ok | warn                         剂量环（绿=正常 红=告警）"),
        QStringLiteral("patinfo   = open | close | toggle             患者信息面板"),
        QStringLiteral("reset     = 1                                 全部回设计稿初值"),
    };
}

void MainWindow::on_pushButton_clicked()
{
    QApplication::quit();
}

// 曝光参数组合 → 哪几个步进器可调。
//   弹层（Exposure parameters）三项：0 "kV−mAs" / 1 "kV−mAs−ms" / 2 "kV−mA−ms"
//   首屏就是 0（卡片/按钮上印的 kV−mAs）。idx = -1 只是设计稿那帧的还原态
//   （卡片透出底图烘焙的 kV-mA），仅给逐像素自检用，正常界面不会落到这里。
//   DEN 不在这里管 —— 它只跟 AEC 开关走。
// 曝光参数组合 → 哪几个步进器可调。
//   弹层三项默认：0 "kV−mAs" / 1 "kV−mAs−ms" / 2 "kV−mA−ms"；
//   底图烘焙的初始态是 "kV-mA"（弹层里没有这一项），用 idx = -1 表示。
//   DEN 不在这里管 —— 它只跟 AEC 开关走。
//
// ⚠ 档位列表已由 cards.json 配置，**位置不再等于模式**（用户可以把顺序换了、
//   或加一个 "kV−mA"），所以这里必须按行文字解析，不能写死 case 0/1/2。
static void expEnablesFrom(const QString &row, bool *ma, bool *mas, bool *ms)
{
    *ma = *mas = *ms = false;
    QString norm = row;
    norm.replace(QChar(0x2212), QLatin1Char('-'));      // 长 minus 统一成连字符
    const QStringList parts = norm.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        const QString k = p.trimmed().toLower();
        if      (k == QLatin1String("mas")) *mas = true;
        else if (k == QLatin1String("ma"))  *ma  = true;
        else if (k == QLatin1String("ms"))  *ms  = true;
        // "kV" 不管：管电压恒可调
    }
    // 一个都没认出来（配置写了别的语言/错字）→ 保守退回底图初值那套 kV + mA
    if (!*ma && !*mas && !*ms)
        *ma = true;
}

void MainWindow::applyExposureMode(int idx)
{
    bool ma = false, mas = false, ms = false;
    if (idx < 0 || !m_expP) {
        ma = true;                       // -1 = 底图烘焙的 kV-mA
    } else {
        expEnablesFrom(m_expP->popup()->itemText(idx), &ma, &mas, &ms);
    }
    if (m_stMa)  m_stMa->setOn(ma);
    if (m_stMas) m_stMas->setOn(mas);
    if (m_stMs)  m_stMs->setOn(ms);
    qDebug("exposure mode %d '%s' -> kV=1 mA=%d mAs=%d ms=%d",
           idx, idx < 0 ? "baked kV-mA" : qPrintable(m_expP->popup()->itemText(idx)),
           ma, mas, ms);
}

// =====================================================================
//  接主控链路
// ---------------------------------------------------------------------
//  这一层刻意写得很薄：不改任何界面绘制代码，只做三件事 ——
//    ① 下行帧 → setVar()（就是一开始留的那个口子）
//    ② 步进器点 ± → sendStep(±1)
//    ③ 「300ms 没回音」→ 该侧变灰 + 停连发
//  为什么不在本地预测数值：抓包实测主控是查表跳档（mA 180→200→220…），
//  本地按固定步长算出来的数一定是错的，等广播反而只跳一次。
// =====================================================================
void MainWindow::attachLink(DrLink *link)
{
    m_link = link;
    if (!link)
        return;

    // 五个步进器：切远程驱动 + 接上行意图 + 接「到底」回执。
    // DEN 例外：它只在 AEC 打开时可调，可调性由 onChanged 管，不在这里改。
    struct Wiring { StepperCard *st; DrParam p; const char *name; };
    const Wiring ws[5] = {
        { m_stKv,  DrParam::Kv,  "kV"  },
        { m_stMa,  DrParam::Ma,  "mA"  },
        { m_stMs,  DrParam::Ms,  "ms"  },
        { m_stMas, DrParam::Mas, "mAs" },
        { m_stDen, DrParam::Den, "DEN" },
    };
    for (const Wiring &w : ws) {
        if (!w.st)
            continue;
        StepperCard *st = w.st;
        const DrParam p = w.p;                    // C++11：先落局部再按值捕获
        st->setRemoteDriven(true);
        connect(st, &StepperCard::stepped, this, [this, p](int dir, double) {
            if (m_link) m_link->sendStep(p, dir);
        });
        connect(link, &DrLink::stepAtLimit, this, [st, p](DrParam q, int dir) {
            if (q == p) st->noteLimit(dir);       // 主控没动 → 这一侧到底
        });
        // 广播把值改回来了 → 说明还能继续走，把「到底」标记清掉由 setValue 自己做
    }

    // 焦点 / EL / 曝光模式：这三个不是 ±1 模型，发绝对值，本轮先不接
    //（用户确认：SID / 光野 / 滤过 / 部位 / 机架 都是只读，不做可控）

    qDebug("link attached: %s", qPrintable(link->stateText()));
}

// 自检：程序化点击步进器的 ± 格子。
// 走的是 QMouseEvent → mousePressEvent → applyStep → stepped 的真实路径，
// 所以能验证「按钮按下 → 上行帧」整条线，而不用手指去戳屏。
//   + 格子在控件右侧 92px 宽，− 格子在左侧 92px 宽（见 layout_profile）
void MainWindow::debugStep(const QString &key, int dir)
{
    StepperCard *st = nullptr;
    const QString k = key.toLower();
    if      (k == QLatin1String("kv"))  st = m_stKv;
    else if (k == QLatin1String("ma"))  st = m_stMa;
    else if (k == QLatin1String("ms"))  st = m_stMs;
    else if (k == QLatin1String("mas")) st = m_stMas;
    else if (k == QLatin1String("den")) st = m_stDen;
    if (!st || !st->isOn()) {
        qWarning("sim-step: %s 不可用（key 不认识或该步进器置灰）", qPrintable(key));
        return;
    }
    const QRect r = st->rect();
    const QPoint pt = dir > 0 ? QPoint(r.right() - r.width() / 10, r.height() / 2)
                              : QPoint(r.left()  + r.width() / 10, r.height() / 2);
    QMouseEvent pr(QEvent::MouseButtonPress, pt, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(st, &pr);
    QMouseEvent rl(QEvent::MouseButtonRelease, pt, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(st, &rl);
    qDebug("sim-step %s %+d -> value=%g remote=%d", qPrintable(key), dir,
           st->value(), int(st->isRemoteDriven()));
}
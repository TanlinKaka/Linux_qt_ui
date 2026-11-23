// ============================================================================
//  InfoStrip / FooterBar 实现 —— 读数区 + 底栏的「盖色块重画」
//
//  所有坐标都是设计稿（1200x1920）绝对坐标减去各自 band 的左上角，
//  即「相对控件坐标」。量测来源见 .workbuddy/memory/2026-11-11.md。
//
//  底色 #171717 / 分隔线 #2E2E2E / 主文字 #D1D1D1 / 次文字 #8C8C8C
//  都是设计稿直采，不是估的。
// ============================================================================
#include "statusstrip.h"

#include <QPainter>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPaintEvent>
#include <QDebug>

// ---------------------------------------------------------------------------
//  字体：板上 buildroot 没有 Ubuntu，必须内嵌（与 patientcard.cpp 同一套 ttf，
//  重复注册返回同一个 id，互不影响）
// ---------------------------------------------------------------------------
static QString g_stFamily;
static const QString &stFontFamily()
{
    if (g_stFamily.isEmpty()) {
        const int id = QFontDatabase::addApplicationFont(
                    QStringLiteral(":/mselect/Ubuntu-R.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral(":/mselect/Ubuntu-B.ttf"));
        const QStringList fams = QFontDatabase::applicationFontFamilies(id);
        g_stFamily = fams.isEmpty() ? QStringLiteral("Ubuntu") : fams.first();
    }
    return g_stFamily;
}

static QFont stFont(int px, QFont::Weight w = QFont::Normal)
{
    QFont f(stFontFamily());
    f.setPixelSize(px);
    f.setWeight(w);
    f.setHintingPreference(QFont::PreferFullHinting);
    return f;
}

// 按「墨迹左端 + 基线」定位，和步进器同一套（boundingRect 的 top 不可靠）
static void drawInk(QPainter &p, int inkLeft, int baseline,
                    const QString &s, const QFont &f, const QColor &c,
                    qreal track = 0.0)
{
    QFont ff = f;
    if (track != 0.0)
        ff.setLetterSpacing(QFont::AbsoluteSpacing, track);
    QFontMetricsF fm(f);
    const QRectF br = fm.tightBoundingRect(s);
    p.setFont(ff);
    p.setPen(c);
    p.drawText(QPointF(qRound(inkLeft - br.left()), baseline), s);
}

// 按「墨迹右端 + 基线」定位。
// 为什么要右对齐：设计稿字体的「1」比 Ubuntu 窄一大截（"110" 只有 59 宽，
// Ubuntu 42px 要 69），数值一旦含 1 就会变宽 7~13px，左对齐会压住右边的单位。
// 钉住右端、让多出来的宽度往左长，单位位置就永远不动。
static void drawInkRight(QPainter &p, int inkRight, int baseline,
                         const QString &s, const QFont &f, const QColor &c,
                         qreal track = 0.0)
{
    QFont ff = f;
    if (track != 0.0)
        ff.setLetterSpacing(QFont::AbsoluteSpacing, track);
    QFontMetricsF fm(f);
    const QRectF br = fm.tightBoundingRect(s);
    // 注意：fm 是不带字距的，而实际绘制带字距 —— 墨迹右端会多出 track*(n-1)。
    // 不减掉这一截，右对齐就会整体右移，字距越负偏得越多（"110" 曾偏 11px）。
    const qreal extra = (s.size() > 1) ? track * (s.size() - 1) : 0.0;
    p.setFont(ff);
    p.setPen(c);
    p.drawText(QPointF(qRound(inkRight - br.right() - extra), baseline), s);
}

// 设计稿字体的「1」比 Ubuntu 窄 5.5px/个（量测："115" 55 vs 67、"110" 59 vs 68、
// "175" 61 vs 67），其余数字两边一样宽（"75" 44 vs 45）。所以只对含 1 的数值
// 收字距，别的一律不收 —— 否则 "75" 会被压扁。
static qreal numTrack(const QString &s)
{
    const int ones = s.count(QLatin1Char('1'));
    if (ones == 0 || s.size() < 2)
        return 0.0;
    return -5.5 * ones / (s.size() - 1);
}

static const QColor kBg      (0x17, 0x17, 0x17);
static const QColor kSep     (0x2E, 0x2E, 0x2E);
static const QColor kInkMain (0xD1, 0xD1, 0xD1);
static const QColor kInkDim  (0x8C, 0x8C, 0x8C);

// 字号：由 _probe_status 反查（按设计稿墨迹高度定字号，宽度靠字距补）
static const int kInfoLabelPx = 40;
static const int kInfoValuePx = 42;
static const int kInfoUnitPx  = 44;
static const int kFootPx      = 30;
// 字距：Ubuntu 比设计稿字体紧，这几个字符串要撑开才够宽
static const qreal kInfoLabelTrack = 2.17;  // "Req.SID" 137 → 151（设计稿 151）
static const qreal kInfoUnitTrack  = 1.0;   // "cm" 53 → 54

// 联动阈值（待产品确认，见 界面功能说明.md）
static const int kTempWarn = 70;   // ≥ 70°C 温度柱转警示色

// ============================================================================
//  读数区 —— 竖屏 1×4
//    band = 设计稿 (280,425) 896x128
//    上边线 y425..426 / 竖线 x724、x950（y436..551，色 #2E2E2E）
//    四组值墨迹左端 324 / 562 / 801 / 999，基线 y531
// ============================================================================
static const QRect kInfoBand(280, 425, 896, 128);

struct InfoText {
    int which;       // 0..3 = 值；4..7 = 单位；-1 = 静态标签
    int pos;         // 左端 或 右端（看 right 标志）
    int baseline;
    const char *fixed;
    bool right;
};
// 数值钉右端（378 / 605 / 859 / 1059），单位钉左端，标签左对齐
// 基线：标签 48（墨迹顶 441 + cap 高）、值 106、单位 107（实测都差 1px，已补）
static const InfoText kPorInfoText[] = {
    { -1,  31,  48, "Req.SID", false },
    {  0,  99, 106, nullptr, true  },   {  4, 111, 107, nullptr, false },
    {  1, 326, 106, nullptr, true  },   {  5, 338, 107, nullptr, false },
    {  2, 580, 106, nullptr, true  },   {  6, 582, 107, nullptr, false },
    {  3, 780, 106, nullptr, true  },   {  7, 792, 107, nullptr, false },
};

struct IconAt { int x, y; const char *slot; };
static const IconAt kPorInfoIcons[] = {
    {  316,  16, "rdH"   },
    {  540,  22, "rdAng" },
    {  763,  20, "rdSt"  },
};

// ---------------------------------------------------------------------------
//  读数区 —— 横屏 1920x1200 是 2x2（不是竖屏的 1x4）
//    上排：Req.SID 115cm / AID 75cm      下排：110° / 175cm
//    量测自 Resource/PHILIPS_TUI/曝光备份.png（2026-11-20，tools/scan_ui.py）
//    band (32,673) 776x276；横线 y142 全宽 2px；竖线 x387 y160..275
//    标签墨迹 x147 / 值右端 214·605 / 单位左端 227·618（下排 257·613 / 626）
// ---------------------------------------------------------------------------
static const QRect kLandInfoBand(32, 673, 776, 276);
static const InfoText kLandInfoText[] = {
    { -1, 115,  53, "Req.SID", false },   // 墨迹 x147..297 / y694..728
    {  0, 182, 106, nullptr, true  },     // 115 -> 147..214
    {  4, 195, 107, nullptr, false },     // cm  -> 227..280
    {  1, 573, 106, nullptr, true  },     // 75  -> 562..605
    {  5, 586, 107, nullptr, false },     // cm  -> 618..671
    {  2, 211, 252, nullptr, true  },     // 110 -> 185..243（宽 59，与设计稿一致）
    {  6, 214, 252, nullptr, false },     // °   -> 246..257（上标，别漏）
    {  3, 581, 252, nullptr, true  },     // 175 -> 553..613
    {  7, 594, 252, nullptr, false },     // cm  -> 626..679
};
static const IconAt kLandInfoIcons[] = {
    { 564,  16, "rdH"   },   // abs (596,689)
    { 172, 169, "rdAng" },   // abs (204,842)
    { 565, 167, "rdSt"  },   // abs (597,840)
};

InfoStrip::InfoStrip(QWidget *page) : QWidget(page)
{
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);
    resetAll();
}

void InfoStrip::applyProfile(bool landscape)
{
    m_land = landscape;
    // 横屏读数区是 2x2，两套 band 各自的坐标系不同，控件跟着换位置和大小
    setGeometry(landscape ? kLandInfoBand : kInfoBand);
    show();
}

void InfoStrip::setNum(int i, const QString &num)
{
    if (i < 0 || i > 3) return;
    m_num[i] = num.trimmed();
    update();
}

void InfoStrip::setUnit(int i, const QString &u)
{
    if (i < 0 || i > 3) return;
    m_unit[i] = u;
    update();
}

void InfoStrip::resetAll()
{
    setNum(0, QStringLiteral("115"));   setUnit(0, QStringLiteral("cm"));
    setNum(1, QStringLiteral("75"));    setUnit(1, QStringLiteral("cm"));
    setNum(2, QStringLiteral("110"));   setUnit(2, QStringLiteral("\u00B0"));
    setNum(3, QStringLiteral("175"));   setUnit(3, QStringLiteral("cm"));
}

void InfoStrip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    p.fillRect(rect(), kBg);

    // 分隔线：竖屏一条上边线 + 两条竖线；横屏一条横线 + 一条竖线
    if (m_land) {
        p.fillRect(QRect(0, 142, width(), 2), kSep);
        p.fillRect(QRect(387, 160, 2, 116), kSep);
    } else {
        p.fillRect(QRect(0, 0, width(), 2), kSep);
        p.fillRect(QRect(444, 11, 2, 116), kSep);
        p.fillRect(QRect(670, 11, 2, 116), kSep);
    }

    const IconAt *icons = m_land ? kLandInfoIcons : kPorInfoIcons;
    const int nIcons = int(sizeof(kPorInfoIcons) / sizeof(IconAt));
    for (int i = 0; i < nIcons; ++i) {
        const IconAt &ic = icons[i];
        QPixmap pm(QStringLiteral(":/mselect/%1.png").arg(QLatin1String(ic.slot)));
        if (!pm.isNull())
            p.drawPixmap(ic.x, ic.y, pm);
        else
            qWarning("!! 读数区图标缺失: %s", ic.slot);
    }

    const InfoText *tab = m_land ? kLandInfoText : kPorInfoText;
    const int nTab = m_land ? int(sizeof(kLandInfoText) / sizeof(InfoText))
                            : int(sizeof(kPorInfoText) / sizeof(InfoText));
    const QFont fLbl = stFont(kInfoLabelPx);
    const QFont fVal = stFont(kInfoValuePx, QFont::Bold);
    const QFont fUni = stFont(kInfoUnitPx);
    for (int i = 0; i < nTab; ++i) {
        const InfoText &t = tab[i];
        if (t.which < 0) {
            drawInk(p, t.pos, t.baseline, QLatin1String(t.fixed), fLbl, kInkDim,
                    kInfoLabelTrack);
        } else if (t.which < 4) {
            const QString &s = m_num[t.which];
            drawInkRight(p, t.pos, t.baseline, s, fVal, kInkMain, numTrack(s));
        } else {
            drawInk(p, t.pos, t.baseline, m_unit[t.which - 4], fUni, kInkDim,
                    kInfoUnitTrack);
        }
    }
}

// ============================================================================
//  底栏 —— band = 设计稿 (0,1756) 1200x164
//    两行：行1 基线 y1813、行2 基线 y1877（相对 57 / 121）
//    三条竖线 x160 / x433 / x897（y1780..1891 → 相对 24..135）
// ============================================================================
static const QRect kFootBand(0, 1756, 1200, 164);

// 坐标由 tools/mk_status_icons.py 直接吐出，改素材后重跑脚本粘贴即可
static const IconAt kPorFootIcons[] = {
    {   25,  28, "ftDetA"   },
    {   96,  24, "ftGridA"  },
    {   30,  90, "ftDetB"   },
    {   96,  88, "ftGridB"  },
    {  180,  26, "ftCollim" },
    {  179,  90, "ftFilt"   },
    {  457,  31, "ftBA"     },
    {  523,  32, "ftArrA"   },
    {  595,  35, "ftBatA"   },
    {  668,  26, "ftTempA"  },
    {  457,  95, "ftBB"     },
    {  523,  96, "ftArrB"   },
    {  595,  90, "ftBatB"   },
    {  668,  90, "ftTempB"  },
    {  927,  58, "ftTempHU" },
    { 1114,  53, "ftRing"   },
};

// 基线/字距都是「渲染一次 → 量墨迹 → 补偏移」得到的，track 各串不同：
// Ubuntu 与设计稿字体的字宽比例不是线性关系，长串要更大的正字距才够宽。
struct FootText { int which; int inkLeft; int baseline; qreal track; int align; };
static const FootText kPorFootText[] = {
    // align 0 = 按墨迹左端定位（inkLeft 是左端）；1 = 按墨迹右端定位（inkLeft 是右端）
    { 0, 243,  58, 0.8,  0 },   // 29 x 29 cm
    { 1, 243, 121, 0.16, 0 },   // 0.9 Cu + 1 Al（这串 Ubuntu 本身就宽，字距必须压到近 0）
    { 2, 733,  58, 2.0,  0 },   // 球管 A 温度
    { 3, 733, 122, 2.0,  0 },   // 球管 B 温度
    // HU 值会变长（"HU 0%" → "HU 100%"），钉住右端免得压到右边的剂量环（左端 1114）
    { 4, 1093, 88, 2.0,  1 },   // HU 0%
};

// ---------------------------------------------------------------------------
//  底栏 —— 横屏 1920x1200 是「四段」，竖线 x234 / 716 / 1338
//    量测自 Resource/PHILIPS_TUI/曝光备份.png（2026-11-20，tools/scan_ui.py）
//    内容墨迹 y1064..1175 → band 取 (0,1064) 1920x136
//    组1 探测器×2 + 滤线栅×2 | 组2 限束器/滤过 + 两行文字
//    组3 A/B 灯·滤·电·温 + 48°C | 组4 HU + 剂量环
// ---------------------------------------------------------------------------
static const QRect kFootBandLand(0, 1064, 1920, 136);
static const int   kFootSepH = 112;      // 竖线高度（设计稿 y1064..1175）

static const IconAt kLandFootIcons[] = {
    {   49,   4, "ftDetA"   },   // abs 1068
    {  120,   0, "ftGridA"  },   // 1064
    {   49,  66, "ftDetB"   },   // 1130
    {  120,  64, "ftGridB"  },   // 1128
    {  321,   2, "ftCollim" },   // 1066
    {  321,  66, "ftFilt"   },   // 1130
    {  828,   7, "ftBA"     },   // 1071
    {  894,   8, "ftArrA"   },   // 1072
    {  966,  11, "ftBatA"   },   // 1075
    { 1039,   2, "ftTempA"  },   // 1066
    {  828,  71, "ftBB"     },   // 1135
    {  894,  72, "ftArrB"   },   // 1136
    {  966,  66, "ftBatB"   },   // 1130
    { 1039,  66, "ftTempB"  },   // 1130
    { 1448,  34, "ftTempHU" },   // 1098
    { 1786,  29, "ftRing"   },   // 1093
};

// 字号与竖屏一致（墨迹高 21 = 30px 字），字距照抄竖屏
static const FootText kLandFootText[] = {
    { 0,  385,  34, 0.8,  0 },   // 29 x 29 cm    （墨迹 y1077..1097）
    { 1,  385,  98, 0.16, 0 },   // 0.9 Cu + 1 Al （y1141..1161）
    { 2, 1104,  34, 2.0,  0 },   // 球管 A 温度 48°C
    { 3, 1104,  98, 2.0,  0 },   // 球管 B 温度 48°C
    { 4, 1518,  66, 2.0,  0 },   // HU 0%（y1107..1129）
};

FooterBar::FooterBar(QWidget *page) : QWidget(page)
{
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);
    resetAll();
}

void FooterBar::applyProfile(bool landscape)
{
    m_land = landscape;
    // 横屏底栏是四段版式（左中右 + 剂量环），band 与竖屏不同
    setGeometry(landscape ? kFootBandLand : kFootBand);
    show();
}

void FooterBar::setTxt(int i, const QString &s)
{
    if (i < 0 || i > 4) return;
    m_txt[i] = s;
    update();
}

void FooterBar::setState(const QString &slot, const QString &st)
{
    if (m_st.value(slot) == st) return;
    m_st[slot] = st;
    update();
}

// 图标取用：状态图缺失时自动退回主状态，不会画空
QPixmap FooterBar::pix(const QString &slot) const
{
    const QString st = m_st.value(slot);
    const QString key = st.isEmpty() ? slot : slot + QLatin1Char('_') + st;
    auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) return it.value();

    QPixmap pm(QStringLiteral(":/mselect/%1.png").arg(key));
    if (pm.isNull() && !st.isEmpty())
        pm.load(QStringLiteral(":/mselect/%1.png").arg(slot));
    if (pm.isNull())
        qWarning("!! 底栏图标缺失: %s", qPrintable(key));
    m_cache.insert(key, pm);
    return pm;
}

// 主状态（无视 m_st）：用来量「正常态」的画布尺寸，作为变体居中的基准
QPixmap FooterBar::pixMain(const QString &slot) const
{
    const QString key = slot + QLatin1String("#main");
    auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) return it.value();
    QPixmap pm(QStringLiteral(":/mselect/%1.png").arg(slot));
    m_cache.insert(key, pm);
    return pm;
}

// 画一个槽位。变体画布尺寸可能和主状态不同（典型：充电电池 45x44 比放电电池
// 46x26 高，闪电上下都要探出电池），直接按左上角贴会整体偏下 9px。
// 所以尺寸不一致时按主状态画布居中 —— 主状态坐标取自设计稿，居中后电池本体、
// 闪电的上下探出量就都回到设计稿位置。
static void drawSlot(QPainter &p, int x, int y,
                     const QPixmap &pm, const QPixmap &base)
{
    if (pm.isNull()) return;
    int dx = 0, dy = 0;
    if (!base.isNull() && (pm.width() != base.width() || pm.height() != base.height())) {
        dx = (base.width()  - pm.width())  / 2;
        dy = (base.height() - pm.height()) / 2;
    }
    p.drawPixmap(x + dx, y + dy, pm);
}

// ---- 数值 → 图标档位的联动 ------------------------------------------------
void FooterBar::setDetSize(int cm)
{
    setDetSize(cm, cm);
}

// 探测器/限束器开口：横向 × 纵向。设计稿这里是正方形 "29 x 29 cm"，
// 实际设备横纵开口可以不同（例如 35 x 43），所以要能分开下发。
void FooterBar::setDetSize(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    m_detW = w;
    m_detH = h;
    setTxt(0, QStringLiteral("%1 x %2 cm").arg(m_detW).arg(m_detH));
}

void FooterBar::setDetW(int w) { setDetSize(w, m_detH); }
void FooterBar::setDetH(int h) { setDetSize(m_detW, h); }

void FooterBar::setFilter(const QString &s)
{
    setTxt(1, s);
}

void FooterBar::setTemp(int idx, int celsius)
{
    if (idx < 0 || idx > 1) return;
    setTxt(idx == 0 ? 2 : 3, QStringLiteral("%1\u00B0C").arg(celsius));
    setState(idx == 0 ? QStringLiteral("ftTempA") : QStringLiteral("ftTempB"),
             celsius >= kTempWarn ? QStringLiteral("warn") : QString());
}

void FooterBar::setHu(int pct)
{
    setTxt(4, QStringLiteral("HU %1%").arg(pct));
}

// 电池：level 0..4 → 图标内 0/1/2/3/4 格填充条；charging → 绿电池 + 白闪电；
//       低电另外用 low（1 格 + 橙）走过去，见 mainwindow 的 bata=low。
//   A 槽位主状态是「满格放电电池」，B 槽位主状态是「带闪电的充电电池」，
//   所以 p0..p4 两张都存了同名同内容的一份，谁都不会去用对方的主状态。
void FooterBar::setBattery(int idx, int level, bool charging)
{
    const QString slot = idx == 0 ? QStringLiteral("ftBatA") : QStringLiteral("ftBatB");
    if (charging) {
        setState(slot, QStringLiteral("chg"));
        return;
    }
    const int q = qBound(0, level, 4);
    setState(slot, QStringLiteral("p%1").arg(q));
}

// 低电：橙色 + 1 格
void FooterBar::setBatteryLow(int idx)
{
    setState(idx == 0 ? QStringLiteral("ftBatA") : QStringLiteral("ftBatB"),
             QStringLiteral("low"));
}

void FooterBar::setTubeActive(int idx)
{
    setState(QStringLiteral("ftBA"), idx == 0 ? QString() : QStringLiteral("off"));
    setState(QStringLiteral("ftBB"), idx == 1 ? QStringLiteral("on") : QString());
}

void FooterBar::setGrid(int idx, int state)
{
    const QString slot = idx == 0 ? QStringLiteral("ftGridA") : QStringLiteral("ftGridB");
    setState(slot, state == 2 ? QStringLiteral("warn") : QString());
}

void FooterBar::setDetector(int idx, int state)
{
    // 设计稿天然是「A 蓝（就绪）+ B 灰（未就绪）」：
    // 同一句 ready 落到 A 上是主状态，落到 B 上就得用 _on 变体，反之亦然。
    const bool ready = (state == 0);
    const bool slotIsBlue = (idx == 0);
    const QString slot = slotIsBlue ? QStringLiteral("ftDetA") : QStringLiteral("ftDetB");
    QString st;
    if (ready != slotIsBlue)
        st = ready ? QStringLiteral("on") : QStringLiteral("off");
    setState(slot, st);
}

void FooterBar::setCollim(int state)
{
    setState(QStringLiteral("ftCollim"),
             state == 1 ? QStringLiteral("off")
                        : (state == 2 ? QStringLiteral("warn") : QString()));
}

void FooterBar::setFiltState(int state)
{
    setState(QStringLiteral("ftFilt"),
             state == 2 ? QStringLiteral("warn") : QString());
}

// HU 左边那只温度计（探测器温度）：界面不显示数字，只靠颜色表意，
// 所以这里只有「正常 / 告警」两态，阈值与球管温度同一个 kTempWarn。
void FooterBar::setDetTemp(int celsius)
{
    setState(QStringLiteral("ftTempHU"),
             celsius >= kTempWarn ? QStringLiteral("warn") : QString());
}

// 剂量环：整圈绿 → 红
void FooterBar::setRing(int state)
{
    setState(QStringLiteral("ftRing"),
             state == 2 ? QStringLiteral("warn") : QString());
}

void FooterBar::resetAll()
{
    m_st.clear();
    m_cache.clear();
    m_detW = m_detH = 29;
    setTxt(0, QStringLiteral("29 x 29 cm"));
    setTxt(1, QStringLiteral("0.9 Cu + 1 Al"));
    setTxt(2, QStringLiteral("48\u00B0C"));
    setTxt(3, QStringLiteral("48\u00B0C"));
    setTxt(4, QStringLiteral("HU 0%"));
    // 设计稿默认帧：A 满格放电、B 充电中（闪电）
    m_st.insert(QStringLiteral("ftBatA"), QStringLiteral("p4"));
    m_st.insert(QStringLiteral("ftBatB"), QStringLiteral("chg"));
}

void FooterBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    p.fillRect(rect(), kBg);

    if (m_land) {
        // 横屏三条竖线：组1|组2、组2|组3、组3|组4
        p.fillRect(QRect(234,  0, 2, kFootSepH), kSep);
        p.fillRect(QRect(716,  0, 2, kFootSepH), kSep);
        p.fillRect(QRect(1338, 0, 2, kFootSepH), kSep);
    } else {
        p.fillRect(QRect(160, 24, 2, 112), kSep);
        p.fillRect(QRect(433, 24, 2, 112), kSep);
        p.fillRect(QRect(897, 24, 2, 112), kSep);
    }

    const IconAt *icons = m_land ? kLandFootIcons : kPorFootIcons;
    const int nIcons = m_land ? int(sizeof(kLandFootIcons) / sizeof(IconAt))
                              : int(sizeof(kPorFootIcons) / sizeof(IconAt));
    for (int i = 0; i < nIcons; ++i) {
        const QLatin1String slot(icons[i].slot);
        drawSlot(p, icons[i].x, icons[i].y, pix(slot), pixMain(slot));
    }

    const FootText *tab = m_land ? kLandFootText : kPorFootText;
    const int nTab = m_land ? int(sizeof(kLandFootText) / sizeof(FootText))
                            : int(sizeof(kPorFootText) / sizeof(FootText));
    const QFont f = stFont(kFootPx);
    for (int i = 0; i < nTab; ++i) {
        const FootText &t = tab[i];
        if (t.align == 1)
            drawInkRight(p, t.inkLeft, t.baseline, m_txt[t.which], f, kInkMain, t.track);
        else
            drawInk(p, t.inkLeft, t.baseline, m_txt[t.which], f, kInkMain, t.track);
    }
}

// ============================================================================
//  顶栏 HeaderBar —— 头像 + 患者姓名 + 下拉箭头
// ----------------------------------------------------------------------------
//  量测（设计稿 1200x1920）：
//    底色 #383838，整条 y0..112，y=112 起是黑色分隔（#050505）
//    头像 58x54 @(27,29)      姓名墨迹 x100..475 / y34..79     箭头 40x30 @(498,43)
//    文字色 #D7D7D7（比底栏 #D1D1D1 略亮）
//  头像与箭头直接从设计稿裁（底色同为 #383838，贴上去零色差），只有文字重画。
// ============================================================================
static const QRect  kHdrBand   (0, 0, 1200, 112);
static const QRect  kHdrAvatar (27, 29, 58, 54);
static const QRect  kHdrArrow  (498, 43, 40, 30);
static const int    kHdrNameLeft = 100;
static const int    kHdrNameBase = 71;   // 设计稿墨迹 y34..79，实测基线要 71 才对得齐
static const int    kHdrNamePx   = 48;
static const int    kHdrNameMax  = 478 - kHdrNameLeft;   // 再长就压到箭头
static const QColor kHdrBg   (0x38, 0x38, 0x38);
static const QColor kHdrInk  (0xD7, 0xD7, 0xD7);
static const QString kNameDefault = QStringLiteral("Ahmed, Ayesha");

HeaderBar::HeaderBar(QWidget *page) : QWidget(page)
{
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);
    resetAll();
}

void HeaderBar::applyProfile(bool landscape)
{
    m_land = landscape;
    // 横屏顶栏与竖屏同坐标（头像是 27,29、姓名左端 100、基线 71，量过完全一致），
    // 只是画布宽 1920。右侧 1200..1920 是纯 #383838，fillRect 自然铺满。
    setGeometry(landscape ? QRect(0, 0, 1920, kHdrBand.height()) : kHdrBand);
    show();
}

void HeaderBar::setName(const QString &name)
{
    m_name = name.trimmed();
    if (m_name.isEmpty())
        m_name = kNameDefault;
    update();
}

void HeaderBar::resetAll()
{
    m_name = kNameDefault;
    update();
}

void HeaderBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), kHdrBg);

    const QPixmap av(QStringLiteral(":/mselect/hdrAvatar.png"));
    if (!av.isNull())
        p.drawPixmap(kHdrAvatar.topLeft(), av);
    const QPixmap ar(QStringLiteral(":/mselect/hdrArrow.png"));
    if (!ar.isNull())
        p.drawPixmap(kHdrArrow.topLeft(), ar);

    // 名字太长就截断加省略号，别压掉下拉箭头
    const QFont f = stFont(kHdrNamePx, QFont::Bold);
    QFontMetricsF fm(f);
    QString s = m_name;
    if (fm.tightBoundingRect(s).width() > kHdrNameMax) {
        while (s.size() > 1 && fm.tightBoundingRect(s + QStringLiteral("\u2026")).width() > kHdrNameMax)
            s.chop(1);
        s += QStringLiteral("\u2026");
    }
    drawInk(p, kHdrNameLeft, kHdrNameBase, s, f, kHdrInk);
}

// ============================================================================
//  缩略图下面的部位名 ThumbCaption（"Chest PA"）
// ----------------------------------------------------------------------------
//  量测：卡片条 x24..263 / y384..463（240x80），底 #2B2B2B
//        "Chest PA" 墨迹 x80..206 / y410..431，正好在卡内水平居中，基线 431
//  只重画文字，缩略图本身（脚片）仍是底图像素。
// ============================================================================
static const QRect kCapBand   (24, 384, 240, 80);
// 横屏：缩略图 (32,144) 240x328，条子贴底 ⇒ y392。文字左端/基线相对 offset 与竖屏相同
static const QRect kCapBandLand(32, 392, 240, 80);
static const int   kCapBase   = 433 - 384;   // 设计稿墨迹 y410..431
static const int   kCapPx     = 30;
static const QColor kCapBg    (0x2B, 0x2B, 0x2B);
static const QString kPartDefault = QStringLiteral("Chest PA");

ThumbCaption::ThumbCaption(QWidget *page) : QWidget(page)
{
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::NoFocus);
    resetAll();
}

void ThumbCaption::applyProfile(bool landscape)
{
    m_land = landscape;
    // 横屏也有这块条子（"Chest PA"），只是跟着缩略图挪到 (32,392)
    setGeometry(landscape ? kCapBandLand : kCapBand);
    show();
}

void ThumbCaption::setPart(const QString &part)
{
    m_part = part.trimmed();
    if (m_part.isEmpty())
        m_part = kPartDefault;
    update();
}

void ThumbCaption::resetAll()
{
    m_part = kPartDefault;
    update();
}

void ThumbCaption::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), kCapBg);

    const QFont f = stFont(kCapPx);
    QFontMetricsF fm(f);
    QString s = m_part;
    // 卡片 240 宽，留 12px 内边距，太长就截断
    const qreal maxW = width() - 24;
    if (fm.tightBoundingRect(s).width() > maxW) {
        while (s.size() > 1 && fm.tightBoundingRect(s + QStringLiteral("\u2026")).width() > maxW)
            s.chop(1);
        s += QStringLiteral("\u2026");
    }
    const QRectF br = fm.tightBoundingRect(s);
    // 设计稿 "Chest PA" 墨迹 126 宽，Ubuntu 30px 只有 119 —— 补 1px 字距（8 字符 +7）
    QFont ff = f;
    ff.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
    const qreal x = (width() - (br.width() + 7.0)) / 2.0 - br.left();
    p.setFont(ff);
    p.setPen(kInkMain);
    p.drawText(QPointF(x, kCapBase), s);
}

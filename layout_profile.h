// 版式表：竖屏 1200x1920 / 横屏 1920x1200 两套。
//
// 为什么要这张表？
//   原工程的 mainwindow.ui 里只有 3 个「隐形热区」按钮（pushButton/pushButton_2/comboBox，
//   坐标还是随手摆的），真正的界面是 **一张整页 PNG 底图**。也就是说：
//   —— 没有控件树可以导出坐标，版式只能从设计稿量出来。
//
// 量测方法（可复现）：
//   share/tools/layout_scan.py 把设计稿按「非底色连通块」切成矩形，
//   分割线/分格线再单独扫（见 _probe/layout/README.md 的量测表）。
//
// 用法：
//   竖↔横切换 = 换 profile + 换 sceneRect + 换 view 旋转角，控件本身不重建：
//
//     const LayoutProfile &p = dr::profile(landscape);
//     page->setFixedSize(p.design);
//     scene.setSceneRect(0, 0, p.design.width(), p.design.height());
//     view.resetTransform(); view.rotate(p.rot); view.fitInView(...);
//     page->applyProfile(p);          // 内部按 p.spots / p.steppers 挪控件
//
// 坐标约定：全部是「设计稿坐标」，原点在画布左上角，与 .ui 里的绝对坐标同一套。
#ifndef LAYOUT_PROFILE_H
#define LAYOUT_PROFILE_H

#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

namespace dr {

// ---------- 一个可点区域 ----------
struct Hotspot {
    QString id;      // 语义 id：代码里靠它找控件
    QRect   r;       // 设计坐标
    QString label;   // 中文说明，仅调试/日志用
};

// ---------- 步进器：外框 + 三个真实点击子格 ----------
// 设计稿里 −/数值/+ 之间是 #575757 的 1px 分隔线，点「−」和点「数值」是不同动作，
// 所以三个子格必须分开算，不能只用一个外框。
struct StepperSpot {
    QString id;
    QRect   outer;
    QRect   dec;     // −
    QRect   val;     // 数值区（有的机型可点弹键盘）
    QRect   inc;     // +
    bool    enabled; // 竖屏 mAs/ms 是灰的（#1C1C1C 底），横屏同理
};

// ---------- 一套完整版式 ----------
struct LayoutProfile {
    QString name;          // "portrait" / "landscape"
    QSize   design;        // 设计画布尺寸
    int     rot;           // QGraphicsView::rotate 的角度（顺时针为正）
    QString bg;            // 底图文件名（在 PHILIPS_TUI 下）

    QRect header;          // 顶栏（#383838 实心条）
    QRect body;            // 顶栏下沿 ~ 底栏上沿之间的内容区
    QRect tabBar;          // Tab 行（含底部基线）
    QRect footer;          // 底栏
    QRect info;            // 左列读数区（Req.SID / AID / 角度 / 高度）

    // ---- 部位卡片内部：全部相对卡片左上角 ----
    // 量法：share/tools/_scan_card.py 对设计稿卡片区做列投影分组
    //   竖屏 432 宽：图标墨迹 160..218 / 字母 244..274 / 三角 400..431
    //   横屏 512 宽：图标墨迹 198..256 / 字母 282..312 / 三角 480..511
    // ⇒ 图标与字母随卡宽居中（右移 38），三角贴右边（右移 80）
    QRect  cardIcon;       // 图标 64x64 框（比墨迹四周各大 3px）
    QPoint cardInk;        // 字母墨迹左上角
    QRect  cardTri;        // 右下角三角

    // ---- 站位（Workspot）卡片内部：整卡居中 ----
    //   竖屏 lamp 卡片 432 宽：图标墨迹 abs 934..989 / y 172..227
    //                          ⇒ 64 框 abs (930,168) = 卡片内 (186,32)
    //   横屏卡片 512 宽：(512-64)/2 = 224
    QRect  wsIcon;

    QVector<QRect>   tabs;      // 4 个 Tab 的命中区（等分）
    QVector<Hotspot> spots;     // 卡片类热区
    QVector<StepperSpot> steppers;

    const Hotspot *spot(const QString &id) const
    {
        for (const Hotspot &h : spots)
            if (h.id == id) return &h;
        return nullptr;
    }
    const StepperSpot *stepper(const QString &id) const
    {
        for (const StepperSpot &s : steppers)
            if (s.id == id) return &s;
        return nullptr;
    }
};

// =====================================================================
// 竖屏 1200x1920 —— 设计稿 曝光2备份.png（AEC 关）/ 曝光2备份 3.png（AEC 开）
//   面板物理竖装，HDMI 帧是横的 ⇒ 整页再转 270°（与 dr_ui 的 --q 3 同一套几何）
//
//   ⚠️ 2026-11-11 换底图：原底图 对中2.png 的步进器只有 2 行、高 130；
//   新设计 曝光2备份.png 是 3 行、高 98（多了 DEN 行）。两张稿在 y<1019 完全一样，
//   只有步进器区不同，所以换底图不影响上面任何卡片/弹层。
// =====================================================================
inline const LayoutProfile &portraitProfile()
{
    static const LayoutProfile p = [] {
        LayoutProfile q;
        q.name   = QStringLiteral("portrait");
        q.design = QSize(1200, 1920);
        q.rot    = 270;
        q.bg     = QStringLiteral("曝光2备份.png");

        q.header = QRect(0, 0, 1200, 112);      // #383838
        q.body   = QRect(0, 120, 1200, 1624);   // y120..1743
        q.tabBar = QRect(8, 588, 1184, 96);     // 基线 y680..683
        q.footer = QRect(0, 1752, 1200, 168);   // y1752..1919
        q.info   = QRect(24, 420, 1152, 160);   // 上边线 y425..426（x24..1175）

        q.cardIcon = QRect(157, 32, 64, 64);
        q.cardInk  = QPoint(245, 46);
        q.cardTri  = QRect(400, 96, 32, 32);    // 右边界 = 卡片右边界 432
        q.wsIcon   = QRect(186, 32, 64, 64);    // 站位卡片 432 宽 ⇒ (432-64)/2 = 184, 实测 186

        // 4 个 Tab 等分 8..1191（每个 296）
        for (int i = 0; i < 4; ++i)
            q.tabs << QRect(8 + i * 296, 588, 296, 96);

        q.spots = {
            {QStringLiteral("thumb"),     QRect( 24, 136, 240, 328), QStringLiteral("缩略图 + 部位名")},
            {QStringLiteral("bodyPart"),  QRect(288, 136, 432, 128), QStringLiteral("部位卡片 M（下拉）")},
            {QStringLiteral("lamp"),      QRect(744, 136, 432, 128), QStringLiteral("灯 / 限束")},
            {QStringLiteral("laser"),     QRect(288, 280, 432, 128), QStringLiteral("准星 / 对中")},
            {QStringLiteral("collim"),    QRect(744, 280, 432, 128), QStringLiteral("束光器")},
            {QStringLiteral("aec"),       QRect( 32, 716, 362, 128), QStringLiteral("AEC")},
            {QStringLiteral("view"),      QRect(419, 716, 362, 128), QStringLiteral("视图")},
            {QStringLiteral("el"),        QRect(806, 716, 362, 128), QStringLiteral("EL 320")},
            {QStringLiteral("kvma"),      QRect( 32, 868, 749, 128), QStringLiteral("kV-mAs 显示")},
            {QStringLiteral("square"),    QRect(806, 868, 362, 128), QStringLiteral("焦点")},
        };

        // 步进器：外框 558 宽 x 98 高，−/值/+ = 92/366/92，分隔线在 x125..126、x493..494
        // 三行：kV·mA / mAs·ms / DEN（DEN 只有左列，右列第三行是空的）
        // 行距 120（1019 / 1139 / 1259）
        auto mk = [](const QString &id, int x, int y, int ow, int valW, int incW, bool en) {
            StepperSpot s;
            s.id      = id;
            s.outer   = QRect(x, y, ow, 98);
            s.dec     = QRect(x + 2,    y + 1, 92,   96);
            s.val     = QRect(x + 96,   y + 1, valW, 96);
            s.inc     = QRect(x + 98 + valW, y + 1, incW, 96);
            s.enabled = en;
            return s;
        };
        q.steppers = {
            mk(QStringLiteral("stKv"),    31, 1019, 558, 366, 92, true),
            mk(QStringLiteral("stMa"),   611, 1019, 558, 366, 92, true),
            mk(QStringLiteral("stMas"),   31, 1139, 558, 366, 92, false),
            mk(QStringLiteral("stMs"),   611, 1139, 558, 366, 92, false),
            mk(QStringLiteral("stDen"),   31, 1259, 558, 366, 92, false),
        };
        return q;
    }();
    return p;
}

// =====================================================================
// 横屏 1920x1200 —— 设计稿 曝光备份.png（AEC 关）/ 曝光备份 3.png（AEC 开）
//   注意 rot：面板物理竖装时，横屏内容要转 0°；若面板改成横装则要改 180°。
//   先跑起来再按实物微调这一个数（也可用 --rot 从命令行覆盖）。
//   ⚠ 2026-11-20 起除了 rot 还有镜像：--flip h|v|hv（见 main.cpp 的 g_flipH/g_flipV）。
//   真正不同的朝向只有 4 种：0° / 180° / 左右镜像 / 上下镜像
//   （rot180 ≡ 两个轴都翻），用 `--orient-sheet <dir>` 一次全导出来挑。
//
//   ⚠️ 2026-11-11 换底图：原底图 常规状态.png 的步进器只有 2 行、高 130；
//   新设计 曝光备份.png 是 3 行、高 98（多了 DEN 行）。y<540 完全一样。
// =====================================================================
inline const LayoutProfile &landscapeProfile()
{
    static const LayoutProfile p = [] {
        LayoutProfile q;
        q.name   = QStringLiteral("landscape");
        q.design = QSize(1920, 1200);
        q.rot    = 0;
        q.bg     = QStringLiteral("曝光备份.png");

        q.header = QRect(0, 0, 1920, 112);      // #383838
        q.body   = QRect(0, 120, 1920, 912);    // y120..1031
        q.tabBar = QRect(840, 120, 1072, 96);   // 蓝色活动基线 y212..215
        q.footer = QRect(0, 1040, 1920, 160);   // y1040..1199
        q.info   = QRect(32, 673, 776, 269);    // 左列下部读数区（墨迹 147..679 / 689..925，四周留白）

        q.cardIcon = QRect(195, 32, 64, 64);    // 卡片 512 宽：图标/字母右移 38（居中）
        q.cardInk  = QPoint(283, 46);
        q.cardTri  = QRect(480, 96, 32, 32);    // 右移 80 = 贴卡片右边
        q.wsIcon   = QRect(224, 32, 64, 64);    // 站位卡片 512 宽 ⇒ (512-64)/2 = 224

        // 4 个 Tab 等分 840..1911（每个 268）
        for (int i = 0; i < 4; ++i)
            q.tabs << QRect(840 + i * 268, 120, 268, 96);

        q.spots = {
            {QStringLiteral("thumb"),     QRect( 32, 144, 240, 328), QStringLiteral("缩略图 + 部位名")},
            {QStringLiteral("bodyPart"),  QRect(296, 144, 512, 128), QStringLiteral("部位卡片 M（下拉）")},
            {QStringLiteral("lamp"),      QRect(296, 296, 512, 128), QStringLiteral("灯 / 限束")},
            {QStringLiteral("laser"),     QRect(296, 448, 244, 128), QStringLiteral("准星 / 对中")},
            {QStringLiteral("collim"),    QRect(564, 448, 244, 128), QStringLiteral("束光器")},
            {QStringLiteral("aec"),       QRect(864, 248, 325, 128), QStringLiteral("AEC")},
            {QStringLiteral("view"),      QRect(1213, 248, 325, 128), QStringLiteral("视图")},
            {QStringLiteral("el"),        QRect(1563, 248, 325, 128), QStringLiteral("EL 320")},
            {QStringLiteral("kvma"),      QRect(864, 400, 674, 128), QStringLiteral("kV-mAs 显示")},
            {QStringLiteral("square"),    QRect(1563, 400, 325, 128), QStringLiteral("焦点")},
        };

        // 步进器：外框 502 宽 x 98 高，−/值/+ = 92/310/92，分隔线在 x957..958、x1269..1270
        // 三行：kV·mA / mAs·ms / DEN（行距 120：551 / 671 / 791）
        auto mk = [](const QString &id, int x, int y, bool en) {
            StepperSpot s;
            s.id      = id;
            s.outer   = QRect(x, y, 502, 98);
            s.dec     = QRect(x + 2,    y + 1, 92,  96);
            s.val     = QRect(x + 96,   y + 1, 310, 96);
            s.inc     = QRect(x + 98 + 310, y + 1, 92, 96);
            s.enabled = en;
            return s;
        };
        q.steppers = {
            mk(QStringLiteral("stKv"),   863, 551, true),
            mk(QStringLiteral("stMa"),  1387, 551, true),
            mk(QStringLiteral("stMas"),  863, 671, false),
            mk(QStringLiteral("stMs"),  1387, 671, false),
            mk(QStringLiteral("stDen"),  863, 791, false),
        };
        return q;
    }();
    return p;
}

inline const LayoutProfile &profile(bool landscape)
{
    return landscape ? landscapeProfile() : portraitProfile();
}

} // namespace dr

#endif // LAYOUT_PROFILE_H

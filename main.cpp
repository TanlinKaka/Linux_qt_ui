#include <QApplication>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsProxyWidget>
#include <QResizeEvent>
#include <QDebug>
#include <QDateTime>
#include <QPainter>
#include <QScreen>
#include <QWindow>
#include <QColor>
#include <QList>
#include <QResource>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <cstring>
#include <cstdlib>
#include <QTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QThread>
#include <QTouchEvent>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include "mainwindow.h"
#include "patientcard.h"
#include "layout_profile.h"
#include "drlink.h"
#include "uicards.h"
#include <QUrl>

// 设计稿尺寸：竖屏 1200x1920 / 横屏 1920x1200，由版式表决定，不再写死。
// 2026-11-10 起 --landscape 切到横屏那套（两个是独立设计稿，不是旋转出来的）。
static QSize g_design(1200, 1920);

// 面板物理竖装，但 HDMI 输出的帧是横的（1280x800）⇒ 整页要再转 270°
// 与 dr_ui 的 --q 3 同一套几何（QTransform::rotate 正角 = 顺时针）
// 横屏那套的逻辑尺寸本身是 1920x1200，转多少由 profile.rot 给，--rot 可覆盖。
static int g_rotDeg = 270;

// 镜像翻转（沿设计稿的中轴线翻）。和 rotate 是两个独立维度：
//   · 转 180° 解决「屏装倒了」——客户现场最常见
//   · 沿 X 镜像（左右翻）解决「面板装在机器另一侧、从背后看」——少见但有
//   二者都作用在**设计坐标**上，所以点/触摸的坐标反算仍然由 Qt 的逆变换负责，
//   不用另写一套；但**硬件触摸**（evdev 原始坐标）不会跟着翻，见 README/文档。
//   组合关系：rot180 + mirrorH ≡ mirrorV，所以真正不同的朝向只有 4 种。
static bool g_flipH = false;      // 沿竖直中轴镜像（左右翻）
static bool g_flipV = false;      // 沿水平中轴镜像（上下翻）

// 设计稿比例与屏幕比例不一致时怎么铺（设计稿 16:10，DSI 屏 1024x600 = 1.71:1）：
//   stretch —— 默认。X/Y 各自缩放填满视口，黑边彻底消失；内容一个像素都不裁，
//              代价是单向约 6% 压缩（1920→1024 是 0.533，1200→600 是 0.5）。
//   contain —— 等比，整页完整但四周留黑边。只有在 16:10 的屏（HDMI 1280x800）上
//              才等价于满屏；1024x600 上会缩成 960x600、左右各 32px 边。
//   crop    —— 按短边铺满，多出来的部分裁掉（1920 宽的画布上下各裁 20px，
//              header 里患者名会被切掉一块，只在确定不要页边时用）。
enum FitMode { FitStretch, FitContain, FitCrop };
static FitMode g_fitMode = FitStretch;

// 用一个 QGraphicsView 包住设计页：
//   scene 坐标恒为 1200x1920（.ui 里的绝对坐标一个字都不用改）
//   resizeEvent 里做「摆正朝向 + 等比充满」，触摸/鼠标坐标由 Qt 自动反算回设计坐标
class ScaleView : public QGraphicsView
{
public:
    explicit ScaleView(QGraphicsScene *scene, QWidget *parent = nullptr)
        : QGraphicsView(scene, parent)
    {
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // 视口留白用页面底色（设计稿左右/底部 #171717），这样即使 corner 真露出来
        // 也不是刺眼的纯黑，视觉上与页面连成一片。
        setBackgroundBrush(QColor(0x17, 0x17, 0x17));
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing
                       | QPainter::SmoothPixmapTransform);
        setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        // 注意：这里**不要**给 viewport 打开 WA_AcceptTouchEvents。
        // 2026-11-16 试过「触摸自己转鼠标回灌 viewport」，结果板上所有按钮
        // 全部点不动（吞掉触摸、Qt 就不再合成鼠标，而回灌的 QMouseEvent
        // 又到不了 scene）。触摸这条路保持 Qt 默认，只做观测，见 viewportEvent。
    }

    // 运行期换朝向：只重算变换，不重建任何控件（用来在实物上挑 rot）
    void setRot(int deg)
    {
        if (deg == g_rotDeg)
            return;
        g_rotDeg = deg;
        relayout();
    }

    // 运行期换镜像：与 setRot 同理，只重算变换
    void setFlip(bool h, bool v)
    {
        if (h == g_flipH && v == g_flipV)
            return;
        g_flipH = h;
        g_flipV = v;
        relayout();
    }

    // 强制重算变换。换版式时 g_design / g_rotDeg 变了，但 view 自身尺寸没变，
    // 不会触发 resizeEvent，必须手动来一下（见 main 里的 orientationToggleRequested）。
    void refresh() { relayout(); }

protected:
    // 2026-11-16 记录：不要在 ScaleView 里重写 viewportEvent 去处理触摸。
    //   · accept() + 回灌 QMouseEvent → 触摸被吞、Qt 不再合成鼠标，全局点不动
    //   · 只打日志、照常交给父类     → 板上一摸就闪退（日志只到 touch#1 type=194）
    // 触摸这条路先不动，滚动改从输入设备侧解决（见 dbg.sh / 记忆）。
    //
    // --windowed 下是无边框窗口，没有标题栏可关，用 Esc 退出方便调试
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape)
            QCoreApplication::quit();
        else
            QGraphicsView::keyPressEvent(e);
    }

    void resizeEvent(QResizeEvent *e) override
    {
        QGraphicsView::resizeEvent(e);
        relayout();
    }

private:
    void relayout()
    {
        if (!scene() || scene()->sceneRect().isEmpty())
            return;

        const QRect vp = viewport()->rect();
        if (vp.isEmpty())
            return;

        // 旋转 90/270 时，设计稿的「宽」占的是视口的「高」，可用宽高要换一下
        const bool swap = (g_rotDeg % 180) != 0;
        const qreal availW = swap ? vp.height() : vp.width();
        const qreal availH = swap ? vp.width()  : vp.height();

        qreal sx = availW / g_design.width();
        qreal sy = availH / g_design.height();
        if (g_fitMode == FitContain) {
            const qreal s = qMin(sx, sy); sx = sy = s;
        } else if (g_fitMode == FitCrop) {
            const qreal s = qMax(sx, sy); sx = sy = s;
        }

        // 不用 fitInView()：Qt5 的实现里 viewport 被 adjusted(2,2,-2,-2)，
        // 四周恒定留 2px；而且旋转之后它按未旋转的场景矩形算比例，
        // stretch 模式下会算成 373 宽（实测）。自己拼变换才能精确铺满。
        QTransform t;
        t.translate(vp.width() / 2.0, vp.height() / 2.0);
        t.rotate(g_rotDeg);
        t.scale(sx, sy);
        // 镜像放在 rotate/scale 之后、平移回原点之前：正负号作用在**设计坐标**上，
        // 也就是「把设计稿沿自己的中轴线翻过来」，而不是翻视口。
        if (g_flipH || g_flipV)
            t.scale(g_flipH ? -1.0 : 1.0, g_flipV ? -1.0 : 1.0);
        t.translate(-g_design.width() / 2.0, -g_design.height() / 2.0);
        setTransform(t);

        // 注意：旋转后 m11() 是 cos，不能当缩放比看，
        // 要量"设计稿映射到视口后的实际像素尺寸"
        const QRectF mapped = t.mapRect(scene()->sceneRect());
        static const char *kFitName[3] = {"stretch", "contain", "crop"};
        qDebug("view %dx%d  vp %dx%d  design %dx%d  rot=%d flip=%d%d  fit=%s  scale %.3fx%.3f  mapped %.0fx%.0f",
               width(), height(), vp.width(), vp.height(),
               g_design.width(), g_design.height(),
               g_rotDeg, g_flipH ? 1 : 0, g_flipV ? 1 : 0,
               kFitName[g_fitMode], sx, sy, mapped.width(), mapped.height());
    }
};

// 模拟「手指按住划动」：press → 若干 move → release，坐标同样是设计稿坐标。
// 用来验「划动只翻页、松手不选中」这条手势链，人手在 VM 里很难稳定复现。
//   --sim-drag "x1,y1,x2,y2"   起点/终点（设计坐标）
static void simDragDesign(QGraphicsView &view, const QString &arg)
{
    const QStringList p = arg.split(QLatin1Char(','));
    if (p.size() != 4) {
        qWarning("!! --sim-drag 需要 \"x1,y1,x2,y2\"，收到 '%s'", qPrintable(arg));
        return;
    }
    const QPointF a(p[0].trimmed().toDouble(), p[1].trimmed().toDouble());
    const QPointF b(p[2].trimmed().toDouble(), p[3].trimmed().toDouble());
    const QPoint va = view.mapFromScene(a);
    const QPoint vb = view.mapFromScene(b);

    QMouseEvent press(QEvent::MouseButtonPress, va, view.viewport()->mapToGlobal(va),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &press);      // 弹层拿到按下

    const int steps = 12;
    for (int i = 1; i <= steps; ++i) {
        const QPoint v = va + (vb - va) * i / steps;
        QMouseEvent mv(QEvent::MouseMove, v, view.viewport()->mapToGlobal(v),
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view.viewport(), &mv);
        QCoreApplication::processEvents();
        // 让时间真的流逝 ~8ms/步：惯性是按「px/ms」采样的，
        // 12 步挤在同一毫秒里会算出天文速度，测出来的滑行距离全是假的。
        QThread::msleep(8);
    }

    QMouseEvent release(QEvent::MouseButtonRelease, vb, view.viewport()->mapToGlobal(vb),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &release);
    qDebug("[sim-drag] design(%.0f,%.0f)->(%.0f,%.0f) vp(%d,%d)->(%d,%d)",
           a.x(), a.y(), b.x(), b.y(), va.x(), va.y(), vb.x(), vb.y());
}

// 往视口里真发一对 press/release。坐标用设计稿坐标（1200x1920），
// 由 QGraphicsView 的变换反算到视口像素，所以和旋转/缩放无关。
static void simClickDesign(QGraphicsView &view, const QString &xyStr)
{
    const QStringList xy = xyStr.split(QLatin1Char(','));
    if (xy.size() != 2) {
        qWarning("!! --sim-click 需要 \"x,y\"，收到 '%s'", qPrintable(xyStr));
        return;
    }
    const QPointF design(xy[0].trimmed().toDouble(), xy[1].trimmed().toDouble());
    const QPoint vp = view.mapFromScene(design);
    qDebug("[sim-click] design(%.0f,%.0f) -> viewport(%d,%d)",
           design.x(), design.y(), vp.x(), vp.y());

    // 命中诊断：不看事件，先问「这个点上到底是谁」
    QWidget *hit = view.viewport()->childAt(vp);
    qDebug("[sim-click] childAt = %s",
           hit ? qPrintable(hit->objectName().isEmpty()
                            ? QString::fromLatin1(hit->metaObject()->className())
                            : hit->objectName())
               : "(none -> viewport/scene)");

    const QPoint global = view.viewport()->mapToGlobal(vp);
    QMouseEvent press(QEvent::MouseButtonPress, vp, global,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &press);
    qDebug("[sim-click] press   accepted=%d", int(press.isAccepted()));

    QMouseEvent release(QEvent::MouseButtonRelease, vp, global,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &release);
    qDebug("[sim-click] release accepted=%d", int(release.isAccepted()));
    qDebug("[sim-click] done at design(%.0f,%.0f)", design.x(), design.y());
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 资源包用外部二进制（rcc -binary），不编进可执行文件：
    // 45MB 图片直接编进 qrc 会让 g++ 吃到 6.6GB 内存被 OOM 杀掉，资源反而丢失。
    // 注册之后 ":/new/prefix1/..." 的写法照旧可用，界面代码一个字都不用改。
    const QStringList rccCands = {
        QCoreApplication::applicationDirPath() + "/res.rcc",
        QCoreApplication::applicationDirPath() + "/../UI/res.rcc",
        QStringLiteral("/userdata/img/res.rcc"),
        QStringLiteral("/usr/share/myui/res.rcc"),
    };
    bool resOk = false;
    for (int i = 0; i < rccCands.size(); ++i) {
        if (!QFile::exists(rccCands.at(i)))
            continue;
        if (QResource::registerResource(rccCands.at(i))) {
            qDebug("resource loaded: %s", qPrintable(rccCands.at(i)));
            resOk = true;
            break;
        }
    }
    if (!resOk)
        qWarning("!! res.rcc NOT loaded - all :/ images will fail");

    int wantScreen = -1;
    int  orientArg = 0;              // 0 竖屏（默认，面板物理竖装）、1 横屏
    int  rotArg = -1;                // <0 表示没在命令行给，用 profile 里的
    QString shotDir;                 // --shot <dir>：离屏自检，导出 PNG 后退出
    QStringList setPairs;            // --set "kv=120,ma=200"：启动时设一批变量
    QString watchFile;               // --vars [file]：运行中热加载变量文件
    bool dumpVars = false;           // --dump-vars：打印可调变量清单后退出
    int  shotDelay = 400;            // --shot-delay <ms>：离屏导出前的等待时间
    bool windowed = false;           // --windowed：桌面调试用，开普通窗口不全屏
    bool forceFull = false;          // --full：强制全屏（桌面平台默认窗口化时用）
    QUrl wsUrl;                      // --ws ws://ip:8080/ws：连主控（替代 Android 平板）
    bool wsTrace = false;            // --ws-trace：打印每帧收发原文
    // 摄像头 RTSP 地址：--cam-url 覆盖；不传就读环境变量 CAM_URL；都没有 = 不开流。
    // 板上这条最省事：改 /etc/myui.conf 的 MYUI_ARGS 加 --cam-url 即可。
    QString camUrl = QString::fromLocal8Bit(qgetenv("CAM_URL"));
    bool camFitCover = true;         // --cam-fit contain：改成内切留边（默认铺满裁边）
    // 摄像头取流调优（都在压 CPU，见 camsource.cpp 顶部）：
    int     camFps  = 15;            // --cam-fps N：限帧，0 = 跟源（25fps）
    QString camFmt  = QStringLiteral("NV12");   // --cam-fmt BGRx：让硬解直接出 RGB
    int     camDecW = 0, camDecH = 0;           // --cam-size 480x360：解码器直接出小图
    // 延迟相关：默认 auto（udp 优先、自动回退） + latency=0。
    // ⚠ rtspsrc 的 latency 默认 2000ms，强制 tcp 还会叠加丢包重传。
    QString camTransport = QStringLiteral("auto");   // --cam-transport auto|udp|tcp
    int     camLatency   = 0;                        // --cam-lat <ms>
    bool    camDropOnLat = false;                    // --cam-droplat 1（实测涨 4~5 倍 CPU）
    int     camBufs      = 1;                        // --cam-bufs N（appsink 队列，0=不限）
    int  simStepMs = 0;              // --sim-step <ms>：延时后自检点一次步进器
    QString simKeys = QStringLiteral("kv,ma,ms,mas,den");  // --sim-key kv / "kv,ma"
    int  simDir = +1;                // --sim-dir -1：点「−」格（发 val=-1），+1 点「+」
    // --sim-click "x,y"：延时后往视口里真发一对 press/release（设计坐标），
    //  用来验「页签/按钮点不动」这类事件投递问题，不依赖人手点和鼠标位置。
    QString simClickPt;
    // --sim-drag "x1,y1,x2,y2"：延时后真发 press→move×12→release，
    //  验「手指划动翻列表、松手不选中」。
    QString simDragArg;
    int  simDragDelay = 2200;        // --sim-drag-delay <ms>
    int  quitAfterMs  = 0;           // --quit-after <ms>
    int  simClickDelay = 1200;       // --sim-click-delay <ms>
    int  simClickGap   = 700;        // --sim-click-gap <ms>：连点时两次之间的间隔
    QString cardsFile;               // --cards <file>：下拉卡片配置（空 = 走候选链）
    bool dumpCards = false;          // --dump-cards：打印生效的下拉配置后退出
    QString orientDir;               // --orient-sheet <dir>：导出四种朝向候选图
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--rot") == 0 && i + 1 < argc)
            rotArg = atoi(argv[++i]);
        else if (strcmp(argv[i], "--flip") == 0 && i + 1 < argc) {
            // h = 左右翻、v = 上下翻、hv = 两个都翻（≡ 转 180°）
            const char *v = argv[++i];
            g_flipH = (strchr(v, 'h') != nullptr);
            g_flipV = (strchr(v, 'v') != nullptr);
        }
        else if (strcmp(argv[i], "--orient-sheet") == 0 && i + 1 < argc)
            orientDir = QString::fromLocal8Bit(argv[++i]);
        else if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc)
            wantScreen = atoi(argv[++i]);
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
            shotDir = QString::fromLocal8Bit(argv[++i]);
        else if (strcmp(argv[i], "--set") == 0 && i + 1 < argc)
            setPairs << QString::fromLocal8Bit(argv[++i]);
        else if (strcmp(argv[i], "--vars") == 0) {
            // 文件名可省，默认当前目录的 debug_vars.txt
            if (i + 1 < argc && argv[i + 1][0] != '-')
                watchFile = QString::fromLocal8Bit(argv[++i]);
            else
                watchFile = QStringLiteral("debug_vars.txt");
        }
        else if (strcmp(argv[i], "--dump-vars") == 0)
            dumpVars = true;
        else if (strcmp(argv[i], "--shot-delay") == 0 && i + 1 < argc)
            shotDelay = atoi(argv[++i]);
        else if (strcmp(argv[i], "--windowed") == 0)
            windowed = true;
        else if (strcmp(argv[i], "--full") == 0)
            forceFull = true;
        else if (strcmp(argv[i], "--landscape") == 0)
            orientArg = 1;
        else if (strcmp(argv[i], "--portrait") == 0)
            orientArg = 0;
        else if (strcmp(argv[i], "--ws") == 0 && i + 1 < argc)
            wsUrl = QUrl(QString::fromLocal8Bit(argv[++i]));
        else if (strcmp(argv[i], "--ws-trace") == 0)
            wsTrace = true;
        else if (strcmp(argv[i], "--sim-step") == 0 && i + 1 < argc)
            simStepMs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sim-key") == 0 && i + 1 < argc)
            simKeys = QString::fromLatin1(argv[++i]);
        else if (strcmp(argv[i], "--sim-dir") == 0 && i + 1 < argc)
            simDir = atoi(argv[++i]) < 0 ? -1 : +1;
        else if (strcmp(argv[i], "--sim-click") == 0 && i + 1 < argc)
            simClickPt = QString::fromLatin1(argv[++i]);
        else if (strcmp(argv[i], "--sim-click-delay") == 0 && i + 1 < argc)
            simClickDelay = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sim-click-gap") == 0 && i + 1 < argc)
            simClickGap = atoi(argv[++i]);
        else if (strcmp(argv[i], "--sim-drag") == 0 && i + 1 < argc)
            simDragArg = QString::fromLatin1(argv[++i]);
        else if (strcmp(argv[i], "--sim-drag-delay") == 0 && i + 1 < argc)
            simDragDelay = atoi(argv[++i]);
        else if (strcmp(argv[i], "--quit-after") == 0 && i + 1 < argc)
            quitAfterMs = atoi(argv[++i]);       // 自检/截图脚本用：到点自己退出，日志才不丢
        else if (strcmp(argv[i], "--cards") == 0 && i + 1 < argc)
            cardsFile = QString::fromLocal8Bit(argv[++i]);
        else if (strcmp(argv[i], "--dump-cards") == 0)
            dumpCards = true;
        else if (strcmp(argv[i], "--cam-url") == 0 && i + 1 < argc)
            camUrl = QString::fromLocal8Bit(argv[++i]);
        else if (strcmp(argv[i], "--cam-fit") == 0 && i + 1 < argc) {
            camFitCover = (strcmp(argv[++i], "contain") != 0);
        }
        else if (strcmp(argv[i], "--cam-fps") == 0 && i + 1 < argc)
            camFps = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cam-fmt") == 0 && i + 1 < argc)
            camFmt = QString::fromLocal8Bit(argv[++i]);
        else if (strcmp(argv[i], "--cam-transport") == 0 && i + 1 < argc)
            camTransport = QString::fromLocal8Bit(argv[++i]);
        else if (strcmp(argv[i], "--cam-lat") == 0 && i + 1 < argc)
            camLatency = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cam-droplat") == 0 && i + 1 < argc)
            camDropOnLat = atoi(argv[++i]) != 0;
        else if (strcmp(argv[i], "--cam-bufs") == 0 && i + 1 < argc)
            camBufs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cam-size") == 0 && i + 1 < argc) {
            // "480x360"，0x0 / 不传 = 原始分辨率
            const char *v = argv[++i];
            const char *x = strchr(v, 'x');
            if (x) { camDecW = atoi(v); camDecH = atoi(x + 1); }
        }
        else if (strcmp(argv[i], "--fit") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if      (strcmp(v, "contain") == 0) g_fitMode = FitContain;
            else if (strcmp(v, "crop") == 0)    g_fitMode = FitCrop;
            else                                g_fitMode = FitStretch;
        }
    }

    // 桌面平台（X11/xcb）默认开等比窗口：这样 Qt Creator 里直接 Ctrl+R、
    // 或者命令行不带任何参数，都能得到 1152x720 的窗口而不是全屏。
    // 板上跑的是 wayland/eglfs，平台名不是 xcb，仍旧全屏。
    // 想在全屏看效果：--full；想强制窗口：--windowed。
    if (!windowed && !forceFull && QGuiApplication::platformName() == QLatin1String("xcb"))
        windowed = true;

    // 桌面调试且没显式给 --vars 时，自动找一个 debug_vars.txt 来热加载，
    // 这样 Qt Creator 里 Ctrl+R 起的实例也能被 ./dbg.sh set 驱动，不用改 Run 配置。
    //
    // ⚠ 不能只用 applicationDirPath()/.. 去猜：Qt Creator 是影子构建，
    //   二进制在 ~/build-UI-DesktopQt5-Debug/ 这类目录里，/.. 会落到 $HOME，
    //   于是变成监听 /home/neo/debug_vars.txt，而 dbg.sh 写的是
    //   /home/neo/UI/debug_vars.txt —— 一条都收不到（踩过）。
    //   这里按固定优先级挑第一个存在的；都不存在就锁定项目里那个
    //   （watcher 同时监听目录，之后由 dbg.sh 创建也能被抓到）。
    if (watchFile.isEmpty() && windowed) {
        const QString home = QDir::homePath();
        const QString uiVars = home + QStringLiteral("/UI/debug_vars.txt");
        const QStringList cands = {
            qEnvironmentVariable("MYUI_VARS"),
            uiVars,
            QDir(QCoreApplication::applicationDirPath() + QStringLiteral("/.."))
                .absoluteFilePath(QStringLiteral("debug_vars.txt")),
            QCoreApplication::applicationDirPath() + QStringLiteral("/debug_vars.txt"),
            QDir::current().absoluteFilePath(QStringLiteral("debug_vars.txt")),
        };
        for (const QString &c : cands) {
            if (c.isEmpty())
                continue;
            if (QFileInfo::exists(c)) { watchFile = c; break; }
        }
        if (watchFile.isEmpty())
            watchFile = uiVars;
    }

    // 纯查询：打印全部可调变量后直接退出，不用起界面
    if (dumpVars) {
        qDebug("---- 可调变量清单（全部 key）----");
        const QStringList ks = MainWindow::varKeys();
        for (const QString &s : ks)
            qDebug("  %s", qPrintable(s));
        qDebug("----------------------------------");
        return 0;
    }

    // 下拉卡片配置（焦点 / EL / 曝光参数三张卡的弹层行）。
    // 必须在 new MainWindow 之前加载 —— 构造函数里就要按配置决定行数与文案。
    // cardsFile 为空时走候选链（MYUI_CARDS、工程根、/etc/myui/cards.json …）；
    // 一个都没找到就用代码里的设计稿默认值，行为与加这层之前完全一致。
    uicards::load(cardsFile);

    if (dumpCards) {                     // 纯查询：看看到底读到了哪份、内容是什么
        qDebug("%s", qPrintable(uicards::dumpJson()));
        return 0;
    }

    // 板上 weston 现在只暴露 HDMI-A-1 1280x800（DSI 已在 weston.ini 里 mode=off），
    // 但仍先列一遍：以后接双屏或换屏，--screen 就能指定。
    const QList<QScreen *> screens = QGuiApplication::screens();
    int pick = 0;
    for (int i = 0; i < screens.size(); ++i) {
        const QSize g = screens[i]->geometry().size();
        qDebug("screen[%d] '%s'  %dx%d", i, qPrintable(screens[i]->name()),
               g.width(), g.height());
        if (wantScreen < 0) {
            const QSize best = screens[pick]->geometry().size();
            if (g.width() * g.height() > best.width() * best.height())
                pick = i;
        }
    }
    if (wantScreen >= 0 && wantScreen < screens.size())
        pick = wantScreen;

    // 朝向只能由命令行决定，不能按屏幕宽高自动判：面板物理竖装，DSI-1 的
    // 输出帧本身就是 1024x600（宽>高），但整页还要 rotate(270) 才是正的。
    // 竖屏/横屏是两张独立设计稿而不是旋转出来的，这里只决定用哪张底图 + 哪套坐标。
    bool landscape = (orientArg == 1);
    const QSize scr = (pick >= 0 && pick < screens.size())
                      ? screens[pick]->geometry().size() : QSize(0, 0);

    // 版式表：尺寸 / 旋转角 / 底图 / 卡片坐标 一次全取出来
    const dr::LayoutProfile &prof = dr::profile(landscape);
    g_design = prof.design;
    g_rotDeg = (rotArg >= 0) ? rotArg : prof.rot;
    qDebug("orientation %s, screen %dx%d  design %dx%d rot=%d",
           landscape ? "landscape" : "portrait", scr.width(), scr.height(),
           g_design.width(), g_design.height(), g_rotDeg);

    // 必须 new 在堆上：scene.addWidget() 会把 widget 的所有权交给
    // QGraphicsProxyWidget，scene 析构时由它 delete（见 qgraphicsproxywidget.cpp:552）。
    // 如果这里写成栈对象 MainWindow page;，退出时就是 delete 一个栈地址，
    // glibc 直接 abort：double free or corruption (out)。
    MainWindow *page = new MainWindow;
    // 套版式：锁死设计尺寸（别让 view 把它拉变形）+ 换底图 + 摆好卡片
    page->applyProfile(prof);

    // 摄像头：地址只是登记，真正起流由用户点相机按钮（或 --set cam=on）触发。
    page->setCameraUrl(camUrl);
    page->setCameraFit(camFitCover);
    page->setCameraTuning(camFps, camFmt, camDecW, camDecH);
    page->setCameraNet(camTransport, camLatency, camDropOnLat);
    page->setCameraSinkBufs(camBufs);

    // ==================================================================
    //  主控链路：本机替代那台 Android 平板，当 WS 客户端
    // ------------------------------------------------------------------
    //  ./myui --ws ws://192.168.8.188:8080/ws        连主控
    //  ./myui --ws ws://... --ws-trace               同时打印每帧收发原文
    //  没给 --ws 时看环境变量 MYUI_WS（部署脚本/setenv 里配，省得改启动行）
    //  给了就切「远程驱动」：步进器点 ± 只发 ±1 意图，数值等主控广播回填。
    // ==================================================================
    if (wsUrl.isEmpty()) {
        const QString envWs = qEnvironmentVariable("MYUI_WS").trimmed();
        if (!envWs.isEmpty())
            wsUrl = QUrl(envWs);
    }
    if (!wsUrl.isEmpty() && wsUrl.isValid()) {
        DrLink *link = new DrLink(&app);
        link->setTrace(wsTrace);
        QObject::connect(link, &DrLink::varChanged, page, &MainWindow::setVar);
        QObject::connect(link, &DrLink::logLine, [](const QString &s) {
            qInfo("%s", qPrintable(s));
        });
        page->attachLink(link);
        link->start(wsUrl);
    } else if (!wsUrl.isEmpty()) {
        qWarning("!! --ws 地址非法: %s", qPrintable(wsUrl.toString()));
    }

    // 链路自检：延时后点一次步进器，验证「按钮 → 上行帧 → 主控回广播」整条线。
    //   --sim-step 2000                     依次点五个的「+」
    //   --sim-step 2000 --sim-key kv        只点 kV
    //   --sim-step 2000 --sim-key kv --sim-dir -1   点 kV 的「−」（复现抓包 #23518）
    if (simStepMs > 0) {
        const QStringList keys = simKeys.split(QLatin1Char(','), Qt::SkipEmptyParts);
        const int dir = simDir;
        QTimer::singleShot(simStepMs, page, [page, keys, dir]() {
            for (const QString &k : keys)
                page->debugStep(k.trimmed(), dir);
        });
    }

    // ==================================================================
    //  调试变量：让界面上每个数值/图标状态都能从外部改，不用改代码重编
    // ------------------------------------------------------------------
    //  ① ./myui --set "kv=120,aec=1,focal=large"   启动时一次设好
    //  ② ./myui --vars                             读当前目录 debug_vars.txt
    //     运行中改文件保存 → 120ms 后自动生效（界面立刻变）
    //  ③ ./myui --dump-vars                        打印所有 key 和取值范围
    //  以后接网口：收到的数据解析成 (key,value) 后调 page->setVar()，同一条路。
    // ==================================================================
    auto applyPair = [&](const QString &p, const char *src) {
        const int eq = p.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            qWarning("!! %s: '%s' 不是 key=value", src, qPrintable(p));
            return;
        }
        const QString key = p.left(eq).trimmed();
        const QString val = p.mid(eq + 1).trimmed();
        if (page->setVar(key, val))
            qDebug("var  %-9s = %-12s [%s]", qPrintable(key), qPrintable(val), src);
        else
            qWarning("!! %s: 不认识的变量 '%s=%s'（--dump-vars 看清单）",
                     src, qPrintable(key), qPrintable(val));
    };
    for (const QString &p : setPairs) {
        for (const QString &one : p.split(QLatin1Char(','), Qt::SkipEmptyParts))
            applyPair(one, "--set");
    }

    if (!watchFile.isEmpty()) {
        const QString absFile = QFileInfo(watchFile).absoluteFilePath();
        qDebug("watch vars file: %s", qPrintable(absFile));
        auto loadVarsFile = [&]() {
            QFile f(absFile);
            if (!f.open(QIODevice::ReadOnly)) {
                qWarning("!! 变量文件打不开: %s", qPrintable(absFile));
                return;
            }
            const QString txt = QString::fromUtf8(f.readAll());
            f.close();
            int n = 0;
            const QStringList lines = txt.split(QLatin1Char('\n'));
            for (const QString &raw : lines) {
                QString line = raw;
                const int hash = line.indexOf(QLatin1Char('#'));
                if (hash >= 0) line = line.left(hash);
                line = line.trimmed();
                if (line.isEmpty())
                    continue;
                const int eq = line.indexOf(QLatin1Char('='));
                if (eq <= 0)
                    continue;
                applyPair(line, "file");
                ++n;
            }
            qDebug("vars file 重载 %d 条  <- %s", n, qPrintable(absFile));
        };

        // 编辑器保存常常是「删了重建」，watcher 会丢掉这个 path，所以每次
        // 变化都重新 addPath；再用 120ms 防抖吃掉一次保存触发的多条事件。
        QFileSystemWatcher *watcher = new QFileSystemWatcher(&app);
        const QString absDir = QFileInfo(absFile).absolutePath();
        auto rearm = [watcher, absFile, absDir]() {
            if (QFile::exists(absFile) && !watcher->files().contains(absFile))
                watcher->addPath(absFile);
            if (!watcher->directories().contains(absDir))
                watcher->addPath(absDir);
        };
        rearm();
        QTimer *deb = new QTimer(&app);
        deb->setSingleShot(true);
        deb->setInterval(120);
        QObject::connect(deb, &QTimer::timeout, &app, loadVarsFile);
        auto kick = [deb]() { deb->start(); };
        QObject::connect(watcher, &QFileSystemWatcher::fileChanged, &app, kick);
        QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, &app, [rearm, deb]() {
            rearm();
            deb->start();
        });
        if (!QFile::exists(absFile))
            qWarning("!! 变量文件还不存在，先创建即可生效: %s", qPrintable(absFile));
        loadVarsFile();              // 启动先读一次
    }

    QGraphicsScene scene;
    QGraphicsProxyWidget *proxy = scene.addWidget(page);
    proxy->setPos(0, 0);
    scene.setSceneRect(0, 0, g_design.width(), g_design.height());

    ScaleView view(&scene);
    view.setWindowFlags(Qt::FramelessWindowHint);
    view.winId();                       // 先把 native window 建出来，才有 windowHandle
    if (pick >= 0 && pick < screens.size() && view.windowHandle()) {
        view.windowHandle()->setScreen(screens[pick]);
        qDebug("using screen[%d] '%s'", pick, qPrintable(screens[pick]->name()));
    }
    if (windowed) {
        // 桌面调试：不全屏，开一个普通窗口，好和编辑器并排放。
        // 注意窗口要按「旋转之后」的视觉尺寸开：竖屏设计 1200x1920 转 270° 后
        // 看到的是 1920x1200 横的；照 1200x1920 的 60% 开会得到 720x1152 的窄竖条，
        // 画面被压扁 2.5 倍，底栏挤成一条看不清（踩过）。
        const QSize vis = (g_rotDeg % 180) ? QSize(g_design.height(), g_design.width())
                                           : g_design;
        view.resize(vis.width() * 3 / 5, vis.height() * 3 / 5);
        // 居中显示，配合上面的置顶，边敲命令边看效果
        if (view.screen()) {
            const QRect av = view.screen()->availableGeometry();
            view.move(av.center() - QPoint(view.width() / 2, view.height() / 2));
        }
        view.showNormal();
        // 启动时提到最前（不要用 WindowStaysOnTopHint：那个窗口标志会让
        // QGraphicsProxyWidget 里的子控件不再重绘，整页退回静态底图）
        view.raise();
        view.activateWindow();
    } else {
        view.showFullScreen();
    }

    // ==================================================================
    //  顶栏右上角的隐形热区：点一下换版式（竖 ↔ 横），不重启
    // ------------------------------------------------------------------
    //  main.cpp 是唯一同时拿着 scene / view / page 的地方，所以换版式必须在这里做。
    //  必须同时改四处，少一处就出现「版式换了但缩放/旋转还是旧的」：
    //    ① g_design   —— relayout 用它算缩放比
    //    ② g_rotDeg   —— 竖屏 270 / 横屏 0
    //    ③ scene 矩形 —— 视图按它算居中
    //    ④ page->applyProfile —— 换底图 + 挪所有控件
    //  顺序不能反：先 applyProfile（页面尺寸变 → proxy 变），再设 scene 矩形，最后重算变换。
    // ==================================================================
    // ⚠ 冷却 700ms：手指点一下，触摸屏偶尔会上报两次（press-release 连着来两遍），
    //    或者用户连点两下 —— 翻过去又翻回来，净效果就是「点了没反应」。
    //    700ms 内只认第一次。
    qint64 lastFlipMs = 0;
    QObject::connect(page, &MainWindow::orientationToggleRequested, &view, [&]() {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (lastFlipMs && nowMs - lastFlipMs < 700) {
            qInfo("[flip] ignored (%lld ms since last)", static_cast<long long>(nowMs - lastFlipMs));
            return;
        }
        lastFlipMs = nowMs;
        landscape = !landscape;
        const dr::LayoutProfile &np = dr::profile(landscape);
        g_design = np.design;
        g_rotDeg = (rotArg >= 0) ? rotArg : np.rot;
        page->applyProfile(np);
        proxy->setPos(0, 0);
        // ⚠ 光靠 page->setFixedSize() 不够：QGraphicsProxyWidget 自己缓存了一份
        //   几何，页面在代理里改尺寸不会让它跟着变（实测换完仍是 1200x1920），
        //   于是画布变 1920x1200 而代理还是竖的 —— 右半边点不到任何控件。
        // ⚠ 光靠 page->setFixedSize() 不够：QGraphicsProxyWidget 在第一次 setWidget
        //   时把嵌入控件的 min/max 抄到了自己身上（竖屏 1200x1920），之后 resize
        //   一律被自己的约束夹回去（实测 setGeometry 立刻退回 1200x1920），
        //   于是画布变 1920x1200、代理还是竖的 —— 右半边点不到任何控件。
        //   必须先把代理的约束放开，再给它新尺寸。
        proxy->setMinimumSize(0, 0);
        proxy->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        proxy->resize(np.design);
        scene.setSceneRect(0, 0, np.design.width(), np.design.height());
        view.refresh();
        qInfo("[flip] -> %s  %dx%d rot=%d flip=%d%d", qPrintable(np.name),
              np.design.width(), np.design.height(),
              g_rotDeg, g_flipH ? 1 : 0, g_flipV ? 1 : 0);
    });

    // ==================================================================
    //  鼠标事件自检：--sim-click "x,y"
    // ------------------------------------------------------------------
    //  往 view 的视口里真发一对 press/release（坐标用设计稿坐标，交给
    //  QGraphicsView 的变换反算），用来验证「页签点了没反应」到底是
    //    a) 事件没送到（被别的控件挡住 / 控件隐藏）
    //    b) 送到了但只有 release、没有 press（ProxyWidget 不设抓取者）
    //  日志会分别打印 press / release 是否被接受，以及最终落到了哪个控件。
    // ==================================================================
    if (!simClickPt.isEmpty()) {
        const QString ptStr = simClickPt;
        const int dly = simClickDelay;
        // 支持 "x,y;x,y;..." 连点（间隔 --sim-click-gap，默认 700ms）：
        // 用来验「再点一次取消点亮」「运动中其他按钮锁死」这类有多步的交互。
        const QStringList seq = ptStr.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (int i = 0; i < seq.size(); ++i) {
            const QString one = seq[i].trimmed();
            QTimer::singleShot(dly + i * simClickGap, [&view, one]() {
                simClickDesign(view, one);
            });
        }
    }

    // 手指划动自检：--sim-drag "x1,y1,x2,y2"
    if (!simDragArg.isEmpty()) {
        const QString one = simDragArg;
        QTimer::singleShot(simDragDelay, [&view, one]() {
            simDragDesign(view, one);
        });
    }

    // 自检脚本用：到点正常退出（被 timeout 杀掉会丢最后几行日志）
    if (quitAfterMs > 0)
        QTimer::singleShot(quitAfterMs, qApp, &QCoreApplication::quit);

    // ------------------------------------------------------------------
    //  朝向候选图：--orient-sheet <dir>
    //  板子不在手边、或屏装歪了不确定该怎么转时，一次导出「正像 / 转 180 /
    //  左右镜像 / 上下镜像」四张，直接看哪张是正的就行。
    //  真正不同的朝向只有 4 种：rot180 ≡ mirrorHV，所以不必导 8 张。
    // ------------------------------------------------------------------
    if (!orientDir.isEmpty()) {
        QTimer::singleShot(shotDelay, [&]() {
            QDir().mkpath(orientDir);
            struct Cand { const char *name; int rot; bool fh; bool fv; };
            static const Cand cands[] = {
                {"rot0",    0,   false, false},
                {"rot180",  180, false, false},
                {"mirrorH", 0,   true,  false},
                {"mirrorV", 0,   false, true },
            };
            for (const Cand &c : cands) {
                view.setRot(c.rot);
                view.setFlip(c.fh, c.fv);
                QCoreApplication::processEvents();
                view.grab().save(orientDir + QStringLiteral("/orient_%1.png")
                                 .arg(QLatin1String(c.name)));
            }
            qDebug("orient sheet -> %s (rot0/rot180/mirrorH/mirrorV)",
                   qPrintable(orientDir));
            QCoreApplication::quit();
        });
    }

    // 离屏自检：导出「设计页」的关闭态/展开态，以及缩放旋转后的整屏。
    // 用于和 PHILIPS_TUI/*.png 做逐像素回归，不用接屏。
    // 横屏那套文件名统一加 _land 后缀，竖屏保持原样（历史基线不动）。
    if (!shotDir.isEmpty()) {
        QTimer::singleShot(shotDelay, [&]() {
            const QString tag = landscape ? QStringLiteral("_land") : QString();
            auto out = [&](const char *base) {
                return shotDir + QLatin1String("/") + QLatin1String(base)
                       + tag + QLatin1String(".png");
            };

            page->grab().save(out("page_closed"));
            view.grab().save(out("view_closed"));
            // 页签页自检：--set tab=view/position/stitching 之后导当前页
            page->grab().save(shotDir + QStringLiteral("/page_tab%1%2.png")
                              .arg(page->currentTab()).arg(tag));
            if (PatientCard *c = page->findChild<PatientCard *>()) {
                // 横屏还要多导一张「旋转角候选」，方便在实物上挑朝向：
                // 面板竖装时横屏内容可能是 0 / 90 / 180 / 270，肉眼一看就知道
                if (landscape) {
                    for (int r : {0, 90, 180, 270}) {
                        view.setRot(r);
                        QCoreApplication::processEvents();
                        view.grab().save(shotDir + QStringLiteral("/view_rot%1.png").arg(r));
                    }
                    view.setRot((rotArg >= 0) ? rotArg : prof.rot);
                    QCoreApplication::processEvents();
                }

                c->openPopupAt();
                QCoreApplication::processEvents();
                page->grab().save(out("page_open"));
                view.grab().save(out("view_open"));
                c->popup()->setHoverIndex(0);          // 悬停在 L 行（交互效果示意）
                QCoreApplication::processEvents();
                page->grab().save(out("page_hoverL"));
                c->closePopup();

                // 选中 L / S 后，卡片上的字母和图标都要跟着换（回归用）
                c->setValue(QStringLiteral("L"));
                QCoreApplication::processEvents();
                page->grab().save(out("page_L"));
                c->setValue(QStringLiteral("S"));
                QCoreApplication::processEvents();
                page->grab().save(out("page_S"));
                c->setValue(QStringLiteral("M"));
                QCoreApplication::processEvents();
                page->grab().save(out("page_M"));
            }

            // 站位卡片（Workspot）：page_ws_open 应与设计稿
            // PHILIPS_TUI/下拉弹窗2备份 8.png 逐像素一致
            if (WorkspotCard *w = page->findChild<WorkspotCard *>()) {
                w->openPopupAt();
                QCoreApplication::processEvents();
                page->grab().save(out("page_ws_open"));
                view.grab().save(out("view_ws_open"));
                w->popup()->setHoverIndex(1);          // 悬停在 Table 行
                QCoreApplication::processEvents();
                page->grab().save(out("page_ws_hoverT"));
                w->popup()->setHoverIndex(2);          // 悬停在 Free detector 行
                QCoreApplication::processEvents();
                page->grab().save(out("page_ws_hoverF"));
                w->closePopup();

                // 选 Table / Free detector：卡片上的图标要跟着换
                w->setIndex(1);
                QCoreApplication::processEvents();
                page->grab().save(out("page_ws_table"));
                w->setIndex(2);
                QCoreApplication::processEvents();
                page->grab().save(out("page_ws_free"));
                w->setIndex(0);
                QCoreApplication::processEvents();
            }

            // 三个「底图卡片」的下拉，分别对齐设计稿：
            //   page_focal  → 下拉弹窗2备份 9.png   (1920x1200 时是 下拉弹窗备份 9.png)
            //   page_target → 下拉弹窗2备份 10.png
            //   page_exp    → 下拉弹窗2备份 11.png
            // 展开时高亮的就是「当前值」那一行，所以只导一张即可对齐设计稿。
            auto shootDrop = [&](const char *objName, const char *base, int hoverAlt,
                                 int pickIdx) {
                if (ChoiceCard *c = page->findChild<ChoiceCard *>(QLatin1String(objName))) {
                    c->openPopupAt();
                    QCoreApplication::processEvents();
                    page->grab().save(out(base));
                    if (hoverAlt >= 0) {         // 额外导一张「悬停在另一行」
                        c->popup()->setHoverIndex(hoverAlt);
                        QCoreApplication::processEvents();
                        page->grab().save(shotDir + QLatin1String("/") + QLatin1String(base)
                                          + QLatin1String("_hover") + tag + QLatin1String(".png"));
                    }
                    c->closePopup();
                    QCoreApplication::processEvents();

                    // 选一个别的值 → 卡片上的图标/数值必须跟着变（回归用）
                    // pickIdx == -2 = 选最后一项：档位由 cards.json 配置后行数会变，
                    //   「最后一项」能顺带验证滚动到底（可见区要自动跟过去，滑块也要挪）
                    if (pickIdx >= 0 || pickIdx == -2) {
                        const int pi = (pickIdx == -2)
                                       ? qMax(0, c->popup()->count() - 1) : pickIdx;
                        c->setIndex(pi);
                        QCoreApplication::processEvents();
                        page->grab().save(shotDir + QLatin1String("/") + QLatin1String(base)
                                          + QLatin1String("_pick") + tag + QLatin1String(".png"));
                        // 再展开一次：压暗态下卡片也要显示新值（不能露出旧的烘焙值）
                        c->openPopupAt();
                        QCoreApplication::processEvents();
                        page->grab().save(shotDir + QLatin1String("/") + QLatin1String(base)
                                          + QLatin1String("_reopen") + tag + QLatin1String(".png"));
                        c->closePopup();
                        QCoreApplication::processEvents();
                    }
                }
            };
            // 顶部患者信息面板：点顶栏展开，整块覆盖患者卡区。最先导 —— 此时
            // 其它卡片都还是设计稿的初始值，面板底下那块才是纯底图。
            //   page_patinfo        → 患者信息2.png（横屏 患者信息.png）
            //   page_patinfo_closed → 关闭后必须回到与底图逐像素一致
            if (PatientInfoPanel *pi =
                    page->findChild<PatientInfoPanel *>(QLatin1String("patInfoPanel"))) {
                pi->openPanel();
                QCoreApplication::processEvents();
                page->grab().save(out("page_patinfo"));

                pi->closePanel();
                QCoreApplication::processEvents();
                page->grab().save(shotDir + QLatin1String("/page_patinfo_closed")
                                  + tag + QLatin1String(".png"));
            }

            // AEC 开关 + 参数步进器使能：
            //   page_aec_on   → 曝光2备份 3.png（横屏 曝光备份 3.png）：AEC 点亮 + DEN 那行变亮
            //   page_aec_off  → 再点一次必须回到与底图完全一致
            //   （曝光参数卡从构造起就是 kV−mAs，按钮文字已经和设计稿那版
            //     "kV-mA" 不同了 —— 这是以实机为准的改动，逐像素比对时这块
            //     会有差异，别再把它当回归失败）
            if (ToggleCard *a = page->findChild<ToggleCard *>(QLatin1String("cardAec"))) {
                a->setOn(true);
                QCoreApplication::processEvents();
                page->grab().save(out("page_aec_on"));
                a->setOn(false);
                QCoreApplication::processEvents();
                page->grab().save(shotDir + QLatin1String("/page_aec_off")
                                  + tag + QLatin1String(".png"));
            }

            // 曝光参数组合 → 步进器使能态（无对应设计稿整页，只做肉眼验收）：
            //   page_exp_kv     → 0 kV−mAs    ：kV + mAs 可调（按钮文案也是它）
            //   page_exp_kvmas  → 1 kV−mAs−ms ：kV + mAs + ms 可调
            //   page_exp_kvma   → 2 kV−mA−ms  ：kV + mA + ms 可调
            // 必须夹在 AEC 开关前后之间导：这时 AEC 亮着（DEN 才可调），
            // 而且 EL 卡还是底图烘焙的 320。
            auto shootMode = [&](int idx, const char *base) {
                if (ChoiceCard *c = page->findChild<ChoiceCard *>(QLatin1String("cardExpP"))) {
                    c->setIndex(idx);
                    QCoreApplication::processEvents();
                    page->grab().save(shotDir + QLatin1String("/") + QLatin1String(base)
                                      + tag + QLatin1String(".png"));
                }
            };
            if (ToggleCard *a = page->findChild<ToggleCard *>(QLatin1String("cardAec"))) {
                a->setOn(true);
                QCoreApplication::processEvents();
                shootMode(0, "page_exp_kv");
                shootMode(1, "page_exp_kvmas");
                shootMode(2, "page_exp_kvma");
                a->setOn(false);
                QCoreApplication::processEvents();
            }
            // 步进器数值 / 边界自检（没有设计稿，只做肉眼验收）：
            //   page_step_min  → kV 到下限 40，− 变灰
            //   page_step_max  → kV 到上限 150，+ 变灰
            //   page_step_vals → AEC 亮 + 一组真实值（验 mAs 一位小数、DEN 从「–」变数字）
            {
                auto st = [&](const char *n) {
                    return page->findChild<StepperCard *>(QLatin1String(n));
                };
                StepperCard *kv = st("stKv"), *ma = st("stMa"), *mas = st("stMas");
                StepperCard *ms = st("stMs"), *den = st("stDen");
                if (kv) {
                    kv->setValue(40.0);
                    QCoreApplication::processEvents();
                    page->grab().save(out("page_step_min"));
                    kv->setValue(150.0);
                    QCoreApplication::processEvents();
                    page->grab().save(out("page_step_max"));
                    kv->setValue(75.0);
                }
                if (den) den->setOn(true);
                if (ma)  ma->setValue(200.0);     // 联动：mAs 会跟着 mA×ms 走
                if (ms)  ms->setValue(50.0);
                if (mas) mas->setValue(12.5);     // 反算 ms
                QCoreApplication::processEvents();
                page->grab().save(out("page_step_vals"));
                // 还原成设计稿初值（后面几张还要跟底图比对）
                if (ma)  ma->setValue(80.0);
                if (ms)  ms->setValue(2.0);
                if (mas) mas->setValue(0.2);
                if (den) { den->setValue(0.0); den->setOn(false); }
                if (kv)  kv->setValue(75.0);
                QCoreApplication::processEvents();
            }
            // 还原成「没选过」的样子，后面几张（弹层/开关卡）才好比对底图
            if (ChoiceCard *c = page->findChild<ChoiceCard *>(QLatin1String("cardExpP")))
                c->resetToBaked();

            // 常亮开关卡（准星对中 / 跟随）：点亮态要与设计稿逐像素一致。
            //   page_center      → 对中2备份 2.png（横屏 对中备份 11.png）
            //   page_follow      → 对中2备份 4.png（横屏 对中备份 9.png）
            //   *_off            → 再点一次必须回到与底图完全一致
            auto shootToggle = [&](const char *objName, const char *base) {
                if (ToggleCard *t = page->findChild<ToggleCard *>(QLatin1String(objName))) {
                    t->setOn(true);
                    QCoreApplication::processEvents();
                    page->grab().save(out(base));
                    t->setOn(false);
                    QCoreApplication::processEvents();
                    page->grab().save(shotDir + QLatin1String("/") + QLatin1String(base)
                                      + QLatin1String("_off") + tag + QLatin1String(".png"));
                }
            };
            shootToggle("cardCenter", "page_center");
            shootToggle("cardFollow", "page_follow");

            // AEC 场域：弹层不是行列表（标题 + 人体图 + 三个可点亮场域），单独导。
            // 放在最前面导 page_aec，这样其它卡片还是设计稿的初始值。
            //   page_aec       → 下拉弹窗2备份 7.png（横屏 下拉弹窗备份 7.png）
            //   page_aec_multi → 再点亮左野（验证多选：右野 + 左野都实心）
            //   page_aec_pick  → 关闭态卡片上的小图跟着变
            if (AecFieldCard *c =
                    page->findChild<AecFieldCard *>(QLatin1String("cardAecField"))) {
                c->openPopupAt();
                QCoreApplication::processEvents();
                page->grab().save(out("page_aec"));

                c->setMask(c->mask() ^ 0x1);        // 点亮左野
                QCoreApplication::processEvents();
                page->grab().save(shotDir + QLatin1String("/page_aec_multi")
                                  + tag + QLatin1String(".png"));

                c->closePopup();
                QCoreApplication::processEvents();
                page->grab().save(shotDir + QLatin1String("/page_aec_pick")
                                  + tag + QLatin1String(".png"));

                c->setMask(0x2);                    // 还原，别影响后面的导出
                QCoreApplication::processEvents();
            }

            shootDrop("cardFocal", "page_focal",  1, 1);   // 另导 Large 悬停 + 选 Large
            shootDrop("cardTgtEi", "page_target", -1, 4);  // 选 125（3 位数，验证居中）
            // 再来一遍「选最后一项」：配置超过 5 档时这张会滚到底，滑块也变短
            shootDrop("cardTgtEi", "page_target_last", -1, -2);
            // 首屏态先单独导一张展开图：按钮上印的是 kV−mAs，弹层里被高亮的
            // 就必须是首行 kV−mAs（曾经是第 2 行 kV−mAs−ms，跟按钮文案对不上）
            if (ChoiceCard *c = page->findChild<ChoiceCard *>(QLatin1String("cardExpP"))) {
                c->setIndex(0);
                c->openPopupAt();
                QCoreApplication::processEvents();
                page->grab().save(out("page_exp_open"));
                c->closePopup();
                QCoreApplication::processEvents();
            }
            shootDrop("cardExpP",  "page_exp",    -1, 2);  // 选 kV-mA-ms（最长的那行）
            qDebug("shots -> %s", qPrintable(shotDir));
            QCoreApplication::quit();
        });
    }

    return app.exec();
}

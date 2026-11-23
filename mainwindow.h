#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>
#include <QToolButton>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class PatientCard;
class WorkspotCard;
class ChoiceCard;
class AecFieldCard;
class ToggleCard;
class StepperCard;
class PatientInfoPanel;
class InfoStrip;
class FooterBar;
class HeaderBar;
class ThumbCaption;
class DrLink;
class CamSource;
class TabBarPage;
class CameraOffPage;
class PositionPage;
class StitchingPage;
namespace dr { struct LayoutProfile; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 换版式：页面尺寸 + 底图 + 卡片（内部再转给 PatientCard）
    // 竖屏与横屏是两套独立的整页设计稿，不是把一套旋转/拉伸出来的，
    // 所以底图必须跟着换 —— 这是横屏和「转个角度」最大的区别。
    void applyProfile(const dr::LayoutProfile &p);

    // 截图自检用：当前页签下标（0 Exposure / 1 View / 2 Position / 3 Stitching）
    int  currentTab() const { return m_tab; }

    // ==================================================================
    //  调试变量通道（接网口通讯数据前的替身）
    // ------------------------------------------------------------------
    //  界面上的每个可显示变量都有一个 key，调用 setVar() 即可改。
    //  main.cpp 用两条路径喂进来：
    //    --set "kv=120,ma=200"     启动时一次性设置
    //    --vars debug_vars.txt     运行中热加载：改文件保存，界面立刻变
    //  以后接网口：收到协议帧 → 解析成 (key,value) → 调同一个 setVar()。
    //  变量清单见 varKeys()，以及 share/UI/界面功能说明.md 的「调试变量手册」。
    // ==================================================================
    bool setVar(const QString &key, const QString &value);   // false = key/值非法
    void resetVars();                                        // 回设计稿初值
    static QStringList varKeys();                            // key 清单（打印用）

    // ==================================================================
    //  接主控链路（DrLink = WS 客户端，替代那台 Android 平板）
    // ------------------------------------------------------------------
    //  只在 main.cpp 里调一次，不接管 link 的所有权。挂上之后：
    //    · 下行 varChanged(key,value) → 直接进 setVar()，界面代码零改动
    //    · 上行 StepperCard::stepped(dir) → link->sendStep()，只发 ±1
    //    · 五个步进器切「远程驱动」：本地不预测数值，等广播落定
    //    · 300ms 没等到广播的那个方向 → 单侧变灰（到底了）
    // ==================================================================
    void attachLink(DrLink *link);

    // 自检用：程序化地「点一下」某个步进器的 ± 格子（走真正的鼠标事件路径，
    // 包括 remote 判断、长按定时器、stepped 信号），不从鼠标进也能验上行链路。
    // 只给 --sim-step 用；正常运行时没人调。
    void debugStep(const QString &key, int dir);

    // ==================================================================
    //  摄像头（View 页）：RTSP → GStreamer 硬解 → QImage → paintEvent
    // ------------------------------------------------------------------
    //  帧不走任何原生视频控件：整页套着 rotate270 + scale，原生窗口/subsurface
    //  不参与这个变换，会变成「不旋转的黑块」，--shot 也截不到。
    // ==================================================================
    void setCameraUrl(const QString &url);   // 空 = 不开流
    void setCameraFit(bool cover);           // true=铺满裁边 / false=内切留边
    // 取流调优（压 CPU）：限帧 fps（0=跟源）、解码输出格式 fmt（NV12/BGRx/
    // BGRA/RGB）、解码输出尺寸 decW×decH（0=原始）。下次 start() 生效。
    void setCameraTuning(int fps, const QString &fmt, int decW, int decH);
    // 传输/缓冲（延迟相关）：transport = auto/udp/tcp，latencyMs = jitterbuffer
    void setCameraNet(const QString &transport, int latencyMs, bool dropOnLat);
    void setCameraSinkBufs(int n);       // appsink 队列（1 = 只留最新一帧）
    void startCamera();                      // 开流 → CamOn
    void stopCamera();                       // 停流 → CamOff
    bool cameraRunning() const { return m_camOn; }

signals:
    // 顶栏右上角的隐形热区被点：请 main.cpp 换成另一套版式（横 ↔ 竖）。
    // MainWindow 自己不认识 scene/view，换版式必须由 main.cpp 来做
    // （改 g_design、scene 矩形、view 的 rot，再重新 applyProfile）。
    void orientationToggleRequested();

private slots:
    void on_pushButton_clicked();

private:
    Ui::MainWindow *ui;
    PatientCard  *m_card = nullptr;
    WorkspotCard *m_ws   = nullptr;
    // 底图上已有图形的三张卡片：只有命中区 + 下拉弹层，自己不绘制
    ChoiceCard   *m_focal = nullptr;    // 焦点卡片（大/小焦点） → Focal Spot
    ChoiceCard   *m_tgtEi = nullptr;    // EL 320      → Target exposure index
    ChoiceCard   *m_expP  = nullptr;    // kV-mAs     → Exposure parameters
    // 人体场域卡片（spotId "view"）→ AEC 场域弹层（可多选）
    AecFieldCard *m_aecField = nullptr;
    // 两张「单击点亮、再单击熄灭」的开关卡：准星对中（laser）/ 跟随（collim）
    ToggleCard   *m_center = nullptr;
    ToggleCard   *m_follow = nullptr;
    // AEC 开关卡 + 它带出来的 5 个参数步进器
    ToggleCard   *m_aecBtn = nullptr;
    StepperCard  *m_stKv   = nullptr;
    StepperCard  *m_stMa   = nullptr;
    StepperCard  *m_stMas  = nullptr;
    StepperCard  *m_stMs   = nullptr;
    StepperCard  *m_stDen  = nullptr;
    bool          m_syncing = false;   // mA / ms / mAs 联动时的重入保护
    // 曝光参数组合 → 哪几个步进器可调（0 = 首屏 kV−mAs；-1 仅逐像素自检还原态）
    void applyExposureMode(int idx);
    // 顶栏患者信息：一个隐形热区（图标 + 姓名 + 三角那块）+ 一块整宽覆盖面板
    QToolButton       *m_patHot  = nullptr;
    PatientInfoPanel  *m_patInfo = nullptr;
    // 顶栏右端的隐形热区（临时调试用）：点一下横屏、再点回竖屏，见 mainwindow.cpp
    QWidget           *m_flipBtn = nullptr;

    // 读数区（Req.SID / 身高 / 角度 / 支架高）与底栏（探测器 / 滤过 / 球管 / HU）
    // —— 这两块在底图里是烘焙像素，由这两个控件铺底色盖掉后重画，见 statusstrip.h
    InfoStrip        *m_info   = nullptr;
    FooterBar        *m_footer = nullptr;
    HeaderBar        *m_header = nullptr;
    ThumbCaption     *m_caption = nullptr;

    DrLink           *m_link   = nullptr;   // 主控链路（可能为空 = 本地演示模式）

    // ---- 四个页签：Exposure / View / Position / Stitching ----
    //  底图只烘焙了 Exposure 那一页，切页 = 盖住内容区重画；
    //  Exposure 页的控件要整体隐藏，回来时再显示（见 showTab）。
    TabBarPage     *m_tabs   = nullptr;
    CameraOffPage  *m_viewPage = nullptr;
    PositionPage   *m_posPage  = nullptr;
    StitchingPage  *m_stitchPage = nullptr;
    int             m_tab = 0;
    void showTab(int i);
    void wireTabPage();                     // 四个页签的信号/回调接线
    void onGoToPosition();

    // 摄像头运行态（CamSource 归自己 new/delete）
    CamSource      *m_camSource = nullptr;
    QString         m_camUrl;
    bool            m_camOn   = false;
    bool            m_camFitCover = true;
    int             m_camFps   = 15;
    QString         m_camFmt   = QStringLiteral("NV12");
    int             m_camDecW  = 0;
    int             m_camDecH  = 0;
    qint64          m_lastCamToggle = 0;   // 已废弃：去重改在 CameraOffPage 按事件时间戳做
    QString         m_camTransport = QStringLiteral("auto");
    int             m_camLatencyMs = 0;
    bool            m_camDropOnLat = false;
    int             m_camSinkBufs = 1;

    void applyCameraTuning();
};
#endif // MAINWINDOW_H

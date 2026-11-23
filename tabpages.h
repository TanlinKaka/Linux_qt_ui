#ifndef TABPAGES_H
#define TABPAGES_H

#include <QWidget>
#include <QString>
#include <QVector>
#include <QImage>
#include <QRect>

// =====================================================================
//  Exposure / View / Position / Stitching 四个页签
// ---------------------------------------------------------------------
//  底图（整页 PNG）里烘焙的是 Exposure 那一页，页签行也烘焙着「Exposure 亮、
//  蓝色下划线在 Exposure 下面」。要切页就必须把这两块都盖掉重画：
//
//    TabBarPage   只盖页签行
//    CameraOffPage    盖页内容区，画 View 页（相机未开启 / 已开启）
//    PositionPage     盖页内容区，画 Position 页（两栏角度 + Go to position）
//    StitchingPage    盖页内容区，画 Stitching 页（Top/Bottom + Reset/Apply）
//
//  三者都不是独立窗口，是页面内子控件 —— 和弹层一样要跟着整页 rotate/缩放。
//
//  两套版式都有设计稿（2026-11-20 确认）：
//    竖屏 1200x1920：页内容区 (0,684)-(1200,1744)
//    横屏 1920x1200：页内容区 (840,120)-(1912,1032)   ← 左侧患者栏/卡片/读数一直保留
//  横屏的页控件**只盖右侧这一块**，左边那 840px 是常驻区域（缩略图 + M 卡 +
//  Req.SID 读数），切页时不能盖掉，这是和竖屏最大的结构差别。
// =====================================================================

class TabBarPage : public QWidget
{
    Q_OBJECT
public:
    explicit TabBarPage(QWidget *parent = nullptr);

    void setActive(int i);
    int  active() const { return m_active; }
    bool landscape() const { return m_land; }
    void applyProfile(bool landscape);

signals:
    void tabClicked(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    //  按下必须 accept：整页装在 QGraphicsView 里，QGraphicsProxyWidget 只有在
    //  按下被接受时才会把它登记为「鼠标抓取者」，否则松手事件送不回来
    //  （表现为「点了没反应」，而桌面上单独跑 QWidget 又一切正常）。
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    int  indexAt(const QPoint &pt) const;
    int  m_active = 0;
    int  m_press  = -1;
    bool m_land   = false;
};

// ---------------------------------------------------------------------
//  View 页：相机未开启
// ---------------------------------------------------------------------
class CameraOffPage : public QWidget
{
    Q_OBJECT
public:
    // 三态（设计稿本来就有三张：摄像机关闭 / 摄像机 / 实时摄像机不可使用）
    enum CamState { CamOff = 0, CamOn = 1, CamError = 2 };

    explicit CameraOffPage(QWidget *parent = nullptr);
    void applyProfile(bool landscape);

    void    setCameraState(CamState s);
    CamState cameraState() const { return m_state; }
    void    setCameraOn(bool on) { setCameraState(on ? CamOn : CamOff); }  // 兼容旧调用

    void setCameraFrame(const QImage &img);   // 收到一帧（RGB，尺寸随意，页面自己裁）
    void clearFrame();
    void setFitCover(bool cover) { m_cover = cover; }   // true=铺满裁边 / false=内切留边
    QRect photoRect() const { return m_photo; }         // 画面区（相对本控件）

signals:
    void cameraToggle();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    void rebuildScaled();       // 只算「源矩形 → 画面矩形」的映射，不生成中间图
    // 一次物理点击会派生出多组 press/release（见 mouseReleaseEvent 里的说明），
    // 这里按「事件时间戳」判定是不是同一击 —— 不能用处理时刻。
    bool gestureAccepted(qint64 evtTs, qint64 now);

    CamState m_state = CamOff;
    bool m_land  = false;
    bool m_cover = true;
    bool m_pressHit = false;
    bool m_pressed  = false;    // 按下未抬起（画反馈用）
    qint64 m_lastEvtTs   = 0;   // 上一次已接受点击的事件时间戳（ms）
    qint64 m_lastWallMs  = 0;   // 兜底：事件没带时间戳时用系统时钟
    QRect m_btn;
    QRect m_btnHit;             // 命中区：比可视按钮四周各外扩 32 设计 px
    QRect m_photo;              // 画面矩形（相对本控件）
    QImage m_frame;             // 原始帧
    QRect  m_src;               // 从 m_frame 里取的源矩形（cover 时是居中裁出的那块）
    QRect  m_dst;               // 画到本控件的哪个矩形（cover=m_photo，contain=居中内切）
};

// ---------------------------------------------------------------------
//  Position 页：左侧 4 档机架倾角模式 + 角度按钮 + Go to position
//    模式 0/1 = 两栏（SID 110 + 180），模式 2/3 = 单栏（只有 110）；
//    模式 0/2 = 正角，模式 1/3 = 负角（以实机照片为准，见 rebuildCells）
// ---------------------------------------------------------------------
class PositionPage : public QWidget
{
    Q_OBJECT
public:
    explicit PositionPage(QWidget *parent = nullptr);
    void applyProfile(bool landscape);

    int  tiltMode() const { return m_mode; }        // 左侧竖条选中项 0..3
    int  angleA()  const { return m_angA; }         // -1 = 未选
    int  angleB()  const { return m_angB; }
    bool moving()  const { return m_statusKind == 1; }   // 运动中：按钮全锁
    bool goToEnabled() const;                       // 有选中 且 不在运动/已到位

    void setTiltMode(int i);        // 0/1 = 两栏(110+180) 正/负角；2/3 = 单栏 正/负角
    void setAngle(int panel, int value);            // panel 0/1，value = 角度值
    void setStatus(const QString &text, int kind);  // kind: 0 无 / 1 警示 / 2 到位

signals:
    void tiltModeChanged(int mode);
    void anglePicked(int panel, int value);
    void goToPosition();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    struct Cell { int panel; int value; QRect r; };
    void rebuildCells();
    int  cellAt(const QPoint &pt) const;
    int  targetAt(const QPoint &pt) const;

    bool m_land = false;
    // 按下时记住命中的目标（0..3 竖条 / 100+cell / -1 空 / -2 GoTo），
    // 松手必须命中同一个目标才算一次点击，防止滑动误触。
    int  m_press = -1;
    int  m_mode = 0;
    int  m_angA = -1;
    int  m_angB = -1;
    QString m_status;
    int  m_statusKind = 0;
    QVector<Cell> m_cells;
    QRect m_strip[4];
    QRect m_goTo;
};

// ---------------------------------------------------------------------
//  Stitching 页：Top / Bottom 起始位 + Reset / Apply
//    竖屏设计稿（拼接2备份.png）也有，但目前只实现横屏版式，
//    竖屏仍只铺底色（与旧 BlankPage 行为一致）。
// ---------------------------------------------------------------------
class StitchingPage : public QWidget
{
    Q_OBJECT
public:
    explicit StitchingPage(QWidget *parent = nullptr);
    void applyProfile(bool landscape);

    void setStartPosition(int i);        // 0 = Top，1 = Bottom
    int  startPosition() const { return m_start; }

signals:
    void startPositionChanged(int index);
    void resetClicked();
    void applyClicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    int  targetAt(const QPoint &pt) const;   // 0 Top / 1 Bottom / 2 Reset / 3 Apply / -1
    bool m_land  = false;
    int  m_press = -1;
    int  m_start = 0;
    QRect m_top, m_bot, m_reset, m_apply;
};

#endif // TABPAGES_H

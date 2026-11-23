#ifndef DRLINK_H
#define DRLINK_H

// ===========================================================================
//  DrLink —— 主控 WebSocket 客户端（替代那台 Android 平板）
// ---------------------------------------------------------------------------
//  角色：本程序是 **WS 客户端**，连主控 uDR660 的 ws://<ip>:8080/ws。
//        和原来的 Android 触控屏在链路上完全等价，可以互换。
//
//  协议（见 128_Net/PTO/PLIP_分析/触控屏指令协议对照.md）：
//    · 文本帧 JSON，统一 {"code":<int>,"data":{...}}
//    · 服务端 → 客户端：2000+ 状态广播、100/101 保活、4009 部位库应答
//    · 客户端 → 服务端：4001~4008 操作意图，**只发 ±1，不发绝对值**
//    · 没有 ack / 没有 id / 没有序号 —— 只能等广播回来落定
//
//  三条硬规矩（抓包实测，别自己发明）：
//    1. 步进只发 {"val":±1}，mA/ms/mAs 是主控查表跳档，UI 里不要预测数值；
//    2. 到底了主控**静默忽略**（不报错不回包）→ 靠「300ms 没等到广播」判到底，
//       对应界面上「边界单侧变灰」；
//    3. 心跳是客户端每 5s 发 {"code":100,"data":{}}，15s 无下行视为断线。
//
//  与界面的耦合面只有两条：
//    · 下行  varChanged(key, value)  → MainWindow::setVar(key, value)
//    · 上行  sendStep(param, dir)    ← StepperCard::stepped(dir, value)
//  界面代码一行都不用改（setVar 就是当初为接网口留的那个口子）。
// ===========================================================================

#include <QObject>
#include <QUrl>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QHash>
#include <QStringList>

class QWebSocket;

// 上行可发意图的参数（只读项 SID/光野/滤过/部位/机架 一律不做）
enum class DrParam {
    None = 0,
    Kv,      // 4001
    Ma,      // 4002
    Ms,      // 4003
    Mas,     // 4004
    Den,     // 4005（上下行同 code）
    AecField // 4008（场域，val=±1/±2/±3；先留口不用）
};

class DrLink : public QObject
{
    Q_OBJECT
public:
    explicit DrLink(QObject *parent = nullptr);
    ~DrLink() override;

    // 连上主控；连接失败会自动重连（1s→2s→4s→8s 封顶）
    void start(const QUrl &url);
    void stop();
    bool isConnected() const;

    QUrl    url()        const { return m_url; }
    QString stateText()  const;          // 给顶栏/日志看的一行状态

    void setTrace(bool on) { m_trace = on; }        // 打印每帧收发原文
    bool trace()     const { return m_trace; }

    // ---- 上行：按钮意图 ----
    void sendStep(DrParam p, int dir);              // dir = +1 / -1
    void sendAecField(int delta);                   // 4008
    void sendFocus(int idx);                        // 2005 大/小焦点（绝对值）
    void sendEi(int value);                         // 3020 EI 目标（绝对值）
    void sendMode(int mode);                        // 2006 曝光模式（绝对值）

signals:
    void connected();
    void disconnected();
    void varChanged(const QString &key, const QString &value);
    // 广播确认「这一档真的变了」/ 300ms 内没等到 → 到底
    void stepConfirmed(DrParam p, int dir);
    void stepAtLimit(DrParam p, int dir);
    void logLine(const QString &s);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString &msg);
    void onError();
    void onHeartbeat();
    void onWatchdog();
    void reconnect();

private:
    void sendJson(int code, const QJsonObject &data);
    void handleFrame(int code, const QJsonObject &data);
    void notePending(DrParam p, int dir, int code);   // 记一条「等回执」
    void clearPending(DrParam p);                     // 该参数回执到了
    void trimAecMask();                               // 三路合成 aecfield

    QWebSocket *m_ws = nullptr;
    QUrl        m_url;
    bool        m_trace    = false;
    bool        m_started  = false;
    bool        m_retry    = false;
    int         m_backoff  = 1000;      // 重连退避 ms
    QTimer     *m_hb       = nullptr;   // 5s 心跳
    QTimer     *m_wd       = nullptr;   // 3s 巡视 pending
    QElapsedTimer m_lastRx;             // 最后一次收到下行的时刻

    // 等回执：param → (dir, code, 发起时刻)
    struct Pending { int dir; int code; qint64 ms; };
    QHash<int, Pending> m_pending;

    // AEC 三路缓存（2017/2018/2019 分三帧到，合成一个 mask 给界面）
    int m_aecL = 0, m_aecC = 0, m_aecR = 0;
    bool m_aecSeen = false;

    // 最近一次下行值（trace / 排错用）
    QHash<QString, QString> m_last;
};

#endif // DRLINK_H

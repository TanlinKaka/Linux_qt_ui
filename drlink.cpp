#include "drlink.h"

#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonValue>
#include <QDateTime>
#include <QTimerEvent>
#include <QtMath>
#include <QDebug>

// ---------------------------------------------------------------------------
//  上行 code 表（客户端能发的全部意图）
// ---------------------------------------------------------------------------
static int codeOfParam(DrParam p)
{
    switch (p) {
    case DrParam::Kv:  return 4001;
    case DrParam::Ma:  return 4002;
    case DrParam::Ms:  return 4003;
    case DrParam::Mas: return 4004;
    case DrParam::Den: return 4005;
    default:           return 0;
    }
}

// 下行 code → 参数（用来判「回执」；DEN 上下行同为 4005）
static QString paramName(DrParam p)
{
    switch (p) {
    case DrParam::Kv:  return QStringLiteral("kV");
    case DrParam::Ma:  return QStringLiteral("mA");
    case DrParam::Ms:  return QStringLiteral("ms");
    case DrParam::Mas: return QStringLiteral("mAs");
    case DrParam::Den: return QStringLiteral("DEN");
    case DrParam::AecField: return QStringLiteral("AEC");
    default: return QStringLiteral("?");
    }
}

// JSON 值 → 界面 setVar 用的字符串
static QString val2str(const QJsonValue &v)
{
    if (v.isString())  return v.toString().trimmed();
    if (v.isBool())    return v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (qAbs(d - qRound(d)) < 1e-9)
            return QString::number(qRound(d));
        return QString::number(d, 'g', 6);
    }
    if (v.isArray() || v.isObject())
        return QString::fromUtf8(QJsonDocument(v.isArray()
                                               ? QJsonDocument(v.toArray())
                                               : QJsonDocument(v.toObject()))
                                 .toJson(QJsonDocument::Compact));
    return QString();
}

// 取 data 里第一个命中的字段（协议字段名大小写/拼写有过不一致，容错一下）
static QJsonValue pick(const QJsonObject &d, const char *k1,
                       const char *k2 = nullptr, const char *k3 = nullptr)
{
    if (d.contains(QLatin1String(k1))) return d.value(QLatin1String(k1));
    if (k2 && d.contains(QLatin1String(k2))) return d.value(QLatin1String(k2));
    if (k3 && d.contains(QLatin1String(k3))) return d.value(QLatin1String(k3));
    return QJsonValue();
}

DrLink::DrLink(QObject *parent)
    : QObject(parent)
{
    m_hb = new QTimer(this);
    m_hb->setInterval(5000);                 // 抓包：客户端心跳 5s
    connect(m_hb, &QTimer::timeout, this, &DrLink::onHeartbeat);

    m_wd = new QTimer(this);
    m_wd->setInterval(100);                  // 巡视「等了多久还没回执」
    connect(m_wd, &QTimer::timeout, this, &DrLink::onWatchdog);

    m_wd->start();
}

DrLink::~DrLink()
{
    stop();
}

void DrLink::start(const QUrl &url)
{
    m_url     = url;
    m_started = true;
    m_backoff = 1000;

    if (!m_ws) {
        m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        // 客户端必须掩码（服务端才收）——Qt 默认就是 MASK=1，这里不改
        connect(m_ws, &QWebSocket::connected,    this, &DrLink::onConnected);
        connect(m_ws, &QWebSocket::disconnected, this, &DrLink::onDisconnected);
        connect(m_ws, &QWebSocket::textMessageReceived, this, &DrLink::onTextMessage);
        connect(m_ws, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
                this, &DrLink::onError);
    }
    if (m_ws->state() == QAbstractSocket::ConnectedState)
        return;

    emit logLine(QStringLiteral("link: connecting %1").arg(url.toString()));
    m_ws->open(url);
}

void DrLink::stop()
{
    m_started = false;
    if (m_hb) m_hb->stop();
    if (m_ws) {
        if (m_ws->state() != QAbstractSocket::UnconnectedState)
            m_ws->close();
    }
    m_pending.clear();
}

bool DrLink::isConnected() const
{
    return m_ws && m_ws->state() == QAbstractSocket::ConnectedState;
}

QString DrLink::stateText() const
{
    if (!m_started) return QStringLiteral("link off");
    if (!m_ws)      return QStringLiteral("link idle");
    switch (m_ws->state()) {
    case QAbstractSocket::ConnectedState:      return QStringLiteral("link up  %1").arg(m_url.toString());
    case QAbstractSocket::ConnectingState:     return QStringLiteral("link ... %1").arg(m_url.toString());
    case QAbstractSocket::HostLookupState:     return QStringLiteral("link dns");
    default:                                   return QStringLiteral("link down (retry %1s)").arg(m_backoff / 1000);
    }
}

// ---------------------------------------------------------------------------
//  连接 / 断开 / 错误
// ---------------------------------------------------------------------------
void DrLink::onConnected()
{
    m_backoff = 1000;
    m_lastRx.start();
    m_hb->start();
    emit logLine(QStringLiteral("link: connected"));
    emit connected();

    // 连接后照平板的做法拉一次部位库（4009 空载荷），
    // 顺带把主控的当前状态催一遍，免得首屏停在设计稿的假值上。
    QJsonObject empty;
    sendJson(4009, empty);
}

void DrLink::onDisconnected()
{
    m_hb->stop();
    m_pending.clear();
    emit logLine(QStringLiteral("link: disconnected"));
    emit disconnected();
    if (m_started)
        reconnect();
}

void DrLink::onError()
{
    if (!m_started || !m_ws) return;
    emit logLine(QStringLiteral("link: error %1 (%2)")
                 .arg(int(m_ws->error())).arg(m_ws->errorString()));
    // 没连上时也要重试（disconnected 不一定发）
    if (m_ws->state() == QAbstractSocket::UnconnectedState) {
        m_hb->stop();
        reconnect();
    }
}

void DrLink::reconnect()
{
    if (m_retry || !m_started) return;
    m_retry = true;
    const int delay = m_backoff;
    m_backoff = qMin(m_backoff * 2, 8000);
    QTimer::singleShot(delay, this, [this]() {
        m_retry = false;
        if (m_started && m_ws && m_ws->state() == QAbstractSocket::UnconnectedState) {
            emit logLine(QStringLiteral("link: retry %1").arg(m_url.toString()));
            m_ws->open(m_url);
        }
    });
}

// ---------------------------------------------------------------------------
//  收：下行帧
// ---------------------------------------------------------------------------
void DrLink::onTextMessage(const QString &msg)
{
    m_lastRx.restart();

    QJsonParseError err {};
    const QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        emit logLine(QStringLiteral("link: bad json (%1) %2")
                     .arg(err.errorString(), QString::fromUtf8(msg.left(120).toUtf8())));
        return;
    }
    if (!doc.isObject()) {
        if (m_trace) emit logLine(QStringLiteral("rx[] ") + msg.left(200));
        return;
    }
    const QJsonObject root = doc.object();
    const int code = root.value(QStringLiteral("code")).toInt(-1);
    const QJsonObject data = root.value(QStringLiteral("data")).toObject();

    if (m_trace)
        emit logLine(QStringLiteral("rx %1 %2").arg(code).arg(
            QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)).left(200)));

    handleFrame(code, data);
}

void DrLink::handleFrame(int code, const QJsonObject &d)
{
    auto emitVar = [this](const char *key, const QString &v) {
        if (v.isEmpty()) return;
        const QString k = QString::fromLatin1(key);
        if (m_last.value(k) == v) return;     // 同值不重复刷（主控会连发）
        m_last[k] = v;
        emit varChanged(k, v);
    };

    switch (code) {
    // ---------------- 曝光参数 ----------------
    case 2001: emitVar("kv",  val2str(pick(d, "KV")));   clearPending(DrParam::Kv);  break;
    case 2002: emitVar("ma",  val2str(pick(d, "MA")));   clearPending(DrParam::Ma);  break;
    case 2003: emitVar("ms",  val2str(pick(d, "MS")));   clearPending(DrParam::Ms);  break;
    case 2004: emitVar("mas", val2str(pick(d, "MAS")));  clearPending(DrParam::Mas); break;
    case 4005:                                           // DEN 上下行同 code
        emitVar("den", val2str(pick(d, "DEN")));
        clearPending(DrParam::Den);
        break;

    // 焦点：抓包实测 Focus=0 大焦点 / 1 小焦点
    case 2005: {
        const QJsonValue f = pick(d, "Focus");
        if (!f.isUndefined())
            emitVar("focal", f.toInt() == 0 ? QStringLiteral("large")
                                            : QStringLiteral("small"));
        break;
    }

    // 球管热容量 HU（百分比）
    case 2007: emitVar("hu", val2str(pick(d, "HU"))); break;

    // 光野/FOV 尺寸：{"x":42,"y":42} → 界面 det=42x42（只读，仅显示）
    case 2012: {
        const QString x = val2str(pick(d, "x", "X"));
        const QString y = val2str(pick(d, "y", "Y"));
        if (!x.isEmpty())
            emitVar("det", y.isEmpty() ? x : QStringLiteral("%1x%2").arg(x, y));
        break;
    }

    // SID（只读，仅显示）
    case 2013: emitVar("sid", val2str(pick(d, "SID"))); break;

    // AEC 三路电离室：分三帧到，合成一个 mask 给界面（0x1 左 / 0x2 中 / 0x4 右）
    case 2017: m_aecL = pick(d, "AECL").toInt(0); m_aecSeen = true; trimAecMask(); break;
    case 2018: m_aecC = pick(d, "AECC").toInt(0); m_aecSeen = true; trimAecMask(); break;
    case 2019: m_aecR = pick(d, "AECR").toInt(0); m_aecSeen = true; trimAecMask(); break;

    // 立位/卧位（2008 只有 wallTable 一个数，语义未定；3016 字段名明确）
    case 3016: {
        const int wall  = pick(d, "wall").toInt(0);
        const int table = pick(d, "table").toInt(0);
        const int free_ = pick(d, "free").toInt(0);
        if (free_ > 0)      emitVar("workspot", QStringLiteral("free"));
        else if (table > 0) emitVar("workspot", QStringLiteral("table"));
        else if (wall > 0)  emitVar("workspot", QStringLiteral("wallstand"));
        break;
    }

    // ---------------- 患者 / 部位 ----------------
    case 2034: emitVar("name", val2str(pick(d, "NAME", "name"))); break;
    case 2009: emitVar("part", val2str(pick(d, "ItemName", "itemName"))); break;

    // ---------------- 探测器 ----------------
    case 3001: case 3002: {
        // flag 语义未定，先只在 trace 里留痕（界面 deta/detb 暂不接）
        if (m_trace) emit logLine(QStringLiteral("det%1 flag=%2")
                                  .arg(code - 3000).arg(val2str(pick(d, "flag"))));
        break;
    }
    case 3007: emitVar("hutemp", val2str(pick(d, "temperature"))); break;

    // ---------------- 剂量 / 滤过 ----------------
    case 3023: emitVar("filt", val2str(pick(d, "Filter"))); break;

    // ---------------- 告警文本（界面暂无位置，先落日志）----------------
    case 2015: emit logLine(QStringLiteral("sys: %1").arg(val2str(pick(d, "SysStatus")))); break;
    case 2020: emit logLine(QStringLiteral("err: %1 level=%2")
                            .arg(val2str(pick(d, "info")), val2str(pick(d, "level")))); break;

    // ---------------- 保活 ----------------
    case 100: case 101:
        break;

    default:
        if (m_trace) {
            emit logLine(QStringLiteral("rx ? code=%1 %2").arg(code).arg(
                QString::fromUtf8(QJsonDocument(d).toJson(QJsonDocument::Compact)).left(160)));
        }
        break;
    }
}

void DrLink::trimAecMask()
{
    if (!m_aecSeen) return;
    int mask = 0;
    if (m_aecL) mask |= 0x1;
    if (m_aecC) mask |= 0x2;
    if (m_aecR) mask |= 0x4;
    const QString v = QString::number(mask);
    if (m_last.value(QStringLiteral("aecfield")) == v) return;
    m_last[QStringLiteral("aecfield")] = v;
    emit varChanged(QStringLiteral("aecfield"), v);
}

// ---------------------------------------------------------------------------
//  发：上行意图
// ---------------------------------------------------------------------------
void DrLink::sendJson(int code, const QJsonObject &data)
{
    if (!m_ws || m_ws->state() != QAbstractSocket::ConnectedState) {
        if (m_trace) emit logLine(QStringLiteral("tx drop (not connected) code=%1").arg(code));
        return;
    }
    QJsonObject root;
    root.insert(QStringLiteral("code"), code);
    root.insert(QStringLiteral("data"), data);
    const QByteArray payload =
        QJsonDocument(root).toJson(QJsonDocument::Compact);

    m_ws->sendTextMessage(QString::fromUtf8(payload));
    if (m_trace) emit logLine(QStringLiteral("tx %1").arg(QString::fromUtf8(payload)));
}

void DrLink::sendStep(DrParam p, int dir)
{
    const int code = codeOfParam(p);
    if (!code || dir == 0) return;
    dir = dir > 0 ? 1 : -1;

    QJsonObject d;
    d.insert(QStringLiteral("val"), dir);       // 只发 ±1，绝不发绝对值
    sendJson(code, d);
    notePending(p, dir, code);
}

void DrLink::sendAecField(int delta)
{
    if (delta == 0) return;
    QJsonObject d;
    d.insert(QStringLiteral("val"), delta);
    sendJson(4008, d);
}

void DrLink::sendFocus(int idx)
{
    QJsonObject d;
    d.insert(QStringLiteral("Focus"), idx == 0 ? 0 : 1);   // 0=大 1=小
    sendJson(2005, d);
}

void DrLink::sendEi(int value)
{
    QJsonObject d;
    d.insert(QStringLiteral("EIValue"), value);
    sendJson(3020, d);
}

void DrLink::sendMode(int mode)
{
    QJsonObject d;
    d.insert(QStringLiteral("Mode"), mode);
    sendJson(2006, d);
}

// ---------------------------------------------------------------------------
//  回执追踪：到底了主控静默忽略 → 300ms 没广播就是到底
// ---------------------------------------------------------------------------
void DrLink::notePending(DrParam p, int dir, int code)
{
    Pending pd;
    pd.dir  = dir;
    pd.code = code;
    pd.ms   = QDateTime::currentMSecsSinceEpoch();
    m_pending.insert(int(p), pd);
}

void DrLink::clearPending(DrParam p)
{
    if (!m_pending.contains(int(p))) return;
    const Pending pd = m_pending.take(int(p));
    emit stepConfirmed(p, pd.dir);
}

void DrLink::onWatchdog()
{
    if (m_pending.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<int> expired;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it) {
        if (now - it.value().ms > 300)          // 实测中位 34ms，115ms 封顶 → 300ms 足够
            expired.append(it.key());
    }
    for (int k : expired) {
        const Pending pd = m_pending.take(k);
        const DrParam p = DrParam(k);
        emit stepAtLimit(p, pd.dir);
        if (m_trace)
            emit logLine(QStringLiteral("no-ack %1 dir=%2 → 视为到底")
                         .arg(paramName(p)).arg(pd.dir));
    }
}

void DrLink::onHeartbeat()
{
    if (!isConnected()) return;
    // 15s 没收到任何下行 → 认为链路死了，主动断开重连
    if (m_lastRx.isValid() && m_lastRx.elapsed() > 15000) {
        emit logLine(QStringLiteral("link: 15s 无下行，重建连接"));
        m_ws->abort();
        reconnect();
        return;
    }
    QJsonObject empty;
    sendJson(100, empty);                        // 客户端心跳 {"code":100,"data":{}}
}

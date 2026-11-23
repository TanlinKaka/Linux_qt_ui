#include "uicards.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace {

struct CardCfg {
    QString      title;
    QList<uicards::Item> items;
    QString      def;
    int          visibleRows = 0;
};

QHash<QString, CardCfg> g_cards;
QString                 g_src;

// "targetEi" / "target_ei" / "Target-EI" 都当同一个 id —— 手写配置最容易在
// 大小写和下划线上出错，与其报错不如放宽。
QString normKey(const QString &s)
{
    QString r;
    r.reserve(s.size());
    for (const QChar c : s) {
        if (c == QLatin1Char('_') || c == QLatin1Char('-') || c.isSpace())
            continue;
        r.append(c.toLower());
    }
    return r;
}

QString squash(const QString &s)
{
    QString r;
    r.reserve(s.size());
    for (const QChar c : s)
        if (!c.isSpace())
            r.append(c);
    return r;
}

bool textEq(const QString &a, const QString &b)
{
    return squash(a).compare(squash(b), Qt::CaseInsensitive) == 0;
}

// items 的一个元素：字符串 或 { "text"|"label"|"name"|"value": ..., "icon": ... }
bool parseItem(const QJsonValue &v, uicards::Item *out)
{
    if (v.isString()) {
        out->text = v.toString();
        out->icon.clear();
        return !out->text.isEmpty();
    }
    if (!v.isObject())
        return false;
    const QJsonObject o = v.toObject();
    const char *kText[] = { "text", "label", "name", "value" };
    for (const char *k : kText) {
        const QJsonValue t = o.value(QLatin1String(k));
        if (t.isString() && !t.toString().isEmpty()) { out->text = t.toString(); break; }
        if (t.isDouble()) { out->text = QString::number(t.toDouble()); break; }
    }
    out->icon = o.value(QStringLiteral("icon")).toString();
    return !out->text.isEmpty();
}

void parseCard(const QString &id, const QJsonObject &o)
{
    CardCfg c;
    c.title = o.value(QStringLiteral("title")).toString();
    c.def   = o.value(QStringLiteral("default")).toString();
    if (c.def.isEmpty())                       // default 也允许写成数字
        if (o.value(QStringLiteral("default")).isDouble())
            c.def = QString::number(o.value(QStringLiteral("default")).toDouble());
    c.visibleRows = o.value(QStringLiteral("visibleRows")).toInt(0);

    const QJsonArray arr = o.value(QStringLiteral("items")).toArray();
    for (const QJsonValue &v : arr) {
        uicards::Item it;
        if (parseItem(v, &it))
            c.items.append(it);
        else
            qWarning("[cards] %s: items 里有认不出的元素（要字符串或 {text,icon}）",
                     qPrintable(id));
    }

    if (c.items.isEmpty() && c.title.isEmpty() && c.def.isEmpty()) {
        qWarning("[cards] %s: 既没有 items 也没有 title/default，忽略这一段",
                 qPrintable(id));
        return;
    }
    g_cards.insert(normKey(id), c);
}

void parseRoot(const QJsonObject &root)
{
    // 允许两种写法：{"cards":{...}} 或顶层直接就是卡映射
    QJsonObject cards = root.value(QStringLiteral("cards")).toObject();
    if (cards.isEmpty())
        cards = root;

    for (auto it = cards.constBegin(); it != cards.constEnd(); ++it) {
        if (!it.value().isObject())
            continue;                   // "version": 1 这类顶层字段直接跳过
        parseCard(it.key(), it.value().toObject());
    }
}

const CardCfg *findCfg(const QString &id)
{
    const auto it = g_cards.constFind(normKey(id));
    return (it == g_cards.constEnd()) ? nullptr : &it.value();
}

} // namespace

QString uicards::load(const QString &path)
{
    g_cards.clear();
    g_src.clear();

    QStringList cands;
    const bool byArg = !path.isEmpty();
    const bool byEnv = !qEnvironmentVariableIsEmpty("MYUI_CARDS");
    if (byArg)
        cands << path;
    if (byEnv)
        cands << qEnvironmentVariable("MYUI_CARDS");
    const QString appDir = QCoreApplication::applicationDirPath();
    cands << QDir::cleanPath(appDir + QStringLiteral("/../cards.json"))
          << appDir + QStringLiteral("/cards.json")
          << QStringLiteral("/etc/myui/cards.json")
          << QStringLiteral("/etc/cards.json");

    for (int i = 0; i < cands.size(); ++i) {
        const QString &p = cands[i];
        QFile f(p);
        if (!f.exists()) {
            // 只有「命令行/环境变量明确指定的那份」不存在才告警；候选链后几项
            // 没有是常态（板上通常只有 /etc/myui/cards.json）
            if (i == 0 && (byArg || byEnv))
                qWarning("[cards] 指定的配置文件不存在: %s（改用内置默认值）",
                         qPrintable(p));
            continue;
        }
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning("[cards] 打不开 %s: %s（改用内置默认值）",
                     qPrintable(p), qPrintable(f.errorString()));
            continue;
        }
        const QByteArray raw = f.readAll();
        f.close();

        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            // 坏文件不再往后找：现场就是想用这份，静默换成另一份更难排查
            qWarning("[cards] %s 解析失败（%s，偏移 %d）→ 全部改用内置默认值",
                     qPrintable(p), qPrintable(err.errorString()), int(err.offset));
            return QString();
        }
        parseRoot(doc.object());
        g_src = p;
        qDebug("[cards] 生效: %s（%d 张卡）", qPrintable(p), g_cards.size());
        return g_src;
    }

    // 找不到时把「找过哪些路径」直接打出来。不打印的话，现场只会看到界面
    // 用的是内置默认值（例如 EL 卡还是 5 档），完全无法判断是没配置、
    // 配错位置、还是被 IDE 的影子构建目录带偏了 —— 这是最容易踩的坑。
    qDebug("[cards] 没有配置文件，全部使用内置默认值。找过：");
    for (const QString &p : cands)
        qDebug("[cards]   %s%s", QFile::exists(p) ? "[有]" : "[无]", qPrintable(p));
    qDebug("[cards] 当前可执行目录: %s", qPrintable(QCoreApplication::applicationDirPath()));
    return QString();
}

QString uicards::sourcePath()
{
    return g_src;
}

uicards::Resolved uicards::resolve(const QString &id, const QList<Item> &fallback,
                                   const QString &fallbackTitle)
{
    Resolved r;
    r.title = fallbackTitle;
    r.items = fallback;

    const CardCfg *c = findCfg(id);
    if (!c)
        return r;

    if (!c->title.isEmpty())
        r.title = c->title;
    if (!c->items.isEmpty()) {
        r.items    = c->items;
        r.fromFile = true;
    }
    r.visibleRows = c->visibleRows;

    if (!c->def.isEmpty()) {
        for (int i = 0; i < r.items.size(); ++i) {
            if (textEq(r.items[i].text, c->def)) { r.defIndex = i; break; }
        }
        if (r.defIndex < 0)
            qWarning("[cards] %s: default=\"%s\" 不在 items 里，退回不指定",
                     qPrintable(id), qPrintable(c->def));
    }
    return r;
}

QString uicards::dumpJson()
{
    QJsonObject cards;
    for (auto it = g_cards.constBegin(); it != g_cards.constEnd(); ++it) {
        const CardCfg &c = it.value();
        QJsonObject o;
        if (!c.title.isEmpty()) o.insert(QStringLiteral("title"), c.title);
        if (!c.def.isEmpty())   o.insert(QStringLiteral("default"), c.def);
        if (c.visibleRows > 0)  o.insert(QStringLiteral("visibleRows"), c.visibleRows);
        QJsonArray arr;
        for (const Item &i : c.items) {
            if (i.icon.isEmpty()) {
                arr.append(i.text);
            } else {
                QJsonObject io;
                io.insert(QStringLiteral("text"), i.text);
                io.insert(QStringLiteral("icon"), i.icon);
                arr.append(io);
            }
        }
        o.insert(QStringLiteral("items"), arr);
        cards.insert(it.key(), o);
    }

    QJsonObject root;
    root.insert(QStringLiteral("source"), g_src.isEmpty()
                ? QStringLiteral("(内置默认值)") : g_src);
    root.insert(QStringLiteral("cards"), cards);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

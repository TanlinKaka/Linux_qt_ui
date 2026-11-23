#ifndef UICARDS_H
#define UICARDS_H

#include <QList>
#include <QString>

// ---------------------------------------------------------------------------
//  下拉卡片配置（板上 /etc/myui/cards.json）
// ---------------------------------------------------------------------------
//  设计稿把三张「底图卡片」的弹层行文案钉死了：
//      Focal Spot           2 行 Small / Large
//      Target exposure index 5 行 50 / 64 / 80 / 100 / 125
//      Exposure parameters  3 行 kV−mAs / kV−mAs−ms / kV−mA−ms
//  但实机这些档位随机型/探测器变（EL 尤其明显），改一次就要重编译。
//  所以把「行文案 + 默认选中 + 是否滚动」拆进 JSON；文件不存在、某张卡没写、
//  或 JSON 写坏，都**整体退回代码里的设计稿默认值** —— 也就是说没有配置文件时
//  行为与像素基线完全不变。
//
//  生效顺序（前面找到就不再往后找）：
//     1. 命令行 --cards <文件>
//     2. 环境变量 MYUI_CARDS
//     3. <可执行目录>/../cards.json     （工程根，开发时放这儿）
//     4. <可执行目录>/cards.json
//     5. /etc/myui/cards.json           （板上正式位置）
//     6. /etc/cards.json
//
//  文件长这样（逗号别漏，Qt 的解析器不容错）：
//  {
//    "cards": {
//      "targetEi": { "title": "Target exposure index",
//                    "default": "100", "visibleRows": 5,
//                    "items": ["50", "64", "80", "100", "125"] },
//      "focal":    { "items": [
//                      { "text": "Small", "icon": ":/mselect/icon_chk_small.png" },
//                      { "text": "Large", "icon": ":/mselect/icon_chk_large.png" } ] },
//      "exposure": { "items": ["kV\u2212mAs", "kV\u2212mAs\u2212ms", "kV\u2212mA\u2212ms"] }
//    }
//  }
//  · items 元素可以只写字符串（等价于 { "text": "..." }）
//  · default 按行文字匹配，写错会在日志里告警并退回「不指定」
//  · visibleRows 只在 items 比它更多时起作用：面板高度按设计稿不变，出现滚动
//  · title 不写 = 用代码里的设计稿标题（字号/位置永远走代码，避免破像素基线）
namespace uicards {

struct Item {
    QString text;     // 显示文字（弹层行；卡片上的文字默认也用它）
    QString icon;     // 资源路径，空 = 这一列没有图标
};

struct Resolved {
    QString     title;            // 空 = 调用方默认标题
    QList<Item> items;            // 一定非空（配置有问题时就是 fallback）
    int         defIndex    = -1; // 默认选中项；-1 = 不指定，保持代码现状
    int         visibleRows = 0;  // >0 且 items 更多 ⇒ 面板不增高，改为滚动
    bool        fromFile    = false;
};

// 读配置。path 为空 = 走上面的候选链。返回真正读到的文件（一个都没读到 = 空串）。
QString load(const QString &path = QString());

// 当前生效的配置文件路径（空 = 全部走内置默认）
QString sourcePath();

// 取某张卡的最终配置。fallback 是代码里的设计稿默认值，绝不会返回空列表。
Resolved resolve(const QString &id, const QList<Item> &fallback,
                 const QString &fallbackTitle = QString());

// 当前生效配置的 JSON 文本（--dump-cards 用，方便现场核对到底读到了什么）
QString dumpJson();

} // namespace uicards

#endif // UICARDS_H

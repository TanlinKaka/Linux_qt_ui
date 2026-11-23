#ifndef STATUSSTRIP_H
#define STATUSSTRIP_H

#include <QWidget>
#include <QPixmap>
#include <QString>
#include <QHash>

// ============================================================================
//  读数区 / 底栏 —— 「盖色块重画」
// ----------------------------------------------------------------------------
//  这两块在设计稿里是烘焙进底图的像素，没有控件。底图又打包在 res.rcc
//  （43MB 预编译），改图要重打 rcc 再推板，不值当。
//
//  所以做法是：控件在设计稿坐标上先铺 #171717（这两块的底色就是纯 #171717，
//  占 88% 以上的像素），把底图上的旧内容整个盖掉，再按量测值把分隔线、图标、
//  文字重画一遍。好处是底图一个字节都不用动，设计稿仍是像素回归基准。
//
//  图标来源分两类：
//    主状态  —— 直接从设计稿裁（tools/mk_status_icons.py），底色相同，不抠
//               alpha 直接贴，边缘零色差
//    其它状态 —— 从 PHILIPS_slices 取同体系的 Figma 切片（带 alpha），
//               等比缩放到槽位 bbox 内居中
//
//  坐标全部是「设计稿绝对坐标」，与 layout_profile.h 同一套。
// ============================================================================

// ---- 读数区：Req.SID / 患者身高 / 投照角度 / 支架高度 ----------------------
//   竖屏是 1×4 一行，横屏是 2×2 网格（不是把竖屏转 90°）
class InfoStrip : public QWidget
{
    Q_OBJECT
public:
    explicit InfoStrip(QWidget *page);

    void applyProfile(bool landscape);

    // i: 0=Req.SID 1=身高 2=角度 3=支架高；num 只给数字，单位单独设
    void setNum(int i, const QString &num);
    void setUnit(int i, const QString &u);
    void resetAll();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    bool     m_land = false;
    QString  m_num[4];
    QString  m_unit[4];
};

// ---- 底栏：探测器 / 栅格 / 准直 / 滤过 / 球管 A·B / HU ---------------------
//   两行 112px 高，元素尺寸两个朝向完全一样，只有组间距不同
class FooterBar : public QWidget
{
    Q_OBJECT
public:
    explicit FooterBar(QWidget *page);

    void applyProfile(bool landscape);

    // i: 0=探测器尺寸 1=滤过 2=球管A温度 3=球管B温度 4=HU
    void setTxt(int i, const QString &s);

    // slot 见 mselect_res/ 的 ft* 前缀；st 为空 = 主状态
    void setState(const QString &slot, const QString &st);

    // 三个便捷入口：数值进，图标档位自动跟（阈值在 .cpp 顶部）
    void setDetSize(int cm);              // 正方形：横纵同值
    void setDetSize(int w, int h);        // 限束器/探测器开口：横向 w × 纵向 h
    void setDetW(int w);                  // 只改横向，纵向保持
    void setDetH(int h);                  // 只改纵向，横向保持
    void setFilter(const QString &s);
    void setTemp(int idx, int celsius);   // idx 0=A 1=B
    void setHu(int pct);
    void setBattery(int idx, int level, bool charging);  // idx 0=A 1=B；level 0..4 格
    void setBatteryLow(int idx);          // 低电：橙色 + 1 格
    void setTubeActive(int idx);          // 0=A 1=B，另一个置灰
    void setGrid(int idx, int state);     // 0=A 1=B；state 0=装 1=未装 2=告警
    void setDetector(int idx, int state); // 0=A 1=B；state 0=就绪 1=未就绪
    void setCollim(int state);            // 0=正常 1=关闭 2=告警
    void setFiltState(int state);         // 0=正常 2=告警
    void setDetTemp(int celsius);         // HU 左侧温度计（≥70 转警示色）
    void setRing(int state);              // 剂量环 0=正常 2=告警
    void resetAll();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QPixmap pix(const QString &slot) const;       // 带缓存的图标取用
    QPixmap pixMain(const QString &slot) const;   // 只取主状态（用于量参考尺寸）
    bool  m_land = false;
    QString m_txt[5];
    int   m_detW = 29, m_detH = 29;   // 探测器/限束器开口：横向 × 纵向（cm）
    QHash<QString, QString>  m_st;    // slot -> 状态后缀
    mutable QHash<QString, QPixmap> m_cache;
};

// ---- 顶栏：头像 + 患者姓名 + 下拉箭头 --------------------------------------
//   底色 #383838（比页面 #171717 亮一档），整条 1200x112
class HeaderBar : public QWidget
{
    Q_OBJECT
public:
    explicit HeaderBar(QWidget *page);

    void applyProfile(bool landscape);
    void setName(const QString &name);
    void resetAll();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    bool    m_land = false;
    QString m_name;
};

// ---- 缩略图下方的部位名称条（Chest PA）-------------------------------------
//   卡片底 #2B2B2B，240x80，文字在卡内居中
class ThumbCaption : public QWidget
{
    Q_OBJECT
public:
    explicit ThumbCaption(QWidget *page);

    void applyProfile(bool landscape);
    void setPart(const QString &part);
    void resetAll();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    bool    m_land = false;
    QString m_part;
};

#endif // STATUSSTRIP_H

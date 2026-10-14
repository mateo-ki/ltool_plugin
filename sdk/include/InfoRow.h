#pragma once
#include "core-api.h"

#include <QString>
#include <QWidget>

class QCheckBox;
class QIcon;
class QLabel;
class QPixmap;

// 三个分析页（注册表分析 / 开机启动分析 / 软件列表）共用的列表行：
//   [勾选框(可选)] [色点] 标题                        [徽章]
//                        副标题（一行小字，说清来源/结论）
// 徽章用颜色区分"要不要管"，比整行变色更好读，也不会让列表看起来一片红。
//
// 整行（除勾选框外）都设了 WA_TransparentForMouseEvents，点击穿透回 QListWidget，
// 这样"选中一行"照常工作；勾选则用行里的真 QCheckBox —— item 自带的 check 指示器
// 在被持久控件盖住时点不中（注册表分析页踩过）。
class LTOOL_CORE_API InfoRow : public QWidget {
    Q_OBJECT
public:
    enum Badge { Neutral, Good, Warn, Bad, Muted };

    explicit InfoRow(const QString& title, const QString& subtitle,
                     const QString& badge = QString(), Badge kind = Neutral,
                     const QIcon& icon = QIcon(), int leftPad = 8, QWidget* parent = nullptr);

    // 给这行加一个勾选框（放在最左边）；checked 是初始状态
    void makeCheckable(bool checked);
    bool isChecked() const;

    // 直接给一张算好的位图。比构造时传 QIcon 便宜得多 —— shell 图标的
    // icon.pixmap() 每次要 3-9ms，分页反复建行时这成了大头。
    void setIconPixmap(const QPixmap& pix);

signals:
    void checkToggled(bool on);

private:
    static QColor badgeColor(Badge b);
    Badge m_kind = Neutral;
    QCheckBox* m_check = nullptr;
    QLabel* m_icon = nullptr;
};

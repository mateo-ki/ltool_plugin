#pragma once

#include "ClipboardStore.h"

#include <QRect>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;

// 剪贴板历史那一页：一墙**卡片** + 一个关键字过滤框（2026-10-19 他从"一行一条的列表"
// 改成卡片的：每张卡三样 —— 复制的时间、那条的文案、是图片就带一块缩略图）。
//
// 卡片是自绘的（一颗 QStyledItemDelegate 画在 QListWidget 的 IconMode 上），**不是**
// 每行塞一个 setItemWidget：那个一行 3-4ms，200 条就是 0.6 秒挂在 GUI 线程上
//（「软件列表」那页早就量过、也早就改过一轮）。
// 那几块矩形从卡片尺寸推出来，抽成下面 thumbRect/timeRect/bodyRect 三颗静态函数 ——
// 自绘的东西最容易"字压字"，抽出来判据才能直接问它们互不重叠、图不会被裁掉。
//
// 记录本身（那个 2 秒的时钟 + clipboard.json / clipimg）留在 core 的 ClipboardStore：
// 搜索框里「剪贴板历史」那一节和配置页那颗开关都在读它 —— 把这张卡禁用掉，
// 不该让"复制过的东西不再存盘"跟着变（和「定时任务」那张卡里 TaskScheduler 留 core 同理）。
// 所以这一页只是看它、用它，不拥有它。
class ClipboardPage : public QWidget {
    Q_OBJECT
public:
    explicit ClipboardPage(QWidget* parent = nullptr);

    // 卡片上那三样的数据来源（除 UserRole 以外都是这页自己加的， UserRole 一直是
    // "存储下标"，判据和双击/删除都读它，不能挪）
    static constexpr int TimeRole = Qt::UserRole + 1;    // 复制时间，已经格式化好的
    static constexpr int BodyRole = Qt::UserRole + 2;    // 文案（图片那条就是它的摘要）
    static constexpr int ImageRole = Qt::UserRole + 3;   // 这张卡有没有图（bool）
                                                         // 不能用 DecorationRole 判：QVariant(QIcon()) 不是 null

    // 一张卡的尺寸和里面三块的矩形（给判据直接问几何用，不是内部细节）
    static QSize cardSize();
    static QRect thumbRect(const QRect& card);
    static QRect timeRect(const QRect& card);
    static QRect bodyRect(const QRect& card, bool hasImage);

    // 过滤：把 items 过一遍，返回的是**每条在 ClipboardStore 里的下标**，不是行号。
    // 抽成静态纯函数就为了这一件事能离线钉住 —— "把行号当用下标"是这一页最容易错的格子：
    // 过滤之后第 0 行未必是 items[0]，点错了就把别的一条放回剪贴板。
    static QVector<int> filterIndices(const QVector<ClipboardStore::Item>& items,
                                      const QString& needle);

    // 给自测用的三个可观测量（离屏里点不了真鼠标，就只能读这几个数）
    int rowCount() const;                     // 列表现在摆了几行
    int storeIndexAt(int row) const;          // 第 row 行对应 items 里哪一条（越界/没有 = -1）
    QString rowTextAt(int row) const;         // 那一行显示的字（判据拿它和 UserRole 那条对账）
    // 这页到底是卡片墙还是行列表：IconMode + 装了自绘 delegate 两个条件都算过才算卡片墙。
    // 为什么值得单独暴露：改成卡片那次（2026-10-19）最怕的就是"看着还是列表"——
    // 界面自己不会说假话，但 viewMode 忘了设、delegate 装了却没换 IconMode，画出来还是一列。
    QString viewKind() const;                 // "卡片墙" / "行列表"
    QSize cardSizeAt(int row) const;          // 那一行要占的格子（判据拿它和 cardSize() 对账）
    // 那张卡的文案是从哪一行高开始排的 —— 判据拿它对账"这张卡到底有没有图"。
    // 为什么单独问一句：文本卡的图标是**空 QIcon**，而 `QVariant(QIcon())` 不是 null，
    // 用 DecorationRole 判"有没有图"会给每张文本卡都留一块不存在的图位（离屏渲 PNG 看见的）。
    int bodyTopAt(int row) const;
    QString statusText() const;               // 状态行那句（没在记录就老实写"没在记录"）
    void refresh();                           // 重摆一遍列表

protected:
    // 每次这一页被看见都重摆一次 —— 剪贴板是别人随时在写的，"打开就看到最新的"比
    // 挂一个每秒轮询的定时器诚实，也省掉跨 dll 连 core 里那个 QObject 的信号。
    void showEvent(QShowEvent* e) override;


private:
    void buildRows();

    QLineEdit* m_filter = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_status = nullptr;
};

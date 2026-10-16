#pragma once

#include "ClipboardStore.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;

// 剪贴板历史那一页：一张列表 + 一个关键字过滤框。
//
// 记录本身（那个 2 秒的时钟 + clipboard.json / clipimg）留在 core 的 ClipboardStore：
// 搜索框里「剪贴板历史」那一节和配置页那颗开关都在读它 —— 把这张卡禁用掉，
// 不该让"复制过的东西不再存盘"跟着变（和「定时任务」那张卡里 TaskScheduler 留 core 同理）。
// 所以这一页只是看它、用它，不拥有它。
class ClipboardPage : public QWidget {
    Q_OBJECT
public:
    explicit ClipboardPage(QWidget* parent = nullptr);

    // 过滤：把 items 过一遍，返回的是**每条在 ClipboardStore 里的下标**，不是行号。
    // 抽成静态纯函数就为了这一件事能离线钉住 —— "把行号当用下标"是这一页最容易错的格子：
    // 过滤之后第 0 行未必是 items[0]，点错了就把别的一条放回剪贴板。
    static QVector<int> filterIndices(const QVector<ClipboardStore::Item>& items,
                                      const QString& needle);

    // 给自测用的三个可观测量（离屏里点不了真鼠标，就只能读这几个数）
    int rowCount() const;                     // 列表现在摆了几行
    int storeIndexAt(int row) const;          // 第 row 行对应 items 里哪一条（越界/没有 = -1）
    QString rowTextAt(int row) const;         // 那一行显示的字（判据拿它和 UserRole 那条对账）
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

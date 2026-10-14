#pragma once

#include <QSet>
#include <QVector>
#include <QWidget>

#include "RegClean.h"

class QLabel;
class QListWidget;
class QPushButton;
template <class T> class QFutureWatcher;

// 工具 - 注册表分析页：扫出"指向已经不存在的文件/盘符"的键值，以及 9x 时代留下来的死键。
// 分析是重活（COM 那类要遍历上万个键），所以挂在按钮上、放到后台线程跑；
// 删除一律先导出 .reg 备份，"只提示"类（比如 PATH 里的某一段）不给勾。
class RegistryPage : public QWidget {
    Q_OBJECT
public:
    explicit RegistryPage(QWidget* parent = nullptr);

private:
    void startScan();
    void rebuildList();
    void updateDeleteBtn();
    void deleteSelected();
    void exportReport();

    QListWidget* m_list = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_scanBtn = nullptr;
    QPushButton* m_delBtn = nullptr;
    QVector<RegIssue> m_issues;
    QFutureWatcher<QVector<RegIssue>>* m_watcher = nullptr;
    QSet<int> m_checked;           // 勾了哪些项（下标指向 m_issues）
    bool m_busy = false;
};

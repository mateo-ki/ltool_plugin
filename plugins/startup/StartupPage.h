#pragma once

#include <QVector>
#include <QWidget>

#include "StartupAudit.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
template <class T> class QFutureWatcher;

// 工具 - 开机启动分析页：把注册表 Run / 启动文件夹 / 自动服务 / 计划任务 / 进程内加载项
// 一次列全，标出"目标还在不在、谁签的名、有没有被任务管理器关掉"。
// 只读工具：这里不做禁用和删除，改状态留给任务管理器和注册表分析。
class StartupPage : public QWidget {
    Q_OBJECT
public:
    explicit StartupPage(QWidget* parent = nullptr);

private:
    void startScan();
    void rebuildList();
    void exportReport();

    QListWidget* m_list = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QComboBox* m_filter = nullptr;
    QCheckBox* m_onlyProblems = nullptr;
    QPushButton* m_scanBtn = nullptr;
    QVector<StartupItem> m_items;
    QFutureWatcher<QVector<StartupItem>>* m_watcher = nullptr;
    bool m_busy = false;
};

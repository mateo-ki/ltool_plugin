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
// 扫描只读。**动手只有一件**：把某一条**应用**启动项开/关（走任务管理器那个
// `StartupApproved` 存储，随时能反回来）—— 服务 / 计划任务 / 加载项不在这里改。
// 系统服务（ImagePath 在 %SystemRoot% 下面的那些）默认不列出，勾上「显示系统服务」才看得到。
class StartupPage : public QWidget {
    Q_OBJECT
public:
    explicit StartupPage(QWidget* parent = nullptr);

private:
    void startScan();
    void rebuildList();
    void exportReport();
    void showSelected(int i);
    void toggleSelected();     // 就地两段：第一下"确认要关/开 X"，第二下才真写注册表

    QListWidget* m_list = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_status = nullptr;
    QComboBox* m_filter = nullptr;
    QCheckBox* m_onlyProblems = nullptr;
    QCheckBox* m_showSysSvc = nullptr;
    QPushButton* m_scanBtn = nullptr;
    QPushButton* m_toggleBtn = nullptr;
    QString m_armed;           // 已经按下第一下的那一条的名字（换选中/换状态就清空）
    QVector<StartupItem> m_items;
    QFutureWatcher<QVector<StartupItem>>* m_watcher = nullptr;
    bool m_busy = false;
};

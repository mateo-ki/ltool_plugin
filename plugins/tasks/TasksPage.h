#pragma once

#include <QWidget>

class QListWidget;
class QPushButton;
struct ScheduledTask;

// 配置 - 定时任务页：定时运行 c++/java/python/js/rust 脚本
class TasksPage : public QWidget {
    Q_OBJECT
public:
    explicit TasksPage(QWidget* parent = nullptr);

private:
    void rebuild();
    void addTask();
    void editTask();
    void removeTask();
    void runSelected();
    void toggleEnabled();

    QListWidget* m_list = nullptr;
    QPushButton* m_enableBtn = nullptr;
};

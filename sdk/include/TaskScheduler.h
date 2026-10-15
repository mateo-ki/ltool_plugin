#pragma once

#include "core-api.h"
#include <QObject>
#include <QStringList>
#include <QVector>

class QTimer;

// 一条定时任务：到点把 script 指向的脚本用对应语言跑一遍，输出落在 tasklogs\
// schedule 两种写法：interval:<分钟>（每隔这么久一次）/ daily:HH:mm（每天这个点一次）
struct ScheduledTask {
    QString id;
    QString name;
    QString type;         // python / js / java / c++ / rust
    QString script;       // 脚本或源文件路径
    QString schedule;     // interval:60 / daily:09:30
    QString lastDay;      // daily 用它记"今天已经跑过了"
    qint64 lastRunMs = 0;
    bool enabled = true;
};

// 定时器跑在 GUI 线程上（30 秒一次 tick），真正执行丢给 QtConcurrent
class LTOOL_CORE_API TaskScheduler : public QObject {
    Q_OBJECT
public:
    static TaskScheduler& instance();

    // 载入 tasks.json 并起 30 秒的 tick；重复调用不起第二个定时器
    void start();

    QVector<ScheduledTask> tasks() const;
    void setTasks(const QVector<ScheduledTask>& tasks);

    // 立刻跑一条（界面"运行"按钮）。返回空=已提交，非空=给界面看的原因
    QString runNow(const QString& id);

    static QStringList typeNames();

signals:
    void tasksChanged();

private:
    TaskScheduler() = default;
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    void load();
    void save();
    bool isDue(const ScheduledTask& t) const;
    void tick();
    QString runTask(const ScheduledTask& t, QString* err);

    QVector<ScheduledTask> m_tasks;
    QTimer* m_timer = nullptr;
};

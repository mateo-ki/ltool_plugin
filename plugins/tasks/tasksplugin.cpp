// ltool 工具插件：定时任务
//
// 页面本体是 TasksPage（从 src 搬过来的），这个壳只报元数据、建那张页。
// 调度器（TaskScheduler）留在 core：它是主程序一起来就在跑的后台时钟（每 30 秒看一眼），
// 把那张卡禁用掉也不会让"到点真的执行"跟着消失 —— 界面和时钟是两件事。
#include "ltool-plugin.h"
#include "TasksPage.h"

#include <QObject>
#include <QtPlugin>

class TasksPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("tasks"); }
    QString name() const override { return QStringLiteral("定时任务"); }
    QString desc() const override { return QStringLiteral("到点跑 c++/java/python/js/rust 脚本"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("定时"), QStringLiteral("计划"), QStringLiteral("cron") };
    }
    QWidget* createPage(QWidget* parent) override { return new TasksPage(parent); }
};

#include "tasksplugin.moc"

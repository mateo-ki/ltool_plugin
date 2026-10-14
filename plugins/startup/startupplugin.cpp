// ltool 工具插件：开机启动分析
//
// 页面本体是 StartupPage（从 src 搬过来的），这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "StartupPage.h"

#include <QObject>
#include <QtPlugin>

class StartupPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("startup"); }
    QString name() const override { return QStringLiteral("开机启动分析"); }
    QString desc() const override { return QStringLiteral("开机后都有什么自己起来 · 谁家的 · 哪些该管哪些不用管"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("启动"), QStringLiteral("开机"), QStringLiteral("自启"), QStringLiteral("startup"), QStringLiteral("autostart"), QStringLiteral("服务"), QStringLiteral("计划任务") };
    }
    QWidget* createPage(QWidget* parent) override { return new StartupPage(parent); }
};
#include "startupplugin.moc"

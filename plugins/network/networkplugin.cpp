// ltool 工具插件：网络
//
// 页面本体是 NetworkPage（从 src 搬过来的），这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "NetworkPage.h"

#include <QObject>
#include <QtPlugin>

class NetworkPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("network"); }
    QString name() const override { return QStringLiteral("网络"); }
    QString desc() const override { return QStringLiteral("系统代理检测、代理方案对比与内置下载设置"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("网络"), QStringLiteral("代理"), QStringLiteral("proxy") };
    }
    QWidget* createPage(QWidget* parent) override { return new NetworkPage(parent); }
};
#include "networkplugin.moc"

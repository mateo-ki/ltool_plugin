// ltool 工具插件：注册表分析
//
// 页面本体是 RegistryPage（从 src 搬过来的），这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "RegistryPage.h"

#include <QObject>
#include <QtPlugin>

class RegistryPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("registry"); }
    QString name() const override { return QStringLiteral("注册表分析"); }
    QString desc() const override { return QStringLiteral("扫失效路径与 9x 遗留键：卸载残留 / 启动项 / 关联 / COM / 服务 / 死盘符"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("注册表"), QStringLiteral("registry"), QStringLiteral("reg"), QStringLiteral("残留"), QStringLiteral("废弃"), QStringLiteral("失效") };
    }
    QWidget* createPage(QWidget* parent) override { return new RegistryPage(parent); }
};
#include "registryplugin.moc"

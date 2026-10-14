// ltool 工具插件：DLL 修复
//
// 页面本体是 RepairPage（从 src 搬过来的），这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "RepairPage.h"

#include <QObject>
#include <QtPlugin>

class RepairPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("repair"); }
    QString name() const override { return QStringLiteral("DLL 修复"); }
    QString desc() const override { return QStringLiteral("检测微软运行库 / DirectX / .NET，缺什么补什么"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("dll"), QStringLiteral("运行库"), QStringLiteral("vc++"), QStringLiteral("vcruntime"), QStringLiteral("修复"), QStringLiteral("directx"), QStringLiteral("dotnet"), QStringLiteral("net") };
    }
    QWidget* createPage(QWidget* parent) override { return new RepairPage(parent); }
};
#include "repairplugin.moc"

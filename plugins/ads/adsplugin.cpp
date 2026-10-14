// ltool 工具插件：广告弹窗
//
// 页面本体是 AdsPage（从 src 搬过来的），这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "AdsPage.h"

#include <QObject>
#include <QtPlugin>

class AdsPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("ads"); }
    QString name() const override { return QStringLiteral("广告弹窗"); }
    QString desc() const override { return QStringLiteral("扫描并拦截桌面广告弹窗，白名单与拦截规则"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("广告"), QStringLiteral("弹窗"), QStringLiteral("ad"), QStringLiteral("拦截"), QStringLiteral("广告弹窗") };
    }
    QWidget* createPage(QWidget* parent) override { return new AdsPage(parent); }
};
#include "adsplugin.moc"

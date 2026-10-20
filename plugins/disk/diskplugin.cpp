// ltool 工具插件：磁盘清理
//
// 页面本体是 DiskPage（从 src 搬过来的），这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "DiskPage.h"

#include <QObject>
#include <QtPlugin>

class DiskPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("disk"); }
    QString name() const override { return QStringLiteral("磁盘清理"); }
    QString desc() const override { return QStringLiteral("可清理位置 + 占用分析（矩形树图，能直读 $MFT）"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("磁盘"), QStringLiteral("清理"), QStringLiteral("占用"), QStringLiteral("空间"), QStringLiteral("c盘"), QStringLiteral("disk") };
    }
    QWidget* createPage(QWidget* parent) override { return new DiskPage(parent); }
};

// 这个工具自己的判据。主程序不再链接页面代码，所以测试跟着页一起搬进来，
// 由 --plugintest 通过 QPluginLoader::resolve 找到这个符号来跑。
extern "C" Q_DECL_EXPORT int ltoolPluginSelftest()
{
    return diskRowMenuSelftest();
}

// 慢的那个（要真扫一整盘，几十秒）单独一个入口，只由 --diskui 点名跑
extern "C" Q_DECL_EXPORT int ltoolPluginUiSelftest()
{
    return diskUiSelftest();
}
#include "diskplugin.moc"

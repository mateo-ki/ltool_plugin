// ltool 工具插件：下载器
//
// 页面本体是 DownloadsPage（从 src 搬过来的），这个壳只报元数据、建那张页。
// 下载的实现（DownloadManager）留在 core：「关于」页下载安装包也走它，两处共用同一份任务表。
#include "ltool-plugin.h"
#include "DownloadsPage.h"

#include <QObject>
#include <QtPlugin>

class DownloadsPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("downloads"); }
    QString name() const override { return QStringLiteral("下载器"); }
    QString desc() const override { return QStringLiteral("多线程下载、断点续传与任务表"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("下载"), QStringLiteral("download"), QStringLiteral("http") };
    }
    QWidget* createPage(QWidget* parent) override { return new DownloadsPage(parent); }
};

#include "downloadsplugin.moc"

// ltool 工具插件：软件
//
// 这一页原来是设置窗菜单里的「软件」那一行，两个 tab（推荐软件 / 软件列表）。
// 菜单收成四行之后它搬到插件里：同一个壳，两条判据符号留给主程序那个页面耗时探针点名要
//（探针要单独量每一页建起来花多久，拿整个 hub 量就只能得到一个混在一起的数）。
#include "ltool-plugin.h"
#include "SoftwarePage.h"
#include "UninstallPage.h"

#include <QObject>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QtPlugin>

namespace {

// 那一层壳只管一件事：把两张页摆进两个 tab。tab 的顺序就是原来菜单里那个顺序。
QWidget* makeHub(QWidget* parent)
{
    auto* hub = new QWidget(parent);
    auto* lay = new QVBoxLayout(hub);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* tabs = new QTabWidget(hub);
    tabs->addTab(new SoftwarePage(tabs), QStringLiteral("推荐软件"));
    tabs->addTab(new UninstallPage(tabs), QStringLiteral("软件列表"));
    lay->addWidget(tabs);
    return hub;
}

} // namespace

class SoftwarePlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("software"); }
    QString name() const override { return QStringLiteral("软件"); }
    QString desc() const override
    {
        return QStringLiteral("推荐软件一键安装 + 本机已装程序的卸载与防火墙放行");
    }
    QStringList keywords() const override
    {
        return { QStringLiteral("软件"), QStringLiteral("卸载"), QStringLiteral("winget") };
    }
    QWidget* createPage(QWidget* parent) override { return makeHub(parent); }
};

// 页面本体留给主程序的耗时探针（--pagetime）：它要分别量"推荐软件"和"软件列表"建出来各花多久。
// 走的是 PluginHost::entry 那一条已有的路，不是再造一个入口。
extern "C" Q_DECL_EXPORT QWidget* ltoolSoftwarePage() { return new SoftwarePage(nullptr); }
extern "C" Q_DECL_EXPORT QWidget* ltoolUninstallPage() { return new UninstallPage(nullptr); }

#include "softwareplugin.moc"

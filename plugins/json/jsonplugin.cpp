// ltool 工具插件：JSON 格式化
//
// 页面本体是 JsonTool（从 src 搬过来的），这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "JsonTool.h"

#include <QObject>
#include <QtPlugin>

class JsonPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("json"); }
    QString name() const override { return QStringLiteral("JSON 格式化"); }
    QString desc() const override { return QStringLiteral("可折叠树 + 自动换行，文本区可直接改"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("json"), QStringLiteral("格式化"), QStringLiteral("json格式化") };
    }
    QWidget* createPage(QWidget* parent) override
    {
        auto* p = new JsonTool(parent);
        m_page = p;
        return p;
    }
    bool feedText(const QString& text) override
    {
        if (!m_page)
            return false;      // 页还没建，宿主先 createPage 再喂
        m_page->setText(text);
        return true;
    }
    JsonTool* m_page = nullptr;
};
#include "jsonplugin.moc"

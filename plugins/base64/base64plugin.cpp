// ltool 工具插件：Base64 编解码
//
// 页面本体是 Base64Tool（从 src 搬过来的），这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "Base64Tool.h"

#include <QObject>
#include <QtPlugin>

class Base64Plugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("base64"); }
    QString name() const override { return QStringLiteral("Base64 编解码"); }
    QString desc() const override { return QStringLiteral("文本/图片互转，解不出合法 UTF-8 时给十六进制"); }
    QStringList keywords() const override
    {
        return { QStringLiteral("base64"), QStringLiteral("b64"), QStringLiteral("编码"), QStringLiteral("解码") };
    }
    QWidget* createPage(QWidget* parent) override
    {
        auto* p = new Base64Tool(parent);
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
    Base64Tool* m_page = nullptr;
};
#include "base64plugin.moc"

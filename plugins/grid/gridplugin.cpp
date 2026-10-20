// ltool 工具插件：图片排版（四宫/六宫/九宫）
//
// 页面本体是 GridPage + GridBoard，这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致，
// --plugintest 会把两边都对一遍。
#include "ltool-plugin.h"
#include "GridPage.h"

#include <QObject>
#include <QtPlugin>

class GridPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("grid"); }
    QString name() const override { return QStringLiteral("图片排版"); }
    QString desc() const override
    {
        return QStringLiteral("四宫/六宫/九宫：拖图进去、拖动换顺序、选背景色，合成一张");
    }
    QStringList keywords() const override
    {
        return { QStringLiteral("宫格"), QStringLiteral("四宫格"), QStringLiteral("六宫格"),
                 QStringLiteral("九宫格"), QStringLiteral("排版"), QStringLiteral("拼图"),
                 QStringLiteral("grid") };
    }
    QWidget* createPage(QWidget* parent) override
    {
        auto* p = new GridPage(parent);
        m_page = p;
        return p;
    }

    GridPage* m_page = nullptr;
};

#include "gridplugin.moc"

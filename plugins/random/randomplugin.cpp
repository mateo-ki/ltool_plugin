// ltool 工具插件：随机数生成
//
// 页面本体是 RandomTool，这个壳只报元数据、建那张页、把直达那段文本递给页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致 ——
// --plugintest 会把两边对一遍（两处写法说的是同一件事，漂了没人发现过一次）。
#include "ltool-plugin.h"
#include "RandomTool.h"

#include <QObject>
#include <QPointer>
#include <QtPlugin>

class RandomPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("random"); }
    QString name() const override { return QStringLiteral("随机数生成"); }
    QString desc() const override
    {
        return QStringLiteral("区间取数/不重复抽取/随机字符串/UUID，结果一键复制");
    }
    QStringList keywords() const override
    {
        return { QStringLiteral("随机"), QStringLiteral("随机数"), QStringLiteral("密码"),
                 QStringLiteral("字符串"), QStringLiteral("uuid"), QStringLiteral("random") };
    }
    QWidget* createPage(QWidget* parent) override
    {
        auto* p = new RandomTool(parent);
        m_page = p;
        return p;
    }
    bool feedText(const QString& text) override
    {
        if (!m_page)
            return false;      // 页还没建（或已经被宿主那一侧删掉了）：老实说不接，别去碰已释放的对象
        return m_page->feedText(text);
    }
    // 和 base64/json 同一颗道理：这张页的命在宿主那棵树里（`ToolsPage` 重排卡片墙会摘掉那一格），
    // 2026-10-22 那次「b64 会闪退」就是这里存了裸指针。这一颗从第一天起就用 QPointer。
    QPointer<RandomTool> m_page;
};
#include "randomplugin.moc"

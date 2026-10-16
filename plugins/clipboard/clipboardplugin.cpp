// ltool 工具插件：剪贴板历史（列表 + 关键字过滤）
//
// 页面本体是 ClipboardPage，这个壳只报元数据、建那张页。
// plugin.json 里那份 Name/Desc/Keywords 和下面的 override 必须一致 —— --plugintest 对账。
// 记录那个 2 秒时钟留在 core（ClipboardStore），所以这里**不挂 Activate**：
// 把这张卡禁用掉只该让"看不见历史"，不该偷偷停止收集 —— 那是两件事。
#include "ltool-plugin.h"
#include "ClipboardPage.h"

#include <QObject>
#include <QtPlugin>

class ClipboardPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("clipboard"); }
    QString name() const override { return QStringLiteral("剪贴板"); }
    QString desc() const override
    {
        return QStringLiteral("复制过的文本和图片，列表可按关键字过滤");
    }
    QStringList keywords() const override
    {
        return { QStringLiteral("剪贴板"), QStringLiteral("剪贴板历史"),
                 QStringLiteral("复制历史"), QStringLiteral("clipboard") };
    }
    QWidget* createPage(QWidget* parent) override { return new ClipboardPage(parent); }
};

#include "clipboardplugin.moc"

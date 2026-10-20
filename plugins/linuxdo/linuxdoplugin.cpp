// ltool 工具插件：linux.do 盯帖
//
// 壳只报元数据、建那张页。两件事在这儿多说一句：
//  1) plugin.json 的 Keywords 里**故意没有**「福利 / 女装 / 公益」——那三个是它盯的词，
//     不是它的名字。挂进关键词就会重演「计划任务」那次：搜「福利」的人被这张卡的直达
//     顶掉整页（直达是 return）。盯的词在页面和配置里，门口不挂。
//  2) "Activate": true 挂了牌，所以启动时 PluginHost::startBackground() 会按名字来找
//     下面这个导出符号；两边必须一致（--plugintest 会把牌和符号对一遍）。
#include "ltool-plugin.h"

#include "LinuxdoPage.h"
#include "LinuxdoWatch.h"

#include <QObject>
#include <QtPlugin>

class LinuxdoPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("linuxdo"); }
    QString name() const override { return QStringLiteral("linux.do 盯帖"); }
    QString desc() const override
    {
        return QStringLiteral("每隔几分钟看 linux.do 有没有写着这些词的帖子，命中就在托盘冒一句");
    }
    QStringList keywords() const override
    {
        return { QStringLiteral("盯帖"), QStringLiteral("论坛"), QStringLiteral("监控"),
                 QStringLiteral("通知"), QStringLiteral("新帖"), QStringLiteral("linuxdo") };
    }
    QWidget* createPage(QWidget* parent) override { return new LinuxdoPage(parent); }
};

// 后台那一路的入口（不需要实例化 Qt 插件对象 —— 那等于把懒创建废掉）。
// 幂等：LinuxdoWatch::start() 里定时器只建一次。
extern "C" Q_DECL_EXPORT void ltoolPluginActivate()
{
    LinuxdoWatch::instance().start();
}

#include "linuxdoplugin.moc"

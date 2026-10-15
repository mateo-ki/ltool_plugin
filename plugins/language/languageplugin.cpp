// ltool 工具插件：开发语言与环境
//
// 检测、安装并配置 Java、C++、Node.js、Python、Rust 等开发语言环境
#include "ltool-plugin.h"
#include "LanguagePage.h"

#include <QObject>
#include <QtPlugin>

class LanguagePlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("language"); }
    QString name() const override { return QStringLiteral("开发语言与环境"); }
    QString desc() const override
    {
        return QStringLiteral("检测、安装并配置 Java、C++、Node.js、Python、Rust 等开发语言与环境");
    }
    QStringList keywords() const override
    {
        return {
            QStringLiteral("语言"), QStringLiteral("环境"), QStringLiteral("翻译"),
            QStringLiteral("java"), QStringLiteral("python"), QStringLiteral("node"),
            QStringLiteral("nodejs"), QStringLiteral("rust"), QStringLiteral("c++"),
            QStringLiteral("cpp"), QStringLiteral("winget"), QStringLiteral("path")
        };
    }
    QWidget* createPage(QWidget* parent) override { return new LanguagePage(parent); }
};

#include "languageplugin.moc"

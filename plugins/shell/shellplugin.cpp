// ltool 工具插件：资源管理器右键的那几件事
//
// 这是第一层「动作型插件」：用户按的不是商店里的一张卡片，而是资源管理器的右键项 ——
// 注册表里那条 verb 由主程序写（`ShellMenu`，它决定"有哪些入口"），点进来之后主程序
// 起一个一次性进程、把旗标转成动作名交给这里（`invoke`）。
// 界面本体（占用分析那个窗、粉碎的三次覆写、解压/压缩的进度与取消）全在 shellaction.cpp。
#include "ltool-plugin.h"
#include "ShellMenu.h"
#include "shellaction.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

// 这个插件接哪些动作**只有一个来源**：shellaction.cpp 里那张分发表（`ShellAction::implemented()`）。
// plugin.json 里那份 "Actions" 是门口挂的牌（不装载就能查，商店和日志用它），
// --plugintest 会把牌和分发表对一遍：对不上 = 路由会以分发表为准，牌上写的会落空。

class ShellPlugin : public QObject, public LToolPlugin
{
    Q_OBJECT
    Q_INTERFACES(LToolPlugin)
    Q_PLUGIN_METADATA(IID LTOOL_PLUGIN_IID FILE "plugin.json")

public:
    int apiVersion() const override { return LTOOL_PLUGIN_API_VERSION; }
    QString id() const override { return QStringLiteral("shell"); }
    QString name() const override { return QStringLiteral("右键动作"); }
    QString desc() const override
    {
        return QStringLiteral("占用分析 / 谁占用着它 / 粉碎 / 解压 / 压缩为 zip·7z");
    }
    QStringList keywords() const override
    {
        return { QStringLiteral("解压"), QStringLiteral("压缩"), QStringLiteral("粉碎"),
                 QStringLiteral("占用"), QStringLiteral("右键"), QStringLiteral("shell") };
    }
    QStringList actions() const override { return ShellAction::implemented(); }

    // 右键那一路的旗标本来就带两个短横（注册表里写死的那些），动作名是去掉短横的那份。
    // 返回值只问一件事：**这条动作我实没实现**。压缩失败/用户按取消/窗口正常关闭都算"接住了"，
    // 那些结果由窗口、托盘和日志自己去说 —— 主程序再盖一层"做不了"是第二次报错（2026-10-15 错过）。
    bool invoke(const QString& action, const QStringList& args) override
    {
        return ShellAction::run(QStringLiteral("--") + action, args.value(0)) != ShellAction::kNotHandled;
    }

    // 动作型插件也得有一张页（商店里点开要有交代）。这页只说实话：接哪些动作、
    // 右键现在集成成什么样了 —— 「开/关右键集成」那颗勾只在配置页有，这儿不放第二颗。
    QWidget* createPage(QWidget* parent) override
    {
        auto* w = new QWidget(parent);
        auto* col = new QVBoxLayout(w);
        auto* head = new QLabel(QStringLiteral("资源管理器右键的那几件事"), w);
        head->setStyleSheet(QStringLiteral("font-weight:600;font-size:15px;"));
        col->addWidget(head);
        col->addWidget(new QLabel(QStringLiteral(
            "这些不是点卡片用的：在文件/文件夹上按右键，主程序把那条 verb 转成动作名交给我。\n接的动作：%1")
                                     .arg(ShellAction::implemented().join(QStringLiteral("、"))), w));
        auto* box = new QHBoxLayout();
        box->addWidget(new QLabel(ShellMenu::installed() ? QStringLiteral("右键已集成")
                                                          : QStringLiteral("右键还没集成"), w));
        box->addWidget(new QLabel(ShellMenu::locationHint(), w));
        col->addLayout(box);
        col->addWidget(new QLabel(QStringLiteral("要开或关：配置页 →「资源管理器右键集成」那一行。"), w));
        col->addStretch(1);
        return w;
    }
};

#include "shellplugin.moc"

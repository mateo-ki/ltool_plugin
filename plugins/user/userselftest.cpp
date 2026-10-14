// 「Windows 用户名」这一页的判据。
//
// 为什么在插件里而不是 main.cpp：页面类 UserAccountPage 和 UserAccount 那几个拼脚本的函数
// 现在都是这个插件的内部实现，主程序链接不到它们。宿主通过壳里那个 extern "C" 的
// ltoolPluginSelftest 把这段跑一遍（见 src/PluginHost.h 的 selftestEntry）。
#include "UserAccount.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QSet>
#include <QString>
#include <QThread>
#include <QWidget>

#include <cstdio>
#include <functional>

int userPageSelftest()
{
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    const auto has = [&bad](const char* name, bool ok) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok)
            ++bad;
    };
    using namespace UserAccount;

    // ---- 引号：这段是要提权执行的，逃逸口必须没有 ----
    check("psQuote 把单引号翻倍", psQuote(QStringLiteral("a'b")), QStringLiteral("'a''b'"));
    check("psQuote 包住整串", psQuote(QStringLiteral("x")).left(1) + psQuote(QStringLiteral("x")).right(1),
          QStringLiteral("''"));
    const QString evil = QStringLiteral("x'; Add-LocalGroupMember -Group 'Administrators'; #");
    const QString quoted = psQuote(evil);
    has("psQuote 对注入串：首尾各一个引号、里面没有裸单引号",
        quoted.startsWith(QLatin1Char('\'')) && quoted.endsWith(QLatin1Char('\''))
            && [&quoted] {
                const QString inner = quoted.mid(1, quoted.size() - 2);
                for (int i = 0; i < inner.size(); ++i) {
                    if (inner.at(i) != QLatin1Char('\''))
                        continue;
                    // 单引号只能成对出现；下一格不是引号就是逃逸口
                    if (i + 1 >= inner.size() || inner.at(i + 1) != QLatin1Char('\''))
                        return false;
                    ++i;
                }
                return true;
            }());
    std::printf("INFO 逃逸尝试拼出来的是：%s\n", quoted.toUtf8().constData());

    // ---- 名字合法性 ----
    has("空名被拒", !reject(QStringLiteral("   "), true).isEmpty());
    has("带斜杠被拒", !reject(QStringLiteral("a/b"), true).isEmpty());
    has("带引号被拒", !reject(QStringLiteral("a\"b"), true).isEmpty());
    has("带换行被拒", !reject(QString::fromUtf8("a") + QChar(10) + QString::fromUtf8("b"), true).isEmpty());
    has("账户名 20 字通过", reject(QString(20, QLatin1Char('a')), true).isEmpty());
    has("账户名 21 字被拒", !reject(QString(21, QLatin1Char('a')), true).isEmpty());
    has("显示名可以长一点（40 字通过）", reject(QString(40, QLatin1Char('a')), false).isEmpty());
    has("中文显示名通过", reject(QString::fromUtf8("张三"), false).isEmpty());

    // ---- 拼出去的脚本 ----
    const QString fs = fullNameScript(QStringLiteral("MACH"), QStringLiteral("usr"),
                                      QString::fromUtf8("新名字"));
    has("改显示名：ADSI 路径是拼出来的（单引号不会变成名字的一部分）",
        fs.contains(QStringLiteral("\"WinNT://\" + 'MACH' + '/' + 'usr' + ',user'")));
    has("改显示名：新值作为单引号字面量传进去", fs.contains(QStringLiteral("$u.FullName = '")
                                                         + QString::fromUtf8("新名字") + QStringLiteral("'")));
    has("改显示名：SetInfo 在", fs.contains(QStringLiteral("SetInfo")));
    const QString ns1 = newUserScript(QStringLiteral("newbie"), QString(), QStringLiteral("p'w"), true);
    has("新建用户：New-LocalUser 在", ns1.contains(QStringLiteral("New-LocalUser -Name 'newbie'")));
    has("新建用户：管理员组按 SID 找（不写死组名）", ns1.contains(QStringLiteral("S-1-5-32-544"))
        && ns1.contains(QStringLiteral("Add-LocalGroupMember")));
    has("新建用户：密码里的单引号也翻倍", ns1.contains(QStringLiteral("'p''w'")));
    const QString ns0 = newUserScript(QStringLiteral("newbie"), QString(),
                                      QStringLiteral("pw"), false);
    has("新建用户：不勾管理员就不加组", !ns0.contains(QStringLiteral("Add-LocalGroupMember")));
    has("新建用户：留空显示名时拿账户名顶上", ns0.contains(QStringLiteral("-FullName 'newbie'")));
    has("改账户名：走 Rename-LocalUser",
        renameScript(QStringLiteral("M"), QStringLiteral("old"), QStringLiteral("new"))
            .contains(QStringLiteral("Rename-LocalUser -Name 'old' -NewName 'new'")));

    // ---- 真实读取（只读）----
    const Info me = read();
    has("读得到当前账户", me.readable);
    std::printf("INFO 当前：%s@%s 本地=%d 显示名=[%s] 目录=%s\n", me.name.toUtf8().constData(),
                me.machine.toUtf8().constData(), me.local ? 1 : 0,
                me.fullName.toUtf8().constData(), me.profileDir.toUtf8().constData());
    has("账户名非空", !me.name.isEmpty());
    has("SID 长得对", me.sid.startsWith(QStringLiteral("S-1-")));
    has("用户目录真的在盘上", !me.profileDir.isEmpty() && QFileInfo::exists(me.profileDir));
    const Probe ghost = probe(QStringLiteral("ltool-no-such-user-zz"));
    has("查一个不存在的账户：可读、且不存在", ghost.readable && !ghost.exists);

    // ---- 页面：三颗按钮都得摆在布局里（数 findChild 不等于界面可见，配置页栽过）----
    UserAccountPage page;
    QSet<const QWidget*> laidOut;
    std::function<void(QLayout*)> walk = [&](QLayout* l) {
        if (!l)
            return;
        for (int i = 0; i < l->count(); ++i) {
            QLayoutItem* it = l->itemAt(i);
            if (!it)
                continue;
            if (QWidget* w = it->widget()) {
                laidOut.insert(w);
                walk(w->layout());
            } else if (QLayout* sub = it->layout())
                walk(sub);
        }
    };
    walk(page.layout());
    int onLayout = 0;
    for (const char* objName : { "applyFull", "applyName", "applyCreate" }) {
        QWidget* w = page.findChild<QWidget*>(QString::fromLatin1(objName));
        if (w && laidOut.contains(w))
            ++onLayout;
    }
    check("改显示名/改账户名/新建用户 三颗按钮都在布局里", QString::number(onLayout),
          QStringLiteral("3"));
    // 后台那次读取要真等到回包，别拿"正在读"当状态
    for (int i = 0; i < 120; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }
    const auto labels = page.findChildren<QLabel*>();
    QString state;
    for (const QLabel* l : labels)
        if (l->text().contains(QStringLiteral("账户名")))
            state = l->text();
    has("页面读完之后状态行填上了（不是停在「正在读」）",
        !state.isEmpty() && !state.contains(QStringLiteral("正在读")));

    return bad;
}

// 剪贴板插件自带的判据：过滤那条纯函数、页面"行号 ↔ 存储下标"的映射、状态行说实话、
// 双击复用 / Delete 删这条 / 清空 三条动作真的落到数据上。
//
// 全在离屏跑，数据用沙箱 APPDATA 里自造的 clipboard.json（main.cpp 那串白名单会把
// --cliptest 的数据目录挪到临时目录），一条都不碰他真实的剪贴板历史。
// 夹具里 clipboard/enable 留 false：那样引擎 2 秒轮询会在 capture() 第一行就返回，
// 不会因为他真实剪贴板动了一下就往我的夹具前面插一条、把下标全冲乱。
#include "ClipboardPage.h"
#include "ClipboardStore.h"
#include "Utils.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStringList>

#include <cstdio>
#include <functional>

namespace {

bool reachedFrom(QWidget* root, QWidget* w)
{
    bool found = false;
    std::function<void(QLayout*)> walk = [&](QLayout* l) {
        if (!l || found)
            return;
        for (int i = 0; i < l->count(); ++i) {
            QLayoutItem* it = l->itemAt(i);
            if (!it)
                continue;
            if (QWidget* cw = it->widget()) {
                if (cw == w) { found = true; return; }
                walk(cw->layout());
            } else if (QLayout* sub = it->layout())
                walk(sub);
        }
    };
    walk(root->layout());
    return found;
}

// 每一行显示的必须是**它自己那个 UserRole 下标**对应的那条。
// 这就是"行号当用下标"的照妖镜：过滤之后两者不相等，写成 row 会指着别的一条。
QString rowsAgreeWithTheirIndexes(const QVector<ClipboardStore::Item>& items, const ClipboardPage& page)
{
    for (int row = 0; row < page.rowCount(); ++row) {
        const int idx = page.storeIndexAt(row);
        if (idx < 0 || idx >= items.size())
            return QStringLiteral("第%1行的下标 %2 越界").arg(row).arg(idx);
        const QString shown = page.rowTextAt(row);
        if (!shown.contains(ClipboardStore::summary(items[idx])))
            return QStringLiteral("第%1行显示的不是 items[%2] 那条").arg(row).arg(idx);
    }
    return QStringLiteral("ok");
}

QJsonObject textItem(const QString& when, const QString& hash, const QString& text)
{
    QJsonObject o;
    o.insert("when", when);
    o.insert("hash", hash);
    o.insert("image", false);
    o.insert("text", text);
    o.insert("bytes", double(text.size()));
    return o;
}

} // namespace

extern "C" Q_DECL_EXPORT int ltoolPluginSelftest()
{
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        std::printf("%s %s got=[%s] want=[%s]\n", got == want ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
        if (got != want)
            ++bad;
    };
    const auto list = [](const QVector<int>& v) {
        QStringList s;
        for (const int i : v)
            s << QString::number(i);
        return s.join(QLatin1Char(','));
    };

    // ---- 夹具：写进沙箱数据目录，然后让 core 那侧自己 load() ----
    auto& cs = ClipboardStore::instance();
    const QString data = Utils::dataDir();
    const QString pngPath = QDir::toNativeSeparators(data + "/clipimg/selftest.png");
    bool fixtureOk = false;
    if (QDir().mkpath(data + "/clipimg")) {
        QImage img(1280, 720, QImage::Format_RGB32);
        img.fill(QColor(20, 90, 200));
        if (img.save(pngPath, "PNG")) {
            QJsonObject o;
            o.insert("when", "2026-10-16T09:08:07");
            o.insert("hash", "h0");
            o.insert("image", true);
            o.insert("path", pngPath);
            o.insert("w", 1280);
            o.insert("h", 720);
            o.insert("bytes", 34560.0);
            QJsonArray arr;
            arr.append(o);                                        // 0：一张图
            arr.append(textItem("2026-10-16T09:07:01", "h1", "ZZZclip 甲 alpha"));
            arr.append(textItem("2026-10-15T22:10:00", "h2", "第一行很普通\n第二行里有 zzzclipbeta"));
            arr.append(textItem("2026-05-04T23:01:02", "h3", "普通一条 ZZZ"));
            QFile f(data + "/clipboard.json");
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
                f.close();
                fixtureOk = true;
            }
        }
    }
    check("夹具写进了沙箱数据目录（写不进去后面全是废判据）",
          fixtureOk ? QStringLiteral("ok") : QStringLiteral("fail at %1").arg(data), "ok");

    cs.start();                       // load() 读那份夹具；enable 留 false（见文件头）
    cs.setEnabled(false);             // start() 之后强制关掉收集：真实剪贴板别往夹具里插东西
    const QVector<ClipboardStore::Item> items = cs.items();
    check("夹具四条都进来了", QString::number(items.size()), "4");

    // ---- 纯函数：过滤 ----
    check("空关键字 = 全部", list(ClipboardPage::filterIndices(items, QString())), "0,1,2,3");
    check("只有空格也算空关键字（别一条都不显示）",
          list(ClipboardPage::filterIndices(items, QStringLiteral("   "))), "0,1,2,3");
    // 整个插件最容易写错的一格：过滤后只剩一条时行号是 0，而它在存储里是第 1 条。
    check("命中一条时返回的是**存储下标**不是行号（alpha → 1，不是 0）",
          list(ClipboardPage::filterIndices(items, QStringLiteral("alpha"))), "1");
    check("一个词命中两条 → 两条下标都在、且按原顺序",
          list(ClipboardPage::filterIndices(items, QStringLiteral("ZZZclip"))), "1,2");
    check("大小写不管（ALPHA 一样命中 1）",
          list(ClipboardPage::filterIndices(items, QStringLiteral("ALPHA"))), "1");
    check("前后空格先 trim 再筛",
          list(ClipboardPage::filterIndices(items, QStringLiteral("  alpha  "))), "1");
    check("中文关键字照样能筛",
          list(ClipboardPage::filterIndices(items, QStringLiteral("普通"))), "2,3");
    check("命不中就一条都不给",
          list(ClipboardPage::filterIndices(items, QStringLiteral("zzzznope"))), QString());
    check("图片条目能按「图片」筛出来",
          list(ClipboardPage::filterIndices(items, QStringLiteral("图片"))), "0");
    check("图片条目还能按尺寸筛（1280）",
          list(ClipboardPage::filterIndices(items, QStringLiteral("1280"))), "0");
    check("搜的是整段而不是首行（第二行里的词也算）",
          list(ClipboardPage::filterIndices(items, QStringLiteral("zzzclipbeta"))), "2");
    check("能按日期筛（2026-05-04 那条）",
          list(ClipboardPage::filterIndices(items, QStringLiteral("2026-05-04"))), "3");
    {
        // zzz 同时出现在 1/2/3 三条里，而第 2 条的**摘要和正文里都有** —— 不许重复列
        const QVector<int> many = ClipboardPage::filterIndices(items, QStringLiteral("zzz"));
        QStringList uniq = QStringList();
        for (const int i : many)
            if (!uniq.contains(QString::number(i)))
                uniq << QString::number(i);
        check("一条命中多处也只出现一次", list(many) == uniq.join(QLatin1Char(',')) ? "yes" : "no", "yes");
    }

    // ---- 页面 ----
    ClipboardPage page;
    auto* status = page.findChild<QLabel*>(QStringLiteral("clipStatus"));
    auto* filter = page.findChild<QLineEdit*>(QStringLiteral("clipFilter"));
    auto* listw = page.findChild<QListWidget*>(QStringLiteral("clipList"));
    auto* clearBtn = page.findChild<QPushButton*>(QStringLiteral("clipClear"));
    auto* hint = page.findChild<QLabel*>(QStringLiteral("clipHint"));
    check("状态行摆在布局里（不是 new 出来就算）", reachedFrom(&page, status) ? "yes" : "no", "yes");
    check("过滤框摆在布局里", reachedFrom(&page, filter) ? "yes" : "no", "yes");
    check("列表摆在布局里", reachedFrom(&page, listw) ? "yes" : "no", "yes");
    check("清空那颗摆在布局里", reachedFrom(&page, clearBtn) ? "yes" : "no", "yes");
    check("说明那行摆在布局里", reachedFrom(&page, hint) ? "yes" : "no", "yes");

    check("没过滤时列表四行", QString::number(page.rowCount()), "4");
    check("每一行都对得上自己的下标", rowsAgreeWithTheirIndexes(cs.items(), page), "ok");
    filter->setText(QStringLiteral("alpha"));
    check("过滤成一条之后列表只剩一行", QString::number(page.rowCount()), "1");
    check("过滤后第 0 行映射回的下标是 1（不是 0）", QString::number(page.storeIndexAt(0)), "1");
    check("过滤状态下每一行仍对得上", rowsAgreeWithTheirIndexes(cs.items(), page), "ok");
    check("状态行跟着报过滤出的条数",
          page.statusText().contains(QStringLiteral("过滤出 1 条")) ? "yes" : "no", "yes");
    check("越界问下标老实给 -1（不崩）",
          QStringLiteral("%1,%2").arg(page.storeIndexAt(-1)).arg(page.storeIndexAt(999)), "-1,-1");

    // 复用走的是这一行自己的下标：过滤状态下复用第 0 行，进剪贴板的必须是那条文本
    cs.restore(page.storeIndexAt(0));
    QApplication::processEvents();
    check("复用过滤后的那一行，进剪贴板的是它本身而不是 items[0]",
          QApplication::clipboard()->text(), QStringLiteral("ZZZclip 甲 alpha"));

    // Delete：选中第 0 行（过滤状态下的那条 = items[1]），送一个真按键事件给列表。
    // **必须在下面那段 setEnabled(true) 之前** —— 一打开收集，capture() 就会把剪贴板里
    // 那条补进夹具、把固定 4 条的下标冲乱（上一版就是这么让这两条假红的）。
    listw->setCurrentRow(0);
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QApplication::sendEvent(listw, &del);
    check("按 Delete 把选中的那条从列表里拿掉了", QString::number(page.rowCount()), "0");
    filter->setText(QString());
    check("剩下的还是三条（删的正是那一条，不是一整片）", QString::number(page.rowCount()), "3");
    check("删完之后每一行仍对得上（下标跟着重排了）",
          rowsAgreeWithTheirIndexes(cs.items(), page), "ok");

    // 清空
    clearBtn->click();
    check("点清空之后列表空了", QString::number(page.rowCount()), "0");
    check("清空是真的清掉了数据（不是只藏起来）", QString::number(cs.items().size()), "0");
    check("清空之后状态行还在报 0 条（不写\"没在记录\"以外的假话）",
          page.statusText().contains(QStringLiteral("0 条")) ? "yes" : "no", "yes");

    // 状态行要说实话：关着不能说"正在记录"，开着不能说"没在记录"。放最后做，
    // 因为 setEnabled(true) 会真的开始收集、可能往数据里补条目（见上面那条注释）。
    check("没在记录时状态行写「没在记录」，并指路开关在哪",
          page.statusText().contains(QStringLiteral("没在记录")) ? "yes" : "no", "yes");
    cs.setEnabled(true);
    check("打开记录之后状态行跟着写「正在记录」",
          page.statusText().contains(QStringLiteral("正在记录")) ? "yes" : "no", "yes");
    cs.setEnabled(false);

    std::printf("clipboard: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad == 0 ? 0 : 1;
}

// 「随机数生成」这颗插件自带的判据。
//
// 两层，且**都走同一份实现**：Rand::generate() 就是页面按下「生成」真正调的那一颗，
// 不是在测试里重抄一遍算法；页面那一层用 RandomTool 的真控件驱动（点模式按钮、改那两格、
// 按生成），不自己 new 一个"长得像"的对象。
//
// 全程离屏、一次都不 show()：不碰他的桌面。复制那支只验按钮和状态行说的话 ——
// offscreen 的 QClipboard 是内存桩，真剪贴板内容量不到，那条如实标 SKIP。
#include "RandomTool.h"
#include "Theme.h"

#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <cstdio>
#include <limits>

namespace {

Rand::Spec baseSpec(Rand::Kind k)
{
    Rand::Spec s;
    s.kind = k;
    s.count = 5;
    return s;
}

QPushButton* buttonByText(QWidget* w, const QString& text)
{
    const auto btns = w->findChildren<QPushButton*>();
    for (QPushButton* b : btns)
        if (b->text() == text)
            return b;
    return nullptr;
}

bool clickKind(QWidget* w, const QString& label)
{
    QPushButton* b = buttonByText(w, label);
    if (!b)
        return false;
    b->click();     // 走的是和真人点同一条 clicked 信号，不是绕过界面去改成员
    return true;
}

// 这一页"现在在说哪种话"体现在哪几行可见上。量 isHidden()：离屏从没 show 过，
// isVisible() 一律 false —— 那是探针坏了，不是页面坏了。
QWidget* rowOfLine(QWidget* page, int lineEditIndex)
{
    const auto les = page->findChildren<QLineEdit*>();
    QLineEdit* lo = lineEditIndex < les.size() ? les.at(lineEditIndex) : nullptr;
    return lo ? lo->parentWidget() : nullptr;
}

QWidget* charsetRowOf(QWidget* page)
{
    const auto kids = page->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* w : kids)
        if (w->findChildren<QCheckBox*>(QString(), Qt::FindDirectChildrenOnly).size() >= 4)
            return w;
    return nullptr;
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
        std::fflush(stdout);
    };
    const auto has = [&bad](const char* name, bool ok) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok)
            ++bad;
        std::fflush(stdout);
    };

    // ---------- 第一层：Rand::generate 这一颗纯函数 ----------
    QStringList out;
    QString err;

    Rand::Spec s = baseSpec(Rand::Kind::Int);
    s.lo = 1; s.hi = 3; s.count = 200;
    has("整数：1…3 取 200 个生成得出来", Rand::generate(s, &out, &err));
    bool inRange = out.size() == 200;
    for (const QString& t : out) {
        const qint64 v = t.toLongLong();
        if (v < 1 || v > 3) { inRange = false; break; }
    }
    check("整数：200 行、每行都落在 1…3 里", inRange ? QStringLiteral("yes") : QStringLiteral("no"),
          QStringLiteral("yes"));

    s = baseSpec(Rand::Kind::Int);
    s.lo = 1; s.hi = 10; s.count = 10; s.unique = true;
    has("不重复（窄区间→抽样洗那条）：1…10 取满 10 个", Rand::generate(s, &out, &err));
    check("不重复：这 10 个真的互不相同",
          QStringLiteral("%1").arg(QSet<QString>(out.begin(), out.end()).size()), QStringLiteral("10"));

    s = baseSpec(Rand::Kind::Int);
    s.lo = 1; s.hi = 1000000000LL; s.count = 500; s.unique = true;
    has("不重复（宽区间→掷到没撞那条）：1…10 亿取 500 个", Rand::generate(s, &out, &err));
    check("不重复：宽区间那 500 个互不相同",
          QStringLiteral("%1").arg(QSet<QString>(out.begin(), out.end()).size()), QStringLiteral("500"));

    // 区间不够就**不许静默少给**：这一档最容易糊人的就是"少给几个也算成"
    s = baseSpec(Rand::Kind::Int);
    s.lo = 1; s.hi = 3; s.count = 5; s.unique = true;
    const bool enough = Rand::generate(s, &out, &err);
    check("区间只有 3 个值要 5 个：说不行（不是给 3 个当成功）",
          QStringLiteral("%1/%2").arg(enough ? QStringLiteral("ok") : QStringLiteral("refused"))
              .arg(out.size()),
          QStringLiteral("refused/0"));
    has("那句拒绝的话里带了数字（人能看懂差多少）",
        err.contains(QStringLiteral("3")) && err.contains(QStringLiteral("5")));

    s = baseSpec(Rand::Kind::Int); s.lo = 10; s.hi = 1;
    check("上界比下界小：说不", Rand::generate(s, &out, &err) ? QStringLiteral("ok") : QStringLiteral("refused"),
          QStringLiteral("refused"));
    s = baseSpec(Rand::Kind::Int); s.lo = 1; s.hi = 9; s.count = 0;
    check("数量 0 个：说不", Rand::generate(s, &out, &err) ? QStringLiteral("ok") : QStringLiteral("refused"),
          QStringLiteral("refused"));
    s = baseSpec(Rand::Kind::Int); s.lo = 1; s.hi = 9; s.count = 100001;
    check("数量超上限：纯函数自己也夹（不靠界面挡）",
          Rand::generate(s, &out, &err) ? QStringLiteral("ok") : QStringLiteral("refused"),
          QStringLiteral("refused"));

    // 整条 2^64 那一档：不许回绕成「0 个值」然后当成功
    s = baseSpec(Rand::Kind::Int);
    s.lo = std::numeric_limits<qint64>::min(); s.hi = std::numeric_limits<qint64>::max(); s.count = 3;
    check("区间宽到 2^64 个值：明说不（不回绕、不静默）",
          Rand::generate(s, &out, &err) ? QStringLiteral("ok") : QStringLiteral("refused"),
          QStringLiteral("refused"));

    s = baseSpec(Rand::Kind::Int); s.lo = -5; s.hi = 5; s.count = 300;
    has("负区间：-5…5 取 300 个生成得出来", Rand::generate(s, &out, &err));
    bool negOk = out.size() == 300;
    for (const QString& t : out) {
        const qint64 v = t.toLongLong();
        if (v < -5 || v > 5) { negOk = false; break; }
    }
    check("负区间：每行都在 -5…5 里", negOk ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));

    s = baseSpec(Rand::Kind::Decimal); s.lo = 0; s.hi = 1; s.count = 40; s.digits = 3;
    has("小数：生成成功", Rand::generate(s, &out, &err));
    const QRegularExpression dec(QStringLiteral("^0\\.\\d{3}$"));
    bool decOk = out.size() == 40;
    for (const QString& t : out)
        if (!dec.match(t).hasMatch()) { decOk = false; break; }
    check("小数：40 行全是三位小数", decOk ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
    s.digits = 99;
    has("小数：位数 99 被夹住（不是造一行 99 位的字）", Rand::generate(s, &out, &err));
    check("小数：夹完是 12 位（小数点那两个字符之外）",
          QStringLiteral("%1").arg(out.at(0).size() - 2), QStringLiteral("12"));
    s = baseSpec(Rand::Kind::Decimal); s.lo = 0; s.hi = 1; s.digits = 2; s.unique = true;
    check("小数这一档不许偷偷实现「不重复」：直接说不",
          Rand::generate(s, &out, &err) ? QStringLiteral("ok") : QStringLiteral("refused"),
          QStringLiteral("refused"));

    // 字符集：池子大小只有一份算法，界面画的和判据验的是同一颗
    s = baseSpec(Rand::Kind::String); s.sets = QStringLiteral("d"); s.dropLookalike = false;
    check("只勾数字时池子就是 10 个字符", QStringLiteral("%1").arg(Rand::poolOf(s).size()), QStringLiteral("10"));
    s.sets = QStringLiteral("du"); s.dropLookalike = true;
    // 数字去掉 0/1 = 8，大写去掉 O/I = 24 → 32。写成"全局删这几个字符"就会跨档删、数对不上
    check("「去掉易混」只在各自那一档里删", QStringLiteral("%1").arg(Rand::poolOf(s).size()), QStringLiteral("32"));
    s.sets = QString();
    check("一个字符集都没勾：说不",
          Rand::generate(s, &out, &err) ? QStringLiteral("ok") : QStringLiteral("refused"),
          QStringLiteral("refused"));
    s = baseSpec(Rand::Kind::String); s.sets = QStringLiteral("d"); s.length = 20; s.unique = true;
    check("只勾数字（池子 10）要 20 位不重复：说不行",
          Rand::generate(s, &out, &err) ? QStringLiteral("ok") : QStringLiteral("refused"),
          QStringLiteral("refused"));
    s = baseSpec(Rand::Kind::String); s.sets = QStringLiteral("dlu"); s.length = 16; s.count = 4;
    s.dropLookalike = false;
    has("字符串：16 位 ×4 生成成功", Rand::generate(s, &out, &err));
    const QString allow = QStringLiteral("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    bool strOk = out.size() == 4;
    for (const QString& t : out) {
        if (t.size() != 16) { strOk = false; break; }
        for (const QChar& c : t)
            if (!allow.contains(c)) { strOk = false; break; }
    }
    check("字符串：长度 16、只出现勾了的字符集里的字符", strOk ? QStringLiteral("yes") : QStringLiteral("no"),
          QStringLiteral("yes"));
    s.length = 12; s.count = 1; s.unique = true;
    has("字符串「字符不重复」：12 位造得出来", Rand::generate(s, &out, &err));
    check("字符串「字符不重复」：这一串 12 个字符互不相同",
          QStringLiteral("%1").arg(QSet<QChar>(out.at(0).begin(), out.at(0).end()).size()), QStringLiteral("12"));

    s = baseSpec(Rand::Kind::Uuid); s.count = 5;
    has("UUID：生成成功", Rand::generate(s, &out, &err));
    const QRegularExpression uuid(
        QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    bool uOk = out.size() == 5;
    for (const QString& t : out)
        if (!uuid.match(t).hasMatch()) { uOk = false; break; }
    check("UUID：五颗都是 8-4-4-4-12 小写十六进制（不带大括号）",
          uOk ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
    check("UUID：五颗互不相同", QStringLiteral("%1").arg(QSet<QString>(out.begin(), out.end()).size()),
          QStringLiteral("5"));

    // ---------- 第二层：那一页的真控件 ----------
    RandomTool page;
    check("页面：没 show 过（不碰他的桌面）",
          page.isVisible() ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("no"));

    const bool kAll = clickKind(&page, QStringLiteral("整数")) && clickKind(&page, QStringLiteral("小数"))
                    && clickKind(&page, QStringLiteral("字符串")) && clickKind(&page, QStringLiteral("UUID"));
    check("页面：四颗模式按钮都按得到", kAll ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));

    QWidget* rangeRow = rowOfLine(&page, 0);
    QWidget* charRow = charsetRowOf(&page);
    has("页面：找得到区间行和字符集行（找不到就是探针空转，不算绿）",
        rangeRow != nullptr && charRow != nullptr);

    clickKind(&page, QStringLiteral("整数"));
    check("整数档：区间行可见", rangeRow && !rangeRow->isHidden() ? QStringLiteral("yes") : QStringLiteral("no"),
          QStringLiteral("yes"));
    check("整数档：字符集那行收了（不是灰着让人以为能勾）",
          charRow && charRow->isHidden() ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
    clickKind(&page, QStringLiteral("字符串"));
    check("字符串档：字符集那行出来了", charRow && !charRow->isHidden() ? QStringLiteral("yes") : QStringLiteral("no"),
          QStringLiteral("yes"));
    check("字符串档：区间行收了（字符串没有上下界）",
          rangeRow && rangeRow->isHidden() ? QStringLiteral("yes") : QStringLiteral("no"), QStringLiteral("yes"));
    clickKind(&page, QStringLiteral("小数"));
    check("小数档：区间行还在", rangeRow && !rangeRow->isHidden() ? QStringLiteral("yes") : QStringLiteral("no"),
          QStringLiteral("yes"));
    clickKind(&page, QStringLiteral("UUID"));
    check("UUID 档：上下界和字符集都收了",
          rangeRow && rangeRow->isHidden() && charRow && charRow->isHidden() ? QStringLiteral("yes")
                                                                             : QStringLiteral("no"),
          QStringLiteral("yes"));
    clickKind(&page, QStringLiteral("整数"));

    QPushButton* gen = buttonByText(&page, QStringLiteral("生成"));
    has("页面：那颗「生成」按钮在", gen != nullptr);
    if (gen) {
        gen->click();
        check("按生成：结果区的行数 = 数量框那个数",
              QStringLiteral("%1").arg(page.resultText().split(QLatin1Char('\n')).size()),
              QStringLiteral("%1").arg(page.currentSpec().count));
        has("状态行说了 ✔、把档位写出来（不是只说「好了」）",
            page.statusText().startsWith(QStringLiteral("✔"))
                && page.statusText().contains(QStringLiteral("整数")));

        const auto les = page.findChildren<QLineEdit*>();
        if (les.size() >= 2) {
            les.at(0)->setText(QStringLiteral("50"));
            les.at(1)->setText(QStringLiteral("5"));
            gen->click();
            check("区间弄坏之后：结果区清空（留着上一轮那五行就是「看着像成功了」）",
                  page.resultText().isEmpty() ? QStringLiteral("yes") : QStringLiteral("no"),
                  QStringLiteral("yes"));
            has("区间弄坏之后：状态行改口说 ✘ 那句原因", page.statusText().startsWith(QStringLiteral("✘")));
            les.at(0)->setText(QStringLiteral("1"));
            les.at(1)->setText(QStringLiteral("100"));
            gen->click();
            has("改回来之后：当场又生成得出来（不是永久卡在错误那一行）",
                page.resultText().split(QLatin1Char('\n')).size() == page.currentSpec().count);
        } else {
            check("页面：找得到下界/上界那两格", QStringLiteral("%1").arg(les.size()), QStringLiteral("2"));
        }
    }

    has("直达「随 7 个」接得住", page.feedText(QStringLiteral("随 7 个")));
    check("那一段真的把数量改成 7（结果 7 行）",
          QStringLiteral("%1").arg(page.resultText().split(QLatin1Char('\n')).size()), QStringLiteral("7"));
    check("没有数字的一段话老实说不接（不偷偷生成）",
          page.feedText(QStringLiteral("随便说点什么")) ? QStringLiteral("fed") : QStringLiteral("refused"),
          QStringLiteral("refused"));

    QPushButton* copy = buttonByText(&page, QStringLiteral("复制结果"));
    has("页面：那颗「复制结果」按钮在", copy != nullptr);
    if (copy) {
        copy->click();
        check("复制之后按钮就地改口（不新插一块东西）",
              copy->text() == QStringLiteral("已复制 ✓") ? QStringLiteral("yes") : QStringLiteral("no"),
              QStringLiteral("yes"));
        std::printf("INFO SKIP 剪贴板里到底是不是那几行：offscreen 平台是内存桩，量不到"
                    "（要验这条得用 QT_QPA_PLATFORM=windows 且不 show 窗口）\n");
    }

    // 状态行那支颜色是每次现挂的，切模式必须跟着换。
    // 这一条钉的是本轮自己写坏的那处：拿「已经展开过的样式表」再 expand 一遍 =
    // 读上一轮留下的状态 —— 切完白天它还挂着夜里那支绿，界面就不说实话了。
    const Theme::Mode orig = Theme::current();
    Theme::setMode(Theme::Mode::Dark);
    const QString okDark = Theme::token("okText");
    const QString textBefore = page.statusText();
    Theme::setMode(Theme::Mode::Light);
    const QString okLight = Theme::token("okText");
    const QString sheet = page.statusStyle();
    check("切到日间：状态行样式表里是白天那支、夜里那支不在",
          QStringLiteral("%1/%2").arg(sheet.contains(okLight) ? QStringLiteral("light")
                                                              : QStringLiteral("same"))
              .arg(sheet.contains(okDark) ? QStringLiteral("夜里那支还在") : QStringLiteral("干净")),
          QStringLiteral("light/干净"));
    check("切模式之后：状态行那句话一个字没变（重挂颜色不许把它擦空）",
          page.statusText() == textBefore ? QStringLiteral("yes") : QStringLiteral("no"),
          QStringLiteral("yes"));
    Theme::setMode(orig);

    std::printf("randtest: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad;
}

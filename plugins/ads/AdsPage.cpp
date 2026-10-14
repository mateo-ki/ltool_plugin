#include "AdsPage.h"
#include "AdBlocker.h"
#include "FileTools.h"
#include "Utils.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <windows.h>

namespace {

// 一行一个片段
QStringList linesToPatterns(const QString& text)
{
    QStringList out;
    for (const QString& l : text.split(QLatin1Char('\n'))) {
        const QString v = l.trimmed();
        if (!v.isEmpty() && !out.contains(v))
            out << v;
    }
    return out;
}

} // namespace

AdsPage::AdsPage(QWidget* parent)
    : QWidget(parent)
{
    AdBlocker::instance().start();   // 幂等；拦截器活在 qApp 上，不随本页销毁

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 4, 4, 4);
    lay->setSpacing(8);

    auto* tip = new QLabel("扫描桌面上的广告弹窗：按窗口标题 / 进程名片段匹配，白名单优先于拦截名单。"
                           "自动拦截是常驻的，关掉这个设置页也继续生效；改完规则先点「试跑规则…」看会不会误杀，"
                           "不用等下一次真弹。");
    tip->setObjectName("tip");
    tip->setWordWrap(true);
    lay->addWidget(tip);

    m_summary = new QLabel(this);
    m_summary->setObjectName("muted");
    lay->addWidget(m_summary);

    lay->addWidget(new QLabel("当前桌面上命中的窗口"));
    m_list = new QListWidget(this);
    m_list->setMaximumHeight(180);
    lay->addWidget(m_list);

    auto* btns = new QHBoxLayout;
    auto* scan = new QPushButton("重新扫描", this);
    auto* close = new QPushButton("关闭窗口", this);
    auto* kill = new QPushButton("结束进程", this);
    auto* block = new QPushButton("加入拦截名单", this);
    auto* allow = new QPushButton("加入白名单", this);
    auto* dry = new QPushButton("试跑规则…", this);
    auto* rules = new QPushButton("编辑规则…", this);
    close->setToolTip("给选中的窗口发关闭消息（大窗口不会被关，避免误杀正常程序主界面）");
    kill->setToolTip("结束该进程：弹窗背后的推送进程一起停掉，今天不会再弹");
    block->setToolTip("把这个窗口所属的进程名加进拦截名单，以后它的弹窗自动关");
    allow->setToolTip("白名单优先于拦截名单：这个进程/标题以后永远不再被动");
    dry->setToolTip("拿当前规则把桌面上所有可见窗口跑一遍，只报告不关窗，用来看会不会误杀");
    rules->setToolTip("维护标题 / 进程名的匹配片段（含白名单），一行一个");
    btns->addWidget(scan);
    btns->addWidget(close);
    btns->addWidget(kill);
    btns->addWidget(block);
    btns->addWidget(allow);
    btns->addWidget(dry);
    btns->addWidget(rules);
    btns->addStretch(1);
    lay->addLayout(btns);

    m_auto = new QCheckBox("自动拦截（每 5 秒扫一遍桌面，命中的直接关闭；关掉设置页也继续生效）", this);
    m_observe = new QCheckBox("观察模式：只记录、不关闭（先看看规则会不会误杀正常窗口）", this);
    m_auto->setChecked(AdBlocker::instance().autoEnabled());
    m_observe->setChecked(AdBlocker::instance().observeOnly());
    m_observe->setEnabled(m_auto->isChecked());
    lay->addWidget(m_auto);
    lay->addWidget(m_observe);

    lay->addWidget(new QLabel("最近拦截记录"));
    m_history = new QListWidget(this);
    m_history->setMaximumHeight(200);
    lay->addWidget(m_history);
    auto* histBtns = new QHBoxLayout;
    auto* refresh = new QPushButton("刷新记录", this);
    auto* clear = new QPushButton("清空记录", this);
    histBtns->addWidget(refresh);
    histBtns->addWidget(clear);
    histBtns->addStretch(1);
    lay->addLayout(histBtns);

    // 统计：记录已经落盘，重启也不清零，所以这里能看出"这一周到底拦没拦"
    m_stats = new QLabel(this);
    m_stats->setObjectName("muted");
    m_stats->setTextFormat(Qt::PlainText);
    lay->addWidget(m_stats);

    connect(scan, &QPushButton::clicked, this, &AdsPage::rescan);
    connect(close, &QPushButton::clicked, this, &AdsPage::closeSelected);
    connect(kill, &QPushButton::clicked, this, &AdsPage::killSelected);
    connect(block, &QPushButton::clicked, this, &AdsPage::blockSelected);
    connect(allow, &QPushButton::clicked, this, &AdsPage::allowSelected);
    connect(dry, &QPushButton::clicked, this, &AdsPage::dryRun);
    connect(rules, &QPushButton::clicked, this, &AdsPage::editRules);
    connect(refresh, &QPushButton::clicked, this, &AdsPage::rebuildHistory);
    connect(clear, &QPushButton::clicked, this, [this] {
        AdBlocker::instance().clearHistory();
        rebuildHistory();
    });
    connect(m_auto, &QCheckBox::toggled, this, [this](bool on) {
        AdBlocker::instance().setAutoEnabled(on);
        m_observe->setEnabled(on);
        rescan();
    });
    connect(m_observe, &QCheckBox::toggled, this, [](bool on) {
        AdBlocker::instance().setObserveOnly(on);
    });

    rescan();
}

void AdsPage::rescan()
{
    auto& ab = AdBlocker::instance();
    m_hits.clear();
    for (const AdBlocker::Hit& h : ab.scan())
        if (ab.matches(h.title, h.process))
            m_hits.append(h);

    m_list->clear();
    if (m_hits.isEmpty())
        m_list->addItem("桌面上没有命中规则的窗口（规则可以在「编辑规则…」里加）");
    for (const AdBlocker::Hit& h : m_hits)
        new QListWidgetItem(QString("%1    进程: %2 (pid %3)    %4×%5")
                                .arg(h.title, h.process).arg(h.pid).arg(h.width).arg(h.height),
                            m_list);

    m_summary->setText(QString("自动拦截：%1   观察模式：%2   今天已关 %3 个   "
                               "名单：进程 %4 条 / 标题 %5 条   白名单：%6 条")
                           .arg(ab.autoEnabled() ? QStringLiteral("开") : QStringLiteral("关"))
                           .arg(ab.observeOnly() ? QStringLiteral("开") : QStringLiteral("关"))
                           .arg(ab.closedToday())
                           .arg(ab.procPatterns().size())
                           .arg(ab.titlePatterns().size())
                           .arg(ab.allowProcPatterns().size() + ab.allowTitlePatterns().size()));
    rebuildHistory();
    renderStats();
}

// 文字柱状：不想为一个统计块引入 QtCharts（安装包已经 20MB 了）
void AdsPage::renderStats()
{
    auto& ab = AdBlocker::instance();
    const auto daily = ab.dailyCounts(7);
    const auto top = ab.topProcesses(7, 5);
    int week = 0;
    int peak = 1;
    for (const auto& d : daily) {
        week += d.second;
        peak = qMax(peak, d.second);
    }
    if (week == 0 && top.isEmpty()) {
        m_stats->setText("最近 7 天没有拦截记录（规则没命中过，或者自动拦截没开）");
        return;
    }
    QString text = QString("最近 7 天共关 %1 个：\n").arg(week);
    for (const auto& d : daily) {
        const int bar = d.second * 24 / peak;
        text += QString("  %1  %2 %3\n")
                    .arg(d.first.toString("MM-dd"))
                    .arg(QString(bar, QChar(0x2588)))
                    .arg(d.second);
    }
    text += QStringLiteral("按进程：");
    QStringList parts;
    for (const auto& t : top)
        parts << QString("%1 ×%2").arg(t.first).arg(t.second);
    text += parts.join(QStringLiteral("，"));
    m_stats->setText(text.trimmed());
}

// 拿当前规则把所有可见窗口判一遍：改完规则立刻知道会不会误杀，不用等下次真弹
void AdsPage::dryRun()
{
    auto& ab = AdBlocker::instance();
    const auto all = ab.scan();
    QVector<AdBlocker::Hit> willClose;
    int nBig = 0, nAllow = 0;
    for (const AdBlocker::Hit& h : all) {
        const AdBlocker::Verdict v = ab.verdictOf(h);
        if (v == AdBlocker::Verdict::WillClose || v == AdBlocker::Verdict::WouldCloseIfNotObserving)
            willClose.append(h);
        else if (v == AdBlocker::Verdict::TooBig)
            ++nBig;
        else if (v == AdBlocker::Verdict::Allowed)
            ++nAllow;
    }
    const int nNone = all.size() - willClose.size() - nBig - nAllow;
    QDialog dlg(this);
    dlg.setWindowTitle(QString("试跑规则 — 桌面上 %1 个可见窗口").arg(all.size()));
    dlg.resize(860, 560);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(QString("会被处理：%1 个    命中但是大窗口（不会关）：%2 个    "
                                      "白名单放行：%3 个    不匹配：%4 个")
                                  .arg(willClose.size()).arg(nBig).arg(nAllow).arg(nNone)));
    auto* list = new QListWidget(&dlg);
    lay->addWidget(list, 1);
    for (const AdBlocker::Hit& h : all) {
        const AdBlocker::Verdict v = ab.verdictOf(h);
        if (v == AdBlocker::Verdict::NoMatch)
            continue;                       // 没命中的占绝大多数，列出来反而看不清
        list->addItem(QString("%1    %2    %3×%4    %5")
                          .arg(AdBlocker::verdictText(v), h.process)
                          .arg(h.width).arg(h.height).arg(h.title));
    }
    if (list->count() == 0)
        list->addItem("没有任何窗口命中当前规则（说明这套规则现在不会动任何东西）");
    auto* btns = new QHBoxLayout;
    btns->addStretch(1);
    auto* ok = new QPushButton("关闭", &dlg);
    btns->addWidget(ok);
    lay->addLayout(btns);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    dlg.exec();
    rescan();
}

void AdsPage::rebuildHistory()
{
    m_history->clear();
    const auto& h = AdBlocker::instance().history();
    if (h.isEmpty()) {
        m_history->addItem("还没有记录：勾上「自动拦截」后，命中并处理的弹窗会列在这里");
        return;
    }
    for (int i = h.size() - 1; i >= 0; --i) {
        const AdBlocker::Record& r = h[i];
        m_history->addItem(QString("%1  %2  %3  %4")
                               .arg(r.when.toString("MM-dd hh:mm:ss"),
                                    r.closed ? QStringLiteral("已关闭") : QStringLiteral("仅记录"),
                                    r.process, r.title));
    }
}

void AdsPage::closeSelected()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_hits.size())
        return;
    const int closed = AdBlocker::instance().closeHits({m_hits[row]});
    QMessageBox::information(this, "ltool",
        closed ? QStringLiteral("已发送关闭请求")
               : QStringLiteral("这个窗口太大（超过 1100×820），判断为正常程序主界面，没有关。"
                                "确实要关可以在「结束进程」里处理。"));
    rescan();
}

void AdsPage::killSelected()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_hits.size())
        return;
    const AdBlocker::Hit& h = m_hits[row];
    if (QMessageBox::question(this, "ltool",
            QString("结束进程 %1 (pid %2)？\n\n"
                    "这会把弹窗背后的整个程序停掉（今天它不会再弹），"
                    "但正在编辑的内容可能丢失。")
                .arg(h.process).arg(h.pid))
        != QMessageBox::Yes)
        return;
    const QString msg = FileTools::unlockByPid(h.pid);
    if (msg.isEmpty())
        QMessageBox::information(this, "ltool", "进程已结束");
    else
        QMessageBox::warning(this, "ltool", msg);
    rescan();
}

void AdsPage::blockSelected()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_hits.size())
        return;
    auto& ab = AdBlocker::instance();
    const QString procBase = m_hits[row].process.toLower().remove(QLatin1String(".exe"));
    ab.addProcPattern(procBase);
    // 白名单压过拦截名单：不先把覆盖它的白名单条目去掉，点了"加入拦截名单"其实什么都不拦
    const QString proc = m_hits[row].process;
    QStringList keep;
    for (const QString& s : ab.allowProcPatterns())
        if (!s.isEmpty() && !proc.contains(s, Qt::CaseInsensitive))
            keep << s;
    if (keep.size() != ab.allowProcPatterns().size())
        ab.setAllowPatterns(keep, ab.allowTitlePatterns());
    QMessageBox::information(this, "ltool",
        QString("已拦截进程 “%1”，以后它的弹窗会被自动关闭。\n"
                "名单可以在「编辑规则…」里删改。").arg(m_hits[row].process));
    rescan();
}

void AdsPage::allowSelected()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_hits.size())
        return;
    const AdBlocker::Hit& h = m_hits[row];
    auto& ab = AdBlocker::instance();
    const QString procBase = h.process.toLower().remove(QLatin1String(".exe"));
    ab.addAllowProc(procBase);
    ab.addAllowTitle(h.title);
    QMessageBox::information(this, "ltool",
        QString("已放行 “%1”，它的窗口以后不会再被自动关闭。\n"
                "白名单优先于拦截名单，要反悔在「编辑规则…」的「忽略的进程」里删掉。")
            .arg(h.process));
    rescan();
}

void AdsPage::editRules()
{
    auto& ab = AdBlocker::instance();
    QDialog dlg(this);
    dlg.setWindowTitle(QString("拦截规则 — 拦截 %1 条 / 白名单 %2 条")
                           .arg(ab.procPatterns().size() + ab.titlePatterns().size())
                           .arg(ab.allowProcPatterns().size() + ab.allowTitlePatterns().size()));
    dlg.resize(980, 560);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel("一行一个片段；标题按“包含”匹配（中文原样），进程名按不区分大小写匹配（不含 .exe 也行）。"
                              "白名单优先：命中白名单的窗口无论拦截图怎么写都不会被动。"));
    auto* cols = new QHBoxLayout;
    auto mk = [&](const QString& title, const QStringList& init) {
        auto* box = new QVBoxLayout;
        box->addWidget(new QLabel(title));
        auto* ed = new QPlainTextEdit(init.join(QLatin1Char('\n')));
        box->addWidget(ed);
        cols->addLayout(box);
        return ed;
    };
    auto* procEd = mk("进程名片段", ab.procPatterns());
    auto* titleEd = mk("窗口标题片段", ab.titlePatterns());
    auto* apEd = mk("忽略的进程", ab.allowProcPatterns());
    auto* atEd = mk("忽略的标题", ab.allowTitlePatterns());
    lay->addLayout(cols, 1);
    auto* btns = new QHBoxLayout;
    auto* reset = new QPushButton("恢复默认名单", &dlg);
    reset->setToolTip("清掉白名单，并把拦截名单换成内置的默认值");
    btns->addWidget(reset);
    btns->addStretch(1);
    auto* ok = new QPushButton("保存", &dlg);
    ok->setObjectName("primary");
    auto* cancel = new QPushButton("取消", &dlg);
    btns->addWidget(ok);
    btns->addWidget(cancel);
    lay->addLayout(btns);
    QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    QObject::connect(reset, &QPushButton::clicked, &dlg, [&] {
        procEd->setPlainText(AdBlocker::defaultProcPatterns().join(QLatin1Char('\n')));
        titleEd->setPlainText(AdBlocker::defaultTitlePatterns().join(QLatin1Char('\n')));
        apEd->setPlainText(QString());
        atEd->setPlainText(QString());
    });
    if (dlg.exec() != QDialog::Accepted)
        return;
    ab.setPatterns(linesToPatterns(procEd->toPlainText()), linesToPatterns(titleEd->toPlainText()));
    ab.setAllowPatterns(linesToPatterns(apEd->toPlainText()), linesToPatterns(atEd->toPlainText()));
    rescan();
}

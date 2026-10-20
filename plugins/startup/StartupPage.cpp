#include "StartupPage.h"
#include "platform/Launch.h"

#include "InfoRow.h"
#include "StartupAudit.h"
#include "Utils.h"

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

bool isProblem(const StartupItem& it)
{
    return it.targetMissing || !it.enabled || (it.checkedSig && !it.hasSignature);
}

// 下面三个函数只负责"把事实说成人话"：什么时候跑 / 谁签的名 / 要不要管。
// 结论全部从已有事实推出来，不去猜某个软件是干什么的。

QString whenLabel(const StartupItem& it)
{
    switch (it.source) {
    case StartupItem::Source::RegistryRun:
        return it.note.contains(QLatin1String("RunOnce"))
            ? QStringLiteral("下次登录时只跑一次")
            : QStringLiteral("每次登录以后");
    case StartupItem::Source::StartupFolder:
        return QStringLiteral("每次登录以后");
    case StartupItem::Source::Service:
        return it.note.contains(QStringLiteral("延迟"))
            ? QStringLiteral("开机后稍等一会儿自动起来")
            : QStringLiteral("开机就自动起来");
    case StartupItem::Source::Task:
        if (it.note.contains(QStringLiteral("启动时")) && it.note.contains(QStringLiteral("登录时")))
            return QStringLiteral("开机和登录时");
        return it.note.contains(QStringLiteral("启动时")) ? QStringLiteral("开机时")
                                                          : QStringLiteral("登录时");
    case StartupItem::Source::ShellExtension:
        return QStringLiteral("别的程序启动时被塞进去");
    }
    return {};
}

QString whoLabel(const StartupItem& it)
{
    if (!it.publisher.isEmpty())
        return it.publisher + QStringLiteral(" 签的名");
    return it.checkedSig ? QStringLiteral("没有数字签名")
                         : QStringLiteral("没查到签名（拿不到文件路径）");
}

QString verdictLabel(const StartupItem& it)
{
    if (it.targetMissing)
        return QStringLiteral("文件已经不在了 · 建议清掉（工具 → 注册表分析）");
    if (!it.enabled)
        return QStringLiteral("已经被关掉 · 不用管");
    if (it.publisher.contains(QLatin1String("Microsoft"), Qt::CaseInsensitive))
        return QStringLiteral("微软的东西 · 建议留着");
    switch (it.source) {
    case StartupItem::Source::Service:
        return QStringLiteral("后台服务，不在桌面上抢开机时间 · 不认识就先留着");
    case StartupItem::Source::ShellExtension:
        return QStringLiteral("挂在别的进程里 · 出问题时才需要查它");
    default:
        // "没签名"这件事由 whoLabel 说，这里别重复一遍
        return QStringLiteral("正常自启动 · 想关去 任务管理器 → 启动");
    }
}

QString plainGroup(StartupItem::Source s)
{
    switch (s) {
    case StartupItem::Source::RegistryRun:    return QStringLiteral("登录以后自动启动的程序");
    case StartupItem::Source::StartupFolder:  return QStringLiteral("放在“启动”文件夹里的东西");
    case StartupItem::Source::Service:        return QStringLiteral("开机自动起来的后台服务");
    case StartupItem::Source::Task:           return QStringLiteral("计划任务里设了开机/登录触发的");
    case StartupItem::Source::ShellExtension: return QStringLiteral("塞进浏览器 / 资源管理器的加载项");
    }
    return {};
}

// 徽章只放结论关键词，完整那句在选中后的下面一行里
QString shortVerdict(const StartupItem& it, InfoRow::Badge* kind)
{
    *kind = InfoRow::Neutral;
    if (it.targetMissing) {
        *kind = InfoRow::Bad;
        return QStringLiteral("文件没了 · 建议清");
    }
    if (!it.enabled) {
        *kind = InfoRow::Muted;
        return QStringLiteral("已关掉 · 不用管");
    }
    if (it.publisher.contains(QLatin1String("Microsoft"), Qt::CaseInsensitive)) {
        *kind = InfoRow::Good;
        return QStringLiteral("微软的 · 建议留着");
    }
    if (it.publisher.isEmpty()) {
        *kind = InfoRow::Warn;
        return QStringLiteral("没签名 · 自己判断");
    }
    if (it.source == StartupItem::Source::Service) {
        *kind = InfoRow::Good;
        return QStringLiteral("后台服务 · 不用管");
    }
    *kind = InfoRow::Good;
    return QStringLiteral("正常自启动");
}

} // namespace

StartupPage::StartupPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 6, 6, 6);
    lay->setSpacing(8);

    auto* tip = new QLabel(QStringLiteral(
        "这一页回答三个问题：开机后有东西自己起来吗？都是谁？哪些该管？\n"
        "每一行读法：软件名 — 什么时候跑 · 谁签的名 · 要不要管。"
        "红字是文件已经不在了的（建议清），灰字是已经被关掉的（不用管），黄字是没有数字签名的（自己判断）。"
        "这一页只读，不会改你任何东西。"), this);
    tip->setObjectName(QStringLiteral("tip"));
    tip->setWordWrap(true);
    lay->addWidget(tip);

    auto* row = new QHBoxLayout;
    m_scanBtn = new QPushButton(QStringLiteral("开始分析"), this);
    auto* reportBtn = new QPushButton(QStringLiteral("导出报告"), this);
    auto* tsMgr = new QPushButton(QStringLiteral("打开任务管理器的启动项设置"), this);
    row->addWidget(m_scanBtn);
    row->addWidget(reportBtn);
    row->addWidget(tsMgr);
    row->addStretch(1);
    lay->addLayout(row);

    auto* filterRow = new QHBoxLayout;
    filterRow->addWidget(new QLabel(QStringLiteral("只看哪一类："), this));
    m_filter = new QComboBox(this);
    m_filter->addItem(QStringLiteral("全部"));
    for (const auto s : { StartupItem::Source::RegistryRun, StartupItem::Source::StartupFolder,
                          StartupItem::Source::Service, StartupItem::Source::Task,
                          StartupItem::Source::ShellExtension })
        m_filter->addItem(plainGroup(s), int(s));
    m_onlyProblems = new QCheckBox(QStringLiteral("只看需要我处理的"), this);
    filterRow->addWidget(m_filter);
    filterRow->addWidget(m_onlyProblems);
    filterRow->addStretch(1);
    lay->addLayout(filterRow);

    m_summary = new QLabel(QStringLiteral("点「开始分析」，把这台机器开机时会自己起来的东西列一遍，"
                                          "每一行都会告诉你它什么时候跑、谁家的、要不要管。"), this);
    m_summary->setObjectName(QStringLiteral("muted"));
    m_summary->setWordWrap(true);
    lay->addWidget(m_summary);

    m_list = new QListWidget(this);
    lay->addWidget(m_list, 1);

    m_status = new QLabel(QStringLiteral("选中一行可以看它的完整注册位置和命令行。"), this);
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    connect(m_scanBtn, &QPushButton::clicked, this, &StartupPage::startScan);
    connect(reportBtn, &QPushButton::clicked, this, &StartupPage::exportReport);
    connect(tsMgr, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:startupapps")));
    });
    connect(m_filter, &QComboBox::currentIndexChanged, this, [this] { rebuildList(); });
    connect(m_onlyProblems, &QCheckBox::toggled, this, [this] { rebuildList(); });
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        const auto sel = m_list->selectedItems();
        if (sel.isEmpty())
            return;
        const int i = sel.first()->data(Qt::UserRole).toInt();
        if (i < 0 || i >= m_items.size())
            return;
        const StartupItem& it = m_items[i];
        m_status->setText(QStringLiteral("%1 — %2 · %3 · %4\n从哪来的：%5\n它要执行的命令：%6")
                              .arg(it.name, whenLabel(it), whoLabel(it), verdictLabel(it), it.location,
                                   it.command.isEmpty() ? QStringLiteral("（看不到命令行）") : it.command));
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        const int i = item->data(Qt::UserRole).toInt();
        if (i >= 0 && i < m_items.size()) {
            QGuiApplication::clipboard()->setText(m_items[i].command.isEmpty()
                                                      ? m_items[i].location
                                                      : m_items[i].command);
            m_status->setText(QStringLiteral("已把命令行复制到剪贴板"));
        }
    });
}

void StartupPage::startScan()
{
    if (m_busy)
        return;
    m_busy = true;
    m_scanBtn->setEnabled(false);
    m_status->setText(QStringLiteral("分析中…"));
    if (!m_watcher) {
        m_watcher = new QFutureWatcher<QVector<StartupItem>>(this);
        connect(m_watcher, &QFutureWatcherBase::finished, this, [this] {
            m_items = m_watcher->result();
            m_busy = false;
            m_scanBtn->setEnabled(true);
            rebuildList();
        });
    }
    m_watcher->setFuture(QtConcurrent::run([] { return StartupAudit::scan(); }));
}

void StartupPage::rebuildList()
{
    m_list->clear();
    if (m_items.isEmpty()) {
        m_summary->setText(QStringLiteral("还没有结果，或这台机器上一条启动项都没扫出来。"));
        return;
    }
    const int srcSel = m_filter->currentIndex();          // 0 = 全部
    int missing = 0, unsigned_ = 0, disabled = 0, shown = 0;
    for (const StartupItem& it : m_items) {
        if (it.targetMissing)
            ++missing;
        if (it.checkedSig && !it.hasSignature)
            ++unsigned_;
        if (!it.enabled)
            ++disabled;
    }

    const auto sources = { StartupItem::Source::RegistryRun, StartupItem::Source::StartupFolder,
                           StartupItem::Source::Service, StartupItem::Source::Task,
                           StartupItem::Source::ShellExtension };
    for (const auto src : sources) {
        if (srcSel > 0 && int(src) != m_filter->itemData(srcSel).toInt())
            continue;
        QVector<int> rows;
        for (int i = 0; i < m_items.size(); ++i)
            if (m_items[i].source == src)
                rows.append(i);
        if (rows.isEmpty())
            continue;
        // 有问题的排前面，其余按名字
        std::sort(rows.begin(), rows.end(), [this](int a, int b) {
            const bool pa = isProblem(m_items[a]), pb = isProblem(m_items[b]);
            if (pa != pb)
                return pa;
            return m_items[a].name.compare(m_items[b].name, Qt::CaseInsensitive) < 0;
        });
        m_list->addItem([this] {
            auto* s = new QListWidgetItem();
            s->setFlags(Qt::NoItemFlags);
            s->setSizeHint(QSize(0, 30));
            s->setForeground(QColor(125, 134, 152));
            QFont f = s->font();
            f.setBold(true);
            s->setFont(f);
            return s;
        }());
        m_list->item(m_list->count() - 1)
            ->setText(QString("── %1（%2）──").arg(plainGroup(src)).arg(rows.size()));
        for (const int i : rows) {
            const StartupItem& it = m_items[i];
            if (m_onlyProblems->isChecked() && !isProblem(it))
                continue;
            auto* item = new QListWidgetItem();
            item->setData(Qt::UserRole, i);
            item->setSizeHint(QSize(0, 48));
            InfoRow::Badge kind = InfoRow::Neutral;
            const QString badge = shortVerdict(it, &kind);
            auto* row = new InfoRow(it.name,
                                    QStringLiteral("%1 · %2").arg(whenLabel(it), whoLabel(it)),
                                    badge, kind);
            m_list->addItem(item);
            m_list->setItemWidget(item, row);
            ++shown;
        }
    }
    int normal = 0;
    for (const StartupItem& it : m_items)
        if (!isProblem(it))
            ++normal;
    m_summary->setText(QString("一共 %1 项。需要处理的 %2 项（文件已经不在了，红字）· "
                               "已经被关掉的 %3 项（灰字）· 没签名的 %4 项（黄字）· 其余 %5 项正常。")
                           .arg(m_items.size()).arg(missing).arg(disabled).arg(unsigned_).arg(normal));
    m_status->setText(QStringLiteral("想关掉某项：选中它，看下面这行的位置和命令，"
                                     "再去 任务管理器 → 启动 里禁用；文件已经不在了的那些，"
                                     "可以在 工具 → 注册表分析 里连记录一起清掉。双击一行 = 复制它的命令行。"));
}

void StartupPage::exportReport()
{
    if (m_items.isEmpty()) {
        m_status->setText(QStringLiteral("还没有分析结果可导出。"));
        return;
    }
    const QString path = QString("%1/startup-report.txt").arg(Utils::dataDir());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_status->setText(QStringLiteral("写报告失败：%1").arg(path));
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << QStringLiteral("ltool 开机启动分析报告\n\n");
    for (const StartupItem& it : m_items) {
        ts << QString("· %1\n  来源：%2  状态：%3\n  位置：%4\n  命令：%5\n  签名：%6\n\n")
                  .arg(it.name, StartupAudit::sourceLabel(it.source),
                       it.enabled ? QStringLiteral("启用") : QStringLiteral("已禁用"),
                       it.location, it.command,
                       it.publisher.isEmpty()
                           ? (it.checkedSig ? QStringLiteral("无") : QStringLiteral("未检查"))
                           : QString("%1（校验%2）").arg(it.publisher,
                                                         it.signatureValid ? "通过" : "未通过"));
    }
    f.close();
    Platform::reveal(path);
    m_status->setText(QStringLiteral("报告已导出：%1").arg(path));
}

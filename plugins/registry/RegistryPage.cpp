#include "RegistryPage.h"

#include "InfoRow.h"
#include "RegClean.h"
#include "Utils.h"

#include <algorithm>

#include <QDesktopServices>
#include <QFile>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

// 分组顺序：删了最没风险的排前面，只提示的排最后
const RegIssue::Kind kOrder[] = {
    RegIssue::Kind::UninstallOrphan, RegIssue::Kind::RunOrphan, RegIssue::Kind::AppPathsOrphan,
    RegIssue::Kind::MuiCacheOrphan,  RegIssue::Kind::ServiceOrphan, RegIssue::Kind::DeadDrive,
    RegIssue::Kind::LegacyKey,       RegIssue::Kind::AssociationOrphan, RegIssue::Kind::ComOrphan,
    RegIssue::Kind::PathEntry,
};

QString kindLabel(RegIssue::Kind kind)
{
    RegIssue it;
    it.kind = kind;
    return RegClean::categoryLabel(it);
}

bool kindDeletable(RegIssue::Kind kind)
{
    RegIssue it;
    it.kind = kind;
    return it.deletable();
}

QListWidgetItem* sectionItem(const QString& text)
{
    auto* it = new QListWidgetItem(text);
    it->setFlags(Qt::NoItemFlags);
    it->setSizeHint(QSize(0, 30));
    it->setForeground(QColor(125, 134, 152));
    QFont f = it->font();
    f.setBold(true);
    it->setFont(f);
    return it;
}

} // namespace

RegistryPage::RegistryPage(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 6, 6, 6);
    lay->setSpacing(8);

    auto* tip = new QLabel(QStringLiteral(
        "分析注册表里「指向已经不存在的文件 / 盘符」的项，以及 9x 时代留下来的死键："
        "卸载残留、启动项、App Paths、文件关联、COM 组件、服务、死盘符、PATH 失效目录。\n"
        "删除前会把原值导出成 .reg 备份到 backups\\，双击那个文件就能还原。"
        "PATH 里的某一段这类只给提示，不做整值删除。"), this);
    tip->setObjectName(QStringLiteral("tip"));
    tip->setWordWrap(true);
    lay->addWidget(tip);

    auto* row = new QHBoxLayout;
    m_scanBtn = new QPushButton(QStringLiteral("开始分析"), this);
    m_delBtn = new QPushButton(QStringLiteral("备份并删除所选"), this);
    auto* reportBtn = new QPushButton(QStringLiteral("导出报告"), this);
    auto* backupBtn = new QPushButton(QStringLiteral("打开备份目录"), this);
    m_delBtn->setEnabled(false);
    row->addWidget(m_scanBtn);
    row->addWidget(m_delBtn);
    row->addWidget(reportBtn);
    row->addWidget(backupBtn);
    row->addStretch(1);
    lay->addLayout(row);

    m_summary = new QLabel(QStringLiteral("还没分析。COM 组件那一类要遍历上万个键，大约 1~3 秒。"), this);
    m_summary->setObjectName(QStringLiteral("muted"));
    m_summary->setWordWrap(true);
    lay->addWidget(m_summary);

    m_list = new QListWidget(this);
    lay->addWidget(m_list, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    connect(m_scanBtn, &QPushButton::clicked, this, &RegistryPage::startScan);
    connect(m_delBtn, &QPushButton::clicked, this, &RegistryPage::deleteSelected);
    connect(reportBtn, &QPushButton::clicked, this, &RegistryPage::exportReport);
    connect(backupBtn, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(Utils::backupsDir()));
    });
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        const auto sel = m_list->selectedItems();
        if (sel.isEmpty())
            return;
        const int idx = sel.first()->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_issues.size())
            return;
        const RegIssue& it = m_issues[idx];
        m_status->setText(QString("%1\\%2%3").arg(it.hiveName, it.subKey,
            it.valueName.isEmpty() ? QString() : QString("  @%1").arg(it.valueName)));
    });
}

void RegistryPage::startScan()
{
    if (m_busy)
        return;
    m_busy = true;
    m_scanBtn->setEnabled(false);
    m_delBtn->setEnabled(false);
    m_status->setText(QStringLiteral("分析中…（COM 组件那一类最慢）"));
    if (!m_watcher) {
        m_watcher = new QFutureWatcher<QVector<RegIssue>>(this);
        connect(m_watcher, &QFutureWatcherBase::finished, this, [this] {
            m_issues = m_watcher->result();
            m_busy = false;
            m_scanBtn->setEnabled(true);
            rebuildList();
        });
    }
    m_watcher->setFuture(QtConcurrent::run([] { return RegClean::scan(); }));
}

void RegistryPage::rebuildList()
{
    m_list->clear();
    m_checked.clear();
    QStringList heads;
    for (const RegIssue::Kind kind : kOrder) {
        QVector<int> rows;
        for (int i = 0; i < m_issues.size(); ++i)
            if (m_issues[i].kind == kind)
                rows.append(i);
        if (rows.isEmpty())
            continue;
        const QString label = QString("── %1（%2）──").arg(kindLabel(kind)).arg(rows.size());
        m_list->addItem(sectionItem(label));
        heads << QString("%1 %2").arg(kindLabel(kind)).arg(rows.size());
        for (const int i : rows) {
            const RegIssue& it = m_issues[i];
            auto* item = new QListWidgetItem();
            item->setData(Qt::UserRole, i);
            item->setSizeHint(QSize(0, 46));

            InfoRow::Badge kind = InfoRow::Neutral;
            QString badge;
            if (it.risk == RegIssue::Risk::Safe) { kind = InfoRow::Good; badge = QStringLiteral("可安全删"); }
            else if (it.risk == RegIssue::Risk::Careful) { kind = InfoRow::Warn; badge = QStringLiteral("谨慎删"); }
            else { kind = InfoRow::Muted; badge = QStringLiteral("只提示"); }
            auto* row = new InfoRow(it.title,
                                    QStringLiteral("%1 · %2").arg(RegClean::categoryLabel(it), it.detail),
                                    badge, kind, QIcon(), 30);
            row->setToolTip(QString("%1\\%2\n%3").arg(it.hiveName, it.subKey, RegClean::riskLabel(it))
                            + (it.badPath.isEmpty() ? QString() : QString("\n失效目标：%1").arg(it.badPath)));
            if (it.deletable()) {
                row->makeCheckable(false);
                connect(row, &InfoRow::checkToggled, this, [this, i](bool on) {
                    if (on)
                        m_checked.insert(i);
                    else
                        m_checked.remove(i);
                    updateDeleteBtn();
                });
            }
            m_list->addItem(item);
            m_list->setItemWidget(item, row);
        }
    }
    // 按类别汇总，顺手把"只提示"的单独说明，免得用户以为工具不干活
    if (m_issues.isEmpty()) {
        m_summary->setText(QStringLiteral("分析完成：没有发现失效项或遗留项。"));
        m_status->setText(QStringLiteral("没有可删的项。"));
    } else {
        int readOnly = 0;
        for (const RegIssue& it : m_issues)
            if (!kindDeletable(it.kind))
                ++readOnly;
        m_summary->setText(heads.join(QStringLiteral(" · "))
            + (readOnly > 0
                   ? QString(" · 其中 %1 项只提示（不能整值删除）").arg(readOnly)
                   : QString()));
        m_status->setText(QString("共 %1 项 · 勾选后要删除的，删除前自动备份").arg(m_issues.size()));
    }
}

void RegistryPage::updateDeleteBtn()
{
    m_delBtn->setEnabled(!m_checked.isEmpty() && !m_busy);
    if (!m_busy && !m_checked.isEmpty())
        m_status->setText(QString("已选 %1 项 · 删除前会先把原值导出成 .reg 备份").arg(m_checked.size()));
}

void RegistryPage::deleteSelected()
{
    QVector<int> picked;
    for (const int i : m_checked)
        picked.append(i);
    std::sort(picked.begin(), picked.end());
    if (picked.isEmpty()) {
        m_status->setText(QStringLiteral("先勾上要处理的项。"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("确认删除"),
            QString("要删除 %1 项注册表记录，删除前会先把原值导出成 .reg 备份。\n\n"
                    "HKLM / HKU 下的项需要管理员权限，没权限的会报失败并留在列表里。\n确定继续吗？")
                .arg(picked.size()))
        != QMessageBox::Yes)
        return;

    int ok = 0;
    QStringList fails;
    QSet<int> gone;
    for (const int i : picked) {
        QString err;
        if (RegClean::deleteIssue(m_issues[i], &err)) {
            ++ok;
            gone.insert(i);
        } else {
            fails << QString("%1：%2").arg(m_issues[i].title, err);
        }
    }
    QVector<RegIssue> left;
    for (int i = 0; i < m_issues.size(); ++i)
        if (!gone.contains(i))
            left.append(m_issues[i]);
    m_issues = left;
    rebuildList();

    QString msg = QString("已删除 %1/%2 项 · 备份在 backups\\registry-backup-*.reg")
                      .arg(ok).arg(picked.size());
    if (!fails.isEmpty())
        msg += QString(" · 失败 %1 项（%2）").arg(fails.size()).arg(fails.first());
    m_status->setText(msg);
    Utils::log(QString("registry page: deleted %1/%2").arg(ok).arg(picked.size()));
}

void RegistryPage::exportReport()
{
    if (m_issues.isEmpty()) {
        m_status->setText(QStringLiteral("还没有分析结果可导出。"));
        return;
    }
    const QString path = QString("%1/registry-report.txt").arg(Utils::dataDir());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_status->setText(QStringLiteral("写报告失败：%1").arg(path));
        return;
    }
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << "ltool 注册表分析报告\n\n";
    for (const RegIssue& it : m_issues) {
        ts << QString("[%1] %2\n  位置：%3\\%4%5\n  原因：%6\n  风险：%7\n\n")
                  .arg(RegClean::categoryLabel(it), it.title, it.hiveName, it.subKey)
                  .arg(it.valueName.isEmpty() ? QString() : QString(" @%1").arg(it.valueName),
                       it.detail, RegClean::riskLabel(it));
    }
    f.close();
    Utils::revealInExplorer(path);
    m_status->setText(QString("报告已导出：%1").arg(path));
}

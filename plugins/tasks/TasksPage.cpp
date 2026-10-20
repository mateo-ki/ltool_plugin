#include "TasksPage.h"

#include "TaskScheduler.h"
#include "Utils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimeEdit>
#include <QUuid>
#include <QVBoxLayout>

namespace {

QString scheduleLabel(const QString& schedule)
{
    if (schedule.startsWith(QLatin1String("interval:")))
        return QStringLiteral("每隔 %1 分钟").arg(schedule.mid(9).toInt());
    if (schedule.startsWith(QLatin1String("daily:")))
        return QStringLiteral("每天 %1").arg(schedule.mid(6));
    return schedule;
}

// 列表行号就是任务序号（rebuild 按 tasks() 的顺序一条一行），所以选中项只要行号
int selectedRow(const QListWidget* list)
{
    return list->currentRow();
}

// 新建和编辑共用：填好一条任务返回 true
bool taskDialog(QWidget* parent, ScheduledTask* t)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(t->name.isEmpty() ? QStringLiteral("新建定时任务")
                                         : QStringLiteral("编辑定时任务"));
    dlg.setMinimumWidth(460);
    auto* lay = new QVBoxLayout(&dlg);
    auto* form = new QFormLayout;

    auto* name = new QLineEdit(t->name, &dlg);
    auto* type = new QComboBox(&dlg);
    type->addItems(TaskScheduler::typeNames());
    const int ti = type->findText(t->type);
    type->setCurrentIndex(ti >= 0 ? ti : 0);

    auto* script = new QLineEdit(t->script, &dlg);
    script->setPlaceholderText(QStringLiteral("脚本或源文件，例如 D:\\work\\stats.py"));
    auto* pick = new QPushButton(QStringLiteral("浏览…"), &dlg);
    auto* scriptRow = new QWidget(&dlg);
    auto* scriptLay = new QHBoxLayout(scriptRow);
    scriptLay->setContentsMargins(0, 0, 0, 0);
    scriptLay->addWidget(script, 1);
    scriptLay->addWidget(pick);

    // 计划两种排法，同一时刻只有一格能用
    auto* mode = new QComboBox(&dlg);
    mode->addItems({QStringLiteral("每隔若干分钟"), QStringLiteral("每天固定时刻")});
    auto* every = new QSpinBox(&dlg);
    every->setRange(1, 24 * 60);
    every->setSuffix(QStringLiteral(" 分钟"));
    auto* at = new QTimeEdit(QTime(9, 0), &dlg);
    at->setDisplayFormat(QStringLiteral("HH:mm"));
    if (t->schedule.startsWith(QLatin1String("daily:"))) {
        mode->setCurrentIndex(1);
        const QTime q = QTime::fromString(t->schedule.mid(6), QStringLiteral("HH:mm"));
        if (q.isValid())
            at->setTime(q);
    } else {
        mode->setCurrentIndex(0);
        every->setValue(qMax(1, t->schedule.mid(9).toInt()));
    }
    const auto syncMode = [mode, every, at]() {
        every->setEnabled(mode->currentIndex() == 0);
        at->setEnabled(mode->currentIndex() == 1);
    };
    QObject::connect(mode, &QComboBox::currentIndexChanged, &dlg,
                     [syncMode](int) { syncMode(); });
    syncMode();

    auto* enabled = new QCheckBox(QStringLiteral("启用"), &dlg);
    enabled->setChecked(t->enabled);

    form->addRow(QStringLiteral("名称"), name);
    form->addRow(QStringLiteral("语言"), type);
    form->addRow(QStringLiteral("脚本"), scriptRow);
    form->addRow(QStringLiteral("计划"), mode);
    form->addRow(QString(), every);
    form->addRow(QString(), at);
    form->addRow(QStringLiteral("状态"), enabled);
    lay->addLayout(form);

    auto* hint = new QLabel(
        QStringLiteral("到点就把脚本跑一遍，输出落在 %1\\tasklogs。编译型（c++/rust）先编译再跑，"
                       "编译器从 PATH 或「语言」那一页配的环境里找。").arg(Utils::dataDir()),
        &dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8b93a7;"));
    lay->addWidget(hint);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(btns);
    QObject::connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(pick, &QPushButton::clicked, &dlg, [script]() {
        const QString f = QFileDialog::getOpenFileName(
            script->parentWidget(), QStringLiteral("选脚本"),
            QFileInfo(script->text()).absolutePath());
        if (!f.isEmpty())
            script->setText(QDir::fromNativeSeparators(f));
    });
    if (dlg.exec() != QDialog::Accepted)
        return false;

    t->name = name->text().trimmed();
    t->type = type->currentText();
    t->script = script->text().trimmed();
    t->schedule = mode->currentIndex() == 0
        ? QStringLiteral("interval:%1").arg(every->value())
        : QStringLiteral("daily:%1").arg(at->time().toString(QStringLiteral("HH:mm")));
    t->enabled = enabled->isChecked();
    return !t->name.isEmpty() && !t->script.isEmpty();
}

} // namespace

TasksPage::TasksPage(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    auto* tip = new QLabel(
        QStringLiteral("定时任务：到点把脚本跑一遍（python / js / java / c++ / rust）。"
                       "定时器 30 秒走一次，所以「每天」那一档最多晚半分钟触发。"),
        this);
    tip->setWordWrap(true);
    root->addWidget(tip);

    m_list = new QListWidget(this);
    m_list->setAlternatingRowColors(true);
    root->addWidget(m_list, 1);

    auto* btns = new QHBoxLayout;
    auto* addBtn = new QPushButton(QStringLiteral("新建"), this);
    auto* editBtn = new QPushButton(QStringLiteral("编辑"), this);
    auto* delBtn = new QPushButton(QStringLiteral("删除"), this);
    auto* runBtn = new QPushButton(QStringLiteral("立即运行"), this);
    m_enableBtn = new QPushButton(QStringLiteral("停用"), this);
    btns->addWidget(addBtn);
    btns->addWidget(editBtn);
    btns->addWidget(delBtn);
    btns->addStretch(1);
    btns->addWidget(runBtn);
    btns->addWidget(m_enableBtn);
    root->addLayout(btns);

    connect(addBtn, &QPushButton::clicked, this, &TasksPage::addTask);
    connect(editBtn, &QPushButton::clicked, this, &TasksPage::editTask);
    connect(delBtn, &QPushButton::clicked, this, &TasksPage::removeTask);
    connect(runBtn, &QPushButton::clicked, this, &TasksPage::runSelected);
    connect(m_enableBtn, &QPushButton::clicked, this, &TasksPage::toggleEnabled);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &TasksPage::editTask);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int) {
        const auto tasks = TaskScheduler::instance().tasks();
        const int i = selectedRow(m_list);
        const bool has = i >= 0 && i < tasks.size();
        m_enableBtn->setEnabled(has);
        m_enableBtn->setText(has && tasks[i].enabled ? QStringLiteral("停用")
                                                     : QStringLiteral("启用"));
    });
    connect(&TaskScheduler::instance(), &TaskScheduler::tasksChanged, this, &TasksPage::rebuild);

    rebuild();
}

void TasksPage::rebuild()
{
    const int keep = selectedRow(m_list);
    m_list->clear();
    const auto tasks = TaskScheduler::instance().tasks();
    for (const ScheduledTask& t : tasks) {
        QString line = QStringLiteral("%1  [%2]  %3").arg(scheduleLabel(t.schedule), t.type, t.name);
        line += QStringLiteral("\n%1").arg(t.script);
        line += t.lastRunMs > 0
            ? QStringLiteral("   上次 %1").arg(
                  QDateTime::fromMSecsSinceEpoch(t.lastRunMs).toString("yyyy-MM-dd hh:mm"))
            : QStringLiteral("   还没跑过");
        auto* it = new QListWidgetItem(line, m_list);
        if (!t.enabled)
            it->setForeground(QColor(125, 134, 152));
        it->setSizeHint(QSize(0, 48));
    }
    if (keep >= 0 && keep < tasks.size())
        m_list->setCurrentRow(keep);
    if (m_list->count() == 0) {
        m_enableBtn->setEnabled(false);
        m_enableBtn->setText(QStringLiteral("启用"));
    } else if (m_list->currentRow() < 0) {
        // 没选中过就别让按钮停在灰着的状态； setCurrentRow 会顺带刷新那一栏
        m_list->setCurrentRow(0);
    }
}

void TasksPage::addTask()
{
    ScheduledTask t;
    t.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    t.enabled = true;
    t.type = QStringLiteral("python");
    t.schedule = QStringLiteral("interval:60");
    if (!taskDialog(this, &t))
        return;
    auto tasks = TaskScheduler::instance().tasks();
    tasks.append(t);
    TaskScheduler::instance().setTasks(tasks);
    Utils::log(QString("tasks: 新建 %1 (%2 %3)").arg(t.name, t.type, t.schedule));
}

void TasksPage::editTask()
{
    auto tasks = TaskScheduler::instance().tasks();
    const int i = selectedRow(m_list);
    if (i < 0 || i >= tasks.size())
        return;
    ScheduledTask t = tasks[i];
    if (!taskDialog(this, &t))
        return;
    tasks[i] = t;
    TaskScheduler::instance().setTasks(tasks);
}

void TasksPage::removeTask()
{
    auto tasks = TaskScheduler::instance().tasks();
    const int i = selectedRow(m_list);
    if (i < 0 || i >= tasks.size())
        return;
    if (QMessageBox::question(this, QStringLiteral("删除定时任务"),
                              QStringLiteral("删掉「%1」？脚本本身不动。").arg(tasks[i].name))
        != QMessageBox::Yes)
        return;
    tasks.removeAt(i);
    TaskScheduler::instance().setTasks(tasks);
}

void TasksPage::runSelected()
{
    auto tasks = TaskScheduler::instance().tasks();
    const int i = selectedRow(m_list);
    if (i < 0 || i >= tasks.size())
        return;
    const QString err = TaskScheduler::instance().runNow(tasks[i].id);
    if (!err.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("运行失败"), err);
        return;
    }
    // runNow 只保证"已经交出去跑"，输出落在 tasklogs 里，所以这里指个路
    QMessageBox::information(this, QStringLiteral("已提交"),
                             QStringLiteral("输出在 %1\\tasklogs").arg(Utils::dataDir()));
}

void TasksPage::toggleEnabled()
{
    auto tasks = TaskScheduler::instance().tasks();
    const int i = selectedRow(m_list);
    if (i < 0 || i >= tasks.size())
        return;
    tasks[i].enabled = !tasks[i].enabled;
    TaskScheduler::instance().setTasks(tasks);
}

#include "DownloadsPage.h"
#include "platform/Launch.h"

#include "DownloadManager.h"
#include "FileOps.h"
#include "Utils.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QUrl>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 一条任务：标题行（文件名 + 状态）+ 进度条 + 四个操作按钮
// 按钮用 property("act") 认，refreshRows 靠它决定哪个该禁用
QWidget* makeRow(const DownloadTask& t, QWidget* parent)
{
    auto* row = new QWidget(parent);
    row->setProperty("id", t.id);
    auto* lay = new QVBoxLayout(row);
    lay->setContentsMargins(0, 4, 0, 4);
    lay->setSpacing(4);

    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    auto* name = new QLabel(QFileInfo(t.file).fileName(), row);
    name->setObjectName(QStringLiteral("name"));
    auto* info = new QLabel(row);
    info->setObjectName(QStringLiteral("info"));
    info->setStyleSheet(QStringLiteral("color:#8b93a7;"));
    top->addWidget(name, 1);
    top->addWidget(info, 0);
    lay->addLayout(top);

    auto* bar = new QProgressBar(row);
    bar->setObjectName(QStringLiteral("bar"));
    bar->setRange(0, 100);
    bar->setTextVisible(false);
    bar->setFixedHeight(6);
    lay->addWidget(bar);

    auto* btns = new QHBoxLayout;
    btns->setContentsMargins(0, 0, 0, 0);
    btns->setSpacing(6);
    const auto addBtn = [row, btns](const char* act, const QString& text) {
        auto* b = new QPushButton(text, row);
        b->setProperty("act", QString::fromLatin1(act));
        b->setFixedHeight(24);
        btns->addWidget(b);
    };
    addBtn("pause", QStringLiteral("暂停"));
    addBtn("resume", QStringLiteral("继续"));
    addBtn("open", QStringLiteral("打开位置"));
    addBtn("del", QStringLiteral("删除"));
    btns->addStretch(1);
    lay->addLayout(btns);
    return row;
}

} // namespace

DownloadsPage::DownloadsPage(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    auto* head = new QHBoxLayout;
    auto* tip = new QLabel(
        QStringLiteral("下载器：一个大文件切成几段并行拉（服务器支持 Range 才分段），"
                       "断点靠 Range 续。默认存到软件目录。"),
        this);
    tip->setWordWrap(true);
    head->addWidget(tip, 1);
    auto* addBtn = new QPushButton(QStringLiteral("添加下载"), this);
    auto* openBtn = new QPushButton(QStringLiteral("打开下载目录"), this);
    head->addWidget(addBtn);
    head->addWidget(openBtn);
    root->addLayout(head);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* holder = new QWidget(scroll);
    m_rowsLay = new QVBoxLayout(holder);
    m_rowsLay->setContentsMargins(0, 6, 0, 0);
    m_rowsLay->setSpacing(2);
    m_rowsLay->addStretch(1);
    scroll->setWidget(holder);
    root->addWidget(scroll, 1);

    connect(addBtn, &QPushButton::clicked, this, &DownloadsPage::addDownload);
    connect(openBtn, &QPushButton::clicked, this, [this]() {
        const QString dir = DownloadManager::instance().saveDir();
        QDir().mkpath(dir);
        Platform::launch(dir, {}, {}, true);
    });
    connect(&DownloadManager::instance(), &DownloadManager::changed, this,
            &DownloadsPage::refreshRows);

    // 速度要按时间差算，所以自己也要定期看一眼（不只在 changed 时）
    m_poll = new QTimer(this);
    m_poll->setInterval(500);
    connect(m_poll, &QTimer::timeout, this, &DownloadsPage::refreshRows);
    m_poll->start();

    rebuild();
}

void DownloadsPage::addDownload()
{
    bool ok = false;
    const QString url = QInputDialog::getText(this, QStringLiteral("添加下载"),
                                              QStringLiteral("下载地址（http/https）"),
                                              QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || url.isEmpty())
        return;
    const QString guess = QFileInfo(QUrl(url).path()).fileName();
    const QString def = DownloadManager::instance().saveDir() + "/"
                        + (guess.isEmpty() ? QStringLiteral("download.bin") : guess);
    const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("存到哪里"), def);
    if (file.isEmpty())
        return;
    DownloadManager::instance().add(url, file);
    rebuild();
}

void DownloadsPage::clearRows()
{
    if (!m_rowsLay)
        return;
    while (m_rowsLay->count() > 1) {
        QLayoutItem* it = m_rowsLay->takeAt(0);
        if (it->widget())
            it->widget()->deleteLater();
        delete it;
    }
}

void DownloadsPage::rebuild()
{
    clearRows();
    const auto& tasks = DownloadManager::instance().tasks();
    for (const DownloadTask& t : tasks) {
        QWidget* row = makeRow(t, nullptr);
        m_rowsLay->insertWidget(m_rowsLay->count() - 1, row);
        const QString id = t.id;
        const QString file = t.file;
        const auto buttons = row->findChildren<QPushButton*>();
        for (QPushButton* b : buttons) {
            const QString act = b->property("act").toString();
            connect(b, &QPushButton::clicked, this, [this, act, id, file]() {
                DownloadManager& dm = DownloadManager::instance();
                if (act == QLatin1String("pause")) {
                    dm.pause(id);
                } else if (act == QLatin1String("resume")) {
                    dm.resume(id);
                } else if (act == QLatin1String("open")) {
                    if (QFile::exists(file))
                        Platform::reveal(file);
                    else
                        Platform::launch(dm.saveDir(), {}, {}, true);
                } else if (act == QLatin1String("del")) {
                    // remove() 会连没下完的半成品文件一起删掉，所以先问一句
                    if (QMessageBox::question(this, QStringLiteral("删除这条下载"),
                                              QStringLiteral("要把任务和已下载的部分一起删掉：%1")
                                                  .arg(QFileInfo(file).fileName()))
                        == QMessageBox::Yes)
                        dm.remove(id);
                    rebuild();
                }
            });
        }
    }
    refreshRows();
}

void DownloadsPage::refreshRows()
{
    const auto& tasks = DownloadManager::instance().tasks();
    QStringList ids;
    for (const DownloadTask& t : tasks)
        ids.append(t.id);
    QStringList shown;
    for (int i = 0; i < m_rowsLay->count(); ++i) {
        QWidget* w = m_rowsLay->itemAt(i)->widget();
        if (w)
            shown.append(w->property("id").toString());
    }
    if (shown != ids) {   // 条数或顺序变了：整列重建（新增/删除是低频动作）
        rebuild();
        return;
    }

    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    const qint64 nowMs = clock.elapsed();

    for (int i = 0; i < m_rowsLay->count(); ++i) {
        QWidget* row = m_rowsLay->itemAt(i)->widget();
        if (!row)
            continue;
        const DownloadTask* t = nullptr;
        for (const DownloadTask& tt : tasks)
            if (tt.id == row->property("id").toString())
                t = &tt;
        if (!t)
            continue;

        const qint64 got = t->receivedTotal();
        qint64 lastBytes = row->property("bytes").toLongLong();
        qint64 lastMs = row->property("ms").toLongLong();
        double speed = 0.0;
        if (lastMs > 0 && nowMs > lastMs && got >= lastBytes)
            speed = double(got - lastBytes) * 1000.0 / double(nowMs - lastMs);
        if (nowMs > lastMs) {   // 时间戳往前滚，字节数跟着当前值
            row->setProperty("bytes", got);
            row->setProperty("ms", nowMs);
        }

        auto* bar = row->findChild<QProgressBar*>(QStringLiteral("bar"));
        if (t->total > 0) {
            bar->setRange(0, 100);
            bar->setValue(int(qint64(got) * 100 / t->total));
        } else {
            bar->setRange(0, 0);   // 服务器没给长度：跑马灯，别装作停在 0%
        }
        auto* info = row->findChild<QLabel*>(QStringLiteral("info"));
        QString text = QStringLiteral("%1 / %2").arg(FileOps::human(got),
                                                     t->total > 0 ? FileOps::human(t->total)
                                                                  : QStringLiteral("未知"));
        if (t->status == QStringLiteral("下载中"))
            text += QStringLiteral("  %1/s").arg(FileOps::human(qint64(speed)));
        text.prepend(t->status + QStringLiteral("  "));
        if (!t->error.isEmpty())
            text += QStringLiteral("  （%1）").arg(t->error);
        info->setText(text);

        const bool running = t->status == QStringLiteral("下载中")
                             || t->status == QStringLiteral("等待");
        const auto buttons = row->findChildren<QPushButton*>();
        for (QPushButton* b : buttons) {
            const QString act = b->property("act").toString();
            if (act == QLatin1String("pause"))
                b->setEnabled(running);
            else if (act == QLatin1String("resume"))
                b->setEnabled(!running && t->status != QStringLiteral("完成"));
            else if (act == QLatin1String("open"))
                b->setEnabled(QFile::exists(t->file));
        }
    }
}

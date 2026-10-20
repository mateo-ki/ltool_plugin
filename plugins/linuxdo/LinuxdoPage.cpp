#include "LinuxdoPage.h"

#include "LinuxdoWatch.h"
#include "Utils.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

LinuxdoPage::LinuxdoPage(QWidget* parent)
    : QWidget(parent)
{
    LinuxdoWatch& w = LinuxdoWatch::instance();
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    auto* row1 = new QHBoxLayout;
    row1->setSpacing(6);
    m_on = new QCheckBox(QStringLiteral("盯着"), this);
    m_on->setObjectName(QStringLiteral("linuxdoEnabled"));
    m_on->setChecked(w.enabled());
    m_minutes = new QSpinBox(this);
    m_minutes->setObjectName(QStringLiteral("linuxdoMinutes"));
    m_minutes->setRange(3, 240);
    m_minutes->setSuffix(QStringLiteral(" 分钟一查"));
    m_minutes->setValue(w.intervalMinutes());
    m_now = new QPushButton(QStringLiteral("立即查一次"), this);
    m_now->setObjectName(QStringLiteral("linuxdoNow"));
    m_now->setCursor(Qt::PointingHandCursor);
    row1->addWidget(m_on);
    row1->addWidget(m_minutes);
    row1->addWidget(m_now);
    row1->addStretch(1);
    lay->addLayout(row1);

    auto* row2 = new QHBoxLayout;
    row2->setSpacing(6);
    row2->addWidget(new QLabel(QStringLiteral("关键词"), this));
    m_words = new QLineEdit(w.keywords().join(QStringLiteral(", ")), this);
    m_words->setObjectName(QStringLiteral("linuxdoWords"));
    m_words->setPlaceholderText(QStringLiteral("逗号分开，比的是标题和标签，例如：福利,女装,公益"));
    row2->addWidget(m_words, 1);
    lay->addLayout(row2);

    auto* row3 = new QHBoxLayout;
    row3->setSpacing(6);
    row3->addWidget(new QLabel(QStringLiteral("中转前缀"), this));
    m_prefix = new QLineEdit(w.prefix(), this);
    m_prefix->setObjectName(QStringLiteral("linuxdoPrefix"));
    m_prefix->setPlaceholderText(QStringLiteral("留空 = 只用直连（这台机器上直连是死的）"));
    row3->addWidget(m_prefix, 1);
    lay->addLayout(row3);

    m_hits = new QListWidget(this);
    m_hits->setObjectName(QStringLiteral("linuxdoHits"));
    lay->addWidget(m_hits, 1);

    auto* row4 = new QHBoxLayout;
    row4->setSpacing(6);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("linuxdoStatus"));
    m_status->setWordWrap(true);
    m_open = new QPushButton(QStringLiteral("打开这条"), this);
    m_open->setObjectName(QStringLiteral("linuxdoOpen"));
    m_open->setCursor(Qt::PointingHandCursor);
    row4->addWidget(m_status, 1);
    row4->addWidget(m_open);
    lay->addLayout(row4);

    m_cats = new QLabel(this);
    m_cats->setObjectName(QStringLiteral("linuxdoCats"));
    m_cats->setWordWrap(true);
    m_cats->setStyleSheet(QStringLiteral("color:#8b93a1;font-size:8.5pt;"));
    lay->addWidget(m_cats);

    auto* note = new QLabel(QStringLiteral(
        "关键词比的是<b>标题、标签和分类名</b>（实测：新帖里标题含「福利」的 0 条，但在分类「福利羊毛」里的有 4 条，所以分类名必须参与比对）。看到的是<b>登出态的公开内容</b>：Cloudflare 把 cf_clearance 绑在浏览器的 TLS 指纹上，"
        "非浏览器客户端带着 cookie 也换不来登录态（实测三条路径全回挑战页）。"
        "系统气泡点了开不了浏览器（Qt6 在 Windows 上不吐这个点击事件），所以要点着看就来这一页。"),
        this);
    note->setObjectName(QStringLiteral("linuxdoNote"));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#8b93a1;font-size:8.5pt;"));
    lay->addWidget(note);

    connect(m_on, &QCheckBox::toggled, this, [&w](bool on) { w.setEnabled(on); });
    connect(m_minutes, &QSpinBox::valueChanged, this, [&w](int v) { w.setIntervalMinutes(v); });
    connect(m_words, &QLineEdit::editingFinished, this, [this, &w] {
        w.setKeywords(m_words->text().split(QLatin1Char(','), Qt::SkipEmptyParts));
        m_words->setText(w.keywords().join(QStringLiteral(", ")));
    });
    connect(m_prefix, &QLineEdit::editingFinished, this, [&w, this] {
        w.setPrefix(m_prefix->text());
        m_prefix->setText(w.prefix());
    });
    connect(m_now, &QPushButton::clicked, this, [&w] { w.checkNow(); });
    connect(m_open, &QPushButton::clicked, this, [this] { openCurrent(); });
    connect(m_hits, &QListWidget::itemActivated, this, [this] { openCurrent(); });
    connect(&w, &LinuxdoWatch::changed, this, [this] { refresh(); });
    refresh();
}

void LinuxdoPage::openCurrent()
{
    auto* it = m_hits->currentItem();
    if (!it)
        return;
    const QString url = it->data(Qt::UserRole).toString();
    if (url.startsWith(QStringLiteral("https://linux.do/")))
        QDesktopServices::openUrl(QUrl(url));
}

void LinuxdoPage::refresh()
{
    const LinuxdoWatch& w = LinuxdoWatch::instance();
    m_status->setText(w.statusLine());
    // 分类表没拿到的时候「福利」这类词根本撞不到 —— 这个降级状态必须写在页面上，
    // 不能让人以为"今天没有新帖"
    m_cats->setText(w.categories().isEmpty()
        ? QStringLiteral("分类表：还没拿到 —— 现在只能比标题和标签，「福利」这类只出现在分类名上的词撞不到")
        : QStringLiteral("分类表：已拿到 %1 个分类%2")
              .arg(w.categories().size())
              .arg(w.catCheckText().isEmpty() ? QString() : QStringLiteral("（%1）").arg(w.catCheckText())));
    m_now->setEnabled(!w.busy());
    const int keep = m_hits->currentRow();
    m_hits->clear();
    for (const LinuxdoHit& h : w.hits()) {
        const QString when = h.topic.createdAt.left(10);
        const QString why = h.where.isEmpty()
            ? QStringLiteral("命中「%1」").arg(h.matched)
            : QStringLiteral("命中「%1」在%2").arg(h.matched, h.where);
        auto* it = new QListWidgetItem(QStringLiteral("%1  ·  %2  ·  %3")
                                           .arg(h.topic.title.left(60), why, when),
                                       m_hits);
        it->setData(Qt::UserRole, h.topic.url);
    }
    if (keep >= 0 && keep < m_hits->count())
        m_hits->setCurrentRow(keep);
    m_open->setEnabled(m_hits->currentItem() != nullptr);
}

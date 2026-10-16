#include "ClipboardPage.h"

#include "ClipboardStore.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

namespace {

// 一行显示什么：时间 + 那条的摘要。摘要是 core 那侧同一份函数
//（搜索框里「剪贴板历史」那一节用的也是它），两边显示出来的东西不会分叉。
QString rowText(const ClipboardStore::Item& it)
{
    return QStringLiteral("%1  %2").arg(it.when.toString(QStringLiteral("MM-dd HH:mm:ss")),
                                        ClipboardStore::summary(it));
}

// 列表上按 Delete = 删掉选中那条。装一个只在 m_list 上生效的事件过滤器，
// 不 subclass QListWidget —— 这页只要这一处键盘动作，犯不上造一个类。
class DeleteKeyFilter : public QObject {
public:
    DeleteKeyFilter(ClipboardPage* page, QObject* parent) : QObject(parent), m_page(page) {}

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        auto* list = qobject_cast<QListWidget*>(obj);
        if (!list || ev->type() != QEvent::KeyPress)
            return false;
        auto* k = static_cast<QKeyEvent*>(ev);
        if (k->key() != Qt::Key_Delete)
            return false;
        auto* row = list->currentItem();
        if (!row)
            return true;                       // 没选中，吞掉这一下（别把 Delete 传出去删了文件）
        ClipboardStore::instance().remove(row->data(Qt::UserRole).toInt());
        m_page->refresh();
        return true;
    }

private:
    ClipboardPage* m_page = nullptr;
};

} // namespace

QVector<int> ClipboardPage::filterIndices(const QVector<ClipboardStore::Item>& items,
                                          const QString& needleRaw)
{
    const QString needle = needleRaw.trimmed().toLower();
    QVector<int> out;
    for (int i = 0; i < items.size(); ++i) {
        if (needle.isEmpty()) {
            out.append(i);        // 没输入 = 全部（不是"一条都不显示"）
            continue;
        }
        const ClipboardStore::Item& it = items[i];
        // 能搜的三样：内容本身（文本条目）、那一行显示的字（图片就靠它筛"图片"和尺寸）、时间
        QString hay = ClipboardStore::summary(it).toLower();
        if (!it.isImage)
            hay += QLatin1Char('\n') + it.text.toLower();
        hay += QLatin1Char('\n') + it.when.toString(QStringLiteral("yyyy-MM-dd HH:mm")).toLower();
        if (hay.contains(needle))
            out.append(i);
    }
    return out;
}

ClipboardPage::ClipboardPage(QWidget* parent)
    : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("clipStatus"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(8);
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("clipFilter"));
    m_filter->setPlaceholderText(QStringLiteral("按关键字过滤（内容 / 图片 / 时间都行）"));
    m_filter->setClearButtonEnabled(true);
    topRow->addWidget(m_filter, 1);
    auto* clear = new QPushButton(QStringLiteral("清空记录"), this);
    clear->setObjectName(QStringLiteral("clipClear"));
    topRow->addWidget(clear);
    lay->addLayout(topRow);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("clipList"));
    m_list->setUniformItemSizes(true);
    m_list->installEventFilter(new DeleteKeyFilter(this, m_list));
    lay->addWidget(m_list, 1);

    auto* hint = new QLabel(QStringLiteral("双击一行 = 放回剪贴板 · 选中按 Delete = 从记录里去掉这一条"), this);
    hint->setObjectName(QStringLiteral("clipHint"));
    hint->setWordWrap(true);
    lay->addWidget(hint);

    connect(m_filter, &QLineEdit::textChanged, this, [this] { buildRows(); });

    // 双击 = 把那条放回剪贴板。走 core 那侧同一个 restore（搜索框那一节也是它），
    // 这里不抄第二份"写剪贴板"的逻辑。
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* row) {
        if (!row)
            return;
        ClipboardStore::instance().restore(row->data(Qt::UserRole).toInt());
        refresh();
    });

    // 清空：只做"去掉记录"，不碰那个开关（开关归配置页那颗，两处都能拨迟早打架）。
    connect(clear, &QPushButton::clicked, this, [this] {
        ClipboardStore::instance().clear();
        refresh();
    });

    refresh();
}

void ClipboardPage::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();        // 这一页每次被看见都跟一次真数据，不拿着上次的快照当现在的
}

void ClipboardPage::refresh()
{
    buildRows();
    m_status->setText(statusText());
}

void ClipboardPage::buildRows()
{
    const QVector<ClipboardStore::Item>& items = ClipboardStore::instance().items();
    const QVector<int> picked = filterIndices(items, m_filter ? m_filter->text() : QString());
    m_list->clear();
    for (const int idx : picked) {
        const ClipboardStore::Item& it = items[idx];
        auto* row = new QListWidgetItem(ClipboardStore::instance().iconFor(it),
                                        rowText(it), m_list);
        row->setData(Qt::UserRole, idx);        // 存的是**存储下标**，行号会变，这个不会
        row->setToolTip(it.isImage ? it.imagePath : it.text.left(4000));
    }
}

int ClipboardPage::rowCount() const
{
    return m_list ? m_list->count() : 0;
}

QString ClipboardPage::rowTextAt(int row) const
{
    if (!m_list || row < 0 || row >= m_list->count())
        return QString();
    return m_list->item(row)->text();
}

int ClipboardPage::storeIndexAt(int row) const
{
    if (!m_list || row < 0 || row >= m_list->count())
        return -1;
    return m_list->item(row)->data(Qt::UserRole).toInt();
}

QString ClipboardPage::statusText() const
{
    auto& cs = ClipboardStore::instance();
    const int total = cs.items().size();
    // 状态行要说实话：没在记录的时候不能只报"0 条"让人以为功能坏了，
    // 也不能摆着一句"记录中"骗人。
    QString s = cs.enabled()
        ? QStringLiteral("正在记录 · 共 %1 条（最多留 %2 条，图片另存在 clipimg\\）")
              .arg(total).arg(ClipboardStore::kMaxItems)
        : QStringLiteral("没在记录 —— 开关在 配置 页那一栏（已经存下的 %1 条还在，可以照常复用）")
              .arg(total);
    if (m_filter && !m_filter->text().trimmed().isEmpty() && m_list)
        s += QStringLiteral(" · 过滤出 %1 条").arg(m_list->count());
    return s;
}

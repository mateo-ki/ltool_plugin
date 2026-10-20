#include "ClipboardPage.h"

#include "ClipboardStore.h"
#include "Utils.h"

#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

// ---- 卡片上那三块的尺寸（唯一的一份，判据问的也是这几个数）----
// 一张卡 208x176：顶部一行时间，中间一块最多 120x84 的缩略图，下面是文案。
// 纯文本那张没有图，文案就从时间下面一直排到卡底 —— 所以 bodyRect 要分两种。
namespace {
constexpr int kCardW = 208;
constexpr int kCardH = 176;
constexpr int kPad = 9;
constexpr int kThumbH = 84;
constexpr int kTimeH = 16;
constexpr int kGap = 8;
} // namespace

QSize ClipboardPage::cardSize()
{
    return QSize(kCardW, kCardH);
}

QRect ClipboardPage::timeRect(const QRect& card)
{
    return QRect(card.left() + kPad, card.top() + 7, card.width() - 2 * kPad, kTimeH);
}

QRect ClipboardPage::thumbRect(const QRect& card)
{
    return QRect(card.left() + kPad, card.top() + 27, card.width() - 2 * kPad, kThumbH);
}

QRect ClipboardPage::bodyRect(const QRect& card, bool hasImage)
{
    const int top = hasImage ? 27 + kThumbH + kGap : 27;
    return QRect(card.left() + kPad, card.top() + top, card.width() - 2 * kPad,
                 card.height() - top - kPad);
}

namespace {

// 卡片本身怎么画。颜色抄的是这套深色里那几颗真值（ToolsPage 的卡片同一套写法：
// 底 rgba(255,255,255,0.08)、描边 rgba(255,255,255,0.12)、选中 #3f6ae0），
// 不在这儿发明第四种深色。
class ClipCardDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& index) const override
    {
        const bool selected = opt.state & QStyle::State_Selected;
        const QRect card = opt.rect.adjusted(1, 1, -1, -1);
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setPen(selected ? QColor(0x3f, 0x6a, 0xe0) : QColor(255, 255, 255, 31));
        p->setBrush(selected ? QColor(63, 106, 224, 40) : QColor(255, 255, 255, 20));
        p->drawRoundedRect(card, 6, 6);

        // 时间：一行小灰字，画不下的部分让给它（右边那半截留给"图片/文本"那个词）
        const QString when = index.data(ClipboardPage::TimeRole).toString();
        // 有没有图**必须问那个显式的 bool**：文本条的图标是空 QIcon，可 `QVariant(QIcon())`
        // 不是 null —— 拿 DecorationRole 判的话每张卡都会给一块不存在的图位让路（2026-10-19
        // 离屏渲了一张 PNG 才看出来：三张文本卡都空着上半截）。
        const bool hasImage = index.data(ClipboardPage::ImageRole).toBool();
        QRect tr = ClipboardPage::timeRect(card);
        if (hasImage)
            tr.setWidth(tr.width() - 34);
        p->setPen(QColor(0x8b, 0x93, 0xa7));
        QFont tf = p->font();
        tf.setPointSizeF(8.5);
        p->setFont(tf);
        p->drawText(tr, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(tf).elidedText(when, Qt::ElideRight, tr.width()));
        if (hasImage) {
            QRect tag(card.right() - kPad - 32, tr.top(), 32, tr.height());
            p->drawText(tag, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("图"));
        }

        // 缩略图：等比塞进那块 120x84，居中；图标本身就是 core 落盘的那张小的
        if (hasImage) {
            const QRect box = ClipboardPage::thumbRect(card);
            const QPixmap pm = qvariant_cast<QIcon>(index.data(Qt::DecorationRole))
                                   .pixmap(box.size());
            if (!pm.isNull()) {
                const QRect fit = QRect(box.center() - pm.rect().center(), pm.size());
                p->drawPixmap(fit, pm);
            }
        }

        // 文案：文本那条给的是正文（不是摘要），图片那条给的是"图片 1264×871 · 1.2 MB"
        const QString body = index.data(ClipboardPage::BodyRole).toString();
        QFont bf = p->font();
        bf.setPointSizeF(9.5);
        p->setFont(bf);
        p->setPen(QColor(0xe8, 0xeb, 0xf2));
        const QRect br = ClipboardPage::bodyRect(card, hasImage);
        p->drawText(br, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, body);
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        return ClipboardPage::cardSize();
    }
};

// 卡片上那句文案：文本条目给正文的前 600 字（一张卡装不下更多，画的时候还会按行裁），
// 图片条目给 core 那份摘要 —— 和搜索框那一节、列表那版显示的字是同一个来源。
QString cardBody(const ClipboardStore::Item& it)
{
    if (it.isImage)
        return ClipboardStore::summary(it);
    QString t = it.text;
    t.replace(QLatin1Char('\t'), QLatin1Char(' '));   // 制表符在卡片里会把行撑歪
    if (t.size() > 600)
        t = t.left(600) + QStringLiteral("…");
    return t;
}

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
    // 卡片墙（不是每行一条的列表）。仍然是**一个控件 + 自绘 delegate**，
    // 一行塞一个 setItemWidget 的话 200 条要 0.6 秒，那正是上一轮"打开卡"的形状。
    m_list->setViewMode(QListView::IconMode);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setWrapping(true);
    m_list->setUniformItemSizes(true);
    m_list->setSpacing(8);
    m_list->setIconSize(QSize(120, 84));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setItemDelegate(new ClipCardDelegate(m_list));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->installEventFilter(new DeleteKeyFilter(this, m_list));
    lay->addWidget(m_list, 1);

    auto* hint = new QLabel(QStringLiteral("双击一张卡 = 放回剪贴板 · 选中按 Delete = 从记录里去掉那一条"), this);
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
    QElapsedTimer clock;
    clock.start();
    const QVector<ClipboardStore::Item>& items = ClipboardStore::instance().items();
    const QVector<int> picked = filterIndices(items, m_filter ? m_filter->text() : QString());
    m_list->clear();
    int images = 0;
    for (const int idx : picked) {
        const ClipboardStore::Item& it = items[idx];
        if (it.isImage)
            ++images;
        auto* row = new QListWidgetItem(ClipboardStore::instance().iconFor(it),
                                        rowText(it), m_list);
        row->setData(Qt::UserRole, idx);        // 存的是**存储下标**，行号会变，这个不会
        row->setData(TimeRole, it.when.toString(QStringLiteral("MM-dd HH:mm:ss")));
        row->setData(BodyRole, cardBody(it));
        row->setData(ImageRole, it.isImage);
        row->setSizeHint(ClipboardPage::cardSize());
        row->setToolTip(it.isImage ? it.imagePath : it.text.left(4000));
    }
    // 慢的时候要说得出慢在哪一行数据上：2026-10-19 他报"打开剪贴板很卡"，
    // 而那时离屏判据跑的是空 store，量不到他真实那 52 张图 —— 这颗日志就是留给那一趟的。
    const qint64 ms = clock.elapsed();
    if (ms > 60)
        Utils::log(QString("clipboard: 重铺列表 %1 行（其中图片 %2 张）用了 %3 ms")
                       .arg(picked.size()).arg(images).arg(ms));
}

int ClipboardPage::rowCount() const
{
    return m_list ? m_list->count() : 0;
}

QString ClipboardPage::viewKind() const
{
    if (!m_list)
        return QStringLiteral("没有那张列表");
    return (m_list->viewMode() == QListView::IconMode && m_list->itemDelegate())
        ? QStringLiteral("卡片墙") : QStringLiteral("行列表");
}

QSize ClipboardPage::cardSizeAt(int row) const
{
    if (!m_list || row < 0 || row >= m_list->count())
        return QSize();
    return m_list->item(row)->sizeHint();
}

int ClipboardPage::bodyTopAt(int row) const
{
    if (!m_list || row < 0 || row >= m_list->count())
        return -1;
    const QListWidgetItem* item = m_list->item(row);
    const QRect card(QPoint(0, 0), item->sizeHint().isValid() ? item->sizeHint() : cardSize());
    return bodyRect(card, item->data(ImageRole).toBool()).top();
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
    // 两条上限都说出来：只报"最多留 200 条"会让人以为老条目一直看得见，
    // 而它其实到天数就走了（天数在 配置 页那一栏改，0=不限）。
    const QString caps = cs.keepDays() > 0
        ? QStringLiteral("最多留 %1 条 / %2 天").arg(ClipboardStore::kMaxItems).arg(cs.keepDays())
        : QStringLiteral("最多留 %1 条 · 天数不限").arg(ClipboardStore::kMaxItems);
    QString s = cs.enabled()
        ? QStringLiteral("正在记录 · 共 %1 条（%2，图片另存在 clipimg\\）")
              .arg(total).arg(caps)
        : QStringLiteral("没在记录 —— 开关在 配置 页那一栏（已经存下的 %1 条还在，可以照常复用）")
              .arg(total);
    if (m_filter && !m_filter->text().trimmed().isEmpty() && m_list)
        s += QStringLiteral(" · 过滤出 %1 条").arg(m_list->count());
    return s;
}

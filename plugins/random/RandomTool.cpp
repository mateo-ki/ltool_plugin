#include "RandomTool.h"

#include "Theme.h"
#include "Utils.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace Rand {
namespace {

// 一次最多给多少个：界面上的 QSpinBox 夹到 10000，纯函数这一层也夹住 ——
// 判据跑的就是这一颗，不能"界面拦得住、函数随便造 2GB 列表"。
constexpr int kMaxCount = 100000;
constexpr int kMaxLength = 4096;

qint64 pick(QRandomGenerator& rng, quint64 span)
{
    return static_cast<qint64>(rng.bounded(span));
}

} // namespace

QVector<CharSet> charSets()
{
    QString lower, upper;
    for (ushort u = 'a'; u <= 'z'; ++u)
        lower.append(QChar(u));
    for (ushort u = 'A'; u <= 'Z'; ++u)
        upper.append(QChar(u));
    return {
        { QStringLiteral("d"), QStringLiteral("数字 0-9"),
          QStringLiteral("0123456789"), QStringLiteral("01") },
        { QStringLiteral("l"), QStringLiteral("小写字母"), lower, QStringLiteral("l") },
        { QStringLiteral("u"), QStringLiteral("大写字母"), upper, QStringLiteral("OI") },
        { QStringLiteral("s"), QStringLiteral("符号"),
          QStringLiteral("!@#$%^&*()-_=+[]{};:,.<>?"), QStringLiteral("|") },
    };
}

QString poolOf(const Spec& s)
{
    QString pool;
    for (const CharSet& cs : charSets()) {
        if (!s.sets.contains(cs.key))
            continue;
        QString chars = cs.chars;
        if (s.dropLookalike) {
            // 只在**本档内**删：`0`/`1` 归数字、`l` 归小写、`O`/`I` 归大写、`|` 归符号。
            // 全局删同一个字符表会把没勾的那一档也啃掉，池子大小就对不上界面说的话了。
            for (const QChar& c : cs.lookalike)
                chars.remove(c);
        }
        pool += chars;
    }
    return pool;
}

QString kindName(Kind k)
{
    switch (k) {
    case Kind::Int: return QStringLiteral("整数");
    case Kind::Decimal: return QStringLiteral("小数");
    case Kind::String: return QStringLiteral("字符串");
    case Kind::Uuid: return QStringLiteral("UUID");
    }
    return QStringLiteral("整数");
}

bool generate(const Spec& s, QStringList* out, QString* err)
{
    out->clear();
    if (s.count <= 0) {
        *err = QStringLiteral("数量至少 1 个");
        return false;
    }
    if (s.count > kMaxCount) {
        *err = QStringLiteral("一次最多 %1 个（现在要 %2）").arg(kMaxCount).arg(s.count);
        return false;
    }
    QRandomGenerator* rng = QRandomGenerator::global();

    switch (s.kind) {
    case Kind::Int: {
        if (s.hi < s.lo) {
            *err = QStringLiteral("上界 %1 比下界 %2 小").arg(s.hi).arg(s.lo);
            return false;
        }
        // 无符号回绕正好算出差值：(quint64)hi - (quint64)lo == hi - lo，不会因为
        // 下界是很大的负数而先溢出。整条 2^64 那一档单独挡掉（span 会加回 0）。
        const quint64 diff = static_cast<quint64>(s.hi) - static_cast<quint64>(s.lo);
        if (diff == std::numeric_limits<quint64>::max()) {
            *err = QStringLiteral("区间宽到 2^64 个值，这一档取不了");
            return false;
        }
        const quint64 span = diff + 1;
        if (s.unique && static_cast<quint64>(s.count) > span) {
            *err = QStringLiteral("%1…%2 之间只有 %3 个不同的数，取不出 %4 个不重复的")
                       .arg(s.lo).arg(s.hi).arg(span).arg(s.count);
            return false;
        }
        if (!s.unique) {
            for (int i = 0; i < s.count; ++i)
                out->append(QString::number(s.lo + pick(*rng, span)));
            return true;
        }
        if (span <= static_cast<quint64>(s.count) * 8) {
            // 池子不宽就抽样：把前 count 个位置洗完（部分 Fisher-Yates），
            // 比"一直掷到没重复为止" bounded 次数，也不会在那儿假装卡住是随机数的问题
            QVector<qint64> bag;
            bag.reserve(static_cast<qsizetype>(span));
            for (quint64 i = 0; i < span; ++i)
                bag.append(s.lo + static_cast<qint64>(i));
            for (int i = 0; i < s.count; ++i) {
                const int rest = bag.size() - i;
                const int j = i + static_cast<int>(rng->bounded(static_cast<quint32>(rest)));
                std::swap(bag[i], bag[j]);
                out->append(QString::number(bag[i]));
            }
            return true;
        }
        QSet<qint64> seen;
        while (out->size() < s.count) {
            const qint64 v = s.lo + pick(*rng, span);
            if (seen.contains(v))
                continue;
            seen.insert(v);
            out->append(QString::number(v));   // 按掷出来的顺序写，别按哈希桶顺序
        }
        return true;
    }

    case Kind::Decimal: {
        if (s.hi < s.lo) {
            *err = QStringLiteral("上界 %1 比下界 %2 小").arg(s.hi).arg(s.lo);
            return false;
        }
        if (s.unique) {
            // 这一档没给"不重复"这个口子（位数一夹必然撞），函数也不许偷偷去实现一个
            *err = QStringLiteral("小数这一档不提供“不重复”：位数夹到 %1 位，%2 个数不可能都不撞")
                       .arg(s.digits).arg(s.count);
            return false;
        }
        const double a = static_cast<double>(s.lo);
        const double b = static_cast<double>(s.hi);
        const int digits = qBound(0, s.digits, 12);
        // generateDouble() 是 53 位随机源；bounded<double> 只吃 32 位，
        // 在 1e9 以上的区间里会成段重复 —— 小数那一档要的是铺得开
        for (int i = 0; i < s.count; ++i)
            out->append(QString::number(a + (b - a) * rng->generateDouble(), 'f', digits));
        return true;
    }

    case Kind::String: {
        const QString pool = poolOf(s);
        if (pool.isEmpty()) {
            *err = QStringLiteral("一个字符集都没勾，生成不出来");
            return false;
        }
        if (s.length <= 0) {
            *err = QStringLiteral("长度至少 1");
            return false;
        }
        if (s.length > kMaxLength) {
            *err = QStringLiteral("一条最多 %1 个字符（现在要 %2）").arg(kMaxLength).arg(s.length);
            return false;
        }
        if (s.unique && s.length > pool.size()) {
            *err = QStringLiteral("字符池只有 %1 个不同字符（勾了“不重复”之后），造不出 %2 个字符长的")
                       .arg(pool.size()).arg(s.length);
            return false;
        }
        const int poolSize = static_cast<int>(pool.size());
        for (int i = 0; i < s.count; ++i) {
            if (!s.unique) {
                QString t;
                t.reserve(s.length);
                for (int k = 0; k < s.length; ++k)
                    t.append(pool.at(static_cast<int>(rng->bounded(static_cast<quint32>(poolSize)))));
                out->append(t);
                continue;
            }
            QVector<int> idx(poolSize);
            for (int k = 0; k < poolSize; ++k)
                idx[k] = k;
            for (int k = 0; k < s.length; ++k) {
                const int rest = poolSize - k;
                const int j = k + static_cast<int>(rng->bounded(static_cast<quint32>(rest)));
                std::swap(idx[k], idx[j]);
            }
            QString t;
            t.reserve(s.length);
            for (int k = 0; k < s.length; ++k)
                t.append(pool.at(idx[k]));
            out->append(t);
        }
        return true;
    }

    case Kind::Uuid: {
        for (int i = 0; i < s.count; ++i)
            out->append(QUuid::createUuid().toString(QUuid::WithoutBraces));
        return true;
    }
    }
    *err = QStringLiteral("认不出这一档");
    return false;
}

} // namespace Rand

RandomTool::RandomTool(QWidget* parent)
    : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    // 模式那一排：和 Base64 那两颗一样，当前那一档吃 objectName=primary 的高亮
    auto* kinds = new QHBoxLayout;
    kinds->setSpacing(6);
    kinds->addWidget(new QLabel(QStringLiteral("模式")));
    const Rand::Kind order[4] = { Rand::Kind::Int, Rand::Kind::Decimal, Rand::Kind::String, Rand::Kind::Uuid };
    for (int i = 0; i < 4; ++i) {
        auto* b = new QPushButton(Rand::kindName(order[i]));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [this, k = order[i]] {
            m_kind = k;
            applyKindUi();
            regenerate();
        });
        m_kindBtn[i] = b;
        kinds->addWidget(b);
    }
    kinds->addStretch();
    lay->addLayout(kinds);

    // 参数一律"一行一件事"，动作按钮排在行尾（他给这类页挑的形状）
    auto* rangeRow = new QHBoxLayout;
    rangeRow->setSpacing(6);
    m_loLabel = new QLabel(QStringLiteral("下界"));
    m_hiLabel = new QLabel(QStringLiteral("上界"));
    m_lo = new QLineEdit(QStringLiteral("1"));
    m_hi = new QLineEdit(QStringLiteral("100"));
    for (QLineEdit* e : { m_lo, m_hi }) {
        e->setPlaceholderText(QStringLiteral("整数"));
        e->setMaximumWidth(140);
    }
    rangeRow->addWidget(m_loLabel);
    rangeRow->addWidget(m_lo);
    rangeRow->addWidget(m_hiLabel);
    rangeRow->addWidget(m_hi);
    rangeRow->addStretch();
    m_rangeRow = new QWidget;
    m_rangeRow->setLayout(rangeRow);
    lay->addWidget(m_rangeRow);

    auto* countRow = new QHBoxLayout;
    countRow->setSpacing(6);
    countRow->addWidget(new QLabel(QStringLiteral("数量")));
    m_count = new QSpinBox;
    m_count->setRange(1, 10000);
    m_count->setValue(5);
    countRow->addWidget(m_count);
    countRow->addStretch();
    lay->addLayout(countRow);

    auto* digitsRow = new QHBoxLayout;
    digitsRow->setSpacing(6);
    digitsRow->addWidget(new QLabel(QStringLiteral("小数位")));
    m_digits = new QSpinBox;
    m_digits->setRange(0, 12);
    m_digits->setValue(2);
    digitsRow->addWidget(m_digits);
    digitsRow->addStretch();
    m_digitsRow = new QWidget;
    m_digitsRow->setLayout(digitsRow);
    lay->addWidget(m_digitsRow);

    auto* lengthRow = new QHBoxLayout;
    lengthRow->setSpacing(6);
    lengthRow->addWidget(new QLabel(QStringLiteral("长度")));
    m_length = new QSpinBox;
    m_length->setRange(1, 512);
    m_length->setValue(16);
    lengthRow->addWidget(m_length);
    lengthRow->addStretch();
    m_stringRow = new QWidget;   // 和下面那行字符集共用这一个开关
    m_stringRow->setLayout(lengthRow);
    lay->addWidget(m_stringRow);

    auto* charsetRow = new QHBoxLayout;
    charsetRow->setSpacing(10);
    for (const Rand::CharSet& cs : Rand::charSets()) {
        auto* cb = new QCheckBox(cs.label);
        cb->setChecked(cs.key != QLatin1String("s"));   // 默认不勾符号：拷出去粘贴到别处最容易被吃掉
        m_sets.append(cb);
        charsetRow->addWidget(cb);
    }
    m_lookalike = new QCheckBox(QStringLiteral("去掉易混的 0O1lI|"));
    m_lookalike->setChecked(true);
    charsetRow->addWidget(m_lookalike);
    charsetRow->addStretch();
    m_charsetRow = new QWidget;
    m_charsetRow->setLayout(charsetRow);
    lay->addWidget(m_charsetRow);

    auto* uniqRow = new QHBoxLayout;
    uniqRow->setSpacing(6);
    m_unique = new QCheckBox;
    m_uniqueLabel = new QLabel(QStringLiteral("（勾上=一次抽样里不许撞）"));
    uniqRow->addWidget(m_unique);
    uniqRow->addWidget(m_uniqueLabel);
    uniqRow->addStretch();
    lay->addLayout(uniqRow);

    auto* actRow = new QHBoxLayout;
    actRow->setSpacing(6);
    actRow->addStretch();
    auto* genBtn = new QPushButton(QStringLiteral("生成"));
    genBtn->setObjectName(QStringLiteral("primary"));
    m_copyBtn = new QPushButton(QStringLiteral("复制结果"));
    auto* clearBtn = new QPushButton(QStringLiteral("清空"));
    actRow->addWidget(genBtn);
    actRow->addWidget(m_copyBtn);
    actRow->addWidget(clearBtn);
    lay->addLayout(actRow);

    m_out = new QPlainTextEdit;
    m_out->setReadOnly(true);
    m_out->setLineWrapMode(QPlainTextEdit::NoWrap);
    lay->addWidget(m_out, 1);

    // 状态行常驻在所有动作行下面：没生成过 / 生成好了 / 参数不对，三种情况都得有话说
    m_status = new QLabel(QStringLiteral("还没生成过：按下「生成」"));
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setTextFormat(Qt::PlainText);
    lay->addWidget(m_status);

    connect(genBtn, &QPushButton::clicked, this, [this] { regenerate(); });
    connect(m_copyBtn, &QPushButton::clicked, this, [this] { copyResult(); });
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_out->clear();
        setStatus(QStringLiteral("已清空"), true);
    });
    for (QCheckBox* cb : m_sets)
        connect(cb, &QCheckBox::toggled, this, [this] { regenerate(); });
    connect(m_lookalike, &QCheckBox::toggled, this, [this] { regenerate(); });
    connect(m_unique, &QCheckBox::toggled, this, [this] { regenerate(); });
    connect(m_count, qOverload<int>(&QSpinBox::valueChanged), this, [this] { regenerate(); });
    connect(m_digits, qOverload<int>(&QSpinBox::valueChanged), this, [this] { regenerate(); });
    connect(m_length, qOverload<int>(&QSpinBox::valueChanged), this, [this] { regenerate(); });
    // 区间那两格是手打的：按回车或移开焦点才重算，别每敲一个字符就把结果区清空一次
    connect(m_lo, &QLineEdit::editingFinished, this, [this] { regenerate(); });
    connect(m_hi, &QLineEdit::editingFinished, this, [this] { regenerate(); });

    applyKindUi();
    regenerate();
}

Rand::Spec RandomTool::currentSpec() const
{
    Rand::Spec s;
    s.kind = m_kind;
    bool okLo = false, okHi = false;
    s.lo = m_lo->text().trimmed().isEmpty() ? 0 : m_lo->text().trimmed().toLongLong(&okLo);
    s.hi = m_hi->text().trimmed().isEmpty() ? 0 : m_hi->text().trimmed().toLongLong(&okHi);
    if (!okLo || !okHi) {
        s.lo = 0;
        s.hi = -1;      // 让它一定落到"上界比下界小"那句上，比新造一句"输入不是数字"少一份分支
    }
    s.count = m_count->value();
    s.digits = m_digits->value();
    s.length = m_length->value();
    s.unique = m_unique->isChecked();
    s.dropLookalike = m_lookalike->isChecked();
    for (const QCheckBox* cb : m_sets)
        if (cb->isChecked()) {
            const int i = m_sets.indexOf(const_cast<QCheckBox*>(cb));
            s.sets += Rand::charSets().at(i).key;
        }
    return s;
}

void RandomTool::applyKindUi()
{
    const bool range = m_kind == Rand::Kind::Int || m_kind == Rand::Kind::Decimal;
    m_rangeRow->setVisible(range);
    m_digitsRow->setVisible(m_kind == Rand::Kind::Decimal);
    m_stringRow->setVisible(m_kind == Rand::Kind::String);
    m_charsetRow->setVisible(m_kind == Rand::Kind::String);
    m_unique->setVisible(m_kind == Rand::Kind::Int || m_kind == Rand::Kind::String);
    m_uniqueLabel->setText(m_kind == Rand::Kind::String
                               ? QStringLiteral("（勾上=一条里字符不撞，池子得够长）")
                               : QStringLiteral("（勾上=一次抽样里不许撞）"));
    m_loLabel->setText(m_kind == Rand::Kind::Decimal ? QStringLiteral("下界（整数部分）")
                                                     : QStringLiteral("下界"));
    m_hiLabel->setText(m_kind == Rand::Kind::Decimal ? QStringLiteral("上界（整数部分）")
                                                     : QStringLiteral("上界"));
    const Rand::Kind order[4] = { Rand::Kind::Int, Rand::Kind::Decimal, Rand::Kind::String, Rand::Kind::Uuid };
    for (int i = 0; i < 4; ++i) {
        const bool on = order[i] == m_kind;
        m_kindBtn[i]->setChecked(on);
        m_kindBtn[i]->setObjectName(on ? QStringLiteral("primary") : QString());
        // setObjectName 之后不 polish 一下，高亮还留在上一颗按钮上（Base64 那颗踩过）
        style()->polish(m_kindBtn[i]);
    }
}

void RandomTool::regenerate()
{
    const Rand::Spec s = currentSpec();
    QStringList lines;
    QString err;
    if (!Rand::generate(s, &lines, &err)) {
        m_out->clear();            // 参数不对就一句不留：旧结果留在框里就是"看着像成功了"
        setStatus(QStringLiteral("✘ %1").arg(err), false);
        return;
    }
    m_out->setPlainText(lines.join(QLatin1Char('\n')));
    QString note = QStringLiteral("✔ %1 · %2 个").arg(Rand::kindName(s.kind)).arg(lines.size());
    if (s.kind == Rand::Kind::Int)
        note += QStringLiteral("（%1…%2%3）").arg(s.lo).arg(s.hi).arg(s.unique ? QStringLiteral("，不重复") : QString());
    else if (s.kind == Rand::Kind::Decimal)
        note += QStringLiteral("（%1…%2，%3 位小数）").arg(s.lo).arg(s.hi).arg(s.digits);
    else if (s.kind == Rand::Kind::String)
        note += QStringLiteral("（每串 %1 个字符，池子 %2%3）").arg(s.length)
                    .arg(Rand::poolOf(s).size())
                    .arg(s.unique ? QStringLiteral("，字符不重复") : QStringLiteral("，可重复"));
    setStatus(note, true);
}

void RandomTool::copyResult()
{
    const QString text = m_out->toPlainText();
    if (text.trimmed().isEmpty()) {
        setStatus(QStringLiteral("✘ 结果区是空的，没东西可复制"), false);
        return;
    }
    QApplication::clipboard()->setText(text);
    m_copyBtn->setText(QStringLiteral("已复制 ✓"));
    QTimer::singleShot(1200, m_copyBtn, [this] { m_copyBtn->setText(QStringLiteral("复制结果")); });
    setStatus(QStringLiteral("✔ 已把 %1 行复制到剪贴板").arg(text.count(QLatin1Char('\n')) + 1), true);
    Utils::log(QStringLiteral("randotool: 复制了 %1 个字符").arg(text.size()));
}

QString RandomTool::resultText() const
{
    return m_out->toPlainText();
}

QString RandomTool::statusText() const
{
    return m_status->text();
}

QString RandomTool::statusStyle() const
{
    return m_status->styleSheet();
}

bool RandomTool::feedText(const QString& text)
{
    // 直达这条路只能给出"要几个"：串里第一个整数当数量，别的（区间、字符集）不猜
    static const QRegularExpression re(QStringLiteral("(\\d{1,6})"));
    const auto m = re.match(text);
    if (!m.hasMatch())
        return false;
    m_count->setValue(m.captured(1).toInt());
    applyKindUi();
    regenerate();
    Utils::log(QStringLiteral("randotool: 收到 %1 个字符，数量按第一个整数取 %2")
                   .arg(text.size()).arg(m_count->value()));
    return true;
}

void RandomTool::setStatus(const QString& text, bool ok)
{
    m_statusText = text;
    m_statusOk = ok;
    m_status->setText(text);
    // 颜色从 Theme 那张表拿，不抄 Base64 里那两份字面量：这一页白天也要看得清。
    // 每次都从**带 @token 的那一份模板**展开 —— 挂在控件上的样式表里没有 `@okText` 这几个字了，
    // 拿它自己再 expand 一遍就是"读上一轮留下的状态"，切完模式颜色还停在夜里那支。
    m_status->setStyleSheet(Theme::expand(ok ? QStringLiteral("color:@okText;background:transparent;")
                                             : QStringLiteral("color:@bad;background:transparent;")));
}

void RandomTool::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::ThemeChange) {
        // 样式表挂在窗口自己身上，qApp 那份改了传导不下来；状态行那一支是每次现挂的，
        // 得按最后那句话重挂一次，否则切完白天它还挂着夜里那支绿（界面就不说实话了）。
        setStatus(m_statusText, m_statusOk);
    }
}

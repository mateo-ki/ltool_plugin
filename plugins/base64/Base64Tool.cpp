#include "Base64Tool.h"
#include "Utils.h"

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStringDecoder>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {


// 第一个非法字符的位置（-1 = 全部合法）
int firstBadChar(const QString& s)
{
    static const QString alpha = QStringLiteral(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=-_");
    for (int i = 0; i < s.size(); ++i)
        if (!alpha.contains(s.at(i)))
            return i;
    return -1;
}

QString utf8OrEmpty(const QByteArray& bytes, bool* ok)
{
    QStringDecoder dec(QStringConverter::Utf8);
    const QString text = dec.decode(bytes);
    if (dec.hasError()) {
        *ok = false;
        return QString();
    }
    // UTF-8 能解但全是控制字符（比如 gzip/图片）也没意义，交给十六进制
    for (const QChar& c : text) {
        if (c.unicode() < 0x20 && c != QChar('\n') && c != QChar('\r') && c != QChar('\t')) {
            *ok = false;
            return QString();
        }
    }
    *ok = true;
    return text;
}

QString hexDump(const QByteArray& bytes, int limitBytes, bool& truncated)
{
    truncated = bytes.size() > limitBytes;
    const int n = qMin(bytes.size(), limitBytes);
    QString out;
    for (int off = 0; off < n; off += 16) {
        QString hex, ascii;
        for (int i = off; i < qMin(off + 16, n); ++i) {
            const uchar b = static_cast<uchar>(bytes.at(i));
            hex += QString::asprintf("%02X ", b);
            ascii += (b >= 0x20 && b < 0x7F) ? QChar(b) : QChar('.');
        }
        out += QString("%1  %2  %3\n")
                   .arg(off, 8, 16, QLatin1Char('0'))
                   .arg(hex.leftJustified(48))
                   .arg(ascii);
    }
    if (truncated)
        out += QString("…（只显示前 %1 / %2 字节）\n").arg(n).arg(bytes.size());
    return out;
}

} // namespace

Base64Tool::Base64Tool(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(QStringLiteral("Base64 编解码"));
    resize(1080, 640);
    setAcceptDrops(true);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(8);

    auto* tools = new QHBoxLayout;
    tools->setSpacing(6);
    m_decodeBtn = new QPushButton(QStringLiteral("解码"), this);
    m_encodeBtn = new QPushButton(QStringLiteral("编码"), this);
    for (auto* b : {m_decodeBtn, m_encodeBtn}) {
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        tools->addWidget(b);
    }
    m_decodeBtn->setToolTip(QStringLiteral("把输入当成 base64 解成原始字节（标准与 URL-safe 字母表都收）"));
    m_encodeBtn->setToolTip(QStringLiteral("把输入按 UTF-8 编成 base64"));
    tools->addSpacing(12);
    auto* copyBtn = new QPushButton(QStringLiteral("复制结果"), this);
    copyBtn->setCursor(Qt::PointingHandCursor);
    tools->addWidget(copyBtn);
    auto* swapBtn = new QPushButton(QStringLiteral("结果搬到输入"), this);
    swapBtn->setToolTip(QStringLiteral("把输出区内容当成新的输入，方便来回验"));
    swapBtn->setCursor(Qt::PointingHandCursor);
    tools->addWidget(swapBtn);
    auto* pasteBtn = new QPushButton(QStringLiteral("粘贴输入"), this);
    pasteBtn->setCursor(Qt::PointingHandCursor);
    tools->addWidget(pasteBtn);
    auto* clearBtn = new QPushButton(QStringLiteral("清空"), this);
    clearBtn->setCursor(Qt::PointingHandCursor);
    tools->addWidget(clearBtn);
    tools->addStretch(1);
    lay->addLayout(tools);

    const QFont mono = [] {
        QFont f(QStringLiteral("Consolas"));
        f.setStyleHint(QFont::Monospace);
        f.setPointSizeF(9.6);
        return f;
    }();
    auto* split = new QSplitter(Qt::Vertical, this);
    m_input = new QPlainTextEdit(split);
    m_input->setPlaceholderText(QStringLiteral("粘贴 base64（换行、空格、引号都能混着来），或粘贴要编码的文本"));
    m_input->setFont(mono);
    m_out = new QPlainTextEdit(split);
    m_out->setReadOnly(true);
    m_out->setFont(mono);
    m_out->setPlaceholderText(QStringLiteral("结果"));
    split->addWidget(m_input);
    split->addWidget(m_out);
    split->setSizes({300, 300});
    lay->addWidget(split, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setTextFormat(Qt::PlainText);
    lay->addWidget(m_status);

    connect(m_decodeBtn, &QPushButton::clicked, this, [this] { setMode(true); });
    connect(m_encodeBtn, &QPushButton::clicked, this, [this] { setMode(false); });
    connect(copyBtn, &QPushButton::clicked, this, [this, copyBtn] {
        QApplication::clipboard()->setText(m_out->toPlainText());
        copyBtn->setText(QStringLiteral("已复制 ✓"));
        QTimer::singleShot(1200, copyBtn, [copyBtn] { copyBtn->setText(QStringLiteral("复制结果")); });
    });
    connect(swapBtn, &QPushButton::clicked, this, [this] {
        const QString t = m_out->toPlainText();
        if (t.trimmed().isEmpty())
            return;
        m_input->setPlainText(t);
        recompute();
    });
    connect(pasteBtn, &QPushButton::clicked, this, [this] {
        const QString t = QApplication::clipboard()->text();
        if (t.trimmed().isEmpty())
            return;
        m_input->setPlainText(t);
        recompute();
    });
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        m_input->clear();
        m_out->clear();
        setStatus(QStringLiteral("已清空"), true);
    });

    auto* debounce = new QTimer(this);
    debounce->setInterval(250);
    debounce->setSingleShot(true);
    connect(debounce, &QTimer::timeout, this, &Base64Tool::recompute);
    connect(m_input, &QPlainTextEdit::textChanged, debounce, [debounce] { debounce->start(); });

    setMode(true);
}

void Base64Tool::setText(const QString& text)
{
    m_input->setPlainText(text);
    recompute();
    Utils::log(QStringLiteral("base64tool: 收到 %1 个字符").arg(text.size()));
}

void Base64Tool::setMode(bool decode)
{
    applyModeUi(decode);
    recompute();
}

void Base64Tool::applyModeUi(bool decode)
{
    m_decode = decode;
    m_decodeBtn->setChecked(decode);
    m_encodeBtn->setChecked(!decode);
    m_decodeBtn->setObjectName(decode ? QStringLiteral("primary") : QString());
    m_encodeBtn->setObjectName(decode ? QString() : QStringLiteral("primary"));
    // setObjectName 之后要让样式重新生效，否则主按钮的高亮还留在旧的那个上
    style()->polish(m_decodeBtn);
    style()->polish(m_encodeBtn);
    setWindowTitle(decode ? QStringLiteral("Base64 解码") : QStringLiteral("Base64 编码"));
}

void Base64Tool::recompute()
{
    const QString raw = m_input->toPlainText();
    if (raw.trimmed().isEmpty()) {
        m_out->clear();
        setStatus(QStringLiteral("输入为空"), true);
        return;
    }

    if (!m_decode) {
        const QByteArray bytes = raw.toUtf8();
        m_out->setPlainText(QString::fromLatin1(bytes.toBase64().constData()));
        setStatus(QStringLiteral("✔ %1 字符 / %2 字节 → base64 %3 字符")
                      .arg(raw.size()).arg(bytes.size()).arg(m_out->toPlainText().size()), true);
        return;
    }

    QString s = raw;
    s.remove(QRegularExpression(QStringLiteral("[\\s\"']")));
    const bool urlSafe = s.contains(QLatin1Char('-')) || s.contains(QLatin1Char('_'));
    if (urlSafe)
        s.replace(QLatin1Char('-'), QLatin1Char('+')).replace(QLatin1Char('_'), QLatin1Char('/'));
    const int bad = firstBadChar(s);
    if (bad >= 0) {
        m_out->clear();
        setStatus(QStringLiteral("✘ 第 %1 个字符 “%2” 不在 base64 字母表里")
                      .arg(bad + 1).arg(s.at(bad)), false);
        return;
    }
    if (s.size() % 4 == 1) {
        m_out->clear();
        setStatus(QStringLiteral("✘ 去掉空白后是 %1 个字符，base64 长度不可能是 4n+1").arg(s.size()), false);
        return;
    }
    const QByteArray bytes = QByteArray::fromBase64(s.toLatin1());
    if (bytes.isEmpty()) {
        m_out->clear();
        setStatus(QStringLiteral("✘ 解出 0 字节（输入可能只是普通文本，试试“编码”）"), false);
        return;
    }
    bool textOk = false;
    const QString text = utf8OrEmpty(bytes, &textOk);
    if (textOk) {
        m_out->setPlainText(text);
    } else {
        bool trunc = false;
        m_out->setPlainText(hexDump(bytes, 4096, trunc));
    }
    QString note = textOk
        ? QStringLiteral("✔ 解出 %1 字节，按 UTF-8 显示").arg(bytes.size())
        : QStringLiteral("✔ 解出 %1 字节，不是合法 UTF-8 文本 → 显示十六进制").arg(bytes.size());
    if (urlSafe)
        note += QStringLiteral(" · URL-safe 字母表已转换");
    setStatus(note, true);
}

void Base64Tool::setStatus(const QString& text, bool ok)
{
    m_status->setText(text);
    m_status->setStyleSheet(QString("color:%1;background:transparent;")
                                .arg(ok ? QStringLiteral("#2ea058") : QStringLiteral("#e06c5f")));
}

void Base64Tool::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasText())
        e->acceptProposedAction();
}

void Base64Tool::dropEvent(QDropEvent* e)
{
    for (const QUrl& u : e->mimeData()->urls()) {
        const QString path = u.toLocalFile();
        if (path.isEmpty())
            continue;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            setStatus(QStringLiteral("✘ 打不开 %1").arg(QFileInfo(path).fileName()), false);
            return;
        }
        // 拖文件进来按"这是要编码的二进制"处理，比丢进输入区再猜更符合直觉
        const QByteArray bytes = f.readAll();
        applyModeUi(false);
        m_input->setPlainText(QFileInfo(path).fileName());
        const QString enc = QString::fromLatin1(bytes.toBase64().constData());
        m_out->setPlainText(enc);
        setStatus(QStringLiteral("✔ 已把 %1（%2 字节）编成 base64：%3 字符")
                      .arg(QFileInfo(path).fileName()).arg(bytes.size()).arg(enc.size()), true);
        e->acceptProposedAction();
        return;
    }
    const QString text = e->mimeData()->text();
    if (!text.trimmed().isEmpty()) {
        m_input->setPlainText(text);
        recompute();
        e->acceptProposedAction();
    }
}

#include "JsonTool.h"
#include "Utils.h"

#include <QApplication>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {


// 树里一个节点显示成 "键 = 值"；容器把子项数量也写出来，折叠时一眼看得出里面有多少东西
QString scalarText(const QJsonValue& v)
{
    switch (v.type()) {
    case QJsonValue::Bool:   return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double: return QString::number(v.toDouble(), 'g', 17);
    case QJsonValue::String: return v.toString();
    case QJsonValue::Null:   return QStringLiteral("null");
    default:                 return QString();
    }
}

int fillTree(QTreeWidgetItem* parent, const QJsonValue& v, int* nodes, int depth, int* maxDepth)
{
    ++*nodes;
    *maxDepth = qMax(*maxDepth, depth);
    if (v.isArray()) {
        const QJsonArray arr = v.toArray();
        parent->setText(1, QString("[%1 项]").arg(arr.size()));
        for (int i = 0; i < arr.size(); ++i) {
            auto* it = new QTreeWidgetItem(parent);
            it->setText(0, QString::number(i));
            fillTree(it, arr.at(i), nodes, depth + 1, maxDepth);
        }
        return arr.size();
    }
    if (v.isObject()) {
        const QJsonObject obj = v.toObject();
        parent->setText(1, QString("{%1 键}").arg(obj.size()));
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            auto* child = new QTreeWidgetItem(parent);
            child->setText(0, it.key());
            fillTree(child, it.value(), nodes, depth + 1, maxDepth);
        }
        return obj.size();
    }
    parent->setText(1, scalarText(v));
    if (v.isString())
        parent->setForeground(1, QBrush(QColor(150, 210, 160)));
    else if (v.isDouble() || v.isBool())
        parent->setForeground(1, QBrush(QColor(150, 190, 240)));
    return 0;
}

} // namespace

JsonTool::JsonTool(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(QStringLiteral("JSON 格式化"));
    resize(1180, 720);
    setAcceptDrops(true);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(8);

    auto* tools = new QHBoxLayout;
    tools->setSpacing(6);
    const auto mkBtn = [this, &tools](const QString& text, const QString& tip) {
        auto* b = new QPushButton(text, this);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        tools->addWidget(b);
        return b;
    };
    auto* fmtBtn = mkBtn(QStringLiteral("格式化"), QStringLiteral("重新解析并按 2 空格缩进写回文本区"));
    fmtBtn->setObjectName(QStringLiteral("primary"));
    auto* minBtn = mkBtn(QStringLiteral("压缩"), QStringLiteral("去掉缩进，压成一行"));
    auto* copyBtn = mkBtn(QStringLiteral("复制结果"), QStringLiteral("把文本区内容放进剪贴板"));
    auto* pasteBtn = mkBtn(QStringLiteral("粘贴输入"), QStringLiteral("用剪贴板内容替换文本区并解析"));
    auto* openBtn = mkBtn(QStringLiteral("打开文件…"), QStringLiteral("读入一个 .json 文件（也可以直接拖进来）"));
    tools->addSpacing(12);
    auto* expBtn = mkBtn(QStringLiteral("全部展开"), QStringLiteral("展开所有节点"));
    auto* colBtn = mkBtn(QStringLiteral("全部折叠"), QStringLiteral("折叠到第一层"));
    tools->addStretch(1);
    m_wrap = new QCheckBox(QStringLiteral("自动换行"), this);
    m_wrap->setChecked(true);
    tools->addWidget(m_wrap);
    lay->addLayout(tools);

    auto* split = new QSplitter(Qt::Horizontal, this);
    m_tree = new QTreeWidget(split);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({QStringLiteral("键"), QStringLiteral("值")});
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(false);
    m_tree->setWordWrap(true);
    m_tree->setColumnWidth(0, 240);
    m_edit = new QPlainTextEdit(split);
    m_edit->setPlaceholderText(QStringLiteral("把 JSON 粘贴到这里，或从托盘菜单/命令 json 打开后拖入 .json 文件"));
    m_edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(9.6);
    m_edit->setFont(mono);
    m_tree->setFont(mono);
    // 左边输入、右边看效果（他 2026-10-21 点名）。以前是反的：树在左、文本框在右，
    // 于是要先把光标挪到右边才能粘 JSON，左边那块先被解析结果占住。
    split->addWidget(m_edit);
    split->addWidget(m_tree);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setSizes({640, 560});
    lay->addWidget(split, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setTextFormat(Qt::PlainText);
    lay->addWidget(m_status);

    connect(fmtBtn, &QPushButton::clicked, this, [this] { reparse(true); });
    connect(minBtn, &QPushButton::clicked, this, [this] {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(m_edit->toPlainText().toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            setStatus(QStringLiteral("✘ %1（第 %2 行第 %3 列）")
                          .arg(err.errorString()).arg(err.offset + 1), false);
            return;
        }
        m_edit->setPlainText(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
        reparse(false);
    });
    connect(copyBtn, &QPushButton::clicked, this, [this, copyBtn] {
        QApplication::clipboard()->setText(m_edit->toPlainText());
        copyBtn->setText(QStringLiteral("已复制 ✓"));
        QTimer::singleShot(1200, copyBtn, [copyBtn] { copyBtn->setText(QStringLiteral("复制结果")); });
    });
    connect(pasteBtn, &QPushButton::clicked, this, [this] {
        const QString t = QApplication::clipboard()->text();
        if (t.trimmed().isEmpty())
            return;
        m_edit->setPlainText(t);
        reparse(true);
    });
    connect(openBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("打开 JSON 文件"), QString(),
            QStringLiteral("JSON / 文本 (*.json *.txt *);;所有文件 (*)"));
        if (!path.isEmpty())
            loadFile(path);
    });
    connect(expBtn, &QPushButton::clicked, this, [this] { expandAll(true); });
    connect(colBtn, &QPushButton::clicked, this, [this] { expandAll(false); });
    connect(m_wrap, &QCheckBox::toggled, this, [this](bool on) {
        m_edit->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        m_tree->setWordWrap(on);
    });

    // 输入时防抖解析：只刷新树和状态，不动文本（否则打字会被自己格式化回去）
    auto* debounce = new QTimer(this);
    debounce->setInterval(450);
    debounce->setSingleShot(true);
    connect(debounce, &QTimer::timeout, this, [this] { reparse(false); });
    connect(m_edit, &QPlainTextEdit::textChanged, debounce, [debounce] { debounce->start(); });
}

void JsonTool::setText(const QString& text)
{
    m_edit->setPlainText(text);
    reparse(true);
    Utils::log(QStringLiteral("jsontool: 收到 %1 个字符").arg(text.size()));
}

void JsonTool::reparse(bool rewriteText)
{
    const QString raw = m_edit->toPlainText();
    if (raw.trimmed().isEmpty()) {
        m_tree->clear();
        setStatus(QStringLiteral("文本区是空的"), true);
        return;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        // 行列自己数：QJsonParseError 只给 offset，而状态栏里给行列更好定位
        const QString head = raw.left(int(err.offset));
        const int line = head.count(QLatin1Char('\n')) + 1;
        const int col = int(err.offset) - (head.lastIndexOf(QLatin1Char('\n')) + 1) + 1;
        setStatus(QStringLiteral("✘ %1 · 第 %2 行第 %3 列（树里还是上一次的结果）")
                      .arg(err.errorString()).arg(line).arg(col), false);
        return;
    }
    if (rewriteText) {
        const QString pretty = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
        if (pretty != raw) {
            const QSignalBlocker b(m_edit);
            m_edit->setPlainText(pretty);
        }
    }
    buildTree(doc.isNull() ? QJsonValue() : (doc.isObject()
                                                 ? QJsonValue(doc.object())
                                                 : QJsonValue(doc.array())));
}

void JsonTool::buildTree(const QJsonValue& root)
{
    m_tree->clear();
    auto* top = new QTreeWidgetItem(m_tree);
    top->setText(0, QStringLiteral("(根)"));
    int nodes = 0, maxDepth = 0;
    const int kids = fillTree(top, root, &nodes, 0, &maxDepth);
    top->setExpanded(true);
    m_tree->expandToDepth(1);
    setStatus(QStringLiteral("✔ 合法 JSON · %1 · %2 个节点 · 最深 %3 层 · 文本 %4 字符")
                  .arg(root.isArray() ? QStringLiteral("数组 %1 项").arg(kids)
                                      : QStringLiteral("对象 %1 键").arg(kids))
                  .arg(nodes).arg(maxDepth + 1).arg(m_edit->toPlainText().size()), true);
}

void JsonTool::expandAll(bool on)
{
    if (!on) {
        m_tree->collapseAll();
        if (QTreeWidgetItem* top = m_tree->topLevelItem(0))
            top->setExpanded(true);
        return;
    }
    m_tree->expandAll();
}

void JsonTool::setStatus(const QString& text, bool ok)
{
    m_status->setText(text);
    m_status->setStyleSheet(QString("color:%1;background:transparent;")
                                .arg(ok ? QStringLiteral("#2ea058") : QStringLiteral("#e06c5f")));
}

void JsonTool::loadFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("✘ 打不开 %1").arg(QFileInfo(path).fileName()), false);
        return;
    }
    m_edit->setPlainText(QString::fromUtf8(f.readAll()));
    reparse(true);
    setWindowTitle(QStringLiteral("JSON 格式化 — %1").arg(QFileInfo(path).fileName()));
}

void JsonTool::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls() || e->mimeData()->hasText())
        e->acceptProposedAction();
}

void JsonTool::dropEvent(QDropEvent* e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    for (const QUrl& u : urls) {
        const QString local = u.toLocalFile();
        if (!local.isEmpty()) {
            loadFile(local);
            e->acceptProposedAction();
            return;
        }
    }
    const QString text = e->mimeData()->text();
    if (!text.trimmed().isEmpty()) {
        m_edit->setPlainText(text);
        reparse(true);
        e->acceptProposedAction();
    }
}

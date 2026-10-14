#include "DiskPage.h"
#include "Utils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFrame>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

#include <windows.h>
#include <shellapi.h>

namespace {

// 清单本体在 DiskScan::cleanTargets()（--disktime 诊断要用同一份），这里只留个短名
using CleanItem = DiskScan::CleanTarget;
QVector<CleanItem> cleanItems() { return DiskScan::cleanTargets(); }

// 必须带 Hidden|System：QDirIterator 默认不返回隐藏/系统条目，少了它们
// pagefile、回收站、系统目录里的东西整个漏掉（实测 C 盘少算约一半容量）
qint64 dirSize(const QString& path)
{
    qint64 total = 0;
    QDirIterator it(path, QDir::Files | QDir::NoSymLinks | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
    }
    return total;
}

QString human(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("不存在");
    if (bytes < 1024)
        return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QString("%1 MB").arg(bytes / 1024.0 / 1024, 0, 'f', 1);
    return QString("%1 GB").arg(bytes / 1024.0 / 1024 / 1024, 0, 'f', 2);
}

qint64 clearDir(const QString& path, QString* err)
{
    qint64 freed = 0;
    QDir d(path);
    if (!d.exists())
        return 0;
    const auto entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot
                                         | QDir::NoSymLinks | QDir::System | QDir::Hidden);
    for (const QFileInfo& fi : entries) {
        const qint64 sz = fi.isDir() ? dirSize(fi.absoluteFilePath()) : fi.size();
        bool ok = false;
        if (fi.isDir())
            ok = QDir(fi.absoluteFilePath()).removeRecursively();
        else
            ok = QFile::remove(fi.absoluteFilePath());
        if (ok)
            freed += sz;
        else if (err && err->size() < 200)
            *err += QString("  跳过 %1（占用中/无权限）\n").arg(fi.fileName());
    }
    return freed;
}

// 满宽占比条：轨道铺满可用宽度，填充按同级最大值归一
class TrackBar : public QFrame {
public:
    explicit TrackBar(QWidget* parent = nullptr) : QFrame(parent)
    {
        setFixedHeight(8);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setRatio(qreal r, bool hottest)
    {
        m_ratio = r;
        m_hottest = hottest;
        update();
    }

    QSize minimumSizeHint() const override { return QSize(24, 8); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#2b2d35"));
        p.drawRoundedRect(QRectF(0, 1, width(), 6), 3, 3);
        if (m_ratio <= 0.0)
            return;
        const qreal w = qMax<qreal>(5.0, qBound(0.004, m_ratio, 1.0) * width());
        p.setBrush(QColor(m_hottest ? "#5b85f0" : "#3f6ae0"));
        p.drawRoundedRect(QRectF(0, 1, w, 6), 3, 3);
    }

private:
    qreal m_ratio = 0.0;
    bool m_hottest = false;
};

// 右键某一行时该弹什么：只有「打开文件夹」这一项，要开的那个目录挂在这项的 data 上，
// 由行卡片那边点完再开；目录已经不在了就摆一条灰掉的说明，免得点了没反应还以为是界面坏了。
// 拆成独立函数是为了能在离屏自测里检查它（--rowmenu）——真按一下就会拉起资源管理器，
// 那个不能放进自测。
QMenu* makePathMenu(const QString& path, QWidget* parent)
{
    auto* menu = new QMenu(parent);
    QAction* open = menu->addAction(QStringLiteral("打开文件夹"));
    open->setData(path);
    if (!QFileInfo::exists(path)) {
        open->setEnabled(false);
        open->setText(QStringLiteral("这个目录已经不在了"));
    }
    return menu;
}

// 一行卡片：[勾选?] 名称 ——占比条—— 大小，第二行完整路径；给了 onClick 即可点击下钻
class RowCard : public QWidget {
public:
    RowCard(const QString& name, const QString& subPath, bool checkable,
            std::function<void()> onClick, QWidget* parent = nullptr)
        : QWidget(parent), m_onClick(std::move(onClick))
    {
        setObjectName("row");
        setAttribute(Qt::WA_StyledBackground);
        if (m_onClick)
            setCursor(Qt::PointingHandCursor);

        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(12, 8, 12, 8);
        col->setSpacing(3);

        auto* line = new QHBoxLayout;
        line->setSpacing(12);
        if (checkable) {
            check = new QCheckBox(this);
            check->setChecked(true);
            line->addWidget(check);
        }
        auto* nameLabel = new QLabel(name, this);
        nameLabel->setMinimumWidth(120);
        nameLabel->setMaximumWidth(230);
        line->addWidget(nameLabel);
        bar = new TrackBar(this);
        line->addWidget(bar, 1);
        sizeLabel = new QLabel(this);
        sizeLabel->setMinimumWidth(96);
        sizeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        line->addWidget(sizeLabel);
        col->addLayout(line);

        if (!subPath.isEmpty()) {
            auto* path = new QLabel(subPath, this);
            path->setObjectName("muted");
            QFont pf = path->font();
            pf.setPointSizeF(8.5);
            path->setFont(pf);
            col->addSpacing(2);
            col->addWidget(path);

            // 右键这一行（名字、下面那行路径、进度条都算）= 打开这个文件夹。
            // 左键在「占用分析」里是下钻，不能拿来开资源管理器，所以走右键。
            setContextMenuPolicy(Qt::CustomContextMenu);
            const QString target = subPath;
            connect(this, &QWidget::customContextMenuRequested, this, [this, target](const QPoint& pos) {
                QMenu* menu = makePathMenu(target, this);
                QAction* chosen = menu->exec(mapToGlobal(pos));
                delete menu;
                if (chosen && chosen->isEnabled())
                    QDesktopServices::openUrl(QUrl::fromLocalFile(chosen->data().toString()));
            });
        }
    }

    void setValues(qint64 bytes, qreal ratio, bool hottest)
    {
        sizeLabel->setText(human(bytes));
        bar->setRatio(ratio, hottest);
    }

    QCheckBox* check = nullptr;
    TrackBar* bar = nullptr;
    QLabel* sizeLabel = nullptr;

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && m_onClick) {
            // 下钻会销毁包括本行在内的所有行：绝不能在自身的 mousePressEvent 里同步执行，
            // 否则返回基类处理时 this 已析构。排到本次事件之后再跑。
            auto cb = m_onClick;
            QMetaObject::invokeMethod(this, [cb] { cb(); }, Qt::QueuedConnection);
            e->accept();
            return;
        }
        QWidget::mousePressEvent(e);
    }

private:
    std::function<void()> m_onClick;
};

QLabel* sectionTitle(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSizeF(10.5);
    f.setBold(true);
    l->setFont(f);
    return l;
}

// ---- WizTree 式矩形树图（squarified treemap）----

struct TCell {
    QRectF rect;
    int node = -2;        // -1 = 本目录直属文件，>=0 = 子目录节点
    qint64 bytes = 0;
    QString label;
    QColor color;
};

qreal worstRatio(const QVector<qreal>& row, qreal sum, qreal side)
{
    if (row.isEmpty() || sum <= 0.0 || side <= 0.0)
        return 1e18;
    qreal mx = 0.0, mn = 1e30;
    for (qreal v : row) {
        mx = qMax(mx, v);
        mn = qMin(mn, v);
    }
    const qreal s2 = sum * sum, w2 = side * side;
    return qMax(w2 * mx / s2, s2 / (w2 * mn));
}

} // namespace

class TreemapWidget : public QWidget {
public:
    explicit TreemapWidget(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMouseTracking(true);   // 不按下也要有悬停高亮/提示
        setMinimumSize(320, 220);
    }

    std::function<void(int)> onPick;

    void setData(const DiskScan::Tree* tree, int node)
    {
        m_tree = tree;
        m_node = node;
        m_hover = -2;
        relayout();
        update();
    }
    void clearData()
    {
        m_tree = nullptr;
        m_node = -1;
        m_cells.clear();
        update();
    }


protected:
    void resizeEvent(QResizeEvent*) override { relayout(); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x17, 0x18, 0x1c));
        if (m_cells.isEmpty()) {
            p.setPen(QColor(0x8b, 0x93, 0xa1));
            p.drawText(rect(), Qt::AlignCenter,
                       QStringLiteral("这里显示占用矩形图：面积 = 占用，点一块进入该目录"));
            return;
        }
        for (const TCell& c : m_cells) {
            p.fillRect(c.rect, c.color);
            p.setPen(QColor(0x17, 0x18, 0x1c));
            p.drawRect(c.rect.toAlignedRect());
        }
        for (const TCell& c : m_cells) {   // 文字后画，保证压在色块上
            if (c.rect.width() < 54 || c.rect.height() < 16)
                continue;
            QFont f = p.font();
            f.setPointSizeF(8.5);
            f.setBold(true);
            p.setFont(f);
            QFontMetrics fm(f);
            p.setPen(Qt::white);
            const QRect r = c.rect.toRect().adjusted(5, 3, -4, -2);
            p.drawText(r.topLeft() + QPoint(0, fm.ascent()),
                       fm.elidedText(c.label, Qt::ElideRight, r.width()));
            if (c.rect.height() >= 32) {
                QFont sf = p.font();
                sf.setBold(false);
                sf.setPointSizeF(8.0);
                p.setFont(sf);
                QFontMetrics sfm(sf);
                p.setPen(QColor(0xf0, 0xf0, 0xf0, 225));
                p.drawText(r.topLeft() + QPoint(0, fm.height() + sfm.ascent() - 2),
                           sfm.elidedText(human(c.bytes), Qt::ElideRight, r.width()));
            }
        }
        if (m_hover >= 0 && m_hover < m_cells.size()) {
            p.setPen(QPen(Qt::white, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(m_cells[m_hover].rect.toAlignedRect().adjusted(1, 1, -1, -1));
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        int hit = -1;
        for (int i = 0; i < m_cells.size(); ++i) {
            if (m_cells[i].rect.contains(QPointF(e->pos()))) {
                hit = i;
                break;
            }
        }
        if (hit != m_hover) {
            m_hover = hit;
            update();
        }
        if (hit >= 0 && m_tree && m_node >= 0) {
            const TCell& c = m_cells[hit];
            const qint64 total = m_tree->size[m_node] > 0 ? m_tree->size[m_node] : 1;
            const QString what = c.node < 0 ? QStringLiteral("本目录文件") : m_tree->name[c.node];
            QToolTip::showText(e->globalPosition().toPoint(),
                               QString("%1\n%2（%3%）\n%4")
                                   .arg(what, human(c.bytes),
                                        QString::number(c.bytes * 100.0 / total, 'f', 1),
                                        c.node >= 0 ? QStringLiteral("点击进入该目录")
                                                    : QString()));
        } else {
            QToolTip::hideText();
        }
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !m_tree)
            return;
        for (const TCell& c : m_cells) {
            if (c.node >= 0 && c.rect.contains(QPointF(e->pos())) && onPick) {
                auto cb = onPick;
                const int node = c.node;
                QMetaObject::invokeMethod(this, [cb, node] { cb(node); }, Qt::QueuedConnection);
                return;
            }
        }
    }

    void leaveEvent(QEvent*) override
    {
        m_hover = -2;
        QToolTip::hideText();
        update();
    }

private:
    void relayout()
    {
        m_cells.clear();
        if (!m_tree || m_node < 0 || width() < 8 || height() < 8)
            return;
        const DiskScan::Tree& t = *m_tree;
        const qint64 total = t.size[m_node];
        if (total <= 0)
            return;

        QVector<int> nodes;
        QVector<qreal> areas;
        const qreal full = qreal(width()) * qreal(height());
        const auto push = [&](int node, qint64 bytes) {
            if (bytes <= 0)
                return;
            nodes.append(node);
            areas.append(qreal(bytes) / qreal(total) * full);
        };
        push(-1, t.fileBytes[m_node]);
        for (int i = t.firstKid[m_node]; i < t.firstKid[m_node] + t.kidCount[m_node]; ++i)
            push(t.kids[i], t.size[t.kids[i]]);
        if (nodes.isEmpty())
            return;

        QRectF rest(rect());
        int i = 0;
        while (i < areas.size() && rest.width() > 2.0 && rest.height() > 2.0) {
            const bool wide = rest.width() >= rest.height();   // 条带沿短边铺开
            const qreal side = wide ? rest.height() : rest.width();
            QVector<qreal> row;
            qreal sum = 0.0, worst = 1e18;
            int j = i;
            while (j < areas.size()) {
                QVector<qreal> trial = row;
                trial.append(areas[j]);
                const qreal w = worstRatio(trial, sum + areas[j], side);
                if (!row.isEmpty() && w > worst)
                    break;
                row = trial;
                sum += areas[j];
                worst = w;
                ++j;
            }
            const qreal thick = sum / side;
            qreal off = 0.0;
            for (int k = 0; k < row.size(); ++k) {
                const qreal span = row[k] / thick;
                TCell c;
                c.rect = wide ? QRectF(rest.x(), rest.y() + off, thick, span)
                              : QRectF(rest.x() + off, rest.y(), span, thick);
                off += span;
                const int node = nodes[i + k];
                c.node = node;
                c.bytes = node < 0 ? t.fileBytes[m_node] : t.size[node];
                c.label = node < 0 ? QStringLiteral("本目录文件") : t.name[node];
                const uint hue = node < 0 ? 210u : uint((node * 47u) % 360u);
                c.color = QColor::fromHsv(int(hue), 145,
                                          node < 0 ? 95 : 140 + int(node % 5) * 14);
                m_cells.append(c);
            }
            // 条带沿短边铺开，所以被吃掉的是"长边方向"的那一段厚度
            rest = wide ? QRectF(rest.x() + thick, rest.y(), rest.width() - thick, rest.height())
                        : QRectF(rest.x(), rest.y() + thick, rest.width(), rest.height() - thick);
            i = j;
        }
    }

    const DiskScan::Tree* m_tree = nullptr;
    int m_node = -1;
    int m_hover = -2;
    QVector<TCell> m_cells;
};



// 逻辑磁盘下拉：只列"就绪"的盘（U 盘拔了就不该还能选），标签带上文件系统和剩余空间，
// 免得要点开才知道哪个是数据盘。扫描是重 I/O，所以换盘符只清结果、不自动开扫。
void DiskPage::refreshDrives()
{
    const QString keep = m_drive ? m_drive->currentData().toString() : m_root;
    const QSignalBlocker blocker(m_drive);
    m_drive->clear();
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        const QString letter = QString(QChar(L'A' + i)) + QLatin1Char(':');
        const QString rootNative = letter + QLatin1String("\\");
        const UINT type = GetDriveTypeW(reinterpret_cast<LPCWSTR>(rootNative.utf16()));
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
            continue;
        ULARGE_INTEGER freeAvail{}, total{}, freeTotal{};
        QString tail;
        if (GetDiskFreeSpaceExW(reinterpret_cast<LPCWSTR>(rootNative.utf16()), &freeAvail, &total, &freeTotal)
            && total.QuadPart) {
            wchar_t fs[64] = {};
            DWORD serial = 0, flags = 0, nameLen = 64;
            GetVolumeInformationW(reinterpret_cast<LPCWSTR>(rootNative.utf16()), nullptr, 0, &serial,
                                  &nameLen, &flags, fs, 64);
            tail = QString("  %1 · 剩余 %2 / %3")
                       .arg(QString::fromWCharArray(fs).isEmpty() ? QStringLiteral("?")
                                                                  : QString::fromWCharArray(fs),
                            human(qint64(freeTotal.QuadPart)), human(qint64(total.QuadPart)));
        } else {
            tail = QStringLiteral("  未就绪");
        }
        m_drive->addItem(letter + QLatin1Char('\\') + tail, letter + QLatin1Char('/'));
    }
    int idx = m_drive->findData(keep);
    if (idx < 0)
        idx = m_drive->findData(QStringLiteral("C:/"));
    if (idx < 0 && m_drive->count())
        idx = 0;
    if (idx >= 0) {
        m_drive->setCurrentIndex(idx);
        m_root = m_drive->itemData(idx).toString();
    }
}

DiskPage::DiskPage(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 10, 14, 10);
    outer->setSpacing(10);

    // ---- 汇总卡 ----
    auto* summary = new QWidget(this);
    summary->setObjectName("row");
    summary->setAttribute(Qt::WA_StyledBackground);
    auto* grid = new QGridLayout(summary);
    grid->setContentsMargins(16, 12, 16, 12);
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(2);
    m_cleanTotal = new QLabel("点「整理」开始", summary);
    m_diskUsed = new QLabel(summary);
    m_diskFree = new QLabel(summary);
    for (QLabel* v : {m_cleanTotal, m_diskUsed, m_diskFree}) {
        QFont f = v->font();
        f.setPointSizeF(12.5);
        f.setBold(true);
        v->setFont(f);
        v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    const char* titles[] = {"可清理合计", "已用空间", "剩余空间"};
    for (int i = 0; i < 3; ++i) {
        auto* t = new QLabel(titles[i], summary);
        t->setObjectName("muted");
        QFont tf = t->font();
        tf.setPointSizeF(8.5);
        t->setFont(tf);
        grid->addWidget(t, 0, i);
        if (i == 1)
            m_usedTitle = t;      // 换盘符时这里要跟着写是哪个盘
    }
    grid->addWidget(m_cleanTotal, 1, 0);
    grid->addWidget(m_diskUsed, 1, 1);
    grid->addWidget(m_diskFree, 1, 2);
    grid->setColumnStretch(0, 3);
    grid->setColumnStretch(1, 2);
    grid->setColumnStretch(2, 2);

    // ---- 操作条 ----
    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto* driveLabel = new QLabel("磁盘", this);
    driveLabel->setObjectName("muted");
    m_drive = new QComboBox(this);
    m_drive->setAccessibleName("driveBox");
    m_drive->setMinimumWidth(230);
    m_drive->setToolTip("选择要分析哪个盘的使用情况；「可清理位置」是系统和用户目录的缓存，"
                        "与这里选的盘无关");
    m_analyzeBtn = new QPushButton("整理", this);
    m_analyzeBtn->setObjectName("primary");
    m_analyzeBtn->setAccessibleName("analyzeBtn");
    m_analyzeBtn->setToolTip("扫描常见可清理位置的占用，并分析所选磁盘各目录占用");
    m_cleanBtn = new QPushButton("清理选中项", this);
    m_cleanBtn->setEnabled(false);   // 扫出结果前不给清理，避免空列表也去清空回收站
    m_status = new QLabel(this);
    m_status->setObjectName("muted");
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    actions->addWidget(driveLabel);
    actions->addWidget(m_drive);
    actions->addWidget(m_analyzeBtn);
    actions->addWidget(m_cleanBtn);
    actions->addWidget(m_status, 1);

    // ---- 两个 tab ----
    m_tabs = new QTabBar(this);
    m_tabs->addTab("可清理位置");
    m_tabs->addTab("占用分析");
    m_tabs->setExpanding(false);
    m_stack = new QStackedWidget(this);

    auto* cleanPage = new QWidget(m_stack);
    auto* cleanLay = new QVBoxLayout(cleanPage);
    cleanLay->setContentsMargins(0, 6, 0, 0);
    cleanLay->setSpacing(6);
    m_rowsLay = new QVBoxLayout;
    m_rowsLay->setSpacing(6);
    m_rowsLay->addStretch(1);
    cleanLay->addLayout(m_rowsLay, 1);

    auto* anaPage = new QWidget(m_stack);
    auto* anaLay = new QVBoxLayout(anaPage);
    anaLay->setContentsMargins(0, 6, 0, 0);
    anaLay->setSpacing(8);
    auto* dirHead = new QHBoxLayout;
    dirHead->setSpacing(10);
    m_upBtn = new QPushButton("‹ 上一层", anaPage);
    m_upBtn->setEnabled(false);
    m_upBtn->setToolTip("返回上一级目录（已在根目录时不可用）");
    m_analysisPath = new QLabel("未开始分析", anaPage);
    m_analysisPath->setObjectName("muted");
    m_analysisPath->setAccessibleName("analysisPath");
    m_analysisPath->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_analysisStatus = new QLabel(anaPage);
    m_analysisStatus->setObjectName("muted");
    m_analysisStatus->setAccessibleName("analysisStatus");
    m_analysisStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    dirHead->addWidget(m_upBtn);
    dirHead->addWidget(m_analysisPath, 1);
    dirHead->addWidget(m_analysisStatus);
    anaLay->addLayout(dirHead);

    m_map = new TreemapWidget(anaPage);
    m_map->onPick = [this](int node) { showNode(node); };
    anaLay->addWidget(m_map, 3);

    auto* listHead = new QHBoxLayout;
    listHead->addWidget(sectionTitle("本层目录", anaPage));
    listHead->addStretch(1);
    anaLay->addLayout(listHead);
    auto* listScroll = new QScrollArea(anaPage);
    listScroll->setWidgetResizable(true);
    listScroll->setFrameShape(QFrame::NoFrame);
    auto* listHost = new QWidget(listScroll);
    listHost->setObjectName("rowsHost");
    listHost->setAccessibleName("analysisRows");
    m_analysisLay = new QVBoxLayout(listHost);
    m_analysisLay->setContentsMargins(0, 0, 0, 0);
    m_analysisLay->setSpacing(6);
    m_analysisLay->addStretch(1);
    listScroll->setWidget(listHost);
    anaLay->addWidget(listScroll, 2);

    m_stack->addWidget(cleanPage);
    m_stack->addWidget(anaPage);
    connect(m_tabs, &QTabBar::currentChanged, m_stack, &QStackedWidget::setCurrentIndex);

    outer->addWidget(summary);
    outer->addLayout(actions);
    outer->addWidget(m_tabs);
    outer->addWidget(m_stack, 1);

    connect(m_analyzeBtn, &QPushButton::clicked, this, &DiskPage::startAll);
    connect(m_cleanBtn, &QPushButton::clicked, this, &DiskPage::cleanSelected);
    connect(m_upBtn, &QPushButton::clicked, this, &DiskPage::analysisUp);

    m_timer = new QTimer(this);
    m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, &DiskPage::tick);

    refreshDrives();
    connect(m_drive, &QComboBox::currentIndexChanged, this, [this](int) {
        const QString next = m_drive->currentData().toString();
        if (next.isEmpty() || next == m_root)
            return;
        m_root = next;
        updateSummary();
        // 换盘 = 换分析对象：先看有没有扫过的缓存（扫过就直接摆出来，不再等十几秒），
        // 没有就把界面清空等「整理」。旧树属于上一个盘，留着最容易被当成结论。
        if (!m_task)
            showCached(next);
        if (!m_tree) {
            m_cleanPhase = 0;
            m_cleanTotal->setText("点「整理」开始");
            m_analysisPath->setText(QString("已切到 %1，点「整理」重新扫描（扫描是重 I/O，不自动开）")
                                        .arg(m_root.left(2).toUpper()));
            m_analysisStatus->setText(QString());
        }
        m_status->setText(QString("已切到 %1：占用分析用这个盘的缓存（没有就点「整理」）；"
                                  "可清理位置仍是系统/用户目录，不受换盘影响。")
                              .arg(m_root.left(2).toUpper()));
    });

    updateSummary();
    m_status->setText("点「整理」扫描可清理位置并分析目录占用；矩形图里点一块即可进入该目录。");
}

void DiskPage::updateSummary()
{
    const QString native = QDir::toNativeSeparators(m_root);   // "C:/" -> "C:\"
    m_usedTitle->setText(QString("%1 盘已用").arg(m_root.left(1).toUpper()));
    ULARGE_INTEGER freeAvail{}, total{}, freeTotal{};
    if (!GetDiskFreeSpaceExW(reinterpret_cast<LPCWSTR>(native.utf16()), &freeAvail, &total, &freeTotal)) {
        m_diskUsed->setText("读取失败");
        m_diskFree->setText("-");
        return;
    }
    const qint64 bytesTotal = qint64(total.QuadPart);
    const qint64 bytesFree = qint64(freeTotal.QuadPart);
    m_diskUsed->setText(QString("%1 / %2").arg(human(bytesTotal - bytesFree),
                                               human(bytesTotal)));
    m_diskFree->setText(human(bytesFree));
}

void DiskPage::clearRows()
{
    while (m_rowsLay->count() > 1) {
        QLayoutItem* item = m_rowsLay->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    m_rows.clear();
}

void DiskPage::startAll()
{
    if (m_timer->isActive())
        return;   // 目录树还在扫，别重复起线程
    m_task = std::make_shared<Task>();
    // 「整理」就是明确要求重扫：把这个盘的缓存拿掉，扫完再放回去。
    // 留着的话 tick() 收尾时那个 `if (m_tree)` 一短路，界面会一直显示旧数 —— 看着像"扫好了"
    m_byDrive.remove(m_root);
    m_scannedAt.remove(m_root);
    m_tree.reset();
    m_cleanPhase = 0;
    m_analyzeBtn->setEnabled(false);
    m_drive->setEnabled(false);   // 扫描中途换盘会让树上的数字和界面说的盘对不上
    m_upBtn->setEnabled(false);
    m_analysisPath->setText(QString("正在统计可清理位置…（整个 %1 的占用分析要再等十几秒）")
                                .arg(m_root.left(2).toUpper()));
    m_status->setText("正在统计可清理位置…");
    m_cleanTotal->setText("扫描中…");
    m_analysisStatus->setText("已统计 0 B / 0 个目录");
    m_map->clearData();
    for (QWidget* w : m_analysisRows)
        delete w;
    m_analysisRows.clear();

    const QVector<CleanItem> items = cleanItems();
    QStringList paths;
    for (const CleanItem& c : items)
        paths << c.path;

    const QString root = m_root;      // 线程里用副本，别在后台读界面状态
    auto task = m_task;
    std::thread([task, paths, root] {
        // 第一段：只查那 9 个可清理位置。它们都是小目录，一秒级就能把第一个 tab 填满，
        // 用户点「整理」后不再是对着空白等十几秒。
        task->cleanSizes = DiskScan::sizesFor(paths);
        task->cleanDone.store(true, std::memory_order_release);
        // 第二段：整棵目录树（占用分析 + 可清理位置的定稿数据）
        auto tree = std::make_shared<DiskScan::Tree>(
            DiskScan::run(root, 1 << 20, [task](const DiskScan::Progress& p) {
                task->bytes.store(p.bytes, std::memory_order_relaxed);
                task->dirs.store(p.dirs, std::memory_order_relaxed);
                const std::lock_guard<std::mutex> g(task->m);
                task->current = p.current;
                task->branches = p.doneBranches;
                task->branchTotal = p.branches;
            }));
        task->tree = tree;
        task->done.store(true, std::memory_order_release);
    }).detach();
    m_timer->start();
}

// 轮询后台线程：进度用原子量、结果用 shared_ptr，回调不跨线程碰界面
void DiskPage::tick()
{
    if (!m_task) {
        m_timer->stop();
        return;
    }
    if (m_task->cleanDone.load(std::memory_order_acquire) && m_cleanPhase == 0) {
        m_cleanPhase = 1;
        renderClean(m_task->cleanSizes);
        m_analysisPath->setText(QString("可清理位置已出，正在遍历 %1 目录树做占用分析…")
                                    .arg(m_root.left(2).toUpper()));
    }
    if (!m_task->done.load(std::memory_order_acquire)) {
        const qint64 bytes = m_task->bytes.load(std::memory_order_relaxed);
        const qint64 dirs = m_task->dirs.load(std::memory_order_relaxed);
        QString cur;
        int doneBranches = 0;
        int totalBranches = 0;
        {
            const std::lock_guard<std::mutex> g(m_task->m);
            cur = m_task->current;
            doneBranches = m_task->branches.size();
            totalBranches = m_task->branchTotal;
        }
        const QString text = QString("已统计 %1 / %2 个目录 · 一级分支已扫完 %3 / %4%5")
                                 .arg(human(bytes))
                                 .arg(dirs)
                                 .arg(doneBranches)
                                 .arg(totalBranches)
                                 .arg(cur.isEmpty() ? QString() : QString(" · 正在 %1").arg(cur));
        m_analysisStatus->setText(text);
        renderBranches();       // 已经定稿的一级分支先摆出来（整盘要十几秒，别干等）
        if (m_cleanPhase < 2)
            m_status->setText(text);
        return;
    }
    m_timer->stop();
    m_tree = m_task->tree;
    m_task.reset();
    if (m_tree && !m_tree->isEmpty()) {
        m_byDrive.insert(m_root, m_tree);          // 换回这个盘就不用再扫一遍
        m_scannedAt.insert(m_root, QDateTime::currentDateTime());
    }
    m_analyzeBtn->setEnabled(true);
    m_drive->setEnabled(true);
    if (!m_tree || m_tree->isEmpty()) {
        m_analysisPath->setText("遍历失败（权限或路径问题）");
        m_status->setText(m_cleanPhase == 1
                              ? QStringLiteral("可清理位置已列出；目录树遍历失败（权限或路径问题）")
                              : QStringLiteral("遍历失败（权限或路径问题）"));
        return;
    }
    m_node = m_tree->root;
    renderAnalysis();
    m_cleanPhase = 2;          // 先定阶段，最后这次渲染才不带"数字可能还会变"
    renderCleanFromTree();     // 用整棵树的数据定稿（比快速通道口径一致）
}

// 扫描途中把"已经整体扫完的一级分支"先摆出来。这些是定稿值 —— DiskScan 只在
// 那个分支名下再没有任务在飞时才报（见 Counters::finishJob），所以先看到的数
// 不会因为后面又扫出东西而变大。整盘要十几秒，小分支几百毫秒就出得来。
void DiskPage::renderBranches()
{
    if (!m_task || m_tree)
        return;
    QVector<QPair<QString, qint64>> done;
    {
        const std::lock_guard<std::mutex> g(m_task->m);
        done = m_task->branches;
    }
    if (done.size() == m_analysisRows.size())
        return;                       // 一条没多，别重画（每 250ms 一次，重画也浪费）
    std::sort(done.begin(), done.end(),
              [](const QPair<QString, qint64>& a, const QPair<QString, qint64>& b) {
                  return a.second > b.second;
              });
    for (QWidget* w : m_analysisRows)
        delete w;
    m_analysisRows.clear();
    qint64 maxSz = 1;
    for (const auto& pr : done)
        maxSz = qMax(maxSz, pr.second);
    for (const auto& pr : done) {
        // 树还没建完，点不动（下钻要查表）—— 所以不传 onClick，光标也不变手型
        auto* row = new RowCard(pr.first, m_root + pr.first, false, nullptr, this);
        row->setValues(pr.second, qreal(pr.second) / maxSz, pr.second == maxSz);
        m_analysisLay->insertWidget(m_analysisLay->count() - 1, row);
        m_analysisRows.append(row);
    }
}

// 这个盘扫过就直接用缓存：换回来看一眼不该再等十几秒。强制重扫是「整理」那颗按钮。
void DiskPage::showCached(const QString& root)
{
    m_tree = m_byDrive.value(root);
    if (!m_tree || m_tree->isEmpty()) {
        m_tree.reset();
        m_node = -1;
        m_cleanPhase = 0;
        m_map->clearData();
        for (QWidget* w : m_analysisRows)
            delete w;
        m_analysisRows.clear();
        m_upBtn->setEnabled(false);
        return;
    }
    m_node = m_tree->root;
    m_cleanPhase = 2;              // 缓存里就带着整棵树，可清理位置的数直接查它
    m_upBtn->setEnabled(false);
    renderAnalysis();
    renderCleanFromTree();
    m_analysisPath->setText(QString("%1 · 上次扫描 %2（点「整理」重扫）")
                                .arg(m_tree->pathOf(m_tree->root),
                                     m_scannedAt.value(root).toString("MM-dd hh:mm:ss")));
}

void DiskPage::showNode(int node)
{
    if (!m_tree || node < 0 || node >= m_tree->name.size())
        return;
    m_node = node;
    renderAnalysis();
}

void DiskPage::analysisUp()
{
    if (!m_tree || m_node < 0 || m_node == m_tree->root)
        return;
    showNode(m_tree->parent[m_node]);
}

void DiskPage::renderAnalysis()
{
    if (!m_tree || m_node < 0)
        return;
    const DiskScan::Tree& t = *m_tree;
    m_upBtn->setEnabled(m_node != t.root);
    m_analysisPath->setText(t.pathOf(m_node));
    m_map->setData(&t, m_node);

    for (QWidget* w : m_analysisRows)
        delete w;
    m_analysisRows.clear();

    const int count = t.kidCount[m_node];
    qint64 maxSz = 1;
    for (int i = 0; i < count; ++i)
        maxSz = qMax(maxSz, t.size[t.kids[t.firstKid[m_node] + i]]);
    const int shown = qMin(count, 80);
    for (int i = 0; i < shown; ++i) {
        const int kid = t.kids[t.firstKid[m_node] + i];
        const qint64 bytes = t.size[kid];
        auto* row = new RowCard(t.name[kid], t.pathOf(kid), false,
                                [this, kid] { showNode(kid); }, this);
        row->setValues(bytes, bytes > 0 ? qreal(bytes) / maxSz : 0.0, bytes == maxSz);
        m_analysisLay->insertWidget(m_analysisLay->count() - 1, row);
        m_analysisRows.append(row);
    }
    QString status = QString("本层 %1 个子目录，合计 %2").arg(count).arg(human(t.size[m_node]));
    if (count > shown)
        status += QString("（列表只显示前 %1，矩形图里全都有）").arg(shown);
    if (m_node == t.root && t.pruned > 0)
        status += QString("，另有 %1 个小于 1 MB 的目录未列出").arg(t.pruned);
    if (m_node == t.root && m_scannedAt.contains(m_root))
        status += QString(" · 上次扫描 %1").arg(m_scannedAt.value(m_root).toString("hh:mm:ss"));
    m_analysisStatus->setText(status);
}

void DiskPage::renderCleanFromTree()
{
    if (!m_tree)
        return;
    const QVector<CleanItem> items = cleanItems();
    QVector<qint64> sizes;
    for (int i = 0; i < items.size(); ++i) {
        const int node = m_tree->nodeForPath(items[i].path);
        if (node >= 0) {
            sizes << m_tree->size[node];
        } else if (i < m_cleanSizes.size()) {
            // 选了非系统盘时树上根本没有这些路径（可清理位置都在系统盘），
            // 这时保留快速通道量到的数，别把它们抹成 0
            sizes << m_cleanSizes[i];
        } else {
            sizes << (QDir(items[i].path).exists() ? qint64(0) : qint64(-1));
        }
    }
    renderClean(sizes);
}

// 可清理位置的列表。sizes 与 cleanItems() 同序；负数=目录不存在。
// 快速通道（只查这几个目录）和定稿（查整棵目录树）都走这里，界面只有一套画法。
void DiskPage::renderClean(const QVector<qint64>& sizes)
{
    m_cleanSizes = sizes;       // 换盘符后树上查不到时靠它兜底
    clearRows();
    m_cleanBtn->setEnabled(false);

    const QVector<CleanItem> items = cleanItems();
    qint64 maxSz = 1;
    for (int i = 0; i < items.size() && i < sizes.size(); ++i) {
        const qint64 s = sizes[i];
        if (!QDir(items[i].path).exists())
            continue;
        maxSz = qMax(maxSz, s);
    }
    for (int i = 0; i < items.size() && i < sizes.size(); ++i) {
        const CleanItem& c = items[i];
        const bool exists = QDir(c.path).exists();
        const qint64 bytes = exists ? qMax(qint64(0), sizes[i]) : qint64(-1);
        CleanRow cr;
        cr.path = QDir::toNativeSeparators(c.path);
        cr.admin = c.admin;
        const qreal ratio = bytes > 0 ? qreal(bytes) / maxSz : 0.0;
        auto* row = new RowCard(QString::fromUtf8(c.name), cr.path, true, nullptr, this);
        row->setValues(bytes, ratio, bytes > 0 && bytes == maxSz);
        if (cr.admin)
            row->sizeLabel->setToolTip("清理该项需要管理员权限");
        cr.box = row->check;
        cr.bar = row->bar;
        cr.size = row->sizeLabel;
        m_rowsLay->insertWidget(m_rowsLay->count() - 1, row);
        m_rows.append(cr);
    }
    qint64 total = 0;
    for (int i = 0; i < items.size() && i < sizes.size(); ++i)
        if (sizes[i] > 0)
            total += sizes[i];
    m_cleanTotal->setText(human(total));
    m_cleanBtn->setEnabled(true);
    m_status->setText(QString("可清理位置合计 %1（清理前请确认相关软件已关闭）%2")
                          .arg(human(total),
                               m_cleanPhase < 2
                                   ? QStringLiteral(" · 目录树还在后台扫，数字可能还会变")
                                   : QString()));
}

void DiskPage::cleanSelected()
{
    if (QMessageBox::question(this, "ltool", "确定清理勾选的项目吗？此操作不可恢复。")
        != QMessageBox::Yes)
        return;
    qint64 freed = 0;
    QString skipped;
    for (const CleanRow& cr : m_rows) {
        if (!cr.box->isChecked())
            continue;
        QString err;
        freed += clearDir(cr.path, &err);
        if (!err.isEmpty())
            skipped += err;
    }
    SHEmptyRecycleBinW(nullptr, L"C:\\",
                       SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    QMessageBox::information(this, "ltool",
        QString("已释放 %1 磁盘空间。%2").arg(human(freed),
              skipped.isEmpty() ? QString() : "\n部分文件被跳过：\n" + skipped));
    updateSummary();
    startAll();   // 容量变了，重扫一次目录树，两个 tab 一起刷新
}

// 磁盘页那两栏列表的右键（「可清理位置」和「占用分析 · 本层目录」共用同一行卡片）。
// 这条链只有界面在用，而且真按一下就会拉起资源管理器——所以自测里不真按、也不真开，
// 验的是菜单里摆了哪些项、那一项灰没灰、以及"能点的项"后面到底接没接线。
int diskRowMenuSelftest()
{
    int bad = 0;
    const auto check = [&bad](const char* name, const QString& got, const QString& want) {
        const bool ok = got == want;
        if (!ok)
            ++bad;
        std::printf("%s %s got=[%s] want=[%s]\n", ok ? "PASS" : "FAIL", name,
                    got.toUtf8().constData(), want.toUtf8().constData());
    };
    QTemporaryDir tmp;
    check("自测有自己的临时目录", tmp.isValid() ? "yes" : "no", "yes");
    const QString dir = tmp.path();

    auto* live = makePathMenu(dir, nullptr);
    check("存在的目录：就一项", QString::number(live->actions().size()), QStringLiteral("1"));
    QAction* a0 = live->actions().first();
    check("存在的目录：文案", a0->text(), QStringLiteral("打开文件夹"));
    check("存在的目录：点得动", a0->isEnabled() ? "yes" : "no", "yes");
    // 行卡片点开之后拿这项的 data 去开资源管理器，所以 data 必须是这一行自己的目录
    check("存在的目录：点开就是要开这个目录", a0->data().toString(), dir);
    delete live;

    auto* gone = makePathMenu(dir + "/根本没这个目录", nullptr);
    QAction* g0 = gone->actions().first();
    check("目录已经不在了：文案说实话", g0->text(), QStringLiteral("这个目录已经不在了"));
    check("目录已经不在了：点不动", g0->isEnabled() ? "yes" : "no", "no");
    delete gone;

    // 右键要落在整行上（名字、下面那行路径、占比条都算），不能只有路径那一条字能点
    RowCard row(QStringLiteral("临时目录"), dir, false, nullptr, nullptr);
    check("整行都收得到右键", row.contextMenuPolicy() == Qt::CustomContextMenu ? "yes" : "no", "yes");
    int labels = 0;
    for (QLabel* l : row.findChildren<QLabel*>())
        if (l->text() == dir)
            ++labels;
    check("下面那行路径就是这一行自己的子控件（右键会冒上来）", labels == 1 ? "yes" : "no", "yes");
    return bad;
}


// --diskui：磁盘页那条「点整理 → 边扫边出数 → 定稿 → 换盘回来不重扫」的界面链条。
// 全程不 show 这一页（不往用户桌面上放东西），控件用 click()/setCurrentIndex() 驱动。
// 它慢（要真扫一整盘），所以是独立入口，不跟着 --plugintest 跑。
namespace {

QString bool2s(const void* p)
{
    return p ? QStringLiteral("yes") : QStringLiteral("no");
}

// 占用分析那一栏的行卡片数。"row" 这个 objectName 顶部汇总卡也叫这个名字，
// 而且两张卡都直接挂在这一页下 —— 所以按 parentWidget 是不是 rowsHost 来分。
QWidget* analysisHost(QWidget* root)
{
    const auto all = root->findChildren<QWidget*>(QString(), Qt::FindChildrenRecursively);
    for (QWidget* w : all)
        if (w->accessibleName() == QLatin1String("analysisRows"))
            return w;
    return nullptr;
}

int rowCountOf(DiskPage* p)
{
    QWidget* host = analysisHost(p);
    if (!host)
        return -1;
    int n = 0;
    const auto kids = p->findChildren<QWidget*>(QStringLiteral("row"));
    for (QWidget* w : kids)
        if (w->parentWidget() == static_cast<QWidget*>(host))
            ++n;
    return n;
}

QWidget* findByAcc(QWidget* root, const char* accName)
{
    const auto all = root->findChildren<QWidget*>(QString(), Qt::FindChildrenRecursively);
    for (QWidget* w : all)
        if (w->accessibleName() == QString::fromLatin1(accName))
            return w;
    return nullptr;
}

} // namespace

int diskUiSelftest()
{
    int bad = 0;
    const auto say = [&bad](const char* name, bool ok, const QString& got) {
        std::printf("%s %s：%s\n", ok ? "PASS" : "FAIL", name, got.toUtf8().constData());
        if (!ok)
            ++bad;
        std::fflush(stdout);
    };

    DiskPage page;
    auto* btn = qobject_cast<QPushButton*>(findByAcc(&page, "analyzeBtn"));
    auto* status = qobject_cast<QLabel*>(findByAcc(&page, "analysisStatus"));
    auto* path = qobject_cast<QLabel*>(findByAcc(&page, "analysisPath"));
    auto* drive = qobject_cast<QComboBox*>(findByAcc(&page, "driveBox"));
    say("按钮、两条状态、盘符下拉都找得到", btn && status && path && drive,
        QStringLiteral("btn=%1 status=%2 path=%3 drive=%4")
            .arg(bool2s(btn), bool2s(status), bool2s(path), bool2s(drive)));
    if (!btn || !status || !path || !drive)
        return bad ? bad : 1;

    const int before = rowCountOf(&page);
    btn->click();                       // 就是用户点那颗「整理」
    QElapsedTimer t;
    t.start();
    // 第一段：等到"至少一个一级分支定稿"。整盘十几秒，这一段通常几百毫秒就到。
    bool sawBranch = false;
    while (t.elapsed() < 30000 && !sawBranch) {
        QCoreApplication::processEvents();
        QThread::msleep(30);
        const QString s = status->text();
        sawBranch = s.contains(QString::fromUtf8("一级分支已扫完"))
                    && !s.contains(QString::fromUtf8("已扫完 0 /"));
    }
    const qint64 firstMs = t.elapsed();
    say("扫描途中状态行报了「已扫完 N 个一级分支」", sawBranch,
        QStringLiteral("%1ms · %2").arg(firstMs).arg(status->text()));
    const int duringRows = rowCountOf(&page);
    say("途中列表已经摆了行卡片（不是空白干等）", duringRows > 0,
        QStringLiteral("%1 行（点之前 %2 行）").arg(duringRows).arg(before));

    // 第二段：等它定稿（上限 180 秒，够冷缓存扫完整盘）
    bool doneScan = false;
    while (t.elapsed() < 180000 && !doneScan) {
        QCoreApplication::processEvents();
        QThread::msleep(30);
        doneScan = path->text().contains(QString::fromUtf8("上次扫描"))
                   || status->text().contains(QString::fromUtf8("本层"));
    }
    say("扫完定稿（状态换成树上的数、并且写下这次扫描的时间）", doneScan,
        QStringLiteral("%1ms · %2 / %3").arg(t.elapsed()).arg(path->text(), status->text()));
    const int afterRows = rowCountOf(&page);
    say("定稿后列表有东西", afterRows > 0, QStringLiteral("%1 行").arg(afterRows));

    // 第三段：换盘再换回来 —— 该直接用缓存，不该重扫，也不该把行弄丢
    if (drive->count() < 2) {
        std::printf("INFO 这台机器只列出一个盘，换盘那条没法测\n");
    } else {
        const int keep = drive->currentIndex();
        const int other = keep + 1 < drive->count() ? keep + 1 : keep - 1;
        drive->setCurrentIndex(other);
        QCoreApplication::processEvents();
        say("换到没扫过的盘：列表让位，不拿上一个盘的数当结论", rowCountOf(&page) == 0,
            QStringLiteral("%1 行").arg(rowCountOf(&page)));
        drive->setCurrentIndex(keep);
        QCoreApplication::processEvents();
        const int back = rowCountOf(&page);
        say("换回来直接用缓存就把行摆出来了（没重扫）", back > 0,
            QStringLiteral("%1 行 · %2").arg(back).arg(path->text()));
    }
    std::printf("diskui: 失败 %d 项\n", bad);
    std::fflush(stdout);
    return bad;
}

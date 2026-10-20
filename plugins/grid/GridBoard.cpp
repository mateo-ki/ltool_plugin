#include "GridBoard.h"

#include "Utils.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeData>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

namespace {

constexpr int kMargin = 8;      // 板子四周留白
constexpr int kGap = 8;         // 格子之间的缝
constexpr int kRadius = 8;
constexpr int kCloseBox = 22;   // ✕ 那颗按钮的边长
const char kSlotMime[] = "application/x-ltool-grid-slot";

// 后缀表从 Qt 自己那一份 supportedImageFormats() 来，不在这儿抄第二遍：
// 抄的那份第一次 Qt 加个格式就过期（webp/avif 就是这么漏掉的）。
bool suffixIsImage(const QString& suffix)
{
    static const QStringList supported = [] {
        QStringList out;
        for (const QByteArray& fmt : QImageReader::supportedImageFormats())
            out << QString::fromLatin1(fmt);
        return out;
    }();
    return supported.contains(suffix.toLower(), Qt::CaseInsensitive);
}

// 居中裁成正方形：预览和合成用的是同一条算法，两边看到的构图必须一致。
QRect squareCrop(int w, int h)
{
    const int side = qMin(w, h);
    return QRect((w - side) / 2, (h - side) / 2, side, side);
}

} // namespace

GridBoard::GridBoard(QWidget* parent)
    : QWidget(parent)
{
    setSlots(4);
    setAcceptDrops(true);
    setMinimumSize(320, 320);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("把图片拖到格子里；从一格拖到另一格换顺序；点右上角 ✕ 拿掉那一张"));
}

bool GridBoard::isImageFile(const QString& f)
{
    return QFileInfo(f).isFile() && suffixIsImage(QFileInfo(f).suffix());
}

void GridBoard::setSlots(int n)
{
    const int want = qBound(1, n, 9);
    QVector<QString> next(want);
    for (int i = 0; i < qMin(m_paths.size(), want); ++i)
        next[i] = m_paths[i];          // 换格数的时候把前面几张留着，别让人重摆
    m_paths = next;
    m_preview.clear();
    emit changed();
    update();
}

int GridBoard::cols() const
{
    return m_paths.size() == 4 ? 2 : 3;
}

int GridBoard::rows() const
{
    const int c = cols();
    return (m_paths.size() + c - 1) / c;
}

int GridBoard::filled() const
{
    int n = 0;
    for (const QString& p : m_paths)
        if (!p.isEmpty())
            ++n;
    return n;
}

QRect GridBoard::gridGeom() const
{
    return rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);
}

QRect GridBoard::cellRect(int i) const
{
    if (i < 0 || i >= m_paths.size())
        return QRect();
    const QRect g = gridGeom();
    const int c = cols();
    const int r = rows();
    // 格子必须是正方形：出图（GridPage::compose）就是一格一正方形，板子上画成长条，
    // 预览就得把方图拉扁去填满 —— 人摆的时候看着刚好、生成出来两边各少一块。
    const int side = qMax(1, qMin((g.width() - (c - 1) * kGap) / c,
                                  (g.height() - (r - 1) * kGap) / r));
    const int left = g.left() + (g.width() - (side * c + (c - 1) * kGap)) / 2;
    const int top = g.top() + (g.height() - (side * r + (r - 1) * kGap)) / 2;
    const int col = i % c;
    const int row = i / c;
    return QRect(left + col * (side + kGap), top + row * (side + kGap), side, side);
}

int GridBoard::cellAt(const QPoint& p) const
{
    for (int i = 0; i < m_paths.size(); ++i)
        if (cellRect(i).contains(p))
            return i;
    return -1;
}

QRect GridBoard::closeRect(const QRect& cell)
{
    return QRect(cell.right() - kCloseBox - 4, cell.top() + 4, kCloseBox, kCloseBox);
}

void GridBoard::addImages(const QStringList& files)
{
    int accepted = 0, refused = 0;
    for (const QString& f : files) {
        ++m_dropped;
        if (!isImageFile(f)) {
            ++refused;
            ++m_rejected;
            continue;
        }
        int slot = -1;
        for (int i = 0; i < m_paths.size(); ++i)
            if (m_paths[i].isEmpty()) { slot = i; break; }
        if (slot < 0) {
            ++refused;
            ++m_rejected;
            continue;
        }
        m_paths[slot] = QFileInfo(f).absoluteFilePath();
        ++accepted;
    }
    m_preview.clear();
    emit changed();
    update();
    if (refused > 0)
        emit status(QStringLiteral("收了 %1 张；另有 %2 个不是图片或者格子已经满了（一共 %3 格）")
                        .arg(accepted).arg(refused).arg(m_paths.size()));
    else
        emit status(QStringLiteral("收了 %1 张，现在 %2/%3 格").arg(accepted).arg(filled()).arg(m_paths.size()));
}

void GridBoard::clearImages()
{
    for (QString& p : m_paths)
        p.clear();
    m_preview.clear();
    emit changed();
    update();
}

void GridBoard::removeAt(int i)
{
    if (i < 0 || i >= m_paths.size() || m_paths[i].isEmpty())
        return;
    m_paths[i].clear();
    m_preview.clear();
    emit changed();
    emit status(QStringLiteral("拿掉了第 %1 格").arg(i + 1));
    update();
}

void GridBoard::moveOrSwap(int from, int to)
{
    if (from < 0 || to < 0 || from >= m_paths.size() || to >= m_paths.size() || from == to)
        return;
    if (m_paths[from].isEmpty())
        return;                       // 空格拖不动：没有东西可放
    m_paths.swapItemsAt(from, to);    // 目标是空格时这就等于"挪过去"
    emit changed();
    update();
}

bool GridBoard::acceptsMime(const QMimeData* data) const
{
    if (!data)
        return false;
    if (data->hasFormat(QLatin1String(kSlotMime)))
        return true;
    if (!data->hasUrls())
        return false;
    for (const QUrl& u : data->urls()) {
        if (u.isLocalFile() && isImageFile(u.toLocalFile()))
            return true;
    }
    return false;
}

QPixmap GridBoard::previewAt(int i, const QRect& cell)
{
    const QString path = m_paths.value(i);
    if (path.isEmpty())
        return QPixmap();
    const QString key = QStringLiteral("%1@%2x%3").arg(path).arg(cell.width()).arg(cell.height());
    auto it = m_preview.constFind(key);
    if (it != m_preview.constEnd())
        return it.value();
    QImageReader r(path);
    r.setAutoTransform(true);         // 手机照片带 EXIF 方向，不转就是歪的/倒的
    // 解码到"短边 = 格子边长"就够了，再把正中间那块正方形切出来 —— 和 GridPage::compose
    // 必须同一块构图：整张压扁成方形是另一种样子，人会以为生成错了。
    const QSize hinted = r.size();
    if (hinted.isValid() && qMin(hinted.width(), hinted.height()) > cell.width()) {
        const double k = double(cell.width()) / qMin(hinted.width(), hinted.height());
        r.setScaledSize(QSize(qMax(1, qRound(hinted.width() * k)),
                              qMax(1, qRound(hinted.height() * k))));
    }
    const QImage img = r.read();
    if (img.isNull()) {
        Utils::log(QStringLiteral("grid: 预览读不出来 %1：%2").arg(path, r.errorString()));
        return QPixmap();
    }
    const QPixmap pm = QPixmap::fromImage(img.convertToFormat(QImage::Format_ARGB32_Premultiplied))
                           .copy(squareCrop(img.width(), img.height()))
                           .scaled(cell.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    m_preview.insert(key, pm);
    return pm;
}

void GridBoard::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    for (int i = 0; i < m_paths.size(); ++i) {
        const QRect cell = cellRect(i);
        QPainterPath clip;
        clip.addRoundedRect(cell, kRadius, kRadius);
        if (m_paths[i].isEmpty()) {
            p.setPen(QPen(palette().color(QPalette::Mid), 1.4, Qt::DashLine));
            p.drawPath(clip);
            p.setPen(palette().color(QPalette::PlaceholderText));
            // 两行摆：一行写完在 138 见方的小格子里会被裁掉（离屏量出来的）
            p.drawText(cell.adjusted(0, 0, 0, -cell.height() / 5), Qt::AlignCenter,
                       QStringLiteral("第 %1 格").arg(i + 1));
            p.drawText(cell.adjusted(0, cell.height() / 5, 0, 0), Qt::AlignCenter,
                       QStringLiteral("点一下或拖进来"));
            continue;
        }
        const QPixmap pm = previewAt(i, cell);
        if (pm.isNull()) {
            // 文件被挪走/解码失败：这一格要说实话，不能画成"还没放图"那个样子
            p.setPen(palette().color(QPalette::PlaceholderText));
            p.drawText(cell, Qt::AlignCenter, QStringLiteral("第 %1 格 · 这张读不出来").arg(i + 1));
            continue;
        }
        p.save();
        p.setClipPath(clip);
        p.drawPixmap(cell.topLeft(), pm);
        p.restore();
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.drawPath(clip);
        // 序号角标：合成出来的图上也有同一个号，两边对得上才敢说"顺序就是你看到的"
        p.setBrush(QColor(0, 0, 0, 150));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(cell.left() + 5, cell.top() + 5, 22, 18, 5, 5);
        p.setPen(Qt::white);
        p.drawText(QRect(cell.left() + 5, cell.top() + 5, 22, 18), Qt::AlignCenter, QString::number(i + 1));
        const QRect x = closeRect(cell);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(x, 5, 5);
        p.setPen(Qt::white);
        p.drawText(x, Qt::AlignCenter, QStringLiteral("✕"));
    }
}

void GridBoard::dragEnterEvent(QDragEnterEvent* e)
{
    if (acceptsMime(e->mimeData()))
        e->acceptProposedAction();
}

void GridBoard::dragMoveEvent(QDragMoveEvent* e)
{
    if (acceptsMime(e->mimeData()))
        e->acceptProposedAction();
}

void GridBoard::dropEvent(QDropEvent* e)
{
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QLatin1String(kSlotMime))) {
        const int from = md->data(QLatin1String(kSlotMime)).trimmed().toInt();
        const int to = cellAt(e->position().toPoint());
        if (to >= 0)
            moveOrSwap(from, to);
        e->acceptProposedAction();
        return;
    }
    QStringList files;
    for (const QUrl& u : md->urls())
        if (u.isLocalFile())
            files << u.toLocalFile();
    if (files.isEmpty())
        return;
    addImages(files);
    e->acceptProposedAction();
}

void GridBoard::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const int i = cellAt(e->pos());
    if (i < 0)
        return;
    if (!m_paths[i].isEmpty() && closeRect(cellRect(i)).contains(e->pos())) {
        removeAt(i);
        return;
    }
    m_pressEmpty = -1;
    m_pressCell = -1;
    if (m_paths[i].isEmpty()) {
        // 空格子没有东西可拖，但"点它一下"是有意思的：往里放一张。松开时原位才算点中。
        m_pressEmpty = i;
        m_emptyPos = e->pos();
        return;
    }
    m_pressPos = e->pos();
}

void GridBoard::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    const int i = m_pressEmpty;
    m_pressEmpty = -1;
    if (i < 0)
        return;
    if (cellAt(e->pos()) != i)
        return;                                     // 松在别的格子上 = 没点中这一格
    if ((e->pos() - m_emptyPos).manhattanLength() > QApplication::startDragDistance())
        return;                                     // 按下去还挪了一大截 = 不是点
    emit pickInto(i);
}

bool GridBoard::putAt(int i, const QString& file)
{
    if (i < 0 || i >= m_paths.size() || !isImageFile(file))
        return false;
    m_paths[i] = QFileInfo(file).absoluteFilePath();
    m_preview.clear();
    emit changed();
    emit status(QStringLiteral("第 %1 格放了 %2").arg(i + 1).arg(QFileInfo(file).fileName()));
    update();
    return true;
}

void GridBoard::mouseMoveEvent(QMouseEvent* e)
{
    if (m_pressCell < 0 || !(e->buttons() & Qt::LeftButton))
        return;
    if ((e->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance())
        return;
    const int from = m_pressCell;
    m_pressCell = -1;
    auto* mime = new QMimeData;
    mime->setData(QLatin1String(kSlotMime), QByteArray::number(from));
    auto* drag = new QDrag(this);
    drag->setMimeData(mime);
    const QRect cell = cellRect(from);
    const QPixmap pm = previewAt(from, cell);
    if (pm.isNull())
        return;                       // 读不出来的那张不拖着走（QDrag 拿一张空图，屏幕上什么都有不了）
    drag->setPixmap(pm);
    drag->setHotSpot(e->pos() - cell.topLeft());
    drag->exec(Qt::MoveAction);
}

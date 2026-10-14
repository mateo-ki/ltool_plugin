#include "Anno.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <QtMath>

namespace Anno {

QIcon toolIcon(Tool t)
{
    QPixmap pm(18, 18);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor("#e8ebf2"));
    pen.setWidthF(1.7);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (t) {
    case Rect:
        p.drawRoundedRect(QRectF(3, 4.5, 12, 9), 2, 2);
        break;
    case Ellipse:
        p.drawEllipse(QRectF(3, 3, 12, 12));
        break;
    case Arrow:
        p.drawLine(QPointF(4, 14), QPointF(13.5, 4.5));
        {
            QPainterPath head;
            head.moveTo(9.5, 4.0);
            head.lineTo(14.0, 4.0);
            head.lineTo(14.0, 8.5);
            p.drawPath(head);
        }
        break;
    case Text:
        p.drawLine(QPointF(4, 4.5), QPointF(14, 4.5));
        p.drawLine(QPointF(9, 4.5), QPointF(9, 14));
        break;
    case Mosaic:
        p.setBrush(QColor("#e8ebf2"));
        p.drawRect(3, 3, 5, 5);
        p.drawRect(10, 3, 5, 5);
        p.drawRect(3, 10, 5, 5);
        p.setBrush(Qt::NoBrush);
        p.drawRect(10, 10, 5, 5);
        break;
    case Crop: { // 裁剪：虚线框 + 左上角实线直角，一眼看出"留下框里的部分"
        QPen dash(QColor("#e8ebf2"));
        dash.setWidthF(1.4);
        dash.setStyle(Qt::DashLine);
        dash.setCapStyle(Qt::FlatCap);
        p.setPen(dash);
        p.drawRect(4, 2, 11, 11);
        p.setPen(pen);
        p.drawLine(2, 5, 2, 2);
        p.drawLine(2, 2, 5, 2);
        p.drawLine(16, 13, 16, 16);
        p.drawLine(16, 16, 13, 16);
        break;
    }
    case Pin:
        p.setBrush(QColor("#e8ebf2"));
        p.drawRoundedRect(QRectF(4.5, 3, 9, 5), 2, 2);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(9, 8), QPointF(9, 14.5));
        p.drawLine(QPointF(6, 14.5), QPointF(12, 14.5));
        break;
    default:
        break;
    }
    p.end();
    return QIcon(pm);
}

void drawArrow(QPainter& p, const QPoint& from, const QPoint& to)
{
    p.drawLine(from, to);
    const double ang = qAtan2(to.y() - from.y(), to.x() - from.x());
    const int head = 14;
    QPolygonF tri;
    tri << QPointF(to);
    tri << QPointF(to.x() - head * qCos(ang - 0.45), to.y() - head * qSin(ang - 0.45));
    tri << QPointF(to.x() - head * qCos(ang + 0.45), to.y() - head * qSin(ang + 0.45));
    p.setBrush(p.pen().color());
    p.drawPolygon(tri);
}

void drawShape(QPainter& p, Tool t, const QRect& r, const QPoint& from, const QPoint& to)
{
    switch (t) {
    case Rect:    p.drawRect(r); break;
    case Ellipse: p.drawEllipse(r); break;
    case Arrow:   drawArrow(p, from, to); break;
    default: break;
    }
}

// 逻辑矩形 → 这张图上的设备矩形。dpr=1（100% 缩放）时原样返回，所以不动老行为。
QRect deviceRect(const QRect& logical, qreal dpr)
{
    if (logical.isEmpty() || dpr <= 0.0 || dpr == 1.0)
        return logical;
    const int px = int(logical.x() * dpr);
    const int py = int(logical.y() * dpr);
    return QRect(px, py, int((logical.x() + logical.width()) * dpr) - px,
                 int((logical.y() + logical.height()) * dpr) - py);
}

// 一张带 DPR 的图"能画的范围"是逻辑尺寸，不是 QPixmap::rect()（那是物理像素）。
// 打码的裁剪范围如果拿 rect() 去交，150% 下会多出去三分之一。
QRect logicalBounds(const QPixmap& pm)
{
    const qreal dpr = pm.devicePixelRatio() > 0.0 ? pm.devicePixelRatio() : 1.0;
    if (dpr == 1.0)
        return pm.rect();
    return QRect(0, 0, int(pm.width() / dpr + 0.5), int(pm.height() / dpr + 0.5));
}

// 打码块的取材：源图按设备像素取，放大回来也要按设备尺寸做，最后用 DPR 告诉绘制者
// "这块的逻辑大小就是这个" —— 不换算的话缩放 150% 时马赛克盖住的不是你要挡的那块，
// 等于没打码（这是隐私问题，不是美观问题）。
QPixmap mosaicOf(const QPixmap& src, const QRect& logicalZone, int block)
{
    const qreal dpr = src.devicePixelRatio() > 0.0 ? src.devicePixelRatio() : 1.0;
    const QRect dev = deviceRect(logicalZone, dpr);
    QImage small = src.copy(dev).toImage().scaled(qMax(1, dev.width() / block),
                                                 qMax(1, dev.height() / block),
                                                 Qt::IgnoreAspectRatio, Qt::FastTransformation);
    QPixmap mosaic = QPixmap::fromImage(
        small.scaled(dev.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation));
    mosaic.setDevicePixelRatio(dpr);
    return mosaic;
}

void mosaicSmudge(QPixmap& canvas, const QPixmap& src, const QPoint& center, int radius, int block)
{
    const QRect zone(center.x() - radius, center.y() - radius, radius * 2, radius * 2);
    const QRect z = zone.intersected(logicalBounds(canvas));
    if (z.isEmpty() || src.isNull() || block <= 0)
        return;
    const QPixmap mosaic = mosaicOf(src, z, block);
    QPainter p(&canvas);
    QPainterPath clip;
    clip.addEllipse(center, radius, radius);
    p.setClipPath(clip);
    p.drawPixmap(z.topLeft(), mosaic);
}

void mosaicBlock(QPixmap& canvas, const QPixmap& src, const QRect& area, int block)
{
    const QRect z = area.normalized().intersected(logicalBounds(canvas));
    if (z.isEmpty() || src.isNull() || block <= 0)
        return;
    QPainter p(&canvas);
    p.drawPixmap(z.topLeft(), mosaicOf(src, z, block));
}

QRect shapeRect(const Shape& s)
{
    return QRect(s.a, s.b).normalized();
}

// 命中块比画出来的方块大一点（11px），小屏上也点得中

// 命中块比画出来的方块大一点（11px），小屏上也点得中
QVector<QRect> boxHandles(const QRect& r)
{
    const int k = 11;
    const auto at = [k](int x, int y) { return QRect(x - k / 2, y - k / 2, k, k); };
    const int l = r.left(), t = r.top(), rr = r.right(), b = r.bottom();
    const int cx = (l + rr) / 2, cy = (t + b) / 2;
    return { at(l, t), at(cx, t), at(rr, t), at(rr, cy),
             at(rr, b), at(cx, b), at(l, b), at(l, cy) };
}

QVector<QRect> shapeHandles(const Shape& s)
{
    if (s.tool == Arrow) {
        const int k = 11;
        const auto at = [k](const QPoint& p) { return QRect(p.x() - k / 2, p.y() - k / 2, k, k); };
        return { at(s.a), at(s.b) };   // 箭头只拖两端，拖"包围盒手柄"没意义
    }
    return boxHandles(shapeRect(s));
}

bool hitsShape(const Shape& s, const QPoint& pos)
{
    if (s.tool == Arrow) {
        const double vx = double(s.b.x() - s.a.x()), vy = double(s.b.y() - s.a.y());
        const double len2 = vx * vx + vy * vy;
        double t = 0.0;
        if (len2 > 1.0)
            t = qBound(0.0, (double(pos.x() - s.a.x()) * vx + double(pos.y() - s.a.y()) * vy) / len2, 1.0);
        const double px = pos.x() - (s.a.x() + vx * t), py = pos.y() - (s.a.y() + vy * t);
        return std::sqrt(px * px + py * py) < 8.0;
    }
    if (s.tool == Rect || s.tool == Ellipse || s.tool == Mosaic)
        return shapeRect(s).adjusted(-6, -6, 6, 6).contains(pos);
    return false;
}

int hitShape(const QVector<Shape>& shapes, const QPoint& pos)
{
    for (int i = shapes.size() - 1; i >= 0; --i)
        if (hitsShape(shapes[i], pos))
            return i;
    return -1;
}

// 拖手柄=按边伸缩；从按下时的快照算，避免一点点累积漂移。bounds 用来夹住别跑出图外
QRect resizeRect(const QRect& r0, int what, const QPoint& d, const QRect& bounds)
{
    int l = r0.left(), t = r0.top(), r = r0.right(), b = r0.bottom();
    switch (what) {
    case 0: l += d.x(); t += d.y(); break;
    case 1: t += d.y(); break;
    case 2: r += d.x(); t += d.y(); break;
    case 3: r += d.x(); break;
    case 4: r += d.x(); b += d.y(); break;
    case 5: b += d.y(); break;
    case 6: l += d.x(); b += d.y(); break;
    case 7: l += d.x(); break;
    default: return r0;
    }
    return QRect(QPoint(l, t), QPoint(r, b)).normalized().intersected(bounds);
}

} // namespace Anno

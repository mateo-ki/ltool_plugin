#pragma once

#include <QIcon>
#include <QPoint>
#include <QRect>
#include <QVector>

class QPainter;
class QPixmap;

// 截图标注的共用实现：截图浮层（CaptureOverlay）和置顶窗（PinWindow 编辑模式）
// 用同一套工具、同一份图标和同一种画笔手感，两边行为必须一致，所以只留一份实现。
namespace Anno {

enum Tool { None = 0, Rect, Ellipse, Arrow, Text, Mosaic, Crop, Pin };

// 一笔形状 = 两个对角点（箭头是起点/终点）。画出来是"对象"而不是直接烙进画布，
// 这样事后还能点中它拖动、拉手柄改大小。
struct Shape {
    Tool tool = Rect;
    QPoint a, b;
};

// 自绘图标：不依赖字体里的符号（📌 之类在深色浮层上渲染不稳）
QIcon toolIcon(Tool t);

// 一条箭头（含箭头三角），pen 决定颜色/线宽
void drawArrow(QPainter& p, const QPoint& from, const QPoint& to);

// 拖出来的一笔形状：矩形/圆/箭头。马赛克和文字另走各自的接口
void drawShape(QPainter& p, Tool t, const QRect& r, const QPoint& from, const QPoint& to);

// 马赛克一笔：把 src 上以 center 为圆心、radius 为半径的圆内区域打码，画回 canvas 同一位置。
// 直接改像素（不是缩放贴图），这样置顶窗在缩放显示下也能按图像真实坐标涂抹。
void mosaicSmudge(QPixmap& canvas, const QPixmap& src, const QPoint& center, int radius,
                  int block = 8);

// 矩形打码块：把 src 上 r 那块打码后画回 canvas 同一位置。置顶窗里"拖框马赛克"
// 是个可编辑对象，所以画的时候才现算，不烙进底图。
void mosaicBlock(QPixmap& canvas, const QPixmap& src, const QRect& r, int block = 8);

// 逻辑矩形 → 这张图上的设备矩形（dpr=1 时原样返回，100% 缩写下行为一点没动）。
// 存在的理由：QPixmap::copy 和 QPainter::drawPixmap 的**来源**矩形都是按**设备像素**
// 解释的（--capgeo 实测），而浮层手里的选区/圆心全是逻辑坐标 —— 缩放 150% 时不换算就是
// "截全屏只截到左上 44%"和"马赛克盖错地方"。画到画布上的那一步（目标矩形）反而是逻辑的。
QRect deviceRect(const QRect& logical, qreal dpr);

// 一张带 DPR 的图"画得进去的范围"是它的逻辑尺寸，不是 QPixmap::rect()（那是物理像素）。
QRect logicalBounds(const QPixmap& pm);

// ---- 选中/拖动/缩放的纯几何：两边共用一份，避免浮层和置顶窗各写一套手感就分叉 ----

constexpr int kHandleCount = 8;      // 包围盒八个手柄（左上起顺时针）
constexpr int kMove = 8;             // "拖内部整体平移"用的第 9 个编号

QRect shapeRect(const Shape& s);
QVector<QRect> boxHandles(const QRect& r);           // 包围盒的八个手柄命中块
QVector<QRect> shapeHandles(const Shape& s);         // 箭头 = 两端点，其余 = 包围盒
bool hitsShape(const Shape& s, const QPoint& pos);
int hitShape(const QVector<Shape>& shapes, const QPoint& pos);   // 后画的在上层，先命中
QRect resizeRect(const QRect& r0, int what, const QPoint& d, const QRect& bounds);

} // namespace Anno

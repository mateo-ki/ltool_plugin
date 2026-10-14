#pragma once

#include <QVector>
#include <QWidget>

#include "Anno.h"

// F1 截图：全屏截图 → 框选/窗口吸附 → 标注（矩形/圆/箭头/文字/马赛克）→ 复制+保存
class QLineEdit;

class CaptureOverlay : public QWidget {
    Q_OBJECT
public:
    static void start();  // 注册的 F1 热键触发入口
    static bool isActive();   // 浮层正开着：搜索框靠它决定"截图期间先别收起"
    // 诊断入口：走和按 Esc 完全一样的取消路径。存在的意义是让"按 F1 有没有反应"能量化，
    // 而不必往系统注入 F1（那是全局热键，注入成功=把全屏浮层压在用户屏幕上）
    static void cancelActive();

    // 诊断入口（--snaprect）：这个窗口被吸附出来会是哪个选区（**物理像素**，不换算逻辑坐标，
    // 所以能和一个窗口两种矩形直接比）。存在的意义是"吸附到底贴不贴边"可以不开浮层就断言。
    static QRect snapRectFor(WId hwnd);

    // 诊断入口（--capgeo）：把"逻辑选区 × 物理画布"这一格换算单独拿出来，rendered() 用的
    // 就是它。缩放 150% 时截不全全屏就错在这一步，而它不需要真去截屏就能断言。
    static QPixmap cropCanvas(const QPixmap& canvas, const QRect& logicalSel);

    // 上面那一步的换算本体，单独摆出来好断言（dpr=1 时原样返回）。
    static QRect deviceRectFor(const QRect& logicalSel, qreal dpr);

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    enum Mode { Selecting, Annotate };
    // 工具与置顶窗共用一份定义（Anno），两边手感/图标保持一致
    using Tool = Anno::Tool;
    using enum Anno::Tool;

    // 画出来的形状是"对象"而不是直接烙进画布：这样事后还能点中它拖动、拉手柄改大小。
    // 形状本身和选中/缩放的几何都在 Anno 里，置顶窗（PinWindow）用的是同一套。
    using Shape = Anno::Shape;
    struct Snapshot {
        QPixmap canvas;
        QVector<Shape> shapes;
    };

    // 选区调整：0..7 是八个手柄（左上起顺时针），kMove 是选区内部拖动
    static constexpr int kHandleCount = 8;
    static constexpr int kMove = 8;
    static constexpr int kNoDrag = -1;

    explicit CaptureOverlay(const QPixmap& shot);
    void finishSelect();
    void buildToolbar();
    void placeToolbar();
    QRect selNorm() const;
    QVector<QRect> handleRects() const;
    static QVector<QRect> handlesFor(const QRect& r);
    static QRect shapeRect(const Shape& s);
    QVector<QRect> shapeHandles(const Shape& s) const;
    bool hitsShape(const Shape& s, const QPoint& pos) const;
    int shapeAt(const QPoint& pos) const;
    void beginShapeDrag(int what, const QPoint& pos);
    void applyShapeDrag(const QPoint& pos);
    void applyDrag(const QPoint& pos);
    void drawShapes(QPainter& p) const;
    QPixmap rendered() const;
    void undoStep();
    void pushUndo();
    void commit();
    void toClipboard();
    void extractText();
    void saveFile();
    void cancel();

    QPixmap m_shot;   // 原始截图
    QPixmap m_canvas; // 标注画布（马赛克/文字烙在上面，形状保持为对象）
    Mode m_mode = Selecting;
    Tool m_tool = None;
    QRect m_sel;       // 已确认选区
    QRect m_suggest;   // 窗口吸附建议
    QPoint m_press;
    bool m_dragging = false;
    bool m_moved = false;
    bool m_hasSuggest = false;
    int m_dragWhat = kNoDrag;   // 标注阶段：正在拖手柄/移动选区
    QRect m_sel0;               // 拖动开始时的选区快照
    QVector<Shape> m_shapes;
    int m_selShape = -1;        // 选中的形状下标
    Shape m_shapeSnap;          // 这一笔拖动开始前的形状
    bool m_shapePushed = false; // 拖动是否已压进撤销栈
    QVector<Snapshot> m_undo;
    QPoint m_last;
    QWidget* m_toolbar = nullptr;
    QLineEdit* m_edit = nullptr;
    QPoint m_textPos;
};

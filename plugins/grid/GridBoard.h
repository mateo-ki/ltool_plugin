#pragma once

#include <QHash>
#include <QPixmap>
#include <QVector>
#include <QWidget>

class QMimeData;

// 宫格板：N 张图摆在 2x2（四宫）/ 3x2（六宫）/ 3x3（九宫）的格子里。
//
// 三种动手的方式，都在这一个控件里：
//   往板上拖文件      = 从第一个空格开始往里填，满了不收（页上会说一句）
//   从一格拖到另一格  = 换位置（目标有图就是交换，空格就是挪过去）
//   点格子右上角的 ✕  = 拿掉那一张，后面的不动（留个空格，合成时那块就是背景色）
//
// 预览走 QImageReader 的 scaledSize，不把原图整张吃进内存 —— 九张 4000x3000 的图
// 按原尺寸进 QPixmap 就是几百 MB，而那只是给人看个位置。合成是另一条路（GridPage::compose），
// 那边要全尺寸。
class GridBoard : public QWidget
{
    Q_OBJECT
public:
    explicit GridBoard(QWidget* parent = nullptr);

    void setSlots(int n);                       // 4 / 6 / 9
    int slotCount() const { return m_paths.size(); }
    int filled() const;
    QVector<QString> paths() const { return m_paths; }

    void addImages(const QStringList& files);   // 只收真在盘上、后缀是图片的那些
    void clearImages();
    void moveOrSwap(int from, int to);
    void removeAt(int i);                       // 拿掉那一格（右上角那颗 ✕ 走的就是它）
    bool putAt(int i, const QString& file);         // 往**指定那一格**放一张（点空格子选回来的图走这条）

    int cols() const;                           // 四宫 2 列，六宫/九宫 3 列
    int rows() const;
    QRect cellRect(int i) const;                // 第 i 格的矩形（正方形，整块居中）—— 判据也量它
    bool acceptsMime(const QMimeData* data) const;   // 拖进来的东西收不收（判据也走这条）
    int droppedCount() const { return m_dropped; }  // 一共往里放过几张（含被拒的）
    int rejectedCount() const { return m_rejected; }

signals:
    void changed();                             // 内容或顺序变了
    void status(const QString& text);           // 页上那一行状态字（满了/收了几个）
    void pickInto(int index);                   // 空格子被点了一下：请页弹选图框，把选中的交给 putAt

protected:
    void paintEvent(QPaintEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;   // 空格子按下来又原位松开 = 点它一下

private:
    int cellAt(const QPoint& p) const;          // -1 = 不在任何格子上
    static QRect closeRect(const QRect& cell);  // 格子右上角那颗 ✕ 的命中区
    static bool isImageFile(const QString& f);
    QPixmap previewAt(int i, const QRect& cell);
    QRect gridGeom() const;                     // 整个格子区的矩形（含间距）

    QVector<QString> m_paths;                   // 长度 = slotCount()，空位是空串
    mutable QHash<QString, QPixmap> m_preview;  // 按文件路径缓存的缩略图
    QPoint m_pressPos;
    int m_pressCell = -1;
    QPoint m_emptyPos;
    int m_pressEmpty = -1;
    int m_dropped = 0;
    int m_rejected = 0;
};

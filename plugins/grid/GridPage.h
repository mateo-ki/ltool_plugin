#pragma once

#include <QColor>
#include <QImage>
#include <functional>
#include <utility>
#include <QWidget>

class GridBoard;
class QDialog;
class QLabel;
class QPushButton;

// 图片排版那一页：选几宫（四/六/九）→ 把图拖进格子 → 拖动换顺序 → 挑一个背景色 → 生成。
//
// 生成出来的是一张正方形格子的合成图：每格居中裁成正方形（和板上预览同一条算法），
// 空格子就是背景色。落点用配置里那个「截图存放目录」，文件名 grid-<时间>-<n>宫.png。
class GridPage : public QWidget
{
    Q_OBJECT
public:
    explicit GridPage(QWidget* parent = nullptr);

    GridBoard* board() const { return m_board; }
    void setSlots(int n);                       // 4 / 6 / 9，同时把那一颗按钮按下去
    int slotCount() const;

    // 合成：每格 cellPx 像素、格子之间留 gap 像素、底色 bg。空格子只留底色。
    QImage compose(int cellPx, int gap, const QColor& bg) const;

    // 「生成宫图」那颗：合成 → **先弹框把效果图给人过目** → 人按「保存」才落盘。
    // 返回写出来的文件路径；一张都没有、或者人在弹框里按了取消，回空串（话写在状态行里）。
    QString generate();

    // 弹框里那颗「保存」走的就是这一条。拆出来有两个理由：一是"看过了再写"这条顺序要在代码里
    // 看得见，二是判据能绕开模态框直接验落盘那一步（离屏里 exec() 的返回值不作数）。
    QString saveComposed(const QImage& img);
    // 弹框上面那行字（尺寸 / 几宫 / 背景 / 落在哪个目录）—— 抽出来是因为它也得被断言。
    QString previewCaption(const QImage& img, const QColor& bg) const;

    // 弹框本体（造好但没弹）。判据拿它验"保存/取消/背景色三颗都在、那行字说的是全尺寸、
    // 换色当场重画"。造的时候把 m_previewBg / m_previewImg 摆成当前这套。
    QDialog* previewDialog(const QImage& img);
    QImage previewImage() const { return m_previewImg; }     // 弹框里当前那张（换过色就是新的）
    QColor previewBackground() const { return m_previewBg; } // 弹框里正在挑的那个色
    // 按「保存」才认这个色：把 m_bg 定下来并落盘（generate() 走的就是这一条，判据也走这一条）。
    QString commitPreview(const QImage& img, const QColor& bg);

    // 弹框那一步的"门"。默认就是弹框问人；判据把它换成直接同意 / 直接回绝，
    // 这样**"没看过效果图就不许落盘"这条顺序本身**能被断言 —— 离屏里 exec() 不作数，
    // 光测 saveComposed 的话，谁把弹框从 generate() 里删掉都还是全绿。
    void setPreviewGate(std::function<bool(const QImage&)> gate) { m_gate = std::move(gate); }

    // 「点空格子 → 选一张图放进去」那一步的选图框：默认是 QFileDialog，判据换成给一个固定路径。
    // 理由和上面那道门一样 —— 不然"点了空格子会把图放进**那一格**"这条只能靠人眼看。
    void setPickGate(std::function<QString()> gate) { m_pickGate = std::move(gate); }

    // 弹框里那颗「背景色」的取色框：默认是 QColorDialog，判据换成给一个固定颜色。
    void setPreviewBgGate(std::function<QColor(const QColor&)> gate) { m_bgGate = std::move(gate); }

    void setBackgroundColor(const QColor& c);   // 就是定下这个色（选它的按钮在效果图弹框里）
    QColor backgroundColor() const { return m_bg; }
    void setCellPixels(int px) { m_cellPx = qBound(128, px, 4096); }

signals:
    void generated(const QString& file);        // 自测拿它确认"真写出来了"

private:
    void pickPreviewBg();             // 弹框里那颗：取色 → 重摆 → 重画那一块
    void pickIntoCell(int index);   // 板子发出 pickInto 之后：弹选图框，选中的交给那一格
    void addFromFile();
    void showStatus(const QString& text);

    GridBoard* m_board = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_openBtn = nullptr;
    QPushButton* m_dirBtn = nullptr;
    QVector<QPushButton*> m_slotBtns;           // 索引 0/1/2 = 四/六/九
    QColor m_bg = Qt::white;
    int m_cellPx = 1080;
    std::function<bool(const QImage&)> m_gate;
    std::function<QString()> m_pickGate;
    std::function<QColor(const QColor&)> m_bgGate;
    QColor m_previewBg = Qt::white;     // 弹框里正在挑的（没按保存不算数）
    QImage m_previewImg;                // 弹框里当前那张效果图
    QString m_lastFile;
};

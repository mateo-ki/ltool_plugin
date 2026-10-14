#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class Recorder;

// F2 录屏的界面那一层：
//  WindowPicker = 列出"整个屏幕"+ 桌面上看得见的窗口，选一个要录的（不弹就不知道该录谁）
//  RecordBar    = 录制中挂在屏幕上的小条：红点 + 计时 + 帧数 + 停止
// 二者都只是 Recorder 的壳，编码/取帧全在 Recorder 里。
//
// 2026-10-15：这一层（连同 Recorder / RecProbe）从 ltoolcore.dll 搬进了 plugins/record，
// 只有那个插件用它们，主程序只认动作名 "record"。原来 WindowPicker 上的 LTOOL_CORE_API
// 是为了让 src/main.cpp 能点名这个类型才贴的，现在跨不到边界了，宏和 core-api.h 一起删。
namespace RecordUi {

// F2 的入口：弹选择列表，选完就开录（已经在录时什么都不做）
void startViaPicker();
bool isRecording();

// 给插件那张状态页说实话用的两句：当前配置的画质档 / 画面档
//（就是选择窗里那两颗下拉框存进 config.ini 的值，档位表只有一份 —— 见 RecordUi.cpp）
QString qualityText();
QString maxWidthText();

} // namespace RecordUi

class WindowPicker : public QWidget {
    Q_OBJECT
public:
    explicit WindowPicker(QWidget* parent = nullptr);

signals:
    // monitor=true 时 target 是 HMONITOR（录整块屏幕），否则是 HWND（录那一个窗口）
    void picked(WId target, const QString& title, bool monitor);

protected:
    // 这个窗没有标题栏：Esc、点到别处、再按一次 F2 都得能收掉它，否则按了 F2
    // 就只能硬着头皮选一个开始录（提示语一直写着"Esc 取消"，以前没实现）。
    // 右上角另外有个「✕」，那是给人眼看的落点。
    void keyPressEvent(QKeyEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;

private:
    QWidget* m_list = nullptr;
    QPushButton* m_close = nullptr;
};

class RecordBar : public QWidget {
    Q_OBJECT
public:
    RecordBar(Recorder* rec, const QString& path, QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    void poll();
    void finish(const QString& error);

    Recorder* m_rec;
    QString m_path;
    QLabel* m_text = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_cancel = nullptr;
    QPoint m_press;
    bool m_dragging = false;
    bool m_finished = false;
    bool m_discard = false;      // 「不要了」按下去：收尾之后把文件删掉，别留一个废 MP4
};

#pragma once

#include <QObject>
#include <QPixmap>
#include <QSize>
#include <QWindow>   // WId

class QTimer;

// 选择窗右边那一小块实时预览。
//
// 取帧用的是和 Recorder 同一一路 Windows.Graphics.Capture —— 不是另找一条便宜路
// （PrintWindow 对 GPU 直出的内容给的是黑块，那种预览比没有更坏）。
// 帧按当前「画面」档缩到**编码尺寸**再摆出来，所以那块小图就是将要进 mp4 的那一张。
//
// 只有一帧一帧往 CPU 拷这一步和 Recorder 相同；这里不建编码器、不落盘，
// 而且刻意跑在主线程（QTimer 每 100ms 取一帧，拷一帧中位 1ms 上下，实测见 --recprobe）。
class WindowPreview : public QObject {
    Q_OBJECT
public:
    explicit WindowPreview(QObject* parent = nullptr);
    ~WindowPreview() override;

    // target 默认是 HWND；monitor=true 时它是 HMONITOR（预览整块屏幕）。
    // 换目标就再调一次 begin（内部会先停掉上一路）。失败返回 false 并给出人话原因。
    bool begin(WId target, bool monitor, QString* err);
    void stop();
    bool active() const { return m_active; }

    // 编码档变了不必重开这一路：下一帧直接按新档缩
    void setMaxWidth(int cap) { m_maxWidth = cap; }

    int framesSeen() const { return m_frames; }
    QSize sourceSize() const { return m_source; }     // 采集项自己的尺寸（物理像素）
    QSize shownSize() const { return m_shown; }       // 缩完摆出来的尺寸 = Recorder::encodedSize(源, 档)
    QPixmap frame() const { return m_frame; }
    QString lastError() const { return m_err; }

signals:
    void frameReady();

private:
    void pump();

    struct Priv;
    Priv* m_priv = nullptr;
    QTimer* m_poll = nullptr;
    bool m_active = false;
    WId m_target = 0;                 // 开起来那一路的目标，尺寸变了拿它重开一次
    bool m_monitor = false;
    int m_maxWidth = 0;
    int m_frames = 0;
    int m_emptyPumps = 0;
    QSize m_source;
    QSize m_shown;
    QPixmap m_frame;
    QString m_err;
};

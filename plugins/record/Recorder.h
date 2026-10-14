#pragma once

#include <QObject>
#include <QSize>
#include <QString>
#include <QWindow>   // WId

#include <memory>

// F2 录屏：录某一个软件的页面，或者整块屏幕。
//
// 取帧用的是 Windows 11 自带的 Windows.Graphics.Capture（拿的是那个窗口/那块屏幕自己的
// 合成帧，所以别的程序盖在窗口上面也不会被录进去），编码用的是系统 Media Foundation 的
// H.264，产物是普通 .mp4 —— 都不往下拉任何外部 DLL/EXE。
// 实测（本机 2026-10-08 --recprobe）：480x320 一帧拷到 CPU 256us，1280x720 编一帧 1.8ms。
//
// 采集/拷贝/编码全都在这一个工作线程里做：D3D11 的 immediate context 和 sink writer
// 都只认创建它们的线程，跨线程要开 DEBUG 层，不值得。
//
// 2026-10-15：整个录屏那一路（Recorder / RecordUi / RecProbe 和它们的判据）从 ltoolcore.dll
// 搬进了 plugins/record —— 主程序现在只认动作名 "record"，不再点名这个类，
// 所以这里从来没有也不需要 LTOOL_CORE_API。
class Recorder : public QObject {
    Q_OBJECT
public:
    struct Options {
        int fps = 15;              // 目标帧率（窗口没重绘时不会硬凑帧）
        int videoBitrateKbps = 8000;
        int maxWidth = 0;          // 0 = 录多大就编多大；>0 = 长边不超过这个数（等比缩，两边取偶）
    };

    // 采集尺寸 → 编码尺寸：等比缩到长边 <= maxWidth，然后两边各取偶
    // （H.264 奇数画幅会被编码器当场拒，实测 0xc00d36b4）。
    // 单独做成函数是给 --rectest 用的：文件里的画幅必须能用同一套算法先算出来再比。
    static QSize encodedSize(int fw, int fh, int maxWidth);
    // 码率 = 每像素每帧的 bit 数(bpp) × 画幅 × 帧率。**按编码后的画幅算**，
    // 所以"缩到 1280"就真的少花一半以上的空间，不会出现"画面小了码率照旧"的白花钱。
    // 界面那四档和 --recq 的对照录复制用这一个函数（不许有两份算法）。
    static int bitrateKbps(double bpp, int w, int h, int fps);

    explicit Recorder(QObject* parent = nullptr);
    ~Recorder() override;

    // 这台机器能不能录（缺编码器/被策略挡住时会给出人话原因）
    static bool available(QString* why = nullptr);

    // 把自己写出来的 mp4 读回来数一遍（帧数/时长/有没有音轨）——
    // "文件存在"不算证据，能解码出 N 帧才算录到了。
    struct Mp4Info {
        bool ok = false;
        qint64 frames = 0;
        qint64 durationMs = 0;
        qint64 bytes = 0;
        int w = 0, h = 0;         // 画幅：验"录整块屏幕时解出来的确实是那块屏幕的尺寸（取偶）"
        bool hasAudio = false;
        QString error;
    };
    static Mp4Info inspectMp4(const QString& path);

    // 把自家 mp4 解一帧出来，算 n×n 的亮度网格（0..255），用来判"画面朝向"。
    // 为什么解成 NV12 而不是 RGB32：YUV 的 Y 平面第 0 行就是画面上沿，没有 RGB
    // 那套"biHeight 为正 = bottom-up"的行序歧义 —— 拿 RGB 当判据会把编码端的错
    // 和解端的错互相抵消，看着像没问题（用户报的"录出来是倒的"必须能被这个抓到）。
    struct LumaGrid {
        bool ok = false;
        int w = 0;
        int h = 0;
        int n = 0;
        long pitch = 0;
        QVector<double> cells;   // n*n，行优先，第 0 行 = 画面上沿
        QString error;
        double at(int row, int col) const { return cells.size() < n * n ? 0.0 : cells[row * n + col]; }
    };
    static LumaGrid inspectLuma(const QString& path, int n);

    // target 是 Win32 句柄：默认 HWND（录那一个窗口）；monitorTarget=true 时它是 HMONITOR，
    // 录的是整块屏幕。两条路的差别只在采集项怎么建 + 屏幕那一路把鼠标指针也画进帧里
    // （窗口那一路从来不含指针，--rectest 的朝向断言靠的就是这个确定性）。
    bool start(WId target, const Options& opt, const QString& path, QString* err,
               bool monitorTarget = false);
    void stop();                      // 阻塞到工作线程收完尾（最多一秒左右）
    // 工作线程自己收尾了（窗口关了/编码器拒了）就不算在录 —— 否则 F2 会一直
    // 以为"还在录"而只去 raise 一条已经死掉的悬浮条（实测踩过）
    bool running() const;

    // 状态都由 UI 那边定时读，工作线程不直接回调 GUI —— 省掉"回调时对象还在不在"这一摊
    int framesCaptured() const;
    qint64 elapsedMs() const;         // 从采到第一帧起算
    bool isDone() const;              // 录完了（正常结束或出错），结果可以取了
    QString statusText() const;       // 一行实况，给悬浮条/日志
    QString errorText() const;        // isDone() 之后才有意义；空=顺利
    qint64 fileBytes() const;

private:
    struct Priv;
    std::shared_ptr<Priv> m_priv;
    bool m_running = false;
};

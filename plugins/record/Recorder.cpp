#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00
#ifdef NTDDI_VERSION
#undef NTDDI_VERSION
#endif
#define NTDDI_VERSION 0x0A000000
#ifdef WINVER
#undef WINVER
#endif
#define WINVER 0x0A00

#include "Recorder.h"

#include "Utils.h"

#include <windows.h>
#include <roapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <d3d11.h>
#include <dxgi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <Windows.Graphics.Capture.Interop.h>

#include <QThreadPool>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QFileInfo>
#include <mutex>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <vector>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgdd = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

QString why(const wchar_t* what, HRESULT hr)
{
    return QString::fromWCharArray(what) + QStringLiteral("（0x%1）")
        .arg(quint32(hr), 8, 16, QLatin1Char('0'));
}


} // namespace

struct Recorder::Priv {
    std::atomic<bool> stopRequested { false };
    std::atomic<bool> done { false };
    std::atomic<int> frames { 0 };
    std::atomic<qint64> bytes { 0 };
    std::atomic<qint64> startedMs { 0 };
    std::atomic<qint64> wallMs { 0 };   // worker 每轮刷新，悬浮条的计时就读它
    std::mutex mtx;
    QString error;             // 只在 worker 结束时写一次
    QString status;            // 一行实况，worker 写、GUI 读
};

Recorder::Recorder(QObject* parent)
    : QObject(parent), m_priv(std::make_shared<Priv>())
{
}

Recorder::~Recorder()
{
    if (m_running) {
        m_priv->stopRequested = true;
        for (int i = 0; i < 100 && !m_priv->done; ++i)
            Sleep(10);
    }
}

bool Recorder::available(QString* why2)
{
    static std::atomic<bool> mfStarted { false };
    if (!mfStarted.exchange(true))
        MFStartup(MF_VERSION);
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            if (why2)
                *why2 = QStringLiteral("这台机器不支持按窗口取帧（Windows.Graphics.Capture 被系统挡了）");
            return false;
        }
    } catch (winrt::hresult_error const& e) {
        if (why2)
            *why2 = QStringLiteral("问不到按窗口取帧的支持情况：%1")
                        .arg(QString::fromWCharArray(e.message().c_str()));
        return false;
    }
    // 编码器是运行期枚举的，别只查 DLL 在不在
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    const HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                 MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                                 nullptr, nullptr, &acts, &n);
    if (FAILED(hr)) {
        if (why2)
            *why2 = why(L"问不到系统编码器", hr);
        return false;
    }
    bool h264 = false;
    for (UINT32 i = 0; i < n; ++i) {
        WCHAR* name = nullptr;
        IMFAttributes* at = nullptr;
        if (SUCCEEDED(acts[i]->QueryInterface(IID_PPV_ARGS(&at)))) {
            if (SUCCEEDED(at->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, nullptr)) && name) {
                const QString s = QString::fromWCharArray(name).toLower();
                if (s.contains(QStringLiteral("h264")) || s.contains(QStringLiteral("h.264")))
                    h264 = true;
                CoTaskMemFree(name);
            }
            at->Release();
        }
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    if (!h264) {
        if (why2)
            *why2 = QStringLiteral("系统里没有 H.264 编码器，录不了（mfh264enc.dll 可能被精简过）");
        return false;
    }
    return true;
}

namespace {

// 面积平均缩小（不是隔行取样）：录屏里全是文字和 1px 边框，隔行取样一缩放就闪、就出摩尔纹。
// 每个目标像素取它对应那块源像素的平均；整数边界保证最后一块一定吃到底行/底列，
// 不会因为整除把右下角裁掉一圈。
void downscaleBGRA(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh)
{
    for (int y = 0; y < dh; ++y) {
        const int sy0 = int(qint64(y) * sh / dh);
        int sy1 = int(qint64(y + 1) * sh / dh);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        for (int x = 0; x < dw; ++x) {
            const int sx0 = int(qint64(x) * sw / dw);
            int sx1 = int(qint64(x + 1) * sw / dw);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            unsigned b = 0, g = 0, r = 0, a = 0;
            for (int yy = sy0; yy < sy1; ++yy) {
                const uint8_t* row = src + (size_t(yy) * sw + sx0) * 4;
                for (int xx = 0; xx < sx1 - sx0; ++xx) {
                    b += row[xx * 4 + 0];
                    g += row[xx * 4 + 1];
                    r += row[xx * 4 + 2];
                    a += row[xx * 4 + 3];
                }
            }
            const unsigned n = unsigned(sy1 - sy0) * unsigned(sx1 - sx0);
            uint8_t* o = dst + (size_t(y) * dw + x) * 4;
            o[0] = uint8_t(b / n);
            o[1] = uint8_t(g / n);
            o[2] = uint8_t(r / n);
            o[3] = uint8_t(a / n);
        }
    }
}

} // namespace

int Recorder::bitrateKbps(double bpp, int w, int h, int fps)
{
    // 下限 400：再低的话小窗口录出来的东西糊得没法看；上限 60Mbps 是防手滑
    return qBound(400, int(bpp * double(w) * double(h) * double(qMax(1, fps)) / 1000.0), 60000);
}

QSize Recorder::encodedSize(int fw, int fh, int maxWidth)
{
    // 先按 H.264 的规矩取偶，再等比缩到长边不超过 maxWidth（**只缩不放大**），最后再取一次偶
    int w = fw - fw % 2;
    int h = fh - fh % 2;
    if (maxWidth > 0 && w > maxWidth) {
        h = int(qint64(h) * maxWidth / w);
        w = maxWidth;
    }
    w -= w % 2;
    h -= h % 2;
    return QSize(qMax(2, w), qMax(2, h));
}

bool Recorder::start(WId target, const Options& opt, const QString& path, QString* err,
                     bool monitorTarget)
{
    // 只看 running()，不看 m_running：上一局是工作线程自己收尾的（编码器拒了/窗口关了/
    // 改了大小）时 m_running 还停在 true，拿它当闸门就等于"失败一次 F2 就永久失灵"。
    if (running()) {
        if (err)
            *err = QStringLiteral("已经在录了");
        return false;
    }
    m_running = false;   // 上一局已经 done，把它彻底清掉再开新的一局
    QString why2;
    if (!available(&why2)) {
        if (err)
            *err = why2;
        return false;
    }
    if (!target) {
        if (err)
            *err = monitorTarget ? QStringLiteral("没给要录的屏幕")
                                 : QStringLiteral("没给要录的窗口");
        return false;
    }

    m_priv->stopRequested = false;
    m_priv->done = false;
    m_priv->frames = 0;
    m_priv->bytes = 0;
    m_priv->error.clear();
    {
        std::lock_guard<std::mutex> lk(m_priv->mtx);
        m_priv->status = QStringLiteral("正在开始…");
    }
    m_running = true;

    const Options o = opt;
    const QString outPath = path;
    const bool asMonitor = monitorTarget;
    auto priv = m_priv;
    // 用 QThreadPool 直接投任务，不用 QtConcurrent::run：后者返回的 QFuture 我一概不接
    // （以前是丢掉它，MSVC 也警告过 C4858），而 Qt 会把被丢弃的 QFuture 的内部接口连同
    // 它的锁/条件变量留在原地 —— 实测每录一次句柄净涨 2~4 个（--memloop 6 轮：384→604，
    // 其中解压那几轮全是 +0，涨的都在录制轮）。这里不需要结果，所以根本不该有 future。
    QThreadPool::globalInstance()->start([priv, target, asMonitor, o, outPath] {
        // ---- 采集 + 编码全在这一个线程里：D3D11 的 immediate context 和 sink writer
        //      都只认创建它的线程 ----
        QString fail;
        int frames = 0;
        QElapsedTimer wall;
        wall.start();
        const HRESULT ro = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(ro) && ro != RPC_E_CHANGED_MODE && ro != S_FALSE)
            fail = why(L"这个线程初始化 WinRT 失败", ro);
        if (fail.isEmpty() && FAILED(MFStartup(MF_VERSION)))
            fail = QStringLiteral("MFStartup 失败");

        winrt::com_ptr<ID3D11Device> d3d;
        winrt::com_ptr<ID3D11DeviceContext> ctx;
        wgdd::IDirect3DDevice device{nullptr};
        wgc::GraphicsCaptureItem item { nullptr };
        wgc::Direct3D11CaptureFramePool pool { nullptr };
        wgc::GraphicsCaptureSession session { nullptr };
        winrt::com_ptr<IMFSinkWriter> writer;
        winrt::com_ptr<ID3D11Texture2D> staging;
        std::vector<uint8_t> scratch;
        std::vector<uint8_t> encBuf;   // 缩放后的待编码帧（没选缩小就空着）

        int frameErrs = 0;
        UINT baseW = 0, baseH = 0;   // 开局量到的窗口矩形（看门狗用它比，不用帧尺寸）
        const HWND hwndTarget = asMonitor ? nullptr : reinterpret_cast<HWND>(target);
        const HMONITOR monTarget = asMonitor ? reinterpret_cast<HMONITOR>(target) : nullptr;
        UINT W = 0, H = 0;      // 喂给编码器的尺寸（偶数，可能已经按比例缩过）
        UINT SW = 0, SH = 0;    // 从纹理拷出来的尺寸（画面取偶；没选缩小就和 W/H 一样）
        UINT FW = 0, FH = 0;    // 帧的真实尺寸（可能是奇数）
        LONGLONG t0Ms = 0, lastTime100ns = 0;
        qint64 nextOkMs = 0;   // 帧率封顶：这会儿还不该要下一帧就别要
        DWORD stream = 0;

        if (fail.isEmpty()) {
            const HRESULT dhr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                  D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                                  D3D11_SDK_VERSION, d3d.put(), nullptr, ctx.put());
            if (FAILED(dhr))
                fail = why(L"建不出 D3D 设备", dhr);
        }
        if (fail.isEmpty()) {
            const auto dxgi = d3d.as<IDXGIDevice>();
            winrt::com_ptr<::IInspectable> insp;
            const HRESULT ehr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
            if (FAILED(ehr))
                fail = why(L"D3D 设备转不成 WinRT 设备", ehr);
            else
                device = insp.as<wgdd::IDirect3DDevice>();
        }
        if (fail.isEmpty()) {
            try {
                auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                              IGraphicsCaptureItemInterop>();
                winrt::check_hresult(asMonitor
                                         ? interop->CreateForMonitor(monTarget,
                                                                     winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                                     winrt::put_abi(item))
                                         : interop->CreateForWindow(reinterpret_cast<HWND>(target),
                                                                    winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                                    winrt::put_abi(item)));
                FW = UINT(item.Size().Width);
                FH = UINT(item.Size().Height);
                // H.264 只收偶数宽高：录 MAA 那种奇数尺寸的窗口就是死在 SetInputMediaType
                // （0xc00d36b4，实测 1280x800/1920x1080/2560x1440 都好，只有奇数挂）。
                // 往下取偶数，多出来的那一像素不录 —— 但比对帧尺寸必须用原始值。
                SW = FW - FW % 2;     // 从纹理拷进 scratch 的尺寸（就是画面大小，取偶）
                SH = FH - FH % 2;
                // 编码尺寸：用户选了"长边不超过 N"就在这里等比缩小，没选就等于 SW/SH
                {
                    const QSize enc = encodedSize(int(FW), int(FH), int(o.maxWidth));
                    W = UINT(enc.width());
                    H = UINT(enc.height());
                }
                const QString what = asMonitor ? QStringLiteral("屏幕") : QStringLiteral("窗口");
                if (W < 16 || H < 16) {
                    fail = QStringLiteral("这个%1太小（%2x%3），没什么可录").arg(what).arg(FW).arg(FH);
                }
            } catch (winrt::hresult_error const& e) {
                fail = QStringLiteral("%1录不了：%2")
                           .arg(asMonitor ? QStringLiteral("这块屏幕") : QStringLiteral("这个窗口"),
                                QString::fromWCharArray(e.message().c_str()));
            }
        }
        // 帧格式是 B8G8R8A8，直接当 RGB32 喂编码器，不用转色
        auto makeStaging = [&] {
            D3D11_TEXTURE2D_DESC sd {};
            sd.Width = FW;
            sd.Height = FH;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging = nullptr;
            return SUCCEEDED(d3d->CreateTexture2D(&sd, nullptr, staging.put()));
        };
        if (fail.isEmpty() && !makeStaging())
            fail = QStringLiteral("建不出中转纹理");

        if (fail.isEmpty()) {
            winrt::com_ptr<IMFAttributes> attrs;
            winrt::com_ptr<IMFMediaType> outType, inType;
            MFCreateAttributes(attrs.put(), 2);
            attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
            const std::wstring wpath = outPath.toStdWString();
            const HRESULT whr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attrs.get(),
                                                          writer.put());
            if (FAILED(whr)) {
                fail = why(L"建不出 MP4 写入器", whr);
            } else {
                MFCreateMediaType(outType.put());
                outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
                outType->SetUINT32(MF_MT_AVG_BITRATE, DWORD(o.videoBitrateKbps * 1000));
                outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                outType->SetUINT32(MF_MT_MPEG2_PROFILE, 77 /* Main */);
                MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, W, H);
                MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE, DWORD(o.fps), 1);
                MFSetAttributeRatio(outType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                const HRESULT adr = writer->AddStream(outType.get(), &stream);
                if (FAILED(adr)) {
                    fail = why(L"H.264 流建不出来", adr);
                } else {
                    MFCreateMediaType(inType.put());
                    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                    MFSetAttributeSize(inType.get(), MF_MT_FRAME_SIZE, W, H);
                    MFSetAttributeRatio(inType.get(), MF_MT_FRAME_RATE, DWORD(o.fps), 1);
                    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                    inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
                    // stride 声明成正值 (W*4)：实测这样 MF 就按"缓冲区第 0 行是画面上沿"来编码。
                    // 以前写的是 -(W*4)，录出来的 mp4 整个上下颠倒（播放器和解码器都一致显示反的，
                    // 见下面写帧那段的注释 + --rectest 的朝向断言）。
                    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, DWORD(W * 4));
                    const HRESULT ihr = writer->SetInputMediaType(stream, inType.get(), nullptr);
                    if (FAILED(ihr))
                        fail = why(L"编码器不吃 RGB32", ihr);
                    else
                        writer->BeginWriting();
                }
            }
        }
        if (fail.isEmpty()) {
            scratch.assign(size_t(SW) * SH * 4, 0);
            // 选了缩小才要这个缓冲；没缩就直接把 scratch 交给编码器（少一次整帧拷贝）
            encBuf.clear();
            if (W != SW || H != SH)
                encBuf.assign(size_t(W) * H * 4, 0);
            try {
                pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                    device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, item.Size());
                session = pool.CreateCaptureSession(item);
                session.IsBorderRequired(false);
                // 录整块屏幕时把鼠标指针也画进帧里（WGC 默认不含）。录窗口那一路保持不含：
                // --rectest 的朝向断言要的是"合成帧里没有外来像素"这个确定性。
                if (asMonitor)
                    session.IsCursorCaptureEnabled(true);
                session.StartCapture();
            } catch (winrt::hresult_error const& e) {
                fail = QStringLiteral("开始采集失败：%1")
                           .arg(QString::fromWCharArray(e.message().c_str()));
            }
        }
        if (fail.isEmpty()) {
            t0Ms = wall.elapsed();
            priv->startedMs = t0Ms;
            RECT base {};
            if (GetWindowRect(hwndTarget, &base)) {
                baseW = UINT(base.right - base.left);
                baseH = UINT(base.bottom - base.top);
            }
            while (!priv->stopRequested) {
                priv->wallMs = wall.elapsed();
                wgc::Direct3D11CaptureFrame frame { nullptr };
                HRESULT frhr = S_OK;
                try {
                    frame = pool.TryGetNextFrame();
                } catch (winrt::hresult_error const& e) {
                    frame = nullptr;
                    frhr = e.code();
                }
                if (!frame) {
                    // 目标窗口消失、被最小化、或者改了大小：WGC 在这种情况下**什么都不给**
                    // （不报错也不发新帧），以前悬浮条会继续涨秒但一帧不录 —— 那是骗人。
                    // 下面这几条都是"窗口才会犯的错"，录整块屏幕时不适用（hwndTarget 是空的，
                    // 而 IsWindow(nullptr) 返回 FALSE，不挡住的话一开录就自己判定"目标被关了"）。
                    if (hwndTarget && (!IsWindow(hwndTarget) || IsIconic(hwndTarget))) {
                        fail = IsWindow(hwndTarget)
                            ? QStringLiteral("目标窗口最小化了，录不下去；已录的都保留在文件里")
                            : QStringLiteral("目标窗口被关了；已录的都保留在文件里");
                        break;
                    }
                    if (FAILED(frhr) && ++frameErrs > 30) {   // 连续 ~150ms 取帧都报错：多半是改了大小
                        fail = asMonitor
                            ? QStringLiteral("这块屏幕变了（常见于改分辨率/切显示模式），编码器那一路过不去；"
                                             "已录的都保留在文件里")
                            : QStringLiteral("目标窗口变了（常见于改大小/全屏切换），编码器那一路过不去；"
                                             "已录的都保留在文件里");
                        break;
                    }
                    RECT now {};
                    if (hwndTarget && GetWindowRect(hwndTarget, &now)
                        && baseW && (qAbs((now.right - now.left) - int(baseW)) > 2
                            || qAbs((now.bottom - now.top) - int(baseH)) > 2)) {
                        fail = QStringLiteral("目标窗口改了大小（%1x%2 → %3x%4），MP4 的画幅改不了，"
                                              "这一局到此为止；已录下的都在文件里")
                                   .arg(baseW).arg(baseH).arg(now.right - now.left).arg(now.bottom - now.top);
                        break;
                    }
                    // 窗口没重绘就没有新帧，这是正常的（静态页面尤其如此），别当故障
                    Sleep(4);
                    if (wall.elapsed() - priv->startedMs > 500) {
                        std::lock_guard<std::mutex> lk(priv->mtx);
                        priv->status = QStringLiteral("已录 %1 秒 / %2 帧")
                                           .arg((wall.elapsed() - priv->startedMs) / 1000).arg(frames);
                    }
                    continue;
                }
                // 帧率封顶：窗口自己 60fps 重绘时不能照着写，不然文件能大四五倍，
                // 而播放器也只会把多出来的帧丢掉
                const qint64 nowMs = wall.elapsed();
                if (nextOkMs && nowMs < nextOkMs)
                    continue;
                nextOkMs = nowMs + qMax(1, 1000 / std::max(1, o.fps));
                const auto desc = frame.Surface().Description();
                if (UINT(desc.Width) != FW || UINT(desc.Height) != FH) {
                    // 中途改尺寸：MP4 的流尺寸是开录时定死的，只能收尾
                    fail = QStringLiteral("%1（%2x%3 → %4x%5），这一局到此为止；"
                                          "已经录下的部分都在文件里")
                               .arg(asMonitor ? QStringLiteral("屏幕分辨率变了")
                                              : QStringLiteral("窗口改了大小"))
                               .arg(FW).arg(FH).arg(desc.Width).arg(desc.Height);
                    break;
                }
                bool copied = false;
                try {
                    const auto access = frame.Surface().as<
                        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    winrt::com_ptr<ID3D11Texture2D> tex;
                    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),
                                                              tex.put_void()));
                    ctx->CopyResource(staging.get(), tex.get());
                    D3D11_MAPPED_SUBRESOURCE map {};
                    winrt::check_hresult(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map));
                    const BYTE* src = reinterpret_cast<const BYTE*>(map.pData);
                    // 纹理第 0 行 = 画面上沿，直接照顺序拷进缓冲，配合上面声明的"正 stride"，
                    // 录出来的就是正的。这里踩过一次：以前给的是 MF_MT_DEFAULT_STRIDE = -(W*4)，
                    // 实测那样 MF 会把整幅上下翻一遍再编（录完播放是倒的）。用"上半白/下半黑"的
                    // 合成帧单独验过编码器认哪种约定，4 组数据都只能由"正 stride + 直拷"解释。
                    for (UINT y = 0; y < SH; ++y)   // 奇数那多出来的一行/列直接不要
                        memcpy(scratch.data() + size_t(y) * SW * 4,
                               src + size_t(y) * map.RowPitch, size_t(SW) * 4);
                    ctx->Unmap(staging.get(), 0);
                    copied = true;
                } catch (winrt::hresult_error const&) {
                    copied = false;
                }
                if (!copied)
                    continue;
                winrt::com_ptr<IMFSample> sample;
                winrt::com_ptr<IMFMediaBuffer> buf;
                const uint8_t* pixels = scratch.data();
                size_t pixelBytes = scratch.size();
                if (!encBuf.empty()) {          // 选了缩小：面积平均那一遍在这儿做
                    downscaleBGRA(scratch.data(), int(SW), int(SH),
                                  encBuf.data(), int(W), int(H));
                    pixels = encBuf.data();
                    pixelBytes = encBuf.size();
                }
                MFCreateSample(sample.put());
                MFCreateAlignedMemoryBuffer(DWORD(W * H * 4), DWORD(31), buf.put());
                BYTE* data = nullptr;
                DWORD maxLen = 0;
                if (FAILED(buf->Lock(&data, &maxLen, nullptr)))
                    break;
                memcpy(data, pixels, std::min<size_t>(maxLen, pixelBytes));
                buf->Unlock();
                buf->SetCurrentLength(DWORD(W * H * 4));
                sample->AddBuffer(buf.get());
                // 时间戳按真实到达时间走，一帧的 duration 就是它被显示的时长：
                // 窗口静止时 WGC 不给帧，播放器会自己把上一帧按住，不用我们凑帧。
                const LONGLONG now100 = LONGLONG(wall.elapsed() - t0Ms) * 10000;
                const LONGLONG dur = lastTime100ns ? (now100 - lastTime100ns) : LONGLONG(200000);
                lastTime100ns = now100;
                sample->SetSampleTime(now100);
                sample->SetSampleDuration(dur);
                if (FAILED(writer->WriteSample(stream, sample.get()))) {
                    fail = QStringLiteral("写帧失败，停在第 %1 帧").arg(frames + 1);
                    break;
                }
                frameErrs = 0;
                ++frames;
                priv->frames = frames;
            }
            try {
                session.Close();
                pool.Close();
            } catch (winrt::hresult_error const&) {
            }
        }
        if (writer) {
            writer->Finalize();
            writer = nullptr;
        }
        const QFileInfo fi(outPath);
        priv->bytes = fi.exists() ? fi.size() : 0;
        if (fail.isEmpty() && frames == 0)
            fail = asMonitor ? QStringLiteral("一帧都没采到（这块屏幕一直没变化，动一下鼠标就有了）")
                             : QStringLiteral("一帧都没采到（这个窗口可能一直没重绘）");
        {
            std::lock_guard<std::mutex> lk(priv->mtx);
            priv->error = fail;
            priv->status = fail.isEmpty() ? QStringLiteral("完成") : fail;
        }
        Utils::log(QString("record done: path=%1 frames=%2 bytes=%3 err=%4")
                       .arg(outPath).arg(frames).arg(priv->bytes.load()).arg(fail));
        priv->done = true;
        MFShutdown();
        if (ro == S_OK)
            RoUninitialize();
    });
    return true;
}

bool Recorder::running() const
{
    // done 是工作线程自己置的：编码器拒了/目标窗口没了都会自己结束，
    // 这时候必须让外面知道"没在录了"，不然 F2 就再也按不动了
    return m_running && !m_priv->done.load();
}

void Recorder::stop()
{
    if (!m_running)
        return;
    m_priv->stopRequested = true;
    for (int i = 0; i < 200 && !m_priv->done; ++i)
        Sleep(5);
    m_running = false;
}

Recorder::Mp4Info Recorder::inspectMp4(const QString& path)
{
    Mp4Info info;
    info.bytes = QFileInfo(path).size();
    const std::wstring wpath = path.toStdWString();
    winrt::com_ptr<IMFSourceReader> reader;
    const HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, reader.put());
    if (FAILED(hr)) {
        info.error = QStringLiteral("读不回来: 0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
        return info;
    }
    winrt::com_ptr<IMFMediaType> audio;
    info.hasAudio = SUCCEEDED(reader->GetNativeMediaType(
        DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, audio.put()));
    winrt::com_ptr<IMFMediaType> vtype;
    if (SUCCEEDED(reader->GetNativeMediaType(
            DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, vtype.put()))) {
        UINT32 vw = 0, vh = 0;
        if (SUCCEEDED(MFGetAttributeSize(vtype.get(), MF_MT_FRAME_SIZE, &vw, &vh))) {
            info.w = int(vw);
            info.h = int(vh);
        }
    }
    LONGLONG last = 0;
    for (;;) {
        winrt::com_ptr<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG ts = 0;
        if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags,
                                      &ts, sample.put()))) {
            info.error = QStringLiteral("解到第 %1 帧时失败").arg(info.frames);
            return info;
        }
        if (ts > last)
            last = ts;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (sample)
            ++info.frames;
        if (info.frames > 100000)
            break;
    }
    info.durationMs = last / 10000;
    info.ok = info.frames > 0;
    return info;
}

Recorder::LumaGrid Recorder::inspectLuma(const QString& path, int n)
{
    LumaGrid g;
    g.n = qBound(2, n, 32);
    const std::wstring wpath = path.toStdWString();
    winrt::com_ptr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, reader.put()))) {
        g.error = QStringLiteral("读不回来");
        return g;
    }
    winrt::com_ptr<IMFMediaType> want;
    if (FAILED(MFCreateMediaType(want.put())))
        return g;
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    const HRESULT setHr = reader->SetCurrentMediaType(
        DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, want.get());
    if (FAILED(setHr)) {
        g.error = QStringLiteral("解不成 NV12: 0x%1")
                      .arg(quint32(setHr), 8, 16, QLatin1Char('0'));
        return g;
    }
    winrt::com_ptr<IMFMediaType> got;
    reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), got.put());
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(got.get(), MF_MT_FRAME_SIZE, &w, &h);
    if (!w || !h) {
        g.error = QStringLiteral("问不到帧尺寸");
        return g;
    }
    g.w = int(w);
    g.h = int(h);

    winrt::com_ptr<IMFSample> pick;
    for (int i = 0; i < 3; ++i) {          // 取第二帧：第一帧有时是黑场
        winrt::com_ptr<IMFSample> sample;
        DWORD flags = 0;
        if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr,
                                      &flags, nullptr, sample.put())))
            break;
        if (sample)
            pick = sample;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
    }
    if (!pick) {
        g.error = QStringLiteral("一帧都没解出来");
        return g;
    }
    winrt::com_ptr<IMFMediaBuffer> mb;
    if (FAILED(pick->GetBufferByIndex(0, mb.put()))) {
        g.error = QStringLiteral("样本里没有缓冲");
        return g;
    }
    winrt::com_ptr<IMF2DBuffer> b2;
    if (FAILED(mb->QueryInterface(__uuidof(IMF2DBuffer), b2.put_void()))) {
        g.error = QStringLiteral("这块缓冲不支持 Lock2D");
        return g;
    }
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    if (FAILED(b2->Lock2D(&scan0, &pitch))) {
        g.error = QStringLiteral("Lock2D 失败");
        return g;
    }
    g.pitch = long(pitch);
    // 负的 pitch 意思是 scan0 指的是最后一行；这一路是解码器给的 NV12，真遇到就说明
    // 我的假设错了，直接把符号报出来，不在这里悄悄翻一遍把事情搅浑
    const bool flippedPitch = pitch < 0;
    const LONG absPitch = flippedPitch ? -pitch : pitch;
    const BYTE* plane = flippedPitch ? scan0 + pitch * (g.h - 1) : scan0;
    g.cells.assign(size_t(g.n) * g.n, 0.0);
    for (int r = 0; r < g.n; ++r) {
        const int y0 = int(int64_t(r) * g.h / g.n);
        const int y1 = qMax(y0 + 1, int(int64_t(r + 1) * g.h / g.n));
        for (int c = 0; c < g.n; ++c) {
            const int x0 = int(int64_t(c) * g.w / g.n);
            const int x1 = qMax(x0 + 1, int(int64_t(c + 1) * g.w / g.n));
            double sum = 0.0;
            long cnt = 0;
            for (int y = y0; y < qMin(y1, g.h); ++y) {
                const BYTE* rowp = plane + size_t(y) * absPitch;
                for (int x = x0; x < qMin(x1, g.w); ++x) {
                    sum += rowp[x];
                    ++cnt;
                }
            }
            g.cells[size_t(r) * g.n + c] = cnt ? sum / cnt : 0.0;
        }
    }
    b2->Unlock2D();
    g.ok = true;
    return g;
}

int Recorder::framesCaptured() const
{
    return m_priv->frames.load();
}

qint64 Recorder::elapsedMs() const
{
    const qint64 start = m_priv->startedMs.load();
    return start ? m_priv->wallMs.load() - start : 0;
}

bool Recorder::isDone() const
{
    return m_priv->done.load();
}

QString Recorder::errorText() const
{
    QMutexLocker lk(&m_priv->mtx);
    return m_priv->error;
}

qint64 Recorder::fileBytes() const
{
    return m_priv->bytes.load();
}

QString Recorder::statusText() const
{
    std::lock_guard<std::mutex> lk(m_priv->mtx);
    return m_priv->status;
}

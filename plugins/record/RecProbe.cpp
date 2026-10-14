// MF 这些接口里 Lock2D / SetBufferByIndex / EndWriting / MFT_ENUM_CONFIG 全都在
// `#if (NTDDI_VERSION >= NTDDI_WIN8)` 后面；Qt/CMake 传进来的 _WIN32_WINNT 是 0x0601，
// 不顶高就直接报"不是 IMFMediaBuffer 的成员"，看着像 SDK 缺文件。所以这段必须在所有 #include 之前。
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

#include "RecProbe.h"

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

#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QPalette>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <vector>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgdd = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

QString hresult(const wchar_t* what, HRESULT hr)
{
    return QString::fromStdWString(what)
        + QStringLiteral(": 0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

QString mftNames(REFGUID category)
{
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    const ULONG flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    const HRESULT hr = MFTEnumEx(category, flags, nullptr, nullptr, &acts, &n);
    if (FAILED(hr))
        return hresult(L"MFTEnumEx", hr);
    QStringList names;
    for (UINT32 i = 0; i < n; ++i) {
        WCHAR* name = nullptr;
        IMFAttributes* at = nullptr;
        if (SUCCEEDED(acts[i]->QueryInterface(IID_PPV_ARGS(&at)))) {
            if (SUCCEEDED(at->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, nullptr)) && name) {
                names << QString::fromWCharArray(name);
                CoTaskMemFree(name);
            }
            at->Release();
        }
        acts[i]->Release();
    }
    names.sort();
    names.removeDuplicates();
    return QStringLiteral("%1 个: %2").arg(n).arg(names.join(QStringLiteral(" | ")).left(400));
}

// ---- 1) MF 能不能写 MP4/H.264：现场编 30 帧再读回来数帧 ----
QString encoderPathTest(QString& err)
{
    Q_UNUSED(err);
    const int W = 1280, H = 720, fps = 15, frames = 30;
    QTemporaryDir dir;
    if (!dir.isValid())
        return QStringLiteral("临时目录建不出来");
    const QString path = dir.path() + "/probe.mp4";
    const std::wstring wpath = path.toStdWString();

    winrt::com_ptr<IMFSinkWriter> writer;
    winrt::com_ptr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(attrs.put(), 3);
    if (FAILED(hr))
        return hresult(L"MFCreateAttributes", hr);
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    attrs->SetUINT32(MF_LOW_LATENCY, FALSE);
    hr = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attrs.get(), writer.put());
    if (FAILED(hr))
        return hresult(L"MFCreateSinkWriterFromURL(mp4)", hr);

    winrt::com_ptr<IMFMediaType> in;
    MFCreateMediaType(in.put());
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, DWORD(LONG(-(W * 4))));   // 负 stride = 从上到下，和 BitBlt 拿到的顺序一致
    in->SetUINT32(MF_MT_AVG_BITRATE, 6'000'000);
    winrt::com_ptr<IMFMediaType> outType;
    MFCreateMediaType(outType.put());
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, 6000000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, 66 /* eAVEncH264VProfile_Baseline */);
    MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(outType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    hr = writer->AddStream(outType.get(), &stream);
    if (FAILED(hr))
        return hresult(L"AddStream(输出=H.264)", hr);
    hr = writer->SetInputMediaType(stream, in.get(), nullptr);
    if (FAILED(hr))
        return hresult(L"SetInputMediaType(输入=RGB32)", hr);
    writer->BeginWriting();

    std::vector<uint8_t> pixels(size_t(W) * H * 4);
    QElapsedTimer enc;
    enc.start();
    for (int f = 0; f < frames; ++f) {
        for (int y = 0; y < H; ++y) {
            uint8_t* row = pixels.data() + size_t(y) * W * 4;
            for (int x = 0; x < W; ++x) {
                const int v = (x + y + f * 8) & 0xFF;
                row[x * 4 + 0] = uint8_t(v);
                row[x * 4 + 1] = uint8_t(255 - v);
                row[x * 4 + 2] = uint8_t((x ^ y) & 0xFF);
                row[x * 4 + 3] = 0xFF;
            }
        }
        winrt::com_ptr<IMFSample> sample;
        winrt::com_ptr<IMFMediaBuffer> buf;
        MFCreateSample(sample.put());
        // 用普通对齐缓冲（MFCreate2DMediaBuffer 在这里返回失败，别跟它较劲）：
        // RGB32 负 stride 就是从上到下，整块 memcpy 即可。
        if (FAILED(MFCreateAlignedMemoryBuffer(DWORD(W * H * 4), DWORD(31), buf.put())))
            return QStringLiteral("MFCreateAlignedMemoryBuffer 失败");
        BYTE* data = nullptr;
        DWORD maxLen = 0;
        if (FAILED(buf->Lock(&data, &maxLen, nullptr)))
            return QStringLiteral("Lock 失败");
        memcpy(data, pixels.data(), size_t(W) * H * 4);
        buf->Unlock();
        buf->SetCurrentLength(DWORD(W * H * 4));
        sample->AddBuffer(buf.get());
        sample->SetSampleTime(LONGLONG(10'000'000LL * f / fps));
        sample->SetSampleDuration(LONGLONG(10'000'000LL / fps));
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        hr = writer->WriteSample(stream, sample.get());
        if (FAILED(hr))
            return hresult(L"WriteSample", hr);
    }
    writer->Finalize();
    const qint64 encodeMs = enc.elapsed();

    QFile out(path);
    const qint64 bytes = out.exists() ? out.size() : -1;

    // 读回来数一遍：写出去的东西自己当观众验一次
    winrt::com_ptr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, reader.put());
    if (FAILED(hr))
        return QStringLiteral("写出来了(%1 字节)但读不回来: %2").arg(bytes).arg(hresult(L"SourceReader", hr));
    int got = 0;
    LONGLONG lastTime = 0;
    for (;;) {
        winrt::com_ptr<IMFSample> sample;
        DWORD flags = 0;
        const HRESULT rh = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                              &flags, &lastTime, sample.put());
        if (FAILED(rh))
            return QStringLiteral("读样本失败 %1（已读到 %2 帧）").arg(hresult(L"ReadSample", rh)).arg(got);
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (sample)
            ++got;
        if (got > 200)
            break;
    }
    return QStringLiteral("编 %1 帧 %2x%3 用了 %4ms（平均 %5ms/帧），文件 %6 字节；读回来 %7 帧 / %8ms 时长")
        .arg(frames).arg(W).arg(H).arg(encodeMs).arg(encodeMs / frames).arg(bytes).arg(got)
        .arg(lastTime / 10000);
}

// ---- 2) 按窗口取帧：Windows.Graphics.Capture 在这台机器上到底能不能用 ----
QString windowCaptureTest(HWND hwnd, int observeMs, QString& err)
{
    Q_UNUSED(err);
    if (!wgc::GraphicsCaptureSession::IsSupported())
        return QStringLiteral("GraphicsCaptureSession::IsSupported() = false");

    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                   d3d.put(), nullptr, ctx.put());
    if (FAILED(hr))
        return hresult(L"D3D11CreateDevice", hr);
    const winrt::com_ptr<IDXGIDevice> dxgi = d3d.as<IDXGIDevice>();
    hr = dxgi ? S_OK : E_NOINTERFACE;
    if (FAILED(hr))
        return hresult(L"as<IDXGIDevice>", hr);
    winrt::com_ptr<::IInspectable> insp;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
    if (FAILED(hr))
        return hresult(L"CreateDirect3D11DeviceFromDXGIDevice", hr);
    const auto device = insp.as<wgdd::IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{nullptr};
    hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr))
        return hresult(L"CreateForWindow(按窗口取帧的入口)", hr);
    const auto size = item.Size();

    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    auto session = pool.CreateCaptureSession(item);
    try {
        session.IsBorderRequired(false);   // 不要那圈黄框（Win11 才有这个属性，老系统会抛）
    } catch (const winrt::hresult_error&) {
    }
    session.StartCapture();

    // 收帧：记录到达间隔 + 每帧拷到 CPU 的耗时（这就是喂给编码器前的真实成本）
    std::vector<qint64> arrivals;
    std::vector<qint64> copyUs;
    QElapsedTimer clock;
    clock.start();
    winrt::com_ptr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = UINT(size.Width);
    sd.Height = UINT(size.Height);
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const HRESULT stageHr = d3d->CreateTexture2D(&sd, nullptr, staging.put());

    QEventLoop loop;
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        auto frame = pool.TryGetNextFrame();
        if (!frame)
            return;
        arrivals.push_back(clock.elapsed());
        if (FAILED(stageHr))
            return;
        QElapsedTimer t;
        t.start();
        try {
            const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void()));
            ctx->CopyResource(staging.get(), tex.get());
            D3D11_MAPPED_SUBRESOURCE map{};
            winrt::check_hresult(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map));
            volatile BYTE first = *reinterpret_cast<BYTE*>(map.pData);
            (void)first;
            ctx->Unmap(staging.get(), 0);
            copyUs.push_back(t.nsecsElapsed() / 1000);
        } catch (const winrt::hresult_error&) {
        }
    });
    QTimer::singleShot(observeMs, &loop, &QEventLoop::quit);
    poll.start(5);
    loop.exec();
    session.Close();
    pool.Close();

    if (arrivals.empty())
        return QStringLiteral("StartCapture 成功但 %1ms 内一帧都没到（这个窗口可能没重绘）").arg(observeMs);
    std::vector<qint64> gaps;
    for (size_t i = 1; i < arrivals.size(); ++i)
        gaps.push_back(arrivals[i] - arrivals[i - 1]);
    std::sort(gaps.begin(), gaps.end());
    std::sort(copyUs.begin(), copyUs.end());
    const qint64 medGap = gaps.empty() ? -1 : gaps[gaps.size() / 2];
    const qint64 medCopy = copyUs.empty() ? -1 : copyUs[copyUs.size() / 2];
    const double px = double(size.Width) * double(size.Height);
    QString out = QStringLiteral("窗口 %1x%2：收到 %3 帧，中位间隔 %4ms(=%5 fps)，拷贝一帧中位 %6us")
                      .arg(size.Width).arg(size.Height).arg(arrivals.size()).arg(medGap)
                      .arg(medGap > 0 ? 1000.0 / double(medGap) : 0.0)
                      .arg(medCopy) + QStringLiteral("us");
    if (medCopy >= 0) {
        const double us1080 = double(medCopy) * (1920.0 * 1080.0 / px);
        out += QStringLiteral("；按像素线性推 1080p 拷一帧约 %1ms → 单这一项上限约 %2 fps")
                   .arg(us1080 / 1000.0, 0, 'f', 1).arg(1000000.0 / std::max(1.0, us1080), 0, 'f', 0);
    }
    return out;
}

} // namespace

namespace RecProbe {

QString run(int observeMs)
{
    QStringList rep;
    const HRESULT co = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE && co != S_FALSE)
        rep << hresult(L"RoInitialize", co);
    const HRESULT mf = MFStartup(MF_VERSION);
    rep << (SUCCEEDED(mf) ? QStringLiteral("MFStartup ok") : hresult(L"MFStartup", mf));

    rep << QStringLiteral("视频编码器 -> %1").arg(mftNames(MFT_CATEGORY_VIDEO_ENCODER));
    rep << QStringLiteral("音频编码器 -> %1").arg(mftNames(MFT_CATEGORY_AUDIO_ENCODER));

    QString err;
    rep << QStringLiteral("MP4/H.264 写一遍 -> %1").arg(encoderPathTest(err));

    // 探针自己的窗口：每 16ms 换个底色，保证一直在重绘（按窗口取帧只在你重绘时给帧）
    QWidget w;
    w.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    w.resize(480, 320);
    w.setWindowTitle(QStringLiteral("ltool 录屏探针"));
    w.show();
    QCoreApplication::processEvents();
    int tick = 0;
    QTimer anim;
    QObject::connect(&anim, &QTimer::timeout, &w, [&] {
        ++tick;
        w.setPalette(QPalette(QColor(tick % 2 ? 30 : 220, (tick * 37) % 256, (tick * 61) % 256)));
        w.update();
        QCoreApplication::processEvents();
    });
    anim.start(16);
    QCoreApplication::processEvents();
    rep << QStringLiteral("按窗口取帧 -> %1").arg(windowCaptureTest(reinterpret_cast<HWND>(w.winId()),
                                                                   observeMs, err));
    anim.stop();
    w.hide();

    MFShutdown();
    return rep.join(QStringLiteral("\n"));
}

} // namespace RecProbe

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

#include "WindowPreview.h"

#include "Recorder.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <Windows.Graphics.Capture.Interop.h>

#include <QImage>
#include <QTimer>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgdd = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace {

QString hr(const wchar_t* what, HRESULT h)
{
    return QString::fromWCharArray(what) + QStringLiteral("（0x%1）")
        .arg(quint32(h), 8, 16, QLatin1Char('0'));
}

constexpr int kPollMs = 100;
// 连续这么多拍一帧都没有就当这一路是哑的（那个窗口没重绘 / 被最小化了）。
// 界面要能说出这句话，不能拿一张旧图继续装作在预览。
constexpr int kQuietPumps = 20;

} // namespace

struct WindowPreview::Priv {
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::com_ptr<ID3D11Texture2D> staging;
    wgc::GraphicsCaptureItem item { nullptr };
    wgc::Direct3D11CaptureFramePool pool { nullptr };
    wgc::GraphicsCaptureSession session { nullptr };
    UINT w = 0;
    UINT h = 0;
};

WindowPreview::WindowPreview(QObject* parent)
    : QObject(parent), m_priv(new Priv)
{
    m_poll = new QTimer(this);
    m_poll->setTimerType(Qt::PreciseTimer);
    connect(m_poll, &QTimer::timeout, this, &WindowPreview::pump);
}

WindowPreview::~WindowPreview()
{
    stop();
    delete m_priv;
}

bool WindowPreview::begin(WId target, bool monitor, QString* err)
{
    stop();
    m_err.clear();
    m_frames = 0;
    m_emptyPumps = 0;
    m_frame = QPixmap();
    if (!wgc::GraphicsCaptureSession::IsSupported()) {
        m_err = QStringLiteral("这台机器不支持 Windows.Graphics.Capture");
        if (err)
            *err = m_err;
        return false;
    }
    QString fail;
    try {
        const HRESULT dhr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                              D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                              D3D11_SDK_VERSION, m_priv->d3d.put(), nullptr,
                                              m_priv->ctx.put());
        if (FAILED(dhr))
            fail = hr(L"建不出 D3D 设备", dhr);
    } catch (const winrt::hresult_error& e) {
        fail = QString::fromWCharArray(e.message().c_str());
    }
    wgdd::IDirect3DDevice device { nullptr };
    if (fail.isEmpty()) {
        try {
            const auto dxgi = m_priv->d3d.as<IDXGIDevice>();
            winrt::com_ptr<::IInspectable> insp;
            const HRESULT ehr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), insp.put());
            if (FAILED(ehr))
                fail = hr(L"D3D 设备转不成 WinRT 设备", ehr);
            else
                device = insp.as<wgdd::IDirect3DDevice>();
        } catch (const winrt::hresult_error& e) {
            fail = QString::fromWCharArray(e.message().c_str());
        }
    }
    if (fail.isEmpty()) {
        try {
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
                                                          IGraphicsCaptureItemInterop>();
            winrt::check_hresult(monitor
                                     ? interop->CreateForMonitor(reinterpret_cast<HMONITOR>(target),
                                                                 winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                                 winrt::put_abi(m_priv->item))
                                     : interop->CreateForWindow(reinterpret_cast<HWND>(target),
                                                                winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                                                winrt::put_abi(m_priv->item)));
            m_priv->w = UINT(m_priv->item.Size().Width);
            m_priv->h = UINT(m_priv->item.Size().Height);
            if (m_priv->w < 2 || m_priv->h < 2)
                fail = QStringLiteral("这个目标现在是 %1x%2，没什么可预览")
                           .arg(m_priv->w).arg(m_priv->h);
        } catch (const winrt::hresult_error& e) {
            fail = QStringLiteral("%1预览开不起来：%2")
                       .arg(monitor ? QStringLiteral("这块屏幕") : QStringLiteral("这个窗口"),
                            QString::fromWCharArray(e.message().c_str()));
        }
    }
    if (fail.isEmpty()) {
        D3D11_TEXTURE2D_DESC sd {};
        sd.Width = m_priv->w;
        sd.Height = m_priv->h;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        const HRESULT shr = m_priv->d3d->CreateTexture2D(&sd, nullptr, m_priv->staging.put());
        if (FAILED(shr))
            fail = hr(L"建不出中转纹理", shr);
    }
    if (fail.isEmpty()) {
        try {
            m_priv->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                device, wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_priv->item.Size());
            m_priv->session = m_priv->pool.CreateCaptureSession(m_priv->item);
            try {
                m_priv->session.IsBorderRequired(false);   // 不要那圈黄框（老系统会抛）
            } catch (const winrt::hresult_error&) {
            }
            m_priv->session.StartCapture();
        } catch (const winrt::hresult_error& e) {
            fail = QStringLiteral("取帧开不起来：%1").arg(QString::fromWCharArray(e.message().c_str()));
        }
    }
    if (!fail.isEmpty()) {
        stop();
        m_err = fail;
        if (err)
            *err = fail;
        return false;
    }
    m_source = QSize(int(m_priv->w), int(m_priv->h));
    m_shown = Recorder::encodedSize(int(m_priv->w), int(m_priv->h), m_maxWidth);
    m_target = target;
    m_monitor = monitor;
    m_active = true;
    m_poll->start(kPollMs);
    return true;
}

void WindowPreview::stop()
{
    m_poll->stop();
    m_active = false;
    try {
        if (m_priv->session)
            m_priv->session.Close();
        if (m_priv->pool)
            m_priv->pool.Close();
    } catch (const winrt::hresult_error&) {
    }
    m_priv->session = nullptr;
    m_priv->pool = nullptr;
    m_priv->item = nullptr;
    m_priv->staging = nullptr;
    m_priv->ctx = nullptr;
    m_priv->d3d = nullptr;
}

void WindowPreview::pump()
{
    if (!m_active)
        return;
    bool got = false;
    try {
        // 目标被关掉/最小化之后帧会停，也可能直接抛 —— 那都算这一路到头了
        for (int i = 0; i < 4; ++i) {     // 一次最多排掉 4 拍，别让人拖窗口时堆住主线程
            auto frame = m_priv->pool.TryGetNextFrame();
            if (!frame)
                break;
            const auto desc = frame.Surface().Description();   // 就是纹理那份描述：Width/Height 是字段
            if (UINT(desc.Width) != m_priv->w || UINT(desc.Height) != m_priv->h) {
                // 那个窗口被拉大了/拉小了：整路重开一次，尺寸和编码画幅才对得上
                QString err;
                begin(m_target, m_monitor, &err);
                return;
            }
            const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> tex;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void()));
            m_priv->ctx->CopyResource(m_priv->staging.get(), tex.get());
            D3D11_MAPPED_SUBRESOURCE map {};
            winrt::check_hresult(m_priv->ctx->Map(m_priv->staging.get(), 0, D3D11_MAP_READ, 0, &map));
            // 纹理就是 BGRA，QImage::Format_ARGB32 在小端上存的字节序正是 B,G,R,A
            QImage img(reinterpret_cast<uchar*>(map.pData), int(m_priv->w), int(m_priv->h),
                       int(map.RowPitch), QImage::Format_ARGB32);
            m_frame = QPixmap::fromImage(img.copy().scaled(
                Recorder::encodedSize(int(m_priv->w), int(m_priv->h), m_maxWidth),
                Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            m_priv->ctx->Unmap(m_priv->staging.get(), 0);
            m_shown = m_frame.size();
            ++m_frames;
            m_emptyPumps = 0;
            m_err.clear();
            got = true;
        }
    } catch (const winrt::hresult_error& e) {
        m_err = QStringLiteral("取帧断了：%1").arg(QString::fromWCharArray(e.message().c_str()));
        stop();
        emit frameReady();
        return;
    }
    if (!got && ++m_emptyPumps == kQuietPumps) {
        m_err = QStringLiteral("这一路开起来了，但 %1ms 内一帧都没到（那个窗口没重绘，或者被最小化了）")
                    .arg(kQuietPumps * kPollMs);
        emit frameReady();
        return;
    }
    if (got)
        emit frameReady();
}

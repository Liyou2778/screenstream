#include "capture.h"

#include <algorithm>
#include <thread>

namespace ss {

namespace {

// Releases the acquired DXGI frame no matter how we leave the scope.
struct FrameReleaser {
    IDXGIOutputDuplication* dup = nullptr;
    bool armed = false;
    ~FrameReleaser() {
        if (armed && dup != nullptr) {
            dup->ReleaseFrame();
        }
    }
};

} // namespace

DesktopCapture::~DesktopCapture() {
    Shutdown();
}

void DesktopCapture::Shutdown() {
    ReleaseDxgi();
    ReleaseGdi();
    mode_ = "none";
}

void DesktopCapture::ReleaseDxgi() {
    duplication_.Reset();
    staging_.Reset();
    staging_width_ = 0;
    staging_height_ = 0;
    context_.Reset();
    device_.Reset();
    dxgi_ok_ = false;
}

void DesktopCapture::ReleaseGdi() {
    if (mem_dc_ != nullptr) {
        if (old_dib_ != nullptr) {
            SelectObject(mem_dc_, old_dib_);
            old_dib_ = nullptr;
        }
        DeleteDC(mem_dc_);
        mem_dc_ = nullptr;
    }
    if (dib_ != nullptr) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    dib_bits_ = nullptr;
    if (screen_dc_ != nullptr) {
        ReleaseDC(nullptr, screen_dc_);
        screen_dc_ = nullptr;
    }
    gdi_width_ = 0;
    gdi_height_ = 0;
    gdi_ok_ = false;
}

bool DesktopCapture::Init() {
    last_error_.clear();
    if (InitDxgi()) {
        dxgi_ok_ = true;
        UpdateModeString();
        return true;
    }
    Log("DXGI Desktop Duplication unavailable: %s", last_error_.c_str());
    if (InitGdi()) {
        UpdateModeString();
        return true;
    }
    Log("GDI BitBlt fallback also unavailable: %s", last_error_.c_str());
    return false;
}

void DesktopCapture::UpdateModeString() {
    if (dxgi_ok_) {
        mode_ = "dxgi";
    } else if (gdi_ok_) {
        mode_ = "gdi";
    } else {
        mode_ = "none";
    }
}

bool DesktopCapture::InitDxgi() {
    ReleaseDxgi();

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_9_1;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   _countof(levels), D3D11_SDK_VERSION, &device_, &obtained, &context_);
    if (FAILED(hr)) {
        last_error_ = Format("D3D11CreateDevice failed: %s", HrToString(hr).c_str());
        ReleaseDxgi();
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    hr = device_.As(&dxgi_device);
    if (FAILED(hr)) {
        last_error_ = Format("ID3D11Device->IDXGIDevice failed: %s", HrToString(hr).c_str());
        ReleaseDxgi();
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        last_error_ = Format("IDXGIDevice::GetAdapter failed: %s", HrToString(hr).c_str());
        ReleaseDxgi();
        return false;
    }

    // Prefer the output whose desktop origin is (0,0) - that is the primary monitor.
    Microsoft::WRL::ComPtr<IDXGIOutput> chosen;
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(i, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            break;
        }
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) {
            continue;
        }
        const bool is_primary = desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0;
        if (!chosen || is_primary) {
            chosen = output;
        }
        if (is_primary) {
            break;
        }
    }
    if (!chosen) {
        last_error_ = "no attached DXGI output found";
        ReleaseDxgi();
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = chosen.As(&output1);
    if (FAILED(hr)) {
        last_error_ = Format("IDXGIOutput1 unavailable: %s", HrToString(hr).c_str());
        ReleaseDxgi();
        return false;
    }

    hr = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        last_error_ = Format("DuplicateOutput failed: %s", HrToString(hr).c_str());
        ReleaseDxgi();
        return false;
    }
    return true;
}

bool DesktopCapture::EnsureStagingTexture(int width, int height) {
    if (staging_ && staging_width_ == width && staging_height_ == height) {
        return true;
    }
    staging_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
    if (FAILED(hr)) {
        last_error_ = Format("CreateTexture2D(staging) failed: %s", HrToString(hr).c_str());
        return false;
    }
    staging_width_ = width;
    staging_height_ = height;
    return true;
}

GrabResult DesktopCapture::GrabDxgi(CaptureFrame& out) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return GrabResult::NoNewFrame;
    }
    if (FAILED(hr)) {
        last_error_ = Format("AcquireNextFrame failed: %s", HrToString(hr).c_str());
        return GrabResult::Failed;
    }

    FrameReleaser releaser;
    releaser.dup = duplication_.Get();
    releaser.armed = true;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);
    if (FAILED(hr)) {
        last_error_ = Format("IDXGIResource->ID3D11Texture2D failed: %s", HrToString(hr).c_str());
        return GrabResult::Failed;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        last_error_ = Format("unexpected duplication format: %u", static_cast<unsigned>(desc.Format));
        return GrabResult::Failed;
    }
    const int width = static_cast<int>(desc.Width);
    const int height = static_cast<int>(desc.Height);
    if (!EnsureStagingTexture(width, height)) {
        return GrabResult::Failed;
    }

    context_->CopyResource(staging_.Get(), texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        last_error_ = Format("Map(staging) failed: %s", HrToString(hr).c_str());
        return GrabResult::Failed;
    }

    out.Resize(width, height);
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < height; ++y) {
        std::memcpy(out.bgra.data() + static_cast<size_t>(y) * out.stride,
                    source + static_cast<size_t>(y) * mapped.RowPitch, row_bytes);
    }

    context_->Unmap(staging_.Get(), 0);
    return GrabResult::Fresh;
}

bool DesktopCapture::InitGdi() {
    ReleaseGdi();

    screen_dc_ = GetDC(nullptr);
    if (screen_dc_ == nullptr) {
        last_error_ = "GetDC(nullptr) failed";
        return false;
    }

    gdi_width_ = GetSystemMetrics(SM_CXSCREEN);
    gdi_height_ = GetSystemMetrics(SM_CYSCREEN);
    if (gdi_width_ <= 0 || gdi_height_ <= 0) {
        last_error_ = Format("unexpected screen size %dx%d", gdi_width_, gdi_height_);
        ReleaseGdi();
        return false;
    }

    mem_dc_ = CreateCompatibleDC(screen_dc_);
    if (mem_dc_ == nullptr) {
        last_error_ = "CreateCompatibleDC failed";
        ReleaseGdi();
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = gdi_width_;
    info.bmiHeader.biHeight = -gdi_height_; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    dib_ = CreateDIBSection(screen_dc_, &info, DIB_RGB_COLORS, &dib_bits_, nullptr, 0);
    if (dib_ == nullptr || dib_bits_ == nullptr) {
        last_error_ = "CreateDIBSection failed";
        ReleaseGdi();
        return false;
    }
    old_dib_ = SelectObject(mem_dc_, dib_);

    gdi_ok_ = true;
    mode_ = "gdi";
    Log("GDI BitBlt fallback active, screen %dx%d", gdi_width_, gdi_height_);
    return true;
}

bool DesktopCapture::ForceGdiFallback() {
    Log("switching capture backend to GDI BitBlt");
    ReleaseDxgi();
    if (!InitGdi()) {
        return false;
    }
    UpdateModeString();
    return true;
}

GrabResult DesktopCapture::GrabGdi(CaptureFrame& out) {
    if (!gdi_ok_) {
        return GrabResult::Failed;
    }

    // Recreate resources if the desktop resolution changed.
    const int screen_width = GetSystemMetrics(SM_CXSCREEN);
    const int screen_height = GetSystemMetrics(SM_CYSCREEN);
    if (screen_width != gdi_width_ || screen_height != gdi_height_) {
        Log("desktop resolution changed %dx%d -> %dx%d, recreating GDI surface", gdi_width_, gdi_height_,
            screen_width, screen_height);
        if (!InitGdi()) {
            return GrabResult::Failed;
        }
    }

    if (!BitBlt(mem_dc_, 0, 0, gdi_width_, gdi_height_, screen_dc_, 0, 0, SRCCOPY | CAPTUREBLT)) {
        last_error_ = Format("BitBlt failed: %s", HrToString(static_cast<long>(GetLastError())).c_str());
        return GrabResult::Failed;
    }

    out.Resize(gdi_width_, gdi_height_);
    std::memcpy(out.bgra.data(), dib_bits_, out.bgra.size());
    return GrabResult::Fresh;
}

GrabResult DesktopCapture::Grab(CaptureFrame& out) {
    if (dxgi_ok_) {
        GrabResult result = GrabDxgi(out);
        if (result != GrabResult::Failed) {
            return result;
        }
        Log("DXGI capture failed (%s), switching to GDI BitBlt", last_error_.c_str());
        ReleaseDxgi();
        UpdateModeString();
        if (!InitGdi()) {
            Log("GDI fallback failed: %s", last_error_.c_str());
            return GrabResult::Failed;
        }
        UpdateModeString();
    }
    if (gdi_ok_) {
        GrabResult result = GrabGdi(out);
        if (result == GrabResult::Failed) {
            Log("GDI capture failed: %s", last_error_.c_str());
        }
        return result;
    }
    return GrabResult::Failed;
}

} // namespace ss

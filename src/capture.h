#pragma once

#include "common.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace ss {

// One captured desktop image, BGRA8, top-down, tightly packed rows.
struct CaptureFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<uint8_t> bgra;

    void Resize(int w, int h) {
        width = w;
        height = h;
        stride = w * 4;
        bgra.resize(static_cast<size_t>(stride) * static_cast<size_t>(h));
    }
};

enum class GrabResult {
    Fresh,       // a new desktop frame was captured
    NoNewFrame,  // DXGI had nothing new (static desktop) - caller may reuse the old frame
    Failed       // capture path broke; caller should fall back
};

// Desktop capture with two backends:
//   1. DXGI Desktop Duplication (GPU, primary output)
//   2. GDI BitBlt (always available fallback)
// Init() tries DXGI first and silently falls back to GDI. The active backend is
// exposed through mode() so it can be printed and reported in /api/stats.
class DesktopCapture {
public:
    DesktopCapture() = default;
    ~DesktopCapture();

    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;

    bool Init();
    GrabResult Grab(CaptureFrame& out);
    void Shutdown();

    bool IsDxgi() const { return dxgi_ok_; }
    const std::string& mode() const { return mode_; }
    const std::string& last_error() const { return last_error_; }

    // Release DXGI and switch to GDI (used when DXGI never delivers frames).
    bool ForceGdiFallback();

private:
    bool InitDxgi();
    bool InitGdi();
    GrabResult GrabDxgi(CaptureFrame& out);
    GrabResult GrabGdi(CaptureFrame& out);
    void ReleaseDxgi();
    void ReleaseGdi();
    bool EnsureStagingTexture(int width, int height);
    void UpdateModeString();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    int staging_width_ = 0;
    int staging_height_ = 0;
    bool dxgi_ok_ = false;

    HDC screen_dc_ = nullptr;
    HDC mem_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_dib_ = nullptr;
    void* dib_bits_ = nullptr;
    int gdi_width_ = 0;
    int gdi_height_ = 0;
    bool gdi_ok_ = false;

    std::string mode_ = "none";
    std::string last_error_;
};

} // namespace ss

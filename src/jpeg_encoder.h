#pragma once

#include "capture.h"
#include "common.h"

#include <wincodec.h>
#include <wrl/client.h>

namespace ss {

// Downscales the captured BGRA frame and encodes it as JPEG using Windows Imaging Component.
// WIC gives us SIMD accelerated scaling (IWICBitmapScaler) and a decent baseline encoder
// without pulling in any third-party dependency.
class JpegEncoder {
public:
    JpegEncoder() = default;
    ~JpegEncoder();

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    bool Init(int target_width, int target_height, double quality);
    bool Encode(const CaptureFrame& source, std::vector<uint8_t>& jpeg);
    void Shutdown();

    int target_width() const { return target_width_; }
    int target_height() const { return target_height_; }
    const std::string& last_error() const { return last_error_; }

private:
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
    int target_width_ = 0;
    int target_height_ = 0;
    double quality_ = 0.72;
    std::string last_error_;
};

} // namespace ss

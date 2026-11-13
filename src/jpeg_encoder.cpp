#include "jpeg_encoder.h"

#include <ocidl.h>  // IPropertyBag2
#include <oleauto.h> // VARIANT helpers

#include <algorithm>
#include <cstdio>

namespace ss {

JpegEncoder::~JpegEncoder() {
    Shutdown();
}

void JpegEncoder::Shutdown() {
    factory_.Reset();
}

bool JpegEncoder::Init(int target_width, int target_height, double quality) {
    target_width_ = std::max(2, target_width);
    target_height_ = std::max(2, target_height);
    quality_ = std::clamp(quality, 0.05, 1.0);
    last_error_.clear();

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        last_error_ = Format("CoCreateInstance(CLSID_WICImagingFactory) failed: %s", HrToString(hr).c_str());
        return false;
    }
    return true;
}

bool JpegEncoder::Encode(const CaptureFrame& source, std::vector<uint8_t>& jpeg) {
    if (!factory_) {
        last_error_ = "encoder not initialized";
        return false;
    }
    if (source.width <= 0 || source.height <= 0 || source.bgra.empty()) {
        last_error_ = "empty source frame";
        return false;
    }

    const UINT source_width = static_cast<UINT>(source.width);
    const UINT source_height = static_cast<UINT>(source.height);
    const UINT target_width = static_cast<UINT>(target_width_);
    const UINT target_height = static_cast<UINT>(target_height_);
    const UINT stride = static_cast<UINT>(source.stride);
    const UINT buffer_size = static_cast<UINT>(source.bgra.size());

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    HRESULT hr = factory_->CreateBitmapFromMemory(source_width, source_height, GUID_WICPixelFormat32bppBGRA,
                                                  stride, buffer_size, const_cast<BYTE*>(source.bgra.data()),
                                                  &bitmap);
    if (FAILED(hr)) {
        last_error_ = Format("CreateBitmapFromMemory failed: %s", HrToString(hr).c_str());
        return false;
    }

    // Scale first (if needed), then convert BGRA -> BGR for the JPEG encoder.
    Microsoft::WRL::ComPtr<IWICBitmapSource> scaled;
    if (source_width != target_width || source_height != target_height) {
        Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
        hr = factory_->CreateBitmapScaler(&scaler);
        if (FAILED(hr)) {
            last_error_ = Format("CreateBitmapScaler failed: %s", HrToString(hr).c_str());
            return false;
        }
        hr = scaler->Initialize(bitmap.Get(), target_width, target_height, WICBitmapInterpolationModeLinear);
        if (FAILED(hr)) {
            last_error_ = Format("IWICBitmapScaler::Initialize failed: %s", HrToString(hr).c_str());
            return false;
        }
        scaled = scaler;
    } else {
        scaled = bitmap;
    }

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = factory_->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        last_error_ = Format("CreateFormatConverter failed: %s", HrToString(hr).c_str());
        return false;
    }
    hr = converter->Initialize(scaled.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        last_error_ = Format("IWICFormatConverter::Initialize failed: %s", HrToString(hr).c_str());
        return false;
    }

    Microsoft::WRL::ComPtr<IStream> stream;
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) {
        last_error_ = Format("CreateStreamOnHGlobal failed: %s", HrToString(hr).c_str());
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    hr = factory_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr)) {
        last_error_ = Format("CreateEncoder(JPEG) failed: %s", HrToString(hr).c_str());
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        last_error_ = Format("IWICBitmapEncoder::Initialize failed: %s", HrToString(hr).c_str());
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(&frame, &properties);
    if (FAILED(hr)) {
        last_error_ = Format("CreateNewFrame failed: %s", HrToString(hr).c_str());
        return false;
    }

    if (properties) {
        PROPBAG2 option = {};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = static_cast<FLOAT>(quality_);
        properties->Write(1, &option, &value);
        VariantClear(&value);
    }

    hr = frame->Initialize(properties.Get());
    if (FAILED(hr)) {
        last_error_ = Format("IWICBitmapFrameEncode::Initialize failed: %s", HrToString(hr).c_str());
        return false;
    }
    hr = frame->SetSize(target_width, target_height);
    if (FAILED(hr)) {
        last_error_ = Format("IWICBitmapFrameEncode::SetSize failed: %s", HrToString(hr).c_str());
        return false;
    }
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
    hr = frame->SetPixelFormat(&pixel_format);
    if (FAILED(hr)) {
        last_error_ = Format("IWICBitmapFrameEncode::SetPixelFormat failed: %s", HrToString(hr).c_str());
        return false;
    }
    hr = frame->WriteSource(converter.Get(), nullptr);
    if (FAILED(hr)) {
        last_error_ = Format("WriteSource failed: %s", HrToString(hr).c_str());
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr)) {
        last_error_ = Format("frame Commit failed: %s", HrToString(hr).c_str());
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr)) {
        last_error_ = Format("encoder Commit failed: %s", HrToString(hr).c_str());
        return false;
    }

    STATSTG stat{};
    hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (FAILED(hr) || stat.cbSize.QuadPart == 0) {
        last_error_ = "empty JPEG stream";
        return false;
    }
    const ULONGLONG size = stat.cbSize.QuadPart;
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    jpeg.resize(static_cast<size_t>(size));
    ULONG read = 0;
    hr = stream->Read(jpeg.data(), static_cast<ULONG>(jpeg.size()), &read);
    if (FAILED(hr) || read == 0) {
        last_error_ = "IStream::Read failed";
        jpeg.clear();
        return false;
    }
    jpeg.resize(read);
    return true;
}

} // namespace ss

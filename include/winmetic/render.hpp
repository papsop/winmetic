#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <string_view>
#include <stdexcept>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace winmetic {

using Microsoft::WRL::ComPtr;

/**
 * @brief Offscreen D3D11 WARP (Software) Renderer & Screenshot Capturer.
 * Renders without physical GPU contention and extracts frames via staging
 * textures directly to PNG without touching the desktop display DC or calling BitBlt.
 */
class OffscreenRenderer {
public:
    OffscreenRenderer(uint32_t width, uint32_t height)
        : width_(width), height_(height) {
        init_d3d();
        init_wic();
    }

    ~OffscreenRenderer() = default;

    /**
     * @brief Capture the given render texture to a PNG file on disk.
     */
    void save_texture_to_png(ID3D11Texture2D* texture, std::wstring_view path) {
        if (!texture) throw std::invalid_argument("Winmetic: Invalid D3D11 texture pointer.");

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);

        // Create CPU-readable staging texture
        D3D11_TEXTURE2D_DESC staging_desc = desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> staging;
        HRESULT hr = device_->CreateTexture2D(&staging_desc, nullptr, &staging);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to create D3D11 staging texture.");

        context_->CopyResource(staging.Get(), texture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to map D3D11 staging texture.");

        save_pixels_to_png(
            static_cast<const uint8_t*>(mapped.pData),
            desc.Width,
            desc.Height,
            mapped.RowPitch,
            path
        );

        context_->Unmap(staging.Get(), 0);
    }

    [[nodiscard]] ID3D11Device* device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* context() const noexcept { return context_.Get(); }
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }

private:
    void init_d3d() {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0 };
        D3D_FEATURE_LEVEL feature_level;

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP, // Pure software rasterizer: zero GPU load/interference
            nullptr,
            flags,
            feature_levels,
            1,
            D3D11_SDK_VERSION,
            &device_,
            &feature_level,
            &context_
        );

        if (FAILED(hr)) {
            throw std::runtime_error("Winmetic: Failed to create D3D11 WARP device.");
        }
    }

    void init_wic() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        (void)hr; // S_FALSE or S_OK or RPC_E_CHANGED_MODE are all fine

        hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&wic_factory_)
        );

        if (FAILED(hr)) {
            throw std::runtime_error("Winmetic: Failed to create WIC Imaging Factory.");
        }
    }

    void save_pixels_to_png(
        const uint8_t* pixels,
        uint32_t width,
        uint32_t height,
        uint32_t pitch,
        std::wstring_view path
    ) {
        ComPtr<IWICStream> stream;
        HRESULT hr = wic_factory_->CreateStream(&stream);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to create WIC stream.");

        hr = stream->InitializeFromFilename(path.data(), GENERIC_WRITE);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to initialize WIC stream from filename.");

        ComPtr<IWICBitmapEncoder> encoder;
        hr = wic_factory_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to create PNG encoder.");

        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to initialize encoder with stream.");

        ComPtr<IWICBitmapFrameEncode> frame;
        hr = encoder->CreateNewFrame(&frame, nullptr);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to create new encoder frame.");

        hr = frame->Initialize(nullptr);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to initialize frame.");

        hr = frame->SetSize(width, height);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to set frame size.");

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to set pixel format.");

        UINT buffer_size = pitch * height;
        hr = frame->WritePixels(height, pitch, buffer_size, const_cast<BYTE*>(pixels));
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to write frame pixels.");

        hr = frame->Commit();
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to commit frame.");

        hr = encoder->Commit();
        if (FAILED(hr)) throw std::runtime_error("Winmetic: Failed to commit encoder.");
    }

    uint32_t width_;
    uint32_t height_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IWICImagingFactory> wic_factory_;
};

} // namespace winmetic

#include "preview_shared_texture.h"

#include <cstdio>

namespace recorder_core {

PreviewSharedTexture::~PreviewSharedTexture() {
    Reset();
}

bool PreviewSharedTexture::Create(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                  HANDLE* out_handle, std::string& err) {
    Reset();
    if (device == nullptr || out_handle == nullptr || width == 0 || height == 0) {
        err = "PreviewSharedTexture::Create: invalid arguments";
        return false;
    }
    *out_handle = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // NT handle (cross-device open) + keyed mutex (non-blocking producer/consumer sync).
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    winrt::com_ptr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, tex.put());
    if (FAILED(hr)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "CreateTexture2D(shared preview) failed 0x%08lX",
                      static_cast<unsigned long>(hr));
        err = buf;
        return false;
    }

    winrt::com_ptr<IDXGIKeyedMutex> mutex;
    hr = tex->QueryInterface(IID_PPV_ARGS(mutex.put()));
    if (FAILED(hr)) {
        err = "QueryInterface(IDXGIKeyedMutex) failed";
        return false;
    }

    winrt::com_ptr<IDXGIResource1> res1;
    hr = tex->QueryInterface(IID_PPV_ARGS(res1.put()));
    if (FAILED(hr)) {
        err = "QueryInterface(IDXGIResource1) failed";
        return false;
    }

    HANDLE handle = nullptr;
    hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
    if (FAILED(hr) || handle == nullptr) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "CreateSharedHandle failed 0x%08lX", static_cast<unsigned long>(hr));
        err = buf;
        return false;
    }

    tex_ = std::move(tex);
    mutex_ = std::move(mutex);
    width_ = width;
    height_ = height;
    format_ = format;
    *out_handle = handle;
    return true;
}

bool PreviewSharedTexture::TryPublish(ID3D11DeviceContext* context, ID3D11Texture2D* src) {
    if (context == nullptr || src == nullptr || !Valid())
        return false;

    // 0 ms acquire: if the consumer currently holds the mutex we drop this frame
    // rather than stall the encode thread. WAIT_TIMEOUT / WAIT_ABANDONED land here.
    HRESULT hr = mutex_->AcquireSync(kPreviewSharedProducerKey, 0);
    if (hr != S_OK)
        return false;

    context->CopyResource(tex_.get(), src);
    mutex_->ReleaseSync(kPreviewSharedConsumerKey);
    return true;
}

void PreviewSharedTexture::Reset() noexcept {
    mutex_ = nullptr;
    tex_ = nullptr;
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
}

} // namespace recorder_core

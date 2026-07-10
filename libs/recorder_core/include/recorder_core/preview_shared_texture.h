#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/base.h>

#include <cstdint>
#include <string>

namespace recorder_core {

// Keyed-mutex key contract shared by the producer (engine) and the consumer
// (preview renderer). The mutex starts on the producer key.
//   Producer: AcquireSync(kProducerKey, 0) -> CopyResource -> ReleaseSync(kConsumerKey)
//   Consumer: AcquireSync(kConsumerKey, 0) -> copy out            -> ReleaseSync(kProducerKey)
// A 0 ms timeout on both sides means neither party ever blocks: on contention the
// AcquireSync returns WAIT_TIMEOUT and the caller simply drops that frame.
inline constexpr UINT64 kPreviewSharedProducerKey = 0;
inline constexpr UINT64 kPreviewSharedConsumerKey = 1;

// Producer-side shared GPU texture for the live preview: the engine's WYSIWYG
// tap during recording, and the DXGI capture hub's idle feed.
//
// Creates an NT-handle + keyed-mutex texture on the producer's D3D11 device,
// hands out exactly one shared handle (ownership passes to the caller/consumer,
// which opens it with ID3D11Device1::OpenSharedResource1 and then CloseHandle's
// it), and publishes frames into it. The publish path NEVER stalls the producer:
// a 0 ms keyed-mutex acquire drops the preview frame on contention.
//
// Not thread-safe; all methods run on the producer's one thread (the engine's
// video thread, or the hub's pump thread).
class PreviewSharedTexture {
  public:
    PreviewSharedTexture() = default;
    ~PreviewSharedTexture();

    PreviewSharedTexture(const PreviewSharedTexture&) = delete;
    PreviewSharedTexture& operator=(const PreviewSharedTexture&) = delete;

    // Create the shared texture at width x height with the given format on `device`.
    // On success writes a fresh NT handle to *out_handle and returns true. The
    // handle is owned by the caller and must be CloseHandle'd after the consumer
    // opens it. On failure returns false, sets `err`, and leaves the object invalid.
    bool Create(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format, HANDLE* out_handle,
                std::string& err);

    [[nodiscard]] bool Valid() const noexcept {
        return tex_ != nullptr && mutex_ != nullptr;
    }
    [[nodiscard]] uint32_t Width() const noexcept {
        return width_;
    }
    [[nodiscard]] uint32_t Height() const noexcept {
        return height_;
    }
    [[nodiscard]] DXGI_FORMAT Format() const noexcept {
        return format_;
    }

    // Non-blocking publish. 0 ms AcquireSync(kProducerKey); on success copies `src`
    // into the shared texture and releases with kConsumerKey. Returns true when the
    // frame was published, false when the consumer holds the mutex (frame dropped —
    // the encode path is never stalled) or the object is invalid. `src` must match
    // the shared texture's format and dimensions exactly (CopyResource requirement).
    bool TryPublish(ID3D11DeviceContext* context, ID3D11Texture2D* src);

    void Reset() noexcept;

  private:
    winrt::com_ptr<ID3D11Texture2D> tex_;
    winrt::com_ptr<IDXGIKeyedMutex> mutex_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace recorder_core

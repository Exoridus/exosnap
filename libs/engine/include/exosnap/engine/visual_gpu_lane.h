#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <exosnap/engine/device_generation.h>
#include <exosnap/engine/preview_shared_texture.h>
#include <exosnap/engine/visual_frame.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <variant>

namespace exosnap::engine {

class VisualSceneRuntime;
class GpuPublicationLease;

struct VisualGpuIdentity {
    SourceToken token;
    uint64_t resource_id = 0;
    DeviceGeneration producer;
    DeviceGeneration consumer;
};

struct VisualGpuFrame {
    VisualGpuIdentity identity;
    VisualFrameStamp stamp;
    ID3D11ShaderResourceView* view = nullptr;
};

// One producer and one consumer own their respective immediate contexts. The
// host pins this fixed generation through every call and use of a borrowed view.
// Create and destruction occur outside frame processing after both ends quiesce.
class VisualGpuLane final {
  public:
    static std::unique_ptr<VisualGpuLane> Create(const VisualGpuIdentity&, ID3D11Texture2D* source,
                                                 ID3D11DeviceContext* producer, ID3D11DeviceContext* consumer,
                                                 std::string& error);
    ~VisualGpuLane() = default;
    VisualGpuLane(const VisualGpuLane&) = delete;
    VisualGpuLane& operator=(const VisualGpuLane&) = delete;

    // A lane has no queue. Each call makes one zero-timeout acquire attempt.
    PublishResult TryPublish(const VisualFrameStamp&) noexcept;
    // Updated and Unchanged alone fill result. It is borrowed until the next
    // successful consume, so the host must exclude rewriting while frame-pinned.
    ReadResult TryRead(VisualGpuFrame&) noexcept;
    void Retire() noexcept;
    [[nodiscard]] bool Retired() const noexcept;
    [[nodiscard]] uint64_t LogicalTransportBytes() const noexcept;

  private:
    friend Outcome<std::monostate> StageGpuData(VisualSceneRuntime&, GpuPublicationLease&, ID3D11Texture2D*,
                                                ID3D11DeviceContext*, DeviceGeneration);
    // The runtime captured this pair on the consumer context's owner thread.
    static std::unique_ptr<VisualGpuLane> CreateForRegisteredConsumer(const VisualGpuIdentity&, ID3D11Texture2D*,
                                                                      ID3D11DeviceContext*, ID3D11DeviceContext*,
                                                                      ID3D11Device*, std::string&);
    VisualGpuLane() = default;
    bool DevicesValid() noexcept;

    VisualGpuIdentity identity_;
    std::atomic<bool> retired_{false};
    std::mutex metadata_;
    VisualFrameStamp published_;
    VisualFrameStamp accepted_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    winrt::com_ptr<ID3D11Device> producer_device_;
    winrt::com_ptr<ID3D11Device> consumer_device_;
    winrt::com_ptr<ID3D11DeviceContext> producer_context_;
    winrt::com_ptr<ID3D11DeviceContext> consumer_context_;
    winrt::com_ptr<ID3D11Texture2D> source_;
    PreviewSharedTexture shared_;
    winrt::com_ptr<ID3D11Texture2D> imported_;
    winrt::com_ptr<IDXGIKeyedMutex> consumer_mutex_;
    winrt::com_ptr<ID3D11Texture2D> local_;
    winrt::com_ptr<ID3D11ShaderResourceView> view_;
};

} // namespace exosnap::engine

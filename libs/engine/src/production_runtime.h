#pragma once

#include "file_output.h"

#include <exosnap/engine/color_metadata.h>

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace exosnap::engine {

class SharedAudioCaptureRegistry;
class VideoThread;
using GpuDeviceGeneration = ScalarValue<struct GpuDeviceGenerationTag>;

struct RuntimeCompletionToken {
    uint64_t domain = 0;
    GpuDeviceGeneration device_generation;
    uint64_t submit_serial = 0;
    auto operator<=>(const RuntimeCompletionToken&) const = default;
};

struct RuntimeFrameGeometry {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t native_format = 0;
    ColorMetadata color;
    enum class ColorSpace { Unknown, Srgb, LinearScrgb, Bt2020Pq };
    ColorSpace color_space = ColorSpace::Unknown;
};

class RuntimeFrameLease {
  public:
    RuntimeFrameLease() = default;
    [[nodiscard]] void* texture() const noexcept;
    [[nodiscard]] uint64_t sequence() const noexcept;
    [[nodiscard]] RuntimeQpc100ns capture_qpc() const noexcept;
    [[nodiscard]] const RuntimeFrameGeometry& geometry() const;
    [[nodiscard]] RuntimeCompletionToken completion() const noexcept;
    [[nodiscard]] bool completed() const noexcept;

  private:
    struct Slot;
    std::shared_ptr<const Slot> slot_;
    explicit RuntimeFrameLease(std::shared_ptr<const Slot> slot);
    friend class RuntimeFramePool;
};

// Reservation, completion and retirement are GPU-owner operations. Leases may
// be held or released by readers without accessing the immediate context.
class RuntimeFramePool {
  public:
    RuntimeFramePool(size_t slots, uint64_t domain, GpuDeviceGeneration generation);
    [[nodiscard]] std::optional<size_t> Reserve();
    Outcome<RuntimeCompletionToken> BeginWrite(size_t slot, std::shared_ptr<void> texture);
    Outcome<RuntimeFrameLease> Publish(size_t slot, uint64_t sequence, RuntimeQpc100ns qpc,
                                       RuntimeFrameGeometry geometry);
    void CancelReserved(size_t slot) noexcept;
    bool Complete(const RuntimeCompletionToken& token) noexcept;
    bool RotateGeneration(GpuDeviceGeneration generation);
    [[nodiscard]] size_t retired_slots() const noexcept;
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] bool reclaimable() const noexcept;

  private:
    void Collect();
    size_t count_;
    uint64_t domain_;
    GpuDeviceGeneration generation_;
    uint64_t next_serial_ = 0;
    std::vector<std::shared_ptr<RuntimeFrameLease::Slot>> current_;
    std::vector<std::shared_ptr<RuntimeFrameLease::Slot>> retired_;
};

struct RuntimeOutputCommand {
    CommandId command_id;
    ConfigRevision expected_revision{1};
    OutputGeneration output_generation;
    // Canonical resolved intent supplied by the control owner, excluding progress.
    std::string content_key;
};

class RawVideoReader {
  public:
    [[nodiscard]] std::optional<RuntimeFrameLease> TakeLatest();
    void Close() noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] uint64_t frames_published() const noexcept;
    bool TryPublish(RuntimeFrameLease frame);

  private:
    std::mutex mutex_;
    std::optional<RuntimeFrameLease> latest_;
    std::atomic<bool> closed_{false};
    std::atomic<uint64_t> frames_{0};
};

class ProductionRuntime : public std::enable_shared_from_this<ProductionRuntime> {
  public:
    ProductionRuntime();
    ~ProductionRuntime();
    Outcome<std::shared_ptr<FileOutput>> PrepareOutput(std::shared_ptr<SessionState> state,
                                                       RuntimeOutputCommand command);
    Outcome<bool> AttachOutput(std::shared_ptr<FileOutput> output);
    [[nodiscard]] std::shared_ptr<FileOutput> TakePendingOutput();
    [[nodiscard]] bool HasPendingOutput() const;
    void OutputDetached(const std::shared_ptr<FileOutput>& output);
    [[nodiscard]] std::shared_ptr<FileOutput> active_output() const;
    // Raw capture can run without creating a path or an encoder. Each reader has
    // three independent export slots; acquisition is bounded to four readers.
    Outcome<std::shared_ptr<RawVideoReader>> OpenRawReader(const RecorderConfig& capture_config);
    [[nodiscard]] std::vector<std::shared_ptr<RawVideoReader>> RawReaders();
    [[nodiscard]] bool HasRawReaders();
    std::shared_ptr<VideoThread> AcquireVideoWorker(std::shared_ptr<SessionState> initial_state);
    void VideoWorkerExited(VideoThread* worker) noexcept;
    bool VideoWorkerMayExit(VideoThread* worker) noexcept;
    void RequestShutdown() noexcept;
    [[nodiscard]] bool stopping() const noexcept;
    [[nodiscard]] ConfigRevision revision() const noexcept;
    void SetAudioRegistry(std::shared_ptr<SharedAudioCaptureRegistry> registry);
    [[nodiscard]] std::shared_ptr<SharedAudioCaptureRegistry> audio_registry() const;
    void CountRawExport(uint64_t logical_bytes) noexcept;
    [[nodiscard]] uint64_t raw_export_copies() const noexcept;
    [[nodiscard]] uint64_t raw_export_bytes() const noexcept;
    void MarkGpuStalled() noexcept;
    [[nodiscard]] bool gpu_stalled() const noexcept;

  private:
    struct CommandEntry {
        RuntimeOutputCommand command;
        std::weak_ptr<FileOutput> output;
    };
    mutable std::mutex mutex_;
    ConfigRevision revision_{1};
    OutputGeneration latest_generation_;
    std::optional<RecorderConfig> capture_config_;
    std::deque<CommandEntry> commands_;
    std::shared_ptr<FileOutput> pending_;
    std::weak_ptr<FileOutput> active_;
    std::vector<std::weak_ptr<RawVideoReader>> raw_readers_;
    std::shared_ptr<VideoThread> video_worker_;
    std::shared_ptr<SharedAudioCaptureRegistry> audio_registry_;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> raw_reader_count_{0};
    std::atomic<uint64_t> raw_export_copies_{0}, raw_export_bytes_{0};
    std::atomic<bool> gpu_stalled_{false};
};

} // namespace exosnap::engine

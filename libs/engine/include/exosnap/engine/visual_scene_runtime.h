#pragma once

#include <exosnap/engine/overlay_shader.h>
#include <exosnap/engine/visual_diagnostics.h>
#include <exosnap/engine/visual_frame.h>
#include <exosnap/engine/visual_gpu_lane.h>

#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace exosnap::gamepad {
class HistoryReadView;
}

namespace exosnap::engine {

struct GamepadSpriteSet;
class VisualSceneRuntime;

namespace detail {
struct CpuFrameSlot;
struct CpuImagePool;
struct SceneSlot;
struct VisualRuntimeStorage;
struct ConsumerReadState;
struct GpuResource;
} // namespace detail

struct PreparedSceneItem {
    SceneItemId id;
    VisualAddress source;
    SceneItemProperties properties;
    ChromaKeyParams chroma_key;
    uint32_t binding_index = 0;
};

struct PreparedSceneDescription {
    ConfigRevision revision;
    std::vector<PreparedSceneItem> items;
    // PublishScene resolves these addresses into retained host resources.
    std::vector<VisualAddress> binding_sources;
};

struct CpuImageBinding {
    CpuImageLayout layout;
};

struct GamepadBinding {
    const GamepadSpriteSet* sprites = nullptr;
    const gamepad::HistoryReadView* history = nullptr;
};

struct GpuImageBinding {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct VisualGpuConsumerDevice {
    ID3D11DeviceContext* context = nullptr;
    DeviceGeneration generation;
};

// All referenced data remains valid for the containing SceneReadLease.
struct PreparedSceneBinding {
    SourceToken token;
    uint64_t resource_id = 0;
    std::variant<CpuImageBinding, GamepadBinding, GpuImageBinding> content;
};

// Both leases borrow runtime storage. Keep its owner alive until Reset. Reset
// releases atomic pins only; CollectRetired owns all native resource destruction.
class GpuReadLease final {
  public:
    GpuReadLease() noexcept = default;
    ~GpuReadLease();
    GpuReadLease(GpuReadLease&&) noexcept;
    GpuReadLease& operator=(GpuReadLease&&) noexcept;
    GpuReadLease(const GpuReadLease&) = delete;
    GpuReadLease& operator=(const GpuReadLease&) = delete;
    explicit operator bool() const noexcept {
        return resource_ != nullptr;
    }
    const VisualGpuFrame& Frame() const noexcept {
        return frame_;
    }
    void Reset() noexcept;

  private:
    friend class VisualSceneRuntime;
    detail::GpuResource* resource_ = nullptr;
    detail::ConsumerReadState* reader_ = nullptr;
    VisualGpuFrame frame_;
};

// Single producer owner. Destroy its Qt renderer on the creating thread before
// Reset, so the runtime reservation includes producer teardown. Retire first.
class GpuPublicationLease final {
  public:
    GpuPublicationLease() noexcept = default;
    ~GpuPublicationLease();
    GpuPublicationLease(GpuPublicationLease&&) noexcept;
    GpuPublicationLease& operator=(GpuPublicationLease&&) noexcept;
    GpuPublicationLease(const GpuPublicationLease&) = delete;
    GpuPublicationLease& operator=(const GpuPublicationLease&) = delete;
    explicit operator bool() const noexcept {
        return resource_ != nullptr;
    }
    uint64_t ResourceId() const noexcept;
    bool Retired() const noexcept;
    // Copied during consumer registration; does not inspect a foreign context.
    bool AdapterLuid(LUID&) const noexcept;
    PublishResult TryPublish(const VisualFrameStamp&) noexcept;
    void Retire() noexcept;
    void Reset() noexcept;

  private:
    friend Outcome<GpuPublicationLease> ReserveGpuData(VisualSceneRuntime&, SourceToken, uint32_t, uint32_t);
    friend Outcome<std::monostate> StageGpuData(VisualSceneRuntime&, GpuPublicationLease&, ID3D11Texture2D*,
                                                ID3D11DeviceContext*, DeviceGeneration);
    friend Outcome<GpuPublicationLease> StageGpuData(VisualSceneRuntime&, const SourceToken&, ID3D11Texture2D*,
                                                     ID3D11DeviceContext*, DeviceGeneration);
    detail::GpuResource* resource_ = nullptr;
};

class CpuFrameWriteLease final {
  public:
    CpuFrameWriteLease() noexcept = default;
    ~CpuFrameWriteLease();
    CpuFrameWriteLease(CpuFrameWriteLease&&) noexcept;
    CpuFrameWriteLease& operator=(CpuFrameWriteLease&&) noexcept;
    CpuFrameWriteLease(const CpuFrameWriteLease&) = delete;
    CpuFrameWriteLease& operator=(const CpuFrameWriteLease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot_ != nullptr;
    }
    [[nodiscard]] std::span<uint8_t> Pixels() noexcept;
    // Compare the full negotiated layout before copying. Valid only while nonempty.
    [[nodiscard]] const CpuImageLayout& Layout() const noexcept;
    [[nodiscard]] const VisualFrameStamp& Stamp() const noexcept;
    void Reset() noexcept;

  private:
    friend class VisualPublicationWriter;
    std::shared_ptr<detail::VisualRuntimeStorage> storage_;
    detail::CpuImagePool* pool_ = nullptr;
    detail::CpuFrameSlot* slot_ = nullptr;
};

// The consumer retains the runtime owner until all its leases are released.
// Reset and destruction only decrement a pin; payload reclamation is control-side.
class VisualReadLease final {
  public:
    VisualReadLease() noexcept = default;
    ~VisualReadLease();
    VisualReadLease(VisualReadLease&&) noexcept;
    VisualReadLease& operator=(VisualReadLease&&) noexcept;
    VisualReadLease(const VisualReadLease&) = delete;
    VisualReadLease& operator=(const VisualReadLease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot_ != nullptr;
    }
    [[nodiscard]] const WebcamFrameSnapshot& Image() const noexcept;
    [[nodiscard]] uint64_t ResourceId() const noexcept;
    [[nodiscard]] const VisualFrameStamp& Stamp() const noexcept;
    void Reset() noexcept;

  private:
    friend class VisualSceneRuntime;
    detail::ConsumerReadState* reader_ = nullptr;
    detail::CpuImagePool* pool_ = nullptr;
    detail::CpuFrameSlot* slot_ = nullptr;
};

// Retain the outer runtime owner until this pin is released. Destruction never
// reclaims scene resources, even when the source or consumer has stopped.
class SceneReadLease final {
  public:
    SceneReadLease() noexcept = default;
    ~SceneReadLease();
    SceneReadLease(SceneReadLease&&) noexcept;
    SceneReadLease& operator=(SceneReadLease&&) noexcept;
    SceneReadLease(const SceneReadLease&) = delete;
    SceneReadLease& operator=(const SceneReadLease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot_ != nullptr;
    }
    [[nodiscard]] std::span<const PreparedSceneItem> Items() const noexcept;
    [[nodiscard]] std::span<const PreparedSceneBinding> Bindings() const noexcept;
    [[nodiscard]] ConfigRevision Revision() const noexcept;
    [[nodiscard]] uint64_t PublicationSequence() const noexcept;
    void Reset() noexcept;

  private:
    friend class VisualSceneRuntime;
    detail::ConsumerReadState* reader_ = nullptr;
    detail::SceneSlot* slot_ = nullptr;
};

// Copying a writer retains host storage, never a provider or native device owner.
// Default and moved-from writers are empty and cannot publish.
class VisualPublicationWriter final {
  public:
    VisualPublicationWriter() noexcept = default;
    ~VisualPublicationWriter() = default;
    VisualPublicationWriter(const VisualPublicationWriter&) noexcept = default;
    VisualPublicationWriter& operator=(const VisualPublicationWriter&) noexcept = default;
    VisualPublicationWriter(VisualPublicationWriter&&) noexcept = default;
    VisualPublicationWriter& operator=(VisualPublicationWriter&&) noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return storage_ != nullptr;
    }
    Outcome<std::monostate> PrepareImage(const SourceToken&, const CpuImageLayout&);
    std::optional<CpuFrameWriteLease> TryBeginCpuFrame(const VisualFrameStamp&) noexcept;
    PublishResult Publish(CpuFrameWriteLease&&) noexcept;

  private:
    friend class VisualSceneRuntime;
    std::shared_ptr<detail::VisualRuntimeStorage> storage_;
    VisualAddress address_;
};

using VisualConsumerId = uint32_t; // Host-issued, nonzero and never reused within a runtime.

class VisualSceneRuntime final {
  public:
    explicit VisualSceneRuntime(VisualBudgetPolicy);
    ~VisualSceneRuntime();
    VisualSceneRuntime(const VisualSceneRuntime&) = delete;
    VisualSceneRuntime& operator=(const VisualSceneRuntime&) = delete;

    // Counts successful CPU image publications, producer lock/full-pool drops
    // and host image/scene/binding try-lock misses. Not per source or per session.
    [[nodiscard]] VisualRuntimeCounters Counters() const noexcept;
    Outcome<VisualConsumerId> RegisterConsumer(VisualGpuConsumerDevice = {});
    // Call only after the consumer is quiescent and all its leases are empty.
    void UnregisterConsumer(VisualConsumerId);
    // Runtime generations are host-issued and strictly increase across all sources.
    // Rejected creations do not consume a generation; admitted generations never recur.
    Outcome<VisualPublicationWriter> CreateSource(VisualAddress);
    void SetRequest(const SourceToken&, bool publish_allowed, HoldPolicy) noexcept;
    // Reserves per-item sampler scratch for every registered consumer before acceptance.
    Outcome<ConfigRevision> PublishScene(const PreparedSceneDescription&);
    // Compare with the held scene before releasing its pin. Notifications can
    // coalesce or repeat; a changed publication may reuse the same ConfigRevision.
    [[nodiscard]] uint64_t PublishedSceneSequence() const noexcept;
    // Only Updated populates an empty destination. Busy cannot validate cache reuse.
    // Reacquisition can still be Busy after releasing an old scene; skip that tick.
    ReadResult TryReadScene(VisualConsumerId, SceneReadLease&) noexcept;
    // Replaces this consumer's existing scene pin atomically. Busy and Unchanged
    // preserve the old lease. Updated invalidates all views of the previous scene.
    ReadResult TryReplaceScene(VisualConsumerId, SceneReadLease&) noexcept;
    ReadResult TryReadImage(VisualConsumerId, VisualAddress, VisualReadLease&) noexcept;
    // Updated and Unchanged pin this consumer's local texture until Reset.
    // The binding must match the current token and resource, including on Busy.
    ReadResult TryReadGpu(VisualConsumerId, const PreparedSceneBinding&, GpuReadLease&) noexcept;
    // Control-side cache reset. Requires this consumer's image pin to be empty.
    // The next read returns Updated for otherwise unchanged accepted pixels.
    Outcome<std::monostate> InvalidateImage(VisualConsumerId, VisualAddress);
    // Unchanged confirms this binding may be used. Every other result means skip it.
    ReadResult CheckBinding(const PreparedSceneBinding&) noexcept;
    void Revoke(const VisualAddress&) noexcept;
    void CollectRetired();

  private:
    friend class VisualRuntimeTestAccess;
    friend Outcome<GpuPublicationLease> ReserveGpuData(VisualSceneRuntime&, SourceToken, uint32_t, uint32_t);
    friend Outcome<std::monostate> StageGpuData(VisualSceneRuntime&, GpuPublicationLease&, ID3D11Texture2D*,
                                                ID3D11DeviceContext*, DeviceGeneration);
    friend Outcome<GpuPublicationLease> StageGpuData(VisualSceneRuntime&, const SourceToken&, ID3D11Texture2D*,
                                                     ID3D11DeviceContext*, DeviceGeneration);
    friend Outcome<std::monostate> StageGamepadData(VisualSceneRuntime&, SourceToken,
                                                    std::shared_ptr<const GamepadSpriteSet>, gamepad::HistoryReadView);
    std::shared_ptr<detail::VisualRuntimeStorage> storage_;
};

// Control-side built-in integration; adopts immutable data without copying pixels.
Outcome<std::monostate> StageGamepadData(VisualSceneRuntime&, SourceToken, std::shared_ptr<const GamepadSpriteSet>,
                                         gamepad::HistoryReadView);

// Reserve before creating the producer target. Staging creates all lanes before
// changing the active binding. On failure destroy the producer, Reset, then collect.
// GPU consumer registration precedes staging. Changing that set retires active
// generations; publish a scene without the old binding before attempting rebuild.
Outcome<GpuPublicationLease> ReserveGpuData(VisualSceneRuntime&, SourceToken, uint32_t width, uint32_t height);
Outcome<std::monostate> StageGpuData(VisualSceneRuntime&, GpuPublicationLease&, ID3D11Texture2D*, ID3D11DeviceContext*,
                                     DeviceGeneration);
// Convenience for an already-owned texture whose allocation is accounted elsewhere.
Outcome<GpuPublicationLease> StageGpuData(VisualSceneRuntime&, const SourceToken&, ID3D11Texture2D*,
                                          ID3D11DeviceContext*, DeviceGeneration);

} // namespace exosnap::engine

#pragma once

#include <exosnap/engine/overlay_shader.h>
#include <exosnap/engine/visual_diagnostics.h>
#include <exosnap/engine/visual_frame.h>

#include <atomic>
#include <exosnap/engine/render_demand.h>
#include <memory>
#include <optional>
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

// All referenced data remains valid for the containing SceneReadLease.
struct PreparedSceneBinding {
    SourceToken token;
    uint64_t resource_id = 0;
    std::variant<CpuImageBinding, GamepadBinding> content;
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
    // The host's single Preview publishes full-scene physical pixels, including zoom.
    // Null is unknown; zero dimensions mean hidden. This does not own GPU resources.
    void SetPreviewPixelDemand(std::optional<CompositionSize> pixels) noexcept {
        preview_pixels_.store(pixels ? (uint64_t{pixels->width} << 32) | pixels->height : UINT64_MAX,
                              std::memory_order_release);
    }
    [[nodiscard]] std::optional<CompositionSize> PreviewPixelDemand() const noexcept {
        const auto pixels = preview_pixels_.load(std::memory_order_acquire);
        if (pixels == UINT64_MAX)
            return std::nullopt;
        return CompositionSize{static_cast<uint32_t>(pixels >> 32), static_cast<uint32_t>(pixels)};
    }
    Outcome<VisualConsumerId> RegisterConsumer();
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
    // Control-side cache reset. Requires this consumer's image pin to be empty.
    // The next read returns Updated for otherwise unchanged accepted pixels.
    Outcome<std::monostate> InvalidateImage(VisualConsumerId, VisualAddress);
    // Unchanged confirms this binding may be used. Every other result means skip it.
    ReadResult CheckBinding(const PreparedSceneBinding&) noexcept;
    void Revoke(const VisualAddress&) noexcept;
    void CollectRetired();

  private:
    friend class VisualRuntimeTestAccess;
    friend Outcome<std::monostate> StageGamepadData(VisualSceneRuntime&, SourceToken,
                                                    std::shared_ptr<const GamepadSpriteSet>, gamepad::HistoryReadView);
    std::shared_ptr<detail::VisualRuntimeStorage> storage_;
    std::atomic<uint64_t> preview_pixels_{UINT64_MAX};
};

// Control-side built-in integration; adopts immutable data without copying pixels.
Outcome<std::monostate> StageGamepadData(VisualSceneRuntime&, SourceToken, std::shared_ptr<const GamepadSpriteSet>,
                                         gamepad::HistoryReadView);

} // namespace exosnap::engine

#pragma once

#include <exosnap/engine/overlay_shader.h>
#include <exosnap/engine/visual_frame.h>

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
    detail::CpuImagePool* pool_ = nullptr;
    detail::CpuFrameSlot* slot_ = nullptr;
};

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
    void Reset() noexcept;

  private:
    friend class VisualSceneRuntime;
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
    Outcome<std::monostate> PrepareImage(SourceToken, CpuImageLayout);
    std::optional<CpuFrameWriteLease> TryBeginCpuFrame(VisualFrameStamp) noexcept;
    PublishResult Publish(CpuFrameWriteLease&&) noexcept;

  private:
    friend class VisualSceneRuntime;
    std::shared_ptr<detail::VisualRuntimeStorage> storage_;
    VisualAddress address_;
};

using VisualConsumerId = uint32_t;

class VisualSceneRuntime final {
  public:
    explicit VisualSceneRuntime(VisualBudgetPolicy);
    ~VisualSceneRuntime();
    VisualSceneRuntime(const VisualSceneRuntime&) = delete;
    VisualSceneRuntime& operator=(const VisualSceneRuntime&) = delete;

    Outcome<VisualConsumerId> RegisterConsumer();
    // Call only after the consumer is quiescent and all its leases are empty.
    void UnregisterConsumer(VisualConsumerId);
    Outcome<VisualPublicationWriter> CreateSource(VisualAddress);
    void SetRequest(SourceToken, bool publish_allowed, HoldPolicy) noexcept;
    Outcome<ConfigRevision> PublishScene(const PreparedSceneDescription&);
    // Only Updated populates an empty destination. Busy cannot validate cache reuse.
    ReadResult TryReadScene(VisualConsumerId, SceneReadLease&) noexcept;
    ReadResult TryReadImage(VisualConsumerId, VisualAddress, VisualReadLease&) noexcept;
    void Revoke(VisualAddress) noexcept;
    void CollectRetired();

  private:
    friend Outcome<std::monostate> StageGamepadData(VisualSceneRuntime&, SourceToken,
                                                    std::shared_ptr<const GamepadSpriteSet>, gamepad::HistoryReadView);
    std::shared_ptr<detail::VisualRuntimeStorage> storage_;
};

// Control-side built-in integration; adopts immutable data without copying pixels.
Outcome<std::monostate> StageGamepadData(VisualSceneRuntime&, SourceToken, std::shared_ptr<const GamepadSpriteSet>,
                                         gamepad::HistoryReadView);

} // namespace exosnap::engine

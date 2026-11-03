#pragma once

#include <exosnap/engine/overlay_sprites.h>
#include <exosnap/engine/visual_scene_runtime.h>

namespace exosnap::engine {

enum class VisualCompositionMode { Recording, IdlePreview, CompositedPreview };

struct VisualSceneItemFrame {
    const PreparedSceneItem* item = nullptr;
    SourceToken token;
    uint64_t resource_id = 0;
    uint64_t image_sequence = 0;
    ReadResult image_result = ReadResult::NotReady;
    ReadResult sample_result = ReadResult::NotReady;
    const WebcamFrameSnapshot* image = nullptr;
    const SpriteLayerSnapshot* sprites = nullptr;
    WebcamPixelRect image_rect;
    float image_opacity = 1.0F;
    // Authorizes reuse only if the caller has this exact token/resource/sequence cached.
    bool cache_valid = false;
};

// Single-owner consumer. Prepare, Refresh and Close require an exclusive
// maintenance boundary outside acquisition/composition, with no active Frame.
class VisualSceneConsumer final {
  public:
    class Frame final {
      public:
        Frame() noexcept = default;
        ~Frame();
        Frame(Frame&&) noexcept;
        Frame& operator=(Frame&&) noexcept;
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        [[nodiscard]] std::span<const VisualSceneItemFrame> Items() const noexcept;
        [[nodiscard]] bool Changed() const noexcept;
        [[nodiscard]] uint64_t PublicationSequence() const noexcept;
        [[nodiscard]] ConfigRevision Revision() const noexcept;

      private:
        friend class VisualSceneConsumer;
        VisualSceneConsumer* owner_ = nullptr;
        size_t count_ = 0;
        bool changed_ = false;
    };

    static Outcome<std::unique_ptr<VisualSceneConsumer>> Prepare(std::shared_ptr<VisualSceneRuntime>);
    ~VisualSceneConsumer();
    VisualSceneConsumer(const VisualSceneConsumer&) = delete;
    VisualSceneConsumer& operator=(const VisualSceneConsumer&) = delete;

    [[nodiscard]] bool NeedsRefresh() const noexcept;
    Outcome<ReadResult> Refresh();
    // Current CPU bindings, prepared during Refresh and valid until the next maintenance call.
    [[nodiscard]] std::span<const VisualAddress> ImageSources() const noexcept;
    [[nodiscard]] size_t ItemCount() const noexcept;
    [[nodiscard]] bool HasItems(VisualCompositionMode) const noexcept;
    // Returned views are valid only through Frame destruction, which releases CPU
    // pins without reclaiming payloads. The consumer must outlive its Frame.
    [[nodiscard]] Frame Sample(CaptureTime100ns, const WebcamPixelRect&, VisualCompositionMode) noexcept;
    void Close() noexcept;

  private:
    struct BindingState;
    struct ItemState;
    explicit VisualSceneConsumer(std::shared_ptr<VisualSceneRuntime>);
    void ReleaseFrame() noexcept;
    void ReleaseItems() noexcept;

    std::shared_ptr<VisualSceneRuntime> runtime_;
    VisualConsumerId consumer_ = 0;
    SceneReadLease scene_;
    std::vector<std::unique_ptr<BindingState>> bindings_;
    std::vector<ItemState> items_;
    std::vector<VisualSceneItemFrame> frame_items_;
    std::vector<VisualAddress> image_sources_;
    uint64_t prepared_sequence_ = 0;
    bool frame_active_ = false;
    bool scene_changed_ = false;
    bool have_mode_ = false;
    WebcamPixelRect previous_content_;
    VisualCompositionMode previous_mode_ = VisualCompositionMode::Recording;
};

} // namespace exosnap::engine

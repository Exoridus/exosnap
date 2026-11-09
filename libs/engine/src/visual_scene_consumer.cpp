#include "visual_scene_consumer.h"

#include "gamepad_frame_sampler.h"
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/overlay_sprites.h>
#include <exosnap/engine/recording_media_types.h>
#include <exosnap/engine/visual_frame.h>
#include <exosnap/engine/visual_scene.h>
#include <exosnap/engine/visual_scene_runtime.h>
#include <exosnap/engine/webcam_placement.h>
#include <iterator>
#include <memory>
#include <span>
#include <variant>
#include <vector>

#include <algorithm>
#include <cassert>
#include <new>
#include <utility>
#include <windows.h>

namespace exosnap::engine {
namespace {

EngineError ConsumerError(EngineErrorCode code) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::Runtime;
    error.context = "visual scene consumer";
    return error;
}

bool SameRect(const WebcamPixelRect& a, const WebcamPixelRect& b) noexcept {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool VisibleInMode(bool output_visible, VisualCompositionMode mode) noexcept {
    return mode == VisualCompositionMode::IdlePreview || (mode == VisualCompositionMode::Recording && output_visible) ||
           (mode == VisualCompositionMode::CompositedPreview && !output_visible);
}

} // namespace

struct VisualSceneConsumer::BindingState {
    VisualAddress address;
    SourceToken accepted_token;
    uint64_t accepted_resource = 0;
    uint64_t accepted_sequence = 0;
    VisualReadLease image;
    ReadResult result = ReadResult::NotReady;
    bool sampled = false;
    bool needs_image_reset = false;
};

struct VisualSceneConsumer::ItemState {
    SceneItemId id;
    uint32_t binding_index = 0;
    BindingState* binding = nullptr;
    std::unique_ptr<GamepadFrameSampler> sampler;
    SpriteLayerSnapshot output;
    bool drawn = false;
    bool sampled_drawn = false;
};

VisualSceneConsumer::VisualSceneConsumer(std::shared_ptr<VisualSceneRuntime> runtime) : runtime_(std::move(runtime)) {
    char setting[2]{};
    plan_cache_enabled_ =
        GetEnvironmentVariableA("EXOSNAP_STAGE_A_CACHE", setting, sizeof(setting)) == 1 && setting[0] == '1';
}

VisualSceneConsumer::~VisualSceneConsumer() {
    assert(!frame_active_);
    Close();
}

Outcome<std::unique_ptr<VisualSceneConsumer>>
VisualSceneConsumer::Prepare(std::shared_ptr<VisualSceneRuntime> runtime) {
    if (!runtime)
        return ConsumerError(EngineErrorCode::InvalidConfig);
    try {
        auto result = std::unique_ptr<VisualSceneConsumer>(new VisualSceneConsumer(std::move(runtime)));
        auto registered = result->runtime_->RegisterConsumer();
        if (!registered.succeeded())
            return *registered.error();
        result->consumer_ = *registered.value();
        auto refreshed = result->Refresh();
        if (!refreshed.succeeded())
            return *refreshed.error();
        return result;
    } catch (const std::bad_alloc&) {
        return ConsumerError(EngineErrorCode::ResourceBudgetExceeded);
    }
}

bool VisualSceneConsumer::CanReusePlan() const noexcept {
    if (!plan_cache_enabled_ || prepared_sequence_ == 0 || items_.size() != scene_.Items().size() ||
        plan_bindings_.size() != scene_.Bindings().size())
        return false;
    for (size_t index = 0; index < plan_bindings_.size(); ++index) {
        const auto& key = plan_bindings_[index];
        const auto& binding = scene_.Bindings()[index];
        if (key.token != binding.token || key.resource_id != binding.resource_id || key.kind != binding.content.index())
            return false;
    }
    for (size_t index = 0; index < items_.size(); ++index) {
        const auto& state = items_[index];
        const auto& item = scene_.Items()[index];
        if (state.id != item.id || state.binding_index != item.binding_index || state.binding->address != item.source)
            return false;
    }
    return true;
}

bool VisualSceneConsumer::NeedsRefresh() const noexcept {
    return runtime_ && consumer_ != 0 &&
           (invalidation_pending_ || !scene_ || prepared_sequence_ != runtime_->PublishedSceneSequence());
}

Outcome<ReadResult> VisualSceneConsumer::Refresh() {
    if (frame_active_)
        return ConsumerError(EngineErrorCode::Stalled);
    if (!runtime_ || consumer_ == 0)
        return ReadResult::Revoked;
    if (invalidation_pending_) {
        for (auto& binding : bindings_) {
            if (!binding->needs_image_reset)
                continue;
            auto invalidated = runtime_->InvalidateImage(consumer_, binding->address);
            // A revoked source may have lost its reader metadata during collection.
            if (!invalidated.succeeded() && invalidated.error()->code != EngineErrorCode::InvalidConfig)
                return *invalidated.error();
            binding->needs_image_reset = false;
        }
        invalidation_pending_ = false;
    }
    if (!NeedsRefresh())
        return ReadResult::Unchanged;
    if (!scene_ || scene_.PublicationSequence() != runtime_->PublishedSceneSequence()) {
        const auto read = runtime_->TryReplaceScene(consumer_, scene_);
        refresh_deferred_ = read == ReadResult::Busy;
        if (read != ReadResult::Updated)
            return read;
        // Resource IDs are runtime-unique. The new lease pins the same immutable
        // sampler data before any retained borrowed reference can be used again.
        if (CanReusePlan()) {
            prepared_sequence_ = scene_.PublicationSequence();
            scene_changed_ = true;
            refresh_deferred_ = false;
            ++plan_reuse_count_;
            return ReadResult::Updated;
        }
        // Old borrowed views are invalid after replacement; only destroy their
        // holders here, without sampling or dereferencing the previous binding.
        ReleaseItems();
    }
    refresh_deferred_ = true;
    try {
        items_.clear();
        std::erase_if(bindings_, [&](const auto& binding) {
            return std::ranges::none_of(scene_.Items(),
                                        [&](const auto& item) { return item.source == binding->address; });
        });
        items_.reserve(scene_.Items().size());
        frame_items_.resize(scene_.Items().size());
        image_sources_.clear();
        image_sources_.reserve(scene_.Bindings().size());
        if (plan_cache_enabled_)
            plan_bindings_.reserve(scene_.Bindings().size());
        for (const auto& binding : scene_.Bindings()) {
            if (plan_cache_enabled_)
                plan_bindings_.push_back({binding.token, binding.resource_id, binding.content.index()});
            if (std::holds_alternative<CpuImageBinding>(binding.content) &&
                std::ranges::find(image_sources_, binding.token.address) == image_sources_.end())
                image_sources_.push_back(binding.token.address);
        }
        for (const auto& item : scene_.Items()) {
            const auto& binding = scene_.Bindings()[item.binding_index];
            auto found =
                std::ranges::find_if(bindings_, [&](const auto& state) { return state->address == item.source; });
            if (found == bindings_.end()) {
                if (std::holds_alternative<CpuImageBinding>(binding.content)) {
                    auto invalidated = runtime_->InvalidateImage(consumer_, item.source);
                    if (!invalidated.succeeded()) {
                        ReleaseItems();
                        return *invalidated.error();
                    }
                }
                auto state = std::make_unique<BindingState>();
                state->address = item.source;
                bindings_.push_back(std::move(state));
                found = std::prev(bindings_.end());
            }
            ItemState state;
            state.id = item.id;
            state.binding_index = item.binding_index;
            state.binding = found->get();
            if (const auto* pad = std::get_if<GamepadBinding>(&binding.content)) {
                if (!pad->sprites || !pad->history) {
                    ReleaseItems();
                    return ConsumerError(EngineErrorCode::InvalidConfig);
                }
                auto sampler = GamepadFrameSampler::Prepare(*pad->sprites, *pad->history);
                if (!sampler.succeeded()) {
                    ReleaseItems();
                    return *sampler.error();
                }
                state.sampler = std::move(*sampler.value());
                auto output = state.sampler->PrepareOutput(state.output);
                if (!output.succeeded()) {
                    ReleaseItems();
                    return *output.error();
                }
            }
            items_.push_back(std::move(state));
        }
        prepared_sequence_ = scene_.PublicationSequence();
        scene_changed_ = true;
        refresh_deferred_ = false;
        ++plan_rebuild_count_;
        return ReadResult::Updated;
    } catch (const std::bad_alloc&) {
        ReleaseItems();
        return ConsumerError(EngineErrorCode::ResourceBudgetExceeded);
    }
}

std::span<const VisualAddress> VisualSceneConsumer::ImageSources() const noexcept {
    return image_sources_;
}

size_t VisualSceneConsumer::ItemCount() const noexcept {
    return items_.size();
}

bool VisualSceneConsumer::HasItems(VisualCompositionMode mode) const noexcept {
    return !items_.empty() && std::ranges::any_of(scene_.Items(), [&](const auto& item) {
        return VisibleInMode(item.properties.output_visible, mode);
    });
}

VisualSceneConsumer::Frame VisualSceneConsumer::Sample(CaptureTime100ns time, const WebcamPixelRect& content,
                                                       VisualCompositionMode mode) noexcept {
    Frame frame;
    if (frame_active_ || !runtime_)
        return frame;
    frame_active_ = true;
    frame.owner_ = this;
    if (refresh_deferred_ || invalidation_pending_) {
        frame.deferred_ = true;
        return frame;
    }
    frame.changed_ = std::exchange(scene_changed_, false) || !have_mode_ || previous_mode_ != mode ||
                     !SameRect(previous_content_, content);
    previous_mode_ = mode;
    previous_content_ = content;
    have_mode_ = true;
    for (auto& binding : bindings_)
        binding->sampled = false;
    for (size_t index = 0; index < items_.size(); ++index) {
        auto& state = items_[index];
        const auto& item = scene_.Items()[index];
        const auto& binding = scene_.Bindings()[item.binding_index];
        const bool visible = content.IsValid() && VisibleInMode(item.properties.output_visible, mode);
        bool drawn = false;
        if (visible) {
            auto& source = *state.binding;
            if (!source.sampled) {
                source.result = state.sampler ? runtime_->CheckBinding(binding)
                                              : runtime_->TryReadImage(consumer_, item.source, source.image);
                source.sampled = true;
                if (source.result == ReadResult::Updated) {
                    source.accepted_token = source.image.Stamp().token;
                    source.accepted_resource = source.image.ResourceId();
                    source.accepted_sequence = source.image.Stamp().sequence;
                }
            }
            if (source.result == ReadResult::Busy) {
                frame.deferred_ = true;
                break;
            }
            if (source.result == ReadResult::Updated || source.result == ReadResult::Unchanged) {
                auto& view = frame_items_[frame.count_];
                view = {};
                view.item = &item;
                auto properties = item.properties;
                if (!properties.output_visible)
                    properties.opacity *= 0.35F;
                if (state.sampler) {
                    view.token = binding.token;
                    view.resource_id = binding.resource_id;
                    view.sample_result = state.sampler->Sample(time, properties, content, state.output);
                    view.sprites = &state.output;
                    frame.changed_ |= state.sampler->SampleChanged();
                    drawn = !state.output.quads.empty();
                } else if (source.accepted_resource != 0) {
                    view.token = source.accepted_token;
                    view.resource_id = source.accepted_resource;
                    view.image_sequence = source.accepted_sequence;
                    view.image_result = source.result;
                    view.image = source.image ? &source.image.Image() : nullptr;
                    view.image_rect =
                        MapWebcamPlacementToContent(properties.placement, content.x, content.y, content.w, content.h);
                    view.image_opacity = properties.opacity;
                    view.cache_valid = true;
                    frame.changed_ |= source.result == ReadResult::Updated;
                    drawn = true;
                }
                if (drawn)
                    ++frame.count_;
            }
        }
        frame.changed_ |= state.drawn != drawn;
        state.sampled_drawn = drawn;
    }
    if (frame.deferred_) {
        for (auto& binding : bindings_) {
            if (binding->image) {
                binding->needs_image_reset = true;
                invalidation_pending_ = true;
            }
        }
        scene_changed_ |= frame.changed_;
        frame.count_ = 0;
        frame.changed_ = false;
    } else {
        for (auto& state : items_)
            state.drawn = state.sampled_drawn;
    }
    return frame;
}

void VisualSceneConsumer::ReleaseFrame() noexcept {
    for (auto& binding : bindings_)
        binding->image.Reset();
    frame_active_ = false;
}

void VisualSceneConsumer::ReleaseItems() noexcept {
    scene_changed_ |= prepared_sequence_ != 0;
    items_.clear();
    frame_items_.clear();
    image_sources_.clear();
    plan_bindings_.clear();
    prepared_sequence_ = 0;
}

void VisualSceneConsumer::Close() noexcept {
    if (frame_active_)
        return;
    ReleaseItems();
    bindings_.clear();
    scene_.Reset();
    if (runtime_ && consumer_ != 0)
        runtime_->UnregisterConsumer(std::exchange(consumer_, 0));
    runtime_.reset();
}

VisualSceneConsumer::Frame::~Frame() {
    if (owner_)
        owner_->ReleaseFrame();
}

VisualSceneConsumer::Frame::Frame(Frame&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), count_(other.count_), changed_(other.changed_),
      deferred_(other.deferred_) {
}

VisualSceneConsumer::Frame& VisualSceneConsumer::Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        if (owner_)
            owner_->ReleaseFrame();
        owner_ = std::exchange(other.owner_, nullptr);
        count_ = other.count_;
        changed_ = other.changed_;
        deferred_ = other.deferred_;
    }
    return *this;
}

std::span<const VisualSceneItemFrame> VisualSceneConsumer::Frame::Items() const noexcept {
    return owner_ ? std::span<const VisualSceneItemFrame>(owner_->frame_items_.data(), count_)
                  : std::span<const VisualSceneItemFrame>{};
}

bool VisualSceneConsumer::Frame::Changed() const noexcept {
    return changed_;
}

bool VisualSceneConsumer::Frame::Deferred() const noexcept {
    return deferred_;
}

uint64_t VisualSceneConsumer::Frame::PublicationSequence() const noexcept {
    return owner_ ? owner_->scene_.PublicationSequence() : 0;
}

ConfigRevision VisualSceneConsumer::Frame::Revision() const noexcept {
    return owner_ ? owner_->scene_.Revision() : ConfigRevision{};
}

} // namespace exosnap::engine

#pragma once

#include <exosnap/engine/gamepad_sprite_data.h>
#include <exosnap/engine/visual_scene_runtime.h>

#include <atomic>
#include <map>
#include <mutex>

namespace exosnap::engine::detail {

inline constexpr uint32_t kWritingSlot = UINT32_MAX;

struct CpuFrameSlot {
    std::atomic<uint32_t> pins{0};
    WebcamFrameSnapshot image;
    VisualFrameStamp stamp;
};

struct CpuImagePool {
    uint64_t id = 0;
    uint64_t bytes = 0;
    CpuImageLayout layout;
    std::vector<std::unique_ptr<CpuFrameSlot>> frames;
};

struct GamepadResource {
    uint64_t id = 0;
    uint64_t bytes = 0;
    uint64_t scratch_per_consumer = 0;
    uint64_t scratch_instances = 1;
    SourceToken token;
    std::shared_ptr<const GamepadSpriteSet> sprites;
    gamepad::HistoryReadView history;
};

struct ConsumerReadState {
    std::atomic<uint32_t> pins{0};
    uint64_t sequence = 0;
    uint64_t resource_id = 0;
    SourceToken token;
};

struct ImageConsumerState {
    VisualAddress address;
    ConsumerReadState read;
};

struct VisualConsumerState {
    ConsumerReadState scene;
    std::vector<std::unique_ptr<ImageConsumerState>> images;
};

struct SceneSlot {
    std::atomic<uint32_t> pins{0};
    uint64_t sequence = 0;
    ConfigRevision revision;
    std::vector<PreparedSceneItem> items;
    std::vector<PreparedSceneBinding> bindings;
    std::vector<std::shared_ptr<const GamepadResource>> gamepads;
};

struct VisualSourceStorage {
    SourceToken token;
    bool publish_allowed = false;
    bool revoked = false;
    bool image_preparing = false;
    uint64_t retiring_resources = 0;
    std::vector<std::unique_ptr<CpuImagePool>> pools;
    CpuImagePool* active_pool = nullptr;
    CpuImagePool* accepted_pool = nullptr;
    CpuFrameSlot* accepted = nullptr;
    std::vector<std::shared_ptr<GamepadResource>> gamepads;
    GamepadResource* active_gamepad = nullptr;
};

struct VisualRuntimeStorage {
    mutable std::mutex mutex;
    VisualBudgetPolicy policy;
    std::map<VisualAddress, std::unique_ptr<VisualSourceStorage>> sources;
    std::map<VisualConsumerId, std::unique_ptr<VisualConsumerState>> consumers;
    std::vector<std::unique_ptr<SceneSlot>> scenes;
    SceneSlot* scene = nullptr;
    ConfigRevision accepted_revision;
    std::atomic<uint64_t> scene_sequence{0};
    uint64_t next_scene_sequence = 1;
    uint64_t next_resource_id = 1;
    uint64_t last_runtime_generation = 0;
    VisualConsumerId next_consumer = 1;
    uint64_t reserved_bytes = 0;
    std::atomic<uint64_t> accepted_publications{0};
    std::atomic<uint64_t> dropped_publications{0};
    std::atomic<uint64_t> busy_reads{0};
};

} // namespace exosnap::engine::detail

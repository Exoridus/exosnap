#pragma once

#include "CapabilityRegistry.h"
#include "models/VisualSceneConfig.h"

#include <map>

namespace exosnap {

// All entry points and queued source completions run on the control thread.
// The owner retains this manager, its registry and runtime until quiescent.
class VisualSourceManager final {
  public:
    VisualSourceManager(const CapabilityRegistry&, engine::VisualSceneRuntime&,
                        std::function<void(SourceStatus)> observer = {});
    ~VisualSourceManager();
    VisualSourceManager(const VisualSourceManager&) = delete;
    VisualSourceManager& operator=(const VisualSourceManager&) = delete;

    // Validation and factory failures leave live controls unchanged. Once accepted,
    // native Apply failures are source-local; a busy scene projection remains pending.
    engine::Outcome<engine::ConfigRevision> ApplyDesired(const VisualSceneConfig&, engine::ConfigRevision);
    void SetConsumerDemand(VisualConsumer, bool active);
    void OnSourceStatus(SourceStatus);
    std::optional<SourceStatus> Status(engine::VisualAddress) const;
    std::optional<engine::VisualAddress> CurrentAddress(engine::VisualSourceId) const;
    const std::optional<engine::EngineError>& ProjectionError() const noexcept;
    engine::Outcome<std::monostate> Retry(engine::VisualAddress);
    void RequestShutdown() noexcept;
    bool IsQuiescent() const noexcept;
    void CollectRetired();
    // Republish resource bindings without reapplying source configuration.
    void RepublishScene();

  private:
    struct Entry {
        VisualSourceRecord record;
        std::unique_ptr<VisualSource> source;
        engine::SourceToken token;
        SourceStatus status;
        uint8_t demand = 0;
        bool pending = true;
        bool retry_pending = false;
        bool ready = false;
    };
    struct CompletionTarget {
        VisualSourceManager* manager;
    };
    engine::Outcome<Entry> Create(const VisualSourceRecord&);
    void Retire(Entry&);
    void Reconcile(Entry&, bool content_changed = false);
    engine::Outcome<engine::ConfigRevision> PublishScene();
    void DrainStatuses();
    bool Blocked(std::string_view type) const;

    const CapabilityRegistry& registry_;
    engine::VisualSceneRuntime& runtime_;
    std::shared_ptr<CompletionTarget> completion_;
    std::function<void(SourceStatus)> observer_;
    std::vector<SourceStatus> queued_statuses_;
    std::map<engine::VisualSourceId, Entry> sources_;
    std::vector<Entry> retired_;
    VisualSceneConfig applied_scene_;
    engine::ConfigRevision revision_;
    std::optional<engine::EngineError> projection_error_;
    uint64_t next_generation_ = 1;
    uint8_t demand_ = 0;
    bool shutting_down_ = false;
    bool mutating_ = false;
};

} // namespace exosnap

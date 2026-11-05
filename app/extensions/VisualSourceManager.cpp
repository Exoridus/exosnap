#include "VisualSourceManager.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <tuple>

namespace exosnap {
namespace {

auto ItemValues(const SceneItemRecord& item) {
    const auto& p = item.properties;
    const auto& r = p.placement;
    return std::tie(item.id, item.source, r.x, r.y, r.w, r.h, r.mirror, p.opacity, p.output_visible, p.locked,
                    p.aspect_ratio_locked, p.user_placed, p.order, item.chroma_key);
}

bool SameScene(const VisualSceneConfig& a, const VisualSceneConfig& b) {
    return a.sources.size() == b.sources.size() && a.items.size() == b.items.size() &&
           std::equal(a.sources.begin(), a.sources.end(), b.sources.begin(),
                      [](const auto& x, const auto& y) {
                          return x.id == y.id && x.type_id == y.type_id && x.settings_version == y.settings_version &&
                                 x.enabled == y.enabled && x.settings == y.settings;
                      }) &&
           std::equal(a.items.begin(), a.items.end(), b.items.begin(),
                      [](const auto& x, const auto& y) { return ItemValues(x) == ItemValues(y); });
}

engine::EngineError Failure(engine::EngineErrorCode code, std::string context) {
    return {.code = code, .context = std::move(context)};
}

} // namespace

VisualSourceManager::VisualSourceManager(const CapabilityRegistry& registry, engine::VisualSceneRuntime& runtime,
                                         std::function<void(SourceStatus)> observer)
    : registry_(registry), runtime_(runtime), completion_(std::make_shared<CompletionTarget>(CompletionTarget{this})),
      observer_(std::move(observer)) {
}

VisualSourceManager::~VisualSourceManager() {
    completion_->manager = nullptr;
    RequestShutdown();
    assert(IsQuiescent());
}

engine::Outcome<VisualSourceManager::Entry> VisualSourceManager::Create(const VisualSourceRecord& record) {
    std::erase_if(retired_, [](const auto& entry) { return entry.source->IsQuiescent(); });
    runtime_.CollectRetired();
    if (next_generation_ == 0)
        return Failure(engine::EngineErrorCode::ResourceBudgetExceeded, "Visual runtime generation exhausted");
    Entry entry;
    entry.record = record;
    entry.token = {{record.id, engine::VisualRuntimeGeneration{next_generation_++}}, engine::SourceRequestRevision{1}};
    entry.status = {entry.token, SourcePhase::Dormant, {}};
    auto writer = runtime_.CreateSource(entry.token.address);
    if (!writer.succeeded())
        return *writer.error();
    const std::weak_ptr<CompletionTarget> target = completion_;
    auto source =
        registry_.FindVisualSource(record.type_id)
            ->create({entry.token, record.settings, std::move(*writer.value()), [target](SourceStatus status) {
                          if (const auto receiver = target.lock(); receiver && receiver->manager)
                              receiver->manager->OnSourceStatus(std::move(status));
                      }});
    if (!source.succeeded() || !*source.value()) {
        runtime_.Revoke(entry.token.address);
        return source.succeeded() ? Failure(engine::EngineErrorCode::InvalidConfig, "Empty visual source factory")
                                  : *source.error();
    }
    entry.source = std::move(*source.value());
    return entry;
}

bool VisualSourceManager::Blocked(std::string_view type) const {
    return std::any_of(retired_.begin(), retired_.end(),
                       [&](const auto& entry) { return entry.record.type_id == type && !entry.source->IsQuiescent(); });
}

void VisualSourceManager::Retire(Entry& entry) {
    runtime_.Revoke(entry.token.address);
    entry.source->RequestStop();
    retired_.push_back(std::move(entry));
}

engine::Outcome<engine::ConfigRevision> VisualSourceManager::ApplyDesired(const VisualSceneConfig& scene,
                                                                          engine::ConfigRevision revision) {
    if (shutting_down_ || !registry_.IsSealed() || revision.value == 0 || revision < revision_)
        return Failure(engine::EngineErrorCode::StaleRevision,
                       "Visual manager is closed, unsealed or revision is stale");
    auto validated = ValidateVisualScene(scene, registry_);
    if (!validated.succeeded())
        return *validated.error();
    if (revision == revision_ && !SameScene(*validated.value(), applied_scene_))
        return Failure(engine::EngineErrorCode::StaleRevision, "Changed visual configuration reuses its revision");

    std::map<engine::VisualSourceId, Entry> staged;
    mutating_ = true;
    for (const auto& record : validated.value()->sources) {
        const auto* type = registry_.FindVisualSource(record.type_id);
        if (!type || record.settings_version != type->settings_version)
            continue;
        const auto existing = sources_.find(record.id);
        if (existing != sources_.end() && existing->second.record.type_id == record.type_id &&
            existing->second.record.settings_version == record.settings_version)
            continue;
        engine::Outcome<Entry> created =
            Blocked(record.type_id) ? engine::Outcome<Entry>(Failure(engine::EngineErrorCode::Stalled,
                                                                     "Previous visual producer is not quiescent"))
                                    : Create(record);
        if (!created.succeeded()) {
            for (auto& [id, entry] : staged)
                Retire(entry);
            mutating_ = false;
            DrainStatuses();
            return *created.error();
        }
        staged.emplace(record.id, std::move(*created.value()));
    }

    for (auto it = sources_.begin(); it != sources_.end();) {
        const auto desired = std::find_if(validated.value()->sources.begin(), validated.value()->sources.end(),
                                          [&](const auto& record) { return record.id == it->first; });
        const auto* type =
            desired == validated.value()->sources.end() ? nullptr : registry_.FindVisualSource(desired->type_id);
        if (!type || desired->settings_version != type->settings_version || staged.contains(it->first)) {
            Retire(it->second);
            it = sources_.erase(it);
        } else {
            ++it;
        }
    }
    sources_.merge(staged);
    revision_ = revision;
    applied_scene_ = std::move(*validated.value());
    for (const auto& record : applied_scene_.sources) {
        const auto found = sources_.find(record.id);
        if (found == sources_.end())
            continue;
        auto& entry = found->second;
        const bool changed = entry.record.settings != record.settings;
        entry.record = record;
        Reconcile(entry, changed);
    }
    auto published = PublishScene();
    mutating_ = false;
    DrainStatuses();
    return published;
}

void VisualSourceManager::Reconcile(Entry& entry, bool content_changed) {
    if (entry.retry_pending)
        return;
    const bool clear_accepted = content_changed && !entry.source->RetainsAccepted(entry.record.settings);
    if (clear_accepted)
        entry.ready = false;
    const uint8_t demand = entry.record.enabled ? demand_ : 0;
    if (entry.pending && Blocked(entry.record.type_id)) {
        entry.demand = demand;
        entry.status = {entry.token, demand ? SourcePhase::Waiting : SourcePhase::Dormant,
                        demand ? std::optional<engine::EngineError>{Failure(
                                     engine::EngineErrorCode::Stalled, "Previous visual producer is not quiescent")}
                               : std::nullopt};
        OnSourceStatus(entry.status);
        return;
    }
    const bool changed = content_changed || ((entry.demand == 0) != (demand == 0));
    if (!entry.pending && changed) {
        if (entry.token.request.value == std::numeric_limits<uint64_t>::max()) {
            OnSourceStatus(
                {entry.token, SourcePhase::Failed,
                 Failure(engine::EngineErrorCode::ResourceBudgetExceeded, "Visual request revision exhausted")});
            return;
        }
        ++entry.token.request.value;
    }
    const bool reset_status = entry.pending || changed;
    entry.demand = demand;
    entry.pending = false;
    if (reset_status)
        entry.status = {entry.token, demand ? SourcePhase::Preparing : SourcePhase::Dormant, {}};
    else if (entry.status.phase == SourcePhase::Failed)
        return;
    runtime_.SetRequest(entry.token, demand != 0,
                        clear_accepted ? engine::HoldPolicy::ClearAccepted : engine::HoldPolicy::KeepAccepted);
    auto result = entry.source->Apply({entry.token, revision_, {demand}, entry.record.settings});
    if (!result.succeeded())
        OnSourceStatus({entry.token, SourcePhase::Failed, *result.error()});
}

void VisualSourceManager::SetConsumerDemand(VisualConsumer consumer, bool active) {
    if (shutting_down_)
        return;
    const auto bit = static_cast<uint8_t>(consumer);
    if (bit != 1 && bit != 2 && bit != 4)
        return;
    const auto demand = static_cast<uint8_t>(active ? demand_ | bit : demand_ & ~bit);
    if (demand == demand_)
        return;
    demand_ = demand;
    mutating_ = true;
    for (auto& [id, entry] : sources_)
        Reconcile(entry);
    if (revision_.value != 0)
        PublishScene();
    mutating_ = false;
    DrainStatuses();
}

void VisualSourceManager::OnSourceStatus(SourceStatus status) {
    if (mutating_) {
        queued_statuses_.push_back(std::move(status));
        return;
    }
    const auto found = sources_.find(status.token.address.source);
    if (shutting_down_ || found == sources_.end() || found->second.token != status.token || found->second.retry_pending)
        return;
    auto& entry = found->second;
    if (entry.demand == 0 && status.phase != SourcePhase::Dormant && status.phase != SourcePhase::Stopping &&
        status.phase != SourcePhase::Failed)
        return;
    if (entry.status.phase == SourcePhase::Failed && status.phase != SourcePhase::Failed)
        return;
    entry.status = status;
    if (status.phase == SourcePhase::Ready)
        entry.ready = true;
    if (status.phase == SourcePhase::Failed) {
        entry.ready = false;
        runtime_.SetRequest(entry.token, false, engine::HoldPolicy::ClearAccepted);
    }
    PublishScene();
    if (observer_)
        observer_(std::move(status));
}

void VisualSourceManager::DrainStatuses() {
    auto statuses = std::move(queued_statuses_);
    queued_statuses_.clear();
    for (auto& status : statuses)
        OnSourceStatus(std::move(status));
}

std::optional<SourceStatus> VisualSourceManager::Status(engine::VisualAddress address) const {
    const auto found = sources_.find(address.source);
    return found != sources_.end() && found->second.token.address == address
               ? std::optional<SourceStatus>{found->second.status}
               : std::nullopt;
}

std::optional<engine::VisualAddress> VisualSourceManager::CurrentAddress(engine::VisualSourceId id) const {
    const auto found = sources_.find(id);
    return found != sources_.end() ? std::optional<engine::VisualAddress>{found->second.token.address} : std::nullopt;
}

const std::optional<engine::EngineError>& VisualSourceManager::ProjectionError() const noexcept {
    return projection_error_;
}

engine::Outcome<std::monostate> VisualSourceManager::Retry(engine::VisualAddress address) {
    const auto found = sources_.find(address.source);
    if (shutting_down_ || found == sources_.end() || found->second.token.address != address)
        return Failure(engine::EngineErrorCode::StaleRevision, "Visual retry address is stale");
    auto& entry = found->second;
    if (Blocked(entry.record.type_id))
        return Failure(engine::EngineErrorCode::Stalled, "Previous visual producer is not quiescent");
    if (!entry.source->IsQuiescent()) {
        runtime_.Revoke(address);
        entry.retry_pending = true;
        entry.status.phase = SourcePhase::Stopping;
        entry.source->RequestStop();
        PublishScene();
        return std::monostate{};
    }
    mutating_ = true;
    auto replacement = Create(entry.record);
    if (!replacement.succeeded()) {
        entry.retry_pending = false;
        OnSourceStatus({entry.token, SourcePhase::Failed, *replacement.error()});
        mutating_ = false;
        DrainStatuses();
        return *replacement.error();
    }
    Retire(entry);
    entry = std::move(*replacement.value());
    Reconcile(entry);
    PublishScene();
    mutating_ = false;
    DrainStatuses();
    return std::monostate{};
}

engine::Outcome<engine::ConfigRevision> VisualSourceManager::PublishScene() {
    engine::PreparedSceneDescription scene;
    scene.revision = revision_;
    for (const auto& item : applied_scene_.items) {
        const auto found = sources_.find(item.source);
        if (found == sources_.end() || !found->second.ready || found->second.demand == 0 || found->second.pending ||
            found->second.retry_pending || found->second.status.phase == SourcePhase::Failed)
            continue;
        const auto address = found->second.token.address;
        auto binding = std::find(scene.binding_sources.begin(), scene.binding_sources.end(), address);
        if (binding == scene.binding_sources.end()) {
            scene.binding_sources.push_back(address);
            binding = scene.binding_sources.end() - 1;
        }
        const auto rgb = item.chroma_key.active_color();
        const auto& key = item.chroma_key;
        scene.items.push_back({item.id,
                               address,
                               item.properties,
                               {key.enabled, rgb.r, rgb.g, rgb.b, key.tolerance, key.softness, key.spill_reduction},
                               static_cast<uint32_t>(binding - scene.binding_sources.begin())});
    }
    std::stable_sort(scene.items.begin(), scene.items.end(),
                     [](const auto& a, const auto& b) { return a.properties.order < b.properties.order; });
    auto result = runtime_.PublishScene(scene);
    projection_error_ = result.succeeded() ? std::nullopt : std::optional<engine::EngineError>{*result.error()};
    return result;
}

void VisualSourceManager::RequestShutdown() noexcept {
    if (shutting_down_)
        return;
    shutting_down_ = true;
    for (auto& [id, entry] : sources_) {
        runtime_.Revoke(entry.token.address);
        entry.source->RequestStop();
    }
}

bool VisualSourceManager::IsQuiescent() const noexcept {
    return std::all_of(sources_.begin(), sources_.end(),
                       [](const auto& pair) { return pair.second.source->IsQuiescent(); }) &&
           std::all_of(retired_.begin(), retired_.end(), [](const auto& entry) { return entry.source->IsQuiescent(); });
}

void VisualSourceManager::CollectRetired() {
    std::erase_if(retired_, [](const auto& entry) { return entry.source->IsQuiescent(); });
    runtime_.CollectRetired();
    if (shutting_down_)
        return;
    std::vector<engine::VisualAddress> retries;
    mutating_ = true;
    for (auto& [id, entry] : sources_) {
        if (entry.retry_pending && entry.source->IsQuiescent())
            retries.push_back(entry.token.address);
        else if (entry.pending)
            Reconcile(entry);
    }
    if (revision_.value != 0)
        PublishScene();
    mutating_ = false;
    DrainStatuses();
    for (const auto address : retries)
        Retry(address);
}

} // namespace exosnap

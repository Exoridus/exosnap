#include "AudioRoutingAdapter.h"

#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <limits>

namespace exosnap::quick {
namespace {
using namespace engine;

QString Text(const std::string& text) {
    return QString::fromStdString(text);
}
QString IdText(const auto& id) {
    return Text(ToUuid(id));
}

template <typename Id> bool Matches(const Id& id, const QString& text) {
    return FromUuid<Id>(text.toStdString()) == id;
}
template <typename Row> auto Find(std::vector<Row>& rows, const QString& id) {
    return std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return Matches(row.id, id); });
}
double Dbfs(float linear) {
    return linear > 0 ? 20.0 * std::log10(double(linear)) : -std::numeric_limits<double>::infinity();
}
AudioEnvironmentSnapshot PresentationEnvironment(AudioEnvironmentSnapshot environment, const TrackGraph1Intent& graph) {
    for (const auto& source : graph.sources)
        if (std::find(environment.allow_silent_sources.begin(), environment.allow_silent_sources.end(), source.id) ==
            environment.allow_silent_sources.end())
            environment.allow_silent_sources.push_back(source.id);
    return environment;
}
TrackGraph1Intent ActiveGraph(const ResolvedAudioPlan& plan) {
    TrackGraph1Intent graph;
    for (const auto& source : plan.sources)
        graph.sources.push_back(source.spec);
    for (const auto& track : plan.tracks) {
        AudioTrackSpec spec;
        spec.id = track.id;
        spec.order = track.visible_position > 0 ? track.visible_position - 1 : 0;
        spec.name_kind = AudioTrackNameKind::Custom;
        spec.name = track.name;
        spec.role = track.role;
        graph.tracks.push_back(spec);
        for (const auto& route : track.routes) {
            AudioRouteSpec configured;
            configured.id = route.id;
            configured.source_id = route.source_id;
            configured.track_id = track.id;
            configured.muted = route.gain_linear == 0;
            configured.gain_db = route.gain_linear > 0 ? float(Dbfs(route.gain_linear)) : 0;
            graph.routes.push_back(configured);
        }
    }
    graph.default_monitor_track_ids = plan.default_monitor_track_ids;
    graph.monitor_selection_origin = plan.monitor_selection_origin;
    return graph;
}
QString AvailabilityText(AudioSourceAvailability availability) {
    switch (availability) {
    case AudioSourceAvailability::Available:
        return AudioRoutingAdapter::tr("Available");
    case AudioSourceAvailability::UnsupportedPlatform:
        return AudioRoutingAdapter::tr("Unsupported on this platform");
    case AudioSourceAvailability::EndpointUnavailable:
        return AudioRoutingAdapter::tr("Device unavailable");
    case AudioSourceAvailability::ProcessExited:
        return AudioRoutingAdapter::tr("Application exited");
    case AudioSourceAvailability::ProcessIdentityChanged:
        return AudioRoutingAdapter::tr("Application identity changed");
    case AudioSourceAvailability::NoTargetProcess:
        return AudioRoutingAdapter::tr("No target application");
    case AudioSourceAvailability::UnconfirmedProcess:
        return AudioRoutingAdapter::tr("Application needs confirmation");
    }
    return {};
}

bool IsNumber(const QVariant& value) {
    switch (value.metaType().id()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    default:
        return false;
    }
}

bool ApplySourceDetails(AudioSourceSpec& source, const QVariantMap& details, QString& error) {
    const auto invalid = [&](const QString& key) {
        error = AudioRoutingAdapter::tr("Invalid audio source setting: %1.").arg(key);
        return false;
    };
    const auto number = [&](const QString& key, const QVariant& value, double minimum, double maximum,
                            bool integer = false) {
        const double numeric = value.toDouble();
        if (!IsNumber(value) || !std::isfinite(numeric) || numeric < minimum || numeric > maximum ||
            (integer && std::floor(numeric) != numeric))
            return invalid(key);
        return true;
    };
    for (auto item = details.cbegin(); item != details.cend(); ++item) {
        const auto& key = item.key();
        const auto& value = item.value();
        if (key == QStringLiteral("endpointId") || key == QStringLiteral("followDefault") ||
            key == QStringLiteral("endpointRole")) {
            if (source.binding.kind != AudioBindingKind::PlaybackEndpoint &&
                source.binding.kind != AudioBindingKind::MicrophoneEndpoint)
                return invalid(key);
            if (key == QStringLiteral("endpointId")) {
                if (value.metaType().id() != QMetaType::QString)
                    return invalid(key);
                source.binding.endpoint_id = value.toString().toStdString();
            } else if (key == QStringLiteral("followDefault")) {
                if (value.metaType().id() != QMetaType::Bool)
                    return invalid(key);
                source.binding.follow_default = value.toBool();
            } else {
                if (!number(key, value, int(AudioEndpointRole::Console), int(AudioEndpointRole::Communications), true))
                    return false;
                source.binding.endpoint_role = static_cast<AudioEndpointRole>(value.toInt());
            }
        } else if (key == QStringLiteral("channelMode")) {
            if (!number(key, value, int(MicChannelMode::Auto), int(MicChannelMode::RightToStereo), true))
                return false;
            source.channel_mode = static_cast<MicChannelMode>(value.toInt());
        } else if (key == QStringLiteral("gainDb")) {
            if (!number(key, value, kMinGainDb, kMaxGainDb))
                return false;
            source.post_dsp_gain_linear = float(std::pow(10.0, value.toDouble() / 20.0));
        } else if (key == QStringLiteral("hpfCutoffHz")) {
            if (!number(key, value, 20, 1000))
                return false;
            source.dsp.hpf_cutoff_hz = float(value.toDouble());
        } else if (key == QStringLiteral("gateThresholdDb")) {
            if (!number(key, value, -80, 0))
                return false;
            source.dsp.gate_threshold_db = float(value.toDouble());
        } else if (key == QStringLiteral("agcTargetDb")) {
            if (!number(key, value, -40, 0))
                return false;
            source.dsp.agc_target_db = float(value.toDouble());
        } else if (key == QStringLiteral("hpfEnabled") || key == QStringLiteral("gateEnabled") ||
                   key == QStringLiteral("agcEnabled") || key == QStringLiteral("rnnoiseEnabled")) {
            if (value.metaType().id() != QMetaType::Bool)
                return invalid(key);
            if (key == QStringLiteral("hpfEnabled"))
                source.dsp.hpf_enabled = value.toBool();
            else if (key == QStringLiteral("gateEnabled"))
                source.dsp.gate_enabled = value.toBool();
            else if (key == QStringLiteral("agcEnabled"))
                source.dsp.agc_enabled = value.toBool();
            else
                source.dsp.rnnoise_enabled = value.toBool();
        } else {
            error = AudioRoutingAdapter::tr("Unknown audio source setting: %1.").arg(key);
            return false;
        }
    }
    if ((source.binding.kind == AudioBindingKind::PlaybackEndpoint ||
         source.binding.kind == AudioBindingKind::MicrophoneEndpoint) &&
        !source.binding.follow_default && QString::fromStdString(source.binding.endpoint_id).trimmed().isEmpty())
        return invalid(QStringLiteral("endpointId"));
    return true;
}
} // namespace

AudioRoutingAdapter::AudioRoutingAdapter(QObject* parent) : QObject(parent) {
}

void AudioRoutingAdapter::configure(SaveCallback save, EnvironmentProvider environment, LiveMuteCallback liveMute) {
    save_ = std::move(save);
    environment_ = std::move(environment);
    live_mute_ = std::move(liveMute);
}
void AudioRoutingAdapter::refresh(const engine::AudioIntent& intent, bool readOnly, QString validationError) {
    // The save owner may synchronously refresh adapters before the callback returns.
    // Publication must still wait for the save result.
    if (saving_)
        return;
    if (intent_ != intent || read_only_ != readOnly) {
        intent_ = intent;
        read_only_ = readOnly;
        pending_.reset();
        undo_.clear();
        ++revision_;
    }
    error_ = std::move(validationError);
    emit changed();
}
void AudioRoutingAdapter::setActivePlan(std::optional<engine::ResolvedAudioPlan> plan) {
    active_ = std::move(plan);
    source_meters_.clear();
    track_meters_.clear();
    ++revision_;
    emit changed();
    emit metersChanged();
}
void AudioRoutingAdapter::setMeters(std::vector<engine::SourceMeterSnapshot> sources,
                                    std::vector<engine::TrackMeterSnapshot> tracks) {
    source_meters_ = std::move(sources);
    track_meters_ = std::move(tracks);
    emit metersChanged();
}
engine::AudioEnvironmentSnapshot AudioRoutingAdapter::environment() const {
    return environment_ ? environment_() : engine::AudioEnvironmentSnapshot{};
}
engine::TrackGraph1Intent AudioRoutingAdapter::presentationGraph() const {
    if (const auto* graph = std::get_if<engine::TrackGraph1Intent>(&intent_))
        return *graph;
    const auto& legacy = std::get<engine::LegacyRows25Intent>(intent_);
    auto materialized = engine::MaterializeLegacyIntent(legacy, environment());
    if (!materialized.succeeded())
        return {};
    auto graph = *materialized.value();
    for (const auto& row : legacy.rows) {
        if (row.row.enabled)
            continue;
        engine::AudioTrackSpec track;
        track.id = row.anchor_track_id;
        track.order = static_cast<uint32_t>(graph.tracks.size());
        track.name_kind = row.name_kind;
        track.name = row.name;
        track.role = row.role;
        track.record_enabled = row.record_enabled;
        graph.tracks.push_back(track);
    }
    return graph;
}
std::optional<engine::ResolvedAudioPlan> AudioRoutingAdapter::resolvedPlan() const {
    const auto graph = presentationGraph();
    auto resolved = engine::ResolveAudioIntent(graph, PresentationEnvironment(environment(), graph), {revision_});
    return resolved.succeeded() ? std::optional{*resolved.value()} : std::nullopt;
}
QVariantList AudioRoutingAdapter::sourceRows(const engine::TrackGraph1Intent& graph, bool active) const {
    const auto resolved = active ? active_ : resolvedPlan();
    QVariantList rows;
    for (const auto& source : graph.sources) {
        QVariantMap row{
            {"id", IdText(source.id)},
            {"name", Text(source.name)},
            {"participation", source.participation},
            {"muted", source.muted},
            {"meterAvailable", false},
            {"active", active},
            {"bindingKind", int(source.binding.kind)},
            {"endpointId", Text(source.binding.endpoint_id)},
            {"followDefault", source.binding.follow_default},
            {"endpointRole", int(source.binding.endpoint_role)},
            {"channelMode", int(source.channel_mode)},
            {"gainDb", Dbfs(source.post_dsp_gain_linear)},
            {"hpfEnabled", source.dsp.hpf_enabled},
            {"hpfCutoffHz", source.dsp.hpf_cutoff_hz},
            {"gateEnabled", source.dsp.gate_enabled},
            {"gateThresholdDb", source.dsp.gate_threshold_db},
            {"agcEnabled", source.dsp.agc_enabled},
            {"agcTargetDb", source.dsp.agc_target_db},
            {"rnnoiseEnabled", source.dsp.rnnoise_enabled},
            {"sharedLegacyMicrophone", legacy() && source.legacy_row_id.has_value() &&
                                           source.binding.kind == engine::AudioBindingKind::MicrophoneEndpoint}};
        QStringList assigned;
        for (const auto& track : graph.tracks)
            if (std::any_of(graph.routes.begin(), graph.routes.end(), [&](const auto& route) {
                    return route.source_id == source.id && route.track_id == track.id;
                }))
                assigned.push_back(IdText(track.id));
        row.insert("assignedTrackIds", assigned);
        if (source.legacy_row_id)
            row.insert("legacyRowId", IdText(*source.legacy_row_id));
        if (resolved) {
            const auto status = std::find_if(resolved->sources.begin(), resolved->sources.end(),
                                             [&](const auto& item) { return item.spec.id == source.id; });
            if (status != resolved->sources.end()) {
                row.insert("availability", static_cast<int>(status->availability));
                row.insert("availabilityText", AvailabilityText(status->availability));
                row.insert("available", status->availability == engine::AudioSourceAvailability::Available);
            }
        }
        const auto meter = std::find_if(source_meters_.begin(), source_meters_.end(),
                                        [&](const auto& item) { return item.source_id == source.id; });
        if (meter != source_meters_.end() && meter->has_measurement) {
            row.insert("meterAvailable", true);
            row.insert("rmsDbfs", Dbfs(meter->rms));
            row.insert("peakDbfs", Dbfs(meter->peak));
            row.insert("sampleGeneration", QVariant::fromValue<qulonglong>(meter->generation.value));
        }
        rows.push_back(row);
    }
    return rows;
}
QVariantList AudioRoutingAdapter::trackRows(const engine::TrackGraph1Intent& graph, bool active) const {
    auto tracks = graph.tracks;
    std::stable_sort(tracks.begin(), tracks.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
    QVariantList rows;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const auto& track = tracks[index];
        QVariantList routes;
        bool routed = false;
        for (const auto& route : graph.routes) {
            if (route.track_id != track.id)
                continue;
            const auto source = std::find_if(graph.sources.begin(), graph.sources.end(),
                                             [&](const auto& item) { return item.id == route.source_id; });
            routes.push_back(QVariantMap{{"id", IdText(route.id)},
                                         {"sourceId", IdText(route.source_id)},
                                         {"name", source == graph.sources.end() ? QString{} : Text(source->name)},
                                         {"gainDb", route.gain_db},
                                         {"muted", route.muted},
                                         {"enabled", route.enabled}});
            routed = routed || route.enabled;
        }
        QVariantMap row{{"id", IdText(track.id)},
                        {"position", int(index + 1)},
                        {"name", track.name_kind == engine::AudioTrackNameKind::Custom ? Text(track.name)
                                                                                       : tr("Audio %1").arg(index + 1)},
                        {"nameKind", static_cast<int>(track.name_kind)},
                        {"role", static_cast<int>(track.role)},
                        {"recordEnabled", track.record_enabled},
                        {"recording", track.record_enabled && routed},
                        {"empty", routes.empty()},
                        {"routes", routes},
                        {"meterAvailable", false},
                        {"active", active},
                        {"defaultMonitored",
                         std::find(graph.default_monitor_track_ids.begin(), graph.default_monitor_track_ids.end(),
                                   track.id) != graph.default_monitor_track_ids.end()}};
        const auto meter = std::find_if(track_meters_.begin(), track_meters_.end(),
                                        [&](const auto& item) { return item.track_id == track.id; });
        if (meter != track_meters_.end() && meter->has_measurement) {
            row.insert("meterAvailable", true);
            row.insert("rmsDbfs", Dbfs(meter->rms));
        }
        rows.push_back(row);
    }
    return rows;
}
QVariantList AudioRoutingAdapter::tracks() const {
    return trackRows(presentationGraph(), false);
}
QVariantList AudioRoutingAdapter::sources() const {
    return sourceRows(presentationGraph(), false);
}
QVariantMap AudioRoutingAdapter::sourceMeters() const {
    QVariantMap result;
    for (const auto& meter : source_meters_) {
        QVariantMap value{{"available", meter.has_measurement}};
        if (meter.has_measurement) {
            value.insert("rmsDbfs", Dbfs(meter.rms));
            value.insert("peakDbfs", Dbfs(meter.peak));
            value.insert("sampleGeneration", QVariant::fromValue<qulonglong>(meter.generation.value));
        }
        result.insert(IdText(meter.source_id), value);
    }
    return result;
}
QVariantMap AudioRoutingAdapter::trackMeters() const {
    QVariantMap result;
    for (const auto& meter : track_meters_) {
        QVariantMap value{{"available", meter.has_measurement}};
        if (meter.has_measurement)
            value.insert("rmsDbfs", Dbfs(meter.rms));
        result.insert(IdText(meter.track_id), value);
    }
    return result;
}
QVariantList AudioRoutingAdapter::activeTracks() const {
    return active_ ? trackRows(ActiveGraph(*active_), true) : QVariantList{};
}
QVariantList AudioRoutingAdapter::activeSources() const {
    return active_ ? sourceRows(ActiveGraph(*active_), true) : QVariantList{};
}
QString AudioRoutingAdapter::conversionText() const {
    return tr("The current track layout will become fixed. Activating sources later will no longer move existing merge "
              "groups.");
}
QString AudioRoutingAdapter::planLabel() const {
    return active_ ? tr("Next recording plan") : tr("Recording plan");
}
QStringList AudioRoutingAdapter::warnings() const {
    QStringList result;
    if (const auto plan = resolvedPlan())
        for (const auto& warning : plan->warnings)
            result.push_back(Text(warning));
    return result;
}
bool AudioRoutingAdapter::monitorConfirmationRequired() const {
    const auto plan = resolvedPlan();
    return plan && plan->monitor_confirmation_required;
}
QStringList AudioRoutingAdapter::defaultMonitorTrackIds() const {
    QStringList result;
    std::visit(
        [&](const auto& intent) {
            for (const auto& id : intent.default_monitor_track_ids)
                result.push_back(IdText(id));
        },
        intent_);
    return result;
}
QString AudioRoutingAdapter::monitorSelectionOrigin() const {
    return std::visit(
        [](const auto& intent) {
            return intent.monitor_selection_origin == engine::MonitorSelectionOrigin::Default
                       ? QStringLiteral("default")
                       : QStringLiteral("user");
        },
        intent_);
}
bool AudioRoutingAdapter::fail(QString error) {
    error_ = std::move(error);
    emit changed();
    return false;
}
bool AudioRoutingAdapter::checkRevision(qulonglong expectedRevision) {
    if (saving_)
        return fail(tr("Audio settings are being saved."));
    if (expectedRevision != revision_)
        return fail(tr("Audio settings changed. Review the current plan and try again."));
    if (read_only_)
        return fail(tr("This audio configuration is read-only."));
    return true;
}
bool AudioRoutingAdapter::publish(engine::AudioIntent candidate, const engine::AudioEnvironmentSnapshot& environment,
                                  bool remember) {
    using namespace engine;
    TrackGraph1Intent graph;
    if (const auto* native = std::get_if<TrackGraph1Intent>(&candidate)) {
        graph = *native;
    } else {
        const auto materialized = MaterializeLegacyIntent(std::get<LegacyRows25Intent>(candidate), environment);
        if (!materialized.succeeded())
            return fail(Text(materialized.error()->context));
        graph = *materialized.value();
    }
    const auto valid = ValidateTrackGraph1Intent(graph);
    if (!valid.succeeded())
        return fail(Text(valid.error()->context));
    auto plan = ResolveAudioIntent(graph, PresentationEnvironment(environment, graph), {revision_});
    if (!plan.succeeded())
        return fail(Text(plan.error()->context));
    if (auto* native = std::get_if<TrackGraph1Intent>(&candidate);
        remember && native && native->monitor_selection_origin == MonitorSelectionOrigin::Default) {
        TrackGraph1Intent previous_graph;
        if (const auto* previous = std::get_if<TrackGraph1Intent>(&intent_)) {
            previous_graph = *previous;
        } else {
            const auto materialized_previous =
                MaterializeLegacyIntent(std::get<LegacyRows25Intent>(intent_), environment);
            if (!materialized_previous.succeeded())
                return fail(Text(materialized_previous.error()->context));
            previous_graph = *materialized_previous.value();
        }
        const auto previous_plan =
            ResolveAudioIntent(previous_graph, PresentationEnvironment(environment, previous_graph));
        if (!previous_plan.succeeded())
            return fail(Text(previous_plan.error()->context));
        for (const auto& id : plan.value()->default_monitor_track_ids) {
            const bool previously_recorded =
                std::any_of(previous_plan.value()->tracks.begin(), previous_plan.value()->tracks.end(),
                            [&](const auto& track) { return track.id == id; });
            if (previously_recorded)
                continue;
            if (std::find(native->default_monitor_track_ids.begin(), native->default_monitor_track_ids.end(), id) ==
                native->default_monitor_track_ids.end())
                native->default_monitor_track_ids.push_back(id);
        }
    }
    if (candidate == intent_) {
        error_.clear();
        emit changed();
        return true;
    }
    if (!save_)
        return fail(tr("Audio settings cannot be saved."));
    QString error;
    saving_ = true;
    const bool saved = save_(candidate, error);
    saving_ = false;
    if (!saved)
        return fail(error.isEmpty() ? tr("Audio settings could not be saved.") : error);
    if (remember) {
        undo_.push_back(intent_);
        if (undo_.size() > 32)
            undo_.pop_front();
    }
    intent_ = std::move(candidate);
    pending_.reset();
    error_.clear();
    ++revision_;
    emit changed();
    return true;
}
bool AudioRoutingAdapter::editGraph(qulonglong expectedRevision, GraphEdit edit) {
    if (!checkRevision(expectedRevision))
        return false;
    const auto snapshot = environment();
    if (const auto* legacy = std::get_if<engine::LegacyRows25Intent>(&intent_)) {
        auto materialized = engine::MaterializeLegacyIntent(*legacy, snapshot);
        if (!materialized.succeeded())
            return fail(Text(materialized.error()->context));
        auto candidate = *materialized.value();
        if (!edit(candidate))
            return false;
        const auto valid = engine::ValidateTrackGraph1Intent(candidate);
        if (!valid.succeeded())
            return fail(Text(valid.error()->context));
        pending_ = PendingEdit{std::move(edit), snapshot};
        error_.clear();
        emit changed();
        return false;
    }
    auto candidate = std::get<engine::TrackGraph1Intent>(intent_);
    return edit(candidate) && publish(std::move(candidate), snapshot);
}
bool AudioRoutingAdapter::patchLegacy(engine::LegacyRowId row, const engine::LegacyRowPatch& patch) {
    const auto candidate = engine::ApplyLegacyRowEdit(std::get<engine::LegacyRows25Intent>(intent_), row, patch);
    return candidate.succeeded() ? publish(*candidate.value(), environment()) : fail(Text(candidate.error()->context));
}
bool AudioRoutingAdapter::addEmptyTrack(qulonglong expectedRevision) {
    const auto id = engine::NewId<engine::AudioTrackId>();
    return editGraph(expectedRevision, [id](auto& graph) {
        uint32_t order = 0;
        for (const auto& track : graph.tracks)
            order = std::max(order, track.order + 1);
        engine::AudioTrackSpec track;
        track.id = id;
        track.order = order;
        graph.tracks.push_back(track);
        return true;
    });
}
bool AudioRoutingAdapter::setTrackName(QString trackId, QString name, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    name = name.trimmed();
    if (const auto* legacy = std::get_if<engine::LegacyRows25Intent>(&intent_)) {
        auto candidate = *legacy;
        const auto row = std::find_if(candidate.rows.begin(), candidate.rows.end(),
                                      [&](const auto& item) { return Matches(item.anchor_track_id, trackId); });
        if (row == candidate.rows.end())
            return fail(tr("Audio track was not found."));
        engine::TrackGraph1Intent validation;
        engine::AudioTrackSpec track;
        track.id = row->anchor_track_id;
        track.name_kind = engine::AudioTrackNameKind::Custom;
        track.name = name.toStdString();
        validation.tracks.push_back(track);
        const auto valid = engine::ValidateTrackGraph1Intent(validation);
        if (!valid.succeeded())
            return fail(Text(valid.error()->context));
        row->name_kind = track.name_kind;
        row->name = track.name;
        return publish(std::move(candidate), environment());
    }
    return editGraph(expectedRevision, [this, trackId, name](auto& graph) {
        const auto track = Find(graph.tracks, trackId);
        if (track == graph.tracks.end())
            return fail(tr("Audio track was not found."));
        track->name_kind = engine::AudioTrackNameKind::Custom;
        track->name = name.toStdString();
        return true;
    });
}
bool AudioRoutingAdapter::reorderTrackById(const QString& trackId, int delta, qulonglong expectedRevision) {
    return editGraph(expectedRevision, [this, trackId, delta](auto& graph) {
        if (delta != -1 && delta != 1)
            return fail(tr("Move a track up or down one position."));
        std::stable_sort(graph.tracks.begin(), graph.tracks.end(),
                         [](const auto& a, const auto& b) { return a.order < b.order; });
        const auto track = Find(graph.tracks, trackId);
        if (track == graph.tracks.end())
            return fail(tr("Audio track was not found."));
        const auto position = std::distance(graph.tracks.begin(), track);
        const auto target = position + delta;
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(graph.tracks.size()))
            return fail(tr("The track is already at the edge of the list."));
        std::swap(graph.tracks[static_cast<size_t>(position)], graph.tracks[static_cast<size_t>(target)]);
        for (size_t index = 0; index < graph.tracks.size(); ++index)
            graph.tracks[index].order = static_cast<uint32_t>(index);
        return true;
    });
}
bool AudioRoutingAdapter::assignSource(const QString& sourceId, const QString& trackId, int mode,
                                       qulonglong expectedRevision) {
    return editGraph(expectedRevision, [this, sourceId, trackId, mode](auto& graph) {
        if (mode != Move && mode != Additional)
            return fail(tr("Choose Move or Additional for the audio source."));
        const auto source = Find(graph.sources, sourceId);
        const auto track = Find(graph.tracks, trackId);
        if (source == graph.sources.end() || track == graph.tracks.end())
            return fail(tr("Audio source or track was not found."));
        const auto source_id = source->id;
        const auto track_id = track->id;
        if (mode == Move)
            std::erase_if(graph.routes, [&](const auto& route) {
                return route.source_id == source_id && route.track_id != track_id;
            });
        if (std::none_of(graph.routes.begin(), graph.routes.end(), [&](const auto& route) {
                return route.source_id == source_id && route.track_id == track_id;
            })) {
            engine::AudioRouteSpec route;
            route.id = engine::NewId<engine::AudioRouteId>();
            route.source_id = source_id;
            route.track_id = track_id;
            graph.routes.push_back(route);
        }
        return true;
    });
}
bool AudioRoutingAdapter::removeRoute(const QString& routeId, qulonglong expectedRevision) {
    return editGraph(expectedRevision, [this, routeId](auto& graph) {
        const auto route = Find(graph.routes, routeId);
        if (route == graph.routes.end())
            return fail(tr("Audio route was not found."));
        graph.routes.erase(route);
        return true;
    });
}
bool AudioRoutingAdapter::setRouteGain(QString routeId, double gainDb, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (!std::isfinite(gainDb) || gainDb < -60 || gainDb > 24)
        return fail(tr("Route gain must be between -60 and +24 dB."));
    if (const auto* legacy = std::get_if<engine::LegacyRows25Intent>(&intent_)) {
        const auto row = std::find_if(legacy->rows.begin(), legacy->rows.end(),
                                      [&](const auto& item) { return Matches(item.source_id, routeId); });
        if (row == legacy->rows.end())
            return fail(tr("Audio route was not found."));
        engine::LegacyRowPatch patch;
        patch.gain_db = float(gainDb);
        return patchLegacy(row->row_id, patch);
    }
    return editGraph(expectedRevision, [this, routeId, gainDb](auto& graph) {
        const auto route = Find(graph.routes, routeId);
        if (route == graph.routes.end())
            return fail(tr("Audio route was not found."));
        route->gain_db = float(gainDb);
        return true;
    });
}
bool AudioRoutingAdapter::setRouteMute(QString routeId, bool muted, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (const auto* legacy = std::get_if<engine::LegacyRows25Intent>(&intent_)) {
        const auto row = std::find_if(legacy->rows.begin(), legacy->rows.end(),
                                      [&](const auto& item) { return Matches(item.source_id, routeId); });
        if (row == legacy->rows.end())
            return fail(tr("Audio route was not found."));
        engine::LegacyRowPatch patch;
        patch.muted = muted;
        return patchLegacy(row->row_id, patch);
    }
    return editGraph(expectedRevision, [this, routeId, muted](auto& graph) {
        const auto route = Find(graph.routes, routeId);
        if (route == graph.routes.end())
            return fail(tr("Audio route was not found."));
        route->muted = muted;
        return true;
    });
}
bool AudioRoutingAdapter::setSourceParticipation(QString sourceId, bool participating, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (const auto* legacy = std::get_if<engine::LegacyRows25Intent>(&intent_)) {
        const auto row = std::find_if(legacy->rows.begin(), legacy->rows.end(),
                                      [&](const auto& item) { return Matches(item.source_id, sourceId); });
        if (row == legacy->rows.end())
            return fail(tr("Audio source was not found."));
        engine::LegacyRowPatch patch;
        patch.enabled = participating;
        return patchLegacy(row->row_id, patch);
    }
    return editGraph(expectedRevision, [this, sourceId, participating](auto& graph) {
        const auto source = Find(graph.sources, sourceId);
        if (source == graph.sources.end())
            return fail(tr("Audio source was not found."));
        source->participation = participating;
        return true;
    });
}
bool AudioRoutingAdapter::setSourceMute(QString sourceId, bool muted, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (active_) {
        const auto source = std::find_if(active_->sources.begin(), active_->sources.end(),
                                         [&](const auto& item) { return Matches(item.spec.id, sourceId); });
        if (source == active_->sources.end() ||
            std::none_of(active_->tracks.begin(), active_->tracks.end(), [&](const auto& track) {
                return std::any_of(track.routes.begin(), track.routes.end(),
                                   [&](const auto& route) { return route.source_id == source->spec.id; });
            }))
            return fail(tr("Only sources in the active recording plan can be muted live."));
        if (!live_mute_ || !live_mute_(source->spec.id, muted))
            return fail(tr("The active audio source could not be muted."));
        source->spec.muted = muted;
        error_.clear();
        ++revision_;
        emit changed();
        return true;
    }
    if (const auto* legacy = std::get_if<engine::LegacyRows25Intent>(&intent_)) {
        const auto row = std::find_if(legacy->rows.begin(), legacy->rows.end(),
                                      [&](const auto& item) { return Matches(item.source_id, sourceId); });
        if (row == legacy->rows.end())
            return fail(tr("Audio source was not found."));
        engine::LegacyRowPatch patch;
        patch.muted = muted;
        return patchLegacy(row->row_id, patch);
    }
    return editGraph(expectedRevision, [this, sourceId, muted](auto& graph) {
        const auto source = Find(graph.sources, sourceId);
        if (source == graph.sources.end())
            return fail(tr("Audio source was not found."));
        source->muted = muted;
        return true;
    });
}
bool AudioRoutingAdapter::setTrackRecordEnabled(QString trackId, bool enabled, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (const auto* legacy = std::get_if<engine::LegacyRows25Intent>(&intent_)) {
        auto candidate = *legacy;
        const auto row = std::find_if(candidate.rows.begin(), candidate.rows.end(),
                                      [&](const auto& item) { return Matches(item.anchor_track_id, trackId); });
        if (row == candidate.rows.end())
            return fail(tr("Audio track was not found."));
        row->record_enabled = enabled;
        return publish(std::move(candidate), environment());
    }
    return editGraph(expectedRevision, [this, trackId, enabled](auto& graph) {
        const auto track = Find(graph.tracks, trackId);
        if (track == graph.tracks.end())
            return fail(tr("Audio track was not found."));
        track->record_enabled = enabled;
        return true;
    });
}
bool AudioRoutingAdapter::setTrackRole(QString trackId, int role, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (role < int(engine::AudioTrackRole::Standard) || role > int(engine::AudioTrackRole::Mix))
        return fail(tr("Audio track role is invalid."));
    if (const auto* legacy = std::get_if<engine::LegacyRows25Intent>(&intent_)) {
        auto candidate = *legacy;
        const auto row = std::find_if(candidate.rows.begin(), candidate.rows.end(),
                                      [&](const auto& item) { return Matches(item.anchor_track_id, trackId); });
        if (row == candidate.rows.end())
            return fail(tr("Audio track was not found."));
        row->role = static_cast<engine::AudioTrackRole>(role);
        return publish(std::move(candidate), environment());
    }
    return editGraph(expectedRevision, [this, trackId, role](auto& graph) {
        const auto track = Find(graph.tracks, trackId);
        if (track == graph.tracks.end())
            return fail(tr("Audio track was not found."));
        track->role = static_cast<engine::AudioTrackRole>(role);
        return true;
    });
}
bool AudioRoutingAdapter::setDefaultMonitorSelection(const QStringList& trackIds, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    std::vector<engine::AudioTrackId> ids;
    const auto graph = presentationGraph();
    for (const auto& text : trackIds) {
        const auto id = engine::FromUuid<engine::AudioTrackId>(text.toStdString());
        if (!id || std::find(ids.begin(), ids.end(), *id) != ids.end() ||
            std::none_of(graph.tracks.begin(), graph.tracks.end(), [&](const auto& track) { return track.id == *id; }))
            return fail(tr("Monitor selection contains an invalid or duplicate track."));
        ids.push_back(*id);
    }
    auto candidate = intent_;
    std::visit(
        [&](auto& value) {
            value.default_monitor_track_ids = ids;
            value.monitor_selection_origin = engine::MonitorSelectionOrigin::User;
        },
        candidate);
    return publish(std::move(candidate), environment());
}
bool AudioRoutingAdapter::confirmLegacyConversion(qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (!pending_ || !legacy())
        return fail(tr("There is no pending track layout conversion."));
    auto materialized =
        engine::MaterializeLegacyIntent(std::get<engine::LegacyRows25Intent>(intent_), pending_->environment);
    if (!materialized.succeeded())
        return fail(Text(materialized.error()->context));
    auto candidate = *materialized.value();
    const auto snapshot = pending_->environment;
    return pending_->edit(candidate) && publish(std::move(candidate), snapshot);
}
bool AudioRoutingAdapter::cancelLegacyConversion(qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    pending_.reset();
    error_.clear();
    emit changed();
    return true;
}
bool AudioRoutingAdapter::undo(qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (undo_.empty())
        return fail(tr("There is no audio change to undo."));
    const auto candidate = undo_.back();
    if (!publish(candidate, environment(), false))
        return false;
    undo_.pop_back();
    emit changed();
    return true;
}
bool AudioRoutingAdapter::setSourceDetails(QString sourceId, QVariantMap details, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (const auto* legacy = std::get_if<engine::LegacyRows25Intent>(&intent_)) {
        const auto row = std::find_if(legacy->rows.begin(), legacy->rows.end(),
                                      [&](const auto& item) { return Matches(item.source_id, sourceId); });
        if (row == legacy->rows.end())
            return fail(tr("Audio source was not found."));
        if (row->row.kind != engine::AudioSourceKind::Mic)
            return fail(tr("Device and processing settings are shared by the legacy microphone sources only."));
        auto candidate = *legacy;
        engine::AudioSourceSpec microphone;
        microphone.id = row->source_id;
        microphone.binding = candidate.microphone_binding;
        microphone.channel_mode = candidate.microphone_channel_mode;
        microphone.dsp = candidate.microphone_dsp;
        microphone.post_dsp_gain_linear = candidate.microphone_gain_linear;
        QString error;
        if (!ApplySourceDetails(microphone, details, error))
            return fail(std::move(error));
        candidate.microphone_binding = std::move(microphone.binding);
        candidate.microphone_channel_mode = microphone.channel_mode;
        candidate.microphone_dsp = microphone.dsp;
        candidate.microphone_gain_linear = microphone.post_dsp_gain_linear;
        return publish(std::move(candidate), environment());
    }
    return editGraph(expectedRevision, [this, sourceId, details](auto& graph) {
        const auto source = Find(graph.sources, sourceId);
        if (source == graph.sources.end())
            return fail(tr("Audio source was not found."));
        QString error;
        return ApplySourceDetails(*source, details, error) || fail(std::move(error));
    });
}
bool AudioRoutingAdapter::addSource(int bindingKind, const QString& name, qulonglong expectedRevision) {
    if (!checkRevision(expectedRevision))
        return false;
    if (bindingKind < int(engine::AudioBindingKind::PlaybackEndpoint) ||
        bindingKind > int(engine::AudioBindingKind::IndependentProcess))
        return fail(tr("Choose a supported audio source kind."));
    engine::AudioSourceSpec source;
    source.id = engine::NewId<engine::AudioSourceId>();
    source.name = name.trimmed().toStdString();
    source.binding.kind = static_cast<engine::AudioBindingKind>(bindingKind);
    engine::TrackGraph1Intent name_validation;
    engine::AudioTrackSpec label;
    label.id.bytes = source.id.bytes;
    label.name_kind = engine::AudioTrackNameKind::Custom;
    label.name = source.name;
    name_validation.tracks.push_back(label);
    const auto valid_name = engine::ValidateTrackGraph1Intent(name_validation);
    if (!valid_name.succeeded())
        return fail(tr("Source name must contain 1 to 128 characters without control characters."));
    if (source.binding.kind == engine::AudioBindingKind::CaptureTargetProcess ||
        source.binding.kind == engine::AudioBindingKind::IndependentProcess) {
        const auto snapshot = environment();
        if (!snapshot.process_loopback_supported)
            return fail(tr("Application audio capture is unsupported on this platform."));
        if (!snapshot.window_target)
            return fail(tr("Select a window capture target before adding application audio."));
        if (source.binding.kind == engine::AudioBindingKind::IndependentProcess) {
            if (!snapshot.capture_target_process || snapshot.capture_target_process->process_id == 0 ||
                snapshot.capture_target_process->creation_time == 0 ||
                std::find(snapshot.confirmed_processes.begin(), snapshot.confirmed_processes.end(),
                          *snapshot.capture_target_process) == snapshot.confirmed_processes.end())
                return fail(tr("The selected application identity must be confirmed before adding its audio source."));
            source.binding.process = snapshot.capture_target_process;
        }
    }
    return editGraph(expectedRevision, [source = std::move(source)](auto& graph) {
        graph.sources.push_back(source);
        return true;
    });
}
} // namespace exosnap::quick

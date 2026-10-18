#include "EditExportAdapter.h"
#include <QCoreApplication>

#include "EditRenderText.h"
#include "EditSessionAdapter.h"

#include "models/EditTimelineModel.h"
#include "models/MarkerSidecar.h"
#include "models/OutputSettingsModel.h"
#include "models/VideoSettingsModel.h"
#include "services/AtomicFileOps.h"
#include "services/MediaMetadataPublication.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QVariantMap>
#include <capability/encoder_device_resolver.h>
#include <exosnap/engine/edit_export_plan.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <windows.h>

#include <exosnap/engine/mp4_remuxer.h>

#include <algorithm>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

namespace exosnap::quick {
namespace {

QVariantMap option(const QString& value, const QString& label) {
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), value);
    entry.insert(QStringLiteral("label"), label);
    // ExoSelect renders the capability owner's own verdict; both output choices
    // are always available here, so the reason line stays empty.
    entry.insert(QStringLiteral("selectable"), true);
    entry.insert(QStringLiteral("reason"), QString());
    return entry;
}

bool SameSelection(const std::optional<engine::MonitorSelectionSnapshot>& a,
                   const std::optional<engine::MonitorSelectionSnapshot>& b) {
    if (a.has_value() != b.has_value())
        return false;
    if (!a)
        return true;
    if (a->media_id != b->media_id || a->revision != b->revision || a->overlap_confirmed != b->overlap_confirmed ||
        a->tracks.size() != b->tracks.size())
        return false;
    for (size_t index = 0; index < a->tracks.size(); ++index) {
        const auto& left = a->tracks[index];
        const auto& right = b->tracks[index];
        if (left.track_id != right.track_id || left.gain_linear != right.gain_linear || left.muted != right.muted)
            return false;
    }
    if (bool(a->unverified_asset_binding) != bool(b->unverified_asset_binding))
        return false;
    if (a->unverified_asset_binding)
        return a->unverified_asset_binding->file_identity == b->unverified_asset_binding->file_identity &&
               a->unverified_asset_binding->file_size == b->unverified_asset_binding->file_size &&
               a->unverified_asset_binding->sha256 == b->unverified_asset_binding->sha256;
    return true;
}

bool SameMediaIdentity(const engine::EditMediaMetadata& before, const engine::EditMediaMetadata& now) {
    if (!before.content_identity_valid || !now.content_identity_valid || before.file_identity != now.file_identity ||
        before.file_size != now.file_size || before.content_sha256 != now.content_sha256 || !now.error.empty() ||
        before.duration_us != now.duration_us || before.width != now.width || before.height != now.height ||
        before.video_codec != now.video_codec || before.audio_metadata.status != now.audio_metadata.status)
        return false;
    const auto& a = before.audio_metadata;
    const auto& b = now.audio_metadata;
    if (bool(a.unverified_asset_binding) != bool(b.unverified_asset_binding))
        return false;
    if (a.unverified_asset_binding)
        return a.unverified_asset_binding->file_identity == b.unverified_asset_binding->file_identity &&
               a.unverified_asset_binding->file_size == b.unverified_asset_binding->file_size &&
               a.unverified_asset_binding->sha256 == b.unverified_asset_binding->sha256;
    return a.manifest && b.manifest && a.manifest->media_id == b.manifest->media_id &&
           a.manifest->recording_id == b.manifest->recording_id && a.manifest->segment_id == b.manifest->segment_id;
}

QVariantMap AudioRow(const engine::ContainerAudioTrack& track, uint64_t asset, bool verified) {
    return {{QStringLiteral("asset"), QVariant::fromValue<qulonglong>(asset)},
            {QStringLiteral("id"), verified ? QString::fromStdString(engine::ToUuid(track.track_id)) : QString()},
            {QStringLiteral("name"), QString::fromStdString(track.name)},
            {QStringLiteral("role"), QString::fromStdString(track.role)},
            {QStringLiteral("position"), track.stream_index.value + 1}};
}

QString IssueText(const engine::EditExportPlan& plan, const engine::EditExportIssue& issue) {
    using engine::EditExportIssueCode;
    switch (issue.code) {
    case EditExportIssueCode::EmptySpan:
        return QCoreApplication::translate("EditExportAdapter", "The selected export range is empty.");
    case EditExportIssueCode::InvalidSelection:
        return QCoreApplication::translate("EditExportAdapter", "Select clips or a valid time range for export.");
    case EditExportIssueCode::PartialJunctionOwnership:
        return QCoreApplication::translate("EditExportAdapter",
                                           "The selected clips contain only one side of a transition.");
    case EditExportIssueCode::InvalidTimeline:
        return QCoreApplication::translate("EditExportAdapter", "The timeline contains an invalid clip.");
    case EditExportIssueCode::MissingSource:
        return QCoreApplication::translate("EditExportAdapter", "The timeline contains unavailable media.");
    case EditExportIssueCode::SourceChanged:
        return QCoreApplication::translate("EditExportAdapter",
                                           "A source changed after it was imported. Import it again.");
    case EditExportIssueCode::MixSelectionRequired:
        return QCoreApplication::translate(
            "EditExportAdapter", "Choose which audio streams of the recording form the mix before exporting.");
    case EditExportIssueCode::OriginalRecipeChanged:
        return QCoreApplication::translate("EditExportAdapter",
                                           "Original streams can only be kept for unedited source cuts. Export "
                                           "the finished result instead.");
    case EditExportIssueCode::UnsupportedAudioLayout:
        return QCoreApplication::translate("EditExportAdapter",
                                           "An audio stream has a channel layout that cannot be mixed to stereo.");
    case EditExportIssueCode::BoundaryConsentRequired:
        return QCoreApplication::translate("EditExportAdapter",
                                           "Copying would move the cut boundaries. Export a rendered result instead.");
    case EditExportIssueCode::ResourceLimitExceeded:
        return QCoreApplication::translate("EditExportAdapter", "The export needs more resources than are available.");
    case EditExportIssueCode::DestinationAliasesSource:
        return QCoreApplication::translate("EditExportAdapter",
                                           "The destination would replace an original source. Choose another file.");
    case EditExportIssueCode::UnsupportedProfile:
        break;
    }
    if (plan.intent.container == engine::EditExportContainer::Mp4)
        return QCoreApplication::translate("EditExportAdapter",
                                           "MP4 is not available for the finished result yet. Choose MKV.");
    if (issue.scope == engine::EditExportIssueScope::Audio)
        return QCoreApplication::translate("EditExportAdapter",
                                           "The timeline audio cannot be exported with this output.");
    return plan.facts.original_copy_qualified
               ? QCoreApplication::translate("EditExportAdapter",
                                             "This edit cannot be rendered here. To copy the source cut, keep the "
                                             "original streams in the export details.")
               : QCoreApplication::translate("EditExportAdapter",
                                             "The timeline video cannot be exported with this output.");
}

QString RenderVideoCodec(engine::VideoCodec codec) {
    switch (codec) {
    case engine::VideoCodec::Av1:
        return QStringLiteral("AV1");
    case engine::VideoCodec::H264:
        return QStringLiteral("H.264");
    case engine::VideoCodec::Hevc:
        return QStringLiteral("HEVC");
    }
    return {};
}

} // namespace

struct EditExportAdapter::PreparedExport {
    std::vector<engine::TimelineExportClip> recipe;
    engine::TimelineRenderSnapshot snapshot;
    engine::RecorderConfig config;
    std::filesystem::path output;
    MarkerExportPlan marker_plan;
    std::vector<edit::Clip> clips;
    std::vector<edit::Transition> transitions;
    std::vector<edit::Asset> assets;
    uint64_t monitoring_revision = 0;
    uint64_t workspace_revision = 0;
    bool to_mp4 = false;
    bool replace_existing = false;
    engine::TimelineExportAssessment copy_assessment;
    engine::EditRenderPlan plan;
    QVariantMap outcome;
    // Timelines built from per-stream imports use the export planner; older
    // linked imports keep the source-copy/render classification below.
    engine::EditExportIntent intent;
    bool input_mix_validated = true;
    std::shared_ptr<const engine::EditExportPlan> edit_plan;

    [[nodiscard]] bool Planned() const {
        return std::all_of(snapshot.timeline.clips.begin(), snapshot.timeline.clips.end(),
                           [](const auto& clip) { return clip.stream.has_value(); });
    }

    bool SameWorkspace(const edit::Workspace& workspace) const {
        if (workspace.revision() != workspace_revision || workspace.clips() != clips ||
            workspace.transitions() != transitions || workspace.monitoringRevision() != monitoring_revision ||
            workspace.assets().size() != assets.size())
            return false;
        for (size_t index = 0; index < assets.size(); ++index) {
            const auto& a = assets[index];
            const auto& b = workspace.assets()[index];
            if (a.id != b.id || a.path != b.path || a.state != b.state ||
                !SameSelection(a.monitor_selection, b.monitor_selection))
                return false;
        }
        return true;
    }

    bool PreparePlanned(std::string& error, const std::atomic<bool>& cancelled) {
        const auto scoped = engine::SelectEditExportSnapshot(snapshot, intent);
        const auto bounds = engine::EditExportBounds(scoped.timeline, intent);
        std::set<uint64_t> referenced;
        for (const auto& clip : scoped.timeline.clips) {
            const auto track = std::find_if(scoped.timeline.tracks.begin(), scoped.timeline.tracks.end(),
                                            [&](const auto& value) { return value.id == clip.track; });
            if (intent.mode == engine::EditExportMode::OriginalStreams ||
                (clip.enabled && (track == scoped.timeline.tracks.end() || track->enabled) &&
                 clip.start_us < bounds.second && clip.end_us() > bounds.first))
                referenced.insert(clip.asset);
        }
        for (auto& source : snapshot.sources) {
            if (!referenced.contains(source.id))
                continue;
            source.metadata = engine::ProbeEditMedia(source.path, {}, {}, &cancelled);
            const auto imported =
                std::find_if(assets.begin(), assets.end(), [&](const auto& asset) { return asset.id == source.id; });
            if (imported != assets.end() && imported->source_descriptor && source.metadata.content_identity_valid &&
                source.metadata.content_sha256 != imported->source_descriptor->asset_identity_fingerprint) {
                error = QCoreApplication::translate("EditExportAdapter",
                                                    "A source changed after it was imported. Import it again.")
                            .toStdString();
                return false;
            }
            if (cancelled.load()) {
                error = "Export preparation cancelled.";
                return false;
            }
        }
        intent.container = to_mp4 ? engine::EditExportContainer::Mp4 : engine::EditExportContainer::Matroska;
        intent.destination = output;
        intent.replace_destination = replace_existing;
        auto facts = engine::AssessEditExportFacts(snapshot, intent, input_mix_validated, &cancelled);
        edit_plan = engine::PrepareEditExport(snapshot, intent, std::move(facts));
        if (!edit_plan->ready()) {
            error = IssueText(*edit_plan, edit_plan->issues.front()).toStdString();
            return false;
        }
        const auto& planned_snapshot = edit_plan->snapshot;
        const auto first_video = std::find_if(
            planned_snapshot.timeline.clips.begin(), planned_snapshot.timeline.clips.end(), [&](const auto& clip) {
                const auto track =
                    std::find_if(planned_snapshot.timeline.tracks.begin(), planned_snapshot.timeline.tracks.end(),
                                 [&](const auto& value) { return value.id == clip.track; });
                return !clip.audio && clip.enabled &&
                       (track == planned_snapshot.timeline.tracks.end() || track->enabled) &&
                       clip.start_us < edit_plan->span_out_us && clip.end_us() > edit_plan->span_in_us;
            });
        const auto first_source =
            first_video == planned_snapshot.timeline.clips.end()
                ? planned_snapshot.sources.end()
                : std::find_if(planned_snapshot.sources.begin(), planned_snapshot.sources.end(),
                               [&](const auto& source) { return source.id == first_video->asset; });
        if (first_source == planned_snapshot.sources.end() && edit_plan->video_path != engine::EditVideoPath::Render) {
            error = "Export has no available video source.";
            return false;
        }
        // One row per source stream: splitting a clip multiplies its audio clips,
        // not the tracks the export mixes.
        QVariantList selected;
        QVariantList excluded;
        std::set<std::pair<uint64_t, int>> selected_streams;
        std::set<std::pair<uint64_t, int>> excluded_streams;
        for (const auto& clip : snapshot.timeline.clips) {
            if (!clip.audio)
                continue;
            const auto key = std::make_pair(clip.asset, static_cast<int>(clip.stream->container_index));
            if (!(clip.enabled ? selected_streams : excluded_streams).insert(key).second)
                continue;
            const auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                                             [&](const auto& value) { return value.id == clip.asset; });
            const int position = static_cast<int>(clip.stream->container_index) + 1;
            QVariantMap row{{QStringLiteral("asset"), QVariant::fromValue<qulonglong>(clip.asset)},
                            {QStringLiteral("position"), position}};
            QString name;
            if (source != snapshot.sources.end())
                for (const auto& track : source->metadata.audio_metadata.actual_tracks)
                    if (track.track_id == clip.stream->stream_identity) {
                        name = QString::fromStdString(track.name);
                        row.insert(QStringLiteral("role"), QString::fromStdString(track.role));
                    }
            // Foreign media rarely names its streams; the reviewer still has to
            // tell which file and stream a row stands for.
            if (name.isEmpty())
                name = QCoreApplication::translate("EditExportAdapter", "%1, stream %2")
                           .arg(source != snapshot.sources.end()
                                    ? QFileInfo(QString::fromStdWString(source->path.wstring())).fileName()
                                    : QCoreApplication::translate("EditExportAdapter", "Missing source"))
                           .arg(position);
            row.insert(QStringLiteral("name"), name);
            (clip.enabled ? selected : excluded).push_back(row);
        }
        QVariantList output_audio;
        QString audio_summary;
        switch (edit_plan->audio_path) {
        case engine::EditAudioPath::RenderStereo:
        case engine::EditAudioPath::CopyStereo:
            output_audio.push_back(
                QVariantMap{{QStringLiteral("name"), QCoreApplication::translate("EditExportAdapter", "Rendered Mix")},
                            {QStringLiteral("role"), QStringLiteral("mix")},
                            {QStringLiteral("codec"), to_mp4 ? QStringLiteral("AAC") : QStringLiteral("PCM24")},
                            {QStringLiteral("sampleRate"), 48000},
                            {QStringLiteral("channels"), 2}});
            audio_summary = QCoreApplication::translate("EditExportAdapter", "Stereo");
            break;
        case engine::EditAudioPath::CopyOriginalStreams: {
            const auto& metadata = first_source->metadata.audio_metadata;
            for (const auto& track : metadata.actual_tracks)
                output_audio.push_back(
                    AudioRow(track, first_source->id, metadata.status == engine::AudioMetadataStatus::Verified));
            audio_summary = QCoreApplication::translate("EditExportAdapter", "Original streams: %n audio track(s)",
                                                        nullptr, static_cast<int>(output_audio.size()));
            break;
        }
        case engine::EditAudioPath::Absent:
        case engine::EditAudioPath::Unsupported:
            audio_summary = QCoreApplication::translate("EditExportAdapter", "No audio");
            break;
        }
        const bool copy = edit_plan->video_path == engine::EditVideoPath::Copy;
        outcome = {{QStringLiteral("path"), copy ? QStringLiteral("Copy") : QStringLiteral("Render")},
                   {QStringLiteral("container"), to_mp4 ? QStringLiteral("MP4") : QStringLiteral("MKV")},
                   {QStringLiteral("videoCodec"), copy ? QString::fromStdString(first_source->metadata.video_codec)
                                                       : RenderVideoCodec(config.video_codec)},
                   {QStringLiteral("selectedTracks"), selected},
                   {QStringLiteral("excludedTracks"), excluded},
                   {QStringLiteral("outputAudioTracks"), output_audio},
                   {QStringLiteral("audioSummary"), audio_summary},
                   {QStringLiteral("videoOnly"), output_audio.empty()},
                   {QStringLiteral("replaceExisting"), replace_existing},
                   {QStringLiteral("outputFile"), QString::fromStdWString(output.filename().wstring())}};
        return true;
    }

    bool Prepare(std::string& error, const std::atomic<bool>& cancelled) {
        if (Planned())
            return PreparePlanned(error, cancelled);
        if (intent.span != engine::EditExportSpan::EntireTimeline &&
            intent.span != engine::EditExportSpan::ResultRange) {
            error = QCoreApplication::translate("EditExportAdapter",
                                                "Import this source again to export an occupied or selected range.")
                        .toStdString();
            return false;
        }
        copy_assessment = engine::AssessTimelineExport(recipe, to_mp4, &cancelled);
        std::set<uint64_t> referenced;
        std::set<uint64_t> audio_assets;
        for (const auto& clip : snapshot.timeline.clips) {
            referenced.insert(clip.asset);
            if (clip.audio)
                audio_assets.insert(clip.asset);
        }
        for (auto& source : snapshot.sources) {
            if (!referenced.contains(source.id))
                continue;
            source.metadata = engine::ProbeEditMedia(source.path, {}, {}, &cancelled);
            if (!source.metadata.error.empty()) {
                error = source.metadata.error;
                return false;
            }
            const auto& identity = source.metadata.audio_metadata;
            if (!identity.unverified_asset_binding && !identity.manifest) {
                error = "Cannot establish source identity for export review.";
                return false;
            }
        }
        plan = engine::ClassifyEditExport(snapshot, copy_assessment, to_mp4);
        if (plan.path == engine::EditExportPath::Unsupported) {
            error = plan.reason;
            return false;
        }
        if (plan.path == engine::EditExportPath::Render)
            for (const auto& source : snapshot.sources) {
                if (!referenced.contains(source.id))
                    continue;
                std::error_code path_error;
                if (std::filesystem::equivalent(output, source.path, path_error) && !path_error) {
                    error = "Render output must not replace an original source.";
                    return false;
                }
            }
        QVariantList selected;
        QVariantList excluded;
        QVariantList output_audio;
        const auto first_video = std::find_if(snapshot.timeline.clips.begin(), snapshot.timeline.clips.end(),
                                              [](const auto& clip) { return !clip.audio; });
        const auto first_source =
            first_video == snapshot.timeline.clips.end()
                ? snapshot.sources.end()
                : std::find_if(snapshot.sources.begin(), snapshot.sources.end(),
                               [&](const auto& source) { return source.id == first_video->asset; });
        if (first_source == snapshot.sources.end()) {
            error = "Export has no available video source.";
            return false;
        }
        if (plan.path == engine::EditExportPath::StreamCopy) {
            const auto& metadata = first_source->metadata.audio_metadata;
            for (const auto& track : metadata.actual_tracks)
                output_audio.push_back(
                    AudioRow(track, first_source->id, metadata.status == engine::AudioMetadataStatus::Verified));
        } else {
            for (const auto& source : snapshot.sources) {
                if (!referenced.contains(source.id))
                    continue;
                if (!source.monitor_selection) {
                    error = "Render source requires an explicit frozen monitoring selection.";
                    return false;
                }
                engine::EditPlayerEngine player;
                if (!player.Open(source.path, error, &cancelled) ||
                    !player.SetMonitorSelection(*source.monitor_selection, error))
                    return false;
                if (!audio_assets.contains(source.id) && !source.monitor_selection->tracks.empty()) {
                    error = "Video-only render requires an empty frozen monitoring selection.";
                    return false;
                }
                const auto& metadata = source.metadata.audio_metadata;
                const bool verified = metadata.status == engine::AudioMetadataStatus::Verified;
                const auto& tracks = !verified && source.monitor_selection->unverified_asset_binding
                                         ? source.monitor_selection->unverified_asset_binding->actual_tracks
                                         : metadata.actual_tracks;
                for (const auto& track : tracks) {
                    const auto choice =
                        std::find_if(source.monitor_selection->tracks.begin(), source.monitor_selection->tracks.end(),
                                     [&](const auto& item) { return item.track_id == track.track_id; });
                    auto row = AudioRow(track, source.id, verified);
                    if (choice == source.monitor_selection->tracks.end())
                        excluded.push_back(row);
                    else {
                        row.insert(QStringLiteral("gain"), choice->gain_linear);
                        row.insert(QStringLiteral("muted"), choice->muted);
                        selected.push_back(row);
                    }
                }
            }
            if (!selected.empty())
                output_audio.push_back(QVariantMap{
                    {QStringLiteral("name"), QCoreApplication::translate("EditExportAdapter", "Rendered Mix")},
                    {QStringLiteral("role"), QStringLiteral("mix")},
                    {QStringLiteral("codec"), QStringLiteral("PCM24")},
                    {QStringLiteral("sampleRate"), 48000},
                    {QStringLiteral("channels"), 2}});
        }
        outcome = {{QStringLiteral("path"), plan.path == engine::EditExportPath::StreamCopy ? QStringLiteral("Copy")
                                                                                            : QStringLiteral("Render")},
                   {QStringLiteral("container"), to_mp4 ? QStringLiteral("MP4") : QStringLiteral("MKV")},
                   {QStringLiteral("videoCodec"), plan.path == engine::EditExportPath::StreamCopy
                                                      ? QString::fromStdString(first_source->metadata.video_codec)
                                                      : RenderVideoCodec(config.video_codec)},
                   {QStringLiteral("selectedTracks"), selected},
                   {QStringLiteral("excludedTracks"), excluded},
                   {QStringLiteral("outputAudioTracks"), output_audio},
                   {QStringLiteral("videoOnly"), output_audio.empty()},
                   {QStringLiteral("replaceExisting"), replace_existing},
                   {QStringLiteral("outputFile"), QString::fromStdWString(output.filename().wstring())}};
        return true;
    }

    bool RetainUnchangedSources(std::vector<std::shared_ptr<void>>& leases, std::string& error,
                                const std::atomic<bool>& cancelled,
                                const std::function<void(uint64_t)>& source_verified) const {
        const auto& reviewed_sources = edit_plan ? edit_plan->snapshot.sources : snapshot.sources;
        const auto changed_error = "A source changed after export review. Review the export outcome again.";
        // Lock every contributing source before hashing any of them, so a
        // checked source cannot change while another source is being checked.
        for (const auto& source : reviewed_sources) {
            if (cancelled.load())
                return false;
            if (source.metadata.duration_us == 0)
                continue;
            auto acquired = engine::AcquireMediaReadLease(source.path);
            if (!acquired.succeeded()) {
                error = changed_error;
                return false;
            }
            leases.push_back(std::move(*acquired.value()));
        }
        for (const auto& source : reviewed_sources) {
            if (cancelled.load())
                return false;
            if (source.metadata.duration_us == 0)
                continue;
            if (!SameMediaIdentity(source.metadata, engine::ProbeEditMedia(source.path, {}, {}, &cancelled))) {
                error = changed_error;
                return false;
            }
            if (source_verified)
                source_verified(source.id);
        }
        return !cancelled.load();
    }
};

engine::ResolvedEncoderDevice
ResolveEditEncoderDevice(std::span<const capability::AdapterInfo> adapters,
                         std::span<const capability::AdapterEncoderCapability> adapter_caps,
                         capability::VideoCodec codec, const engine::EncoderDevicePreference& preference) {
    const auto candidates = capability::BuildEncoderDeviceCandidates(
        adapters, adapter_caps, {codec, capability::ChromaSubsampling::Cs420, capability::BitDepth::Bit8});
    engine::ResolvedEncoderDevice device;
    const bool automatic = preference.mode == engine::EncoderDevicePreference::Mode::Auto;
    int64_t compositor_luid = 0;
    if (automatic) {
        const auto selected = std::find_if(candidates.begin(), candidates.end(),
                                           [](const auto& candidate) { return candidate.usable(); });
        if (selected == candidates.end()) {
            device.reason =
                EditExportAdapter::tr("No probed encoder adapter supports this render format.").toStdString();
            return device;
        }
        // Decoded media is composed on this adapter, so its affinity is concrete without a capture source.
        compositor_luid = selected->adapter.luid;
    }
    const auto resolution = capability::ResolveEncoderDevice(candidates, preference, compositor_luid, automatic);
    if (resolution.resolved && resolution.candidate_index >= 0) {
        const auto& candidate = candidates[static_cast<size_t>(resolution.candidate_index)];
        device.valid = true;
        device.adapter_luid = candidate.adapter.luid;
        device.vendor_id = candidate.adapter.vendor_id;
        device.backend = candidate.backend;
        device.device = capability::FingerprintFromAdapter(candidate.adapter);
    }
    device.reason = resolution.reason;
    return device;
}

engine::RecorderConfig BuildEditRenderConfig(const OutputSettingsModel& output, const VideoSettingsModel& video,
                                             const capability::CapabilitySet& encoder_caps,
                                             const engine::ResolvedEncoderDevice& encoder_device) {
    engine::RecorderConfig config;
    config.encoder_device = video.encoder_device;
    config.resolved_encoder_device = encoder_device;
    config.backend_tuning =
        ResolveOutputNvencTuning(output, encoder_caps, output.video_codec, video.rate_control).tuning;
    config.cq = video.cq;
    config.rate_control_mode = video.rate_control;
    config.target_bitrate_kbps = video.bitrate_kbps;
    switch (output.video_codec) {
    case capability::VideoCodec::H264:
        config.video_codec = engine::VideoCodec::H264;
        break;
    case capability::VideoCodec::Hevc:
        config.video_codec = engine::VideoCodec::Hevc;
        break;
    case capability::VideoCodec::Av1:
        config.video_codec = engine::VideoCodec::Av1;
        break;
    }
    switch (video.keyframe_interval) {
    case KeyframeIntervalMode::Seconds2:
        config.keyframe_interval_secs = 2.0f;
        break;
    case KeyframeIntervalMode::Seconds1:
        config.keyframe_interval_secs = 1.0f;
        break;
    case KeyframeIntervalMode::Seconds0_5:
        config.keyframe_interval_secs = 0.5f;
        break;
    }
    return config;
}

std::filesystem::path DefaultEditExportPath(const std::filesystem::path& output_directory, bool to_mp4) {
    return output_directory / (to_mp4 ? L"ExoSnap-export.mp4" : L"ExoSnap-export.mkv");
}
bool ShouldPublishExportProgress(float fraction, int last_published_percent) {
    const int percent = fraction <= 0.0f ? 0 : fraction >= 1.0f ? 100 : static_cast<int>(fraction * 100.0f);
    return percent != last_published_percent;
}

EditExportAdapter::EditExportAdapter(QObject* parent) : QObject(parent) {
    auto* timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this] {
        if (run_ && running()) {
            const int percent = run_->percent.load();
            if (percent >= 0 && percent != progress_percent_)
                publishProgress(percent);
        }
    });
    timer->start();
}

QVariantList EditExportAdapter::profileOptions() {
    QVariantList profiles{option(QStringLiteral("match"), tr("Match source"))};
    for (const auto& label :
         {tr("YouTube 1080p"), tr("YouTube 1440p"), tr("YouTube 4K"), tr("Archive / High quality")}) {
        auto profile = option(label, label);
        profile[QStringLiteral("selectable")] = false;
        profile[QStringLiteral("reason")] = tr("Only Match source is supported for render export.");
        profiles.push_back(profile);
    }
    return profiles;
}

void EditExportAdapter::setModeKey(const QString& key) {
    if (running() || mode_key_ == key ||
        (key != QStringLiteral("finishedStereo") && key != QStringLiteral("originalStreams")))
        return;
    mode_key_ = key;
    discardPreparedExport();
    emit optionsChanged();
}

void EditExportAdapter::setSpanKey(const QString& key) {
    span_explicit_ = true;
    if (running() || span_key_ == key ||
        (key != QStringLiteral("result") && key != QStringLiteral("entire") && key != QStringLiteral("occupied") &&
         key != QStringLiteral("time") && key != QStringLiteral("selected")))
        return;
    span_key_ = key;
    discardPreparedExport();
    emit optionsChanged();
}

QVariantList EditExportAdapter::spanOptions() const {
    return {
        QVariantMap{{QStringLiteral("key"), QStringLiteral("entire")},
                    {QStringLiteral("label"), tr("Entire Timeline")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("occupied")},
                    {QStringLiteral("label"), tr("Occupied Content")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("result")},
                    {QStringLiteral("label"), tr("Edited result range")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("time")}, {QStringLiteral("label"), tr("I/O time range")}},
        QVariantMap{{QStringLiteral("key"), QStringLiteral("selected")},
                    {QStringLiteral("label"), tr("Selected clips")}}};
}

void EditExportAdapter::setTimeSelection(qint64 in_ms, qint64 out_ms) {
    if (in_ms < 0 || out_ms <= in_ms || out_ms > (std::numeric_limits<qint64>::max)() / 1000)
        setTimeSelectionUs(-1, -1);
    else
        setTimeSelectionUs(in_ms * 1000, out_ms * 1000);
}

void EditExportAdapter::setTimeSelectionUs(qint64 in_us, qint64 out_us) {
    if (running() || (selection_in_us_ == in_us && selection_out_us_ == out_us))
        return;
    if (in_us < 0 || out_us <= in_us) {
        selection_in_us_ = selection_out_us_ = -1;
    } else {
        selection_in_us_ = in_us;
        selection_out_us_ = out_us;
    }
    discardPreparedExport();
    emit optionsChanged();
}

void EditExportAdapter::setProfileKey(const QString& key) {
    if (key != QStringLiteral("match") || profile_key_ == key)
        return;
    profile_key_ = key;
    discardPreparedExport();
    emit optionsChanged();
}

void EditExportAdapter::chooseDestination() {
    if (running())
        return;
    const QString directory =
        output_directory_ ? output_directory_() : QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    QString filter = container_key_ == QStringLiteral("mp4") ? tr("MP4 video (*.mp4)") : tr("Matroska video (*.mkv)");
    const QString path = QFileDialog::getSaveFileName(
        nullptr, tr("Export video"),
        QString::fromStdWString(DefaultEditExportPath(std::filesystem::path(directory.toStdWString()),
                                                      container_key_ == QStringLiteral("mp4"))
                                    .wstring()),
        tr("Matroska video (*.mkv);;MP4 video (*.mp4)"), &filter);
    if (path.isEmpty())
        return;
    setContainerKey(QFileInfo(path).suffix().toLower() == QStringLiteral("mp4") ? QStringLiteral("mp4")
                                                                                : QStringLiteral("mkv"));
    chosen_output_ = std::filesystem::path(path.toStdWString());
    overwrite_confirmed_ = QFileInfo::exists(path);
    startExport();
}

EditExportAdapter::~EditExportAdapter() {
    cancelAndWait();
}

void EditExportAdapter::cancelAndWait() {
    if (run_)
        run_->cancel.store(true);
    if (auto* worker = std::exchange(export_thread_, nullptr)) {
        disconnect(worker, nullptr, this, nullptr);
        worker->wait();
        delete worker;
    }
    run_.reset();
}

void EditExportAdapter::setSession(EditSessionAdapter* session) {
    session_ = session;
    if (session_ == nullptr)
        return;
    connect(session_, &EditSessionAdapter::clipChanged, this, [this]() {
        // A different clip means the last run's result no longer describes it --
        // unless that run is still in flight, which must be found again as it
        // was when the surface is re-entered.
        if (!running())
            reset();
        emit optionsChanged();
    });
    connect(session_, &EditSessionAdapter::workspaceChanged, this, [this]() {
        if (!running() && !span_explicit_) {
            const auto range = session_->workspace().snapshot()->result_range;
            const auto suggested = range.in_us > 0 || range.out_mode == edit::ResultOutMode::Fixed
                                       ? QStringLiteral("result")
                                       : QStringLiteral("entire");
            if (suggested != span_key_) {
                span_key_ = suggested;
                emit optionsChanged();
            }
        }
        if (state_ == State::Review)
            discardPreparedExport();
    });
    connect(this, &EditExportAdapter::stateChanged, session_, [this]() { session_->setExportRunning(running()); });
}

int EditExportAdapter::stateValue() const noexcept {
    return static_cast<int>(state_);
}

EditExportAdapter::State EditExportAdapter::state() const noexcept {
    return state_;
}

bool EditExportAdapter::running() const noexcept {
    return state_ == State::Preparing || state_ == State::Running || state_ == State::Cancelling;
}

int EditExportAdapter::progressPercent() const noexcept {
    return progress_percent_;
}

QString EditExportAdapter::outputPath() const {
    // Both getters are read by people, not by the filesystem: a path built from
    // a URL keeps its forward slashes, and the same folder then reads one way
    // here and another way in Settings.
    return QDir::toNativeSeparators(QString::fromStdWString(output_path_.wstring()));
}

QString EditExportAdapter::outputFileName() const {
    return QString::fromStdWString(output_path_.filename().wstring());
}

QString EditExportAdapter::outputFolder() const {
    return QDir::toNativeSeparators(QString::fromStdWString(output_path_.parent_path().wstring()));
}

const QString& EditExportAdapter::errorText() const noexcept {
    return error_text_;
}

bool EditExportAdapter::destinationFailure() const noexcept {
    return destination_failure_;
}

const QString& EditExportAdapter::containerKey() const noexcept {
    return container_key_;
}

void EditExportAdapter::setContainerKey(const QString& key) {
    const QString normalized = key == QStringLiteral("mp4") ? key : QStringLiteral("mkv");
    if (container_key_ == normalized)
        return;
    container_key_ = normalized;
    discardPreparedExport();
    emit optionsChanged();
}

const QString& EditExportAdapter::saveModeKey() const noexcept {
    return save_mode_key_;
}

void EditExportAdapter::setSaveModeKey(const QString& key) {
    const QString normalized = key == QStringLiteral("overwrite") ? key : QStringLiteral("new");
    if (save_mode_key_ == normalized)
        return;
    save_mode_key_ = normalized;
    discardPreparedExport();
    emit optionsChanged();
}

QVariantList EditExportAdapter::containerOptions() {
    return {option(QStringLiteral("mkv"), QStringLiteral("MKV")), option(QStringLiteral("mp4"), QStringLiteral("MP4"))};
}

QVariantList EditExportAdapter::saveModeOptions() {
    return {
        option(QStringLiteral("new"), QCoreApplication::translate("EditExportAdapter", "New file")),
        option(QStringLiteral("overwrite"), QCoreApplication::translate("EditExportAdapter", "Overwrite original"))};
}

bool EditExportAdapter::overwriteSelected() const {
    return save_mode_key_ == QStringLiteral("overwrite");
}

QString EditExportAdapter::destinationText() const {
    if (session_ && !session_->workspace().transitions().empty())
        return tr("Render Crossfade: SDR BT.709 video and PCM audio in Matroska. Choose an MKV destination.");
    if (overwriteSelected())
        return QCoreApplication::translate("EditExportAdapter",
                                           "Lossless stream copy\nReplaces the original recording");
    const QString extension = container_key_ == QStringLiteral("mp4") ? QStringLiteral("mp4") : QStringLiteral("mkv");
    return tr("Lossless stream copy. Choose a filename and folder (%1).").arg(extension);
}

// "Overwrite original" finishes with an atomic replace, so once it succeeds no
// copy of the original is left. The question has to come before the run starts,
// not as a report afterwards.
QString EditExportAdapter::overwritePrompt() const {
    const QString name = session_ != nullptr ? QFileInfo(session_->clipPath()).fileName() : QString();
    if (name.isEmpty())
        return QCoreApplication::translate("EditExportAdapter",
                                           "The original recording will be replaced by the exported result.\n"
                                           "The original cannot be recovered afterwards.");
    return QCoreApplication::translate("EditExportAdapter",
                                       "\xe2\x80\x9c%1\xe2\x80\x9d will be replaced by the exported result.\n"
                                       "The original cannot be recovered afterwards.")
        .arg(name);
}

bool EditExportAdapter::canExport() const noexcept {
    return !running() && state_ != State::Failed;
}

QVariantMap EditExportAdapter::reviewOutcome() const {
    return review_outcome_;
}

void EditExportAdapter::discardPreparedExport() {
    prepared_.reset();
    if (!review_outcome_.isEmpty()) {
        review_outcome_.clear();
        emit reviewChanged();
    }
    if (state_ == State::Review)
        setState(State::Options);
}

void EditExportAdapter::setState(State state) {
    if (state_ == state)
        return;
    state_ = state;
    emit stateChanged();
}

void EditExportAdapter::reset() {
    if (running())
        return;
    discardPreparedExport();
    progress_percent_ = 0;
    error_text_.clear();
    destination_failure_ = false;
    emit progressChanged();
    emit resultChanged();
    setState(State::Options);
}

void EditExportAdapter::publishProgress(int percent) {
    if (progress_percent_ == percent)
        return;
    progress_percent_ = percent;
    emit progressChanged();
}

void EditExportAdapter::retry() {
    startExport();
}

void EditExportAdapter::retryInFolder(const QUrl& folder) {
    if (state_ != State::Failed || !destination_failure_ || !folder.isLocalFile() || output_path_.empty())
        return;
    retry_output_path_ = std::filesystem::path(folder.toLocalFile().toStdWString()) / output_path_.filename();
    startExport();
}

void EditExportAdapter::cancel() {
    if (state_ != State::Running && state_ != State::Preparing)
        return;
    if (run_)
        run_->cancel.store(true);
    setState(State::Cancelling);
}

void EditExportAdapter::startExport() {
    if (running() || session_ == nullptr)
        return;
    discardPreparedExport();

    error_text_.clear();
    destination_failure_ = false;
    progress_percent_ = 0;
    emit progressChanged();
    emit resultChanged();

    if (session_->workspace().duration() <= 0) {
        error_text_ = QCoreApplication::translate("EditExportAdapter", "No edit master available for export.");
        emit resultChanged();
        setState(State::Failed);
        return;
    }

    const bool to_mp4 = container_key_ == QStringLiteral("mp4");
    const QString directory =
        output_directory_ ? output_directory_() : QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const auto suggested =
        std::filesystem::path(directory.toStdWString()) /
        (L"ExoSnap-" + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")).toStdWString() +
         (to_mp4 ? L".mp4" : L".mkv"));
    const std::filesystem::path output = retry_output_path_.value_or(chosen_output_.value_or(suggested));
    std::vector<engine::TimelineExportClip> recipe;
    engine::TimelineRenderSnapshot render_snapshot;
    render_snapshot.timeline = session_->workspace().timeline();
    for (const auto& asset : session_->workspace().assets())
        render_snapshot.sources.push_back({asset.id, std::filesystem::path(asset.path), {}, asset.monitor_selection});
    engine::EditExportIntent export_intent;
    export_intent.mode = mode_key_ == QStringLiteral("originalStreams") ? engine::EditExportMode::OriginalStreams
                                                                        : engine::EditExportMode::FinishedStereo;
    export_intent.span = span_key_ == QStringLiteral("occupied")   ? engine::EditExportSpan::OccupiedContent
                         : span_key_ == QStringLiteral("selected") ? engine::EditExportSpan::SelectedClips
                         : span_key_ == QStringLiteral("time")     ? engine::EditExportSpan::TimeRange
                         : span_key_ == QStringLiteral("result")   ? engine::EditExportSpan::ResultRange
                                                                   : engine::EditExportSpan::EntireTimeline;
    export_intent.selection_in_us = selection_in_us_;
    export_intent.selection_out_us = selection_out_us_;
    if (export_intent.span == engine::EditExportSpan::SelectedClips) {
        const auto* lead = session_->workspace().clip(session_->workspace().selection());
        for (const auto& clip : session_->workspace().clips())
            if (lead &&
                (clip.id == lead->id || (session_->linkedSelection && lead->group && clip.group == lead->group)))
                export_intent.selected_clip_ids.push_back(clip.id);
    }
    const auto scope_snapshot = engine::SelectEditExportSnapshot(render_snapshot, export_intent);
    const auto scope_bounds = engine::EditExportBounds(scope_snapshot.timeline, export_intent);
    const auto render_config = render_config_ ? render_config_() : engine::RecorderConfig{};
    std::vector<RecordingMarker> timeline_markers;
    for (const auto& clip : session_->workspace().clips()) {
        if (std::none_of(scope_snapshot.timeline.clips.begin(), scope_snapshot.timeline.clips.end(),
                         [&](const auto& value) { return value.id == clip.id; }) ||
            !clip.enabled || clip.start >= scope_bounds.second || clip.end() <= scope_bounds.first)
            continue;
        const auto track = std::find_if(session_->workspace().tracks().begin(), session_->workspace().tracks().end(),
                                        [&clip](const auto& t) { return t.id == clip.track; });
        if (track->type != edit::TrackType::Video || !track->edit_enabled)
            continue;
        const auto* asset = session_->workspace().asset(clip.asset);
        recipe.push_back({std::filesystem::path(asset->path), clip.source_in, clip.source_out, clip.start});
        for (const auto& marker : asset->markers) {
            const auto source_us = static_cast<int64_t>(marker.time_ms) * 1000;
            if (source_us < clip.source_in || source_us >= clip.source_out)
                continue;
            auto retimed = marker;
            const auto timeline_us = clip.start + source_us - clip.source_in;
            if (timeline_us < scope_bounds.first || timeline_us >= scope_bounds.second)
                continue;
            retimed.time_ms = static_cast<uint64_t>((timeline_us - scope_bounds.first) / 1000);
            timeline_markers.push_back(std::move(retimed));
        }
    }
    if (QFileInfo::exists(QString::fromStdWString(output.wstring())) && !overwrite_confirmed_) {
        error_text_ = tr("Choose a destination and confirm before replacing an existing file.");
        emit resultChanged();
        setState(State::Failed);
        return;
    }
    const bool replace_existing = overwrite_confirmed_;
    overwrite_confirmed_ = false;
    retry_output_path_.reset();
    MarkerExportPlan marker_plan = PlanMarkerSidecarForExport(output, timeline_markers);
    output_path_ = output;
    emit resultChanged();

    auto prepared = std::make_shared<PreparedExport>();
    prepared->recipe = std::move(recipe);
    prepared->snapshot = std::move(render_snapshot);
    prepared->config = render_config;
    prepared->output = output;
    prepared->marker_plan = std::move(marker_plan);
    prepared->clips = session_->workspace().clips();
    prepared->transitions = session_->workspace().transitions();
    prepared->assets = session_->workspace().assets();
    prepared->monitoring_revision = session_->workspace().monitoringRevision();
    prepared->workspace_revision = session_->workspace().revision();
    prepared->to_mp4 = to_mp4;
    prepared->replace_existing = replace_existing;
    prepared->intent = std::move(export_intent);
    const auto scoped = engine::SelectEditExportSnapshot(prepared->snapshot, prepared->intent);
    const auto bounds = engine::EditExportBounds(scoped.timeline, prepared->intent);
    for (const auto& clip : scoped.timeline.clips) {
        const auto track = std::find_if(scoped.timeline.tracks.begin(), scoped.timeline.tracks.end(),
                                        [&](const auto& value) { return value.id == clip.track; });
        if (!clip.enabled || (track != scoped.timeline.tracks.end() && !track->enabled) ||
            clip.start_us >= bounds.second || clip.end_us() <= bounds.first)
            continue;
        const auto* asset = session_->workspace().asset(clip.asset);
        if (asset && asset->mix_decision_required)
            prepared->input_mix_validated = false;
    }
    run_ = std::make_shared<RunState>();
    run_->prepared = std::move(prepared);
    setState(State::Preparing);
    export_thread_ = QThread::create([run = run_] {
        if (!run->cancel.load())
            run->ok = run->prepared->Prepare(run->error, run->cancel);
    });
    export_thread_->setParent(this);
    connect(export_thread_, &QThread::finished, this, [this, run = run_] {
        if (run_ != run)
            return;
        auto* worker = std::exchange(export_thread_, nullptr);
        worker->wait();
        delete worker;
        run_.reset();
        if (run->cancel.load()) {
            setState(State::Options);
            return;
        }
        if (!run->ok) {
            finishRun(false, QString::fromStdString(run->error),
                      QString::fromStdWString(run->prepared->output.wstring()), false);
            return;
        }
        if (!session_ || !run->prepared->SameWorkspace(session_->workspace())) {
            finishRun(false, tr("The timeline or monitoring selection changed during export preparation."),
                      QString::fromStdWString(run->prepared->output.wstring()), false);
            return;
        }
        prepared_ = run->prepared;
        review_outcome_ = prepared_->outcome;
        emit reviewChanged();
        setState(State::Review);
    });
    export_thread_->start();
}

void EditExportAdapter::confirmPreparedExport() {
    if (state_ != State::Review || !prepared_ || !session_)
        return;
    if (!prepared_->SameWorkspace(session_->workspace()) ||
        prepared_->to_mp4 != (container_key_ == QStringLiteral("mp4")) ||
        (render_config_ &&
         engine::RuntimeConfigContentKey(prepared_->config) != engine::RuntimeConfigContentKey(render_config_()))) {
        retry_output_path_ = prepared_->output;
        overwrite_confirmed_ = prepared_->replace_existing;
        discardPreparedExport();
        startExport();
        return;
    }
    beginPreparedExport();
}

void EditExportAdapter::beginPreparedExport() {
    auto prepared = std::exchange(prepared_, std::shared_ptr<PreparedExport>{});
    review_outcome_.clear();
    emit reviewChanged();
    run_ = std::make_shared<RunState>();
    setState(State::Running);
    export_thread_ = QThread::create([run = run_, prepared, source_verified = source_verified_] {
        const auto& recipe = prepared->recipe;
        const auto& render_snapshot = prepared->snapshot;
        const auto& render_config = prepared->config;
        const auto& output = prepared->output;
        const bool to_mp4 = prepared->to_mp4;
        const bool replace_existing = prepared->replace_existing;
        const auto& marker_plan = prepared->marker_plan;
        std::vector<std::shared_ptr<void>> source_leases;
        if (!prepared->RetainUnchangedSources(source_leases, run->error, run->cancel, source_verified))
            return;
        if (!prepared->edit_plan) {
            const auto reassessed = engine::AssessTimelineExport(recipe, to_mp4, &run->cancel);
            if (reassessed.eligibility != prepared->copy_assessment.eligibility ||
                engine::ClassifyEditExport(render_snapshot, reassessed, to_mp4).path != prepared->plan.path) {
                run->error = "Export eligibility changed after review. Review the export outcome again.";
                return;
            }
        }
        const auto temp_output = MakeDisposableSiblingStagingPath(output);

        auto progress_cb = [run](float fraction) -> bool {
            if (run->cancel.load())
                return false;
            const int percent = fraction <= 0.0f ? 0 : fraction >= 1.0f ? 100 : static_cast<int>(fraction * 100.0f);
            run->percent.store(percent);
            return true;
        };

        exosnap::engine::RemuxResult result;
        if (prepared->edit_plan)
            result = engine::ExecuteEditExport(*prepared->edit_plan, render_config, temp_output, progress_cb);
        else
            switch (prepared->plan.path) {
            case engine::EditExportPath::StreamCopy:
                result = engine::ExportTimelineStreamCopy(recipe, temp_output, to_mp4, progress_cb);
                break;
            case engine::EditExportPath::Render:
                result = engine::RenderEditTimeline(render_snapshot, prepared->plan, render_config, temp_output, to_mp4,
                                                    progress_cb);
                break;
            case engine::EditExportPath::Unsupported:
                result = engine::RemuxResult::Fail(-1, prepared->plan.reason);
                break;
            }
        MetadataCompanionPublicationGuard companion_guard(std::move(result.created_metadata_companions));
        bool ok = result.success;
        std::string error = result.message;

        if (ok && !StageMarkerSidecarForPublication(temp_output, output, marker_plan.markers, error, std::nullopt,
                                                    [&] { return !run->cancel.load(); }))
            ok = false;

        if (ok && run->cancel.load()) {
            ok = false;
            error = "Metadata publication cancelled";
        }
        if (ok) {
            std::error_code ec;
            const auto error_code = PublishMediaAndIdentitySidecar(temp_output, output, replace_existing);
            if (error_code)
                ec = std::error_code(static_cast<int>(error_code), std::system_category());
            if (!ec)
                companion_guard.Commit();
            if (ec) {
                ok = false;
                error = "Failed to save output file: " + ec.message();
                if (const std::string left = exosnap::DescribeFailedStagingRemoval(temp_output); !left.empty())
                    error += " (" + left + ")";
            }
        } else {
            if (const std::string left = exosnap::DescribeFailedStagingRemoval(temp_output); !left.empty())
                error += " (" + left + ")";
        }

        if (ok && marker_plan.markers.empty() && !ApplyMarkerExportPlan(marker_plan)) {
            ok = false;
            error = "MetadataIncomplete: cannot publish exported markers";
        }

        run->ok = ok;
        run->error = std::move(error);
    });
    export_thread_->setParent(this);
    connect(export_thread_, &QThread::finished, this, [this, run = run_, output = prepared->output] {
        if (run_ != run)
            return;
        auto* worker = std::exchange(export_thread_, nullptr);
        // finished precedes thread-local destruction. Join before releasing the worker's resources.
        worker->wait();
        delete worker;
        run_.reset();
        finishRun(run->ok, QString::fromStdString(run->error), QString::fromStdWString(output.wstring()),
                  run->cancel.load());
    });
    export_thread_->start();
}

void EditExportAdapter::finishRun(bool ok, const QString& error, const QString& output_path, bool cancelled) {
    output_path_ = std::filesystem::path(output_path.toStdWString());

    if (cancelled && !ok) {
        // A cancelled run left nothing behind, so the panel returns to its
        // options rather than reporting a failure the user asked for.
        progress_percent_ = 0;
        error_text_.clear();
        emit progressChanged();
        emit resultChanged();
        setState(State::Options);
        return;
    }

    if (ok) {
        error_text_.clear();
        progress_percent_ = 100;
        emit progressChanged();
        emit resultChanged();
        setState(State::Done);
        emit exportCompleted(output_path);
        return;
    }

    error_text_ = error.isEmpty()
                      ? QCoreApplication::translate("EditExportAdapter", "Unknown error")
                      : EditExportAdapter::tr("Export could not complete: %1").arg(TranslateEditRenderReason(error));
    const QString lower_error = error_text_.toLower();
    destination_failure_ =
        lower_error.contains(QStringLiteral("save output")) || lower_error.contains(QStringLiteral("permission")) ||
        lower_error.contains(QStringLiteral("denied")) || lower_error.contains(QStringLiteral("disk")) ||
        lower_error.contains(QStringLiteral("space")) || lower_error.contains(QStringLiteral("directory")) ||
        lower_error.contains(QStringLiteral("path"));
    emit resultChanged();
    setState(State::Failed);
    emit exportFailed(error_text_);
}

void EditExportAdapter::applyVisualState(State state, int percent, const QString& output_path, const QString& error) {
    progress_percent_ = std::clamp(percent, 0, 100);
    output_path_ = std::filesystem::path(output_path.toStdWString());
    error_text_ = error;
    destination_failure_ = state == State::Failed;
    emit progressChanged();
    emit resultChanged();
    setState(state);
}

void EditExportAdapter::applyVisualReview(const QVariantMap& outcome) {
    prepared_.reset();
    review_outcome_ = outcome;
    emit reviewChanged();
    setState(State::Review);
}

// The panel offers one folder action, and this is it: "explorer /select,<path>"
// opens the containing folder AND highlights the file, so it strictly contains
// what the separate "Open folder" action did.
void EditExportAdapter::revealFile() {
    const QString path = outputPath();
    if (path.isEmpty())
        return;
    // "explorer /select,<path>" highlights the file rather than opening it.
    QProcess::startDetached(QStringLiteral("explorer"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
}

} // namespace exosnap::quick

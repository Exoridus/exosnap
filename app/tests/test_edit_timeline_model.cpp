#include <gtest/gtest.h>

#include <QDir>
#include <QString>
#include <QTemporaryDir>

#include <filesystem>
#include <vector>

#include "models/EditTimelineModel.h"
#include "models/EditWorkspace.h"
#include "models/MarkerSidecar.h"
#include "models/RecordingMarker.h"

namespace exosnap {
namespace {

RecordingMarker MakeMarker(uint64_t time_ms, const char* label = "Marker") {
    RecordingMarker m;
    m.time_ms = time_ms;
    m.type = RecordingMarkerType::General;
    m.label = label;
    return m;
}

// ---- Timestamp formatting (drag feedback label) ----

TEST(EditTimelineModel, TimestampOmitsHoursForShortRecordings) {
    // Total < 1 h: "MM:SS.mmm", all fields two-digit.
    EXPECT_EQ(FormatTimelineTimestamp(0, 258000), QStringLiteral("00:00.000"));
    EXPECT_EQ(FormatTimelineTimestamp(31482, 258000), QStringLiteral("00:31.482"));
    EXPECT_EQ(FormatTimelineTimestamp(258000, 258000), QStringLiteral("04:18.000"));
}

TEST(EditTimelineModel, TimestampShowsHoursForLongRecordings) {
    // Total >= 1 h: "HH:MM:SS.mmm".
    const qint64 total = 2 * 3600000; // 2 h
    EXPECT_EQ(FormatTimelineTimestamp(0, total), QStringLiteral("00:00:00.000"));
    EXPECT_EQ(FormatTimelineTimestamp(3723456, total), QStringLiteral("01:02:03.456"));
}

TEST(EditTimelineModel, TimestampClampsNegativePositions) {
    EXPECT_EQ(FormatTimelineTimestamp(-500, 10000), QStringLiteral("00:00.000"));
}

TEST(EditTimelineModel, ClockFormatFollowsTheSameHourRule) {
    EXPECT_EQ(FormatTimelineClock(258000, 258000), QStringLiteral("04:18"));
    EXPECT_EQ(FormatTimelineClock(3723000, 2 * 3600000), QStringLiteral("01:02:03"));
}

// ---- Trim handle clamping ----

TEST(EditTimelineModel, TrimHandlesConstrainEachOther) {
    // Start can never reach (or cross) the end handle.
    EXPECT_EQ(ClampTrimStartMs(90000, 60000), 60000 - kMinTrimGapMs);
    // End can never reach (or cross) the start handle.
    EXPECT_EQ(ClampTrimEndMs(10000, 30000, 100000), 30000 + kMinTrimGapMs);
    // In-range values pass through untouched.
    EXPECT_EQ(ClampTrimStartMs(15000, 60000), 15000);
    EXPECT_EQ(ClampTrimEndMs(90000, 30000, 100000), 90000);
    // And both respect the clip bounds.
    EXPECT_EQ(ClampTrimStartMs(-5, 60000), 0);
    EXPECT_EQ(ClampTrimEndMs(200000, 30000, 100000), 100000);
}

TEST(EditTimelineModel, PlayheadClampsToClip) {
    EXPECT_EQ(ClampPlayheadMs(-10, 100000), 0);
    EXPECT_EQ(ClampPlayheadMs(50000, 100000), 50000);
    EXPECT_EQ(ClampPlayheadMs(999999, 100000), 100000);
}

// ---- Marker retiming for trimmed exports ----

TEST(EditTimelineModel, RetimeShiftsSurvivorsAndDropsTrimmedMarkers) {
    const std::vector<RecordingMarker> markers = {
        MakeMarker(5000, "before-in"),   // dropped (before trim start)
        MakeMarker(30000, "at-in"),      // kept, rebased to 0
        MakeMarker(95000, "inside"),     // kept, rebased to 65000
        MakeMarker(200000, "at-out"),    // dropped (half-open window)
        MakeMarker(250000, "after-out"), // dropped
    };
    const auto out = RetimeMarkersForTrim(markers, 30000, 200000);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].time_ms, 0u);
    EXPECT_EQ(out[0].label, "at-in");
    EXPECT_EQ(out[1].time_ms, 65000u);
    EXPECT_EQ(out[1].label, "inside");
}

TEST(EditTimelineModel, RetimeWithEmptyOrInvertedWindowYieldsNothing) {
    const std::vector<RecordingMarker> markers = {MakeMarker(1000)};
    EXPECT_TRUE(RetimeMarkersForTrim(markers, 50000, 50000).empty());
    EXPECT_TRUE(RetimeMarkersForTrim(markers, 60000, 50000).empty());
}

// ---- Marker sidecar export plan ----

TEST(MarkerSidecarExport, SidecarPathReplacesTheMediaExtension) {
    EXPECT_EQ(DeriveMarkerSidecarPath("C:/videos/clip.mkv"), std::filesystem::path("C:/videos/clip.markers.json"));
    EXPECT_EQ(DeriveMarkerSidecarPath("C:/videos/clip_edit.mp4"),
              std::filesystem::path("C:/videos/clip_edit.markers.json"));
    EXPECT_TRUE(DeriveMarkerSidecarPath({}).empty());
}

TEST(MarkerSidecarExport, PlanWritesOnlyWhenMarkersSurvive) {
    const auto with_markers = PlanMarkerSidecarForExport("C:/videos/clip_edit.mkv", {MakeMarker(1000)});
    EXPECT_TRUE(with_markers.shouldWrite());
    EXPECT_EQ(with_markers.media, QStringLiteral("clip_edit.mkv"));

    const auto without_markers = PlanMarkerSidecarForExport("C:/videos/clip_edit.mkv", {});
    EXPECT_FALSE(without_markers.shouldWrite());
    EXPECT_EQ(without_markers.sidecar_path, std::filesystem::path("C:/videos/clip_edit.markers.json"));
}

TEST(MarkerSidecarExport, ApplyWritesARetimedSidecarNextToTheExport) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path media(QDir(tmp.path()).filePath(QStringLiteral("clip_edit.mkv")).toStdWString());

    const auto plan = PlanMarkerSidecarForExport(media, {MakeMarker(0, "at-in"), MakeMarker(65000, "inside")});
    ASSERT_TRUE(ApplyMarkerExportPlan(plan));

    const auto roundtrip = ReadMarkerSidecar(plan.sidecar_path);
    ASSERT_EQ(roundtrip.size(), 2u);
    EXPECT_EQ(roundtrip[0].time_ms, 0u);
    EXPECT_EQ(roundtrip[1].time_ms, 65000u);
    EXPECT_EQ(roundtrip[1].label, "inside");
}

TEST(MarkerSidecarExport, ApplyRemovesAStaleSidecarWhenNoMarkersSurvive) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::filesystem::path media(QDir(tmp.path()).filePath(QStringLiteral("clip.mkv")).toStdWString());

    // Simulate an overwrite-original export: a sidecar from the recording
    // session already sits next to the media file.
    ASSERT_TRUE(ApplyMarkerExportPlan(PlanMarkerSidecarForExport(media, {MakeMarker(1000)})));
    const auto sidecar = DeriveMarkerSidecarPath(media);
    ASSERT_TRUE(std::filesystem::exists(sidecar));

    // The trim cut every marker away: the stale sidecar must disappear.
    ASSERT_TRUE(ApplyMarkerExportPlan(PlanMarkerSidecarForExport(media, {})));
    EXPECT_FALSE(std::filesystem::exists(sidecar));
}

edit::Asset ReplacementSource(const wchar_t* path, uint8_t identity, edit::Time duration = 10'000'000) {
    edit::Asset asset;
    asset.path = path;
    asset.duration = duration;
    asset.fps_num = 30;
    auto descriptor = std::make_shared<edit::ValidatedSourceDescriptor>();
    descriptor->asset_identity_fingerprint.fill(identity);
    const auto stream_identity = identity ? identity : uint8_t{1};
    descriptor->streams.push_back({{0, engine::AudioTrackId{{stream_identity}}, 0, engine::TimelineStreamKind::Video},
                                   {0, duration},
                                   0,
                                   {},
                                   {30, 1}});
    const engine::AudioTrackId audio{{static_cast<uint8_t>(stream_identity + 1)}};
    descriptor->streams.push_back({{0, audio, 1, engine::TimelineStreamKind::Audio},
                                   {0, duration},
                                   0,
                                   {"A_PCM/INT/LIT", {}, 48000, 2, 16, 0, "stereo"},
                                   {}});
    asset.source_descriptor = descriptor;
    asset.input_mix_selection = edit::InputMixSelection{};
    asset.input_mix_selection->asset_identity_fingerprint = descriptor->asset_identity_fingerprint;
    asset.input_mix_selection->selected_stream_identities = {audio};
    return asset;
}

TEST(EditSourceReplacement, ReplacesAllReferencesAtomicallyAndUndoRestoresExactBindings) {
    edit::Workspace workspace;
    const auto asset_id = workspace.addAsset(ReplacementSource(L"old.mkv", 11));
    ASSERT_TRUE(workspace.insertAssets({asset_id, asset_id}, 0, false));
    ASSERT_EQ(workspace.clips().size(), 4U);
    const auto before = workspace.clips();
    const auto old = *workspace.asset(asset_id);
    auto replacement = ReplacementSource(L"new.mkv", 21, 12'000'000);
    replacement.id = asset_id;
    replacement.source_version = 1;
    const auto revision = workspace.revision();
    const edit::ReplaceAssetSource command{asset_id, 0, replacement, true};
    ASSERT_TRUE(edit::PreviewEdit(*workspace.snapshot(), command).valid);
    ASSERT_EQ(workspace.apply(command, revision).status, edit::CommandStatus::Applied);
    EXPECT_EQ(workspace.revision(), revision + 1);
    ASSERT_EQ(workspace.assets().size(), 1U);
    EXPECT_EQ(workspace.asset(asset_id)->path, replacement.path);
    EXPECT_EQ(workspace.asset(asset_id)->source_version, 1U);
    EXPECT_EQ(workspace.asset(asset_id)->source_descriptor->asset_identity_fingerprint,
              replacement.source_descriptor->asset_identity_fingerprint);
    EXPECT_EQ(workspace.asset(asset_id)->input_mix_selection->selected_stream_identities,
              replacement.input_mix_selection->selected_stream_identities);
    ASSERT_EQ(workspace.clips().size(), before.size());
    for (const auto& old_clip : before) {
        const auto* current = workspace.clip(old_clip.id);
        ASSERT_NE(current, nullptr);
        ASSERT_TRUE(current->stream);
        auto expected = old_clip;
        expected.stream->stream_identity =
            replacement.source_descriptor->streams[current->stream->container_index].ref.stream_identity;
        EXPECT_EQ(*current, expected);
    }
    const auto after = workspace.clips();
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.clips(), before);
    EXPECT_EQ(workspace.asset(asset_id)->path, old.path);
    EXPECT_EQ(workspace.asset(asset_id)->source_version, 0U);
    EXPECT_EQ(workspace.asset(asset_id)->source_descriptor, old.source_descriptor);
    EXPECT_EQ(workspace.asset(asset_id)->input_mix_selection, old.input_mix_selection);
    ASSERT_TRUE(workspace.redo());
    EXPECT_EQ(workspace.clips(), after);
    EXPECT_EQ(workspace.asset(asset_id)->path, replacement.path);
    EXPECT_EQ(workspace.asset(asset_id)->source_descriptor, replacement.source_descriptor);
}

TEST(EditSourceReplacement, SourceOnlyChangesAreUndoableAndInvalidCandidatesKeepEveryBinding) {
    edit::Workspace workspace;
    const auto asset_id = workspace.addAsset(ReplacementSource(L"old.mkv", 31));
    auto replacement = ReplacementSource(L"new.mkv", 41);
    replacement.id = asset_id;
    replacement.source_version = 1;
    ASSERT_EQ(workspace.apply(edit::ReplaceAssetSource{asset_id, 0, replacement, true}, workspace.revision()).status,
              edit::CommandStatus::Applied);
    EXPECT_TRUE(workspace.clips().empty());
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.asset(asset_id)->path, L"old.mkv");
    ASSERT_TRUE(workspace.insert(asset_id));
    const auto before = workspace.clips();
    const auto revision = workspace.revision();
    const auto history = workspace.historyBytes();
    for (int fault = 0; fault < 4; ++fault) {
        auto candidate = replacement;
        candidate.source_version = 2;
        auto descriptor = std::make_shared<edit::ValidatedSourceDescriptor>(*candidate.source_descriptor);
        if (fault == 0)
            candidate.duration = 9'000'000;
        if (fault == 1)
            descriptor->streams.pop_back();
        if (fault == 2)
            descriptor->streams.back().codec.channels = 6;
        candidate.source_descriptor = descriptor;
        const auto result =
            workspace.apply(edit::ReplaceAssetSource{asset_id, fault == 3 ? 8U : 0U, candidate, true}, revision);
        EXPECT_EQ(result.status, edit::CommandStatus::Rejected) << fault;
        EXPECT_EQ(workspace.revision(), revision);
        EXPECT_EQ(workspace.historyBytes(), history);
        EXPECT_EQ(workspace.clips(), before);
        EXPECT_EQ(workspace.asset(asset_id)->path, L"old.mkv");
    }
    ASSERT_TRUE(workspace.setTrackLocked(before.front().track, true));
    replacement.source_version = 3;
    const auto locked = workspace.apply(edit::ReplaceAssetSource{asset_id, 0, replacement, true}, revision);
    ASSERT_EQ(locked.status, edit::CommandStatus::Rejected);
    ASSERT_TRUE(locked.error);
    EXPECT_EQ(locked.error->code, edit::EditErrorCode::TrackLocked);
    EXPECT_EQ(workspace.clips(), before);
}

TEST(EditSourceReplacement, UndoProofPublicationCannotRewriteAnotherVersionOrReuseItsNumber) {
    edit::Workspace workspace;
    const auto asset_id = workspace.addAsset(ReplacementSource(L"provisional.mkv", 0));
    ASSERT_TRUE(workspace.insert(asset_id));
    auto replacement = ReplacementSource(L"replacement.mkv", 51);
    replacement.id = asset_id;
    replacement.source_version = 1;
    ASSERT_EQ(workspace.apply(edit::ReplaceAssetSource{asset_id, 0, replacement, true}, workspace.revision()).status,
              edit::CommandStatus::Applied);
    ASSERT_TRUE(workspace.undo());
    const auto revision = workspace.revision();
    std::array<uint8_t, 32> observed;
    observed.fill(61);
    ASSERT_TRUE(workspace.establishAssetIdentity(asset_id, observed));
    EXPECT_EQ(workspace.revision(), revision);
    EXPECT_EQ(workspace.asset(asset_id)->source_descriptor->asset_identity_fingerprint, observed);
    auto reused = ReplacementSource(L"other.mkv", 71);
    reused.id = asset_id;
    reused.source_version = 1;
    EXPECT_FALSE(edit::PreviewEdit(*workspace.snapshot(), edit::ReplaceAssetSource{asset_id, 0, reused, true}).valid);
    EXPECT_EQ(workspace.apply(edit::ReplaceAssetSource{asset_id, 0, reused, true}, revision).status,
              edit::CommandStatus::Rejected);
    ASSERT_TRUE(workspace.redo());
    EXPECT_EQ(workspace.asset(asset_id)->source_descriptor->asset_identity_fingerprint,
              replacement.source_descriptor->asset_identity_fingerprint);
    EXPECT_EQ(workspace.asset(asset_id)->input_mix_selection->asset_identity_fingerprint,
              replacement.source_descriptor->asset_identity_fingerprint);
    ASSERT_TRUE(workspace.undo());
    EXPECT_EQ(workspace.asset(asset_id)->source_descriptor->asset_identity_fingerprint, observed);
    EXPECT_EQ(workspace.asset(asset_id)->input_mix_selection->asset_identity_fingerprint, observed);
    EXPECT_EQ(workspace.snapshot()->source_version_high_watermark, 1U);
}

TEST(EditRecordedDefaultModel, VersionAndFingerprintGuardsPreserveAuthoredOverrideAndFinalizationIsIdempotent) {
    auto source = ReplacementSource(L"recorded.mkv", 11);
    source.source_version = 7;
    source.mix_decision_required = true;
    source.input_mix_selection->selected_stream_identities.clear();
    auto descriptor = std::make_shared<edit::ValidatedSourceDescriptor>(*source.source_descriptor);
    auto alternative = descriptor->streams.back();
    alternative.ref.container_index = 2;
    alternative.ref.stream_identity.bytes[0] = 99;
    descriptor->streams.push_back(alternative);
    source.source_descriptor = descriptor;
    edit::Workspace workspace;
    const auto asset = workspace.addAsset(source);
    ASSERT_TRUE(workspace.insert(asset));
    const auto stem = std::find_if(workspace.clips().begin(), workspace.clips().end(),
                                   [](const auto& clip) { return clip.stream && clip.stream->container_index == 2; });
    ASSERT_NE(stem, workspace.clips().end());
    const auto stem_id = stem->id;
    ASSERT_EQ(workspace.apply(edit::SetClipEnabled{stem_id, true}, workspace.revision()).status,
              edit::CommandStatus::Applied);
    const auto before = workspace.clips();
    const auto revision = workspace.revision();
    edit::InputMixSelection recorded;
    recorded.authority = edit::InputMixAuthority::VerifiedRecordedDefault;
    recorded.asset_identity_fingerprint = descriptor->asset_identity_fingerprint;
    recorded.selected_stream_identities = {descriptor->streams[1].ref.stream_identity};
    auto verified_descriptor = std::make_shared<edit::ValidatedSourceDescriptor>(*descriptor);
    verified_descriptor->audio_identities_verified = true;
    EXPECT_FALSE(workspace.establishRecordedDefault(asset, 6, recorded, verified_descriptor));
    auto wrong = recorded;
    wrong.asset_identity_fingerprint[0] ^= 1;
    EXPECT_FALSE(workspace.establishRecordedDefault(asset, 7, wrong, verified_descriptor));
    EXPECT_EQ(workspace.clips(), before);
    EXPECT_EQ(workspace.revision(), revision);
    ASSERT_TRUE(workspace.establishRecordedDefault(asset, 7, recorded, verified_descriptor));
    EXPECT_EQ(workspace.revision(), revision);
    EXPECT_TRUE(workspace.clip(stem_id)->enabled);
    EXPECT_FALSE(workspace.clip(stem_id)->original_enabled);
    const auto monitoring = workspace.monitoringRevision();
    const auto bytes = workspace.historyBytes();
    EXPECT_FALSE(workspace.establishRecordedDefault(asset, 7, recorded, verified_descriptor));
    EXPECT_EQ(workspace.monitoringRevision(), monitoring);
    EXPECT_EQ(workspace.historyBytes(), bytes);
    ASSERT_TRUE(workspace.undo());
    EXPECT_FALSE(workspace.clip(stem_id)->enabled);
    EXPECT_FALSE(workspace.asset(asset)->mix_decision_required);
    ASSERT_TRUE(workspace.redo());
    EXPECT_TRUE(workspace.clip(stem_id)->enabled);
    EXPECT_FALSE(workspace.clip(stem_id)->original_enabled);
}
} // namespace
} // namespace exosnap

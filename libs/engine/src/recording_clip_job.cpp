#include "audio_media_manifest_adapter.h"
#include "clip_committed_packet_reader.h"
#include "codec_random_access.h"
#include "committed_matroska_view.h"
#include "matroska_stream_writer.h"
#include "recording_media_index.h"
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/color_metadata.h>
#include <exosnap/engine/recording_clip.h>
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <windows.h>

namespace exosnap::engine {
namespace {

constexpr size_t kExtentPage = 256;
constexpr size_t kClipSegmentLimit = 4096;
constexpr size_t kMaxPacketBytes = 4 * 1024 * 1024;
constexpr uint64_t kStartupMetadataBudgetBytes = 2 * 1024 * 1024;
static_assert(kExtentPage * (2 * sizeof(ClusterExtent) + 2 * sizeof(uint64_t) + 64) < 128 * 1024);
static_assert(3 * kClipSegmentLimit * sizeof(ClipCopiedSourceRange) < 1024 * 1024);

uint64_t AddBytes(uint64_t left, uint64_t right) noexcept {
    return right > (std::numeric_limits<uint64_t>::max)() - left ? (std::numeric_limits<uint64_t>::max)()
                                                                 : left + right;
}

uint64_t ArrayBytes(size_t capacity, size_t element_bytes) noexcept {
    if (element_bytes && capacity > (std::numeric_limits<uint64_t>::max)() / element_bytes)
        return (std::numeric_limits<uint64_t>::max)();
    return static_cast<uint64_t>(capacity) * element_bytes;
}

uint64_t StringBytes(const std::string& value) noexcept {
    return AddBytes(value.capacity(), 1);
}

uint64_t CodecPayloadBytes(const MediaCodecDescriptor& codec) noexcept {
    uint64_t bytes = StringBytes(codec.codec_id);
    bytes = AddBytes(bytes, codec.codec_private.capacity());
    return AddBytes(bytes, StringBytes(codec.channel_layout));
}

uint64_t TrackPayloadBytes(const ContainerAudioTrack& track) noexcept {
    uint64_t bytes = StringBytes(track.name);
    bytes = AddBytes(bytes, StringBytes(track.role));
    bytes = AddBytes(bytes, CodecPayloadBytes(track.codec));
    return AddBytes(bytes, ArrayBytes(track.source_ids.capacity(), sizeof(AudioSourceId)));
}

uint64_t ManifestBytes(const AudioManifest1& manifest) noexcept {
    uint64_t bytes = sizeof(AudioManifest1);
    bytes = AddBytes(bytes, ArrayBytes(manifest.derived_from_media_ids.capacity(), sizeof(MediaId)));
    bytes = AddBytes(bytes, ArrayBytes(manifest.sources.capacity(), sizeof(AudioManifestSource)));
    bytes = AddBytes(bytes, ArrayBytes(manifest.tracks.capacity(), sizeof(AudioManifestTrack)));
    bytes = AddBytes(bytes, ArrayBytes(manifest.default_monitor_track_ids.capacity(), sizeof(AudioTrackId)));
    for (const auto& track : manifest.tracks) {
        bytes = AddBytes(bytes, TrackPayloadBytes(track.container));
        bytes = AddBytes(bytes, ArrayBytes(track.routes.capacity(), sizeof(AudioManifestRoute)));
        bytes = AddBytes(bytes, ArrayBytes(track.derived_from_track_ids.capacity(), sizeof(AudioTrackId)));
    }
    return bytes;
}

EngineError ClipError(EngineErrorCode code, const std::string& context, int ffmpeg = 0) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::Clip;
    error.context = context;
    if (ffmpeg != 0)
        error.ffmpeg_code = ffmpeg;
    return error;
}

bool IsCancelled(const std::atomic<bool>* cancelled) {
    return cancelled && cancelled->load(std::memory_order_acquire);
}

struct SpoolExtent {
    uint64_t file_begin = 0;
    uint64_t file_end = 0;
    uint64_t readable_end = 0;
};
static_assert(sizeof(SpoolExtent) == 24);

class ExtentSpool {
  public:
    explicit ExtentSpool(const std::filesystem::path& staging_path) {
        auto path = staging_path;
        path += L"." + std::filesystem::path(ToUuid(NewId<MediaId>())).wstring() + L".extents.tmp";
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    }

    ~ExtentSpool() {
        if (file_ != INVALID_HANDLE_VALUE)
            CloseHandle(file_);
    }

    ExtentSpool(const ExtentSpool&) = delete;
    ExtentSpool& operator=(const ExtentSpool&) = delete;

    bool opened() const noexcept {
        return file_ != INVALID_HANDLE_VALUE;
    }

    bool Write(const SpoolExtent& extent) {
        DWORD written = 0;
        return WriteFile(file_, &extent, sizeof(extent), &written, nullptr) && written == sizeof(extent);
    }

    bool Rewind() {
        LARGE_INTEGER start{};
        return SetFilePointerEx(file_, start, nullptr, FILE_BEGIN) != FALSE;
    }

    bool Read(SpoolExtent& extent) {
        DWORD read = 0;
        return ReadFile(file_, &extent, sizeof(extent), &read, nullptr) && read == sizeof(extent);
    }

  private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
};

std::optional<FileIdentity> IdentityOf(HANDLE handle) {
    FILE_ID_INFO info{};
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info)))
        return std::nullopt;
    FileIdentity identity;
    identity.volume_serial = info.VolumeSerialNumber;
    std::copy(std::begin(info.FileId.Identifier), std::end(info.FileId.Identifier), identity.file_id.begin());
    return identity;
}

bool SameTrackPlan(const RecordingMediaDescriptor& first, const RecordingMediaDescriptor& next) {
    const auto color = [](const ColorMetadata& value) {
        return std::tie(value.primaries, value.transfer, value.matrix, value.range, value.bits_per_channel, value.hdr,
                        value.max_content_light_level, value.max_frame_average_light_level, value.has_mastering_display,
                        value.mastering_display_primary_r_x, value.mastering_display_primary_r_y,
                        value.mastering_display_primary_g_x, value.mastering_display_primary_g_y,
                        value.mastering_display_primary_b_x, value.mastering_display_primary_b_y,
                        value.mastering_display_white_point_x, value.mastering_display_white_point_y,
                        value.mastering_display_max_luminance, value.mastering_display_min_luminance);
    };
    if (first.continuity_group != next.continuity_group || first.video.codec_id != next.video.codec_id ||
        first.video.codec_private != next.video.codec_private || first.video_width != next.video_width ||
        first.video_height != next.video_height || first.frame_rate_num != next.frame_rate_num ||
        first.frame_rate_den != next.frame_rate_den || color(first.color) != color(next.color) ||
        first.audio_tracks.size() != next.audio_tracks.size())
        return false;
    for (size_t index = 0; index < first.audio_tracks.size(); ++index) {
        const auto& a = first.audio_tracks[index];
        const auto& b = next.audio_tracks[index];
        if (a.track_id != b.track_id || a.name != b.name || a.role != b.role || a.codec.codec_id != b.codec.codec_id ||
            a.codec.sample_rate != b.codec.sample_rate || a.codec.channels != b.codec.channels ||
            a.codec.bit_depth != b.codec.bit_depth || a.codec.channel_layout != b.codec.channel_layout ||
            a.encoder_instance_id != b.encoder_instance_id)
            return false;
        auto left = a.codec.codec_private;
        auto right = b.codec.codec_private;
        if (a.codec.codec_id == "A_OPUS" && left.size() >= 12 && right.size() >= 12) {
            left[10] = left[11] = 0;
            right[10] = right[11] = 0;
        }
        if (left != right)
            return false;
    }
    return true;
}

struct JobState {
    JobState(const ClipBoundaryPlan& frozen_plan, const MediaIndexSnapshot& frozen_snapshot,
             std::span<const ClipPinnedSource> pinned_sources, const std::filesystem::path& output_path,
             const std::atomic<bool>* cancellation, const ClipJobIoControl* io_control)
        : plan(frozen_plan), snapshot(frozen_snapshot), sources(pinned_sources), staging_path(output_path),
          cancelled(cancellation), control(io_control) {
    }
    const ClipBoundaryPlan& plan;
    const MediaIndexSnapshot& snapshot;
    std::span<const ClipPinnedSource> sources;
    const std::filesystem::path& staging_path;
    const std::atomic<bool>* cancelled;
    const ClipJobIoControl* control;
    MatroskaStreamWriter writer;
    RecordingClipDraft draft;
    uint64_t source_bytes_read = 0;
    uint64_t staging_bytes_written = 0;
    bool opened = false;
    bool saw_start = false;
    bool saw_end = false;
    std::vector<int64_t> last_audio_end_r_ns;
    std::vector<uint64_t> output_delay_ns;
    std::optional<EngineError> failure;

    bool Fail(EngineError error) {
        failure = std::move(error);
        return false;
    }

    bool BeforeIo(ClipJobIoPhase phase, uint64_t bytes) const {
        if (IsCancelled(cancelled))
            return false;
        try {
            return !control || !control->before_io || control->before_io(phase, bytes);
        } catch (...) {
            return false;
        }
    }

    void AfterIo(ClipJobIoPhase phase, uint64_t bytes) {
        if (phase == ClipJobIoPhase::Reading)
            source_bytes_read += bytes;
        else
            staging_bytes_written += bytes;
        if (control && control->on_progress)
            try {
                control->on_progress(phase, source_bytes_read, staging_bytes_written);
            } catch (...) {
                // Progress reporting cannot unwind through FFmpeg or the container writer.
            }
    }

    bool PrepareWriter(const RecordingMediaDescriptor& descriptor) {
        MatroskaStreamConfig config;
        if ((descriptor.video.codec_id != "V_MPEG4/ISO/AVC" && descriptor.video.codec_id != "V_MPEGH/ISO/HEVC" &&
             descriptor.video.codec_id != "V_AV1") ||
            descriptor.video_width == 0 || descriptor.video_height == 0 ||
            descriptor.audio_tracks.size() > config.audio_tracks.size())
            return Fail(ClipError(EngineErrorCode::UnsupportedFormat, "Source track layout cannot be copied"));
        config.output_path = staging_path.string();
        config.video_codec_id = descriptor.video.codec_id;
        config.video_codec_private = descriptor.video.codec_private;
        config.encode_width = descriptor.video_width;
        config.encode_height = descriptor.video_height;
        config.color = descriptor.color;
        config.audio_track_count = static_cast<uint32_t>(descriptor.audio_tracks.size());
        for (size_t index = 0; index < descriptor.audio_tracks.size(); ++index) {
            const auto& track = descriptor.audio_tracks[index];
            if (index && (track.codec.codec_id != descriptor.audio_tracks[0].codec.codec_id ||
                          track.codec.sample_rate != descriptor.audio_tracks[0].codec.sample_rate ||
                          track.codec.channels != descriptor.audio_tracks[0].codec.channels ||
                          track.codec.bit_depth != descriptor.audio_tracks[0].codec.bit_depth))
                return Fail(ClipError(EngineErrorCode::UnsupportedFormat, "Source audio tracks have mixed formats"));
            if (track.codec.codec_id == "A_OPUS")
                config.audio_codec = StreamAudioCodec::Opus;
            else if (track.codec.codec_id == "A_PCM/INT/LIT" || track.codec.codec_id == "A_PCM/FLOAT/IEEE")
                config.audio_codec = StreamAudioCodec::Pcm;
            else
                return Fail(ClipError(EngineErrorCode::UnsupportedFormat, "Source audio codec cannot be copied"));
            config.audio_sample_rate = track.codec.sample_rate;
            config.audio_channels = track.codec.channels;
            config.audio_bit_depth = track.codec.bit_depth;
            config.audio_float = track.codec.codec_id == "A_PCM/FLOAT/IEEE";
            config.audio_tracks[index].codec_private = track.codec.codec_private;
            config.audio_tracks[index].codec_delay_samples = track.codec.codec_delay_samples;
        }
        config.frame_rate_num = descriptor.frame_rate_num;
        config.frame_rate_den = descriptor.frame_rate_den;
        config.path_pre_reserved = false;
        config.stream_cues_to_disk = true;
        config.collect_media_index = false;
        config.reorder_window_max_bytes = 6 * 1024 * 1024;
        config.reorder_window_max_packets = 4096;
        config.max_cluster_payload_bytes = 6 * 1024 * 1024;
        config.before_write = [this](OutputIoOperation operation, uint64_t bytes) {
            const auto phase =
                operation == OutputIoOperation::TrailerWrite ? ClipJobIoPhase::Finalizing : ClipJobIoPhase::Remuxing;
            return BeforeIo(phase, bytes);
        };
        config.after_write = [this](OutputIoOperation operation, uint64_t bytes) {
            const auto phase =
                operation == OutputIoOperation::TrailerWrite ? ClipJobIoPhase::Finalizing : ClipJobIoPhase::Remuxing;
            AfterIo(phase, bytes);
        };
        config.timeline_duration_ns = static_cast<uint64_t>(plan.actual_end_r_ns - plan.actual_begin_r_ns);
        output_delay_ns.resize(descriptor.audio_tracks.size());
        for (size_t index = 0; index < descriptor.audio_tracks.size(); ++index) {
            const auto& track = descriptor.audio_tracks[index];
            config.audio_tracks[index].name = track.name;
            if (track.codec.codec_id == "A_OPUS") {
                if (track.codec.sample_rate != 48000 || config.audio_tracks[index].codec_private.size() < 19 ||
                    !track.packet_duration_ns || !track.preroll_ns ||
                    *track.preroll_ns >
                        static_cast<uint64_t>((std::numeric_limits<uint16_t>::max)()) * 1000000000ULL / 48000)
                    return Fail(ClipError(EngineErrorCode::UnsupportedFormat, "Opus clip priming is not qualified"));
                const uint64_t lead = static_cast<uint64_t>(plan.actual_begin_r_ns - plan.read_begin_r_ns);
                const uint64_t source_delay =
                    static_cast<uint64_t>(track.codec.codec_delay_samples) * 1000000000ULL / 48000;
                const uint64_t delay = plan.actual_begin_r_ns == descriptor.segment_origin_r_ns ? source_delay : lead;
                if (delay < *track.preroll_ns && plan.actual_begin_r_ns != descriptor.segment_origin_r_ns)
                    return Fail(
                        ClipError(EngineErrorCode::IndexNotReady, "Complete Opus decoder preroll is unavailable"));
                const uint64_t samples = delay * 48000 / 1000000000ULL;
                if (samples > (std::numeric_limits<uint16_t>::max)() || samples * 1000000000ULL / 48000 != delay)
                    return Fail(ClipError(EngineErrorCode::UnsupportedFormat, "Opus discard is not sample-exact"));
                auto& output = config.audio_tracks[index];
                if (*track.packet_duration_ns > (std::numeric_limits<uint64_t>::max)() / 48000)
                    return Fail(ClipError(EngineErrorCode::UnsupportedFormat, "Opus packet duration overflows"));
                const uint64_t packet_samples = *track.packet_duration_ns * 48000 / 1000000000ULL;
                if (packet_samples == 0 || packet_samples > (std::numeric_limits<uint32_t>::max)())
                    return Fail(ClipError(EngineErrorCode::UnsupportedFormat, "Opus packet duration is invalid"));
                config.opus_frame_samples = static_cast<uint32_t>(packet_samples);
                if (static_cast<uint64_t>(config.opus_frame_samples) * 1000000000ULL / 48000 !=
                    *track.packet_duration_ns)
                    return Fail(
                        ClipError(EngineErrorCode::UnsupportedFormat, "Opus packet duration is not sample-exact"));
                output.codec_private[10] = static_cast<uint8_t>(samples);
                output.codec_private[11] = static_cast<uint8_t>(samples >> 8);
                output.codec_delay_samples = static_cast<uint32_t>(samples);
                output_delay_ns[index] = delay;
            } else if (track.codec.codec_id == "A_PCM/INT/LIT" || track.codec.codec_id == "A_PCM/FLOAT/IEEE") {
                if (track.codec.codec_delay_samples != 0 || plan.required_audio_preroll_ns != 0)
                    return Fail(ClipError(EngineErrorCode::UnsupportedFormat, "PCM clip has unexpected priming"));
            } else {
                return Fail(ClipError(EngineErrorCode::UnsupportedFormat,
                                      "Audio codec has no qualified clip decoder-start contract"));
            }
        }
        if (!descriptor.audio_tracks.empty()) {
            if (!sources.front().audio_manifest || !ValidateAudioManifest1(*sources.front().audio_manifest).succeeded())
                return Fail(ClipError(EngineErrorCode::MetadataIncomplete,
                                      "Verified audio provenance is required for clip copy"));
            auto manifest = *sources.front().audio_manifest;
            if (manifest.recording_id != descriptor.recording_id || manifest.segment_id != descriptor.segment_id ||
                manifest.media_id != descriptor.media_id || manifest.tracks.size() != descriptor.audio_tracks.size())
                return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure,
                                      "Audio provenance does not match the pinned source"));
            manifest.media_id = NewId<MediaId>();
            manifest.segment_id = NewId<SegmentId>();
            manifest.segment_origin_r_ns = 0;
            manifest.derived_from_media_ids.clear();
            for (const auto& source : sources)
                manifest.derived_from_media_ids.push_back(source.descriptor->media_id);
            for (size_t index = 0; index < manifest.tracks.size(); ++index) {
                auto& track = manifest.tracks[index].container;
                if (track.track_id != descriptor.audio_tracks[index].track_id || track.stream_index.value != index)
                    return Fail(
                        ClipError(EngineErrorCode::SourceIntegrityFailure, "Audio provenance track mapping changed"));
                track.track_uid = index + 2;
                track.codec.codec_private = config.audio_tracks[index].codec_private;
                track.codec.codec_delay_samples = config.audio_tracks[index].codec_delay_samples;
                track.first_decoded_sample_r.reset();
            }
            if (!ValidateAudioManifest1(manifest).succeeded())
                return Fail(ClipError(EngineErrorCode::MetadataIncomplete, "Derived clip provenance is invalid"));
            config.audio_manifest = manifest;
            draft.output_manifest = std::move(manifest);
        }
        if (!writer.Open(config)) {
            const auto cancelled_io = writer.error().find("Clip output I/O was cancelled") != std::string::npos;
            return Fail(ClipError(cancelled_io ? EngineErrorCode::Cancelled : EngineErrorCode::UnavailableSource,
                                  "Cannot open clip staging writer: " + writer.error()));
        }
        draft.audio_packets.resize(descriptor.audio_tracks.size());
        draft.audio_boundaries.resize(descriptor.audio_tracks.size());
        for (size_t index = 0; index < descriptor.audio_tracks.size(); ++index) {
            auto& boundary = draft.audio_boundaries[index];
            boundary.track_id = descriptor.audio_tracks[index].track_id;
            boundary.packet_duration_ns = descriptor.audio_tracks[index].packet_duration_ns;
            boundary.decoder_discard_samples = config.audio_tracks[index].codec_delay_samples;
        }
        last_audio_end_r_ns.assign(descriptor.audio_tracks.size(), (std::numeric_limits<int64_t>::min)());
        opened = true;
        if (control && control->on_staging_created)
            control->on_staging_created();
        return true;
    }

    bool ConsumePage(const ClipPinnedSource& source, std::vector<ClusterExtent> page, uint64_t readable_end,
                     ClipCopiedSourceRange& range) {
        if (IsCancelled(cancelled))
            return Fail(ClipError(EngineErrorCode::Cancelled, "Clip cancelled"));
        auto opened_view = CommittedMatroskaView::OpenFromPinnedHandle(source.descriptor, std::move(page), readable_end,
                                                                       source.borrowed_handle);
        if (!opened_view.succeeded())
            return Fail(*opened_view.error());
        auto view = std::move(*opened_view.value());
        view->SetIoControl(
            cancelled, [this](uint64_t bytes) { return BeforeIo(ClipJobIoPhase::Reading, bytes); },
            [this](uint64_t bytes) { AfterIo(ClipJobIoPhase::Reading, bytes); });
        if (!opened && !PrepareWriter(*source.descriptor))
            return false;
        ClipCommittedPacketReader reader(*view, source.descriptor->immutable_header.size(), kMaxPacketBytes);
        for (;;) {
            auto next = reader.Next();
            if (!next.succeeded())
                return Fail(*next.error());
            if (!next.value()->has_value())
                break;
            auto packet = std::move(**next.value());
            if (IsCancelled(cancelled)) {
                view->Cancel();
                return Fail(ClipError(EngineErrorCode::Cancelled, "Clip cancelled"));
            }
            if (packet.track_number == 0 || packet.track_number > source.descriptor->audio_tracks.size() + 1 ||
                packet.bytes.empty())
                return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Invalid committed packet"));
            const size_t stream = static_cast<size_t>(packet.track_number - 1);
            draft.peak_packet_bytes = (std::max)(draft.peak_packet_bytes, static_cast<uint64_t>(packet.bytes.size()));
            const auto& exact = packet.timestamps;
            const bool video = stream == 0;
            if (!video && source.descriptor->audio_tracks[stream - 1].codec.sample_rate == 0)
                return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Invalid source audio rate"));
            const uint64_t source_delay_ns =
                video ? 0
                      : static_cast<uint64_t>(source.descriptor->audio_tracks[stream - 1].codec.codec_delay_samples) *
                            1000000000ULL / source.descriptor->audio_tracks[stream - 1].codec.sample_rate;
            if (exact.pts_ns > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) ||
                source_delay_ns > exact.pts_ns)
                return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Invalid source codec delay"));
            const auto effective_pts = static_cast<int64_t>(exact.pts_ns - source_delay_ns);
            if (source_delay_ns > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) ||
                exact.dts_ns < (std::numeric_limits<int64_t>::min)() + static_cast<int64_t>(source_delay_ns))
                return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Source decode time underflows"));
            const auto effective_dts = exact.dts_ns - static_cast<int64_t>(source_delay_ns);
            const auto pts_r = SegmentLocalToRecordingNs(source.descriptor->segment_origin_r_ns, effective_pts);
            const auto dts_r = SegmentLocalToRecordingNs(source.descriptor->segment_origin_r_ns, effective_dts);
            if (!pts_r || !dts_r)
                return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Source timeline overflows"));
            if (video) {
                if (saw_end) {
                    continue;
                }
                if (plan.end_rap && source.descriptor->segment_id == plan.end_rap->segment_id &&
                    *pts_r == plan.end_rap->pts_r_ns &&
                    QualifyClosedRandomAccess(source.descriptor->video.codec_id, source.descriptor->video.codec_private,
                                              packet.bytes, packet.is_key) == RandomAccessKind::ClosedGop)
                    saw_end = true;
                if (saw_end) {
                    continue;
                }
                if (!saw_start) {
                    if (*pts_r != plan.start_rap.pts_r_ns) {
                        continue;
                    }
                    if (QualifyClosedRandomAccess(source.descriptor->video.codec_id,
                                                  source.descriptor->video.codec_private, packet.bytes,
                                                  packet.is_key) != RandomAccessKind::ClosedGop)
                        return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure,
                                              "Clip does not begin at the planned closed access"));
                    saw_start = true;
                }
                if (*pts_r < plan.actual_begin_r_ns || *pts_r >= plan.actual_end_r_ns)
                    return Fail(ClipError(EngineErrorCode::UnsupportedFormat,
                                          "Closed GOP displays beyond its planned interval"));
            } else {
                const size_t audio = static_cast<size_t>(stream - 1);
                if (*pts_r < plan.read_begin_r_ns || *pts_r >= plan.actual_end_r_ns ||
                    *pts_r < last_audio_end_r_ns[audio]) {
                    continue;
                }
                const auto& track = source.descriptor->audio_tracks[audio];
                uint64_t duration = track.packet_duration_ns.value_or(0);
                if (!duration &&
                    (track.codec.codec_id == "A_PCM/INT/LIT" || track.codec.codec_id == "A_PCM/FLOAT/IEEE") &&
                    track.codec.sample_rate && track.codec.channels && track.codec.bit_depth % 8 == 0) {
                    const uint64_t frame_bytes =
                        static_cast<uint64_t>(track.codec.channels) * (track.codec.bit_depth / 8);
                    if (frame_bytes && static_cast<uint64_t>(packet.bytes.size()) % frame_bytes == 0)
                        duration = static_cast<uint64_t>(packet.bytes.size()) / frame_bytes * 1000000000ULL /
                                   track.codec.sample_rate;
                }
                if (*pts_r < 0 || !duration ||
                    duration > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)() - *pts_r))
                    return Fail(
                        ClipError(EngineErrorCode::MetadataIncomplete, "Audio packet duration authority is missing"));
                if (last_audio_end_r_ns[audio] != (std::numeric_limits<int64_t>::min)() &&
                    *pts_r != last_audio_end_r_ns[audio])
                    return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Committed audio has a packet gap"));
                auto& boundary = draft.audio_boundaries[audio];
                if (!boundary.first_copied_packet_r_ns) {
                    boundary.first_copied_packet_r_ns = *pts_r;
                    if (track.codec.codec_id == "A_OPUS") {
                        if (!track.packet_duration_ns ||
                            output_delay_ns[audio] > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
                            return Fail(
                                ClipError(EngineErrorCode::MetadataIncomplete, "Opus clip discard is not measurable"));
                        const uint64_t whole_packets = (static_cast<uint64_t>(*boundary.decoder_discard_samples) / 960);
                        const uint64_t offset = whole_packets * *track.packet_duration_ns;
                        if (offset > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)() - *pts_r))
                            return Fail(
                                ClipError(EngineErrorCode::SourceIntegrityFailure, "Opus audible start overflows"));
                        boundary.first_audible_r_ns = *pts_r + static_cast<int64_t>(offset);
                    } else {
                        boundary.first_audible_r_ns = *pts_r;
                        boundary.packet_duration_ns = duration;
                    }
                }
                last_audio_end_r_ns[audio] = *pts_r + static_cast<int64_t>(duration);
                boundary.last_copied_packet_end_r_ns = last_audio_end_r_ns[audio];
                boundary.last_audible_r_ns = last_audio_end_r_ns[audio];
            }
            MuxPacket output;
            output.track_num = static_cast<uint64_t>(stream + 1);
            output.is_key = video ? packet.is_key : true;
            output.bytes = std::move(packet.bytes);
            const int64_t local_pts = *pts_r - plan.actual_begin_r_ns;
            const int64_t local_dts = *dts_r - plan.actual_begin_r_ns;
            if (video) {
                if (local_pts < 0)
                    return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Negative clip video PTS"));
                output.pts_ns = static_cast<uint64_t>(local_pts);
                output.dts_ns = local_dts;
            } else {
                const int64_t new_delay_ns = static_cast<int64_t>(output_delay_ns[stream - 1]);
                if (local_pts < -new_delay_ns || local_dts < (std::numeric_limits<int64_t>::min)() + new_delay_ns)
                    return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Audio clip time underflows"));
                output.pts_ns = static_cast<uint64_t>(local_pts + new_delay_ns);
                output.dts_ns = local_dts + new_delay_ns;
            }
            if (!writer.Push(std::move(output))) {
                const auto budget = writer.error().find("budget exceeded") != std::string::npos;
                const auto cancelled_io = writer.error().find("Clip output I/O was cancelled") != std::string::npos;
                return Fail(ClipError(cancelled_io ? EngineErrorCode::Cancelled
                                      : budget     ? EngineErrorCode::ResourceBudgetExceeded
                                                   : EngineErrorCode::StorageInsufficient,
                                      "Clip staging write failed: " + writer.error()));
            }
            if (video) {
                const auto& descriptor = *source.descriptor;
                if (!descriptor.frame_rate_num || !descriptor.frame_rate_den)
                    return Fail(
                        ClipError(EngineErrorCode::MetadataIncomplete, "Video frame duration authority is missing"));
                const uint64_t duration =
                    static_cast<uint64_t>(descriptor.frame_rate_den) * 1000000000ULL / descriptor.frame_rate_num;
                if (!duration || duration > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)() - *pts_r))
                    return Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Copied video coverage overflows"));
                range.begin_r_ns = (std::min)(range.begin_r_ns, *pts_r);
                range.end_r_ns = (std::max)(range.end_r_ns,
                                            (std::min)(plan.actual_end_r_ns, *pts_r + static_cast<int64_t>(duration)));
            }
            draft.peak_writer_window_bytes =
                (std::max)(draft.peak_writer_window_bytes, static_cast<uint64_t>(writer.peak_window_bytes()));
            if (video)
                ++draft.video_packets;
            else
                ++draft.audio_packets[static_cast<size_t>(stream - 1)];
        }
        if (view->last_error())
            return Fail(*view->last_error());
        return true;
    }
};

} // namespace

uint64_t EstimateRecordingClipDescriptorBytes(const RecordingMediaDescriptor& descriptor) noexcept {
    uint64_t bytes = sizeof(RecordingMediaDescriptor);
    bytes = AddBytes(bytes, StringBytes(descriptor.path));
    bytes = AddBytes(bytes, CodecPayloadBytes(descriptor.video));
    bytes = AddBytes(bytes, ArrayBytes(descriptor.audio_tracks.capacity(), sizeof(ContainerAudioTrack)));
    bytes = AddBytes(bytes, ArrayBytes(descriptor.default_monitor_track_ids.capacity(), sizeof(AudioTrackId)));
    bytes = AddBytes(bytes, descriptor.immutable_header.capacity());
    for (const auto& track : descriptor.audio_tracks)
        bytes = AddBytes(bytes, TrackPayloadBytes(track));
    if (descriptor.audio_manifest)
        bytes = AddBytes(bytes, ManifestBytes(*descriptor.audio_manifest));
    return bytes;
}

Outcome<size_t> VisitRecordingClipDescriptors(const MediaIndexSnapshot& snapshot,
                                              const std::function<bool(const RecordingMediaDescriptor&)>& visitor,
                                              const std::function<bool(uint64_t)>& before_index_read) {
    return RecordingMediaIndex::Visit(snapshot, visitor, {}, before_index_read);
}

Outcome<RecordingClipDraft> RunRecordingClipJob(const ClipBoundaryPlan& plan, const MediaIndexSnapshot& snapshot,
                                                std::span<const ClipPinnedSource> sources,
                                                const std::filesystem::path& staging_path,
                                                const std::atomic<bool>* cancelled, const ClipJobIoControl* control) {
    if (snapshot.token.recording_id != plan.recording_id ||
        snapshot.token.committed_through != plan.snapshot.committed_through || sources.size() != plan.segments.size() ||
        sources.empty() || staging_path.empty())
        return ClipError(EngineErrorCode::InvalidConfig, "Clip job does not match its frozen plan");
    if (sources.size() > kClipSegmentLimit)
        return ClipError(EngineErrorCode::ResourceBudgetExceeded, "Clip segment count exceeds resource budget");
    uint64_t metadata_bytes = 0;
    for (const auto& source : sources) {
        if (!source.descriptor)
            continue;
        metadata_bytes = AddBytes(metadata_bytes, EstimateRecordingClipDescriptorBytes(*source.descriptor));
        if (source.audio_manifest)
            metadata_bytes = AddBytes(metadata_bytes, ManifestBytes(*source.audio_manifest));
        if (metadata_bytes > kStartupMetadataBudgetBytes)
            return ClipError(EngineErrorCode::ResourceBudgetExceeded, "Clip startup metadata budget exceeded");
    }
    for (size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        const auto& planned = plan.segments[index];
        if (!source.descriptor || source.descriptor->recording_id != plan.recording_id ||
            source.descriptor->segment_id != planned.segment_id ||
            source.descriptor->writer_generation != planned.writer_generation ||
            source.descriptor->descriptor_revision != planned.descriptor_revision ||
            source.descriptor->file_identity != planned.file_identity ||
            IdentityOf(source.borrowed_handle) != planned.file_identity)
            return ClipError(EngineErrorCode::SourceIntegrityFailure, "Pinned clip source identity changed");
        if (index && !SameTrackPlan(*sources.front().descriptor, *source.descriptor))
            return ClipError(EngineErrorCode::UnsupportedFormat, "Segment track layout changed within clip");
        if (!source.descriptor->audio_tracks.empty() &&
            (!source.audio_manifest || !ValidateAudioManifest1(*source.audio_manifest).succeeded() ||
             source.audio_manifest->recording_id != plan.recording_id ||
             source.audio_manifest->segment_id != source.descriptor->segment_id ||
             source.audio_manifest->media_id != source.descriptor->media_id))
            return ClipError(EngineErrorCode::MetadataIncomplete, "A pinned segment lacks verified audio provenance");
    }
    JobState job{plan, snapshot, sources, staging_path, cancelled, control};
    job.draft.staging_path = staging_path;
    job.draft.actual_begin_r_ns = plan.actual_begin_r_ns;
    job.draft.actual_end_r_ns = plan.actual_end_r_ns;
    ExtentSpool spool(staging_path);
    if (!spool.opened())
        return ClipError(EngineErrorCode::StorageInsufficient, "Cannot create clip extent spool");
    const auto write_extent = [&](const SpoolExtent& extent) {
        if (!job.BeforeIo(ClipJobIoPhase::Reading, sizeof(extent)))
            return job.Fail(ClipError(EngineErrorCode::Cancelled, "Clip extent spool write cancelled"));
        if (!spool.Write(extent))
            return job.Fail(ClipError(EngineErrorCode::StorageInsufficient, "Cannot write clip extent spool"));
        return true;
    };
    for (size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        bool current = false;
        bool stopped = false;
        const auto visited = RecordingMediaIndex::Visit(
            snapshot,
            [&](const RecordingMediaDescriptor& descriptor) {
                if (IsCancelled(cancelled)) {
                    stopped = true;
                    job.Fail(ClipError(EngineErrorCode::Cancelled, "Clip extent selection cancelled"));
                    return false;
                }
                current = descriptor.segment_id == source.descriptor->segment_id &&
                          descriptor.writer_generation == source.descriptor->writer_generation &&
                          descriptor.descriptor_revision == source.descriptor->descriptor_revision;
                return true;
            },
            [&](const CommittedMediaReceipt& receipt) {
                if (!current)
                    return true;
                if (receipt.commit_sequence > plan.snapshot.committed_through ||
                    receipt.readable_end_offset > plan.segments[index].readable_end_offset) {
                    stopped = true;
                    job.Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Receipt exceeds frozen clip extent"));
                    return false;
                }
                for (const auto& extent : receipt.clusters) {
                    bool selected = false;
                    for (const auto& coverage : extent.coverage) {
                        const auto begin =
                            SegmentLocalToRecordingNs(source.descriptor->segment_origin_r_ns, coverage.begin_pts_ns);
                        const auto end =
                            SegmentLocalToRecordingNs(source.descriptor->segment_origin_r_ns, coverage.end_pts_ns);
                        if (!begin || !end) {
                            stopped = true;
                            job.Fail(ClipError(EngineErrorCode::SourceIntegrityFailure, "Cluster coverage overflows"));
                            return false;
                        }
                        selected |= *end > plan.read_begin_r_ns && *begin < plan.actual_end_r_ns;
                    }
                    if (plan.end_rap && source.descriptor->segment_id == plan.end_rap->segment_id)
                        for (const auto& rap : extent.random_access_points)
                            selected |= rap.cluster_offset == plan.end_rap->cluster_offset &&
                                        rap.block_offset == plan.end_rap->block_offset;
                    if (!selected)
                        continue;
                    if (!write_extent({extent.file_begin, extent.file_end, receipt.readable_end_offset})) {
                        stopped = true;
                        return false;
                    }
                }
                return true;
            },
            [&](uint64_t bytes) {
                return !IsCancelled(cancelled) &&
                       (!control || !control->before_io || control->before_io(ClipJobIoPhase::Reading, bytes));
            });
        if (!visited.succeeded())
            return *visited.error();
        if (stopped)
            return job.failure.value_or(
                ClipError(EngineErrorCode::SourceIntegrityFailure, "Clip index traversal stopped"));
        if (!write_extent({(std::numeric_limits<uint64_t>::max)(), 0, 0}))
            return *job.failure;
    }
    if (!spool.Rewind())
        return ClipError(EngineErrorCode::SourceIntegrityFailure, "Cannot rewind clip extent spool");
    for (size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        ClipCopiedSourceRange range{plan.recording_id, source.descriptor->segment_id, source.descriptor->media_id,
                                    (std::numeric_limits<int64_t>::max)(), (std::numeric_limits<int64_t>::min)()};
        std::vector<ClusterExtent> page;
        page.reserve(kExtentPage);
        uint64_t readable_end = 0;
        const auto flush = [&] {
            if (page.empty())
                return true;
            auto selected = std::move(page);
            if (!job.ConsumePage(source, std::move(selected), readable_end, range))
                return false;
            page.clear();
            page.reserve(kExtentPage);
            return true;
        };
        for (;;) {
            SpoolExtent extent;
            if (!job.BeforeIo(ClipJobIoPhase::Reading, sizeof(extent)))
                return ClipError(EngineErrorCode::Cancelled, "Clip extent spool read cancelled");
            if (!spool.Read(extent))
                return ClipError(EngineErrorCode::SourceIntegrityFailure, "Cannot read clip extent spool");
            if (extent.file_begin == (std::numeric_limits<uint64_t>::max)())
                break;
            if (extent.file_end <= extent.file_begin || extent.file_end > extent.readable_end ||
                extent.readable_end > plan.segments[index].readable_end_offset)
                return ClipError(EngineErrorCode::SourceIntegrityFailure, "Invalid spooled clip extent");
            readable_end = extent.readable_end;
            page.push_back({extent.file_begin, extent.file_end, {}, {}});
            if (page.size() == kExtentPage && !flush())
                return *job.failure;
        }
        if (!flush())
            return *job.failure;
        if (range.begin_r_ns >= range.end_r_ns)
            return ClipError(EngineErrorCode::SourceIntegrityFailure,
                             "Planned source contributed no copied video interval");
        job.draft.source_ranges.push_back(range);
    }
    if (!job.opened || !job.saw_start || job.draft.video_packets == 0 || (plan.end_rap && !job.saw_end))
        return ClipError(EngineErrorCode::IndexNotReady, "Planned video access was absent from committed media");
    for (const auto count : job.draft.audio_packets)
        if (count == 0)
            return ClipError(EngineErrorCode::IndexNotReady, "A planned audio track has no copied packets");
    for (const auto& boundary : job.draft.audio_boundaries) {
        if (!boundary.first_audible_r_ns || !boundary.last_audible_r_ns || !boundary.packet_duration_ns ||
            *boundary.packet_duration_ns > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) ||
            *boundary.first_audible_r_ns - plan.actual_begin_r_ns >
                static_cast<int64_t>(*boundary.packet_duration_ns) ||
            plan.actual_end_r_ns - *boundary.last_audible_r_ns > static_cast<int64_t>(*boundary.packet_duration_ns))
            return ClipError(EngineErrorCode::IndexNotReady,
                             "An audio track cannot cover the planned clip within one packet");
    }
    if (!job.writer.Finalize())
        return ClipError(IsCancelled(cancelled) ? EngineErrorCode::Cancelled : EngineErrorCode::StorageInsufficient,
                         "Clip staging finalize failed: " + job.writer.error());
    job.draft.peak_cluster_payload_bytes = job.writer.peak_cluster_payload_bytes();
    return job.draft;
}

} // namespace exosnap::engine

#pragma once

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/color_metadata.h>

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace exosnap::engine {

struct AudioManifest1;

template <typename Tag> struct OpaqueId {
    std::array<uint8_t, 16> bytes{};
    auto operator<=>(const OpaqueId&) const = default;
};

using AudioSourceId = OpaqueId<struct AudioSourceIdTag>;
using AudioTrackId = OpaqueId<struct AudioTrackIdTag>;
using AudioRouteId = OpaqueId<struct AudioRouteIdTag>;
using LegacyRowId = OpaqueId<struct LegacyRowIdTag>;
using PresetNamespaceId = OpaqueId<struct PresetNamespaceIdTag>;
using RecordingId = OpaqueId<struct RecordingIdTag>;
using SegmentId = OpaqueId<struct SegmentIdTag>;
using MediaId = OpaqueId<struct MediaIdTag>;
using CommandId = OpaqueId<struct CommandIdTag>;
using JobId = OpaqueId<struct JobIdTag>;
using PhysicalClockId = OpaqueId<struct PhysicalClockIdTag>;
using AudioEncoderInstanceId = OpaqueId<struct AudioEncoderInstanceIdTag>;

[[nodiscard]] std::string FormatIdentity(const std::array<uint8_t, 16>& bytes);
[[nodiscard]] std::optional<std::array<uint8_t, 16>> ParseIdentity(std::string_view text);
[[nodiscard]] std::array<uint8_t, 16> NewIdentity();

template <typename Tag> [[nodiscard]] std::string ToUuid(const OpaqueId<Tag>& id) {
    return FormatIdentity(id.bytes);
}

template <typename Id> [[nodiscard]] std::optional<Id> FromUuid(std::string_view text) {
    const auto bytes = ParseIdentity(text);
    return bytes ? std::optional<Id>{Id{*bytes}} : std::nullopt;
}

template <typename Id> [[nodiscard]] Id NewId() {
    return Id{NewIdentity()};
}

template <typename Tag> struct ScalarValue {
    uint64_t value = 0;
    auto operator<=>(const ScalarValue&) const = default;
};

using ConfigRevision = ScalarValue<struct ConfigRevisionTag>;
using OutputGeneration = ScalarValue<struct OutputGenerationTag>;
using SourceGeneration = ScalarValue<struct SourceGenerationTag>;
using Nanoseconds = ScalarValue<struct NanosecondsTag>;
using QpcTicks = ScalarValue<struct QpcTicksTag>;
using AudioFrames = ScalarValue<struct AudioFramesTag>;

struct AudioStreamIndex {
    uint32_t value = 0;
    auto operator<=>(const AudioStreamIndex&) const = default;
};

enum class EngineErrorCode {
    InvalidConfig,
    StaleRevision,
    UnsupportedPlatform,
    UnsupportedFormat,
    UnavailableSource,
    ProcessIdentityChanged,
    ResourceBudgetExceeded,
    StorageInsufficient,
    IndexNotReady,
    SourceIntegrityFailure,
    MetadataIncomplete,
    Cancelled,
    Stalled,
};

enum class EngineModule { Configuration, Runtime, Audio, FileOutput, MediaIndex, Clip, Editor };
enum class Retryability { Never, AfterUserAction, Transient };

struct EngineError {
    EngineErrorCode code = EngineErrorCode::InvalidConfig;
    EngineModule owner = EngineModule::Configuration;
    std::optional<CommandId> command_id;
    std::optional<JobId> job_id;
    uint64_t generation = 0;
    std::optional<uint32_t> win32_code;
    std::optional<int32_t> hresult;
    std::optional<int32_t> ffmpeg_code;
    std::string context;
    Retryability retryability = Retryability::Never;
};

template <typename T> class Outcome {
  public:
    Outcome(T value) : result_(std::move(value)) {
    }
    Outcome(EngineError error) : result_(std::move(error)) {
    }
    [[nodiscard]] bool succeeded() const noexcept {
        return std::holds_alternative<T>(result_);
    }
    [[nodiscard]] const T* value() const noexcept {
        return std::get_if<T>(&result_);
    }
    [[nodiscard]] T* value() noexcept {
        return std::get_if<T>(&result_);
    }
    [[nodiscard]] const EngineError* error() const noexcept {
        return std::get_if<EngineError>(&result_);
    }

  private:
    std::variant<T, EngineError> result_;
};

struct FileIdentity {
    uint64_t volume_serial = 0;
    std::array<uint8_t, 16> file_id{};
    auto operator<=>(const FileIdentity&) const = default;
};

struct MediaCodecDescriptor {
    std::string codec_id;
    std::vector<uint8_t> codec_private;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t bit_depth = 0;
    uint32_t codec_delay_samples = 0;
    std::string channel_layout;
};

struct ContainerAudioTrack {
    AudioTrackId track_id;
    AudioStreamIndex stream_index;
    uint64_t track_uid = 0;
    std::string name;
    std::string role;
    MediaCodecDescriptor codec;
    std::vector<AudioSourceId> source_ids;
    AudioEncoderInstanceId encoder_instance_id;
    bool encoder_reset = false;
    // Signed recording sample-frame position at this track's sample rate.
    // Absence means that decoded sample coverage has not been established.
    std::optional<int64_t> first_decoded_sample_r;
    std::optional<uint64_t> packet_duration_ns;
    std::optional<uint64_t> preroll_ns;
};

struct RecordingMediaDescriptor {
    RecordingId recording_id;
    MediaId media_id;
    SegmentId segment_id;
    uint64_t source_timeline_revision = 0;
    uint64_t writer_generation = 0;
    uint64_t descriptor_revision = 0;
    FileIdentity file_identity;
    std::string path;
    MediaCodecDescriptor video;
    std::vector<ContainerAudioTrack> audio_tracks;
    std::vector<AudioTrackId> default_monitor_track_ids;
    ColorMetadata color;
    int64_t segment_origin_r_ns = 0;
    uint64_t continuity_group = 0;
    std::vector<uint8_t> immutable_header;
    uint32_t video_width = 0;
    uint32_t video_height = 0;
    uint32_t frame_rate_num = 0;
    uint32_t frame_rate_den = 1;
    std::shared_ptr<const AudioManifest1> audio_manifest;
};

struct MediaTrackCoverage {
    uint64_t track_number = 0;
    int64_t begin_pts_ns = 0;
    int64_t end_pts_ns = 0;
    int64_t min_dts_ns = 0;
    int64_t max_dts_ns = 0;
};

enum class RandomAccessKind { Unknown, ClosedGop, CodecQualified };

struct MediaRandomAccessPoint {
    uint64_t track_number = 0;
    uint64_t cluster_offset = 0;
    uint64_t block_offset = 0;
    int64_t pts_ns = 0;
    int64_t dts_ns = 0;
    RandomAccessKind kind = RandomAccessKind::Unknown;
};

struct ClusterExtent {
    uint64_t file_begin = 0;
    uint64_t file_end = 0;
    std::vector<MediaTrackCoverage> coverage;
    std::vector<MediaRandomAccessPoint> random_access_points;
};

struct CommittedMediaReceipt {
    RecordingId recording_id;
    SegmentId segment_id;
    uint64_t writer_generation = 0;
    uint64_t commit_sequence = 0;
    uint64_t descriptor_revision = 0;
    uint64_t readable_end_offset = 0;
    uint64_t durable_end_offset = 0;
    std::vector<ClusterExtent> clusters;
};

enum class ReceiptAcceptance { Accepted, IndexDegraded };

struct IndexSnapshotToken {
    RecordingId recording_id;
    uint64_t committed_through = 0;
    uint64_t descriptor_revision = 0;
    uint64_t journal_revision = 0;
    Nanoseconds freeze_qpc_ns;
};

// A closed writer's final media extent, tied to the receipt that must be present in a snapshot.
struct RecordingSegmentSeal {
    RecordingId recording_id;
    SegmentId segment_id;
    uint64_t writer_generation = 0;
    uint64_t descriptor_revision = 0;
    uint64_t final_commit_sequence = 0;
    uint64_t final_readable_end = 0;
    int64_t measured_video_end_r_ns = 0;
};

struct MediaIndexSnapshot {
    IndexSnapshotToken token;
    std::filesystem::path journal_path;
    FileIdentity journal_identity;
    uint64_t journal_end = 0;
    std::shared_ptr<const RecordingMediaDescriptor> descriptor;
};

} // namespace exosnap::engine

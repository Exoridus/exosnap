#pragma once

#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/background_io.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace exosnap::engine {

struct AudioManifestSource {
    AudioSourceId id;
    MicChannelMode channel_mode = MicChannelMode::Auto;
    MicDspConfig dsp;
    float post_dsp_gain_linear = 1.0f;
    bool muted = false;
};

struct AudioManifestRoute {
    AudioRouteId id;
    AudioSourceId source_id;
    float gain_linear = 1.0f;
};

struct AudioManifestTrack {
    ContainerAudioTrack container;
    std::vector<AudioManifestRoute> routes;
    uint32_t fixed_average_divisor = 0;
    TrackLimiterConfig limiter;
    std::vector<AudioTrackId> derived_from_track_ids;
};

// Contains frozen processing provenance without endpoint or process identities.
struct AudioManifest1 {
    uint32_t version = 1;
    MediaId media_id;
    RecordingId recording_id;
    SegmentId segment_id;
    std::vector<MediaId> derived_from_media_ids;
    int64_t segment_origin_r_ns = 0;
    uint64_t continuity_group = 0;
    std::vector<AudioManifestSource> sources;
    std::vector<AudioManifestTrack> tracks;
    std::vector<AudioTrackId> default_monitor_track_ids;
    MonitorSelectionOrigin monitor_selection_origin = MonitorSelectionOrigin::Default;
};

struct MonitorTrackSelection {
    AudioTrackId track_id;
    float gain_linear = 1.0f;
    bool muted = false;
};

// Binds an explicitly confirmed selection to exact bytes when semantic track
// provenance is unavailable. Reopening must prove identity before reusing IDs.
struct UnverifiedAudioAssetBinding {
    FileIdentity file_identity;
    uint64_t file_size = 0;
    std::array<uint8_t, 32> sha256{};
    std::vector<ContainerAudioTrack> actual_tracks;
};

// An empty selection deliberately silences monitoring. Identity validation must
// precede playback and rendering so a changed asset cannot reuse stale ordinals.
struct MonitorSelectionSnapshot {
    MediaId media_id;
    ConfigRevision revision;
    std::vector<MonitorTrackSelection> tracks;
    bool overlap_confirmed = false;
    std::shared_ptr<const UnverifiedAudioAssetBinding> unverified_asset_binding;
};

enum class AudioMetadataStatus { Verified, Missing, Invalid, Stale, Pending };

struct MediaIdentitySidecarReferences {
    std::vector<std::string> marker_refs;
    // Preserved associations do not establish decoded clip coverage.
    std::string source_ranges_json = "[]";
};

struct AudioMetadataReadResult {
    // Uses a per-open identity when container provenance cannot be verified.
    MediaId selection_media_id;
    AudioMetadataStatus status = AudioMetadataStatus::Missing;
    std::optional<AudioManifest1> manifest;
    std::vector<ContainerAudioTrack> actual_tracks;
    bool monitoring_confirmation_required = true;
    std::vector<std::string> warnings;
    std::shared_ptr<const UnverifiedAudioAssetBinding> unverified_asset_binding;
    MediaIdentitySidecarReferences references;
    std::optional<std::array<uint8_t, 32>> expected_content_sha256;
};

struct MediaIdentitySnapshot {
    FileIdentity file_identity;
    uint64_t file_size = 0;
    uint64_t last_write_time = 0;
    std::array<uint8_t, 32> sha256{};
};

struct MediaVerificationMeasurements {
    uint64_t hash_starts = 0;
    uint64_t hash_completions = 0;
    uint64_t hash_bytes = 0;
    uint64_t hash_cancellations = 0;
    int64_t hash_ns = 0;
};

enum class MediaHashCause {
    Unspecified,
    SourceProbe,
    AudioMetadata,
    Sidecar,
    RecordingFinalization,
    LiveLease,
    DecoderValidation,
    Count
};
struct MediaHashMeasurements {
    uint64_t starts = 0;
    uint64_t completions = 0;
    uint64_t bytes = 0;
    uint64_t cancellations = 0;
    int64_t elapsed_ns = 0;
};
struct MediaHashMeasurementSnapshot {
    MediaHashMeasurements total;
    std::array<MediaHashMeasurements, static_cast<size_t>(MediaHashCause::Count)> by_cause{};
};
[[nodiscard]] MediaHashMeasurementSnapshot GlobalMediaHashMeasurements();

class VerifiedMediaIdentity;
struct EditMediaMetadata;

// Retains an admitted file object. Explicit-offset reads have independent positions.
class MediaReadCapability {
  public:
    ~MediaReadCapability();
    [[nodiscard]] const std::filesystem::path& Path() const;
    [[nodiscard]] const MediaIdentitySnapshot& Identity() const;
    [[nodiscard]] uint64_t Generation() const;
    [[nodiscard]] Outcome<size_t> ReadAt(uint64_t offset, std::span<uint8_t> bytes) const;
    [[nodiscard]] MediaVerificationMeasurements Measurements() const;

  private:
    struct Impl;
    explicit MediaReadCapability(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
    friend Outcome<std::shared_ptr<const MediaReadCapability>> AcquireMediaReadCapability(const std::filesystem::path&);
    friend Outcome<std::shared_ptr<const VerifiedMediaIdentity>>
    VerifyMediaIdentity(const std::shared_ptr<const MediaReadCapability>&, std::optional<std::array<uint8_t, 32>>,
                        const std::function<bool()>&, const BackgroundIoAdmission&, const std::atomic<bool>*);
    friend std::shared_ptr<const AudioMetadataReadResult>
    CachedMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>&, bool);
    friend void RetainMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>&,
                                         std::shared_ptr<const AudioMetadataReadResult>, bool);
    friend std::shared_ptr<const AudioMetadataReadResult>
    CachedAdoptedMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>&);
    friend void RetainAdoptedMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>&,
                                                std::shared_ptr<const AudioMetadataReadResult>);
    friend std::shared_ptr<const EditMediaMetadata>
    CachedEditMediaMetadata(const std::shared_ptr<const MediaReadCapability>&);
    friend void RetainEditMediaMetadata(const std::shared_ptr<const MediaReadCapability>&,
                                        std::shared_ptr<const EditMediaMetadata>);
};

// Only verification can construct a proof. The retained capability pins its bytes.
class VerifiedMediaIdentity {
  public:
    [[nodiscard]] const MediaIdentitySnapshot& Identity() const;
    [[nodiscard]] const std::shared_ptr<const MediaReadCapability>& Source() const;

  private:
    VerifiedMediaIdentity(std::shared_ptr<const MediaReadCapability> source, MediaIdentitySnapshot identity);
    std::shared_ptr<const MediaReadCapability> source_;
    MediaIdentitySnapshot identity_;
    friend Outcome<std::shared_ptr<const VerifiedMediaIdentity>>
    VerifyMediaIdentity(const std::shared_ptr<const MediaReadCapability>&, std::optional<std::array<uint8_t, 32>>,
                        const std::function<bool()>&, const BackgroundIoAdmission&, const std::atomic<bool>*);
};

[[nodiscard]] Outcome<std::shared_ptr<const MediaReadCapability>>
AcquireMediaReadCapability(const std::filesystem::path& path);
// Expected identity is an independent comparison target and is never replaced.
[[nodiscard]] Outcome<std::shared_ptr<const VerifiedMediaIdentity>>
VerifyMediaIdentity(const std::shared_ptr<const MediaReadCapability>& source,
                    std::optional<std::array<uint8_t, 32>> expected = std::nullopt,
                    const std::function<bool()>& continue_io = {}, const BackgroundIoAdmission& before_io = {},
                    const std::atomic<bool>* cancelled = nullptr);

[[nodiscard]] Outcome<std::monostate> ValidateAudioManifest1(const AudioManifest1& manifest);
[[nodiscard]] Outcome<std::string> SerializeAudioManifest1(const AudioManifest1& manifest);
[[nodiscard]] Outcome<AudioManifest1> ParseAudioManifest1(std::string_view json);
// Keeps media readable and denies writes, deletion and replacement until the
// last copy of the opaque ownership token is released. Does not verify content.
[[nodiscard]] Outcome<std::shared_ptr<void>> AcquireMediaReadLease(const std::filesystem::path& media);
// Call after the media writer has closed, on a background I/O owner. The sidecar
// binds the manifest to the final file identity, size and SHA-256.
[[nodiscard]] Outcome<std::monostate>
WriteMediaIdentitySidecar(const std::filesystem::path& media, const AudioManifest1& manifest,
                          const std::function<bool()>& continue_io = {},
                          const std::optional<MediaIdentitySidecarReferences>& references = std::nullopt,
                          const BackgroundIoAdmission& before_io = {}, const std::atomic<bool>* cancelled = nullptr);
// Verifies the envelope against final file identity, size and a streaming hash.
// Returned track metadata is authored provenance, not a demux or decode result.
[[nodiscard]] AudioMetadataReadResult
ReadMediaIdentitySidecar(const std::filesystem::path& media, const std::function<bool()>& continue_io = {},
                         const BackgroundIoAdmission& before_io = {}, const std::atomic<bool>* cancelled = nullptr,
                         const std::shared_ptr<const MediaReadCapability>& source = {}, bool verify_content = true);
// Updates an existing final-byte-verified envelope. Missing semantic metadata
// remains missing and is never synthesized from marker files. Only the supplied
// owned reference is replaced. Other verified associations are preserved.
[[nodiscard]] Outcome<std::monostate> UpdateMediaIdentityMarkerReference(
    const std::filesystem::path& media, const std::optional<std::string>& marker_reference,
    const std::function<bool()>& continue_io = {}, const std::optional<std::string>& owned_reference = std::nullopt);
[[nodiscard]] Outcome<std::monostate> ValidateMonitorSelection(const MonitorSelectionSnapshot& selection,
                                                               const AudioManifest1& manifest);

} // namespace exosnap::engine

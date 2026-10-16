#pragma once

// Source-based waveform peak analysis and its persistent cache format.
//
// A waveform belongs to one physical audio stream of one validated source, not
// to a clip: every clip over the same stream shares it, and a clip only selects
// a source sample window. Samples are the editor's normalized audio: 48 kHz
// interleaved stereo float, mono duplicated to both channels, exactly what
// EditPlayerEngine::ReadAudioStreamSequential delivers. Codec priming and the
// resampler remain continuous across analysis chunks. Sample indices are
// media-local, the same ones AudioSampleIndex() derives from source time.
//
// The pyramid keeps min, max and RMS per channel and bucket. Level 0 holds
// kWaveformBaseBucketSamples samples per bucket, and each higher level
// aggregates two buckets of the level below, so level L covers
// kWaveformBaseBucketSamples << L samples per bucket. Bucket i of every level
// starts at sample i * samples_per_bucket: boundaries are anchored to absolute
// source samples, never to a view.

#include <array>
#include <atomic>
#include <cstdint>
#include <exosnap/engine/media_cache_store.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap::engine {

inline constexpr std::uint32_t kWaveformSchemaVersion = 1;
inline constexpr std::uint32_t kWaveformSampleRate = 48000;
inline constexpr std::uint32_t kWaveformChannels = 2;
inline constexpr std::uint32_t kWaveformBaseBucketSamples = 128;
inline constexpr std::uint32_t kWaveformMaxLevels = 40;
// Names every input that shapes the analysed samples. Changing the decoder,
// resampler, channel policy, sample format or rate changes this string and
// therefore every key, so a different normalization never reuses old peaks.
inline constexpr std::string_view kWaveformNormalizationContract =
    "exosnap.edit.audio-stream;ffmpeg-swr;stereo,mono-duplicated;f32;48000;continuous-origin-v2";

// Identity of one analysed stream. `stream_identity` is the stream's recorded
// track identity, or all zero when the source declares none; the content hash
// already pins the physical stream either way.
struct WaveformSourceKey {
    std::array<std::uint8_t, 32> content_sha256{};
    std::uint32_t container_index = 0;
    std::array<std::uint8_t, 16> stream_identity{};
};

// Stable 64-character lowercase hex cache key. Never derived from a file name,
// path or clip.
[[nodiscard]] std::string WaveformCacheKey(const WaveformSourceKey& source,
                                           std::uint32_t schema_version = kWaveformSchemaVersion,
                                           std::string_view normalization = kWaveformNormalizationContract);

// One channel of one bucket, quantized to signed 16-bit full scale (32767 is
// 1.0). Sample values beyond full scale are clamped.
struct WaveformPeak {
    std::int16_t min = 0;
    std::int16_t max = 0;
    std::int16_t rms = 0;
    bool operator==(const WaveformPeak&) const = default;
};

[[nodiscard]] constexpr float WaveformPeakToFloat(std::int16_t value) noexcept {
    return static_cast<float>(value) / 32767.0f;
}
[[nodiscard]] std::int16_t QuantizeWaveformValue(double value) noexcept;

[[nodiscard]] constexpr std::uint64_t WaveformBucketSamples(std::uint32_t level) noexcept {
    return static_cast<std::uint64_t>(kWaveformBaseBucketSamples) << level;
}
// Buckets of `level` needed to cover `sample_count` samples.
[[nodiscard]] std::uint64_t WaveformBucketCount(std::uint64_t sample_count, std::uint32_t level) noexcept;
// Levels needed until a single bucket covers the whole stream (at least 1).
[[nodiscard]] std::uint32_t WaveformLevelCount(std::uint64_t sample_count) noexcept;

// Incremental pyramid. Append() takes interleaved stereo samples in source
// order; completed buckets of every level become readable immediately, so a
// partially analysed stream can be drawn while analysis continues. Finish()
// closes the trailing partial buckets.
class WaveformPyramidBuilder {
  public:
    explicit WaveformPyramidBuilder(std::uint64_t sample_count);

    void Append(const float* interleaved_stereo, std::size_t frames);
    void Finish();

    [[nodiscard]] bool Finished() const noexcept {
        return finished_;
    }
    [[nodiscard]] std::uint64_t SampleCount() const noexcept {
        return sample_count_;
    }
    // Samples whose level-0 buckets are final.
    [[nodiscard]] std::uint64_t CompletedSamples() const noexcept;
    [[nodiscard]] std::uint32_t LevelCount() const noexcept {
        return static_cast<std::uint32_t>(levels_.size());
    }
    // Final buckets of `level`, kWaveformChannels peaks per bucket.
    [[nodiscard]] std::span<const WaveformPeak> Level(std::uint32_t level) const;
    [[nodiscard]] std::uint64_t ReadyBuckets(std::uint32_t level) const;

    // Bytes held by the bucket arrays.
    [[nodiscard]] std::size_t MemoryBytes() const noexcept;

  private:
    void CloseLevel0Bucket();
    void Propagate(std::uint32_t level);

    std::uint64_t sample_count_ = 0;
    std::uint64_t consumed_ = 0;
    bool finished_ = false;
    std::vector<std::vector<WaveformPeak>> levels_;
    std::array<double, kWaveformChannels> sum_squares_{};
    std::array<float, kWaveformChannels> min_{};
    std::array<float, kWaveformChannels> max_{};
    std::uint32_t bucket_fill_ = 0;
};

// ---- Persistent payload ----
//
// Little endian: u32 channel count, u32 sample rate, u32 base bucket samples,
// u32 level count, u64 sample count, then per level {u64 bucket count,
// u64 payload offset}, then each level's buckets as int16 {min, max, rms} per
// channel. The store's envelope adds key echo, schema version and SHA-256.

[[nodiscard]] std::vector<std::uint8_t> SerializeWaveformPyramid(const WaveformPyramidBuilder& finished);

struct WaveformFileLayout {
    std::uint64_t sample_count = 0;
    struct LevelInfo {
        std::uint64_t bucket_count = 0;
        std::uint64_t offset = 0;
    };
    std::vector<LevelInfo> levels;
};

inline constexpr std::size_t kWaveformFixedHeaderBytes = 24;
inline constexpr std::size_t kWaveformLevelEntryBytes = 16;
inline constexpr std::size_t kWaveformBucketBytes = kWaveformChannels * 3 * sizeof(std::int16_t);

// Streams the compatible payload into private store staging. Only one pending
// child per level and bounded write blocks are retained. Reads expose final
// buckets before publication. The declared sample count must be delivered.
// The caller serializes operations, including publication and progressive reads.
class WaveformStreamWriter {
  public:
    static std::shared_ptr<WaveformStreamWriter> Open(MediaCacheStore& store, std::string_view key,
                                                      std::uint64_t sample_count, std::string& error);
    ~WaveformStreamWriter();
    bool Append(const float* samples, std::size_t frames);
    bool Finish();
    bool Publish(std::string& error, const std::function<bool()>& continue_io = {});
    std::uint32_t ReadBlock(std::uint32_t level, std::uint64_t first_bucket, std::uint32_t max_buckets,
                            std::vector<WaveformPeak>& out);
    [[nodiscard]] std::uint64_t CompletedSamples() const noexcept;
    [[nodiscard]] std::size_t MemoryBytes() const noexcept;
    [[nodiscard]] const WaveformFileLayout& Layout() const noexcept;

  private:
    struct Impl;
    explicit WaveformStreamWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Reads the layout from the first bytes of a payload. `read` fetches payload
// bytes at an offset. Rejects anything inconsistent with `payload_size`.
[[nodiscard]] std::optional<WaveformFileLayout>
ParseWaveformLayout(std::uint64_t payload_size,
                    const std::function<bool(std::uint64_t offset, std::span<std::uint8_t> out)>& read);

void DecodeWaveformBuckets(std::span<const std::uint8_t> bytes, std::vector<WaveformPeak>& out);

// ---- Analysis ----

// Source of normalized samples for one stream. Implementations need not be
// thread-safe; one analysis owns one decoder.
class WaveformAudioDecoder {
  public:
    virtual ~WaveformAudioDecoder() = default;
    [[nodiscard]] virtual std::uint64_t Seeks() const {
        return 0;
    }
    [[nodiscard]] virtual std::uint64_t DecodedFrames() const {
        return 0;
    }
    // Exactly frame_count stereo frames starting at media-local sample
    // `first_sample`; time the source does not cover is silence.
    virtual bool Decode(std::int64_t first_sample, std::uint32_t frame_count, std::vector<float>& output,
                        std::string& error) = 0;
};

// Opens `container_index` of `path` through EditPlayerEngine without a video
// decoder. Returns nullptr with a reason on failure.
[[nodiscard]] std::unique_ptr<WaveformAudioDecoder>
OpenEditWaveformDecoder(const std::filesystem::path& path, std::uint32_t container_index, std::string& error,
                        const std::atomic<bool>* cancelled = nullptr,
                        const std::array<std::uint8_t, 32>* expected_sha256 = nullptr);

// Upper bound EditPlayerEngine accepts per range call.
inline constexpr std::uint32_t kWaveformAnalysisChunkFrames = 48000;

enum class WaveformAnalysisOutcome { Completed, Cancelled, Failed };

struct WaveformAnalysisControl {
    std::uint32_t chunk_frames = kWaveformAnalysisChunkFrames;
    // Called before every chunk. May block (admission); false cancels.
    std::function<bool()> proceed;
    // Receives each decoded chunk in source order.
    std::function<void(const float* interleaved_stereo, std::size_t frames)> consume;
};

// Decodes samples [0, sample_count) in chunks. On Completed every sample was
// consumed; Cancelled and Failed stop between chunks.
WaveformAnalysisOutcome AnalyzeWaveform(WaveformAudioDecoder& decoder, std::uint64_t sample_count,
                                        const WaveformAnalysisControl& control, std::string& error);

} // namespace exosnap::engine

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_types.h>
#include <exosnap/engine/media_cache_store.h>
#include <exosnap/engine/waveform_cache.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace exosnap::engine {
namespace {

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::uint32_t GetU32(const std::uint8_t* at) {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i)
        value = (value << 8) | at[i];
    return value;
}
std::uint64_t GetU64(const std::uint8_t* at) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << 8) | at[i];
    return value;
}

class EngineWaveformDecoder final : public WaveformAudioDecoder {
  public:
    EngineWaveformDecoder(std::unique_ptr<EditPlayerEngine> engine, std::uint32_t container_index)
        : engine_(std::move(engine)), container_index_(container_index) {
    }
    bool Decode(std::int64_t first_sample, std::uint32_t frame_count, std::vector<float>& output,
                std::string& error) override {
        return engine_->ReadAudioStreamSequential(container_index_, first_sample, frame_count, output, error);
    }

    std::uint64_t Seeks() const override {
        return engine_->AudioSeekCount();
    }
    std::uint64_t DecodedFrames() const override {
        return engine_->AudioDecodedFrameCount();
    }
    std::span<const AudioSampleSpan> Coverage() const override {
        return engine_->AudioDecodedCoverage();
    }

  private:
    std::unique_ptr<EditPlayerEngine> engine_;
    std::uint32_t container_index_ = 0;
};

} // namespace

std::string WaveformCacheKey(const WaveformSourceKey& source, std::uint32_t schema_version,
                             std::string_view normalization) {
    std::vector<std::uint8_t> canonical;
    constexpr std::string_view kDomain = "exosnap.waveform-cache";
    canonical.insert(canonical.end(), kDomain.begin(), kDomain.end());
    canonical.push_back(0);
    PutU32(canonical, schema_version);
    PutU32(canonical, static_cast<std::uint32_t>(normalization.size()));
    canonical.insert(canonical.end(), normalization.begin(), normalization.end());
    canonical.insert(canonical.end(), source.content_sha256.begin(), source.content_sha256.end());
    PutU32(canonical, source.container_index);
    canonical.insert(canonical.end(), source.stream_identity.begin(), source.stream_identity.end());
    PutU32(canonical, kWaveformSampleRate);
    PutU32(canonical, kWaveformChannels);
    PutU32(canonical, kWaveformBaseBucketSamples);
    return ToHex(Sha256(canonical));
}

std::int16_t QuantizeWaveformValue(double value) noexcept {
    if (!std::isfinite(value))
        return 0;
    const double scaled = std::round(std::clamp(value, -1.0, 1.0) * 32767.0);
    return static_cast<std::int16_t>(scaled);
}

std::uint64_t WaveformBucketCount(std::uint64_t sample_count, std::uint32_t level) noexcept {
    const auto per_bucket = WaveformBucketSamples(level);
    return sample_count / per_bucket + (sample_count % per_bucket != 0 ? 1 : 0);
}

std::uint32_t WaveformLevelCount(std::uint64_t sample_count) noexcept {
    std::uint32_t levels = 1;
    while (levels < kWaveformMaxLevels && WaveformBucketCount(sample_count, levels - 1) > 1)
        ++levels;
    return levels;
}

WaveformPyramidBuilder::WaveformPyramidBuilder(std::uint64_t sample_count) : sample_count_(sample_count) {
    levels_.resize(WaveformLevelCount(sample_count));
    min_.fill(std::numeric_limits<float>::infinity());
    max_.fill(-std::numeric_limits<float>::infinity());
}

void WaveformPyramidBuilder::Append(const float* interleaved_stereo, std::size_t frames) {
    if (finished_)
        return;
    const auto accepted = static_cast<std::size_t>(std::min<std::uint64_t>(frames, sample_count_ - consumed_));
    for (std::size_t frame = 0; frame < accepted; ++frame) {
        for (std::uint32_t channel = 0; channel < kWaveformChannels; ++channel) {
            float value = interleaved_stereo[frame * kWaveformChannels + channel];
            if (!std::isfinite(value))
                value = 0;
            min_[channel] = std::min(min_[channel], value);
            max_[channel] = std::max(max_[channel], value);
            sum_squares_[channel] += static_cast<double>(value) * value;
        }
        if (++bucket_fill_ == kWaveformBaseBucketSamples)
            CloseLevel0Bucket();
    }
    consumed_ += accepted;
}

void WaveformPyramidBuilder::CloseLevel0Bucket() {
    if (bucket_fill_ == 0)
        return;
    for (std::uint32_t channel = 0; channel < kWaveformChannels; ++channel) {
        levels_[0].push_back(WaveformPeak{QuantizeWaveformValue(min_[channel]), QuantizeWaveformValue(max_[channel]),
                                          QuantizeWaveformValue(std::sqrt(sum_squares_[channel] / bucket_fill_))});
    }
    bucket_fill_ = 0;
    sum_squares_.fill(0);
    min_.fill(std::numeric_limits<float>::infinity());
    max_.fill(-std::numeric_limits<float>::infinity());
    Propagate(0);
}

// Builds every parent whose children are final. Before Finish() only complete
// pairs qualify; afterwards a trailing single child forms a parent of its own.
void WaveformPyramidBuilder::Propagate(std::uint32_t level) {
    for (std::uint32_t current = level; current + 1 < levels_.size(); ++current) {
        auto& children = levels_[current];
        auto& parents = levels_[current + 1];
        const std::uint64_t child_count = children.size() / kWaveformChannels;
        const std::uint64_t buildable = finished_ ? (child_count + 1) / 2 : child_count / 2;
        const auto child_samples = WaveformBucketSamples(current);
        const auto samples_in = [&](std::uint64_t child) -> double {
            const auto begin = child * child_samples;
            return static_cast<double>(std::min(begin + child_samples, sample_count_) - begin);
        };
        bool grew = false;
        while (parents.size() / kWaveformChannels < buildable) {
            const std::uint64_t parent = parents.size() / kWaveformChannels;
            const std::uint64_t first = parent * 2;
            const bool pair = first + 1 < child_count;
            for (std::uint32_t channel = 0; channel < kWaveformChannels; ++channel) {
                const auto& a = children[first * kWaveformChannels + channel];
                WaveformPeak combined = a;
                if (pair) {
                    const auto& b = children[(first + 1) * kWaveformChannels + channel];
                    const double na = samples_in(first);
                    const double nb = samples_in(first + 1);
                    const double ra = WaveformPeakToFloat(a.rms);
                    const double rb = WaveformPeakToFloat(b.rms);
                    combined.min = std::min(a.min, b.min);
                    combined.max = std::max(a.max, b.max);
                    combined.rms = QuantizeWaveformValue(std::sqrt((ra * ra * na + rb * rb * nb) / (na + nb)));
                }
                parents.push_back(combined);
            }
            grew = true;
        }
        if (!grew && !finished_)
            return;
    }
}

void WaveformPyramidBuilder::Finish() {
    if (finished_)
        return;
    CloseLevel0Bucket();
    // A stream that ended early is described by what it actually delivered.
    sample_count_ = consumed_;
    finished_ = true;
    levels_.resize(WaveformLevelCount(sample_count_));
    Propagate(0);
}

std::uint64_t WaveformPyramidBuilder::CompletedSamples() const noexcept {
    if (finished_)
        return sample_count_;
    return std::min<std::uint64_t>(levels_[0].size() / kWaveformChannels * kWaveformBaseBucketSamples, sample_count_);
}

std::span<const WaveformPeak> WaveformPyramidBuilder::Level(std::uint32_t level) const {
    if (level >= levels_.size())
        return {};
    return levels_[level];
}

std::uint64_t WaveformPyramidBuilder::ReadyBuckets(std::uint32_t level) const {
    if (level >= levels_.size())
        return 0;
    return levels_[level].size() / kWaveformChannels;
}

std::size_t WaveformPyramidBuilder::MemoryBytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& level : levels_)
        bytes += level.capacity() * sizeof(WaveformPeak);
    return bytes;
}

std::vector<std::uint8_t> SerializeWaveformPyramid(const WaveformPyramidBuilder& finished) {
    std::vector<std::uint8_t> out;
    const auto level_count = finished.LevelCount();
    std::uint64_t data_bytes = 0;
    for (std::uint32_t level = 0; level < level_count; ++level)
        data_bytes += finished.ReadyBuckets(level) * kWaveformBucketBytes;
    const std::uint64_t header_bytes = kWaveformFixedHeaderBytes + kWaveformLevelEntryBytes * level_count;
    out.reserve(static_cast<std::size_t>(header_bytes + data_bytes));
    PutU32(out, kWaveformChannels);
    PutU32(out, kWaveformSampleRate);
    PutU32(out, kWaveformBaseBucketSamples);
    PutU32(out, level_count);
    PutU64(out, finished.SampleCount());
    std::uint64_t offset = header_bytes;
    for (std::uint32_t level = 0; level < level_count; ++level) {
        PutU64(out, finished.ReadyBuckets(level));
        PutU64(out, offset);
        offset += finished.ReadyBuckets(level) * kWaveformBucketBytes;
    }
    for (std::uint32_t level = 0; level < level_count; ++level) {
        for (const auto& peak : finished.Level(level)) {
            for (const std::int16_t value : {peak.min, peak.max, peak.rms}) {
                const auto bits = static_cast<std::uint16_t>(value);
                out.push_back(static_cast<std::uint8_t>(bits & 0xff));
                out.push_back(static_cast<std::uint8_t>(bits >> 8));
            }
        }
    }
    return out;
}

struct WaveformStreamWriter::Impl {
    struct Level {
        std::array<WaveformPeak, kWaveformChannels> pending{};
        bool has_pending = false;
        std::uint64_t ready = 0;
        std::uint64_t written = 0;
        std::vector<std::uint8_t> bytes;
    };
    std::unique_ptr<MediaCacheStore::Writer> writer;
    WaveformFileLayout layout;
    std::vector<Level> levels;
    std::array<double, kWaveformChannels> squares{};
    std::array<float, kWaveformChannels> min{};
    std::array<float, kWaveformChannels> max{};
    std::uint64_t consumed = 0;
    std::uint32_t fill = 0;
    bool finished = false;
    bool ok = true;

    bool Flush(std::uint32_t index) {
        auto& level = levels[index];
        if (level.bytes.empty())
            return ok;
        ok = ok && writer->Write(layout.levels[index].offset + level.written * kWaveformBucketBytes, level.bytes);
        if (!ok)
            return false;
        level.written += level.bytes.size() / kWaveformBucketBytes;
        level.bytes.clear();
        return ok;
    }
    void Emit(std::uint32_t index, const std::array<WaveformPeak, kWaveformChannels>& peaks) {
        auto& level = levels[index];
        for (const auto& peak : peaks)
            for (const auto value : {peak.min, peak.max, peak.rms}) {
                const auto bits = static_cast<std::uint16_t>(value);
                level.bytes.push_back(static_cast<std::uint8_t>(bits));
                level.bytes.push_back(static_cast<std::uint8_t>(bits >> 8));
            }
        ++level.ready;
        if (level.bytes.size() >= 2048 * kWaveformBucketBytes)
            Flush(index);
        if (index + 1 == levels.size())
            return;
        if (!level.has_pending) {
            level.pending = peaks;
            level.has_pending = true;
            return;
        }
        std::array<WaveformPeak, kWaveformChannels> parent{};
        const auto child_samples = WaveformBucketSamples(index);
        const auto first = (level.ready - 2) * child_samples;
        const double na = static_cast<double>(std::min(child_samples, layout.sample_count - first));
        const double nb = static_cast<double>(std::min(child_samples, layout.sample_count - first - child_samples));
        for (std::size_t channel = 0; channel < parent.size(); ++channel) {
            const auto& a = level.pending[channel];
            const auto& b = peaks[channel];
            const double ra = WaveformPeakToFloat(a.rms);
            const double rb = WaveformPeakToFloat(b.rms);
            parent[channel] = {std::min(a.min, b.min), std::max(a.max, b.max),
                               QuantizeWaveformValue(std::sqrt((ra * ra * na + rb * rb * nb) / (na + nb)))};
        }
        level.has_pending = false;
        Emit(index + 1, parent);
    }
    void CloseBucket() {
        if (!fill)
            return;
        std::array<WaveformPeak, kWaveformChannels> peaks{};
        for (std::size_t channel = 0; channel < peaks.size(); ++channel)
            peaks[channel] = {QuantizeWaveformValue(min[channel]), QuantizeWaveformValue(max[channel]),
                              QuantizeWaveformValue(std::sqrt(squares[channel] / fill))};
        Emit(0, peaks);
        fill = 0;
        squares.fill(0);
        min.fill(std::numeric_limits<float>::infinity());
        max.fill(-std::numeric_limits<float>::infinity());
    }
};

WaveformStreamWriter::WaveformStreamWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
WaveformStreamWriter::~WaveformStreamWriter() = default;

std::shared_ptr<WaveformStreamWriter> WaveformStreamWriter::Open(MediaCacheStore& store, std::string_view key,
                                                                 std::uint64_t sample_count, std::string& error,
                                                                 MediaCacheWriteFailure* failure) {
    if (failure)
        *failure = MediaCacheWriteFailure::None;
    auto impl = std::make_unique<Impl>();
    impl->layout.sample_count = sample_count;
    const auto count = WaveformLevelCount(sample_count);
    std::uint64_t offset = kWaveformFixedHeaderBytes + count * kWaveformLevelEntryBytes;
    std::vector<std::uint8_t> header;
    PutU32(header, kWaveformChannels);
    PutU32(header, kWaveformSampleRate);
    PutU32(header, kWaveformBaseBucketSamples);
    PutU32(header, count);
    PutU64(header, sample_count);
    for (std::uint32_t level = 0; level < count; ++level) {
        const auto buckets = WaveformBucketCount(sample_count, level);
        if (offset > store.BudgetBytes() || buckets > (store.BudgetBytes() - offset) / kWaveformBucketBytes) {
            error = "The waveform exceeds the disk cache budget.";
            if (failure)
                *failure = MediaCacheWriteFailure::InvalidEntry;
            return {};
        }
        impl->layout.levels.push_back({buckets, offset});
        PutU64(header, buckets);
        PutU64(header, offset);
        offset += buckets * kWaveformBucketBytes;
    }
    impl->writer = store.BeginWrite(key, offset, error, failure);
    if (!impl->writer)
        return {};
    if (!impl->writer->Write(0, header)) {
        error = "Cannot initialize waveform staging.";
        if (failure)
            *failure = MediaCacheWriteFailure::Io;
        return {};
    }
    impl->levels.resize(count);
    for (auto& level : impl->levels)
        level.bytes.reserve(2048 * kWaveformBucketBytes);
    impl->min.fill(std::numeric_limits<float>::infinity());
    impl->max.fill(-std::numeric_limits<float>::infinity());
    return std::shared_ptr<WaveformStreamWriter>(new WaveformStreamWriter(std::move(impl)));
}

bool WaveformStreamWriter::Append(const float* samples, std::size_t frames) {
    if (impl_->finished || !impl_->ok || frames > impl_->layout.sample_count - impl_->consumed)
        return false;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < kWaveformChannels; ++channel) {
            float value = samples[frame * kWaveformChannels + channel];
            if (!std::isfinite(value))
                value = 0;
            impl_->min[channel] = std::min(impl_->min[channel], value);
            impl_->max[channel] = std::max(impl_->max[channel], value);
            impl_->squares[channel] += static_cast<double>(value) * value;
        }
        if (++impl_->fill == kWaveformBaseBucketSamples)
            impl_->CloseBucket();
    }
    impl_->consumed += frames;
    for (std::uint32_t level = 0; level < impl_->levels.size(); ++level)
        impl_->Flush(level);
    return impl_->ok;
}

bool WaveformStreamWriter::Finish() {
    if (impl_->finished)
        return impl_->ok;
    if (impl_->consumed != impl_->layout.sample_count)
        return false;
    impl_->CloseBucket();
    for (std::uint32_t level = 0; level + 1 < impl_->levels.size(); ++level) {
        auto& state = impl_->levels[level];
        if (state.has_pending) {
            state.has_pending = false;
            impl_->Emit(level + 1, state.pending);
        }
    }
    for (std::uint32_t level = 0; level < impl_->levels.size(); ++level)
        impl_->Flush(level);
    impl_->finished = true;
    return impl_->ok;
}

bool WaveformStreamWriter::Publish(std::string& error, const std::function<bool()>& continue_io) {
    if (!impl_->finished || !impl_->ok) {
        error = "The waveform analysis is incomplete.";
        return false;
    }
    return impl_->writer->Publish(error, continue_io);
}

std::uint32_t WaveformStreamWriter::ReadBlock(std::uint32_t level, std::uint64_t first_bucket,
                                              std::uint32_t max_buckets, std::vector<WaveformPeak>& out) {
    if (level >= impl_->levels.size() || first_bucket >= impl_->levels[level].written)
        return 0;
    const auto count =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(max_buckets, impl_->levels[level].written - first_bucket));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count) * kWaveformBucketBytes);
    if (!impl_->writer->Read(impl_->layout.levels[level].offset + first_bucket * kWaveformBucketBytes, bytes))
        return 0;
    DecodeWaveformBuckets(bytes, out);
    return count;
}

std::uint64_t WaveformStreamWriter::CompletedSamples() const noexcept {
    return impl_->finished ? impl_->consumed : impl_->consumed - impl_->fill;
}
std::size_t WaveformStreamWriter::MemoryBytes() const noexcept {
    std::size_t bytes = sizeof(Impl) + impl_->levels.capacity() * sizeof(Impl::Level) +
                        impl_->layout.levels.capacity() * sizeof(WaveformFileLayout::LevelInfo);
    for (const auto& level : impl_->levels)
        bytes += level.bytes.capacity();
    return bytes;
}
const WaveformFileLayout& WaveformStreamWriter::Layout() const noexcept {
    return impl_->layout;
}
std::optional<WaveformFileLayout>
ParseWaveformLayout(std::uint64_t payload_size,
                    const std::function<bool(std::uint64_t offset, std::span<std::uint8_t> out)>& read) {
    if (payload_size < kWaveformFixedHeaderBytes)
        return std::nullopt;
    std::array<std::uint8_t, kWaveformFixedHeaderBytes> fixed{};
    if (!read(0, fixed))
        return std::nullopt;
    const auto channels = GetU32(fixed.data());
    const auto rate = GetU32(fixed.data() + 4);
    const auto base = GetU32(fixed.data() + 8);
    const auto level_count = GetU32(fixed.data() + 12);
    WaveformFileLayout layout;
    layout.sample_count = GetU64(fixed.data() + 16);
    if (channels != kWaveformChannels || rate != kWaveformSampleRate || base != kWaveformBaseBucketSamples ||
        level_count == 0 || level_count != WaveformLevelCount(layout.sample_count))
        return std::nullopt;
    const std::uint64_t header_bytes = kWaveformFixedHeaderBytes + kWaveformLevelEntryBytes * level_count;
    if (header_bytes > payload_size)
        return std::nullopt;
    std::vector<std::uint8_t> table(static_cast<std::size_t>(kWaveformLevelEntryBytes * level_count));
    if (!read(kWaveformFixedHeaderBytes, table))
        return std::nullopt;
    std::uint64_t expected_offset = header_bytes;
    for (std::uint32_t level = 0; level < level_count; ++level) {
        WaveformFileLayout::LevelInfo info{GetU64(table.data() + level * kWaveformLevelEntryBytes),
                                           GetU64(table.data() + level * kWaveformLevelEntryBytes + 8)};
        if (info.bucket_count != WaveformBucketCount(layout.sample_count, level) || info.offset != expected_offset)
            return std::nullopt;
        expected_offset += info.bucket_count * kWaveformBucketBytes;
        layout.levels.push_back(info);
    }
    if (expected_offset != payload_size)
        return std::nullopt;
    return layout;
}

void DecodeWaveformBuckets(std::span<const std::uint8_t> bytes, std::vector<WaveformPeak>& out) {
    const std::size_t peaks = bytes.size() / (3 * sizeof(std::int16_t));
    out.resize(peaks);
    for (std::size_t i = 0; i < peaks; ++i) {
        const auto* at = bytes.data() + i * 6;
        const auto value = [at](int index) {
            return static_cast<std::int16_t>(static_cast<std::uint16_t>(at[index * 2] | (at[index * 2 + 1] << 8)));
        };
        out[i] = WaveformPeak{value(0), value(1), value(2)};
    }
}

std::unique_ptr<WaveformAudioDecoder> OpenEditWaveformDecoder(const std::filesystem::path& path,
                                                              std::uint32_t container_index, std::string& error,
                                                              const std::atomic<bool>* cancelled,
                                                              const std::array<std::uint8_t, 32>* expected_sha256) {
    auto engine = std::make_unique<EditPlayerEngine>();
    if (!engine->OpenAudioStream(path, container_index, error, cancelled))
        return nullptr;
    if (expected_sha256 && !engine->ValidateContentIdentity(*expected_sha256, error, cancelled))
        return nullptr;
    return std::make_unique<EngineWaveformDecoder>(std::move(engine), container_index);
}

std::unique_ptr<WaveformAudioDecoder> OpenEditWaveformDecoder(const std::shared_ptr<const MediaReadCapability>& source,
                                                              std::uint32_t container_index, std::string& error,
                                                              const std::atomic<bool>* cancelled,
                                                              const std::array<std::uint8_t, 32>* expected_sha256) {
    if (expected_sha256) {
        const auto proof = VerifyMediaIdentity(source, *expected_sha256, {}, {}, cancelled);
        if (!proof.succeeded()) {
            error = proof.error()->context;
            return nullptr;
        }
    }
    auto engine = std::make_unique<EditPlayerEngine>();
    if (!engine->OpenAudioStream(source, container_index, error, cancelled))
        return nullptr;
    return std::make_unique<EngineWaveformDecoder>(std::move(engine), container_index);
}

WaveformAnalysisOutcome AnalyzeWaveform(WaveformAudioDecoder& decoder, std::uint64_t sample_count,
                                        const WaveformAnalysisControl& control, std::string& error) {
    const std::uint32_t chunk = std::clamp<std::uint32_t>(control.chunk_frames, 1, kWaveformAnalysisChunkFrames);
    std::vector<float> samples;
    for (std::uint64_t first = 0; first < sample_count; first += chunk) {
        if (control.proceed && !control.proceed())
            return WaveformAnalysisOutcome::Cancelled;
        const auto frames = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk, sample_count - first));
        if (first > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            error = "The waveform sample range is outside the supported range.";
            return WaveformAnalysisOutcome::Failed;
        }
        if (!decoder.Decode(static_cast<std::int64_t>(first), frames, samples, error))
            return WaveformAnalysisOutcome::Failed;
        if (samples.size() < static_cast<std::size_t>(frames) * kWaveformChannels) {
            error = "The audio decoder returned fewer samples than requested.";
            return WaveformAnalysisOutcome::Failed;
        }
        if (control.consume)
            control.consume(samples.data(), frames);
    }
    return WaveformAnalysisOutcome::Completed;
}

} // namespace exosnap::engine

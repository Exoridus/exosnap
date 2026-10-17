#include <exosnap/engine/edit_player_engine.h>
#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_types.h>
#include <exosnap/engine/media_cache_store.h>
#include <exosnap/engine/waveform_cache.h>

#include "test_unique_temp.h"
#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace exosnap::engine;

namespace {

constexpr float kTolerance = 1.0f / 16000.0f;

std::vector<float> Interleave(const std::vector<float>& left, const std::vector<float>& right) {
    std::vector<float> out(left.size() * 2);
    for (std::size_t i = 0; i < left.size(); ++i) {
        out[i * 2] = left[i];
        out[i * 2 + 1] = right[i];
    }
    return out;
}

class FunctionDecoder final : public WaveformAudioDecoder {
  public:
    explicit FunctionDecoder(std::function<float(std::int64_t sample, int channel)> value) : value_(std::move(value)) {
    }
    bool Decode(std::int64_t first, std::uint32_t frames, std::vector<float>& out, std::string&) override {
        ++calls;
        out.resize(static_cast<std::size_t>(frames) * 2);
        for (std::uint32_t i = 0; i < frames; ++i)
            for (int c = 0; c < 2; ++c)
                out[i * 2 + c] = value_(first + i, c);
        return true;
    }
    int calls = 0;

  private:
    std::function<float(std::int64_t, int)> value_;
};

WaveformSourceKey SourceKey(std::uint8_t content_seed, std::uint32_t index = 1) {
    WaveformSourceKey key;
    for (std::size_t i = 0; i < key.content_sha256.size(); ++i)
        key.content_sha256[i] = static_cast<std::uint8_t>(content_seed + i);
    key.container_index = index;
    key.stream_identity[0] = 0x42;
    return key;
}

} // namespace

TEST(WaveformPyramid, LevelZeroKeepsMinMaxRmsPerChannel) {
    // Left: a ramp across one bucket. Right: a constant, so the two channels
    // must stay distinguishable.
    const std::size_t n = kWaveformBaseBucketSamples * 3 + 10;
    std::vector<float> left(n), right(n, -0.5f);
    for (std::size_t i = 0; i < n; ++i)
        left[i] = static_cast<float>(i % kWaveformBaseBucketSamples) / kWaveformBaseBucketSamples - 0.5f;
    const auto samples = Interleave(left, right);

    WaveformPyramidBuilder builder(n);
    builder.Append(samples.data(), n);
    EXPECT_EQ(builder.ReadyBuckets(0), 3u);
    EXPECT_EQ(builder.CompletedSamples(), kWaveformBaseBucketSamples * 3);
    builder.Finish();
    ASSERT_EQ(builder.ReadyBuckets(0), 4u);

    const auto level0 = builder.Level(0);
    double expected_sq = 0;
    for (std::uint32_t i = 0; i < kWaveformBaseBucketSamples; ++i)
        expected_sq += static_cast<double>(left[i]) * left[i];
    const float expected_rms = static_cast<float>(std::sqrt(expected_sq / kWaveformBaseBucketSamples));
    EXPECT_NEAR(WaveformPeakToFloat(level0[0].min), -0.5f, kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(level0[0].max), left[kWaveformBaseBucketSamples - 1], kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(level0[0].rms), expected_rms, kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(level0[1].min), -0.5f, kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(level0[1].max), -0.5f, kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(level0[1].rms), 0.5f, kTolerance);
    // The partial trailing bucket covers only its own 10 samples.
    EXPECT_NEAR(WaveformPeakToFloat(level0[6].max), left[n - 1], kTolerance);
}

TEST(WaveformPyramid, HigherLevelsAggregatePairs) {
    const std::size_t n = kWaveformBaseBucketSamples * 5;
    std::vector<float> left(n, 0), right(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto bucket = i / kWaveformBaseBucketSamples;
        left[i] = (i % 2 ? 1.0f : -1.0f) * 0.1f * static_cast<float>(bucket + 1);
        right[i] = 0.05f * static_cast<float>(bucket);
    }
    const auto samples = Interleave(left, right);
    WaveformPyramidBuilder builder(n);
    // Arbitrary chunking must not matter.
    builder.Append(samples.data(), 100);
    builder.Append(samples.data() + 200, n - 100);
    EXPECT_EQ(builder.ReadyBuckets(1), 2u);
    EXPECT_EQ(builder.ReadyBuckets(2), 1u);
    builder.Finish();
    ASSERT_EQ(builder.LevelCount(), WaveformLevelCount(n));
    ASSERT_EQ(builder.LevelCount(), 4u);
    EXPECT_EQ(builder.ReadyBuckets(1), 3u);
    EXPECT_EQ(builder.ReadyBuckets(2), 2u);
    EXPECT_EQ(builder.ReadyBuckets(3), 1u);

    const auto level1 = builder.Level(1);
    EXPECT_NEAR(WaveformPeakToFloat(level1[0].min), -0.2f, kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(level1[0].max), 0.2f, kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(level1[0].rms), std::sqrt((0.01 + 0.04) / 2), 2 * kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(level1[1].max), 0.05f, kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(level1[1].min), 0.0f, kTolerance);
    // Odd trailing bucket forms a parent of its own.
    EXPECT_NEAR(WaveformPeakToFloat(level1[4].max), 0.5f, kTolerance);
    const auto top = builder.Level(3);
    EXPECT_NEAR(WaveformPeakToFloat(top[0].min), -0.5f, kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(top[0].max), 0.5f, kTolerance);
    EXPECT_NEAR(WaveformPeakToFloat(top[1].max), 0.2f, kTolerance);
}

TEST(WaveformPyramid, SineRmsAndClamping) {
    const std::size_t n = 48000;
    std::vector<float> left(n), right(n, 2.0f);
    for (std::size_t i = 0; i < n; ++i)
        left[i] = static_cast<float>(std::sin(2 * std::numbers::pi * 1000.0 * i / 48000.0));
    const auto samples = Interleave(left, right);
    WaveformPyramidBuilder builder(n);
    builder.Append(samples.data(), n);
    builder.Finish();
    const auto top = builder.Level(builder.LevelCount() - 1);
    ASSERT_EQ(top.size(), 2u);
    EXPECT_NEAR(WaveformPeakToFloat(top[0].rms), std::sqrt(0.5), 1e-3);
    EXPECT_EQ(top[1].max, 32767);
}

TEST(WaveformPyramid, SerializedLayoutRoundTrips) {
    const std::size_t n = 100000;
    std::vector<float> left(n), right(n);
    for (std::size_t i = 0; i < n; ++i) {
        left[i] = std::sin(static_cast<float>(i) * 0.01f) * 0.8f;
        right[i] = -left[i] * 0.5f;
    }
    const auto samples = Interleave(left, right);
    WaveformPyramidBuilder builder(n);
    builder.Append(samples.data(), n);
    builder.Finish();
    const auto payload = SerializeWaveformPyramid(builder);
    const auto layout = ParseWaveformLayout(payload.size(), [&](std::uint64_t offset, std::span<std::uint8_t> out) {
        if (offset + out.size() > payload.size())
            return false;
        std::memcpy(out.data(), payload.data() + offset, out.size());
        return true;
    });
    ASSERT_TRUE(layout.has_value());
    ASSERT_EQ(layout->levels.size(), builder.LevelCount());
    EXPECT_EQ(layout->sample_count, n);
    for (std::uint32_t level = 0; level < builder.LevelCount(); ++level) {
        const auto& info = layout->levels[level];
        std::vector<WaveformPeak> decoded;
        DecodeWaveformBuckets(
            std::span<const std::uint8_t>(payload.data() + info.offset, info.bucket_count * kWaveformBucketBytes),
            decoded);
        const auto expected = builder.Level(level);
        ASSERT_EQ(decoded.size(), expected.size());
        EXPECT_TRUE(std::equal(decoded.begin(), decoded.end(), expected.begin()));
    }
    auto damaged = payload;
    damaged[18] ^= 1; // sample count no longer matches the level table
    EXPECT_FALSE(ParseWaveformLayout(damaged.size(), [&](std::uint64_t offset, std::span<std::uint8_t> out) {
                     std::memcpy(out.data(), damaged.data() + offset, out.size());
                     return true;
                 }).has_value());
}

TEST(WaveformCacheKey, StableAndSensitive) {
    const auto a = WaveformCacheKey(SourceKey(1));
    EXPECT_EQ(a.size(), 64u);
    EXPECT_TRUE(MediaCacheStore::IsValidKey(a));
    // Two clips over the same stream derive the key from the same inputs only.
    EXPECT_EQ(a, WaveformCacheKey(SourceKey(1)));
    EXPECT_NE(a, WaveformCacheKey(SourceKey(2)));
    EXPECT_NE(a, WaveformCacheKey(SourceKey(1, 2)));
    auto other_stream = SourceKey(1);
    other_stream.stream_identity[15] = 1;
    EXPECT_NE(a, WaveformCacheKey(other_stream));
    EXPECT_NE(a, WaveformCacheKey(SourceKey(1), kWaveformSchemaVersion + 1));
    EXPECT_NE(a, WaveformCacheKey(SourceKey(1), kWaveformSchemaVersion, "another normalization"));
}

TEST(WaveformAnalysis, ConsumesEverySampleInChunksAndCancelsBetweenChunks) {
    FunctionDecoder decoder([](std::int64_t sample, int channel) {
        return channel == 0 ? static_cast<float>(sample % 100) / 100.0f : -0.25f;
    });
    const std::uint64_t n = 48000 * 2 + 777;
    WaveformPyramidBuilder builder(n);
    WaveformAnalysisControl control;
    control.consume = [&](const float* samples, std::size_t frames) { builder.Append(samples, frames); };
    std::string error;
    ASSERT_EQ(AnalyzeWaveform(decoder, n, control, error), WaveformAnalysisOutcome::Completed) << error;
    builder.Finish();
    EXPECT_EQ(decoder.calls, 3);
    EXPECT_EQ(builder.SampleCount(), n);
    EXPECT_EQ(builder.ReadyBuckets(0), WaveformBucketCount(n, 0));

    int allowed = 1;
    control.proceed = [&]() { return allowed-- > 0; };
    FunctionDecoder second([](std::int64_t, int) { return 0.0f; });
    EXPECT_EQ(AnalyzeWaveform(second, n, control, error), WaveformAnalysisOutcome::Cancelled);
    EXPECT_EQ(second.calls, 1);
}

TEST(WaveformAnalysis, RealFixturesYieldConstantDcPerChannel) {
    wchar_t directory[32768]{};
    if (!GetEnvironmentVariableW(L"EXOSNAP_EDIT_RENDER_FIXTURES", directory, 32768))
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    for (const auto& [name, level] : {std::pair{L"red.mkv", 0.25f}, std::pair{L"blue.mkv", -0.25f}}) {
        const auto path = std::filesystem::path(directory) / name;
        const auto metadata = ProbeEditMedia(path);
        ASSERT_TRUE(metadata.error.empty()) << metadata.error;
        std::optional<std::uint32_t> audio;
        for (const auto& stream : metadata.streams)
            if (stream.kind == TimelineStreamKind::Audio)
                audio = stream.container_index;
        ASSERT_TRUE(audio.has_value());
        std::string error;
        auto decoder = OpenEditWaveformDecoder(path, *audio, error);
        ASSERT_NE(decoder, nullptr) << error;
        const std::uint64_t n = static_cast<std::uint64_t>(AudioSampleIndex(metadata.duration_us));
        WaveformPyramidBuilder builder(n);
        WaveformAnalysisControl control;
        control.consume = [&](const float* samples, std::size_t frames) { builder.Append(samples, frames); };
        const auto started = std::chrono::steady_clock::now();
        ASSERT_EQ(AnalyzeWaveform(*decoder, n, control, error), WaveformAnalysisOutcome::Completed) << error;
        builder.Finish();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "[waveform] " << std::filesystem::path(name).string() << ": analysed "
                  << static_cast<double>(n) / kWaveformSampleRate << " s of audio in " << seconds << " s ("
                  << (static_cast<double>(n) / kWaveformSampleRate) / seconds << " s/s)\n";

        // Skip the first and last buckets: codec priming and the container's
        // end are not part of the DC contract.
        const auto level0 = builder.Level(0);
        const auto buckets = builder.ReadyBuckets(0);
        ASSERT_GT(buckets, 8u);
        for (std::uint64_t bucket = 2; bucket + 2 < buckets; ++bucket) {
            for (std::uint32_t channel = 0; channel < kWaveformChannels; ++channel) {
                const auto& peak = level0[bucket * kWaveformChannels + channel];
                ASSERT_EQ(peak.min, peak.max) << "bucket " << bucket << " channel " << channel;
                ASSERT_NEAR(WaveformPeakToFloat(peak.max), level, 1e-3f);
                ASSERT_NEAR(WaveformPeakToFloat(peak.rms), std::abs(level), 1e-3f);
            }
        }
    }
}
TEST(WaveformStreaming, PayloadIsByteEquivalentAcrossOddTailsAndChunkSizes) {
    const auto root = exosnap_test::UniqueTempPath("wave_stream");
    MediaCacheStoreConfig config;
    config.root = root;
    config.budget_bytes = 8u << 20;
    config.kind = {'W', 'A', 'V', 'E'};
    config.extension = ".wave";
    MediaCacheStore store(config);
    std::string error;
    for (const std::size_t count : {0u, 1u, 127u, 128u, 129u, 641u, 100003u}) {
        std::vector<float> samples(count * 2);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = std::sin(static_cast<float>(i) * 0.071f) * (i % 11 == 0 ? 1.4f : 0.73f);
        WaveformPyramidBuilder reference(count);
        reference.Append(samples.data(), count);
        reference.Finish();
        const auto expected = SerializeWaveformPyramid(reference);
        for (const std::size_t chunk : {1u, 113u, 48000u}) {
            const auto key = WaveformCacheKey(SourceKey(static_cast<std::uint8_t>(chunk)));
            auto writer = WaveformStreamWriter::Open(store, key, count, error);
            ASSERT_NE(writer, nullptr) << error;
            for (std::size_t first = 0; first < count; first += chunk)
                ASSERT_TRUE(writer->Append(samples.data() + first * 2, std::min(chunk, count - first)));
            ASSERT_TRUE(writer->Finish());
            ASSERT_TRUE(writer->Publish(error)) << error;
            const auto actual = store.Read(key);
            ASSERT_TRUE(actual);
            EXPECT_EQ(*actual, expected) << "samples=" << count << " chunk=" << chunk;
            EXPECT_LT(writer->MemoryBytes(), 1u << 20);
        }
    }
    store.Clear();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
TEST(WaveformAnalysis, ContinuousCursorKeepsSamplesAcrossChunkBoundaries) {
    wchar_t path[32768]{};
    if (!GetEnvironmentVariableW(L"EXOSNAP_WAVEFORM_DECODER_FIXTURE", path, 32768))
        GTEST_SKIP() << "Set EXOSNAP_WAVEFORM_DECODER_FIXTURE to real normalized-audio qualification media.";
    const auto metadata = ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    std::optional<std::uint32_t> index;
    for (const auto& stream : metadata.streams)
        if (stream.kind == TimelineStreamKind::Audio) {
            index = stream.container_index;
            break;
        }
    ASSERT_TRUE(index);
    EditPlayerEngine reference;
    std::string error;
    ASSERT_TRUE(reference.OpenAudioStream(path, *index, error)) << error;
    auto cursor = OpenEditWaveformDecoder(path, *index, error);
    ASSERT_NE(cursor, nullptr) << error;
    const auto end = std::min<std::int64_t>(AudioSampleIndex(metadata.duration_us) + 257, 48000 * 16 + 257);
    std::vector<float> canonical;
    for (std::int64_t at = 0; at < end; at += 48000) {
        std::vector<float> block;
        ASSERT_TRUE(reference.ReadAudioStreamSequential(
            *index, at, static_cast<std::uint32_t>(std::min<std::int64_t>(48000, end - at)), block, error))
            << error;
        canonical.insert(canonical.end(), block.begin(), block.end());
    }
    std::int64_t first = 0;
    std::vector<float> expected, actual;
    const std::array<std::uint32_t, 7> chunks{1, 127, 48000, 7919, 130, 32768, 47999};
    for (std::size_t iteration = 0; first < end; ++iteration) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::int64_t>(chunks[iteration % chunks.size()], end - first));
        expected.assign(canonical.begin() + first * 2, canonical.begin() + (first + count) * 2);
        ASSERT_TRUE(cursor->Decode(first, count, actual, error)) << error;
        ASSERT_EQ(actual.size(), expected.size());
        if (actual != expected) {
            const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin());
            const auto at = static_cast<std::size_t>(mismatch.first - actual.begin());
            std::cout << "[cursor-range-mismatch] sample=" << first + at / 2 << " actual=" << actual[at]
                      << " range=" << expected[at] << '\n';
        }
        ASSERT_EQ(actual, expected) << "first=" << first << " frames=" << count;
        first += count;
    }
    EXPECT_LE(cursor->Seeks(), 1u);
    EXPECT_EQ(cursor->DecodedFrames(), reference.AudioDecodedFrameCount());
    ASSERT_TRUE(cursor->Decode(137, 113, actual, error)) << error;
    EXPECT_LE(cursor->Seeks(), 2u);
}
TEST(WaveformAnalysis, ContinuousCursorMatchesIndependentPcm) {
    wchar_t path[32768]{}, pcm_path[32768]{};
    if (!GetEnvironmentVariableW(L"EXOSNAP_WAVEFORM_DECODER_FIXTURE", path, 32768) ||
        !GetEnvironmentVariableW(L"EXOSNAP_WAVEFORM_PCM_REFERENCE", pcm_path, 32768))
        GTEST_SKIP() << "Set EXOSNAP_WAVEFORM_DECODER_FIXTURE and EXOSNAP_WAVEFORM_PCM_REFERENCE.";
    std::ifstream input(std::filesystem::path(pcm_path), std::ios::binary | std::ios::ate);
    ASSERT_TRUE(input.good());
    const auto bytes = static_cast<std::size_t>(input.tellg());
    ASSERT_EQ(bytes % (2 * sizeof(float)), 0u);
    std::vector<float> expected(bytes / sizeof(float));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(bytes));
    ASSERT_TRUE(input.good());
    const auto metadata = ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    std::optional<std::uint32_t> index;
    for (const auto& stream : metadata.streams)
        if (stream.kind == TimelineStreamKind::Audio) {
            index = stream.container_index;
            break;
        }
    ASSERT_TRUE(index);
    std::string error;
    auto cursor = OpenEditWaveformDecoder(path, *index, error);
    ASSERT_NE(cursor, nullptr) << error;
    const std::array<std::uint32_t, 7> chunks{1, 127, 48000, 7919, 130, 32768, 47999};
    std::vector<float> actual;
    double max_error = 0;
    std::size_t max_at = 0;
    std::optional<std::size_t> first_actual, first_expected, first_difference;
    std::vector<float> decoded(expected.size());
    for (std::size_t first = 0, iteration = 0; first < expected.size() / 2; ++iteration) {
        const auto count = static_cast<std::uint32_t>(
            std::min<std::size_t>(chunks[iteration % chunks.size()], expected.size() / 2 - first));
        ASSERT_TRUE(cursor->Decode(static_cast<std::int64_t>(first), count, actual, error)) << error;
        std::copy(actual.begin(), actual.end(), decoded.begin() + static_cast<std::ptrdiff_t>(first * 2));
        for (std::size_t i = 0; i < actual.size(); ++i) {
            if (!first_actual && std::abs(actual[i]) > 1e-5)
                first_actual = first + i / 2;
            if (!first_expected && std::abs(expected[first * 2 + i]) > 1e-5)
                first_expected = first + i / 2;
            const auto difference = std::abs(static_cast<double>(actual[i]) - expected[first * 2 + i]);
            if (!first_difference && difference > 1e-5)
                first_difference = first + i / 2;
            if (difference > max_error) {
                max_error = difference;
                max_at = first * 2 + i;
            }
        }
        first += count;
    }
    std::cout << "[cursor-pcm] max_error=" << max_error << " at_frame=" << max_at / 2 << " seeks=" << cursor->Seeks()
              << " frames=" << expected.size() / 2 << '\n';
    std::cout << "[cursor-pcm-alignment] first_actual=" << first_actual.value_or(0)
              << " first_expected=" << first_expected.value_or(0)
              << " first_difference=" << first_difference.value_or(0) << " actual_at_max=" << decoded[max_at]
              << " expected_at_max=" << expected[max_at] << '\n';
    EXPECT_LE(max_error, 1e-5);
    EXPECT_LE(cursor->Seeks(), 1u);
}

#include "EditThumbnailImageProvider.h"
#include "ExoWaveformItem.h"

#include "services/ThumbnailCache.h"
#include "services/TimelineThumbnailSource.h"
#include "services/WaveformCacheService.h"

#include <exosnap/engine/edit_timeline.h>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/waveform_cache.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGTransformNode>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include <windows.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace exosnap::quick {
namespace {

using engine::WaveformPeak;

QGuiApplication* EnsureApplication() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_MSC_VER)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#if defined(_DEBUG)
    // Route fatal test reports to stderr so offscreen failures cannot open CRT dialogs.
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
#endif
    if (auto* existing = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        return existing;
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    static int argc = 1;
    static char name[] = "media_cache_service_tests";
    static char* argv[] = {name, nullptr};
    static QGuiApplication app(argc, argv);
    return &app;
}

template <typename Predicate> bool WaitFor(Predicate predicate, int timeout_ms = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeout_ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return true;
}

std::optional<std::filesystem::path> FixtureDirectory() {
    wchar_t directory[32768]{};
    if (!GetEnvironmentVariableW(L"EXOSNAP_EDIT_RENDER_FIXTURES", directory, 32768))
        return std::nullopt;
    return std::filesystem::path(directory);
}

std::size_t FilesWithSuffix(const std::filesystem::path& root, const std::string& suffix) {
    std::size_t count = 0;
    std::error_code error;
    if (!std::filesystem::exists(root, error))
        return 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.is_regular_file() && entry.path().filename().string().ends_with(suffix))
            ++count;
    return count;
}

// Synthetic decoder: left = +amplitude, right = -amplitude, where the amplitude
// steps every 128-sample bucket. An optional gate holds every Decode call until
// the test releases one.
struct DecoderControl {
    std::atomic<int> opens{0};
    std::atomic<int> calls{0};
    std::atomic<int> closes{0};
    std::mutex mutex;
    std::condition_variable cv;
    bool gated = false;
    int permits = 0;
    std::function<void(std::uint32_t)> before_decode;
    std::function<float(std::int64_t)> sample_amplitude;
    std::function<std::vector<engine::AudioSampleSpan>(std::int64_t, std::uint32_t)> sample_coverage;
    std::atomic<bool> fail_decode{false};
    std::atomic<bool> nonfinite{false};
    std::atomic<bool> omit_coverage{false};

    void Release(int count = 1) {
        {
            std::lock_guard lock(mutex);
            permits += count;
        }
        cv.notify_all();
    }
    void Open() {
        {
            std::lock_guard lock(mutex);
            gated = false;
        }
        cv.notify_all();
    }
};

float StepAmplitude(std::int64_t sample) {
    return static_cast<float>((sample / engine::kWaveformBaseBucketSamples) % 7 + 1) / 8.0f;
}

class SyntheticDecoder final : public engine::WaveformAudioDecoder {
  public:
    explicit SyntheticDecoder(std::shared_ptr<DecoderControl> control) : control_(std::move(control)) {
    }
    ~SyntheticDecoder() override {
        control_->closes.fetch_add(1);
    }
    bool Decode(std::int64_t first, std::uint32_t frames, std::vector<float>& out, std::string& error) override {
        {
            std::unique_lock lock(control_->mutex);
            control_->cv.wait(lock, [this]() { return !control_->gated || control_->permits > 0; });
            if (control_->gated)
                --control_->permits;
        }
        if (control_->before_decode)
            control_->before_decode(frames);
        control_->calls.fetch_add(1);
        if (control_->fail_decode) {
            error = "Synthetic decoder failure";
            return false;
        }
        out.resize(static_cast<std::size_t>(frames) * 2);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float amplitude =
                control_->sample_amplitude ? control_->sample_amplitude(first + i) : StepAmplitude(first + i);
            out[i * 2] = amplitude;
            out[i * 2 + 1] = -amplitude;
        }
        coverage_ = {{first, first + frames}};
        if (control_->sample_coverage)
            coverage_ = control_->sample_coverage(first, frames);
        if (control_->omit_coverage)
            coverage_.clear();
        if (control_->nonfinite && !out.empty())
            out[0] = std::numeric_limits<float>::quiet_NaN();
        return true;
    }
    std::span<const engine::AudioSampleSpan> Coverage() const override {
        return coverage_;
    }

  private:
    std::shared_ptr<DecoderControl> control_;
    std::vector<engine::AudioSampleSpan> coverage_;
};

WaveformCacheService::DecoderFactory Factory(const std::shared_ptr<DecoderControl>& control) {
    return [control](const std::filesystem::path&, std::uint32_t, std::string&, const std::atomic<bool>*,
                     const std::array<std::uint8_t, 32>&) {
        control->opens.fetch_add(1);
        return std::make_unique<SyntheticDecoder>(control);
    };
}

engine::WaveformSourceKey SourceKey(std::uint8_t seed) {
    engine::WaveformSourceKey key;
    for (std::size_t i = 0; i < key.content_sha256.size(); ++i)
        key.content_sha256[i] = static_cast<std::uint8_t>(seed * 31 + i);
    key.container_index = 1;
    return key;
}

struct WaveformFixture {
    QTemporaryDir temp;
    std::filesystem::path root;
    QString source_path;
    std::shared_ptr<DecoderControl> control = std::make_shared<DecoderControl>();

    WaveformFixture() {
        EnsureApplication();
        root = std::filesystem::path(temp.filePath(QStringLiteral("waveforms")).toStdWString());
        source_path = temp.filePath(QStringLiteral("source.mkv"));
        std::ofstream(source_path.toStdWString()) << "placeholder";
    }
    std::unique_ptr<WaveformCacheService> Service(std::uint64_t disk_budget = 64ull << 20,
                                                  std::size_t memory_budget = 32u << 20) {
        WaveformCacheService::Config config;
        config.root = root;
        config.disk_budget_bytes = disk_budget;
        config.memory_budget_bytes = memory_budget;
        config.chunk_frames = 4800;
        return std::make_unique<WaveformCacheService>(config, Factory(control));
    }
};

constexpr qint64 kThreeSecondsUs = 3'000'000;

std::uint32_t ReadBlockBlocking(WaveformCacheService& service, const QString& key, std::uint32_t level,
                                std::uint64_t block, std::vector<WaveformPeak>& out) {
    std::uint32_t count = 0;
    WaitFor([&]() { return (count = service.readBlock(key, level, block, out)) > 0; });
    return count;
}

} // namespace

TEST(WaveformCacheService, AnalysesOnceAndAWarmCacheNeverDecodes) {
    WaveformFixture fixture;
    QString key;
    {
        auto service = fixture.Service();
        QSignalSpy ready(service.get(), &WaveformCacheService::ready);
        key = service->request(SourceKey(1), fixture.source_path, kThreeSecondsUs);
        // A second clip over the same stream asks with the same inputs.
        EXPECT_EQ(service->request(SourceKey(1), fixture.source_path, kThreeSecondsUs), key);
        ASSERT_TRUE(WaitFor([&]() { return ready.count() > 0; }));
        EXPECT_EQ(service->state(key), WaveformCacheService::State::Ready);
        EXPECT_EQ(fixture.control->opens.load(), 1);
        EXPECT_EQ(service->totalSamples(key), 144000);
        EXPECT_EQ(FilesWithSuffix(fixture.root, ".wave"), 1u);
    }
    const int calls_before = fixture.control->calls.load();
    auto warm = fixture.Service();
    QSignalSpy ready(warm.get(), &WaveformCacheService::ready);
    EXPECT_EQ(warm->request(SourceKey(1), fixture.source_path, kThreeSecondsUs), key);
    ASSERT_TRUE(WaitFor([&]() { return ready.count() > 0; }));
    EXPECT_EQ(warm->decoderOpensForTest(), 0u);
    EXPECT_EQ(warm->decodeCallsForTest(), 0u);
    EXPECT_EQ(fixture.control->calls.load(), calls_before);

    std::vector<WaveformPeak> peaks;
    const auto buckets = ReadBlockBlocking(*warm, key, 0, 0, peaks);
    ASSERT_EQ(buckets, std::min<std::uint64_t>(kWaveformBlockBuckets, engine::WaveformBucketCount(144000, 0)));
    for (std::uint32_t bucket = 0; bucket < buckets; ++bucket) {
        const float amplitude = StepAmplitude(static_cast<std::int64_t>(bucket) * 128);
        ASSERT_NEAR(engine::WaveformPeakToFloat(peaks[bucket * 2].max), amplitude, 1e-4f);
        ASSERT_NEAR(engine::WaveformPeakToFloat(peaks[bucket * 2 + 1].min), -amplitude, 1e-4f);
    }
    EXPECT_EQ(warm->levelCount(key), engine::WaveformLevelCount(144000));
}

TEST(WaveformCacheService, LongSourceUsesBoundedAnalysisWorkingSet) {
    WaveformFixture fixture;
    WaveformCacheService::Config config;
    config.root = fixture.root;
    config.disk_budget_bytes = 64ull << 20;
    config.analysis_budget_bytes = 2u << 20;
    config.chunk_frames = 48000;
    WaveformCacheService service(config, Factory(fixture.control));
    const auto key = service.request(SourceKey(77), fixture.source_path, 42ll * 60 * 1'000'000);
    ASSERT_TRUE(WaitFor(
        [&]() {
            return service.state(key) == WaveformCacheService::State::Ready ||
                   service.state(key) == WaveformCacheService::State::Failed;
        },
        120000));
    ASSERT_EQ(service.state(key), WaveformCacheService::State::Ready);
    EXPECT_EQ(service.completedSamples(key), 42ll * 60 * 48000);
    EXPECT_EQ(fixture.control->opens.load(), 1);
    EXPECT_EQ(fixture.control->calls.load(), 42 * 60);
    EXPECT_LE(service.memoryBytesForTest(), config.memory_budget_bytes);
    EXPECT_LE(service.analysisMemoryBytesForTest(), config.analysis_budget_bytes);
    std::vector<WaveformPeak> peaks;
    const auto last = engine::WaveformBucketCount(42ull * 60 * 48000, 0) - 1;
    ASSERT_GT(ReadBlockBlocking(service, key, 0, last / kWaveformBlockBuckets, peaks), 0u);
    EXPECT_EQ(peaks[(last % kWaveformBlockBuckets) * 2].max,
              engine::QuantizeWaveformValue(StepAmplitude(static_cast<std::int64_t>(last * 128))));
}
TEST(WaveformCacheService, DiskBudgetRejectsBeforeOpeningDecoder) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(1), fixture.source_path, 86'400'000'000);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Failed; }));
    EXPECT_EQ(fixture.control->opens.load(), 0);
    EXPECT_EQ(service->storeForTest().EntryCount(), 0u);
}

TEST(WaveformCacheService, DestructionCancelsDecoderOpening) {
    WaveformFixture fixture;
    std::atomic<bool> opening{false};
    std::atomic<bool> cancelled_open{false};
    WaveformCacheService::Config config;
    config.root = fixture.root;
    auto service = std::make_unique<WaveformCacheService>(
        config,
        [&](const std::filesystem::path&, std::uint32_t, std::string&, const std::atomic<bool>* cancelled,
            const std::array<std::uint8_t, 32>&) -> std::unique_ptr<engine::WaveformAudioDecoder> {
            opening = true;
            QElapsedTimer timer;
            timer.start();
            while (!cancelled->load() && timer.elapsed() < 5000)
                QThread::msleep(1);
            cancelled_open = cancelled->load();
            return {};
        });
    service->request(SourceKey(1), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return opening.load(); }));
    service.reset();
    EXPECT_TRUE(cancelled_open.load());
}

TEST(ThumbnailBroker, ClearDiscardsAlreadyTakenJobs) {
    QTemporaryDir temp;
    ThumbnailRequestBroker::Config config;
    config.disk_root = std::filesystem::path(temp.path().toStdWString());
    ThumbnailRequestBroker broker(config);
    broker.AttachWorker([]() {});
    ThumbnailSourceInfo info;
    info.content_sha256.fill(1);
    const auto identity = engine::ToHex(info.content_sha256);
    broker.RegisterSource(identity, info);
    bool completed = false;
    broker.Request(identity, 0, 32, [&](const QImage& image) {
        completed = true;
        EXPECT_TRUE(image.isNull());
    });
    const auto job = broker.TakeJob();
    ASSERT_TRUE(job);
    broker.Clear();
    QImage image(32, 32, QImage::Format_RGB32);
    image.fill(Qt::red);
    EXPECT_FALSE(broker.Publish(*job, image, 0));
    broker.Complete(*job, image);
    EXPECT_TRUE(completed);
    EXPECT_FALSE(broker.MemoryLookup(job->key));
    EXPECT_EQ(broker.Disk()->store().EntryCount(), 0u);
}

TEST(ThumbnailBroker, ReplacingContentAtOnePathInvalidatesTheOldRegistration) {
    ThumbnailRequestBroker::Config config;
    config.disk_enabled = false;
    ThumbnailRequestBroker broker(config);
    broker.AttachWorker([]() {});
    ThumbnailSourceInfo old_source;
    old_source.path = L"source.mkv";
    old_source.content_sha256.fill(1);
    const auto old_identity = engine::ToHex(old_source.content_sha256);
    broker.RegisterSource(old_identity, old_source);
    auto replacement = old_source;
    replacement.content_sha256.fill(2);
    const auto replacement_identity = engine::ToHex(replacement.content_sha256);
    broker.RegisterSource(replacement_identity, replacement);
    EXPECT_FALSE(broker.Source(old_identity));
    EXPECT_EQ(broker.IdentityForPath(replacement.path), replacement_identity);
    bool completed = false;
    EXPECT_EQ(broker.Request(old_identity, 0, 32,
                             [&](const QImage& image) {
                                 completed = true;
                                 EXPECT_TRUE(image.isNull());
                             }),
              0u);
    EXPECT_TRUE(completed);
}

TEST(WaveformCacheService, RenderReadDoesNotWaitForTheStagingStore) {
    WaveformFixture fixture;
    fixture.control->gated = true;
    auto service = fixture.Service();
    struct UnblockDecoder {
        std::shared_ptr<DecoderControl> control;
        ~UnblockDecoder() {
            control->Open();
        }
    } unblock{fixture.control};
    const auto key = service->request(SourceKey(91), fixture.source_path, kThreeSecondsUs);
    fixture.control->Release();
    ASSERT_TRUE(WaitFor([&]() { return service->completedSamples(key) >= 4736; }));

    std::string error;
    auto writer = service->storeForTest().BeginWrite(std::string(64, 'a'), 1, error);
    ASSERT_TRUE(writer) << error;
    const std::array<std::uint8_t, 1> payload{1};
    ASSERT_TRUE(writer->Write(0, payload));
    std::atomic<bool> storage_locked{false};
    std::atomic<bool> release_storage{false};
    std::jthread storage([&]() {
        writer->Publish(error, [&]() {
            storage_locked = true;
            while (!release_storage.load())
                QThread::msleep(1);
            return true;
        });
    });
    const bool held = WaitFor([&]() { return storage_locked.load(); });
    std::atomic<bool> render_started{false};
    std::atomic<bool> render_finished{false};
    std::jthread render([&]() {
        render_started = true;
        std::vector<WaveformPeak> peaks;
        service->readBlock(key, 0, 0, peaks);
        render_finished = true;
    });
    const bool started = WaitFor([&]() { return render_started.load(); });
    const bool independent = WaitFor([&]() { return render_finished.load(); }, 250);
    release_storage = true;
    storage.join();
    render.join();
    EXPECT_TRUE(held);
    EXPECT_TRUE(started);
    EXPECT_TRUE(independent) << "Render waited for staging I/O while the storage lock was held";
}

TEST(WaveformCacheService, PartialResultsAreReadableAndCancellationPublishesNothing) {
    WaveformFixture fixture;
    fixture.control->gated = true;
    auto service = fixture.Service();
    const QString key = service->request(SourceKey(2), fixture.source_path, kThreeSecondsUs);
    fixture.control->Release(2);
    ASSERT_TRUE(WaitFor([&]() { return service->completedSamples(key) >= 9600; }));
    EXPECT_EQ(service->state(key), WaveformCacheService::State::Analyzing);
    std::vector<WaveformPeak> peaks;
    const auto partial = ReadBlockBlocking(*service, key, 0, 0, peaks);
    EXPECT_EQ(partial, 9600u / 128u);
    EXPECT_NEAR(engine::WaveformPeakToFloat(peaks[0].max), StepAmplitude(0), 1e-4f);

    service->cancel(key);
    fixture.control->Open();
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Unknown; }));
    EXPECT_EQ(service->storeForTest().EntryCount(), 0u);
    EXPECT_EQ(FilesWithSuffix(fixture.root, ".wave"), 0u);
    EXPECT_EQ(FilesWithSuffix(fixture.root, ".tmp"), 0u);
}

TEST(WaveformCacheService, ClearDropsQueuedProgressAndPrivateStaging) {
    WaveformFixture fixture;
    fixture.control->gated = true;
    auto service = fixture.Service();
    struct UnblockDecoder {
        std::shared_ptr<DecoderControl> control;
        ~UnblockDecoder() {
            control->Open();
        }
    } unblock{fixture.control};
    QSignalSpy progress(service.get(), &WaveformCacheService::progress);
    QSignalSpy ready(service.get(), &WaveformCacheService::ready);
    QSignalSpy failed(service.get(), &WaveformCacheService::failed);
    const auto key = service->request(SourceKey(44), fixture.source_path, kThreeSecondsUs);
    fixture.control->Release();
    QElapsedTimer timer;
    timer.start();
    while (service->completedSamples(key) < 4736 && timer.elapsed() < 5000)
        QThread::msleep(1);
    ASSERT_GE(service->completedSamples(key), 4736);
    service->clearCache();
    fixture.control->Open();
    ASSERT_TRUE(WaitFor([&]() { return fixture.control->closes.load() > 0; }));
    QCoreApplication::processEvents();
    EXPECT_EQ(service->state(key), WaveformCacheService::State::Unknown);
    EXPECT_EQ(progress.count(), 0);
    EXPECT_EQ(ready.count(), 0);
    EXPECT_EQ(failed.count(), 0);
    EXPECT_EQ(FilesWithSuffix(fixture.root, ".tmp"), 0u);
    EXPECT_EQ(service->storeForTest().EntryCount(), 0u);
}

TEST(WaveformCacheService, ProgressivePartialBlocksStayFreshThroughOddTailPublication) {
    WaveformFixture fixture;
    fixture.control->gated = true;
    WaveformCacheService::Config config;
    config.root = fixture.root;
    config.chunk_frames = 8192;
    WaveformCacheService service(config, Factory(fixture.control));
    struct UnblockDecoder {
        std::shared_ptr<DecoderControl> control;
        ~UnblockDecoder() {
            control->Open();
        }
    } unblock{fixture.control};
    const auto key = service.request(SourceKey(45), fixture.source_path, 170688);
    fixture.control->Release();
    ASSERT_TRUE(WaitFor([&]() { return service.completedSamples(key) == 8192; }));
    ASSERT_EQ(service.totalSamples(key), 8193);
    ASSERT_EQ(service.state(key), WaveformCacheService::State::Analyzing);

    std::vector<WaveformPeak> peaks;
    ASSERT_EQ(service.readBlock(key, 1, 0, peaks), 32u);
    EXPECT_EQ(peaks[62].max, engine::QuantizeWaveformValue(0.875));
    EXPECT_GT(service.memoryBytesForTest(), 0u);
    ASSERT_EQ(service.readBlock(key, 0, 0, peaks), 64u);
    EXPECT_GT(service.memoryBytesForTest(), 0u);
    const auto snapshot = service.tryReadPublishedBlock(key, 0, 0);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->first_sample, 0u);
    EXPECT_EQ(snapshot->end_sample, 8192u);

    fixture.control->Open();
    ASSERT_TRUE(WaitFor([&]() { return service.state(key) == WaveformCacheService::State::Ready; }));
    ASSERT_EQ(ReadBlockBlocking(service, key, 1, 0, peaks), 33u);
    ASSERT_EQ(peaks.size(), 66u);
    EXPECT_EQ(peaks[64], (WaveformPeak{8192, 8192, 8192}));
    EXPECT_EQ(peaks[65], (WaveformPeak{-8192, -8192, 8192}));
    EXPECT_GT(service.memoryBytesForTest(), 0u);
    EXPECT_EQ(service.readBlock(key, 1, 0, peaks), 33u);
    ASSERT_EQ(ReadBlockBlocking(service, key, 0, 0, peaks), 65u);
    EXPECT_EQ(peaks[128], (WaveformPeak{8192, 8192, 8192}));
    EXPECT_EQ(snapshot->peaks.size(), 128u);
    EXPECT_EQ(snapshot->end_sample, 8192u);
    const auto final_snapshot = service.tryReadPublishedBlock(key, 0, 0);
    ASSERT_TRUE(final_snapshot);
    EXPECT_NE(final_snapshot, snapshot);
    EXPECT_EQ(final_snapshot->end_sample, 8193u);
    EXPECT_EQ(fixture.control->calls.load(), 2);
}

TEST(WaveformCacheService, PinnedBlocksStayBudgetedAcrossClearAndServiceLifetime) {
    WaveformFixture fixture;
    const auto budget = sizeof(WaveformCacheService::PeakBlock) + kWaveformBlockBuckets * sizeof(WaveformPeak) * 2;
    auto service = fixture.Service(64ull << 20, budget);
    const auto key = service->request(SourceKey(92), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    std::shared_ptr<const WaveformCacheService::PeakBlock> pinned;
    ASSERT_TRUE(WaitFor([&]() { return static_cast<bool>(pinned = service->tryReadPublishedBlock(key, 0, 0)); }));
    const auto bytes = service->memoryBytesForTest();
    EXPECT_GT(bytes, 0u);
    EXPECT_LE(bytes, budget);
    service->clearCache();
    EXPECT_EQ(service->memoryBytesForTest(), bytes);
    EXPECT_EQ(pinned->peaks[0], (WaveformPeak{4096, 4096, 4096}));
    EXPECT_FALSE(service->tryReadPublishedBlock(key, 0, 0));
    service.reset();
    EXPECT_EQ(pinned->first_sample, 0u);
    EXPECT_EQ(pinned->end_sample, kWaveformBlockBuckets * engine::kWaveformBaseBucketSamples);
    EXPECT_EQ(pinned->peaks.size(), kWaveformBlockBuckets * engine::kWaveformChannels);
}

TEST(WaveformCacheService, PcmDetailUsesAnIndependentCursorAndReportsActualCoverage) {
    WaveformFixture fixture;
    fixture.control->gated = true;
    auto detail = std::make_shared<DecoderControl>();
    std::atomic<std::size_t> observed_bytes{0};
    WaveformCacheService* observed_service = nullptr;
    detail->before_decode = [&](std::uint32_t) { observed_bytes = observed_service->pcmMemoryBytesForTest(); };
    std::atomic<int> opens{0};
    WaveformCacheService::Config config;
    config.root = fixture.root;
    WaveformCacheService service(config, [&](const std::filesystem::path&, std::uint32_t, std::string&,
                                             const std::atomic<bool>*, const std::array<std::uint8_t, 32>&) {
        return std::make_unique<SyntheticDecoder>(opens.fetch_add(1) == 0 ? fixture.control : detail);
    });
    observed_service = &service;
    struct UnblockDecoder {
        std::shared_ptr<DecoderControl> control;
        ~UnblockDecoder() {
            control->Open();
        }
    } unblock{fixture.control};
    const auto key = service.request(SourceKey(96), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return opens.load() == 1; }));
    const auto ticket = service.requestPcmWindow(key, 137, 512, 12);
    ASSERT_GT(ticket, 0u);
    ASSERT_TRUE(WaitFor([&]() { return service.pcmWindow(ticket).state == WaveformCacheService::DetailState::Ready; }));
    const auto snapshot = service.pcmWindow(ticket);
    ASSERT_TRUE(snapshot.window);
    EXPECT_EQ(opens.load(), 2);
    EXPECT_EQ(fixture.control->calls.load(), 0);
    EXPECT_EQ(snapshot.window->first_sample, 137);
    ASSERT_EQ(snapshot.window->samples.size(), 1024u);
    EXPECT_FLOAT_EQ(snapshot.window->samples[0], StepAmplitude(137));
    const std::vector<engine::AudioSampleSpan> expected{{137, 649}};
    EXPECT_EQ(snapshot.window->coverage, expected);
    const auto reserved_window = sizeof(WaveformCacheService::PcmWindow) + 65 +
                                 512 * (sizeof(float) * engine::kWaveformChannels + sizeof(engine::AudioSampleSpan));
    EXPECT_GE(observed_bytes.load(), reserved_window + 512 * sizeof(float) * engine::kWaveformChannels);
    service.cancelPcmWindow(ticket);
    EXPECT_EQ(service.pcmWindow(ticket).state, WaveformCacheService::DetailState::Unavailable);
    EXPECT_EQ(snapshot.window->coverage, expected);
}

TEST(WaveformCacheService, PcmContentCacheRetiresOldWindowsWithoutAnAdmissionDeadEnd) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(97), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    for (std::uint64_t i = 0; i < 300; ++i) {
        const auto ticket = service->requestPcmWindow(key, static_cast<std::int64_t>(i), 1, i);
        ASSERT_GT(ticket, 0u) << "view=" << i;
        ASSERT_TRUE(
            WaitFor([&]() { return service->pcmWindow(ticket).state == WaveformCacheService::DetailState::Ready; }));
        service->cancelPcmWindow(ticket);
    }
    EXPECT_LE(service->memoryBytesForTest(), WaveformCacheService::kDefaultMemoryBudgetBytes);
}

TEST(WaveformCacheService, PcmWindowsShareContentButRetireViewSubscriptions) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    service->setForegroundBusy(true);
    const auto key = service->request(SourceKey(98), fixture.source_path, kThreeSecondsUs);
    QSignalSpy ready(service.get(), &WaveformCacheService::pcmWindowChanged);
    const auto obsolete = service->requestPcmWindow(key, 137, 512, 1);
    const auto current = service->requestPcmWindow(key, 137, 512, 2);
    ASSERT_GT(obsolete, 0u);
    ASSERT_GT(current, obsolete);
    service->cancelPcmWindow(obsolete);
    service->setForegroundBusy(false);
    ASSERT_TRUE(
        WaitFor([&]() { return service->pcmWindow(current).state == WaveformCacheService::DetailState::Ready; }));
    ASSERT_TRUE(WaitFor([&]() { return ready.count() == 1; }));
    EXPECT_EQ(ready[0][0].toULongLong(), current);
    EXPECT_EQ(ready[0][1].toULongLong(), 2u);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    const auto calls = fixture.control->calls.load();
    const auto warm = service->requestPcmWindow(key, 137, 512, 3);
    EXPECT_EQ(service->pcmWindow(warm).window, service->pcmWindow(current).window);
    EXPECT_EQ(fixture.control->calls.load(), calls);
}

TEST(WaveformCacheService, PinnedPcmReservationsHonorTheCombinedDataBudget) {
    WaveformFixture fixture;
    constexpr std::uint32_t count = 128;
    const auto budget = sizeof(WaveformCacheService::PcmWindow) + 65 +
                        count * (sizeof(float) * engine::kWaveformChannels + sizeof(engine::AudioSampleSpan)) +
                        count * sizeof(float) * engine::kWaveformChannels;
    auto service = fixture.Service(64ull << 20, budget);
    const auto key = service->request(SourceKey(99), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    const auto first = service->requestPcmWindow(key, 0, count, 1);
    ASSERT_GT(first, 0u);
    ASSERT_TRUE(WaitFor([&]() { return service->pcmWindow(first).state == WaveformCacheService::DetailState::Ready; }));
    auto pinned = service->pcmWindow(first).window;
    ASSERT_TRUE(pinned);
    const auto second = service->requestPcmWindow(key, count, count, 2);
    ASSERT_GT(second, 0u);
    ASSERT_TRUE(
        WaitFor([&]() { return service->pcmWindow(second).state != WaveformCacheService::DetailState::Pending; }));
    EXPECT_EQ(service->pcmWindow(second).state, WaveformCacheService::DetailState::MemoryPressure);
    EXPECT_LE(service->memoryBytesForTest(), budget);
    EXPECT_LE(service->pcmMemoryBytesForTest(), WaveformCacheService::kMaxPcmBytes);
    service->cancelPcmWindow(first);
    service->cancelPcmWindow(second);
    pinned.reset();
    const auto retry = service->requestPcmWindow(key, count, count, 3);
    ASSERT_GT(retry, 0u);
    ASSERT_TRUE(WaitFor([&]() { return service->pcmWindow(retry).state == WaveformCacheService::DetailState::Ready; }));
    auto retained = service->pcmWindow(retry).window;
    service->clearCache();
    EXPECT_EQ(service->pcmWindow(retry).state, WaveformCacheService::DetailState::Unavailable);
    EXPECT_GT(service->memoryBytesForTest(), 0u);
    EXPECT_LE(service->memoryBytesForTest(), budget);
    EXPECT_EQ(retained->first_sample, count);
}

TEST(WaveformCacheService, ConsumerPinnedPcmCannotExceedItsGlobalSublimit) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(100), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    std::vector<std::shared_ptr<const WaveformCacheService::PcmWindow>> pinned;
    for (std::int64_t first = 0; first < 5; ++first) {
        const auto ticket = service->requestPcmWindow(key, first, 131072, 1);
        ASSERT_GT(ticket, 0u);
        ASSERT_TRUE(
            WaitFor([&]() { return service->pcmWindow(ticket).state == WaveformCacheService::DetailState::Ready; }));
        pinned.push_back(service->pcmWindow(ticket).window);
        service->cancelPcmWindow(ticket);
    }
    const auto pressure = service->requestPcmWindow(key, 5, 131072, 2);
    ASSERT_GT(pressure, 0u);
    ASSERT_TRUE(
        WaitFor([&]() { return service->pcmWindow(pressure).state != WaveformCacheService::DetailState::Pending; }));
    EXPECT_EQ(service->pcmWindow(pressure).state, WaveformCacheService::DetailState::MemoryPressure);
    EXPECT_LE(service->pcmMemoryBytesForTest(), WaveformCacheService::kMaxPcmBytes);
    EXPECT_LE(service->memoryBytesForTest(), WaveformCacheService::kDefaultMemoryBudgetBytes);
    EXPECT_EQ(service->requestPcmWindow(key, 0, UINT32_MAX, 3), 0u);
}

TEST(WaveformCacheService, ClearDropsAlreadyQueuedPcmNotificationsAndPreservesPinnedData) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(101), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    QSignalSpy ready(service.get(), &WaveformCacheService::pcmWindowChanged);
    const auto ticket = service->requestPcmWindow(key, 0, 512, 9);
    ASSERT_GT(ticket, 0u);
    QElapsedTimer timer;
    timer.start();
    while (service->pcmWindow(ticket).state == WaveformCacheService::DetailState::Pending && timer.elapsed() < 5000)
        QThread::msleep(1);
    auto pinned = service->pcmWindow(ticket).window;
    ASSERT_TRUE(pinned);
    const auto generation = pinned->generation;
    service->clearCache();
    QCoreApplication::processEvents();
    EXPECT_EQ(ready.count(), 0);
    EXPECT_EQ(service->pcmWindow(ticket).state, WaveformCacheService::DetailState::Unavailable);
    EXPECT_FLOAT_EQ(pinned->samples[0], StepAmplitude(0));
    EXPECT_EQ(service->request(SourceKey(101), fixture.source_path, kThreeSecondsUs), key);
    const auto replacement = service->requestPcmWindow(key, 0, 512, 10);
    ASSERT_GT(replacement, 0u);
    ASSERT_TRUE(
        WaitFor([&]() { return service->pcmWindow(replacement).state == WaveformCacheService::DetailState::Ready; }));
    EXPECT_GT(service->pcmWindow(replacement).window->generation, generation);
}

TEST(WaveformCacheService, ActivePcmTicketsPreserveTheirReadyResultUnderPressure) {
    WaveformFixture fixture;
    constexpr std::uint32_t count = 128;
    const auto budget = sizeof(WaveformCacheService::PcmWindow) + 65 +
                        count * (sizeof(float) * engine::kWaveformChannels + sizeof(engine::AudioSampleSpan)) +
                        count * sizeof(float) * engine::kWaveformChannels;
    auto service = fixture.Service(64ull << 20, budget);
    const auto key = service->request(SourceKey(102), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    const auto first = service->requestPcmWindow(key, 0, count, 1);
    ASSERT_GT(first, 0u);
    ASSERT_TRUE(WaitFor([&]() { return service->pcmWindow(first).state == WaveformCacheService::DetailState::Ready; }));
    const auto second = service->requestPcmWindow(key, count, count, 2);
    ASSERT_GT(second, 0u);
    ASSERT_TRUE(
        WaitFor([&]() { return service->pcmWindow(second).state != WaveformCacheService::DetailState::Pending; }));
    EXPECT_EQ(service->pcmWindow(first).state, WaveformCacheService::DetailState::Ready);
    EXPECT_EQ(service->pcmWindow(second).state, WaveformCacheService::DetailState::MemoryPressure);
}

TEST(WaveformCacheService, LastPcmInterestCancellationInterruptsCancellableSourceOpening) {
    for (const bool cancel_source : {false, true}) {
        SCOPED_TRACE(cancel_source);
        WaveformFixture fixture;
        std::atomic<int> opens{0};
        std::atomic<bool> opening{false}, interrupted{false};
        WaveformCacheService::Config config;
        config.root = fixture.root;
        WaveformCacheService service(
            config,
            [&](const std::filesystem::path&, std::uint32_t, std::string& error, const std::atomic<bool>* cancelled,
                const std::array<std::uint8_t, 32>&) -> std::unique_ptr<engine::WaveformAudioDecoder> {
                if (opens.fetch_add(1) == 0)
                    return std::make_unique<SyntheticDecoder>(fixture.control);
                opening = true;
                QElapsedTimer timer;
                timer.start();
                while (!cancelled->load() && timer.elapsed() < 5000)
                    QThread::msleep(1);
                interrupted = cancelled->load();
                error = "Cancelled source opening";
                return {};
            });
        const auto key = service.request(SourceKey(103), fixture.source_path, kThreeSecondsUs);
        ASSERT_TRUE(WaitFor([&]() { return service.state(key) == WaveformCacheService::State::Ready; }));
        const auto ticket = service.requestPcmWindow(key, 0, 512, 1);
        ASSERT_GT(ticket, 0u);
        ASSERT_TRUE(WaitFor([&]() { return opening.load(); }));
        if (cancel_source)
            service.cancel(key);
        else
            service.cancelPcmWindow(ticket);
        EXPECT_TRUE(WaitFor([&]() { return interrupted.load(); }, 250));
        EXPECT_EQ(service.pcmWindow(ticket).state, WaveformCacheService::DetailState::Unavailable);
    }
}

TEST(WaveformCacheService, PcmDetailRejectsNonfiniteSamplesAndAllowsRetry) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(104), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    fixture.control->nonfinite = true;
    const auto ticket = service->requestPcmWindow(key, 0, 128, 1);
    ASSERT_GT(ticket, 0u);
    ASSERT_TRUE(
        WaitFor([&]() { return service->pcmWindow(ticket).state != WaveformCacheService::DetailState::Pending; }));
    EXPECT_EQ(service->pcmWindow(ticket).state, WaveformCacheService::DetailState::Failed);
    EXPECT_FALSE(service->pcmWindow(ticket).window);
    fixture.control->nonfinite = false;
    const auto retry = service->requestPcmWindow(key, 0, 128, 2);
    ASSERT_GT(retry, 0u);
    ASSERT_TRUE(WaitFor([&]() { return service->pcmWindow(retry).state == WaveformCacheService::DetailState::Ready; }));
    EXPECT_TRUE(std::isfinite(service->pcmWindow(retry).window->samples[0]));
}

TEST(WaveformCacheService, PcmWithoutActualCoverageIsUnavailableAndDecodeErrorsAreObservable) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(105), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    fixture.control->omit_coverage = true;
    const auto unknown = service->requestPcmWindow(key, 0, 128, 1);
    ASSERT_GT(unknown, 0u);
    ASSERT_TRUE(
        WaitFor([&]() { return service->pcmWindow(unknown).state != WaveformCacheService::DetailState::Pending; }));
    EXPECT_EQ(service->pcmWindow(unknown).state, WaveformCacheService::DetailState::NoCoverage);
    EXPECT_FALSE(service->pcmWindow(unknown).window);
    service->cancelPcmWindow(unknown);
    fixture.control->omit_coverage = false;
    fixture.control->fail_decode = true;
    const auto failed = service->requestPcmWindow(key, 0, 128, 2);
    ASSERT_GT(failed, 0u);
    ASSERT_TRUE(
        WaitFor([&]() { return service->pcmWindow(failed).state != WaveformCacheService::DetailState::Pending; }));
    EXPECT_EQ(service->pcmWindow(failed).state, WaveformCacheService::DetailState::Failed);
    EXPECT_EQ(service->pcmWindow(failed).reason, QStringLiteral("Synthetic decoder failure"));
    EXPECT_FALSE(service->pcmWindow(failed).window);
    fixture.control->fail_decode = false;
    const auto retry = service->requestPcmWindow(key, 0, 128, 3);
    ASSERT_GT(retry, 0u);
    ASSERT_TRUE(WaitFor([&]() { return service->pcmWindow(retry).state == WaveformCacheService::DetailState::Ready; }));
}

TEST(WaveformCacheService, EvictedPublishedLayoutAutomaticallyReanalysesOnce) {
    WaveformFixture fixture;
    auto service = fixture.Service(40 * 1024, sizeof(WaveformCacheService::PeakBlock) +
                                                  kWaveformBlockBuckets * sizeof(WaveformPeak) * 2);
    const auto source_a = SourceKey(46);
    const auto key_a = service->request(source_a, fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key_a) == WaveformCacheService::State::Ready; }));
    const auto key_b = service->request(SourceKey(47), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key_b) == WaveformCacheService::State::Ready; }));
    ASSERT_FALSE(service->storeForTest().Contains(key_a.toStdString()));
    const auto opens = service->decoderOpensForTest();
    service->setForegroundBusy(true);
    std::vector<WaveformPeak> peaks;
    EXPECT_EQ(service->readBlock(key_a, 0, 0, peaks), 0u);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key_a) == WaveformCacheService::State::Queued; }, 5000));
    EXPECT_TRUE(service->sourceAvailable(key_a));

    EXPECT_EQ(service->decoderOpensForTest(), opens);
    service->setForegroundBusy(false);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key_a) == WaveformCacheService::State::Ready; }));
    EXPECT_EQ(service->decoderOpensForTest(), opens + 1);
    EXPECT_EQ(service->request(source_a, fixture.source_path, kThreeSecondsUs), key_a);
    ASSERT_GT(ReadBlockBlocking(*service, key_a, 0, 0, peaks), 0u);
    EXPECT_EQ(peaks[0], (WaveformPeak{4096, 4096, 4096}));
    EXPECT_EQ(service->decoderOpensForTest(), opens + 1);
}

TEST(WaveformCacheService, ForegroundBusyPausesAnalysis) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    service->setForegroundBusy(true);
    const QString key = service->request(SourceKey(3), fixture.source_path, kThreeSecondsUs);
    QThread::msleep(150);
    QCoreApplication::processEvents();
    EXPECT_EQ(fixture.control->calls.load(), 0);
    service->setForegroundBusy(false);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    EXPECT_GT(fixture.control->calls.load(), 0);
}

TEST(WaveformCacheService, MissingSourceKeepsCachedPeaksButReportsUnavailable) {
    WaveformFixture fixture;
    const QString missing = fixture.temp.filePath(QStringLiteral("moved-away.mkv"));
    QString key;
    {
        auto service = fixture.Service();
        key = service->request(SourceKey(4), fixture.source_path, kThreeSecondsUs);
        ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    }
    auto service = fixture.Service();
    QSignalSpy availability(service.get(), &WaveformCacheService::sourceAvailabilityChanged);
    service->request(SourceKey(4), missing, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    EXPECT_FALSE(service->sourceAvailable(key));
    ASSERT_TRUE(WaitFor([&]() { return availability.count() > 0; }));
    EXPECT_FALSE(availability.at(0).at(1).toBool());
    std::vector<WaveformPeak> peaks;
    EXPECT_GT(ReadBlockBlocking(*service, key, 0, 0, peaks), 0u);

    const QString uncached = service->request(SourceKey(5), missing, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(uncached) == WaveformCacheService::State::Failed; }));
    EXPECT_FALSE(service->sourceAvailable(uncached));
    EXPECT_EQ(fixture.control->opens.load(), 1);
}

TEST(WaveformCacheService, DiskAndMemoryBudgetsHold) {
    WaveformFixture fixture;
    // One three-second waveform is about 27 KB on disk.
    const std::uint64_t disk_budget = 60 * 1024;
    const std::size_t memory_budget = 3 * kWaveformBlockBuckets * sizeof(WaveformPeak) * 2;
    auto service = fixture.Service(disk_budget, memory_budget);
    std::vector<QString> keys;
    for (std::uint8_t seed = 10; seed < 14; ++seed) {
        keys.push_back(service->request(SourceKey(seed), fixture.source_path, kThreeSecondsUs));
        ASSERT_TRUE(WaitFor([&]() { return service->state(keys.back()) == WaveformCacheService::State::Ready; }));
        EXPECT_LE(service->storeForTest().TotalBytes(), disk_budget);
    }
    EXPECT_LT(service->storeForTest().EntryCount(), keys.size());
    const QString survivor = keys.back();
    std::vector<WaveformPeak> peaks;
    for (std::uint32_t level = 0; level < service->levelCount(survivor); ++level) {
        ReadBlockBlocking(*service, survivor, level, 0, peaks);
        EXPECT_LE(service->memoryBytesForTest(), memory_budget);
    }
    EXPECT_GT(service->clearCache(), 0);
    EXPECT_EQ(FilesWithSuffix(fixture.root, ".wave"), 0u);
    EXPECT_EQ(service->state(survivor), WaveformCacheService::State::Unknown);
}

TEST(WaveformCacheService, SyntheticCorruptOrTruncatedEntryReanalysesOnceAndReopensWarm) {
    for (const bool truncated : {false, true}) {
        SCOPED_TRACE(truncated);
        WaveformFixture fixture;
        QString key;
        std::filesystem::path entry;
        std::vector<WaveformPeak> expected;
        {
            auto service = fixture.Service();
            key = service->request(SourceKey(52), fixture.source_path, kThreeSecondsUs);
            ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
            ASSERT_GT(ReadBlockBlocking(*service, key, 0, 0, expected), 0u);
            entry = service->storeForTest().EntryPath(key.toStdString());
        }
        if (truncated) {
            std::filesystem::resize_file(entry, 12);
        } else {
            std::fstream file(entry, std::ios::in | std::ios::out | std::ios::binary);
            ASSERT_TRUE(file.good());
            file.seekg(-1, std::ios::end);
            const auto value = file.get();
            ASSERT_NE(value, std::char_traits<char>::eof());
            file.seekp(-1, std::ios::end);
            file.put(static_cast<char>(value ^ 0xff));
            ASSERT_TRUE(file.good());
        }
        {
            auto service = fixture.Service();
            EXPECT_EQ(service->request(SourceKey(52), fixture.source_path, kThreeSecondsUs), key);
            ASSERT_TRUE(WaitFor([&]() {
                return service->state(key) == WaveformCacheService::State::Ready ||
                       service->state(key) == WaveformCacheService::State::Failed;
            }));
            ASSERT_EQ(service->state(key), WaveformCacheService::State::Ready);
            EXPECT_EQ(service->storeForTest().Stats().rejected, 1u);
            EXPECT_EQ(service->decoderOpensForTest(), 1u);
            EXPECT_GT(service->decodeCallsForTest(), 0u);
            std::vector<WaveformPeak> actual;
            ASSERT_GT(ReadBlockBlocking(*service, key, 0, 0, actual), 0u);
            EXPECT_EQ(actual, expected);
            EXPECT_EQ(service->storeForTest().EntryCount(), 1u);
            EXPECT_EQ(FilesWithSuffix(fixture.root, ".tmp"), 0u);
        }
        auto warm = fixture.Service();
        warm->request(SourceKey(52), fixture.source_path, kThreeSecondsUs);
        ASSERT_TRUE(WaitFor([&]() { return warm->state(key) == WaveformCacheService::State::Ready; }));
        std::vector<WaveformPeak> actual;
        ASSERT_GT(ReadBlockBlocking(*warm, key, 0, 0, actual), 0u);
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(warm->decoderOpensForTest(), 0u);
        EXPECT_EQ(warm->decodeCallsForTest(), 0u);
    }
}

TEST(WaveformCacheService, RealFixturesThroughTheServiceAndThroughput) {
    const auto directory = FixtureDirectory();
    if (!directory)
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    EnsureApplication();
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    WaveformCacheService::Config config;
    config.root = std::filesystem::path(temp.filePath(QStringLiteral("waveforms")).toStdWString());
    for (const auto& [name, level] : {std::pair{L"red.mkv", 0.25f}, std::pair{L"blue.mkv", -0.25f}}) {
        const auto path = *directory / name;
        const auto metadata = engine::ProbeEditMedia(path);
        ASSERT_TRUE(metadata.error.empty()) << metadata.error;
        engine::WaveformSourceKey source;
        source.content_sha256 = metadata.content_sha256;
        bool found_audio = false;
        for (const auto& stream : metadata.streams)
            if (stream.kind == engine::TimelineStreamKind::Audio) {
                source.container_index = stream.container_index;
                found_audio = true;
                break;
            }
        ASSERT_TRUE(found_audio);
        const auto started = std::chrono::steady_clock::now();
        QString key;
        std::vector<WaveformPeak> expected;
        {
            WaveformCacheService service(config, {});
            key = service.request(source, QString::fromStdWString(path.wstring()), metadata.duration_us);
            ASSERT_TRUE(WaitFor([&]() { return service.state(key) == WaveformCacheService::State::Ready; }));
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::cout << "[waveform-service] " << path.filename().string() << ": "
                      << static_cast<double>(service.totalSamples(key)) / 48000.0 << " s of audio ready in " << seconds
                      << " s\n";
            EXPECT_EQ(service.decoderOpensForTest(), 1u);
            EXPECT_GT(service.decodeCallsForTest(), 0u);
            const auto buckets = ReadBlockBlocking(service, key, 0, 0, expected);
            ASSERT_GT(buckets, 8u);
            for (std::uint32_t bucket = 2; bucket + 2 < buckets; ++bucket)
                for (int channel = 0; channel < 2; ++channel) {
                    const auto& peak = expected[bucket * 2 + channel];
                    ASSERT_EQ(peak.min, peak.max);
                    ASSERT_NEAR(engine::WaveformPeakToFloat(peak.max), level, 1e-3f);
                }
        }
        for (int cycle = 0; cycle < 4; ++cycle) {
            SCOPED_TRACE(cycle);
            WaveformCacheService warm(config, {});
            EXPECT_EQ(warm.request(source, QString::fromStdWString(path.wstring()), metadata.duration_us), key);
            ASSERT_TRUE(WaitFor([&]() { return warm.state(key) == WaveformCacheService::State::Ready; }));
            std::vector<WaveformPeak> actual;
            ASSERT_GT(ReadBlockBlocking(warm, key, 0, 0, actual), 0u);
            EXPECT_EQ(actual, expected);
            EXPECT_EQ(warm.decoderOpensForTest(), 0u);
            EXPECT_EQ(warm.decodeCallsForTest(), 0u);
            EXPECT_EQ(warm.decoderSeeksForTest(), 0u);
            EXPECT_LE(warm.memoryBytesForTest(), config.memory_budget_bytes);
            EXPECT_EQ(FilesWithSuffix(config.root, ".tmp"), 0u);
        }
        const auto after = engine::ProbeEditMedia(path);
        ASSERT_TRUE(after.error.empty()) << after.error;
        EXPECT_EQ(after.content_sha256, metadata.content_sha256);
    }
}

TEST(WaveformCacheService, RealSourceBoundedStreamingBenchmark) {
    const QString path = qEnvironmentVariable("EXOSNAP_WAVEFORM_BENCHMARK_FILE");
    if (path.isEmpty())
        GTEST_SKIP() << "Set EXOSNAP_WAVEFORM_BENCHMARK_FILE to real audio.";
    EnsureApplication();
    const auto metadata = engine::ProbeEditMedia(std::filesystem::path(path.toStdWString()));
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    engine::WaveformSourceKey source;
    source.content_sha256 = metadata.content_sha256;
    bool found = false;
    for (const auto& stream : metadata.streams)
        if (stream.kind == engine::TimelineStreamKind::Audio) {
            source.container_index = stream.container_index;
            found = true;
            break;
        }
    ASSERT_TRUE(found);
    QTemporaryDir temp;
    WaveformCacheService::Config config;
    config.root = std::filesystem::path(temp.path().toStdWString());
    config.analysis_budget_bytes = 2u << 20;
    QString key;
    {
        WaveformCacheService service(config, {});
        QSignalSpy failures(&service, &WaveformCacheService::failed);
        const auto started = std::chrono::steady_clock::now();
        key = service.request(source, path, metadata.duration_us);
        ASSERT_TRUE(WaitFor(
            [&]() {
                return service.state(key) == WaveformCacheService::State::Ready ||
                       service.state(key) == WaveformCacheService::State::Failed;
            },
            600000));
        ASSERT_EQ(service.state(key), WaveformCacheService::State::Ready)
            << (failures.empty() ? "" : failures.at(0).at(1).toString().toStdString());
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "[waveform-benchmark] file=" << path.toStdString() << " elapsed_s=" << seconds
                  << " samples=" << service.totalSamples(key) << " decode_calls=" << service.decodeCallsForTest()
                  << " seeks=" << service.decoderSeeksForTest() << " decoded_frames=" << service.decodedFramesForTest()
                  << " analysis_buffer_bytes=" << service.analysisMemoryBytesForTest()
                  << " disk_bytes=" << service.storeForTest().TotalBytes() << '\n';
        EXPECT_LE(service.analysisMemoryBytesForTest(), config.analysis_budget_bytes);
        std::vector<WaveformPeak> peaks;
        const auto buckets = engine::WaveformBucketCount(static_cast<std::uint64_t>(service.totalSamples(key)), 0);
        ASSERT_GT(ReadBlockBlocking(service, key, 0, (buckets - 1) / kWaveformBlockBuckets, peaks), 0u);
        EXPECT_LE(service.memoryBytesForTest(), config.memory_budget_bytes);
    }
    for (int cycle = 0; cycle < 8; ++cycle) {
        WaveformCacheService warm(config, {});
        warm.request(source, path, metadata.duration_us);
        ASSERT_TRUE(WaitFor([&]() { return warm.state(key) == WaveformCacheService::State::Ready; }));
        EXPECT_EQ(warm.decoderOpensForTest(), 0u) << "cycle=" << cycle;
        EXPECT_EQ(warm.decodeCallsForTest(), 0u) << "cycle=" << cycle;
        EXPECT_EQ(warm.decoderSeeksForTest(), 0u) << "cycle=" << cycle;
        std::vector<WaveformPeak> peaks;
        EXPECT_GT(ReadBlockBlocking(warm, key, 0, 0, peaks), 0u) << "cycle=" << cycle;
        EXPECT_LE(warm.memoryBytesForTest(), config.memory_budget_bytes);
    }
    std::cout << "[waveform-benchmark] warm_cycles=8 decode_calls=0 seeks=0\n";
}
TEST(WaveformCacheService, RealQueuedReplacementCannotPublishUnderOriginalIdentity) {
    const auto directory = FixtureDirectory();
    if (!directory)
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    EnsureApplication();
    const auto original = *directory / "red.mkv";
    const auto replacement = *directory / "blue.mkv";
    const auto metadata = engine::ProbeEditMedia(original);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    engine::WaveformSourceKey source;
    source.content_sha256 = metadata.content_sha256;
    for (const auto& stream : metadata.streams)
        if (stream.kind == engine::TimelineStreamKind::Audio) {
            source.container_index = stream.container_index;
            break;
        }
    QTemporaryDir temp;
    const auto queued = std::filesystem::path(temp.filePath(QStringLiteral("queued.mkv")).toStdWString());
    std::filesystem::copy_file(original, queued);
    WaveformCacheService::Config config;
    config.root = std::filesystem::path(temp.filePath(QStringLiteral("cache")).toStdWString());
    WaveformCacheService service(config, {});
    service.setForegroundBusy(true);
    const auto key = service.request(source, QString::fromStdWString(queued.wstring()), metadata.duration_us);
    std::filesystem::copy_file(replacement, queued, std::filesystem::copy_options::overwrite_existing);
    service.setForegroundBusy(false);
    ASSERT_TRUE(WaitFor([&]() { return service.state(key) == WaveformCacheService::State::Failed; }));
    EXPECT_EQ(service.decodeCallsForTest(), 0u);
    EXPECT_FALSE(service.storeForTest().Contains(key.toStdString()));
    EXPECT_EQ(FilesWithSuffix(config.root, ".tmp"), 0u);
    EXPECT_EQ(service.request(source, QString::fromStdWString(original.wstring()), metadata.duration_us), key);
    ASSERT_TRUE(WaitFor([&]() { return service.state(key) == WaveformCacheService::State::Ready; }));
    std::vector<WaveformPeak> peaks;
    ASSERT_GT(ReadBlockBlocking(service, key, 0, 0, peaks), 8u);
    EXPECT_NEAR(engine::WaveformPeakToFloat(peaks[4].max), 0.25f, 1e-3f);
}
// ---- Waveform item ----

class TestableWaveformItem : public ExoWaveformItem {
  public:
    using ExoWaveformItem::updatePaintNode;
    using ExoWaveformItem::updatePolish;
};

QSGGeometry* FirstPcmGeometry(QSGNode* node) {
    auto* transform = node ? dynamic_cast<QSGTransformNode*>(node->firstChild()) : nullptr;
    auto* geometry = transform ? dynamic_cast<QSGGeometryNode*>(transform->firstChild()) : nullptr;
    return geometry && geometry->geometry()->drawingMode() == QSGGeometry::DrawLines ? geometry->geometry() : nullptr;
}

TEST(ExoWaveformItem, PcmWindowsJoinContiguousSamplesAndRetainGeometryAcrossPan) {
    WaveformFixture fixture;
    fixture.control->sample_amplitude = [](std::int64_t sample) { return sample == 4096 ? 0.8f : 0.0f; };
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(115), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&] { return service->state(key) == WaveformCacheService::State::Ready; }));
    TestableWaveformItem item;
    item.setSize(QSizeF(64, 80));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setSourceInUs(85000);
    item.setSourceOutUs(85666);
    QSGNode* node = nullptr;
    ASSERT_TRUE(WaitFor([&] {
        item.updatePolish();
        node = item.updatePaintNode(node, nullptr);
        return FirstPcmGeometry(node) && node->childCount() == 2;
    }));
    std::unique_ptr<QSGNode> owner(node);
    auto* geometry = FirstPcmGeometry(node);
    ASSERT_EQ(geometry->vertexCount(), 8192);
    const auto* vertices = geometry->vertexDataAsPoint2D();
    EXPECT_FLOAT_EQ(vertices[8190].y, 20);
    EXPECT_NEAR(vertices[8191].y, 4.8f, 1e-4f);
    item.setSourceInUs(85020);
    item.setSourceOutUs(85687);
    node = item.updatePaintNode(owner.release(), nullptr);
    owner.reset(node);
    EXPECT_EQ(FirstPcmGeometry(node), geometry);
}

TEST(ExoWaveformItem, SinglePcmLaneRetainsOppositePolaritiesWithoutAveraging) {
    WaveformFixture fixture;
    fixture.control->sample_amplitude = [](std::int64_t) { return 0.5f; };
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(122), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&] { return service->state(key) == WaveformCacheService::State::Ready; }));
    TestableWaveformItem item;
    item.setSize(QSizeF(256, 80));
    item.setChannelCount(1);
    item.setService(service.get());
    item.setCacheKey(key);
    item.setSourceOutUs(2666);
    std::unique_ptr<QSGNode> owner;
    ASSERT_TRUE(WaitFor([&] {
        item.updatePolish();
        owner.reset(item.updatePaintNode(owner.release(), nullptr));
        return FirstPcmGeometry(owner.get());
    }));
    const auto* transform = owner->firstChild();
    ASSERT_NE(transform, nullptr);
    int channels = 0;
    for (auto* node = transform->firstChild(); node; node = node->nextSibling()) {
        const auto* geometry = dynamic_cast<const QSGGeometryNode*>(node);
        ASSERT_NE(geometry, nullptr);
        ASSERT_GT(geometry->geometry()->vertexCount(), 0);
        EXPECT_NEAR(geometry->geometry()->vertexDataAsPoint2D()[0].y, channels == 0 ? 20.25f : 59.75f, 1e-4f);
        ++channels;
    }
    EXPECT_EQ(channels, 2);
}
TEST(ExoWaveformItem, CacheClearReacquiresDetailForTheSameSourceWindow) {
    WaveformFixture fixture;
    std::atomic<float> amplitude{0.25f};
    fixture.control->sample_amplitude = [&amplitude](std::int64_t) { return amplitude.load(); };
    auto service = fixture.Service();
    const auto source = SourceKey(116);
    const auto key = service->request(source, fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&] { return service->state(key) == WaveformCacheService::State::Ready; }));
    TestableWaveformItem item;
    item.setSize(QSizeF(256, 80));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setSourceOutUs(2666);
    QSGNode* node = nullptr;
    ASSERT_TRUE(WaitFor([&] {
        item.updatePolish();
        node = item.updatePaintNode(node, nullptr);
        return FirstPcmGeometry(node);
    }));
    std::unique_ptr<QSGNode> owner(node);
    EXPECT_NEAR(FirstPcmGeometry(node)->vertexDataAsPoint2D()[0].y, 15.25f, 1e-4f);
    service->clearCache();
    amplitude = 0.75f;
    EXPECT_EQ(service->request(source, fixture.source_path, kThreeSecondsUs), key);
    ASSERT_TRUE(WaitFor(
        [&] {
            item.updatePolish();
            node = item.updatePaintNode(owner.release(), nullptr);
            owner.reset(node);
            auto* geometry = FirstPcmGeometry(node);
            return geometry && std::abs(geometry->vertexDataAsPoint2D()[0].y - 5.75f) < 1e-4f;
        },
        5000));
}

TEST(ExoWaveformItem, PcmCoverageDrawsSilenceWithoutBridgingPadding) {
    WaveformFixture fixture;
    fixture.control->sample_amplitude = [](std::int64_t sample) {
        return sample == 4096 || sample == 4097 ? 0.8f : 0.0f;
    };
    fixture.control->sample_coverage = [](std::int64_t first, std::uint32_t count) {
        std::vector<engine::AudioSampleSpan> covered;
        for (const auto span : {engine::AudioSampleSpan{4090, 4096}, engine::AudioSampleSpan{4097, 4100}}) {
            const auto begin = std::max(first, span.first);
            const auto end = std::min(first + count, span.end);
            if (begin < end)
                covered.push_back({begin, end});
        }
        return covered;
    };
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(118), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&] { return service->state(key) == WaveformCacheService::State::Ready; }));
    TestableWaveformItem item;
    item.setSize(QSizeF(64, 80));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setSourceInUs(85000);
    item.setSourceOutUs(85666);
    std::unique_ptr<QSGNode> owner;
    ASSERT_TRUE(WaitFor([&] {
        item.updatePolish();
        owner.reset(item.updatePaintNode(owner.release(), nullptr));
        return FirstPcmGeometry(owner.get()) && owner->childCount() == 2;
    }));
    const auto* silence = FirstPcmGeometry(owner.get());
    ASSERT_EQ(silence->vertexCount(), 12);
    EXPECT_FLOAT_EQ(silence->vertexDataAsPoint2D()[0].x, 4090);
    EXPECT_FLOAT_EQ(silence->vertexDataAsPoint2D()[11].x, 4096);
    EXPECT_FLOAT_EQ(silence->vertexDataAsPoint2D()[11].y, 20);
    auto* transform = static_cast<QSGTransformNode*>(owner->firstChild()->nextSibling());
    const auto* second = static_cast<QSGGeometryNode*>(transform->firstChild())->geometry();
    ASSERT_EQ(second->vertexCount(), 6);
    EXPECT_FLOAT_EQ(second->vertexDataAsPoint2D()[0].x, 1);
    EXPECT_NEAR(second->vertexDataAsPoint2D()[0].y, 4.8f, 1e-4f);
}

TEST(ExoWaveformItem, PcmDetailRecoversAfterPinnedMemoryIsReleasedWithoutChangingTheView) {
    WaveformFixture fixture;
    constexpr std::uint32_t count = 4096;
    constexpr auto budget = sizeof(WaveformCacheService::PcmWindow) + 65 +
                            count * (sizeof(float) * engine::kWaveformChannels + sizeof(engine::AudioSampleSpan)) +
                            count * sizeof(float) * engine::kWaveformChannels;
    auto service = fixture.Service(64ull << 20, budget);
    const auto key = service->request(SourceKey(119), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&] { return service->state(key) == WaveformCacheService::State::Ready; }));
    const auto held = service->requestPcmWindow(key, 0, count, 1);
    ASSERT_TRUE(WaitFor([&] { return service->pcmWindow(held).state == WaveformCacheService::DetailState::Ready; }));
    auto pinned = service->pcmWindow(held).window;
    service->cancelPcmWindow(held);
    TestableWaveformItem item;
    item.setSize(QSizeF(256, 80));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setSourceInUs(85333);
    item.setSourceOutUs(87999);
    item.updatePolish();
    const auto pressure = service->requestPcmWindow(key, count, count, 2);
    ASSERT_TRUE(
        WaitFor([&] { return service->pcmWindow(pressure).state != WaveformCacheService::DetailState::Pending; }));
    ASSERT_EQ(service->pcmWindow(pressure).state, WaveformCacheService::DetailState::MemoryPressure);
    service->cancelPcmWindow(pressure);
    pinned.reset();
    std::unique_ptr<QSGNode> owner;
    ASSERT_TRUE(WaitFor(
        [&] {
            item.updatePolish();
            owner.reset(item.updatePaintNode(owner.release(), nullptr));
            return FirstPcmGeometry(owner.get());
        },
        5000));
}

TEST(ExoWaveformItem, ResolvedMissingCoverageDoesNotFallBackToAPaddedSilenceLine) {
    WaveformFixture fixture;
    fixture.control->sample_amplitude = [](std::int64_t) { return 0.0f; };
    fixture.control->omit_coverage = true;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(120), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&] { return service->state(key) == WaveformCacheService::State::Ready; }));
    ASSERT_TRUE(WaitFor([&] { return !!service->tryReadPublishedBlock(key, 0, 0); }));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow window;
    window.setGeometry(0, 0, 400, 140);
    window.setColor(Qt::black);
    QQuickItem viewport(window.contentItem());
    viewport.setPosition(QPointF(50, 60));
    viewport.setSize(QSizeF(300, 48));
    viewport.setClip(true);
    ExoWaveformItem item(&viewport);
    item.setSize(QSizeF(300, 40));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setColor(Qt::magenta);
    item.setSourceOutUs(1'600'000);
    window.show();
    const auto coloredPixels = [&] {
        const auto image = window.grabWindow();
        if (image.isNull())
            return -1;
        int count = 0;
        for (int y = 60; y < 100; ++y)
            for (int x = 50; x < 350; ++x)
                if (image.pixelColor(x * image.width() / window.width(), y * image.height() / window.height()).red() >
                    20)
                    ++count;
        return count;
    };
    ASSERT_TRUE(WaitFor([&] { return coloredPixels() > 100 && item.levelForTest() == 1; }));
    item.setSourceOutUs(3125);
    const auto probe = service->requestPcmWindow(key, 0, 4096, 1);
    ASSERT_TRUE(WaitFor([&] { return service->pcmWindow(probe).state != WaveformCacheService::DetailState::Pending; }));
    EXPECT_EQ(service->pcmWindow(probe).state, WaveformCacheService::DetailState::NoCoverage);
    ASSERT_TRUE(WaitFor([&] { return coloredPixels() == 0 && item.levelForTest() == -1; }, 5000));
    service->cancelPcmWindow(probe);
}

struct DrawnBucket {
    double x = 0;
    float top = 0;
    float bottom = 0;
};

// Item-space start and extent of every bucket in the first lane's peak band.
std::vector<DrawnBucket> DrawnBuckets(QSGNode* root) {
    std::vector<DrawnBucket> out;
    for (auto* child = root ? root->firstChild() : nullptr; child; child = child->nextSibling()) {
        auto* transform = static_cast<QSGTransformNode*>(child);
        const auto& matrix = transform->matrix();
        auto* peak = static_cast<QSGGeometryNode*>(transform->firstChild());
        const auto* vertices = peak->geometry()->vertexDataAsPoint2D();
        for (int i = 0; i + 3 < peak->geometry()->vertexCount(); i += 4) {
            const QPointF mapped = matrix.map(QPointF(vertices[i].x, vertices[i].y));
            out.push_back(DrawnBucket{mapped.x(), vertices[i].y, vertices[i + 1].y});
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
    return out;
}

TEST(ExoWaveformItem, LevelOfDetailKeepsAtLeastOneBucketPerColumn) {
    EXPECT_EQ(WaveformLevelForSamplesPerPixel(10, 8), 0);
    EXPECT_EQ(WaveformLevelForSamplesPerPixel(128, 8), 0);
    EXPECT_EQ(WaveformLevelForSamplesPerPixel(255, 8), 0);
    EXPECT_EQ(WaveformLevelForSamplesPerPixel(256, 8), 1);
    EXPECT_EQ(WaveformLevelForSamplesPerPixel(1e9, 8), 7);
    EXPECT_EQ(WaveformLevelForSamplesPerPixel(1000, 0), -1);
}

TEST(ExoWaveformItem, SoftwareRendererDrawsAnalyzedPeaksAndLeavesMissingDataBlank) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const QString key = service->request(SourceKey(22), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow window;
    window.setGeometry(0, 0, 400, 140);
    window.setColor(Qt::black);
    QQuickItem viewport(window.contentItem());
    viewport.setPosition(QPointF(50, 60));
    viewport.setSize(QSizeF(300, 48));
    viewport.setClip(true);
    ExoWaveformItem item(&viewport);
    item.setSize(QSizeF(300, 40));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setColor(Qt::magenta);
    item.setSourceOutUs(1'600'000);
    window.show();

    const auto coloredPixels = [&item](const QImage& window_image, bool upper_lane) {
        const QImage image = window_image.copy(QRect(50, 60, 300, static_cast<int>(item.height())));
        int count = 0;
        const int first_y = upper_lane ? 0 : image.height() / 2;
        const int last_y = upper_lane ? image.height() / 2 : image.height();
        for (int y = first_y; y < last_y; ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.red() > 20 && pixel.blue() > 20 && pixel.green() < 10)
                    ++count;
            }
        return count;
    };
    QImage rendered;
    ASSERT_TRUE(WaitFor(
        [&]() {
            rendered = window.grabWindow();
            return coloredPixels(rendered, true) > 100 && coloredPixels(rendered, false) > 100;
        },
        5000));
    EXPECT_EQ(window.rendererInterface()->graphicsApi(), QSGRendererInterface::Software);
    EXPECT_EQ(item.levelForTest(), 1);

    item.setHeight(48);
    item.setSourceOutUs(800'000);
    ASSERT_TRUE(WaitFor([&]() {
        rendered = window.grabWindow();
        return item.levelForTest() == 0 && coloredPixels(rendered, true) > 100 && coloredPixels(rendered, false) > 100;
    }));
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < rendered.width(); ++x)
            EXPECT_EQ(rendered.pixelColor(x, y), QColor(Qt::black));

    item.setSourceInUs(4'000'000);
    item.setSourceOutUs(5'600'000);
    ASSERT_TRUE(WaitFor([&]() {
        rendered = window.grabWindow();
        return !rendered.isNull() && coloredPixels(rendered, true) == 0 && coloredPixels(rendered, false) == 0;
    }));
}

TEST(ExoWaveformItem, SoftwareRasterIsBoundedByTheClippedViewport) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(93), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow window;
    window.setGeometry(0, 0, 400, 140);
    QQuickItem viewport(window.contentItem());
    viewport.setPosition(QPointF(50, 60));
    viewport.setSize(QSizeF(300, 48));
    viewport.setClip(true);
    TestableWaveformItem item;
    item.setParentItem(&viewport);
    item.setPosition(QPointF(-5000, 0));
    item.setSize(QSizeF(10000, 40));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setSourceOutUs(kThreeSecondsUs);
    window.show();
    ASSERT_TRUE(WaitFor([&]() { return !window.grabWindow().isNull(); }));
    std::unique_ptr<QSGNode> node(item.updatePaintNode(nullptr, nullptr));
    ASSERT_TRUE(node);
    auto* image = dynamic_cast<QSGImageNode*>(node->firstChild());
    ASSERT_TRUE(image);
    ASSERT_TRUE(image->texture());
    EXPECT_LE(image->texture()->textureSize().width(), qCeil(300 * window.effectiveDevicePixelRatio()));
    EXPECT_EQ(image->rect(), QRectF(5000, 0, 300, 40));
}

TEST(ExoWaveformItem, SampleZoomDrawsIndividualPcmImpulseAndReusesWarmWindow) {
    WaveformFixture fixture;
    fixture.control->sample_amplitude = [](std::int64_t sample) { return sample == 17 ? 0.8f : 0.0f; };
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(114), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&] { return service->state(key) == WaveformCacheService::State::Ready; }));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow window;
    window.setGeometry(0, 0, 400, 140);
    window.setColor(Qt::black);
    QQuickItem viewport(window.contentItem());
    viewport.setPosition(QPointF(50, 60));
    viewport.setSize(QSizeF(300, 48));
    viewport.setClip(true);
    ExoWaveformItem item(&viewport);
    item.setSize(QSizeF(300, 40));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setColor(Qt::magenta);
    item.setSourceOutUs(3125);
    window.show();
    const auto ink = [&window](const QImage& image, int x, int y) {
        if (image.isNull())
            return false;
        const auto pixel =
            image.pixelColor((x + 50) * image.width() / window.width(), (y + 60) * image.height() / window.height());
        return pixel.red() > 20 && pixel.blue() > 20 && pixel.green() < 10;
    };
    ASSERT_TRUE(WaitFor(
        [&] {
            const auto image = window.grabWindow();
            return ink(image, 34, 3) && ink(image, 100, 10) && !ink(image, 100, 5);
        },
        5000))
        << "size=" << item.width() << "," << item.height() << " visible=" << item.isVisible()
        << " level=" << item.levelForTest() << " pcm=" << service->pcmMemoryBytesForTest()
        << " calls=" << fixture.control->calls.load();
    const auto calls = fixture.control->calls.load();
    const auto opens = fixture.control->opens.load();
    item.setSourceInUs(1000);
    item.setSourceOutUs(4125);
    ASSERT_TRUE(WaitFor(
        [&] {
            const auto image = window.grabWindow();
            return ink(image, 100, 10) && !ink(image, 34, 3);
        },
        5000))
        << "size=" << item.width() << "," << item.height() << " visible=" << item.isVisible()
        << " level=" << item.levelForTest() << " pcm=" << service->pcmMemoryBytesForTest()
        << " calls=" << fixture.control->calls.load();
    item.setSourceInUs(0);
    item.setSourceOutUs(3125);
    ASSERT_TRUE(WaitFor([&] { return ink(window.grabWindow(), 34, 3); }, 5000))
        << "size=" << item.width() << "," << item.height() << " visible=" << item.isVisible()
        << " level=" << item.levelForTest() << " pcm=" << service->pcmMemoryBytesForTest()
        << " calls=" << fixture.control->calls.load();
    EXPECT_EQ(fixture.control->calls.load(), calls);
    EXPECT_EQ(fixture.control->opens.load(), opens);
}

TEST(ExoWaveformItem, AncestorClippingRetiresPcmInterests) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(117), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&] { return service->state(key) == WaveformCacheService::State::Ready; }));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow window;
    window.setGeometry(0, 0, 400, 140);
    QQuickItem viewport(window.contentItem());
    viewport.setPosition(QPointF(50, 60));
    viewport.setSize(QSizeF(300, 48));
    viewport.setClip(true);
    QQuickItem clip(&viewport);
    clip.setSize(QSizeF(300, 40));
    clip.setClip(true);
    ExoWaveformItem item(&clip);
    item.setSize(QSizeF(300, 40));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setSourceOutUs(3125);
    window.show();
    const auto availableTickets = [&] {
        std::vector<std::uint64_t> tickets;
        for (int i = 0; i < 512; ++i) {
            const auto ticket = service->requestPcmWindow(key, 0, 4096, 1);
            if (!ticket)
                break;
            tickets.push_back(ticket);
        }
        for (const auto ticket : tickets)
            service->cancelPcmWindow(ticket);
        return tickets.size();
    };
    ASSERT_TRUE(WaitFor([&] {
        (void)window.grabWindow();
        return service->pcmMemoryBytesForTest() > 0 && item.levelForTest() == -1 && availableTickets() == 511;
    }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_EQ(availableTickets(), 511u);
    viewport.setX(-1000);
    ASSERT_TRUE(WaitFor(
        [&] {
            (void)window.grabWindow();
            return availableTickets() == 512;
        },
        5000));
    viewport.setX(50);
    ASSERT_TRUE(WaitFor([&] {
        (void)window.grabWindow();
        return availableTickets() == 511;
    }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    ASSERT_EQ(availableTickets(), 511u);
    viewport.setHeight(0);
    ASSERT_TRUE(WaitFor(
        [&] {
            (void)window.grabWindow();
            return availableTickets() == 512;
        },
        5000));
}

TEST(ExoWaveformItem, PanMovesBlocksWithoutShiftingBucketsOrRebuilding) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const QString key = service->request(SourceKey(20), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));

    TestableWaveformItem item;
    item.setSize(QSizeF(300, 40));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setColor(Qt::green);
    // 300 px over 76 800 samples: 256 samples per pixel, level 1.
    item.setSourceInUs(0);
    item.setSourceOutUs(1'600'000);
    QSGNode* node = nullptr;
    ASSERT_TRUE(WaitFor([&]() {
        node = item.updatePaintNode(node, nullptr);
        return node && node->childCount() > 0;
    }));
    EXPECT_EQ(item.levelForTest(), 1);
    const auto before = DrawnBuckets(node);
    ASSERT_GE(before.size(), 300u);
    auto* first_geometry = static_cast<QSGGeometryNode*>(node->firstChild()->firstChild())->geometry();

    // Pan by 768 samples (16 ms): three level-1 buckets, three pixels.
    item.setSourceInUs(16'000);
    item.setSourceOutUs(1'616'000);
    node = item.updatePaintNode(node, nullptr);
    EXPECT_EQ(static_cast<QSGGeometryNode*>(node->firstChild()->firstChild())->geometry(), first_geometry);
    const auto after = DrawnBuckets(node);
    for (const auto& bucket : before) {
        const auto moved = std::find_if(after.begin(), after.end(), [&](const DrawnBucket& candidate) {
            return std::abs(candidate.x - (bucket.x - 3.0)) < 1e-3;
        });
        ASSERT_NE(moved, after.end()) << bucket.x;
        EXPECT_FLOAT_EQ(moved->top, bucket.top);
        EXPECT_FLOAT_EQ(moved->bottom, bucket.bottom);
    }
    // Bucket edges land on whole pixels at this zoom, independent of the pan.
    for (const auto& bucket : after)
        EXPECT_NEAR(bucket.x, std::round(bucket.x), 1e-3);

    // Two lanes: the left channel is drawn above the centre line.
    const auto& sample = after[after.size() / 2];
    EXPECT_LT(sample.bottom, 20.0f);
    delete node;
}

TEST(ExoWaveformItem, DrawsNothingWithoutData) {
    WaveformFixture fixture;
    fixture.control->gated = true;
    auto service = fixture.Service();
    const QString key = service->request(SourceKey(21), fixture.source_path, kThreeSecondsUs);
    TestableWaveformItem item;
    item.setSize(QSizeF(300, 40));
    item.setService(service.get());
    item.setCacheKey(key);
    item.setSourceOutUs(1'600'000);
    QSGNode* node = item.updatePaintNode(nullptr, nullptr);
    EXPECT_TRUE(node == nullptr || node->childCount() == 0);
    delete node;
    service->cancel(key);
    fixture.control->Open();
}

// ---- Thumbnails ----

namespace {

std::array<std::uint8_t, 32> Identity(std::uint8_t seed) {
    std::array<std::uint8_t, 32> identity{};
    for (std::size_t i = 0; i < identity.size(); ++i)
        identity[i] = static_cast<std::uint8_t>(seed + i * 3);
    return identity;
}

QImage SolidImage(int width, int height, QRgb color) {
    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(color);
    return image;
}

struct ThumbnailFixture {
    QTemporaryDir temp;
    std::filesystem::path root;
    ThumbnailFixture() {
        EnsureApplication();
        root = std::filesystem::path(temp.filePath(QStringLiteral("thumbnails")).toStdWString());
    }
    std::shared_ptr<ThumbnailRequestBroker> Broker(std::uint64_t budget = 64ull << 20) const {
        ThumbnailRequestBroker::Config config;
        config.disk_root = root;
        config.disk_budget_bytes = budget;
        return std::make_shared<ThumbnailRequestBroker>(config);
    }
};

struct Result {
    std::mutex mutex;
    std::optional<QImage> image;
    int completions = 0;
};

ThumbnailCompletion Capture(const std::shared_ptr<Result>& result) {
    return [result](const QImage& image) {
        std::lock_guard lock(result->mutex);
        result->image = image;
        ++result->completions;
    };
}

bool Done(const std::shared_ptr<Result>& result) {
    std::lock_guard lock(result->mutex);
    return result->completions > 0;
}

} // namespace

TEST(ThumbnailCache, KeysFollowIdentityTimeHeightAndConversion) {
    const auto key = ThumbnailCacheKey(Identity(1), 1'000'000, 48);
    EXPECT_EQ(key, ThumbnailCacheKey(Identity(1), 1'000'000, 48));
    EXPECT_NE(key, ThumbnailCacheKey(Identity(2), 1'000'000, 48));
    EXPECT_NE(key, ThumbnailCacheKey(Identity(1), 1'000'001, 48));
    EXPECT_NE(key, ThumbnailCacheKey(Identity(1), 1'000'000, 49));
    EXPECT_NE(key, ThumbnailCacheKey(Identity(1), 1'000'000, 48, ThumbnailVariant::AspectFitHeight,
                                     kThumbnailColorConversionVersion + 1));
}

TEST(ThumbnailCache, TileGridIsSharedAcrossClipsPansAndZoomOctaves) {
    // 30 fps, 48 px tiles of a 16:9 source: 85 px per tile.
    const auto clip_a = ThumbnailTilesForWindow(0, 10'000'000, 1000, 48, 1920, 1080, 30, 1);
    const auto clip_b = ThumbnailTilesForWindow(3'000'000, 13'000'000, 1000, 48, 1920, 1080, 30, 1);
    ASSERT_FALSE(clip_a.empty());
    std::set<std::int64_t> times_a;
    for (const auto& tile : clip_a)
        times_a.insert(tile.time_us);
    std::size_t shared = 0;
    for (const auto& tile : clip_b)
        shared += times_a.contains(tile.time_us);
    EXPECT_GT(shared, clip_b.size() / 2);
    for (std::size_t i = 1; i < clip_a.size(); ++i)
        EXPECT_NEAR(clip_a[i].x, clip_a[i - 1].x + clip_a[i - 1].width, 1e-6);
    EXPECT_GE(clip_a.front().width, 85.0 - 1e-6);

    const auto zoomed = ThumbnailTilesForWindow(0, 5'000'000, 1000, 48, 1920, 1080, 30, 1);
    std::size_t reused = 0;
    for (const auto& tile : zoomed)
        reused += times_a.contains(tile.time_us);
    EXPECT_GE(reused * 2 + 2, zoomed.size());
}

TEST(ThumbnailCache, DiskTierRejectsCorruptionAndHoldsItsBudget) {
    ThumbnailFixture fixture;
    ThumbnailDiskCache cache(fixture.root, 64ull << 20);
    const auto key = ThumbnailCacheKey(Identity(3), 0, 48);
    ASSERT_TRUE(cache.Store(key, SolidImage(85, 48, qRgb(200, 10, 10)), 0));
    const auto loaded = cache.Load(key);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->pixel(10, 10), qRgb(200, 10, 10));

    const auto path = cache.store().EntryPath(key);
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(-20, std::ios::end);
        file.put('\x13');
    }
    ThumbnailDiskCache reopened(fixture.root, 64ull << 20);
    EXPECT_FALSE(reopened.Load(key).has_value());
    EXPECT_FALSE(std::filesystem::exists(path));

    const std::uint64_t budget = 8 * 1024;
    ThumbnailDiskCache bounded(fixture.root / "small", budget);
    for (int i = 0; i < 40; ++i) {
        QImage noisy(85, 48, QImage::Format_ARGB32);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 85; ++x)
                noisy.setPixel(x, y, qRgb((x * 37 + i) & 255, (y * 91 + i * 7) & 255, (x * y + i) & 255));
        bounded.Store(ThumbnailCacheKey(Identity(4), i * 1000, 48), noisy, i * 1000);
        EXPECT_LE(bounded.store().TotalBytes(), budget);
    }
}

TEST(ThumbnailBroker, UnknownSourcesCancellationAndDetachAlwaysComplete) {
    ThumbnailFixture fixture;
    auto broker = fixture.Broker();
    auto unattached = std::make_shared<Result>();
    broker->RegisterSource(engine::ToHex(Identity(5)), ThumbnailSourceInfo{"x.mkv", Identity(5), 0, 0, 0, 1});
    EXPECT_EQ(broker->Request(engine::ToHex(Identity(5)), 0, 48, Capture(unattached)), 0u);
    EXPECT_TRUE(Done(unattached));

    broker->AttachWorker([]() {});
    auto unknown = std::make_shared<Result>();
    broker->Request("feedface", 0, 48, Capture(unknown));
    EXPECT_TRUE(Done(unknown));
    EXPECT_TRUE(unknown->image->isNull());

    auto cancelled = std::make_shared<Result>();
    const auto ticket = broker->Request(engine::ToHex(Identity(5)), 0, 48, Capture(cancelled));
    ASSERT_NE(ticket, 0u);
    EXPECT_FALSE(Done(cancelled));
    broker->Cancel(ticket);
    EXPECT_TRUE(Done(cancelled));
    broker->Cancel(ticket);
    EXPECT_EQ(cancelled->completions, 1);
    EXPECT_FALSE(broker->HasWork());

    auto orphan = std::make_shared<Result>();
    broker->Request(engine::ToHex(Identity(5)), 33'333, 48, Capture(orphan));
    broker->DetachWorker();
    EXPECT_TRUE(Done(orphan));
    EXPECT_EQ(orphan->completions, 1);
}

TEST(ThumbnailBroker, ProviderResponseFinishesForUnknownSources) {
    ThumbnailFixture fixture;
    auto broker = fixture.Broker();
    broker->AttachWorker([]() {});
    EditThumbnailImageProvider provider(broker);
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(QStringLiteral("%1/0/48").arg(QString(64, QLatin1Char('a'))), QSize()));
    QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
    ASSERT_TRUE(WaitFor([&]() { return finished.count() == 1; }));
    EXPECT_FALSE(response->errorString().isEmpty());
    EXPECT_EQ(EditThumbnailUrl(QString(64, QLatin1Char('A')), 5, 48),
              QStringLiteral("image://exothumb/%1/5/48").arg(QString(64, QLatin1Char('a'))));
    broker->DetachWorker();
}

TEST(ThumbnailBroker, DestroyingAResponseWithdrawsItsQueuedRequest) {
    ThumbnailFixture fixture;
    auto broker = fixture.Broker();
    broker->AttachWorker([]() {});
    const auto identity = engine::ToHex(Identity(6));
    broker->RegisterSource(identity, ThumbnailSourceInfo{"x.mkv", Identity(6), 0, 0, 0, 1});
    EditThumbnailImageProvider provider(broker);
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(QString::fromStdString(identity) + QStringLiteral("/0/48"), QSize()));
    ASSERT_TRUE(broker->HasWork());
    response.reset();
    EXPECT_FALSE(broker->HasWork());
}

TEST(ThumbnailBroker, CompletingAnInFlightRequestAfterResponseDestructionIsSafe) {
    ThumbnailFixture fixture;
    auto broker = fixture.Broker();
    broker->AttachWorker([]() {});
    const auto identity = engine::ToHex(Identity(6));
    broker->RegisterSource(identity, ThumbnailSourceInfo{"x.mkv", Identity(6), 0, 0, 0, 1});
    EditThumbnailImageProvider provider(broker);
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(QString::fromStdString(identity) + QStringLiteral("/0/48"), QSize()));
    const auto job = broker->TakeJob();
    ASSERT_TRUE(job);
    response.reset();
    QImage image(48, 48, QImage::Format_RGB32);
    image.fill(Qt::red);
    broker->Complete(*job, image);
    QCoreApplication::processEvents();
    EXPECT_FALSE(broker->HasWork());
}

TEST(WaveformCacheService, RepeatedReadyRequestChecksAvailabilityOnTheWorker) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(94), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    {
        std::lock_guard lock(fixture.control->mutex);
        fixture.control->gated = true;
    }
    struct UnblockDecoder {
        std::shared_ptr<DecoderControl> control;
        ~UnblockDecoder() {
            control->Open();
        }
    } unblock{fixture.control};
    service->request(SourceKey(95), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return fixture.control->opens.load() == 2; }));
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::path(fixture.source_path.toStdWString())));
    service->request(SourceKey(94), fixture.source_path, kThreeSecondsUs);
    EXPECT_TRUE(service->sourceAvailable(key)) << "Availability was synchronously checked on the requesting thread";
    fixture.control->Open();
    ASSERT_TRUE(WaitFor([&]() { return !service->sourceAvailable(key); }));
}

TEST(WaveformCacheService, ReadyRequestRechecksMissingSourceWithoutDecoding) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(1), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::path(fixture.source_path.toStdWString())));
    QSignalSpy availability(service.get(), &WaveformCacheService::sourceAvailabilityChanged);
    const auto calls = service->decodeCallsForTest();
    EXPECT_EQ(service->request(SourceKey(1), fixture.source_path, kThreeSecondsUs), key);
    ASSERT_TRUE(WaitFor([&]() { return !service->sourceAvailable(key); }));
    EXPECT_EQ(service->decodeCallsForTest(), calls);
    ASSERT_TRUE(WaitFor([&]() { return availability.count() == 1; }));
}

TEST(ThumbnailBroker, ClipsOfOneSourceShareTilesAndScrollingBackNeverDecodes) {
    const auto directory = FixtureDirectory();
    if (!directory)
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    ThumbnailFixture fixture;
    const auto path = *directory / L"red.mkv";
    const auto metadata = engine::ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    const auto identity = engine::ToHex(metadata.content_sha256);
    const auto info = ThumbnailSourceInfo{
        path, metadata.content_sha256, metadata.width, metadata.height, metadata.fps_num, metadata.fps_den};

    const auto request_window = [&](ThumbnailRequestBroker& broker, std::int64_t in_us, std::int64_t out_us,
                                    double width) {
        const auto tiles = ThumbnailTilesForWindow(in_us, out_us, width, 48, info.video_width, info.video_height,
                                                   info.fps_num, info.fps_den);
        std::vector<std::shared_ptr<Result>> results;
        for (const auto& tile : tiles) {
            results.push_back(std::make_shared<Result>());
            broker.Request(identity, tile.time_us, 48, Capture(results.back()));
        }
        EXPECT_TRUE(WaitFor([&]() { return std::all_of(results.begin(), results.end(), Done); }));
        for (const auto& result : results) {
            EXPECT_TRUE(result->image.has_value() && !result->image->isNull());
            if (result->image)
                EXPECT_EQ(result->image->height(), 48);
        }
        return tiles.size();
    };

    std::uint64_t first_decodes = 0;
    {
        auto broker = fixture.Broker();
        TimelineThumbnailSource worker(broker);
        broker->RegisterSource(identity, info);
        // Clip A covers the first second, clip B the whole source at another
        // zoom and position; then the view scrolls back to clip A.
        ASSERT_GT(request_window(*broker, 0, 1'000'000, 400), 0u);
        first_decodes = worker.decodeCountForTest();
        EXPECT_GT(first_decodes, 0u);
        request_window(*broker, 500'000, 1'500'000, 400);
        const auto after_pan = worker.decodeCountForTest();
        request_window(*broker, 0, 1'000'000, 400);
        EXPECT_EQ(worker.decodeCountForTest(), after_pan);
        EXPECT_LE(after_pan, first_decodes * 2);
    }
    // Reopening (a new session) finds every tile on disk.
    auto broker = fixture.Broker();
    TimelineThumbnailSource worker(broker);
    broker->RegisterSource(identity, info);
    request_window(*broker, 0, 1'000'000, 400);
    request_window(*broker, 500'000, 1'500'000, 400);
    EXPECT_EQ(worker.decodeCountForTest(), 0u);
    EXPECT_GT(broker->Stats().disk_hits, 0u);
}

TEST(ThumbnailBroker, RealFailedSourcesStayIsolatedAcrossWorkerTeardownAndWarmReopen) {
    const auto directory = FixtureDirectory();
    if (!directory)
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    ThumbnailFixture fixture;
    ASSERT_TRUE(fixture.temp.isValid());
    const auto path = std::filesystem::path(fixture.temp.filePath(QStringLiteral("source.mkv")).toStdWString());
    ASSERT_TRUE(std::filesystem::copy_file(*directory / L"red.mkv", path));
    const auto metadata = engine::ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    const auto identity = engine::ToHex(metadata.content_sha256);
    const auto missing_identity = engine::ToHex(Identity(71));
    const auto invalid_path =
        std::filesystem::path(fixture.temp.filePath(QStringLiteral("invalid.mkv")).toStdWString());
    const std::array<std::uint8_t, 7> invalid_bytes{'n', 'o', 't', 'm', 'e', 'd', 'i'};
    {
        std::ofstream invalid(invalid_path, std::ios::binary);
        invalid.write(reinterpret_cast<const char*>(invalid_bytes.data()), invalid_bytes.size());
        ASSERT_TRUE(invalid.good());
    }
    const auto invalid_hash = engine::Sha256(invalid_bytes);
    const auto invalid_identity = engine::ToHex(invalid_hash);
    for (int cycle = 0; cycle < 4; ++cycle) {
        SCOPED_TRACE(cycle);
        {
            auto broker = fixture.Broker();
            broker->RegisterSource(identity, ThumbnailSourceInfo{path, metadata.content_sha256, metadata.width,
                                                                 metadata.height, metadata.fps_num, metadata.fps_den});
            auto unavailable = ThumbnailSourceInfo{
                std::filesystem::path(fixture.temp.filePath(QStringLiteral("missing.mkv")).toStdWString()),
                Identity(71),
                metadata.width,
                metadata.height,
                metadata.fps_num,
                metadata.fps_den};
            broker->RegisterSource(missing_identity, unavailable);
            unavailable.path = invalid_path;
            unavailable.content_sha256 = invalid_hash;
            broker->RegisterSource(invalid_identity, unavailable);
            TimelineThumbnailSource worker(broker);
            std::vector<std::shared_ptr<Result>> results;
            for (const auto& [source, time_us] : {std::pair{missing_identity, 0ll}, std::pair{identity, 0ll},
                                                  std::pair{invalid_identity, 0ll}, std::pair{identity, 1'000'000ll}}) {
                results.push_back(std::make_shared<Result>());
                broker->Request(source, time_us, 48, Capture(results.back()));
            }
            ASSERT_TRUE(WaitFor([&]() { return std::all_of(results.begin(), results.end(), Done); }));
            for (std::size_t i = 0; i < results.size(); ++i) {
                std::lock_guard lock(results[i]->mutex);
                ASSERT_TRUE(results[i]->image.has_value());
                EXPECT_EQ(results[i]->completions, 1);
                if (i % 2 == 0)
                    EXPECT_TRUE(results[i]->image->isNull());
                else {
                    ASSERT_FALSE(results[i]->image->isNull());
                    const auto& image = *results[i]->image;
                    const auto pixel = image.pixelColor(image.width() / 2, image.height() / 2);
                    EXPECT_GT(pixel.red(), pixel.blue());
                }
            }
            EXPECT_EQ(worker.decodeCountForTest(), cycle == 0 ? 2u : 0u);
            EXPECT_EQ(broker->Disk()->store().EntryCount(), 2u);
            EXPECT_FALSE(broker->Disk()->store().Contains(ThumbnailCacheKey(Identity(71), 0, 48)));
            EXPECT_FALSE(broker->Disk()->store().Contains(ThumbnailCacheKey(invalid_hash, 0, 48)));
            EXPECT_EQ(FilesWithSuffix(fixture.root, ".tmp"), 0u);
        }
        const auto handle =
            CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE) << "Source handle retained after worker teardown: " << GetLastError();
        EXPECT_TRUE(CloseHandle(handle));
    }
    const auto after = engine::ProbeEditMedia(path);
    ASSERT_TRUE(after.error.empty()) << after.error;
    EXPECT_EQ(after.content_sha256, metadata.content_sha256);
}

TEST(ThumbnailBroker, ForegroundBusyDefersDecodesNotCacheHits) {
    const auto directory = FixtureDirectory();
    if (!directory)
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    ThumbnailFixture fixture;
    const auto path = *directory / L"blue.mkv";
    const auto metadata = engine::ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    const auto identity = engine::ToHex(metadata.content_sha256);
    auto broker = fixture.Broker();
    TimelineThumbnailSource worker(broker);
    broker->RegisterSource(identity, ThumbnailSourceInfo{path, metadata.content_sha256, metadata.width, metadata.height,
                                                         metadata.fps_num, metadata.fps_den});
    auto warm = std::make_shared<Result>();
    broker->Request(identity, 0, 48, Capture(warm));
    ASSERT_TRUE(WaitFor([&]() { return Done(warm); }));

    worker.setForegroundBusy(true);
    auto cached = std::make_shared<Result>();
    broker->Request(identity, 0, 48, Capture(cached));
    EXPECT_TRUE(Done(cached));
    auto deferred = std::make_shared<Result>();
    broker->Request(identity, 1'000'000, 48, Capture(deferred));
    QThread::msleep(200);
    EXPECT_FALSE(Done(deferred));
    EXPECT_EQ(worker.decodeCountForTest(), 1u);
    worker.setForegroundBusy(false);
    ASSERT_TRUE(WaitFor([&]() { return Done(deferred); }));
    EXPECT_EQ(worker.decodeCountForTest(), 2u);
}

TEST(ThumbnailBroker, StripRunsUseTheDiskTierAcrossReopen) {
    const auto directory = FixtureDirectory();
    if (!directory)
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    ThumbnailFixture fixture;
    const auto path = *directory / L"red.mkv";
    const auto metadata = engine::ProbeEditMedia(path);
    ASSERT_TRUE(metadata.error.empty()) << metadata.error;
    const auto identity = engine::ToHex(metadata.content_sha256);
    const auto run = [&]() {
        auto broker = fixture.Broker();
        broker->RegisterSource(identity, ThumbnailSourceInfo{path, metadata.content_sha256, metadata.width,
                                                             metadata.height, metadata.fps_num, metadata.fps_den});
        TimelineThumbnailSource source(broker);
        QSignalSpy opened(&source, &TimelineThumbnailSource::clipOpened);
        QSignalSpy finished(&source, &TimelineThumbnailSource::runFinished);
        source.openClip(QString::fromStdWString(path.wstring()), {});
        EXPECT_TRUE(WaitFor([&]() { return opened.count() == 1; }));
        source.requestTiles({0, 1000}, 48);
        EXPECT_TRUE(WaitFor([&]() { return finished.count() == 1; }));
        EXPECT_EQ(finished.at(0).at(1).toInt(), 2);
        return source.decodeCountForTest();
    };
    EXPECT_EQ(run(), 2u);
    EXPECT_EQ(run(), 0u);
}

} // namespace exosnap::quick

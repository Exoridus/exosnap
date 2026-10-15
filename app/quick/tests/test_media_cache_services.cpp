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
#include <QSGGeometryNode>
#include <QSGTransformNode>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    std::mutex mutex;
    std::condition_variable cv;
    bool gated = false;
    int permits = 0;

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
    bool Decode(std::int64_t first, std::uint32_t frames, std::vector<float>& out, std::string&) override {
        {
            std::unique_lock lock(control_->mutex);
            control_->cv.wait(lock, [this]() { return !control_->gated || control_->permits > 0; });
            if (control_->gated)
                --control_->permits;
        }
        control_->calls.fetch_add(1);
        out.resize(static_cast<std::size_t>(frames) * 2);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float amplitude = StepAmplitude(first + i);
            out[i * 2] = amplitude;
            out[i * 2 + 1] = -amplitude;
        }
        return true;
    }

  private:
    std::shared_ptr<DecoderControl> control_;
};

WaveformCacheService::DecoderFactory Factory(const std::shared_ptr<DecoderControl>& control) {
    return [control](const std::filesystem::path&, std::uint32_t, std::string&, const std::atomic<bool>*) {
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
    ASSERT_EQ(buckets, engine::WaveformBucketCount(144000, 0));
    for (std::uint32_t bucket = 0; bucket < buckets; ++bucket) {
        const float amplitude = StepAmplitude(static_cast<std::int64_t>(bucket) * 128);
        ASSERT_NEAR(engine::WaveformPeakToFloat(peaks[bucket * 2].max), amplitude, 1e-4f);
        ASSERT_NEAR(engine::WaveformPeakToFloat(peaks[bucket * 2 + 1].min), -amplitude, 1e-4f);
    }
    EXPECT_EQ(warm->levelCount(key), engine::WaveformLevelCount(144000));
}

TEST(WaveformCacheService, AnalysisBudgetRejectsBeforeOpeningDecoder) {
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
        [&](const std::filesystem::path&, std::uint32_t, std::string&,
            const std::atomic<bool>* cancelled) -> std::unique_ptr<engine::WaveformAudioDecoder> {
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

TEST(WaveformCacheService, PartialResultsAreReadableAndCancellationPublishesNothing) {
    WaveformFixture fixture;
    fixture.control->gated = true;
    auto service = fixture.Service();
    const QString key = service->request(SourceKey(2), fixture.source_path, kThreeSecondsUs);
    fixture.control->Release(2);
    ASSERT_TRUE(WaitFor([&]() { return service->completedSamples(key) >= 9600; }));
    EXPECT_EQ(service->state(key), WaveformCacheService::State::Analyzing);
    std::vector<WaveformPeak> peaks;
    const auto partial = service->readBlock(key, 0, 0, peaks);
    EXPECT_EQ(partial, 9600u / 128u);
    EXPECT_NEAR(engine::WaveformPeakToFloat(peaks[0].max), StepAmplitude(0), 1e-4f);

    service->cancel(key);
    fixture.control->Open();
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Unknown; }));
    EXPECT_EQ(service->storeForTest().EntryCount(), 0u);
    EXPECT_EQ(FilesWithSuffix(fixture.root, ".wave"), 0u);
    EXPECT_EQ(FilesWithSuffix(fixture.root, ".tmp"), 0u);
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

TEST(WaveformCacheService, RealFixturesThroughTheServiceAndThroughput) {
    const auto directory = FixtureDirectory();
    if (!directory)
        GTEST_SKIP() << "Set EXOSNAP_EDIT_RENDER_FIXTURES to generated red/blue media.";
    EnsureApplication();
    QTemporaryDir temp;
    WaveformCacheService::Config config;
    config.root = std::filesystem::path(temp.filePath(QStringLiteral("waveforms")).toStdWString());
    WaveformCacheService service(config, {});
    for (const auto& [name, level] : {std::pair{L"red.mkv", 0.25f}, std::pair{L"blue.mkv", -0.25f}}) {
        const auto path = *directory / name;
        const auto metadata = engine::ProbeEditMedia(path);
        ASSERT_TRUE(metadata.error.empty()) << metadata.error;
        engine::WaveformSourceKey source;
        source.content_sha256 = metadata.content_sha256;
        for (const auto& stream : metadata.streams)
            if (stream.kind == engine::TimelineStreamKind::Audio)
                source.container_index = stream.container_index;
        const auto started = std::chrono::steady_clock::now();
        const QString key = service.request(source, QString::fromStdWString(path.wstring()), metadata.duration_us);
        ASSERT_TRUE(WaitFor([&]() { return service.state(key) == WaveformCacheService::State::Ready; }));
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "[waveform-service] " << path.filename().string() << ": "
                  << static_cast<double>(service.totalSamples(key)) / 48000.0 << " s of audio ready in " << seconds
                  << " s\n";
        std::vector<WaveformPeak> peaks;
        const auto buckets = ReadBlockBlocking(service, key, 0, 0, peaks);
        ASSERT_GT(buckets, 8u);
        for (std::uint32_t bucket = 2; bucket + 2 < buckets; ++bucket)
            for (int channel = 0; channel < 2; ++channel) {
                const auto& peak = peaks[bucket * 2 + channel];
                ASSERT_EQ(peak.min, peak.max);
                ASSERT_NEAR(engine::WaveformPeakToFloat(peak.max), level, 1e-3f);
            }
    }
}

// ---- Waveform item ----

class TestableWaveformItem : public ExoWaveformItem {
  public:
    using ExoWaveformItem::updatePaintNode;
};

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

TEST(WaveformCacheService, ReadyRequestRechecksMissingSourceWithoutDecoding) {
    WaveformFixture fixture;
    auto service = fixture.Service();
    const auto key = service->request(SourceKey(1), fixture.source_path, kThreeSecondsUs);
    ASSERT_TRUE(WaitFor([&]() { return service->state(key) == WaveformCacheService::State::Ready; }));
    ASSERT_TRUE(std::filesystem::remove(std::filesystem::path(fixture.source_path.toStdWString())));
    QSignalSpy availability(service.get(), &WaveformCacheService::sourceAvailabilityChanged);
    const auto calls = service->decodeCallsForTest();
    EXPECT_EQ(service->request(SourceKey(1), fixture.source_path, kThreeSecondsUs), key);
    EXPECT_FALSE(service->sourceAvailable(key));
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

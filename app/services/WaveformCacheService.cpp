#include "WaveformCacheService.h"

#include <exosnap/engine/edit_timeline.h>

#include <QDir>
#include <QMetaObject>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace exosnap {
namespace {

using engine::WaveformPeak;

constexpr auto kProgressInterval = std::chrono::milliseconds(100);
// Layouts are a few hundred bytes each; the bound only guards a session that
// touches an unusual number of streams.
constexpr std::size_t kMaxLayouts = 4096;
// Blocks read from a running analysis, remembered so their peaks can seed the
// disk tier's memory cache when the analysis completes and no row flickers.
constexpr std::size_t kMaxTouchedBlocks = 256;

std::filesystem::path DefaultRoot() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return std::filesystem::path(QDir(base).filePath(QStringLiteral("waveforms")).toStdWString());
}

engine::MediaCacheStoreConfig StoreConfig(const WaveformCacheService::Config& config) {
    engine::MediaCacheStoreConfig store;
    store.root = config.root.empty() ? DefaultRoot() : config.root;
    store.budget_bytes = config.disk_budget_bytes;
    store.kind = {'W', 'A', 'V', 'E'};
    store.schema_version = engine::kWaveformSchemaVersion;
    store.extension = ".wave";
    return store;
}

// Counts every decode the service performs, so a warm cache can be proven to
// perform none.
class CountingDecoder final : public engine::WaveformAudioDecoder {
  public:
    CountingDecoder(std::unique_ptr<engine::WaveformAudioDecoder> inner, std::atomic<std::uint64_t>& calls)
        : inner_(std::move(inner)), calls_(calls) {
    }
    bool Decode(std::int64_t first, std::uint32_t frames, std::vector<float>& out, std::string& error) override {
        calls_.fetch_add(1, std::memory_order_relaxed);
        return inner_->Decode(first, frames, out, error);
    }

  private:
    std::unique_ptr<engine::WaveformAudioDecoder> inner_;
    std::atomic<std::uint64_t>& calls_;
};

std::uint32_t CopyBuilderBlock(const engine::WaveformPyramidBuilder& builder, std::uint32_t level, std::uint64_t block,
                               std::vector<WaveformPeak>& out) {
    const auto ready = builder.ReadyBuckets(level);
    const std::uint64_t first = block * kWaveformBlockBuckets;
    if (first >= ready)
        return 0;
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(kWaveformBlockBuckets, ready - first));
    const auto peaks = builder.Level(level);
    out.assign(peaks.begin() + static_cast<std::ptrdiff_t>(first * engine::kWaveformChannels),
               peaks.begin() + static_cast<std::ptrdiff_t>((first + count) * engine::kWaveformChannels));
    return count;
}

} // namespace

WaveformCacheService::WaveformCacheService(QObject* parent) : WaveformCacheService(Config{}, {}, parent) {
}

WaveformCacheService::WaveformCacheService(Config config, DecoderFactory decoder_factory, QObject* parent)
    : QObject(parent), config_(std::move(config)), decoder_factory_(std::move(decoder_factory)) {
    if (!decoder_factory_) {
        decoder_factory_ = [](const std::filesystem::path& path, std::uint32_t index, std::string& error,
                              const std::atomic<bool>* cancelled) {
            return engine::OpenEditWaveformDecoder(path, index, error, cancelled);
        };
    }
    store_ = std::make_unique<engine::MediaCacheStore>(StoreConfig(config_));
    worker_ = std::thread([this]() { workerLoop(); });
}

WaveformCacheService::~WaveformCacheService() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        cancel_running_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

QString WaveformCacheService::request(const engine::WaveformSourceKey& source, const QString& source_path,
                                      qint64 source_end_us) {
    const std::string key = engine::WaveformCacheKey(source);
    const QString qkey = QString::fromStdString(key);
    if (source_end_us <= 0)
        return qkey;
    std::error_code source_error;
    const bool source_exists =
        std::filesystem::is_regular_file(std::filesystem::path(source_path.toStdWString()), source_error);
    bool already_ready = false;
    bool availability_changed = false;
    {
        std::lock_guard lock(mutex_);
        auto& status = status_[key];
        if (status.state == State::Queued || status.state == State::Analyzing)
            return qkey;
        already_ready = status.state == State::Ready;
        if (already_ready) {
            availability_changed = status.source_available != source_exists;
            status.source_available = source_exists;
        } else {
            status.state = State::Queued;
            status.total_samples = static_cast<std::uint64_t>(engine::AudioSampleIndex(source_end_us));
            status.completed_samples = 0;
            queue_.push_back(Job{key, std::filesystem::path(source_path.toStdWString()), source.container_index,
                                 status.total_samples});
        }
    }
    if (availability_changed)
        QMetaObject::invokeMethod(
            this, [this, qkey, source_exists]() { emit sourceAvailabilityChanged(qkey, source_exists); },
            Qt::QueuedConnection);
    if (already_ready)
        return qkey;
    cv_.notify_all();
    return qkey;
}

void WaveformCacheService::cancel(const QString& qkey) {
    const std::string key = qkey.toStdString();
    {
        std::lock_guard lock(mutex_);
        const auto removed = std::erase_if(queue_, [&key](const Job& job) { return job.key == key; });
        if (running_ && *running_ == key)
            cancel_running_ = true;
        else if (removed > 0)
            status_.erase(key);
    }
    cv_.notify_all();
}

void WaveformCacheService::setForegroundBusy(bool busy) {
    {
        std::lock_guard lock(mutex_);
        busy_ = busy;
    }
    cv_.notify_all();
}

bool WaveformCacheService::foregroundBusy() const {
    std::lock_guard lock(mutex_);
    return busy_;
}

qint64 WaveformCacheService::clearCache() {
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        queue_.clear();
        if (running_)
            cancel_running_ = true;
        status_.clear();
        layouts_.clear();
        pending_blocks_.clear();
        blocks_.clear();
        block_lru_.clear();
        block_bytes_ = 0;
    }
    cv_.notify_all();
    // Serialized with publication: an analysis finishing concurrently either
    // published before this clear or sees the new generation and discards.
    std::lock_guard publish(publish_mutex_);
    return static_cast<qint64>(store_->Clear());
}

WaveformCacheService::State WaveformCacheService::state(const QString& key) const {
    std::lock_guard lock(mutex_);
    const auto found = status_.find(key.toStdString());
    return found == status_.end() ? State::Unknown : found->second.state;
}

bool WaveformCacheService::sourceAvailable(const QString& key) const {
    std::lock_guard lock(mutex_);
    const auto found = status_.find(key.toStdString());
    return found == status_.end() || found->second.source_available;
}

qint64 WaveformCacheService::completedSamples(const QString& key) const {
    std::lock_guard lock(mutex_);
    const auto found = status_.find(key.toStdString());
    return found == status_.end() ? 0 : static_cast<qint64>(found->second.completed_samples);
}

qint64 WaveformCacheService::totalSamples(const QString& key) const {
    std::lock_guard lock(mutex_);
    const auto found = status_.find(key.toStdString());
    return found == status_.end() ? 0 : static_cast<qint64>(found->second.total_samples);
}

std::uint32_t WaveformCacheService::levelCount(const QString& qkey) const {
    const std::string key = qkey.toStdString();
    std::lock_guard lock(mutex_);
    if (in_progress_ && in_progress_key_ == key)
        return in_progress_->LevelCount();
    const auto found = layouts_.find(key);
    return found == layouts_.end() ? 0 : static_cast<std::uint32_t>(found->second.levels.size());
}

std::uint32_t WaveformCacheService::readBlock(const QString& qkey, std::uint32_t level, std::uint64_t block,
                                              std::vector<WaveformPeak>& out) {
    if (qkey.isEmpty())
        return 0;
    BlockId id{qkey.toStdString(), level, block};
    bool schedule = false;
    std::uint32_t count = 0;
    {
        std::lock_guard lock(mutex_);
        if (in_progress_ && in_progress_key_ == id.key) {
            if (touched_blocks_.size() < kMaxTouchedBlocks)
                touched_blocks_.insert(id);
            return CopyBuilderBlock(*in_progress_, level, block, out);
        }
        const auto found = blocks_.find(id);
        if (found != blocks_.end()) {
            block_lru_.splice(block_lru_.end(), block_lru_, found->second.lru);
            out = found->second.peaks;
            return static_cast<std::uint32_t>(out.size() / engine::kWaveformChannels);
        }
        const auto status = status_.find(id.key);
        const bool analysing = status != status_.end() &&
                               (status->second.state == State::Queued || status->second.state == State::Analyzing);
        const bool failed = status != status_.end() && status->second.state == State::Failed;
        if (!analysing && !failed && !pending_blocks_.contains(id)) {
            pending_blocks_.insert(id);
            schedule = true;
        }
    }
    if (schedule)
        cv_.notify_all();
    return count;
}

std::uint64_t WaveformCacheService::decoderOpensForTest() const {
    std::lock_guard lock(mutex_);
    return decoder_opens_;
}

std::uint64_t WaveformCacheService::decodeCallsForTest() const {
    return decode_calls_.load(std::memory_order_relaxed);
}

std::size_t WaveformCacheService::memoryBytesForTest() const {
    std::lock_guard lock(mutex_);
    return block_bytes_;
}

engine::MediaCacheStore& WaveformCacheService::storeForTest() {
    return *store_;
}

void WaveformCacheService::workerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;) {
        std::optional<Job> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || !pending_blocks_.empty() || (!queue_.empty() && !busy_); });
            if (stop_)
                return;
            if (pending_blocks_.empty()) {
                job = std::move(queue_.front());
                queue_.pop_front();
                running_ = job->key;
                cancel_running_ = false;
            }
        }
        if (!job) {
            loadPendingBlocks();
            continue;
        }
        runJob(*job);
        std::lock_guard lock(mutex_);
        running_.reset();
    }
}

std::optional<engine::WaveformFileLayout> WaveformCacheService::layoutFor(const std::string& key) {
    {
        std::lock_guard lock(mutex_);
        const auto found = layouts_.find(key);
        if (found != layouts_.end())
            return found->second;
    }
    const auto size = store_->OpenVerified(key);
    if (!size)
        return std::nullopt;
    auto layout = engine::ParseWaveformLayout(*size, [this, &key](std::uint64_t offset, std::span<std::uint8_t> out) {
        return store_->ReadPayloadRange(key, offset, out);
    });
    if (!layout) {
        store_->Remove(key);
        return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    if (layouts_.size() >= kMaxLayouts)
        layouts_.erase(layouts_.begin());
    layouts_[key] = *layout;
    return layout;
}

void WaveformCacheService::loadPendingBlocks() {
    std::set<std::string> loaded_keys;
    for (;;) {
        BlockId id;
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex_);
            if (pending_blocks_.empty() || stop_)
                break;
            id = *pending_blocks_.begin();
            generation = generation_;
        }
        std::vector<WaveformPeak> peaks;
        const auto layout = layoutFor(id.key);
        if (!layout) {
            // Evicted from disk since it was analysed: forget the verdict so
            // the next request() analyses it again instead of drawing nothing.
            std::lock_guard lock(mutex_);
            const auto status = status_.find(id.key);
            if (status != status_.end() && status->second.state == State::Ready)
                status_.erase(status);
        }
        if (layout && id.level < layout->levels.size()) {
            const auto& info = layout->levels[id.level];
            const std::uint64_t first = id.block * kWaveformBlockBuckets;
            if (first < info.bucket_count) {
                const auto count = std::min<std::uint64_t>(kWaveformBlockBuckets, info.bucket_count - first);
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count * engine::kWaveformBucketBytes));
                if (store_->ReadPayloadRange(id.key, info.offset + first * engine::kWaveformBucketBytes, bytes))
                    engine::DecodeWaveformBuckets(bytes, peaks);
            }
        }
        {
            std::lock_guard lock(mutex_);
            pending_blocks_.erase(id);
            if (generation != generation_ || peaks.empty())
                continue;
        }
        insertBlock(id, std::move(peaks));
        loaded_keys.insert(id.key);
    }
    for (const auto& key : loaded_keys) {
        const QString qkey = QString::fromStdString(key);
        QMetaObject::invokeMethod(this, [this, qkey]() { emit blocksLoaded(qkey); }, Qt::QueuedConnection);
    }
}

void WaveformCacheService::insertBlock(const BlockId& id, std::vector<WaveformPeak> peaks) {
    std::lock_guard lock(mutex_);
    if (blocks_.contains(id))
        return;
    const std::size_t bytes = peaks.size() * sizeof(WaveformPeak);
    while (!block_lru_.empty() && block_bytes_ + bytes > config_.memory_budget_bytes) {
        const auto oldest = blocks_.find(block_lru_.front());
        block_bytes_ -= oldest->second.peaks.size() * sizeof(WaveformPeak);
        blocks_.erase(oldest);
        block_lru_.pop_front();
    }
    if (bytes > config_.memory_budget_bytes)
        return;
    block_lru_.push_back(id);
    blocks_.emplace(id, CachedBlock{std::move(peaks), std::prev(block_lru_.end())});
    block_bytes_ += bytes;
}

bool WaveformCacheService::setStatus(const std::string& key, const Status& status,
                                     std::optional<std::uint64_t> generation) {
    bool availability_changed = false;
    {
        std::lock_guard lock(mutex_);
        if (generation && *generation != generation_)
            return false;
        auto& current = status_[key];
        availability_changed = current.source_available != status.source_available;
        current = status;
    }
    const QString qkey = QString::fromStdString(key);
    const bool available = status.source_available;
    if (availability_changed) {
        QMetaObject::invokeMethod(
            this, [this, qkey, available]() { emit sourceAvailabilityChanged(qkey, available); }, Qt::QueuedConnection);
    }
    return true;
}

void WaveformCacheService::postProgress(const std::string& key, std::uint64_t completed, std::uint64_t total) {
    const QString qkey = QString::fromStdString(key);
    QMetaObject::invokeMethod(
        this,
        [this, qkey, completed, total]() {
            emit progress(qkey, static_cast<qint64>(completed), static_cast<qint64>(total));
        },
        Qt::QueuedConnection);
}

void WaveformCacheService::runJob(const Job& job) {
    const QString qkey = QString::fromStdString(job.key);
    std::error_code fs_error;
    const bool source_exists = std::filesystem::is_regular_file(job.path, fs_error);
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        generation = generation_;
    }

    if (const auto layout = layoutFor(job.key)) {
        if (!setStatus(job.key, Status{State::Ready, source_exists, layout->sample_count, layout->sample_count},
                       generation))
            return;
        QMetaObject::invokeMethod(this, [this, qkey]() { emit ready(qkey); }, Qt::QueuedConnection);
        return;
    }
    const auto fail = [&](bool available, const QString& reason) {
        if (!setStatus(job.key, Status{State::Failed, available, job.sample_count, 0}, generation))
            return;
        QMetaObject::invokeMethod(this, [this, qkey, reason]() { emit failed(qkey, reason); }, Qt::QueuedConnection);
    };
    if (!source_exists) {
        fail(false, QStringLiteral("The audio source is unavailable."));
        return;
    }

    std::string error;
    std::uint64_t pyramid_bytes = 0;
    for (std::uint32_t level = 0; level < engine::WaveformLevelCount(job.sample_count); ++level) {
        const auto buckets = engine::WaveformBucketCount(job.sample_count, level);
        if (buckets > config_.analysis_budget_bytes / (engine::kWaveformBucketBytes * 3)) {
            fail(true, QStringLiteral("The waveform exceeds the background analysis memory budget."));
            return;
        }
        pyramid_bytes += buckets * engine::kWaveformBucketBytes;
    }
    // Growing vectors can retain twice their final size while serialization
    // holds another complete copy. Refuse before decoding rather than exceed
    // the bounded background working set.
    if (pyramid_bytes > config_.analysis_budget_bytes / 3) {
        fail(true, QStringLiteral("The waveform exceeds the background analysis memory budget."));
        return;
    }
    std::unique_ptr<engine::WaveformAudioDecoder> opened =
        decoder_factory_(job.path, job.container_index, error, &cancel_running_);
    {
        std::lock_guard lock(mutex_);
        ++decoder_opens_;
    }
    if (!opened) {
        fail(true, QString::fromStdString(error));
        return;
    }
    CountingDecoder decoder(std::move(opened), decode_calls_);

    auto builder = std::make_shared<engine::WaveformPyramidBuilder>(job.sample_count);
    {
        std::lock_guard lock(mutex_);
        in_progress_ = builder;
        in_progress_key_ = job.key;
        touched_blocks_.clear();
    }
    if (!setStatus(job.key, Status{State::Analyzing, true, job.sample_count, 0}, generation))
        return;

    auto last_progress = std::chrono::steady_clock::time_point{};
    engine::WaveformAnalysisControl control;
    control.chunk_frames = config_.chunk_frames;
    control.consume = [&](const float* samples, std::size_t frames) {
        std::uint64_t completed = 0;
        {
            std::lock_guard lock(mutex_);
            if (generation != generation_ || cancel_running_)
                return;
            builder->Append(samples, frames);
            completed = builder->CompletedSamples();
            status_[job.key].completed_samples = completed;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_progress >= kProgressInterval) {
            last_progress = now;
            postProgress(job.key, completed, job.sample_count);
        }
    };
    control.proceed = [&]() {
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || cancel_running_ || !busy_ || !pending_blocks_.empty(); });
                if (stop_ || cancel_running_)
                    return false;
                if (pending_blocks_.empty())
                    return true;
            }
            // Display reads stay served while analysis waits for admission.
            loadPendingBlocks();
        }
    };

    const auto outcome = engine::AnalyzeWaveform(decoder, job.sample_count, control, error);
    if (outcome != engine::WaveformAnalysisOutcome::Completed) {
        {
            std::lock_guard lock(mutex_);
            in_progress_.reset();
            in_progress_key_.clear();
            touched_blocks_.clear();
            if (outcome == engine::WaveformAnalysisOutcome::Cancelled) {
                status_.erase(job.key);
                return;
            }
        }
        fail(true, QString::fromStdString(error));
        return;
    }

    {
        std::lock_guard lock(mutex_);
        builder->Finish();
    }
    const auto payload = engine::SerializeWaveformPyramid(*builder);
    bool published = false;
    {
        std::lock_guard publish(publish_mutex_);
        bool current = false;
        {
            std::lock_guard lock(mutex_);
            current = generation == generation_ && !cancel_running_;
        }
        if (current)
            published = store_->Publish(job.key, payload, error);
    }
    auto layout =
        engine::ParseWaveformLayout(payload.size(), [&payload](std::uint64_t offset, std::span<std::uint8_t> out) {
            if (offset + out.size() > payload.size())
                return false;
            std::memcpy(out.data(), payload.data() + offset, out.size());
            return true;
        });

    std::vector<std::pair<BlockId, std::vector<WaveformPeak>>> seed;
    {
        std::lock_guard lock(mutex_);
        for (const auto& id : touched_blocks_) {
            std::vector<WaveformPeak> peaks;
            if (CopyBuilderBlock(*builder, id.level, id.block, peaks) > 0)
                seed.emplace_back(id, std::move(peaks));
        }
        if (published && layout)
            layouts_[job.key] = *layout;
        in_progress_.reset();
        in_progress_key_.clear();
        touched_blocks_.clear();
        if (generation != generation_) {
            status_.erase(job.key);
            return;
        }
    }
    if (!published) {
        fail(true, QString::fromStdString(error.empty() ? "The waveform could not be persisted." : error));
        return;
    }
    for (auto& [id, peaks] : seed)
        insertBlock(id, std::move(peaks));
    if (!setStatus(job.key, Status{State::Ready, true, builder->SampleCount(), builder->SampleCount()}, generation))
        return;
    postProgress(job.key, builder->SampleCount(), builder->SampleCount());
    QMetaObject::invokeMethod(this, [this, qkey]() { emit ready(qkey); }, Qt::QueuedConnection);
}

} // namespace exosnap

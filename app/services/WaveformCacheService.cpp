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

    std::uint64_t Seeks() const override {
        return inner_->Seeks();
    }
    std::uint64_t DecodedFrames() const override {
        return inner_->DecodedFrames();
    }

  private:
    std::unique_ptr<engine::WaveformAudioDecoder> inner_;
    std::atomic<std::uint64_t>& calls_;
};

} // namespace

WaveformCacheService::WaveformCacheService(QObject* parent) : WaveformCacheService(Config{}, {}, parent) {
}

WaveformCacheService::WaveformCacheService(Config config, DecoderFactory decoder_factory, QObject* parent)
    : QObject(parent), config_(std::move(config)), decoder_factory_(std::move(decoder_factory)) {
    if (!decoder_factory_) {
        decoder_factory_ = [](const std::filesystem::path& path, std::uint32_t index, std::string& error,
                              const std::atomic<bool>* cancelled, const std::array<std::uint8_t, 32>& expected_sha256) {
            return engine::OpenEditWaveformDecoder(path, index, error, cancelled, &expected_sha256);
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
    const Job requested{key, std::filesystem::path(source_path.toStdWString()), source.container_index,
                        static_cast<std::uint64_t>(engine::AudioSampleIndex(source_end_us)), source.content_sha256};
    std::error_code source_error;
    const bool source_exists =
        std::filesystem::is_regular_file(std::filesystem::path(source_path.toStdWString()), source_error);
    bool already_ready = false;
    bool availability_changed = false;
    {
        std::lock_guard lock(mutex_);
        if (!requested_jobs_.contains(key) && requested_jobs_.size() >= kMaxLayouts)
            requested_jobs_.erase(requested_jobs_.begin());
        requested_jobs_[key] = requested;
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
            queue_.push_back(requested);
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
        requested_jobs_.erase(key);
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
        requested_jobs_.clear();
        if (running_)
            cancel_running_ = true;
        status_.clear();
        in_progress_.reset();
        in_progress_key_.clear();
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
        return static_cast<std::uint32_t>(in_progress_->Layout().levels.size());
    const auto status = status_.find(key);
    if (status != status_.end() && status->second.state == State::Analyzing)
        return engine::WaveformLevelCount(status->second.total_samples);
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
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        const auto found = blocks_.find(id);
        if (found != blocks_.end()) {
            block_lru_.splice(block_lru_.end(), block_lru_, found->second.lru);
            out = found->second.peaks;
            return static_cast<std::uint32_t>(out.size() / engine::kWaveformChannels);
        }
        if (in_progress_ && in_progress_key_ == id.key) {
            generation = generation_;
            count = in_progress_->ReadBlock(level, block * kWaveformBlockBuckets, kWaveformBlockBuckets, out);
        }
        const auto status = status_.find(id.key);
        const bool analysing = status != status_.end() &&
                               (status->second.state == State::Queued || status->second.state == State::Analyzing);
        const bool failed = status != status_.end() && status->second.state == State::Failed;
        if (count == 0 && (!analysing || (in_progress_ && in_progress_key_ == id.key)) && !failed &&
            !pending_blocks_.contains(id)) {
            pending_blocks_.insert(id);
            schedule = true;
        }
    }
    if (count == kWaveformBlockBuckets)
        insertBlock(id, out, generation);
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

std::size_t WaveformCacheService::analysisMemoryBytesForTest() const {
    std::lock_guard lock(mutex_);
    return analysis_peak_bytes_;
}
std::uint64_t WaveformCacheService::decoderSeeksForTest() const {
    std::lock_guard lock(mutex_);
    return decoder_seeks_;
}
std::uint64_t WaveformCacheService::decodedFramesForTest() const {
    std::lock_guard lock(mutex_);
    return decoded_frames_;
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
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        generation = generation_;
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
    if (generation != generation_)
        return std::nullopt;
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
        bool from_staging = false;
        {
            std::lock_guard lock(mutex_);
            if (in_progress_ && in_progress_key_ == id.key && generation == generation_) {
                from_staging = true;
                in_progress_->ReadBlock(id.level, id.block * kWaveformBlockBuckets, kWaveformBlockBuckets, peaks);
            }
        }
        const auto layout = from_staging ? std::optional<engine::WaveformFileLayout>{} : layoutFor(id.key);
        bool published_missing = !layout && !from_staging;
        if (layout && id.level < layout->levels.size()) {
            const auto& info = layout->levels[id.level];
            const std::uint64_t first = id.block * kWaveformBlockBuckets;
            if (first < info.bucket_count) {
                const auto count = std::min<std::uint64_t>(kWaveformBlockBuckets, info.bucket_count - first);
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count * engine::kWaveformBucketBytes));
                if (store_->ReadPayloadRange(id.key, info.offset + first * engine::kWaveformBucketBytes, bytes))
                    engine::DecodeWaveformBuckets(bytes, peaks);
                else
                    published_missing = true;
            }
        }
        {
            std::lock_guard lock(mutex_);
            pending_blocks_.erase(id);
            if (generation != generation_)
                continue;
            if (published_missing) {
                layouts_.erase(id.key);
                const auto status = status_.find(id.key);
                if (status != status_.end() && status->second.state == State::Ready) {
                    const auto requested = requested_jobs_.find(id.key);
                    if (requested == requested_jobs_.end()) {
                        status_.erase(status);
                    } else {
                        status->second.state = State::Queued;
                        status->second.total_samples = requested->second.sample_count;
                        status->second.completed_samples = 0;
                        queue_.push_back(requested->second);
                    }
                }
            }
            if (peaks.empty())
                continue;
        }
        insertBlock(id, std::move(peaks), generation, !from_staging);
        loaded_keys.insert(id.key);
    }
    for (const auto& key : loaded_keys) {
        const QString qkey = QString::fromStdString(key);
        QMetaObject::invokeMethod(this, [this, qkey]() { emit blocksLoaded(qkey); }, Qt::QueuedConnection);
    }
}

void WaveformCacheService::insertBlock(const BlockId& id, std::vector<WaveformPeak> peaks, std::uint64_t generation,
                                       bool from_published_entry) {
    std::lock_guard lock(mutex_);
    // Staging tails can arrive after finalization. Their origin, rather than
    // the current status, keeps them out of the immutable block cache.
    if (!from_published_entry && peaks.size() != kWaveformBlockBuckets * engine::kWaveformChannels)
        return;
    if (generation != generation_ || blocks_.contains(id))
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

void WaveformCacheService::postProgress(const std::string& key, std::uint64_t completed, std::uint64_t total,
                                        std::uint64_t generation) {
    const QString qkey = QString::fromStdString(key);
    QMetaObject::invokeMethod(
        this,
        [this, qkey, completed, total, generation]() {
            {
                std::lock_guard lock(mutex_);
                if (generation != generation_)
                    return;
            }
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
        QMetaObject::invokeMethod(
            this,
            [this, qkey, generation]() {
                {
                    std::lock_guard lock(mutex_);
                    if (generation != generation_)
                        return;
                }
                emit ready(qkey);
            },
            Qt::QueuedConnection);
        return;
    }
    const auto fail = [&](bool available, const QString& reason) {
        if (!setStatus(job.key, Status{State::Failed, available, job.sample_count, 0}, generation))
            return;
        QMetaObject::invokeMethod(
            this,
            [this, qkey, reason, generation]() {
                {
                    std::lock_guard lock(mutex_);
                    if (generation != generation_)
                        return;
                }
                emit failed(qkey, reason);
            },
            Qt::QueuedConnection);
    };
    if (!source_exists) {
        fail(false, QStringLiteral("The audio source is unavailable."));
        return;
    }

    std::string error;
    std::shared_ptr<engine::WaveformStreamWriter> builder;
    {
        std::lock_guard publish(publish_mutex_);
        {
            std::lock_guard lock(mutex_);
            if (generation != generation_ || cancel_running_)
                return;
        }
        builder = engine::WaveformStreamWriter::Open(*store_, job.key, job.sample_count, error);
    }
    if (!builder) {
        fail(true, QString::fromStdString(error));
        return;
    }
    const auto working_bytes = builder->MemoryBytes() +
                               static_cast<std::size_t>(std::clamp<std::uint32_t>(
                                   config_.chunk_frames, 1, engine::kWaveformAnalysisChunkFrames)) *
                                   sizeof(float) * engine::kWaveformChannels +
                               64 * 1024;
    if (working_bytes > config_.analysis_budget_bytes) {
        fail(true, QStringLiteral("The background analysis working set exceeds its memory budget."));
        return;
    }
    {
        std::lock_guard lock(mutex_);
        analysis_peak_bytes_ = std::max(analysis_peak_bytes_, working_bytes);
    }
    std::unique_ptr<engine::WaveformAudioDecoder> opened =
        decoder_factory_(job.path, job.container_index, error, &cancel_running_, job.expected_sha256);
    {
        std::lock_guard lock(mutex_);
        ++decoder_opens_;
    }
    if (cancel_running_) {
        std::lock_guard lock(mutex_);
        if (generation == generation_)
            status_.erase(job.key);
        return;
    }
    if (!opened) {
        fail(true, QString::fromStdString(error));
        return;
    }
    CountingDecoder decoder(std::move(opened), decode_calls_);

    bool stream_ok = true;
    {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || cancel_running_)
            return;
        in_progress_ = builder;
        in_progress_key_ = job.key;
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
            stream_ok = builder->Append(samples, frames);
            completed = builder->CompletedSamples();
            status_[job.key].completed_samples = completed;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_progress >= kProgressInterval) {
            last_progress = now;
            postProgress(job.key, completed, job.sample_count, generation);
        }
    };
    control.proceed = [&]() {
        for (;;) {
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this]() { return stop_ || cancel_running_ || !busy_ || !pending_blocks_.empty(); });
                if (stop_ || cancel_running_ || !stream_ok)
                    return false;
                if (pending_blocks_.empty())
                    return true;
            }
            // Display reads stay served while analysis waits for admission.
            loadPendingBlocks();
        }
    };

    auto outcome = engine::AnalyzeWaveform(decoder, job.sample_count, control, error);
    {
        std::lock_guard lock(mutex_);
        decoder_seeks_ += decoder.Seeks();
        decoded_frames_ += decoder.DecodedFrames();
    }
    if (!stream_ok) {
        outcome = engine::WaveformAnalysisOutcome::Failed;
        error = "Cannot write waveform analysis blocks.";
    }
    if (outcome != engine::WaveformAnalysisOutcome::Completed) {
        {
            std::lock_guard lock(mutex_);
            in_progress_.reset();
            in_progress_key_.clear();
            if (outcome == engine::WaveformAnalysisOutcome::Cancelled) {
                if (generation == generation_)
                    status_.erase(job.key);
                return;
            }
        }
        fail(true, QString::fromStdString(error));
        return;
    }
    bool finished = false;
    {
        std::lock_guard lock(mutex_);
        finished = builder->Finish();
        // Detach before hashing: render reads must not hold the service lock
        // while waiting for the store lock needed by cancellable publication.
        in_progress_.reset();
        in_progress_key_.clear();
    }
    bool published = false;
    {
        std::lock_guard publish(publish_mutex_);
        bool current = false;
        {
            std::lock_guard lock(mutex_);
            current = generation == generation_ && !cancel_running_;
        }
        if (current && finished)
            published = builder->Publish(error, [this]() { return !cancel_running_.load(std::memory_order_relaxed); });
    }
    const auto layout = builder->Layout();
    {
        std::lock_guard lock(mutex_);
        if (published && generation == generation_)
            layouts_[job.key] = layout;
        in_progress_.reset();
        in_progress_key_.clear();
        if (generation != generation_)
            return;
        if (cancel_running_) {
            status_.erase(job.key);
            return;
        }
    }
    if (!published) {
        fail(true, QString::fromStdString(error.empty() ? "The waveform could not be persisted." : error));
        return;
    }
    if (!setStatus(job.key, Status{State::Ready, true, job.sample_count, job.sample_count}, generation))
        return;
    postProgress(job.key, job.sample_count, job.sample_count, generation);
    QMetaObject::invokeMethod(
        this,
        [this, qkey, generation]() {
            {
                std::lock_guard lock(mutex_);
                if (generation != generation_)
                    return;
            }
            emit ready(qkey);
        },
        Qt::QueuedConnection);
}

} // namespace exosnap

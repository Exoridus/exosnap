#include "WaveformCacheService.h"

#include <exosnap/engine/edit_timeline.h>

#include <QDir>
#include <QMetaObject>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
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
    std::span<const engine::AudioSampleSpan> Coverage() const override {
        return inner_->Coverage();
    }

  private:
    std::unique_ptr<engine::WaveformAudioDecoder> inner_;
    std::atomic<std::uint64_t>& calls_;
};

} // namespace

struct WaveformCacheService::OverviewCursor {
    std::shared_ptr<engine::WaveformStreamWriter> builder;
    std::unique_ptr<CountingDecoder> decoder;
    std::uint64_t generation = 0;
    std::uint64_t last_used = 0;
    std::uint64_t consumed_samples = 0;
    std::size_t working_bytes = 0;
    std::chrono::steady_clock::time_point last_progress;
};

WaveformCacheService::WaveformCacheService(QObject* parent) : WaveformCacheService(Config{}, {}, parent) {
}

WaveformCacheService::WaveformCacheService(Config config, DecoderFactory decoder_factory, QObject* parent)
    : QObject(parent), config_(std::move(config)), decoder_factory_(std::move(decoder_factory)) {
    custom_decoder_factory_ = bool(decoder_factory_);
    if (!decoder_factory_) {
        decoder_factory_ = [](const std::filesystem::path& path, std::uint32_t index, std::string& error,
                              const std::atomic<bool>* cancelled, const std::array<std::uint8_t, 32>& expected_sha256) {
            return engine::OpenEditWaveformDecoder(path, index, error, cancelled, &expected_sha256);
        };
    }
    store_ = std::make_unique<engine::MediaCacheStore>(StoreConfig(config_));
    worker_ = std::thread([this]() { workerLoop(); });
    pcm_worker_ = std::thread([this]() { pcmWorkerLoop(); });
}

WaveformCacheService::~WaveformCacheService() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        cancel_running_ = true;
        cancel_pcm_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    if (pcm_worker_.joinable())
        pcm_worker_.join();
}

void WaveformCacheService::registerSource(const std::array<std::uint8_t, 32>& identity,
                                          std::shared_ptr<const engine::MediaReadCapability> access,
                                          std::shared_ptr<const engine::VerifiedMediaIdentity> proof) {
    if (!access || (proof && (proof->Source() != access || proof->Identity().sha256 != identity)))
        return;
    std::vector<QString> retired_keys;
    {
        std::lock_guard lock(mutex_);
        std::erase_if(source_access_,
                      [&](const auto& entry) { return entry.first != identity && entry.second.access == access; });
        for (const auto& [key, job] : requested_jobs_)
            if (job.access == access && job.expected_sha256 != identity)
                retired_keys.push_back(QString::fromStdString(key));
        source_access_[identity] = {std::move(access), std::move(proof)};
    }
    for (const auto& key : retired_keys)
        cancel(key);
}

std::unique_ptr<engine::WaveformAudioDecoder> WaveformCacheService::openDecoder(const Job& job, std::string& error,
                                                                                const std::atomic<bool>* cancelled) {
    if (!custom_decoder_factory_ && job.access)
        return engine::OpenEditWaveformDecoder(job.access, job.container_index, error, cancelled,
                                               job.proof ? &job.expected_sha256 : nullptr);
    return decoder_factory_(job.path, job.container_index, error, cancelled, job.expected_sha256);
}

void WaveformCacheService::releaseSources() {
    {
        std::lock_guard lock(mutex_);
        cancel_running_ = true;
        cancel_pcm_ = true;
        release_pcm_cursor_ = true;
        cleanup_overviews_ = true;
        visible_interests_.clear();
        interest_distances_.clear();
        source_access_.clear();
        requested_jobs_.clear();
        queue_.clear();
        pcm_queue_.clear();
        availability_checks_.clear();
        for (auto& [key, writer] : provisional_writers_)
            retired_writers_.push_back(std::move(writer));
        provisional_writers_.clear();
        provisional_working_bytes_ = 0;
    }
    cv_.notify_all();
}

void WaveformCacheService::setVisibleInterest(std::uint64_t clip, const QString& key, qint64 distance_us) {
    if (!clip)
        return;
    {
        std::lock_guard lock(mutex_);
        const bool admitted = visible_interests_.contains(clip) || visible_interests_.size() < 8192;
        removeVisibleInterestLocked(clip);
        if (!key.isEmpty() && admitted) {
            visible_interests_[clip] = {key.toStdString(), std::max<qint64>(0, distance_us)};
            interest_distances_[key.toStdString()].emplace(std::max<qint64>(0, distance_us), clip);
        }
    }
    cv_.notify_all();
}

void WaveformCacheService::removeVisibleInterest(std::uint64_t clip) {
    std::lock_guard lock(mutex_);
    removeVisibleInterestLocked(clip);
}

void WaveformCacheService::removeVisibleInterestLocked(std::uint64_t clip) {
    const auto old = visible_interests_.find(clip);
    if (old == visible_interests_.end())
        return;
    auto distances = interest_distances_.find(old->second.key);
    if (distances != interest_distances_.end()) {
        distances->second.erase({old->second.distance_us, clip});
        if (distances->second.empty())
            interest_distances_.erase(distances);
    }
    visible_interests_.erase(old);
}

void WaveformCacheService::unregisterSource(const std::array<std::uint8_t, 32>& identity) {
    std::vector<QString> retired_keys;
    {
        std::lock_guard lock(mutex_);
        source_access_.erase(identity);
        for (const auto& [key, job] : requested_jobs_)
            if (job.expected_sha256 == identity)
                retired_keys.push_back(QString::fromStdString(key));
        release_pcm_cursor_ = true;
    }
    for (const auto& key : retired_keys)
        cancel(key);
    cv_.notify_all();
}

QString WaveformCacheService::request(const engine::WaveformSourceKey& source, const QString& source_path,
                                      qint64 source_end_us) {
    const std::string key = engine::WaveformCacheKey(source);
    const QString qkey = QString::fromStdString(key);
    if (source_end_us <= 0)
        return qkey;
    Job requested{key, std::filesystem::path(source_path.toStdWString()), source.container_index,
                  static_cast<std::uint64_t>(engine::AudioSampleIndex(source_end_us)), source.content_sha256};
    {
        std::lock_guard lock(mutex_);
        if (const auto access = source_access_.find(source.content_sha256); access != source_access_.end()) {
            requested.access = access->second.access;
            requested.proof = access->second.proof;
            requested.persistent = bool(requested.proof);
        }
        if (!requested_jobs_.contains(key) && requested_jobs_.size() >= kMaxLayouts)
            requested_jobs_.erase(requested_jobs_.begin());
        requested_jobs_[key] = requested;
        auto& status = status_[key];
        if (status.state == State::Queued || status.state == State::Analyzing)
            return qkey;
        if (status.state == State::Ready) {
            availability_checks_[key] = requested;
        } else {
            status.state = State::Queued;
            status.total_samples = static_cast<std::uint64_t>(engine::AudioSampleIndex(source_end_us));
            status.completed_samples = 0;
            queue_.push_back(requested);
        }
    }
    cv_.notify_all();
    return qkey;
}

void WaveformCacheService::cancel(const QString& qkey) {
    const std::string key = qkey.toStdString();
    {
        std::lock_guard lock(mutex_);
        requested_jobs_.erase(key);
        cleanup_overviews_ = true;
        availability_checks_.erase(key);
        if (running_pcm_ && running_pcm_->key == key)
            cancel_pcm_ = true;
        std::erase_if(pcm_interests_, [&key](const auto& interest) { return interest.second.id.key == key; });
        std::erase_if(pcm_queue_, [&key](const auto& job) { return job.id.key == key; });
        if (const auto writer = provisional_writers_.find(key); writer != provisional_writers_.end()) {
            provisional_working_bytes_ -= writer->second->MemoryBytes();
            retired_writers_.push_back(std::move(writer->second));
            provisional_writers_.erase(writer);
        }
        std::erase_if(pcm_entries_, [&key](const auto& entry) {
            return entry.first.key == key && entry.second.state == DetailState::Pending;
        });
        for (auto& [id, entry] : pcm_entries_)
            if (id.key == key)
                entry.subscribers = 0;
        const auto removed = std::erase_if(queue_, [&key](const Job& job) { return job.key == key; });
        if (running_ && *running_ == key)
            cancel_running_ = true;
        if (removed > 0 || !running_ || *running_ != key)
            status_.erase(key);
    }
    cv_.notify_all();
    emit pcmInterestsInvalidated(qkey);
}

void WaveformCacheService::setForegroundBusy(bool busy) {
    setAnalysisPressure(false, busy);
}

void WaveformCacheService::setAnalysisPressure(bool playback, bool exclusive) {
    {
        std::lock_guard lock(mutex_);
        busy_ = playback || exclusive;
        playback_ = playback;
        exclusive_ = exclusive;
    }
    cv_.notify_all();
}

bool WaveformCacheService::foregroundBusy() const {
    std::lock_guard lock(mutex_);
    return busy_;
}

qint64 WaveformCacheService::clearCache() {
    qint64 released = 0;
    {
        std::lock_guard lock(mutex_);
        released = static_cast<qint64>(store_->Retire());
        ++generation_;
        cleanup_overviews_ = true;
        cancel_pcm_ = true;
        pcm_queue_.clear();
        pcm_interests_.clear();
        pcm_entries_.clear();
        pcm_lru_.clear();
        queue_.clear();
        availability_checks_.clear();
        requested_jobs_.clear();
        if (running_)
            cancel_running_ = true;
        status_.clear();
        in_progress_.reset();
        in_progress_key_.clear();
        layouts_.clear();
        for (auto& [key, writer] : provisional_writers_)
            retired_writers_.push_back(std::move(writer));
        provisional_writers_.clear();
        provisional_working_bytes_ = 0;
        pending_blocks_.clear();
        blocks_.clear();
        block_lru_.clear();
    }
    cv_.notify_all();
    emit pcmInterestsInvalidated({});
    return released;
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
    const auto status = status_.find(key);
    if (status != status_.end() && status->second.state == State::Analyzing)
        return engine::WaveformLevelCount(status->second.total_samples);
    const auto found = layouts_.find(key);
    return found == layouts_.end() ? 0 : static_cast<std::uint32_t>(found->second.levels.size());
}

std::uint32_t WaveformCacheService::readBlock(const QString& qkey, std::uint32_t level, std::uint64_t block,
                                              std::vector<WaveformPeak>& out) {
    const auto data = tryReadPublishedBlock(qkey, level, block);
    if (!data) {
        out.clear();
        return 0;
    }
    out = data->peaks;
    return static_cast<std::uint32_t>(out.size() / engine::kWaveformChannels);
}

std::shared_ptr<const WaveformCacheService::PeakBlock>
WaveformCacheService::tryReadPublishedBlock(const QString& qkey, std::uint32_t level, std::uint64_t block) {
    if (qkey.isEmpty() || level >= engine::kWaveformMaxLevels ||
        block >
            (std::numeric_limits<std::uint64_t>::max() / engine::WaveformBucketSamples(level)) / kWaveformBlockBuckets)
        return {};
    BlockId id{qkey.toStdString(), level, block};
    bool schedule = false;
    {
        std::lock_guard lock(mutex_);
        const auto found = blocks_.find(id);
        if (found != blocks_.end()) {
            block_lru_.splice(block_lru_.end(), block_lru_, found->second.lru);
            return found->second.data;
        }
        const auto status = status_.find(id.key);
        const bool analysing = status != status_.end() &&
                               (status->second.state == State::Queued || status->second.state == State::Analyzing);
        const bool failed = status != status_.end() && status->second.state == State::Failed;
        if ((!analysing || status->second.state == State::Analyzing) && !failed && !pending_blocks_.contains(id)) {
            pending_blocks_.insert(id);
            schedule = true;
        }
    }
    if (schedule)
        cv_.notify_all();
    return {};
}

std::uint64_t WaveformCacheService::decoderOpensForTest() const {
    std::lock_guard lock(mutex_);
    return decoder_opens_;
}

std::uint64_t WaveformCacheService::requestPcmWindow(const QString& key, std::int64_t first_sample,
                                                     std::uint32_t sample_count, std::uint64_t view_generation) {
    const auto source_key = key.toStdString();
    const auto bytes = sizeof(PcmWindow) + source_key.size() + 1 +
                       static_cast<std::uint64_t>(sample_count) *
                           (sizeof(float) * engine::kWaveformChannels + sizeof(engine::AudioSampleSpan));
    if (first_sample < 0 || sample_count == 0 || bytes > kMaxPcmWindowBytes ||
        first_sample > std::numeric_limits<std::int64_t>::max() - sample_count)
        return 0;
    std::uint64_t ticket = 0;
    {
        std::lock_guard lock(mutex_);
        const auto source = requested_jobs_.find(source_key);
        if (source == requested_jobs_.end() || pcm_interests_.size() >= 512 ||
            next_pcm_ticket_ == std::numeric_limits<std::uint64_t>::max() ||
            next_pcm_job_ == std::numeric_limits<std::uint64_t>::max())
            return 0;
        const PcmId id{source_key, first_sample, sample_count};
        auto entry = pcm_entries_.find(id);
        if (entry == pcm_entries_.end()) {
            while (pcm_entries_.size() >= 256 && evictInactivePcmWindow()) {
            }
            if (pcm_entries_.size() >= 256 || pcm_queue_.size() >= 32)
                return 0;
            entry = pcm_entries_.emplace(id, PcmEntry{}).first;
            entry->second.job_id = ++next_pcm_job_;
            pcm_queue_.push_back(PcmJob{id, source->second, generation_, entry->second.job_id});
        } else if (entry->second.state == DetailState::Unavailable || entry->second.state == DetailState::Failed ||
                   entry->second.state == DetailState::NoCoverage ||
                   entry->second.state == DetailState::MemoryPressure) {
            if (pcm_queue_.size() >= 32)
                return 0;
            const auto subscribers = entry->second.subscribers;
            entry->second = PcmEntry{};
            entry->second.subscribers = subscribers;
            entry->second.job_id = ++next_pcm_job_;
            pcm_queue_.push_back(PcmJob{id, source->second, generation_, entry->second.job_id});
        }
        ticket = ++next_pcm_ticket_;
        pcm_interests_.emplace(ticket, PcmInterest{id, view_generation});
        ++entry->second.subscribers;
        if (entry->second.state == DetailState::Ready) {
            pcm_lru_.remove(id);
            pcm_lru_.push_back(id);
        }
    }
    cv_.notify_all();
    return ticket;
}

WaveformCacheService::PcmSnapshot WaveformCacheService::pcmWindow(std::uint64_t ticket) const {
    std::lock_guard lock(mutex_);
    const auto interest = pcm_interests_.find(ticket);
    if (interest == pcm_interests_.end())
        return {};
    const auto entry = pcm_entries_.find(interest->second.id);
    if (entry == pcm_entries_.end())
        return {};
    return {entry->second.state, entry->second.window, entry->second.reason};
}

void WaveformCacheService::cancelPcmWindow(std::uint64_t ticket) {
    {
        std::lock_guard lock(mutex_);
        const auto interest = pcm_interests_.find(ticket);
        if (interest == pcm_interests_.end())
            return;
        const auto id = interest->second.id;
        pcm_interests_.erase(interest);
        const auto entry = pcm_entries_.find(id);
        if (entry != pcm_entries_.end() && entry->second.subscribers > 0)
            --entry->second.subscribers;
        if (!pcmInterested(id)) {
            std::erase_if(pcm_queue_, [&id](const auto& job) { return job.id == id; });
            if (entry != pcm_entries_.end() && entry->second.state != DetailState::Ready)
                pcm_entries_.erase(entry);
            if (running_pcm_ && *running_pcm_ == id)
                cancel_pcm_ = true;
        }
    }
    cv_.notify_all();
}

bool WaveformCacheService::pcmInterested(const PcmId& id) const {
    const auto entry = pcm_entries_.find(id);
    return entry != pcm_entries_.end() && entry->second.subscribers > 0;
}

bool WaveformCacheService::evictInactivePcmWindow() {
    const auto oldest =
        std::find_if(pcm_lru_.begin(), pcm_lru_.end(), [this](const auto& id) { return !pcmInterested(id); });
    if (oldest == pcm_lru_.end())
        return false;
    pcm_entries_.erase(*oldest);
    pcm_lru_.erase(oldest);
    return true;
}

std::size_t WaveformCacheService::pcmMemoryBytesForTest() const {
    return pcm_bytes_->load(std::memory_order_relaxed);
}

void WaveformCacheService::pcmWorkerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    PcmCursor cursor;
    for (;;) {
        PcmJob job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || release_pcm_cursor_ || (!busy_ && !pcm_queue_.empty()); });
            if (stop_)
                return;
            if (release_pcm_cursor_) {
                release_pcm_cursor_ = false;
                lock.unlock();
                cursor = {};
                continue;
            }
            job = std::move(pcm_queue_.front());
            pcm_queue_.pop_front();
            running_pcm_ = job.id;
            cancel_pcm_ = false;
        }
        runPcmJob(job, cursor);
        std::lock_guard lock(mutex_);
        running_pcm_.reset();
    }
}

WaveformCacheService::PcmAllocation WaveformCacheService::reservePcmWindow(const PcmJob& job) {
    const auto payload_bytes = sizeof(PcmWindow) + job.id.key.size() + 1 +
                               static_cast<std::size_t>(job.id.count) *
                                   (sizeof(float) * engine::kWaveformChannels + sizeof(engine::AudioSampleSpan));
    const auto scratch_bytes = static_cast<std::size_t>(std::min(job.id.count, engine::kWaveformAnalysisChunkFrames)) *
                               sizeof(float) * engine::kWaveformChannels;
    const auto bytes = payload_bytes + scratch_bytes;
    std::lock_guard lock(mutex_);
    if (job.generation != generation_ || stop_ || !pcmInterested(job.id))
        return {};
    const auto fits = [&]() {
        return pcm_bytes_->load(std::memory_order_relaxed) + bytes <= kMaxPcmBytes &&
               block_bytes_->load(std::memory_order_relaxed) + bytes <= config_.memory_budget_bytes;
    };
    while (!fits() && evictInactivePcmWindow()) {
    }
    while (!block_lru_.empty() && !fits()) {
        blocks_.erase(block_lru_.front());
        block_lru_.pop_front();
    }
    if (!fits())
        return {};
    // Allocate charge owners before admission so allocation failure cannot
    // strand counters for either the immutable result or its decode scratch.
    auto payload = std::make_shared<PcmBudget>();
    auto scratch = std::make_shared<PcmBudget>();
    std::shared_ptr<PcmWindow> window(new PcmWindow, [payload](PcmWindow* data) { delete data; });
    window->source_key = job.id.key;
    window->first_sample = job.id.first;
    window->generation = job.generation;
    payload->total = block_bytes_;
    payload->pcm = pcm_bytes_;
    payload->bytes = payload_bytes;
    scratch->total = block_bytes_;
    scratch->pcm = pcm_bytes_;
    scratch->bytes = scratch_bytes;
    block_bytes_->fetch_add(bytes, std::memory_order_relaxed);
    pcm_bytes_->fetch_add(bytes, std::memory_order_relaxed);
    return {std::move(window), std::move(scratch)};
}

void WaveformCacheService::runPcmJob(const PcmJob& job, PcmCursor& cursor) {
    const auto proceed = [&]() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&]() {
            return stop_ || cancel_pcm_ || job.generation != generation_ || !pcmInterested(job.id) || !busy_;
        });
        const auto entry = pcm_entries_.find(job.id);
        return !stop_ && !cancel_pcm_ && job.generation == generation_ && pcmInterested(job.id) &&
               entry != pcm_entries_.end() && entry->second.job_id == job.job_id;
    };
    if (!proceed()) {
        finishPcmJob(job, {}, true);
        return;
    }
    if (static_cast<std::uint64_t>(job.id.first) >= job.source.sample_count) {
        finishPcmJob(job, {DetailState::NoCoverage, {}, {}}, false);
        return;
    }
    auto allocation = reservePcmWindow(job);
    auto& data = allocation.window;
    if (!data) {
        finishPcmJob(job, {DetailState::MemoryPressure, {}, {}}, false);
        return;
    }
    std::string error;
    if (!cursor.decoder || cursor.key != job.id.key || cursor.path != job.source.path) {
        cursor = {};
        cursor.decoder = openDecoder(job.source, error, &cancel_pcm_);
        if (!cursor.decoder) {
            finishPcmJob(job, {DetailState::Failed, {}, QString::fromStdString(error)}, cancel_pcm_.load());
            return;
        }
        cursor.key = job.id.key;
        cursor.path = job.source.path;
    }
    data->samples.resize(static_cast<std::size_t>(job.id.count) * engine::kWaveformChannels);
    data->coverage.reserve(job.id.count);
    const auto available = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(job.id.count, job.source.sample_count - static_cast<std::uint64_t>(job.id.first)));
    for (std::uint32_t offset = 0; offset < available;) {
        if (!proceed()) {
            finishPcmJob(job, {}, true);
            return;
        }
        const auto count = std::min(engine::kWaveformAnalysisChunkFrames, available - offset);
        const auto first = job.id.first + offset;
        std::vector<float> chunk;
        if (!cursor.decoder->Decode(first, count, chunk, error) ||
            chunk.size() != static_cast<std::size_t>(count) * engine::kWaveformChannels) {
            cursor = {};
            finishPcmJob(job, {DetailState::Failed, {}, QString::fromStdString(error)}, cancel_pcm_.load());
            return;
        }
        if (!std::all_of(chunk.begin(), chunk.end(), [](float sample) { return std::isfinite(sample); })) {
            cursor = {};
            finishPcmJob(job, {DetailState::Failed, {}, tr("Decoded audio contains nonfinite samples.")}, false);
            return;
        }
        std::copy(chunk.begin(), chunk.end(), data->samples.begin() + static_cast<std::size_t>(offset) * 2);
        std::int64_t previous_end = first;
        for (const auto span : cursor.decoder->Coverage()) {
            if (span.first < first || span.end > first + count || span.first >= span.end || span.first < previous_end) {
                cursor = {};
                finishPcmJob(job, {DetailState::Failed, {}, tr("Invalid decoded sample coverage.")}, false);
                return;
            }
            previous_end = span.end;
            if (!data->coverage.empty() && data->coverage.back().end == span.first)
                data->coverage.back().end = span.end;
            else
                data->coverage.push_back(span);
        }
        offset += count;
    }
    if (!proceed()) {
        finishPcmJob(job, {}, true);
        return;
    }
    if (data->coverage.empty())
        finishPcmJob(job, {DetailState::NoCoverage, {}, {}}, false);
    else
        finishPcmJob(job, {DetailState::Ready, std::move(data), {}}, false);
}

void WaveformCacheService::finishPcmJob(const PcmJob& job, PcmSnapshot snapshot, bool cancelled) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> listeners;
    {
        std::lock_guard lock(mutex_);
        const auto entry = pcm_entries_.find(job.id);
        if (job.generation != generation_ || entry == pcm_entries_.end() || entry->second.job_id != job.job_id)
            return;
        if (cancelled) {
            if (!stop_ && pcmInterested(job.id))
                pcm_queue_.push_back(job);
            else
                pcm_entries_.erase(entry);
            cv_.notify_all();
            return;
        }
        entry->second.state = snapshot.state;
        entry->second.window = std::move(snapshot.window);
        entry->second.reason = std::move(snapshot.reason);
        if (entry->second.state == DetailState::Ready)
            pcm_lru_.push_back(job.id);
        for (const auto& [ticket, interest] : pcm_interests_)
            if (interest.id == job.id)
                listeners.emplace_back(ticket, interest.view_generation);
    }
    for (const auto [ticket, view_generation] : listeners) {
        QMetaObject::invokeMethod(
            this,
            [this, ticket, view_generation, generation = job.generation]() {
                {
                    std::lock_guard lock(mutex_);
                    const auto interest = pcm_interests_.find(ticket);
                    if (generation != generation_ || interest == pcm_interests_.end() ||
                        interest->second.view_generation != view_generation)
                        return;
                }
                emit pcmWindowChanged(ticket, view_generation);
            },
            Qt::QueuedConnection);
    }
}

std::uint64_t WaveformCacheService::decodeCallsForTest() const {
    return decode_calls_.load(std::memory_order_relaxed);
}

std::size_t WaveformCacheService::memoryBytesForTest() const {
    return block_bytes_->load(std::memory_order_relaxed);
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
        std::deque<std::shared_ptr<engine::WaveformStreamWriter>> retired;
        bool availability_only = false;
        bool cleanup = false;
        bool stopping = false;
        std::uint64_t generation = 0;
        {
            std::unique_lock lock(mutex_);
            std::erase_if(deferred_overviews_,
                          [this](const auto& entry) { return !requested_jobs_.contains(entry.first); });
            const auto eligible = [this](const Job& candidate) {
                const auto deferred = deferred_overviews_.find(candidate.key);
                return !exclusive_ && (!playback_ || interest_distances_.contains(candidate.key)) &&
                       (deferred == deferred_overviews_.end() || deferred->second <= std::chrono::steady_clock::now());
            };
            const auto work_ready = [this, &eligible]() {
                return stop_ || cleanup_overviews_ || !retired_writers_.empty() || !pending_blocks_.empty() ||
                       !availability_checks_.empty() || std::any_of(queue_.begin(), queue_.end(), eligible);
            };
            if (deferred_overviews_.empty())
                cv_.wait(lock, work_ready);
            else
                cv_.wait_for(lock, std::chrono::milliseconds(50), work_ready);
            stopping = stop_;
            if (stopping) {
                for (auto& [key, writer] : provisional_writers_)
                    retired_writers_.push_back(std::move(writer));
                provisional_writers_.clear();
                provisional_working_bytes_ = 0;
            }
            cleanup = std::exchange(cleanup_overviews_, false);
            if (!retired_writers_.empty()) {
                retired.swap(retired_writers_);
            }
            if (!stopping && !cleanup && retired.empty() && pending_blocks_.empty() && !availability_checks_.empty()) {
                auto first = availability_checks_.begin();
                job = std::move(first->second);
                availability_checks_.erase(first);
                availability_only = true;
                generation = generation_;
            } else if (!stopping && !cleanup && retired.empty() && pending_blocks_.empty()) {
                if (playback_ && std::chrono::steady_clock::now() < next_overview_) {
                    cv_.wait_until(lock, next_overview_);
                    continue;
                }
                const auto priority = [this](const Job& candidate) {
                    const auto interest = interest_distances_.find(candidate.key);
                    const bool visible = interest != interest_distances_.end();
                    const auto cursor = overview_cursors_.find(candidate.key);
                    const auto completed = cursor == overview_cursors_.end() ? 0 : cursor->second->consumed_samples;
                    const auto rounds = completed / std::max<std::uint32_t>(1, config_.chunk_frames);
                    const auto distance = visible ? interest->second.begin()->first : 0;
                    return std::tuple{!visible, rounds, distance};
                };
                const auto next =
                    std::min_element(queue_.begin(), queue_.end(), [&](const Job& left, const Job& right) {
                        if (eligible(left) != eligible(right))
                            return eligible(left);
                        return priority(left) < priority(right);
                    });
                if (next == queue_.end() || !eligible(*next))
                    continue;
                job = std::move(*next);
                queue_.erase(next);
                running_ = job->key;
                cancel_running_ = false;
                if (playback_)
                    next_overview_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
            }
        }
        if (stopping) {
            while (!overview_cursors_.empty())
                discardOverview(overview_cursors_.begin()->first);
            return;
        }
        if (cleanup) {
            purgeOverviews();
            continue;
        }
        if (!retired.empty())
            continue;
        if (availability_only) {
            checkAvailability(*job, generation);
            continue;
        }
        if (!job) {
            loadPendingBlocks();
            continue;
        }
        runJob(*job);
        bool retired_job = false;
        {
            std::lock_guard lock(mutex_);
            retired_job = !requested_jobs_.contains(job->key);
        }
        if (retired_job)
            discardOverview(job->key);
        {
            std::lock_guard lock(mutex_);
            if (retired_job && !requested_jobs_.contains(job->key))
                status_.erase(job->key);
            running_.reset();
        }
    }
}

void WaveformCacheService::checkAvailability(const Job& job, std::uint64_t generation) {
    std::error_code error;
    const bool available = job.access || std::filesystem::is_regular_file(job.path, error);
    {
        std::lock_guard lock(mutex_);
        const auto status = status_.find(job.key);
        if (generation != generation_ || status == status_.end() || status->second.state != State::Ready ||
            status->second.source_available == available)
            return;
        status->second.source_available = available;
    }
    QMetaObject::invokeMethod(
        this,
        [this, key = QString::fromStdString(job.key), available, generation]() {
            {
                std::lock_guard lock(mutex_);
                if (generation != generation_)
                    return;
            }
            emit sourceAvailabilityChanged(key, available);
        },
        Qt::QueuedConnection);
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
    std::map<std::string, std::uint64_t> loaded_keys;
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
        std::shared_ptr<engine::WaveformStreamWriter> staging;
        {
            std::lock_guard lock(mutex_);
            if (in_progress_ && in_progress_key_ == id.key && generation == generation_)
                staging = in_progress_;
            else if (const auto provisional = provisional_writers_.find(id.key);
                     provisional != provisional_writers_.end())
                staging = provisional->second;
        }
        if (!staging) {
            const auto cursor = overview_cursors_.find(id.key);
            if (cursor != overview_cursors_.end() && cursor->second->generation == generation)
                staging = cursor->second->builder;
        }
        const bool from_staging = static_cast<bool>(staging);
        if (staging)
            staging->ReadBlock(id.level, id.block * kWaveformBlockBuckets, kWaveformBlockBuckets, peaks);
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
            if (generation != generation_)
                continue;
            pending_blocks_.erase(id);
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
        const auto valid_samples = staging ? staging->CompletedSamples() : layout->sample_count;
        insertBlock(id, std::move(peaks), generation, valid_samples);
        loaded_keys[id.key] = generation;
    }
    for (const auto& [key, generation] : loaded_keys) {
        const QString qkey = QString::fromStdString(key);
        QMetaObject::invokeMethod(
            this,
            [this, qkey, generation]() {
                {
                    std::lock_guard lock(mutex_);
                    if (generation != generation_)
                        return;
                }
                emit blocksLoaded(qkey);
            },
            Qt::QueuedConnection);
    }
}

void WaveformCacheService::insertBlock(const BlockId& id, std::vector<WaveformPeak> peaks, std::uint64_t generation,
                                       std::uint64_t valid_samples) {
    std::lock_guard lock(mutex_);
    if (generation != generation_ || peaks.empty())
        return;
    const auto found = blocks_.find(id);
    if (found != blocks_.end() && found->second.data->peaks.size() >= peaks.size())
        return;
    if (found != blocks_.end()) {
        block_lru_.erase(found->second.lru);
        blocks_.erase(found);
    }
    const std::size_t bytes = sizeof(PeakBlock) + peaks.capacity() * sizeof(WaveformPeak);
    while (!block_lru_.empty() && block_bytes_->load(std::memory_order_relaxed) + bytes > config_.memory_budget_bytes) {
        const auto oldest = blocks_.find(block_lru_.front());
        blocks_.erase(oldest);
        block_lru_.pop_front();
    }
    if (block_bytes_->load(std::memory_order_relaxed) + bytes > config_.memory_budget_bytes)
        return;
    const auto bucket_samples = engine::WaveformBucketSamples(id.level);
    const auto first = id.block * kWaveformBlockBuckets * bucket_samples;
    const auto end = first + (peaks.size() / engine::kWaveformChannels) * bucket_samples;
    auto data =
        std::unique_ptr<PeakBlock>(new PeakBlock{std::move(peaks), first, std::min(end, valid_samples), generation});
    block_bytes_->fetch_add(bytes, std::memory_order_relaxed);
    std::shared_ptr<const PeakBlock> published(data.release(), [counter = block_bytes_, bytes](const PeakBlock* block) {
        delete block;
        counter->fetch_sub(bytes, std::memory_order_relaxed);
    });
    block_lru_.push_back(id);
    blocks_.emplace(id, CachedBlock{std::move(published), std::prev(block_lru_.end())});
}

void WaveformCacheService::publishProgressBlocks(const Job& job, engine::WaveformStreamWriter& builder,
                                                 std::uint64_t previous, std::uint64_t completed,
                                                 std::uint64_t generation, bool finished) {
    for (std::uint32_t level = 0; level < builder.Layout().levels.size(); ++level) {
        const auto samples = engine::WaveformBucketSamples(level);
        const auto first_bucket = previous / samples;
        const auto end_bucket = finished ? engine::WaveformBucketCount(completed, level) : completed / samples;
        if (end_bucket <= first_bucket)
            continue;
        for (auto block = first_bucket / kWaveformBlockBuckets; block <= (end_bucket - 1) / kWaveformBlockBuckets;
             ++block) {
            if (cancel_running_)
                return;
            std::vector<WaveformPeak> peaks;
            builder.ReadBlock(level, block * kWaveformBlockBuckets, kWaveformBlockBuckets, peaks);
            insertBlock(BlockId{job.key, level, block}, std::move(peaks), generation, completed);
        }
    }
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

void WaveformCacheService::discardOverview(const std::string& key) {
    deferred_overviews_.erase(key);
    const auto found = overview_cursors_.find(key);
    if (found == overview_cursors_.end())
        return;
    {
        std::lock_guard lock(mutex_);
        if (in_progress_key_ == key) {
            in_progress_.reset();
            in_progress_key_.clear();
        }
    }
    overview_working_bytes_ -= found->second->working_bytes;
    overview_cursors_.erase(found);
    deferred_overviews_.clear();
}

void WaveformCacheService::purgeOverviews() {
    std::vector<std::string> retired;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [key, cursor] : overview_cursors_)
            if (cursor->generation != generation_ || !requested_jobs_.contains(key))
                retired.push_back(key);
    }
    for (const auto& key : retired)
        discardOverview(key);
}

void WaveformCacheService::runJob(const Job& job) {
    const QString qkey = QString::fromStdString(job.key);
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        generation = generation_;
        if (cancel_running_ || !requested_jobs_.contains(job.key))
            return;
    }
    const auto fail = [&](const QString& reason, bool available = true) {
        discardOverview(job.key);
        if (!setStatus(job.key, Status{State::Failed, available, job.sample_count, 0}, generation))
            return;
        QMetaObject::invokeMethod(
            this,
            [this, qkey, reason, generation] {
                {
                    std::lock_guard lock(mutex_);
                    if (generation != generation_)
                        return;
                }
                emit failed(qkey, reason);
            },
            Qt::QueuedConnection);
    };
    const auto defer = [&] {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || cancel_running_ || !requested_jobs_.contains(job.key))
            return;
        deferred_overviews_[job.key] = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        queue_.push_back(job);
        if (!provisional_writers_.empty()) {
            const auto retired = provisional_writers_.begin();
            provisional_working_bytes_ -= retired->second->MemoryBytes();
            retired_writers_.push_back(std::move(retired->second));
            provisional_writers_.erase(retired);
        }
    };
    auto found = overview_cursors_.find(job.key);
    if (found != overview_cursors_.end() && found->second->generation != generation) {
        discardOverview(job.key);
        found = overview_cursors_.end();
    }
    std::string error;
    if (found == overview_cursors_.end()) {
        if (job.persistent) {
            if (const auto layout = layoutFor(job.key)) {
                std::error_code availability_error;
                const bool available = job.access || std::filesystem::is_regular_file(job.path, availability_error);
                setStatus(job.key, Status{State::Ready, available, layout->sample_count, layout->sample_count},
                          generation);
                postProgress(job.key, layout->sample_count, layout->sample_count, generation);
                QMetaObject::invokeMethod(
                    this,
                    [this, qkey, generation] {
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
        }
        std::error_code fs_error;
        if (!job.access && !std::filesystem::is_regular_file(job.path, fs_error)) {
            fail(QStringLiteral("The audio source is unavailable."), false);
            return;
        }
        auto cursor = std::make_unique<OverviewCursor>();
        cursor->generation = generation;
        engine::MediaCacheWriteFailure failure = engine::MediaCacheWriteFailure::None;
        cursor->builder = engine::WaveformStreamWriter::Open(*store_, job.key, job.sample_count, error, &failure);
        if (!cursor->builder) {
            if (failure == engine::MediaCacheWriteFailure::Busy) {
                defer();
                return;
            }
            fail(QString::fromStdString(error));
            return;
        }
        cursor->working_bytes = cursor->builder->MemoryBytes() +
                                static_cast<std::size_t>(std::clamp<std::uint32_t>(
                                    config_.chunk_frames, 1, engine::kWaveformAnalysisChunkFrames)) *
                                    sizeof(float) * engine::kWaveformChannels +
                                64 * 1024;
        std::size_t retained_bytes = 0;
        {
            std::lock_guard lock(mutex_);
            retained_bytes = provisional_working_bytes_;
        }
        if (cursor->working_bytes > config_.analysis_budget_bytes) {
            fail(QStringLiteral("The background analysis working set exceeds its memory budget."));
            return;
        }
        if (overview_working_bytes_ + retained_bytes > config_.analysis_budget_bytes - cursor->working_bytes) {
            defer();
            return;
        }
        deferred_overviews_.erase(job.key);
        overview_working_bytes_ += cursor->working_bytes;
        {
            std::lock_guard lock(mutex_);
            analysis_peak_bytes_ = std::max(analysis_peak_bytes_, overview_working_bytes_ + provisional_working_bytes_);
        }
        overview_cursors_.emplace(job.key, std::move(cursor));
        if (!setStatus(job.key, Status{State::Analyzing, true, job.sample_count, 0}, generation)) {
            discardOverview(job.key);
            return;
        }
    }
    auto& cursor = *overview_cursors_.at(job.key);
    cursor.last_used = ++overview_sequence_;
    if (!cursor.decoder) {
        std::size_t active_decoders = 0;
        auto oldest = overview_cursors_.end();
        for (auto candidate = overview_cursors_.begin(); candidate != overview_cursors_.end(); ++candidate) {
            if (!candidate->second->decoder)
                continue;
            ++active_decoders;
            if (oldest == overview_cursors_.end() || candidate->second->last_used < oldest->second->last_used)
                oldest = candidate;
        }
        if (active_decoders >= 2 && oldest != overview_cursors_.end())
            oldest->second->decoder.reset();
        auto decoder = openDecoder(job, error, &cancel_running_);
        {
            std::lock_guard lock(mutex_);
            ++decoder_opens_;
        }
        if (!decoder) {
            if (cancel_running_) {
                discardOverview(job.key);
                return;
            }
            fail(QString::fromStdString(error));
            return;
        }
        cursor.decoder = std::make_unique<CountingDecoder>(std::move(decoder), decode_calls_);
    }
    {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || cancel_running_)
            return;
        in_progress_ = cursor.builder;
        in_progress_key_ = job.key;
    }
    const auto previous = cursor.builder->CompletedSamples();
    const auto chunk = std::clamp<std::uint32_t>(config_.chunk_frames, 1, engine::kWaveformAnalysisChunkFrames);
    const auto frames =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk, job.sample_count - cursor.consumed_samples));
    std::vector<float> samples;
    const auto before_seeks = cursor.decoder->Seeks();
    const auto before_frames = cursor.decoder->DecodedFrames();
    const bool decoded =
        cursor.consumed_samples <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
        cursor.decoder->Decode(static_cast<std::int64_t>(cursor.consumed_samples), frames, samples, error);
    {
        std::lock_guard lock(mutex_);
        decoder_seeks_ += cursor.decoder->Seeks() - before_seeks;
        decoded_frames_ += cursor.decoder->DecodedFrames() - before_frames;
        if (generation != generation_ || cancel_running_)
            return;
    }
    if (!decoded || samples.size() < static_cast<std::size_t>(frames) * engine::kWaveformChannels ||
        !cursor.builder->Append(samples.data(), frames)) {
        fail(QString::fromStdString(error.empty() ? "Cannot decode waveform analysis samples." : error));
        return;
    }
    cursor.consumed_samples += frames;
    const auto completed = cursor.builder->CompletedSamples();
    publishProgressBlocks(job, *cursor.builder, previous, completed, generation);
    {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || cancel_running_)
            return;
        status_[job.key].completed_samples = completed;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - cursor.last_progress >= kProgressInterval) {
        cursor.last_progress = now;
        postProgress(job.key, completed, job.sample_count, generation);
    }
    if (cursor.consumed_samples < job.sample_count) {
        std::lock_guard lock(mutex_);
        if (generation == generation_ && !cancel_running_ && requested_jobs_.contains(job.key))
            queue_.push_back(job);
        return;
    }
    auto builder = cursor.builder;
    const bool finished = builder->Finish();
    if (finished)
        publishProgressBlocks(job, *builder, completed, builder->CompletedSamples(), generation, true);
    const bool published = finished && (!job.persistent || builder->Publish(error, [this] {
                               return !cancel_running_.load(std::memory_order_relaxed);
                           }));
    if (!published) {
        if (cancel_running_) {
            discardOverview(job.key);
            return;
        }
        fail(QString::fromStdString(error.empty() ? "The waveform could not be persisted." : error));
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (generation != generation_ || cancel_running_)
            return;
        layouts_[job.key] = builder->Layout();
        if (!job.persistent) {
            if (provisional_writers_.size() >= 8) {
                provisional_working_bytes_ -= provisional_writers_.begin()->second->MemoryBytes();
                retired_writers_.push_back(std::move(provisional_writers_.begin()->second));
                provisional_writers_.erase(provisional_writers_.begin());
            }
            provisional_writers_[job.key] = builder;
            provisional_working_bytes_ += builder->MemoryBytes();
        }
    }
    discardOverview(job.key);
    if (!setStatus(job.key, Status{State::Ready, true, job.sample_count, job.sample_count}, generation))
        return;
    postProgress(job.key, job.sample_count, job.sample_count, generation);
    QMetaObject::invokeMethod(
        this,
        [this, qkey, generation] {
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

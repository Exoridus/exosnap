#include "ThumbnailCache.h"

#include "TimelineThumbnailSource.h"

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QImageReader>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace exosnap {
namespace {

constexpr std::size_t kMaxTilesPerWindow = 4096;
constexpr int kMaxThumbnailHeight = 256;
constexpr int kMaxThumbnailWidth = 4096;
constexpr std::uint64_t kMaxThumbnailPayloadBytes = 8u * 1024 * 1024;

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void PutU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

std::filesystem::path DefaultThumbnailRoot() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return std::filesystem::path(QDir(base).filePath(QStringLiteral("thumbnails")).toStdWString());
}

engine::MediaCacheStoreConfig ThumbnailStoreConfig(std::filesystem::path root, std::uint64_t budget) {
    engine::MediaCacheStoreConfig config;
    config.root = root.empty() ? DefaultThumbnailRoot() : std::move(root);
    config.budget_bytes = budget;
    config.kind = {'T', 'H', 'M', 'B'};
    config.schema_version = kThumbnailSchemaVersion;
    config.extension = ".thumb";
    return config;
}

// The source frame grid: frame k starts at floor(k * unit_num / unit_den) us.
struct FrameGrid {
    std::int64_t unit_num = 1000; // microseconds per frame, as a fraction
    std::int64_t unit_den = 1;

    static FrameGrid For(int fps_num, int fps_den) {
        if (fps_num <= 0 || fps_den <= 0)
            return {};
        return FrameGrid{1'000'000LL * fps_den, fps_num};
    }
    [[nodiscard]] double FrameUs() const {
        return static_cast<double>(unit_num) / static_cast<double>(unit_den);
    }
    [[nodiscard]] std::int64_t TimeOf(std::int64_t frame) const {
        return static_cast<std::int64_t>((static_cast<long double>(frame) * unit_num) / unit_den);
    }
    [[nodiscard]] std::int64_t FrameAtOrBefore(std::int64_t time_us) const {
        auto frame = static_cast<std::int64_t>((static_cast<long double>(time_us) * unit_den) / unit_num);
        while (frame > 0 && TimeOf(frame) > time_us)
            --frame;
        while (TimeOf(frame + 1) <= time_us)
            ++frame;
        return frame;
    }
};

// Frames per tile: the smallest power of two whose span is at least one tile.
std::int64_t FramesPerTile(const FrameGrid& grid, double us_per_pixel, int height_px, int video_width,
                           int video_height) {
    const double tile_us = TimelineTileWidth(height_px, video_width, video_height) * us_per_pixel;
    std::int64_t frames = 1;
    while (frames < (std::int64_t{1} << 40) && static_cast<double>(frames) * grid.FrameUs() < tile_us)
        frames *= 2;
    return frames;
}

} // namespace

std::string ThumbnailCacheKey(const std::array<std::uint8_t, 32>& content_sha256, std::int64_t source_time_us,
                              int height, ThumbnailVariant variant, std::uint32_t color_conversion_version) {
    std::vector<std::uint8_t> canonical;
    constexpr std::string_view kDomain = "exosnap.thumbnail-cache";
    canonical.insert(canonical.end(), kDomain.begin(), kDomain.end());
    canonical.push_back(0);
    PutU32(canonical, kThumbnailSchemaVersion);
    canonical.insert(canonical.end(), content_sha256.begin(), content_sha256.end());
    PutU64(canonical, static_cast<std::uint64_t>(source_time_us));
    PutU32(canonical, static_cast<std::uint32_t>(height));
    PutU32(canonical, static_cast<std::uint32_t>(variant));
    PutU32(canonical, color_conversion_version);
    return engine::ToHex(engine::Sha256(canonical));
}

std::optional<std::array<std::uint8_t, 32>> ParseContentIdentity(const QString& hex) {
    if (hex.size() != 64)
        return std::nullopt;
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    if (bytes.size() != 32 || bytes.toHex() != hex.toLower().toLatin1())
        return std::nullopt;
    std::array<std::uint8_t, 32> out{};
    std::memcpy(out.data(), bytes.constData(), out.size());
    return out;
}

std::vector<ThumbnailTile> ThumbnailTilesForWindow(std::int64_t source_in_us, std::int64_t source_out_us,
                                                   double width_px, int height_px, int video_width, int video_height,
                                                   int fps_num, int fps_den) {
    std::vector<ThumbnailTile> tiles;
    if (!(width_px > 0) || height_px <= 0 || source_out_us <= source_in_us || source_in_us < 0)
        return tiles;
    const double us_per_pixel = static_cast<double>(source_out_us - source_in_us) / width_px;
    const auto grid = FrameGrid::For(fps_num, fps_den);
    const auto frames = FramesPerTile(grid, us_per_pixel, height_px, video_width, video_height);
    std::int64_t tile = grid.FrameAtOrBefore(source_in_us) / frames;
    for (; tiles.size() < kMaxTilesPerWindow; ++tile) {
        const auto start = grid.TimeOf(tile * frames);
        if (start >= source_out_us)
            break;
        const auto next = grid.TimeOf((tile + 1) * frames);
        tiles.push_back(ThumbnailTile{start, static_cast<double>(start - source_in_us) / us_per_pixel,
                                      static_cast<double>(next - start) / us_per_pixel});
    }
    return tiles;
}

std::vector<std::int64_t> ThumbnailGridTimes(std::int64_t from_us, std::int64_t to_us, double us_per_pixel,
                                             int height_px, int video_width, int video_height, int fps_num, int fps_den,
                                             std::size_t limit) {
    std::vector<std::int64_t> times;
    if (!(us_per_pixel > 0) || height_px <= 0 || to_us <= from_us)
        return times;
    const auto grid = FrameGrid::For(fps_num, fps_den);
    const auto frames = FramesPerTile(grid, us_per_pixel, height_px, video_width, video_height);
    std::int64_t tile = grid.FrameAtOrBefore(std::max<std::int64_t>(0, from_us)) / frames;
    for (; times.size() < limit; ++tile) {
        const auto start = grid.TimeOf(tile * frames);
        if (start >= to_us)
            break;
        times.push_back(start);
    }
    return times;
}

ThumbnailDiskCache::ThumbnailDiskCache(std::filesystem::path root, std::uint64_t budget_bytes)
    : store_(ThumbnailStoreConfig(std::move(root), budget_bytes)) {
}

std::optional<QImage> ThumbnailDiskCache::Load(const std::string& key) {
    const auto size = store_.OpenVerified(key);
    if (!size)
        return std::nullopt;
    if (*size > kMaxThumbnailPayloadBytes) {
        store_.Remove(key);
        return std::nullopt;
    }
    const auto payload = store_.Read(key);
    if (!payload || payload->size() <= 8)
        return std::nullopt;
    QByteArray png(reinterpret_cast<const char*>(payload->data() + 8), static_cast<qsizetype>(payload->size() - 8));
    QBuffer buffer(&png);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    const auto dimensions = reader.size();
    if (dimensions.width() <= 0 || dimensions.width() > kMaxThumbnailWidth || dimensions.height() <= 0 ||
        dimensions.height() > kMaxThumbnailHeight) {
        store_.Remove(key);
        return std::nullopt;
    }
    QImage image = reader.read();
    if (image.isNull()) {
        store_.Remove(key);
        return std::nullopt;
    }
    return image;
}

bool ThumbnailDiskCache::Store(const std::string& key, const QImage& image, std::int64_t decoded_pts_us) {
    if (image.isNull() || image.height() > kMaxThumbnailHeight || image.width() > kMaxThumbnailWidth)
        return false;
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return false;
    std::vector<std::uint8_t> payload;
    payload.reserve(8 + static_cast<std::size_t>(png.size()));
    PutU64(payload, static_cast<std::uint64_t>(decoded_pts_us));
    payload.insert(payload.end(), png.begin(), png.end());
    std::string error;
    return store_.Publish(key, payload, error);
}

std::uint64_t ThumbnailDiskCache::Clear() {
    return store_.Clear();
}

ThumbnailRequestBroker::ThumbnailRequestBroker() : ThumbnailRequestBroker(Config{}) {
}

ThumbnailRequestBroker::ThumbnailRequestBroker(Config config) : config_(std::move(config)) {
    if (!config_.equivalent_files)
        config_.equivalent_files = [](const auto& left, const auto& right) {
            std::error_code error;
            return std::filesystem::equivalent(left, right, error);
        };
    if (config_.disk_enabled)
        disk_ = std::make_unique<ThumbnailDiskCache>(config_.disk_root, config_.disk_budget_bytes);
}

ThumbnailRequestBroker::~ThumbnailRequestBroker() {
    DetachWorker();
}

void ThumbnailRequestBroker::RegisterSource(const std::string& identity_hex, ThumbnailSourceInfo info) {
    std::lock_guard lock(mutex_);
    std::erase_if(sources_,
                  [&](const auto& entry) { return entry.first != identity_hex && entry.second.path == info.path; });
    sources_[identity_hex] = std::move(info);
}

std::optional<ThumbnailSourceInfo> ThumbnailRequestBroker::Source(const std::string& identity_hex) const {
    std::lock_guard lock(mutex_);
    const auto found = sources_.find(identity_hex);
    if (found == sources_.end())
        return std::nullopt;
    return found->second;
}

std::optional<ThumbnailRequestBroker::SourceRegistration>
ThumbnailRequestBroker::ResolveSource(const std::filesystem::path& path) const {
    std::vector<SourceRegistration> candidates;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [identity, info] : sources_)
            if (info.path == path)
                return SourceRegistration{identity, info};
        candidates.reserve(sources_.size());
        for (const auto& [identity, info] : sources_)
            candidates.push_back(SourceRegistration{identity, info});
    }
    for (auto& candidate : candidates)
        if (config_.equivalent_files(candidate.info.path, path))
            return std::optional<SourceRegistration>{std::move(candidate)};
    return std::nullopt;
}

std::optional<std::string> ThumbnailRequestBroker::IdentityForPath(const std::filesystem::path& path) const {
    if (auto source = ResolveSource(path))
        return std::move(source->identity);
    return std::nullopt;
}
std::optional<ThumbnailSourceInfo> ThumbnailRequestBroker::SourceForPath(const std::filesystem::path& path) const {
    if (auto source = ResolveSource(path))
        return std::move(source->info);
    return std::nullopt;
}
std::uint64_t ThumbnailRequestBroker::Request(const std::string& identity_hex, std::int64_t time_us, int height,
                                              ThumbnailCompletion completion) {
    std::string key;
    ThumbnailSourceInfo info;
    {
        std::lock_guard lock(mutex_);
        const auto source = sources_.find(identity_hex);
        if (attached_ && source != sources_.end() && height > 0 && height <= kMaxThumbnailHeight && time_us >= 0) {
            key = ThumbnailCacheKey(source->second.content_sha256, time_us, height);
            info = source->second;
        }
    }
    if (key.empty()) {
        completion(QImage());
        return 0;
    }
    if (const auto hit = MemoryLookup(key)) {
        completion(*hit);
        return 0;
    }
    if (synthetic_images_) {
        const QImage image = synthetic_images_(info, time_us, height);
        if (!image.isNull())
            MemoryInsert(key, image);
        completion(image);
        return 0;
    }
    std::uint64_t ticket = 0;
    {
        std::unique_lock lock(mutex_);
        if (!attached_) {
            lock.unlock();
            completion(QImage());
            return 0;
        } else {
            ticket = ++next_ticket_;
            tickets_[ticket] = key;
            auto [found, inserted] = pending_.try_emplace(key);
            auto& pending = found->second;
            pending.waiters.emplace_back(ticket, std::move(completion));
            if (inserted) {
                pending.job = Job{key, identity_hex, time_us, height, false, generation_};
                visible_order_.push_back(key);
            } else if (pending.prefetch) {
                pending.prefetch = false;
                const auto queued = std::find(prefetch_order_.begin(), prefetch_order_.end(), key);
                if (queued != prefetch_order_.end()) {
                    prefetch_order_.erase(queued);
                    visible_order_.push_back(key);
                }
            }
        }
    }
    Wake();
    return ticket;
}

void ThumbnailRequestBroker::Cancel(std::uint64_t ticket) {
    ThumbnailCompletion completion;
    {
        std::lock_guard lock(mutex_);
        const auto found = tickets_.find(ticket);
        if (found == tickets_.end())
            return;
        const std::string key = found->second;
        tickets_.erase(found);
        const auto pending = pending_.find(key);
        if (pending == pending_.end())
            return;
        auto& waiters = pending->second.waiters;
        const auto waiter =
            std::find_if(waiters.begin(), waiters.end(), [ticket](const auto& entry) { return entry.first == ticket; });
        if (waiter == waiters.end())
            return;
        completion = std::move(waiter->second);
        waiters.erase(waiter);
        if (waiters.empty() && !pending->second.prefetch) {
            const auto remove = [&key](std::deque<std::string>& order) {
                const auto queued = std::find(order.begin(), order.end(), key);
                if (queued == order.end())
                    return false;
                order.erase(queued);
                return true;
            };
            // A job the worker already took stays in pending_ until it completes.
            if (remove(visible_order_) || remove(decode_order_))
                pending_.erase(pending);
        }
    }
    completion(QImage());
}

void ThumbnailRequestBroker::UseSyntheticImagesForHarness(SyntheticImages images) {
    synthetic_images_ = std::move(images);
}

void ThumbnailRequestBroker::Prefetch(const std::string& identity_hex, const std::vector<std::int64_t>& times_us,
                                      int height) {
    if (synthetic_images_)
        return;
    std::vector<std::string> keys;
    {
        std::lock_guard lock(mutex_);
        const auto source = sources_.find(identity_hex);
        if (!attached_ || source == sources_.end() || height <= 0 || height > kMaxThumbnailHeight)
            return;
        for (const auto time_us : times_us)
            keys.push_back(ThumbnailCacheKey(source->second.content_sha256, time_us, height));
    }
    std::vector<bool> cached(keys.size());
    {
        std::lock_guard memory(memory_mutex_);
        for (std::size_t i = 0; i < keys.size(); ++i)
            cached[i] = memory_.contains(keys[i]);
    }
    {
        std::lock_guard lock(mutex_);
        // Replace this source's earlier prefetches that nobody waits for: the
        // view moved, and the old neighbourhood is no longer the likely one.
        for (auto it = prefetch_order_.begin(); it != prefetch_order_.end();) {
            const auto pending = pending_.find(*it);
            if (pending != pending_.end() && pending->second.job.identity == identity_hex &&
                pending->second.waiters.empty()) {
                pending_.erase(pending);
                it = prefetch_order_.erase(it);
            } else {
                ++it;
            }
        }
        for (std::size_t i = 0; i < keys.size() && prefetch_order_.size() < kMaxPrefetchJobs; ++i) {
            if (cached[i] || pending_.contains(keys[i]))
                continue;
            Pending pending;
            pending.job = Job{keys[i], identity_hex, times_us[i], height, false, generation_};
            pending.prefetch = true;
            pending_.emplace(keys[i], std::move(pending));
            prefetch_order_.push_back(keys[i]);
        }
    }
    Wake();
}

void ThumbnailRequestBroker::SetForegroundBusy(bool busy) {
    {
        std::lock_guard lock(mutex_);
        busy_ = busy;
    }
    Wake();
}

bool ThumbnailRequestBroker::ForegroundBusy() const {
    std::lock_guard lock(mutex_);
    return busy_;
}

std::vector<ThumbnailCompletion> ThumbnailRequestBroker::RetireRequestsLocked() {
    ++generation_;
    std::vector<ThumbnailCompletion> retired;
    for (auto& [key, pending] : pending_)
        for (auto& [ticket, completion] : pending.waiters)
            retired.push_back(std::move(completion));
    pending_.clear();
    visible_order_.clear();
    prefetch_order_.clear();
    decode_order_.clear();
    tickets_.clear();
    return retired;
}

std::uint64_t ThumbnailRequestBroker::Clear() {
    std::vector<ThumbnailCompletion> retired;
    std::uint64_t released = 0;
    {
        std::lock_guard publication(publication_mutex_);
        {
            std::lock_guard memory(memory_mutex_);
            memory_.clear();
            memory_lru_.clear();
            memory_bytes_ = 0;
        }
        released = disk_ ? disk_->Clear() : 0;
        // Requests arriving during storage clearing must retire with the old epoch.
        std::lock_guard lock(mutex_);
        retired = RetireRequestsLocked();
    }
    for (auto& completion : retired)
        completion(QImage());
    return released;
}

void ThumbnailRequestBroker::AttachWorker(std::function<void()> wake) {
    {
        std::lock_guard wake_lock(wake_mutex_);
        wake_ = std::move(wake);
    }
    std::lock_guard lock(mutex_);
    attached_ = true;
}

void ThumbnailRequestBroker::DetachWorker() {
    {
        // Waits for a wake in progress, so the worker may be destroyed after.
        std::lock_guard wake_lock(wake_mutex_);
        wake_ = {};
    }
    std::vector<ThumbnailCompletion> orphaned;
    {
        std::lock_guard publication(publication_mutex_);
        std::lock_guard lock(mutex_);
        attached_ = false;
        orphaned = RetireRequestsLocked();
    }
    for (auto& completion : orphaned)
        completion(QImage());
}

bool ThumbnailRequestBroker::HasWork() const {
    std::lock_guard lock(mutex_);
    return !visible_order_.empty() || !prefetch_order_.empty() || (!busy_ && !decode_order_.empty());
}

std::optional<ThumbnailRequestBroker::Job> ThumbnailRequestBroker::TakeFrom(std::deque<std::string>& order) {
    while (!order.empty()) {
        const auto key = order.front();
        order.pop_front();
        const auto pending = pending_.find(key);
        if (pending != pending_.end())
            return pending->second.job;
    }
    return std::nullopt;
}

std::optional<ThumbnailRequestBroker::Job> ThumbnailRequestBroker::TakeJob() {
    std::lock_guard lock(mutex_);
    if (auto job = TakeFrom(visible_order_))
        return job;
    if (!busy_) {
        const auto waited = std::find_if(decode_order_.begin(), decode_order_.end(), [this](const std::string& key) {
            const auto pending = pending_.find(key);
            return pending != pending_.end() && !pending->second.waiters.empty();
        });
        if (waited != decode_order_.end()) {
            const auto key = *waited;
            decode_order_.erase(waited);
            return pending_.find(key)->second.job;
        }
    }
    if (auto job = TakeFrom(prefetch_order_))
        return job;
    if (busy_)
        return std::nullopt;
    return TakeFrom(decode_order_);
}
void ThumbnailRequestBroker::DeferDecode(Job job) {
    {
        std::lock_guard lock(mutex_);
        const auto pending = pending_.find(job.key);
        if (job.generation != generation_ || pending == pending_.end() ||
            pending->second.job.generation != job.generation)
            return;
        job.disk_checked = true;
        pending->second.job = job;
        decode_order_.push_back(job.key);
    }
    Wake();
}

void ThumbnailRequestBroker::Complete(const Job& job, const QImage& image) {
    std::vector<ThumbnailCompletion> waiters;
    QImage accepted;
    {
        std::lock_guard publication(publication_mutex_);
        std::lock_guard lock(mutex_);
        if (job.generation == generation_) {
            accepted = image;
            if (!accepted.isNull())
                MemoryInsert(job.key, accepted);
        }
        const auto pending = pending_.find(job.key);
        if (pending != pending_.end() && pending->second.job.generation == job.generation) {
            for (auto& [ticket, completion] : pending->second.waiters) {
                tickets_.erase(ticket);
                waiters.push_back(std::move(completion));
            }
            pending_.erase(pending);
        }
    }
    for (auto& completion : waiters)
        completion(accepted);
}

bool ThumbnailRequestBroker::Publish(const Job& job, const QImage& image, std::int64_t decoded_pts_us) {
    std::lock_guard publication(publication_mutex_);
    {
        std::lock_guard lock(mutex_);
        if (job.generation != generation_)
            return false;
    }
    return disk_ && disk_->Store(job.key, image, decoded_pts_us);
}

std::uint64_t ThumbnailRequestBroker::Generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
}

std::optional<QImage> ThumbnailRequestBroker::MemoryLookup(const std::string& key) {
    std::lock_guard lock(memory_mutex_);
    const auto found = memory_.find(key);
    if (found == memory_.end())
        return std::nullopt;
    memory_lru_.splice(memory_lru_.end(), memory_lru_, found->second.lru);
    memory_hits_.fetch_add(1, std::memory_order_relaxed);
    return found->second.image;
}

void ThumbnailRequestBroker::MemoryInsert(const std::string& key, const QImage& image) {
    const auto bytes = static_cast<std::size_t>(std::max<qsizetype>(0, image.sizeInBytes()));
    if (bytes == 0 || bytes > config_.memory_budget_bytes)
        return;
    std::lock_guard lock(memory_mutex_);
    if (const auto existing = memory_.find(key); existing != memory_.end()) {
        memory_bytes_ -= existing->second.bytes;
        memory_lru_.erase(existing->second.lru);
        memory_.erase(existing);
    }
    while (!memory_lru_.empty() && memory_bytes_ + bytes > config_.memory_budget_bytes) {
        const auto oldest = memory_.find(memory_lru_.front());
        memory_bytes_ -= oldest->second.bytes;
        memory_.erase(oldest);
        memory_lru_.pop_front();
    }
    memory_lru_.push_back(key);
    // Deep copy: a decoded tile may borrow the decoder's frame buffer.
    memory_.emplace(key, MemoryEntry{image.copy(), std::prev(memory_lru_.end()), bytes});
    memory_bytes_ += bytes;
}

void ThumbnailRequestBroker::CountDiskHit() {
    disk_hits_.fetch_add(1, std::memory_order_relaxed);
}

void ThumbnailRequestBroker::CountDecode() {
    decodes_.fetch_add(1, std::memory_order_relaxed);
}

ThumbnailBrokerStats ThumbnailRequestBroker::Stats() const {
    return ThumbnailBrokerStats{memory_hits_.load(), disk_hits_.load(), decodes_.load()};
}

void ThumbnailRequestBroker::Wake() {
    std::lock_guard wake_lock(wake_mutex_);
    if (wake_)
        wake_();
}

} // namespace exosnap

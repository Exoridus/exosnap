#pragma once

// Persistent timeline thumbnails and the request broker that serves them.
//
// A thumbnail is identified by the validated content identity of its source,
// the source time it was requested for, its pixel height, its scaling variant
// and the color-conversion version. Never by path or clip: every clip over one
// source shares the same tiles, and moving, reopening, resizing or scrolling
// back finds them again.
//
// Tiers, fastest first: a bounded in-memory LRU of decoded images, the disk
// store (MediaCacheStore under CacheLocation/thumbnails, 1 GiB LRU), then a
// decode on the single thumbnail worker (TimelineThumbnailSource). The broker
// owns the first two tiers and the request queue; the worker drains it.

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/media_cache_store.h>

#include <QImage>
#include <QString>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace exosnap {

inline constexpr std::uint32_t kThumbnailSchemaVersion = 1;
// Bump whenever the decoder's BGRA conversion or the scaling changes pixels.
inline constexpr std::uint32_t kThumbnailColorConversionVersion = 1;

enum class ThumbnailVariant : std::uint32_t {
    // Aspect kept, scaled to exactly the requested height, smooth filtering.
    AspectFitHeight = 1,
};

// Stable 64-character hex key.
[[nodiscard]] std::string ThumbnailCacheKey(const std::array<std::uint8_t, 32>& content_sha256,
                                            std::int64_t source_time_us, int height,
                                            ThumbnailVariant variant = ThumbnailVariant::AspectFitHeight,
                                            std::uint32_t color_conversion_version = kThumbnailColorConversionVersion);

[[nodiscard]] std::optional<std::array<std::uint8_t, 32>> ParseContentIdentity(const QString& hex);

// ---- Tile layout for one clip window (pure) ----

struct ThumbnailTile {
    std::int64_t time_us = 0; // source time the tile decodes
    double x = 0;             // left edge in item pixels, relative to the clip's source-in
    double width = 0;         // pixels until the next tile
};

// Tiles covering [source_in_us, source_out_us) drawn across `width_px`.
//
// Tile times lie on a grid anchored at source time zero whose period is a
// power-of-two number of source frames, the smallest one at least as wide as a
// tile of `height_px`. So the grid only depends on the source and the zoom
// octave: two clips of one source, a pan or a reorder ask for the same times,
// and zooming in by an octave reuses every other tile. Without a known frame
// rate the grid unit is one millisecond.
[[nodiscard]] std::vector<ThumbnailTile> ThumbnailTilesForWindow(std::int64_t source_in_us, std::int64_t source_out_us,
                                                                 double width_px, int height_px, int video_width,
                                                                 int video_height, int fps_num, int fps_den);

// The grid times within [from_us, to_us) of the same grid, at most `limit`.
[[nodiscard]] std::vector<std::int64_t> ThumbnailGridTimes(std::int64_t from_us, std::int64_t to_us,
                                                           double us_per_pixel, int height_px, int video_width,
                                                           int video_height, int fps_num, int fps_den,
                                                           std::size_t limit);

// ---- Disk tier ----

class ThumbnailDiskCache {
  public:
    static constexpr std::uint64_t kDefaultBudgetBytes = 1024ull * 1024 * 1024;

    // Empty root selects QStandardPaths::CacheLocation + "/thumbnails".
    explicit ThumbnailDiskCache(std::filesystem::path root = {}, std::uint64_t budget_bytes = kDefaultBudgetBytes);

    [[nodiscard]] std::optional<QImage> Load(const std::string& key);
    bool Store(const std::string& key, const QImage& image, std::int64_t decoded_pts_us,
               std::optional<std::uint64_t> expected_epoch = {});
    std::uint64_t Clear();
    std::uint64_t Retire();
    [[nodiscard]] engine::MediaCacheStore& store() noexcept {
        return store_;
    }

  private:
    engine::MediaCacheStore store_;
};

// ---- Broker ----

struct ThumbnailSourceInfo {
    std::filesystem::path path;
    std::array<std::uint8_t, 32> content_sha256{};
    int video_width = 0;
    int video_height = 0;
    int fps_num = 0;
    int fps_den = 1;
    std::shared_ptr<const engine::MediaReadCapability> access;
    std::shared_ptr<const engine::VerifiedMediaIdentity> proof;
    // A lease-scoped identity may serve provisional pixels from memory only.
    bool persistent = true;
};

// Called exactly once per request. A null image means unavailable (unknown
// source, decode failure, cancellation or shutdown).
using ThumbnailCompletion = std::function<void(const QImage&)>;

struct ThumbnailBrokerStats {
    std::uint64_t memory_hits = 0;
    std::uint64_t disk_hits = 0;
    std::uint64_t decodes = 0;
    std::size_t resident_bytes = 0;
    std::size_t resident_peak_bytes = 0;
    std::size_t pending_jobs = 0;
    std::size_t clip_interests = 0;
};

class ThumbnailRequestBroker {
  public:
    static constexpr std::size_t kDefaultMemoryBudgetBytes = 32u * 1024 * 1024;
    static constexpr std::size_t kMaxPrefetchJobs = 128;

    struct Config {
        bool disk_enabled = true;
        std::filesystem::path disk_root; // empty: CacheLocation/thumbnails
        std::uint64_t disk_budget_bytes = ThumbnailDiskCache::kDefaultBudgetBytes;
        std::size_t memory_budget_bytes = kDefaultMemoryBudgetBytes;
        // Empty uses filesystem equivalence. A supplied comparator must be thread-safe.
        std::function<bool(const std::filesystem::path&, const std::filesystem::path&)> equivalent_files;
    };

    struct Job {
        std::string key;
        std::string identity;
        std::int64_t time_us = 0;
        int height = 0;
        bool disk_checked = false;
        std::uint64_t generation = 0;
        int priority = 3;
    };

    ThumbnailRequestBroker();
    explicit ThumbnailRequestBroker(Config config);
    ~ThumbnailRequestBroker();

    ThumbnailRequestBroker(const ThumbnailRequestBroker&) = delete;
    ThumbnailRequestBroker& operator=(const ThumbnailRequestBroker&) = delete;

    void RegisterSource(const std::string& identity_hex, ThumbnailSourceInfo info);
    void ReleaseSources();
    void RemoveSource(const std::string& identity);
    [[nodiscard]] std::optional<ThumbnailSourceInfo> Source(const std::string& identity_hex) const;
    [[nodiscard]] std::optional<std::string> IdentityForPath(const std::filesystem::path& path) const;
    // Returns one registration snapshot. Source bytes still require open-time validation.
    // Alias probing runs outside broker locks.
    [[nodiscard]] std::optional<ThumbnailSourceInfo> SourceForPath(const std::filesystem::path& path) const;

    // Thread-safe. A memory hit completes inline; anything else is queued for
    // the worker. Returns a ticket for Cancel (0 when already completed).
    std::uint64_t Request(const std::string& identity_hex, std::int64_t time_us, int height,
                          ThumbnailCompletion completion);
    // Completes the ticket with a null image if it has not completed yet.
    void Cancel(std::uint64_t ticket);
    // Queues low-priority decodes without a consumer. Replaces earlier
    // prefetches of the same source; bounded by kMaxPrefetchJobs.
    void Prefetch(const std::string& identity_hex, const std::vector<std::int64_t>& times_us, int height);

    struct InterestTile {
        std::int64_t time_us = 0;
        int height = 48;
        int priority = 3;
        std::int64_t center_distance_us = 0;
    };
    // Replaces one visible clip's interest without withdrawing split siblings.
    // Priorities are 1 (boundaries/playhead) through 5 (idle backfill).
    void ReplaceInterest(std::uint64_t clip_interest_id, const std::string& identity_hex,
                         const std::vector<InterestTile>& tiles, std::uint64_t view_generation);
    void RemoveInterest(std::uint64_t clip_interest_id);
    struct VisibleCoverage {
        int expected = 0;
        int ready = 0;
        int unavailable = 0;
    };
    // Counts unique current visible keys. Lookahead and retired sources are excluded.
    [[nodiscard]] VisibleCoverage Coverage() const;

    void SetForegroundBusy(bool busy);
    void SetAnalysisPressure(bool playback, bool exclusive);
    [[nodiscard]] bool ForegroundBusy() const;

    // Drops both cache tiers and retires pending requests as unavailable.
    // Sources stay registered. Callbacks run outside all broker locks.
    // Returns the number of disk bytes released.
    std::uint64_t Clear();

    // Harness seam (--visual-test): every request is answered inline by
    // `images` instead of the worker, the disk tier and a decoder, and nothing
    // is prefetched. Install before the first request; the function is then
    // called from whichever thread asks.
    using SyntheticImages = std::function<QImage(const ThumbnailSourceInfo& source, std::int64_t time_us, int height)>;
    void UseSyntheticImagesForHarness(SyntheticImages images);

    // ---- Worker side ----
    // The worker installs a wake function; it is called with no broker lock
    // held whenever HasWork() may have become true. Detaching (empty function)
    // completes every outstanding request with a null image and refuses new
    // ones, so no QML image response can wait forever.
    void AttachWorker(std::function<void()> wake, std::function<void()> memory_released = {});
    void DetachWorker();
    [[nodiscard]] bool HasWork() const;
    [[nodiscard]] bool HasMemoryPressure() const;
    // Eligible visible requests precede prefetches. Foreground work withholds
    // decoding while cache lookups remain available.
    [[nodiscard]] std::optional<Job> TakeJob();
    // Parks a job that missed both caches until decoding is admitted.
    void DeferDecode(Job job);
    void Complete(const Job& job, const QImage& image);
    bool Publish(const Job& job, const QImage& image, std::int64_t decoded_pts_us);
    [[nodiscard]] std::uint64_t Generation() const;

    [[nodiscard]] std::optional<QImage> MemoryLookup(const std::string& key);
    QImage MemoryInsert(const std::string& key, const QImage& image);
    [[nodiscard]] std::size_t ResidentBytes() const;
    [[nodiscard]] ThumbnailDiskCache* Disk() noexcept {
        return disk_.get();
    }
    void CountDiskHit();
    void CountDecode();
    [[nodiscard]] ThumbnailBrokerStats Stats() const;

  private:
    struct SourceRegistration {
        std::string identity;
        ThumbnailSourceInfo info;
    };
    std::optional<SourceRegistration> ResolveSource(const std::filesystem::path& path) const;
    struct Pending {
        Job job;
        std::vector<std::pair<std::uint64_t, ThumbnailCompletion>> waiters;
        bool prefetch = false;
        bool in_flight = false;
        std::uint64_t sequence = 0;
        std::map<std::uint64_t, std::pair<int, std::int64_t>> interests;
        std::size_t required_bytes = 0;
    };
    enum class VisibleResult { Pending, Ready, Unavailable };
    struct Interest {
        std::string identity;
        std::uint64_t view_generation = 0;
        std::vector<std::string> keys;
        // Completed results survive relevance-only updates while the interest remains alive.
        std::map<std::string, VisibleResult> visible_results;
    };
    struct MemoryEntry {
        QImage image;
        std::list<std::string>::iterator lru{};
        std::size_t bytes = 0;
    };

    // Requires publication_mutex_ and mutex_ so retired work cannot consume a later epoch.
    std::vector<ThumbnailCompletion> RetireRequestsLocked();
    void Wake();
    std::optional<Job> TakeFrom(std::deque<std::string>& order);
    void RemoveInterestLocked(std::uint64_t clip_interest_id);
    [[nodiscard]] int Priority(const Pending& pending) const;
    [[nodiscard]] std::int64_t CenterDistance(const Pending& pending) const;
    [[nodiscard]] bool CanFitAfterEviction(std::size_t bytes) const;

    Config config_;
    SyntheticImages synthetic_images_;
    std::unique_ptr<ThumbnailDiskCache> disk_;
    // Guards wake_ and is held while it runs, so DetachWorker() returns only
    // after any wake in progress finished.
    std::mutex wake_mutex_;
    std::function<void()> wake_;
    mutable std::mutex mutex_;
    bool attached_ = false;
    bool busy_ = false;
    bool exclusive_ = false;
    std::uint64_t generation_ = 0;
    std::mutex publication_mutex_;
    std::uint64_t next_ticket_ = 0;
    std::uint64_t next_sequence_ = 0;
    std::map<std::uint64_t, Interest> interests_;
    std::map<std::pair<int, std::string>, std::uint64_t> fairness_;
    std::map<std::string, std::int64_t> last_source_time_;
    std::unordered_map<std::string, ThumbnailSourceInfo> sources_;
    std::map<std::string, Pending> pending_;
    std::deque<std::string> visible_order_;
    std::deque<std::string> prefetch_order_;
    std::deque<std::string> decode_order_;
    std::unordered_map<std::uint64_t, std::string> tickets_;

    mutable std::mutex memory_mutex_;
    struct ResidentBudget;
    std::shared_ptr<ResidentBudget> resident_;
    std::unordered_map<std::string, MemoryEntry> memory_;
    std::list<std::string> memory_lru_;
    std::size_t memory_bytes_ = 0;

    std::atomic<std::uint64_t> memory_hits_{0};
    std::atomic<std::uint64_t> disk_hits_{0};
    std::atomic<std::uint64_t> decodes_{0};
};

} // namespace exosnap

#pragma once

// Background waveform analysis and the persistent peak cache behind the Edit
// timeline's audio rows.
//
// One worker thread owns at most one audio decoder at a time and analyses one
// stream at a time in source-time chunks. Requests deduplicate by cache key and
// run in arrival order. While foreground work is busy (playback, export,
// recording) analysis pauses between chunks; reading already cached peaks is
// not paused, because a draw needs only a few kilobytes from disk.
//
// Completed analyses are published atomically into the disk store. Peaks of a
// stream still being analysed are readable from memory as soon as each chunk
// completes. Disk-backed peaks are read in fixed blocks through a bounded LRU,
// so no waveform is ever loaded into memory as a whole.
//
// A missing source never makes the service claim media availability: a cached
// waveform can still be drawn, while sourceAvailable() reports the source
// separately.
//
// Create and use on the GUI thread. Peak reads (readBlock and the layout
// queries) are thread-safe so a scene-graph item may call them from the render
// thread while the GUI thread is blocked.

#include <exosnap/engine/media_cache_store.h>
#include <exosnap/engine/waveform_cache.h>

#include <QObject>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

namespace exosnap {

// Buckets per block of the in-memory LRU and of each disk read.
inline constexpr std::uint32_t kWaveformBlockBuckets = 2048;

class WaveformCacheService : public QObject {
    Q_OBJECT
  public:
    static constexpr std::uint64_t kDefaultDiskBudgetBytes = 512ull * 1024 * 1024;
    static constexpr std::size_t kDefaultMemoryBudgetBytes = 32u * 1024 * 1024;

    struct Config {
        // Empty selects QStandardPaths::CacheLocation + "/waveforms".
        std::filesystem::path root;
        std::uint64_t disk_budget_bytes = kDefaultDiskBudgetBytes;
        std::size_t memory_budget_bytes = kDefaultMemoryBudgetBytes;
        std::size_t analysis_budget_bytes = 64u * 1024 * 1024;
        // Source-time chunk per decode call, in 48 kHz frames.
        std::uint32_t chunk_frames = engine::kWaveformAnalysisChunkFrames;
    };

    using DecoderFactory = std::function<std::unique_ptr<engine::WaveformAudioDecoder>(
        const std::filesystem::path& path, std::uint32_t container_index, std::string& error,
        const std::atomic<bool>* cancelled)>;

    enum class State { Unknown, Queued, Analyzing, Ready, Failed };
    Q_ENUM(State)

    explicit WaveformCacheService(QObject* parent = nullptr);
    WaveformCacheService(Config config, DecoderFactory decoder_factory, QObject* parent = nullptr);
    ~WaveformCacheService() override;

    WaveformCacheService(const WaveformCacheService&) = delete;
    WaveformCacheService& operator=(const WaveformCacheService&) = delete;

    // Requests peaks for one stream and returns its cache key. `source_end_us`
    // is the media-local end of the stream (container start plus duration); it
    // fixes the analysed sample count. A cached waveform is served without
    // opening the source. Repeating a request for a key that is queued,
    // analysing or ready changes nothing.
    QString request(const engine::WaveformSourceKey& source, const QString& source_path, qint64 source_end_us);

    // Drops a queued request or abandons the running analysis of `key`.
    // Nothing partial is ever published.
    Q_INVOKABLE void cancel(const QString& key);

    // While true, analysis pauses between chunks.
    Q_INVOKABLE void setForegroundBusy(bool busy);
    [[nodiscard]] bool foregroundBusy() const;

    // Cancels all work, deletes every persisted waveform and drops the memory
    // tier. Returns the number of disk bytes released.
    Q_INVOKABLE qint64 clearCache();

    [[nodiscard]] State state(const QString& key) const;
    // False once the worker found the source file missing or unreadable.
    // Unknown sources report true.
    [[nodiscard]] Q_INVOKABLE bool sourceAvailable(const QString& key) const;
    [[nodiscard]] Q_INVOKABLE qint64 completedSamples(const QString& key) const;
    [[nodiscard]] Q_INVOKABLE qint64 totalSamples(const QString& key) const;

    // ---- Peak reads (any thread) ----

    // Levels of the pyramid, or 0 while unknown.
    [[nodiscard]] std::uint32_t levelCount(const QString& key) const;

    // Copies the readable buckets of block `block` of `level` into `out`
    // (kWaveformChannels peaks per bucket) and returns how many leading buckets
    // are valid. A block not yet in memory returns 0 and is loaded in the
    // background; blocksLoaded(key) follows.
    std::uint32_t readBlock(const QString& key, std::uint32_t level, std::uint64_t block,
                            std::vector<engine::WaveformPeak>& out);

    // ---- Test observation ----
    [[nodiscard]] std::uint64_t decoderOpensForTest() const;
    [[nodiscard]] std::uint64_t decodeCallsForTest() const;
    [[nodiscard]] std::size_t memoryBytesForTest() const;
    [[nodiscard]] engine::MediaCacheStore& storeForTest();

  signals:
    void progress(const QString& key, qint64 completedSamples, qint64 totalSamples);
    void ready(const QString& key);
    void failed(const QString& key, const QString& reason);
    void blocksLoaded(const QString& key);
    void sourceAvailabilityChanged(const QString& key, bool available);

  private:
    struct Job {
        std::string key;
        std::filesystem::path path;
        std::uint32_t container_index = 0;
        std::uint64_t sample_count = 0;
    };
    struct Status {
        State state = State::Unknown;
        bool source_available = true;
        std::uint64_t total_samples = 0;
        std::uint64_t completed_samples = 0;
    };
    struct BlockId {
        std::string key;
        std::uint32_t level = 0;
        std::uint64_t block = 0;
        auto operator<=>(const BlockId&) const = default;
    };
    struct CachedBlock {
        std::vector<engine::WaveformPeak> peaks;
        std::list<BlockId>::iterator lru{};
    };

    void workerLoop();
    void runJob(const Job& job);
    void loadPendingBlocks();
    bool setStatus(const std::string& key, const Status& status, std::optional<std::uint64_t> generation = {});
    void postProgress(const std::string& key, std::uint64_t completed, std::uint64_t total);
    void insertBlock(const BlockId& id, std::vector<engine::WaveformPeak> peaks);
    std::optional<engine::WaveformFileLayout> layoutFor(const std::string& key);

    Config config_;
    DecoderFactory decoder_factory_;
    std::unique_ptr<engine::MediaCacheStore> store_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool busy_ = false;
    std::deque<Job> queue_;
    std::optional<std::string> running_;
    std::atomic<bool> cancel_running_{false};
    std::unordered_map<std::string, Status> status_;
    // The running analysis; readable while it grows.
    std::shared_ptr<engine::WaveformPyramidBuilder> in_progress_;
    std::string in_progress_key_;
    // Disk layouts of published waveforms, bounded by kMaxLayouts.
    std::map<std::string, engine::WaveformFileLayout> layouts_;
    std::set<BlockId> pending_blocks_;
    std::map<BlockId, CachedBlock> blocks_;
    std::list<BlockId> block_lru_;
    std::size_t block_bytes_ = 0;
    std::set<BlockId> touched_blocks_;
    // Bumped by clearCache(); work started under an older generation is discarded.
    std::uint64_t generation_ = 0;
    // Serializes publication with clearCache().
    std::mutex publish_mutex_;
    std::uint64_t decoder_opens_ = 0;
    std::atomic<std::uint64_t> decode_calls_{0};

    std::thread worker_;
};

} // namespace exosnap

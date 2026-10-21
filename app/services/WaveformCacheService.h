#pragma once

// Background waveform analysis and the persistent peak cache behind the Edit
// timeline's audio rows.
//
// The overview worker rotates between source-time chunks from visible streams.
// A separate detail worker owns an independent decoder/cursor. Requests deduplicate by cache key.
// During playback, visible overviews receive at most one chunk per 50 ms.
// Recording and export suspend optional analysis. Display reads use only immutable
// memory blocks; the worker loads missing disk blocks independently.
//
// Completed analyses are published atomically into the disk store. Peaks of a
// stream still being analysed are published to memory after each chunk.
// Disk-backed peaks are read by the worker through a bounded LRU,
// so no waveform is ever loaded into memory as a whole.
//
// A missing source never makes the service claim media availability: a cached
// waveform can still be drawn, while sourceAvailable() reports the source
// separately.
//
// Create and use on the GUI thread. Peak reads (readBlock and the layout
// queries) are thread-safe so a scene-graph item may call them from the render
// thread while the GUI thread is blocked.

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/media_cache_store.h>
#include <exosnap/engine/waveform_cache.h>

#include <QObject>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <chrono>
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
inline constexpr std::uint32_t kWaveformBlockBuckets = 256;

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
        // Hierarchy, normalized chunk and hash buffers. Decoder allocations
        // have separate block bounds and are excluded from this budget.
        std::size_t analysis_budget_bytes = 64u * 1024 * 1024;
        // Source-time chunk per decode call, in 48 kHz frames.
        std::uint32_t chunk_frames = engine::kWaveformAnalysisChunkFrames;
    };

    // May be called concurrently for independent overview and detail cursors.
    using DecoderFactory = std::function<std::unique_ptr<engine::WaveformAudioDecoder>(
        const std::filesystem::path& path, std::uint32_t container_index, std::string& error,
        const std::atomic<bool>* cancelled, const std::array<std::uint8_t, 32>& expected_sha256)>;

    enum class State { Unknown, Queued, Analyzing, Ready, Failed };
    Q_ENUM(State)

    struct PeakBlock {
        std::vector<engine::WaveformPeak> peaks;
        std::uint64_t first_sample = 0;
        std::uint64_t end_sample = 0;
        std::uint64_t generation = 0;
    };

    // NoCoverage has no verified samples. MemoryPressure permits a later retry.
    // Unavailable denotes an absent or retired ticket.
    enum class DetailState { Pending, Ready, NoCoverage, MemoryPressure, Unavailable, Failed };
    // Interleaved normalized 48 kHz stereo. Only coverage spans establish
    // actual source samples; zeros outside them are padding.
    struct PcmWindow {
        std::string source_key;
        std::int64_t first_sample = 0;
        std::vector<float> samples;
        std::vector<engine::AudioSampleSpan> coverage;
        std::uint64_t generation = 0;
    };
    struct PcmSnapshot {
        DetailState state = DetailState::Unavailable;
        std::shared_ptr<const PcmWindow> window;
        QString reason;
    };
    static constexpr std::size_t kMaxPcmWindowBytes = 4u * 1024 * 1024;
    static constexpr std::size_t kMaxPcmBytes = 16u * 1024 * 1024;

    explicit WaveformCacheService(QObject* parent = nullptr);
    WaveformCacheService(Config config, DecoderFactory decoder_factory, QObject* parent = nullptr);
    ~WaveformCacheService() override;

    WaveformCacheService(const WaveformCacheService&) = delete;
    WaveformCacheService& operator=(const WaveformCacheService&) = delete;

    // Requests peaks for one stream and returns its cache key. `source_end_us`
    // is the media-local end of the stream (container start plus duration); it
    // fixes the analysed sample count. A cached waveform is served without
    // opening the source. Queued or analysing requests deduplicate. Repeating a
    // ready request checks source availability in the background without decode.
    QString request(const engine::WaveformSourceKey& source, const QString& source_path, qint64 source_end_us);
    // Associates a source or lease-scoped preview identity with its held file.
    // Only a matching proof permits persistent waveform publication.
    void registerSource(const std::array<std::uint8_t, 32>& identity,
                        std::shared_ptr<const engine::MediaReadCapability> access,
                        std::shared_ptr<const engine::VerifiedMediaIdentity> proof = {});
    void releaseSources();
    void unregisterSource(const std::array<std::uint8_t, 32>& identity);
    void setVisibleInterest(std::uint64_t clip, const QString& key, qint64 distance_us = 0);
    void removeVisibleInterest(std::uint64_t clip);

    // Drops a queued request or abandons the running analysis of `key`.
    // Nothing partial is ever published.
    Q_INVOKABLE void cancel(const QString& key);

    // While true, analysis pauses between chunks.
    Q_INVOKABLE void setForegroundBusy(bool busy);
    void setAnalysisPressure(bool playback, bool exclusive);
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

    // Returns immutable memory only. A miss schedules worker interest without
    // storage access. Retained blocks count against the budget even after
    // eviction or clear; pressure leaves new data unavailable.
    [[nodiscard]] std::shared_ptr<const PeakBlock> tryReadPublishedBlock(const QString& key, std::uint32_t level,
                                                                         std::uint64_t block);

    // Copies the readable buckets of block `block` of `level` into `out`
    // (kWaveformChannels peaks per bucket) and returns how many leading buckets
    // are valid. A block not yet in memory returns 0 and is loaded in the
    // background; blocksLoaded(key) follows.
    std::uint32_t readBlock(const QString& key, std::uint32_t level, std::uint64_t block,
                            std::vector<engine::WaveformPeak>& out);

    // Subscribes to immutable normalized 48 kHz stereo samples. Content deduplicates
    // independently of view_generation. A zero ticket means no interest was admitted.
    // All source validation/decode runs on a cursor independent of peak analysis.
    [[nodiscard]] std::uint64_t requestPcmWindow(const QString& key, std::int64_t first_sample,
                                                 std::uint32_t sample_count, std::uint64_t view_generation);
    // Memory-only snapshot. Read immediately after subscribing, including on a
    // warm hit; later transitions emit pcmWindowChanged for the active ticket.
    [[nodiscard]] PcmSnapshot pcmWindow(std::uint64_t ticket) const;
    // Retires one view interest. Existing snapshots stay valid and budgeted.
    void cancelPcmWindow(std::uint64_t ticket);

    // ---- Test observation ----
    [[nodiscard]] std::uint64_t decoderOpensForTest() const;
    [[nodiscard]] std::uint64_t decodeCallsForTest() const;
    [[nodiscard]] std::size_t memoryBytesForTest() const;
    [[nodiscard]] std::size_t pcmMemoryBytesForTest() const;
    [[nodiscard]] std::size_t analysisMemoryBytesForTest() const;
    [[nodiscard]] std::uint64_t decoderSeeksForTest() const;
    [[nodiscard]] std::uint64_t decodedFramesForTest() const;
    [[nodiscard]] engine::MediaCacheStore& storeForTest();

  signals:
    void progress(const QString& key, qint64 completedSamples, qint64 totalSamples);
    void ready(const QString& key);
    void failed(const QString& key, const QString& reason);
    void blocksLoaded(const QString& key);
    void sourceAvailabilityChanged(const QString& key, bool available);
    void pcmWindowChanged(quint64 ticket, quint64 viewGeneration);
    // Tickets for this source are retired. An empty key retires all sources.
    void pcmInterestsInvalidated(const QString& key);

  private:
    struct Job {
        std::string key;
        std::filesystem::path path;
        std::uint32_t container_index = 0;
        std::uint64_t sample_count = 0;
        std::array<std::uint8_t, 32> expected_sha256{};
        std::shared_ptr<const engine::MediaReadCapability> access;
        std::shared_ptr<const engine::VerifiedMediaIdentity> proof;
        bool persistent = true;
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
        std::shared_ptr<const PeakBlock> data;
        std::list<BlockId>::iterator lru{};
    };
    struct PcmId {
        std::string key;
        std::int64_t first = 0;
        std::uint32_t count = 0;
        auto operator<=>(const PcmId&) const = default;
    };
    struct PcmEntry {
        DetailState state = DetailState::Pending;
        std::shared_ptr<const PcmWindow> window;
        QString reason;
        std::uint64_t job_id = 0;
        std::size_t subscribers = 0;
    };
    struct PcmInterest {
        PcmId id;
        std::uint64_t view_generation = 0;
    };
    struct PcmJob {
        PcmId id;
        Job source;
        std::uint64_t generation = 0;
        std::uint64_t job_id = 0;
    };
    struct PcmCursor {
        std::string key;
        std::filesystem::path path;
        std::unique_ptr<engine::WaveformAudioDecoder> decoder;
    };
    struct PcmBudget {
        std::shared_ptr<std::atomic<std::size_t>> total;
        std::shared_ptr<std::atomic<std::size_t>> pcm;
        std::size_t bytes = 0;
        ~PcmBudget() {
            if (bytes > 0) {
                total->fetch_sub(bytes, std::memory_order_relaxed);
                pcm->fetch_sub(bytes, std::memory_order_relaxed);
            }
        }
    };
    struct PcmAllocation {
        std::shared_ptr<PcmWindow> window;
        std::shared_ptr<PcmBudget> scratch;
    };

    void workerLoop();
    void checkAvailability(const Job& job, std::uint64_t generation);
    void pcmWorkerLoop();
    void runPcmJob(const PcmJob& job, PcmCursor& cursor);
    bool pcmInterested(const PcmId& id) const;
    PcmAllocation reservePcmWindow(const PcmJob& job);
    bool evictInactivePcmWindow();
    void finishPcmJob(const PcmJob& job, PcmSnapshot snapshot, bool cancelled);
    void runJob(const Job& job);
    void discardOverview(const std::string& key);
    void purgeOverviews();
    void removeVisibleInterestLocked(std::uint64_t clip);
    std::unique_ptr<engine::WaveformAudioDecoder> openDecoder(const Job& job, std::string& error,
                                                              const std::atomic<bool>* cancelled);
    void loadPendingBlocks();
    bool setStatus(const std::string& key, const Status& status, std::optional<std::uint64_t> generation = {});
    void postProgress(const std::string& key, std::uint64_t completed, std::uint64_t total, std::uint64_t generation);
    void insertBlock(const BlockId& id, std::vector<engine::WaveformPeak> peaks, std::uint64_t generation,
                     std::uint64_t valid_samples);
    void publishProgressBlocks(const Job& job, engine::WaveformStreamWriter& builder, std::uint64_t previous,
                               std::uint64_t completed, std::uint64_t generation, bool finished = false);
    std::optional<engine::WaveformFileLayout> layoutFor(const std::string& key);

    Config config_;
    DecoderFactory decoder_factory_;
    bool custom_decoder_factory_ = false;
    std::unique_ptr<engine::MediaCacheStore> store_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool busy_ = false;
    bool playback_ = false;
    bool exclusive_ = false;
    std::chrono::steady_clock::time_point next_overview_;
    std::deque<Job> queue_;
    std::map<std::string, Job> availability_checks_;
    // Only explicitly requested sources may regenerate an evicted entry.
    // The recipes are bounded by the same limit as disk layouts.
    std::map<std::string, Job> requested_jobs_;
    struct SourceAccess {
        std::shared_ptr<const engine::MediaReadCapability> access;
        std::shared_ptr<const engine::VerifiedMediaIdentity> proof;
    };
    std::map<std::array<std::uint8_t, 32>, SourceAccess> source_access_;
    std::map<std::string, std::shared_ptr<engine::WaveformStreamWriter>> provisional_writers_;
    std::size_t provisional_working_bytes_ = 0;
    std::deque<std::shared_ptr<engine::WaveformStreamWriter>> retired_writers_;
    bool release_pcm_cursor_ = false;
    struct OverviewCursor;
    std::map<std::string, std::unique_ptr<OverviewCursor>> overview_cursors_;
    std::map<std::string, std::chrono::steady_clock::time_point> deferred_overviews_;
    std::size_t overview_working_bytes_ = 0;
    std::uint64_t overview_sequence_ = 0;
    bool cleanup_overviews_ = false;
    struct VisibleInterest {
        std::string key;
        qint64 distance_us = 0;
    };
    std::map<std::uint64_t, VisibleInterest> visible_interests_;
    std::map<std::string, std::set<std::pair<qint64, std::uint64_t>>> interest_distances_;
    std::optional<std::string> running_;
    std::atomic<bool> cancel_running_{false};
    std::unordered_map<std::string, Status> status_;
    // The running analysis; completed blocks are readable from staging.
    std::shared_ptr<engine::WaveformStreamWriter> in_progress_;
    std::string in_progress_key_;
    // Disk layouts of published waveforms, bounded by kMaxLayouts.
    std::map<std::string, engine::WaveformFileLayout> layouts_;
    std::set<BlockId> pending_blocks_;
    std::map<BlockId, CachedBlock> blocks_;
    std::list<BlockId> block_lru_;
    std::shared_ptr<std::atomic<std::size_t>> block_bytes_ = std::make_shared<std::atomic<std::size_t>>(0);
    std::shared_ptr<std::atomic<std::size_t>> pcm_bytes_ = std::make_shared<std::atomic<std::size_t>>(0);
    std::map<PcmId, PcmEntry> pcm_entries_;
    std::map<std::uint64_t, PcmInterest> pcm_interests_;
    std::deque<PcmJob> pcm_queue_;
    std::optional<PcmId> running_pcm_;
    std::list<PcmId> pcm_lru_;
    std::uint64_t next_pcm_ticket_ = 0;
    std::uint64_t next_pcm_job_ = 0;
    std::atomic<bool> cancel_pcm_{false};
    // Bumped by clearCache(); work started under an older generation is discarded.
    std::uint64_t generation_ = 0;
    std::size_t analysis_peak_bytes_ = 0;
    std::uint64_t decoder_seeks_ = 0;
    std::uint64_t decoded_frames_ = 0;
    std::uint64_t decoder_opens_ = 0;
    std::atomic<std::uint64_t> decode_calls_{0};

    std::thread worker_;
    std::thread pcm_worker_;
};

} // namespace exosnap

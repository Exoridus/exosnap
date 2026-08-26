#pragma once

// TimelineThumbnailSource -- decodes the Edit timeline's tile strip
// (docs/superpowers/specs/2026-08-02-timeline-thumbnails-multitrack-design.md).
//
// The strip is the timeline's only real information about the clip, so every
// tile is a frame the file actually carries. Decoding happens on a worker
// thread with its own EditPlayerEngine: the engine keeps no global state and
// owns no audio device, so a second instance alongside playback is safe. The
// player *session* is not -- it opens its own WASAPI renderer.
//
// The layout maths and the tile loop are free functions rather than members so
// they can be exercised without a clip, a thread, or a decoder.

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <exosnap/engine/edit_player_engine.h>

namespace exosnap {

// One finished tile: a frame already scaled to row height, plus the clip time
// it stands for. Tiles are drawn left-aligned at that time -- a tile answers
// "from here on it looks like this", which is the question that matters while
// trimming.
struct TimelineThumbnail {
    qint64 time_ms = 0;
    QImage image;
};

// ---- Strip layout (pure) ----

// Width of one tile on a row of `row_height` px, from the recording's own frame
// size. An unknown size falls back to 16:9 rather than to a square -- that is
// what the shipped defaults record, and a square tile would misplace every
// following tile's left edge.
[[nodiscard]] int TimelineTileWidth(int row_height, int video_width, int video_height);

// How many whole tiles of `tile_width` fit across `track_width`. A partial last
// tile is not drawn: clipped content reads as a decode that went wrong.
[[nodiscard]] int TimelineTileCount(int track_width, int tile_width);

// The clip times `tile_count` tiles stand for: spread evenly across the track,
// then snapped to the nearest keyframe so each tile shows a frame that exists.
// Positions collapsing onto the same keyframe are merged -- drawing the same
// frame twice would claim detail the clip does not have.
[[nodiscard]] std::vector<qint64> TimelineTileTimesMs(int tile_count, qint64 duration_ms,
                                                      const std::vector<int64_t>& keyframes_us);

// ---- Tile generation (pure; the caller supplies the decoder) ----

// Decodes the frame at `target_us`, or nullopt when the file carries nothing
// decodable there.
using TimelineFrameDecoder = std::function<std::optional<exosnap::engine::DecodedVideoFrame>(int64_t target_us)>;

// Wraps a decoded frame in a QImage WITHOUT copying it: the image holds a
// reference to the decoder's own allocation and releases it through the cleanup
// hook. Copying instead would memcpy ~15 MB per frame at 1440p.
[[nodiscard]] QImage WrapDecodedFrame(const exosnap::engine::DecodedVideoFrame& frame);

// Decodes one tile per entry of `times_ms`, handing each to `emit_tile` as it
// finishes so the strip fills in progressively instead of appearing at once.
//
// Each full-size frame is released before the next decode starts. Holding them
// would cost ~15 MB per tile at 1440p, i.e. more for one strip than the player
// itself uses. A target that fails to decode is skipped and leaves its place
// empty; the remaining tiles still run. Returns as soon as `cancelled` is set.
void GenerateTimelineTiles(const std::vector<qint64>& times_ms, int row_height, const TimelineFrameDecoder& decode,
                           const std::function<void(TimelineThumbnail&&)>& emit_tile,
                           const std::atomic<bool>& cancelled);

// ---- Tile cache (pure) ----

// Bounded store of finished tiles, keyed by the clip time a tile stands for and
// the row height it was scaled to.
//
// A resize does not change WHICH frames the strip shows, only how many of them:
// tile times are snapped to keyframes, so a track that grows asks for the
// positions it already had plus a few new ones. Without a cache every resize
// re-decoded the whole strip, and a window drag re-decoded it once per
// intermediate width.
//
// Bounded by total pixel bytes rather than by entry count: a tile's size follows
// the row height and the clip's aspect, so a fixed number of entries would mean
// a different memory ceiling for every recording. Eviction is least-recently-
// used, because a resize looks the surviving positions up again immediately.
class TimelineTileCache {
  public:
    static constexpr std::size_t kDefaultCapacityBytes = 16u * 1024u * 1024u;

    explicit TimelineTileCache(std::size_t capacity_bytes = kDefaultCapacityBytes) noexcept;

    // The stored tile, or nullptr. Counts as a use for eviction order, so this
    // is deliberately not const.
    [[nodiscard]] const QImage* Lookup(qint64 time_ms, int row_height);

    // Stores a copy. A tile larger than the whole capacity is not stored at all
    // rather than emptying the cache to hold one entry.
    void Insert(qint64 time_ms, int row_height, const QImage& image);

    // Every cached tile belongs to one clip; the caller clears on every open and
    // close. The key deliberately carries no clip identity: holding tiles for a
    // clip nobody is looking at is memory spent on a strip that is not drawn.
    void Clear() noexcept;

    [[nodiscard]] std::size_t sizeBytes() const noexcept {
        return size_bytes_;
    }
    [[nodiscard]] std::size_t count() const noexcept {
        return entries_.size();
    }

  private:
    struct Key {
        int row_height = 0;
        qint64 time_ms = 0;

        [[nodiscard]] bool operator<(const Key& other) const noexcept {
            return row_height != other.row_height ? row_height < other.row_height : time_ms < other.time_ms;
        }
    };
    struct Entry {
        QImage image;
        std::uint64_t last_used = 0;
        std::size_t bytes = 0;
    };

    void EvictUntilFits(std::size_t incoming_bytes);

    std::map<Key, Entry> entries_;
    std::size_t capacity_bytes_ = kDefaultCapacityBytes;
    std::size_t size_bytes_ = 0;
    std::uint64_t use_counter_ = 0;
};

// ---- Worker ----

// Owns the worker thread and its engine. Every public method returns
// immediately; results arrive on the owner's thread through the signals below.
class TimelineThumbnailSource : public QObject {
    Q_OBJECT
  public:
    explicit TimelineThumbnailSource(QObject* parent = nullptr);
    ~TimelineThumbnailSource() override;

    TimelineThumbnailSource(const TimelineThumbnailSource&) = delete;
    TimelineThumbnailSource& operator=(const TimelineThumbnailSource&) = delete;

    // Opens `path` on the worker and reports its frame size through
    // clipOpened(). `keyframes_us` is the clip's cue table, which the caller
    // already read -- re-reading it here would open the file a second time for
    // nothing. Cancels whatever run is in flight.
    void openClip(const QString& path, std::vector<int64_t> keyframes_us);

    // Starts a run over `times_ms`, scaling each tile to `row_height`. A run
    // already in flight is cancelled first, so a resize storm collapses to its
    // last request instead of stacking one run per intermediate width. Returns
    // the run id carried by this run's tileReady() emissions.
    quint64 requestTiles(std::vector<qint64> times_ms, int row_height);

    // Abandons the run in flight; its remaining tiles are never emitted.
    void cancel();

    // Cancels the run in flight AND closes the clip on the worker, so the file
    // stops being held open once the Edit surface is done with it. Without this,
    // the engine keeps the container open until the process exits and the
    // recording cannot be moved, renamed or deleted. Idempotent.
    void closeClip();

    // The open clip's frame size, or 0x0 when nothing is open. Valid on the
    // owner's thread once clipOpened() has fired.
    [[nodiscard]] int videoWidth() const noexcept {
        return video_width_;
    }
    [[nodiscard]] int videoHeight() const noexcept {
        return video_height_;
    }

  signals:
    // The worker finished opening a clip. 0x0 means it could not be opened or
    // carries no video -- the strip then simply stays empty.
    //
    // `audio_track_names` is EditPlayerEngine::AudioTracks() in stream order;
    // an entry is empty for recordings written before track names were muxed,
    // which a consumer labels positionally rather than guessing a source from
    // the track order. The worker reports it here because it already has the
    // file open -- probing it separately would open the clip a third time.
    void clipOpened(int video_width, int video_height, const QStringList& audio_track_names);

    // One finished tile. `run_id` lets a consumer drop tiles from a run whose
    // geometry it has already moved past.
    void tileReady(qint64 time_ms, const QImage& image, quint64 run_id);

    // QCR-307. A run has ended, and how. Without this a consumer could only
    // observe tiles ARRIVING: a clip the engine cannot open, or one that opens
    // and decodes nothing, produced no tile and no event either, so "still
    // decoding" and "there is nothing to decode" were the same observation
    // forever.
    //
    // `tiles_emitted` is how many tileReady() emissions this run made.
    // `cancelled` separates a run that was abandoned — a resize, a clip switch,
    // a close — from one that ran to the end. Only the second kind can mean the
    // clip carries nothing; a cancelled run is not a failure and must never be
    // reported as one.
    void runFinished(quint64 run_id, int tiles_emitted, bool cancelled);

  private:
    struct OpenJob {
        std::filesystem::path path;
        std::vector<int64_t> keyframes_us; // kept with the clip: the caller's cue table
        quint64 generation = 0;            // which openClip() asked for it
    };
    struct TileJob {
        quint64 run_id = 0;
        std::vector<qint64> times_ms;
        int row_height = 0;
    };

    void workerLoop();

    // Cancels the run in flight and wakes the worker. Caller holds mutex_.
    void cancelLocked();

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    // Close and open are mutually exclusive requests: each clears the other, so
    // the worker never has to decide which of the two came last.
    bool pending_close_ = false;
    // Latest request wins; an unstarted job is simply replaced.
    std::optional<OpenJob> pending_open_;
    std::optional<TileJob> pending_tiles_;
    std::atomic<bool> cancelled_{false};
    quint64 next_run_id_ = 0;

    // Owner-thread mirrors of what the worker reported, so a consumer can size
    // its tiles without reaching across the thread boundary.
    int video_width_ = 0;
    int video_height_ = 0;
    // Bumped by every openClip()/closeClip(). A clipOpened() already queued for
    // an older generation is dropped on arrival: a clip closed while the worker
    // was still opening it must not report a size (or a track list) afterwards.
    // Owner thread only -- the worker just carries the value it was given.
    quint64 open_generation_ = 0;
};

} // namespace exosnap

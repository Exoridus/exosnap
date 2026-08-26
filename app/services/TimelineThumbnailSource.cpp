#include "TimelineThumbnailSource.h"

#include <QMetaObject>
#include <QSize>

#include <exosnap/engine/logging/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace exosnap {

namespace {

// A tile narrower than this cannot show anything; it would only make the strip
// look like noise. Reached only by absurd frame sizes, never by a real clip.
constexpr int kMinTileWidth = 8;

// Scaling to an exact row height with the aspect kept needs a bounding box that
// the height, not the width, limits. Sixteen times the row height clears every
// ratio a display or camera produces, ultrawide included.
constexpr int kAspectBoxWidthFactor = 16;

// Frame size assumed when the clip has not reported one yet.
constexpr int kFallbackAspectW = 16;
constexpr int kFallbackAspectH = 9;

} // namespace

int TimelineTileWidth(int row_height, int video_width, int video_height) {
    if (row_height <= 0)
        return 0;
    const double aspect = (video_width > 0 && video_height > 0)
                              ? static_cast<double>(video_width) / static_cast<double>(video_height)
                              : static_cast<double>(kFallbackAspectW) / static_cast<double>(kFallbackAspectH);
    return std::max(kMinTileWidth, static_cast<int>(std::lround(row_height * aspect)));
}

int TimelineTileCount(int track_width, int tile_width) {
    if (track_width <= 0 || tile_width <= 0)
        return 0;
    return track_width / tile_width;
}

std::vector<qint64> TimelineTileTimesMs(int tile_count, qint64 duration_ms, const std::vector<int64_t>& keyframes_us) {
    std::vector<qint64> times;
    if (tile_count <= 0 || duration_ms <= 0)
        return times;

    // Nearest keyframe rather than the one before: a tile sitting a whole GOP
    // behind its own position would misreport what the clip looks like there.
    const auto snap = [&keyframes_us](qint64 nominal_ms) -> qint64 {
        if (keyframes_us.empty())
            return nominal_ms;
        const int64_t nominal_us = nominal_ms * 1000;
        auto after = std::lower_bound(keyframes_us.begin(), keyframes_us.end(), nominal_us);
        if (after == keyframes_us.begin())
            return static_cast<qint64>(*after / 1000);
        if (after == keyframes_us.end())
            return static_cast<qint64>(*(after - 1) / 1000);
        const int64_t before_us = *(after - 1);
        const int64_t after_us = *after;
        const int64_t chosen = (nominal_us - before_us <= after_us - nominal_us) ? before_us : after_us;
        return static_cast<qint64>(chosen / 1000);
    };

    times.reserve(static_cast<size_t>(tile_count));
    for (int i = 0; i < tile_count; ++i) {
        // The tile's own left edge, not the middle of its slot: the tile is
        // drawn left-aligned there, so the frame must be the one that starts it.
        const qint64 nominal_ms = duration_ms * i / tile_count;
        const qint64 snapped_ms = snap(nominal_ms);
        if (!times.empty() && times.back() == snapped_ms)
            continue;
        times.push_back(snapped_ms);
    }
    return times;
}

QImage WrapDecodedFrame(const exosnap::engine::DecodedVideoFrame& frame) {
    // Geometry first, keep-alive second. QImage refuses a frame it cannot
    // describe (no buffer, a zero side, a stride shorter than one row, a
    // dimension past what its int-based geometry can hold) by returning a null
    // image -- and a null image never runs the cleanup hook, so a keep-alive
    // allocated before the constructor would strand both itself and the
    // decoder's ~15-33 MB frame buffer for the life of the process.
    constexpr auto kMaxSide = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (frame.bgra == nullptr || frame.width == 0 || frame.height == 0 || frame.width > kMaxSide ||
        frame.height > kMaxSide || frame.stride_bytes / 4u < frame.width || frame.stride_bytes > kMaxSide)
        return {};

    auto* keep_alive = new std::shared_ptr<const uint8_t[]>(frame.bgra);
    QImage image(
        frame.bgra.get(), static_cast<int>(frame.width), static_cast<int>(frame.height),
        static_cast<int>(frame.stride_bytes), QImage::Format_ARGB32,
        [](void* owner) { delete static_cast<std::shared_ptr<const uint8_t[]>*>(owner); }, keep_alive);
    if (image.isNull()) {
        // Rejected for a reason the checks above do not name (an allocation
        // failure inside QImage, say): the hook was never registered, so the
        // reference is released here instead of leaking.
        delete keep_alive;
        return {};
    }
    return image;
}

void GenerateTimelineTiles(const std::vector<qint64>& times_ms, int row_height, const TimelineFrameDecoder& decode,
                           const std::function<void(TimelineThumbnail&&)>& emit_tile,
                           const std::atomic<bool>& cancelled) {
    if (row_height <= 0 || !decode || !emit_tile)
        return;

    for (const qint64 time_ms : times_ms) {
        if (cancelled.load(std::memory_order_relaxed))
            return;

        QImage scaled;
        {
            // Scoped deliberately: the decoded frame and the QImage wrapping it
            // must both be gone before the next decode allocates its own buffer.
            // QImage::scaled() produces an independent image, so nothing here
            // outlives the scope.
            const std::optional<exosnap::engine::DecodedVideoFrame> frame = decode(time_ms * 1000);
            if (!frame)
                continue; // this tile stays empty; the rest of the strip still fills in
            const QImage full = WrapDecodedFrame(*frame);
            if (full.isNull())
                continue;
            scaled = full.scaled(QSize(row_height * kAspectBoxWidthFactor, row_height), Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        }
        if (scaled.isNull())
            continue;
        emit_tile(TimelineThumbnail{time_ms, std::move(scaled)});
    }
}

TimelineThumbnailSource::TimelineThumbnailSource(QObject* parent) : QObject(parent) {
    worker_ = std::thread([this]() { workerLoop(); });
}

TimelineThumbnailSource::~TimelineThumbnailSource() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        cancelled_.store(true, std::memory_order_relaxed);
        pending_open_.reset();
        pending_tiles_.reset();
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

TimelineTileCache::TimelineTileCache(std::size_t capacity_bytes) noexcept : capacity_bytes_(capacity_bytes) {
}

const QImage* TimelineTileCache::Lookup(qint64 time_ms, int row_height) {
    const auto it = entries_.find(Key{row_height, time_ms});
    if (it == entries_.end())
        return nullptr;
    it->second.last_used = ++use_counter_;
    return &it->second.image;
}

void TimelineTileCache::Insert(qint64 time_ms, int row_height, const QImage& image) {
    if (image.isNull())
        return;
    const auto bytes = static_cast<std::size_t>(std::max<qsizetype>(0, image.sizeInBytes()));
    if (bytes == 0 || bytes > capacity_bytes_)
        return;

    const Key key{row_height, time_ms};
    if (const auto existing = entries_.find(key); existing != entries_.end()) {
        size_bytes_ -= existing->second.bytes;
        entries_.erase(existing);
    }
    EvictUntilFits(bytes);
    // A deep copy, unconditionally: WrapDecodedFrame() hands out an image that
    // only BORROWS the decoder's frame buffer, and that buffer is released as
    // soon as the next tile decodes. A shared copy would outlive its pixels.
    entries_.emplace(key, Entry{image.copy(), ++use_counter_, bytes});
    size_bytes_ += bytes;
}

void TimelineTileCache::Clear() noexcept {
    entries_.clear();
    size_bytes_ = 0;
}

void TimelineTileCache::EvictUntilFits(std::size_t incoming_bytes) {
    while (!entries_.empty() && size_bytes_ + incoming_bytes > capacity_bytes_) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->second.last_used < oldest->second.last_used)
                oldest = it;
        }
        size_bytes_ -= oldest->second.bytes;
        entries_.erase(oldest);
    }
}

void TimelineThumbnailSource::cancelLocked() {
    cancelled_.store(true, std::memory_order_relaxed);
    pending_tiles_.reset();
}

void TimelineThumbnailSource::openClip(const QString& path, std::vector<int64_t> keyframes_us) {
    video_width_ = 0;
    video_height_ = 0;
    ++open_generation_;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancelLocked();
        pending_close_ = false;
        pending_open_ = OpenJob{std::filesystem::path(path.toStdWString()), std::move(keyframes_us), open_generation_};
    }
    cv_.notify_all();
}

void TimelineThumbnailSource::closeClip() {
    video_width_ = 0;
    video_height_ = 0;
    ++open_generation_;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancelLocked();
        pending_open_.reset();
        pending_close_ = true;
    }
    cv_.notify_all();
}

quint64 TimelineThumbnailSource::requestTiles(std::vector<qint64> times_ms, int row_height) {
    quint64 run_id = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancelLocked();
        run_id = ++next_run_id_;
        pending_tiles_ = TileJob{run_id, std::move(times_ms), row_height};
    }
    cv_.notify_all();
    return run_id;
}

void TimelineThumbnailSource::cancel() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancelLocked();
    }
    cv_.notify_all();
}

void TimelineThumbnailSource::workerLoop() {
    // Constructed on the worker so the engine (and everything FFmpeg allocates
    // behind it) lives and dies on exactly one thread.
    auto engine = std::make_unique<exosnap::engine::EditPlayerEngine>();
    bool open = false;
    // Worker-owned: every read and write happens on this thread, so the cache
    // needs no lock of its own.
    TimelineTileCache tile_cache;

    for (;;) {
        std::optional<OpenJob> open_job;
        std::optional<TileJob> tile_job;
        bool close_job = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stop_ || pending_close_ || pending_open_.has_value() || pending_tiles_.has_value();
            });
            if (stop_)
                break;
            // Closing first, then opening: a tile job queued alongside either
            // belongs to what the clip becomes, never to the one still open.
            if (pending_close_) {
                close_job = true;
                pending_close_ = false;
            } else if (pending_open_) {
                open_job = std::move(pending_open_);
                pending_open_.reset();
            } else {
                tile_job = std::move(pending_tiles_);
                pending_tiles_.reset();
                // The run that starts here is the one the flag now applies to.
                cancelled_.store(false, std::memory_order_relaxed);
            }
        }

        if (close_job) {
            engine->Close();
            tile_cache.Clear();
            open = false;
            continue;
        }

        if (open_job) {
            engine->Close();
            // The key carries no clip identity, so the tiles of the clip being
            // replaced have to go before the new one decodes anything.
            tile_cache.Clear();
            std::string error;
            open = engine->Open(open_job->path, error);
            const int w = open ? engine->VideoWidth() : 0;
            const int h = open ? engine->VideoHeight() : 0;
            QStringList audio_names;
            if (open) {
                for (const auto& track : engine->AudioTracks())
                    audio_names.append(QString::fromStdString(track.name));
            }
            const quint64 generation = open_job->generation;
            QMetaObject::invokeMethod(
                this,
                [this, w, h, audio_names, generation]() {
                    // The clip may have been closed (or replaced) while this open
                    // was still running -- its size and track list then belong to
                    // nothing the owner is showing.
                    if (generation != open_generation_)
                        return;
                    video_width_ = w;
                    video_height_ = h;
                    emit clipOpened(w, h, audio_names);
                },
                Qt::QueuedConnection);
            continue;
        }

        if (!tile_job)
            continue;

        const quint64 run_id = tile_job->run_id;
        // QCR-307. Reported for BOTH endings, including the one that decodes
        // nothing at all. The clip that could not be opened used to leave the
        // loop here silently, which is precisely why the strip could say
        // "Generating previews…" for the rest of the session.
        const auto finish = [this, run_id](int emitted) {
            const bool cancelled = cancelled_.load(std::memory_order_relaxed);
            QMetaObject::invokeMethod(
                this, [this, run_id, emitted, cancelled]() { emit runFinished(run_id, emitted, cancelled); },
                Qt::QueuedConnection);
        };

        if (!open) {
            finish(0);
            continue;
        }

        const auto started = std::chrono::steady_clock::now();
        const int row_height = tile_job->row_height;
        int emitted = 0;
        const auto publish = [this, run_id, &emitted](qint64 time_ms, const QImage& image) {
            ++emitted;
            // One tile at a time onto the owner's thread: the strip fills in
            // progressively instead of arriving as one late batch.
            QMetaObject::invokeMethod(
                this, [this, time_ms, image, run_id]() { emit tileReady(time_ms, image, run_id); },
                Qt::QueuedConnection);
        };

        // The cached positions first, and all of them before any decode starts:
        // after a resize that is most of the strip, and publishing them up front
        // is what turns a re-decode of the whole track into a relayout.
        std::vector<qint64> to_decode;
        to_decode.reserve(tile_job->times_ms.size());
        int from_cache = 0;
        for (const qint64 time_ms : tile_job->times_ms) {
            if (cancelled_.load(std::memory_order_relaxed))
                break;
            if (const QImage* cached = tile_cache.Lookup(time_ms, row_height)) {
                ++from_cache;
                publish(time_ms, *cached);
            } else {
                to_decode.push_back(time_ms);
            }
        }

        GenerateTimelineTiles(
            to_decode, row_height, [&engine](int64_t target_us) { return engine->DecodeFrameAt(target_us); },
            [&tile_cache, &publish, row_height](TimelineThumbnail&& tile) {
                tile_cache.Insert(tile.time_ms, row_height, tile.image);
                publish(tile.time_ms, tile.image);
            },
            cancelled_);

        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        const bool run_cancelled = cancelled_.load(std::memory_order_relaxed);
        const exosnap::engine::logging::LogField fields[] = {
            {"run_id", std::to_string(run_id)},
            {"tiles", std::to_string(emitted)},
            {"cached", std::to_string(from_cache)},
            {"decoded", std::to_string(emitted - from_cache)},
            {"duration_ms", std::to_string(elapsed_ms)},
            {"cache_bytes", std::to_string(tile_cache.sizeBytes())},
            {"cancelled", run_cancelled ? "true" : "false"},
        };
        exosnap::engine::logging::log(exosnap::engine::logging::LogLevel::Debug, "timeline_thumbnails",
                                      "tile run finished", fields);
        finish(emitted);
    }

    engine.reset();
}

} // namespace exosnap

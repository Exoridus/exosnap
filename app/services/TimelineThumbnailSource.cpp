#include "TimelineThumbnailSource.h"

#include <QMetaObject>
#include <QSize>

#include <algorithm>
#include <cmath>
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

QImage WrapDecodedFrame(const recorder_core::DecodedVideoFrame& frame) {
    auto* keep_alive = new std::shared_ptr<const uint8_t[]>(frame.bgra);
    return QImage(
        frame.bgra.get(), static_cast<int>(frame.width), static_cast<int>(frame.height),
        static_cast<int>(frame.stride_bytes), QImage::Format_ARGB32,
        [](void* owner) { delete static_cast<std::shared_ptr<const uint8_t[]>*>(owner); }, keep_alive);
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
            const std::optional<recorder_core::DecodedVideoFrame> frame = decode(time_ms * 1000);
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

void TimelineThumbnailSource::cancelLocked() {
    cancelled_.store(true, std::memory_order_relaxed);
    pending_tiles_.reset();
}

void TimelineThumbnailSource::openClip(const QString& path, std::vector<int64_t> keyframes_us) {
    video_width_ = 0;
    video_height_ = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        cancelLocked();
        pending_open_ = OpenJob{std::filesystem::path(path.toStdWString()), std::move(keyframes_us)};
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
    auto engine = std::make_unique<recorder_core::EditPlayerEngine>();
    bool open = false;

    for (;;) {
        std::optional<OpenJob> open_job;
        std::optional<TileJob> tile_job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || pending_open_.has_value() || pending_tiles_.has_value(); });
            if (stop_)
                break;
            // Opening first: a tile job queued alongside it belongs to the new
            // clip, never to the one still open.
            if (pending_open_) {
                open_job = std::move(pending_open_);
                pending_open_.reset();
            } else {
                tile_job = std::move(pending_tiles_);
                pending_tiles_.reset();
                // The run that starts here is the one the flag now applies to.
                cancelled_.store(false, std::memory_order_relaxed);
            }
        }

        if (open_job) {
            engine->Close();
            std::string error;
            open = engine->Open(open_job->path, error);
            const int w = open ? engine->VideoWidth() : 0;
            const int h = open ? engine->VideoHeight() : 0;
            QStringList audio_names;
            if (open) {
                for (const auto& track : engine->AudioTracks())
                    audio_names.append(QString::fromStdString(track.name));
            }
            QMetaObject::invokeMethod(
                this,
                [this, w, h, audio_names]() {
                    video_width_ = w;
                    video_height_ = h;
                    emit clipOpened(w, h, audio_names);
                },
                Qt::QueuedConnection);
            continue;
        }

        if (!tile_job)
            continue;
        if (!open)
            continue; // nothing decodable: the strip stays empty, by design

        const quint64 run_id = tile_job->run_id;
        GenerateTimelineTiles(
            tile_job->times_ms, tile_job->row_height,
            [&engine](int64_t target_us) { return engine->DecodeFrameAt(target_us); },
            [this, run_id](TimelineThumbnail&& tile) {
                // One tile at a time onto the owner's thread: the strip fills
                // in progressively instead of arriving as one late batch.
                QMetaObject::invokeMethod(
                    this, [this, tile, run_id]() { emit tileReady(tile.time_ms, tile.image, run_id); },
                    Qt::QueuedConnection);
            },
            cancelled_);
    }

    engine.reset();
}

} // namespace exosnap

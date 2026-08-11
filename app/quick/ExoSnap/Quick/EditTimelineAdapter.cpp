#include "EditTimelineAdapter.h"

#include "EditSessionAdapter.h"

#include <QColor>

#include <algorithm>

namespace exosnap::quick {
namespace {

// A resize changes the tile count, so every intermediate width of a window drag
// would otherwise start (and cancel) its own decode run.
constexpr int kResizeDebounceMs = 150;

// Placeholder tile for the visual harness and the adapter tests, which have no
// clip to decode. Deterministic per position so a fixture renders identically on
// every machine -- same shading rule the Widgets fixture used.
QImage fixtureTile(int index, int width, int height) {
    QImage tile(std::max(width, 1), std::max(height, 1), QImage::Format_ARGB32);
    const int shade = 40 + (index * 17) % 90;
    tile.fill(QColor(shade, shade + 6, shade + 10));
    return tile;
}

} // namespace

int TimelineAudioRowHeight(int row_count) {
    if (row_count <= 0)
        return 0;
    const int budget = kTimelineAudioStackBudget - (row_count - 1) * kTimelineRowGap;
    return std::clamp(budget / row_count, kTimelineAudioRowMinHeight, kTimelineAudioRowHeight);
}

int TimelineAudioStackHeight(int row_count) {
    if (row_count <= 0)
        return 0;
    return row_count * (kTimelineRowGap + TimelineAudioRowHeight(row_count));
}

EditTimelineAdapter::EditTimelineAdapter(QObject* parent) : QObject(parent) {
    resize_debounce_.setSingleShot(true);
    resize_debounce_.setInterval(kResizeDebounceMs);
    connect(&resize_debounce_, &QTimer::timeout, this, &EditTimelineAdapter::startTileRun);
}

EditTimelineAdapter::~EditTimelineAdapter() = default;

void EditTimelineAdapter::setTileProvider(EditTimelineTileProvider* provider) {
    provider_ = provider;
}

void EditTimelineAdapter::setSession(EditSessionAdapter* session) {
    session_ = session;
    if (session_ == nullptr)
        return;
    connect(session_, &EditSessionAdapter::clipOpened, this, &EditTimelineAdapter::openClip);
    connect(session_, &EditSessionAdapter::clipClosed, this, &EditTimelineAdapter::closeClip);
    connect(session_, &EditSessionAdapter::clipChanged, this, &EditTimelineAdapter::refreshMarkers);
    // A fixture clip (visual harness, adapter tests) carries a duration but no
    // master path, so it never reaches clipOpened -- the strip layout still
    // needs the length to place its tiles.
    connect(session_, &EditSessionAdapter::durationChanged, this, [this]() {
        duration_ms_ = session_->durationMs();
        refreshMarkers();
        scheduleTileRun();
    });
    // The strip's tile positions snap to the clip's own cue table, so the run
    // waits for the keyframe scan rather than decoding an evenly-spread set that
    // would be thrown away the moment the table lands.
    connect(session_, &EditSessionAdapter::trimSnapReadyChanged, this, [this]() {
        if (session_ == nullptr || !session_->trimSnapReady() || clip_path_.isEmpty())
            return;
        if (!source_.has_value())
            source_.emplace();
        connect(
            &source_.value(), &TimelineThumbnailSource::clipOpened, this,
            [this](int width, int height, const QStringList& audio_track_names) {
                clip_video_width_ = width;
                clip_video_height_ = height;
                setAudioTrackLabels(audio_track_names);
                scheduleTileRun();
            },
            Qt::UniqueConnection);
        connect(
            &source_.value(), &TimelineThumbnailSource::tileReady, this,
            [this](qint64 time_ms, const QImage& image, quint64 run_id) {
                if (run_id != active_run_)
                    return;
                if (provider_ != nullptr)
                    provider_->submitTile(run_id, tiles_ready_, image);
                ++tiles_ready_;
                tile_model_.appendTile(time_ms);
                emit tileProgressChanged();
            },
            Qt::UniqueConnection);
        source_->openClip(clip_path_, session_->keyframeTimestamps());
    });
}

QAbstractItemModel* EditTimelineAdapter::tileModel() noexcept {
    return &tile_model_;
}

QAbstractItemModel* EditTimelineAdapter::markerModel() noexcept {
    return &marker_model_;
}

int EditTimelineAdapter::trackWidth() const noexcept {
    return track_width_;
}

void EditTimelineAdapter::setTrackWidth(int width) {
    const int bounded = std::max(width, 0);
    if (track_width_ == bounded)
        return;
    track_width_ = bounded;
    emit trackWidthChanged();
    refreshMarkers();
    scheduleTileRun();
}

int EditTimelineAdapter::tileWidth() const noexcept {
    return tile_width_;
}

int EditTimelineAdapter::videoRowHeight() noexcept {
    return kTimelineVideoRowHeight;
}

int EditTimelineAdapter::audioRowHeight() const noexcept {
    return TimelineAudioRowHeight(static_cast<int>(audio_track_labels_.size()));
}

int EditTimelineAdapter::audioStackHeight() const noexcept {
    return TimelineAudioStackHeight(static_cast<int>(audio_track_labels_.size()));
}

const QStringList& EditTimelineAdapter::audioTrackLabels() const noexcept {
    return audio_track_labels_;
}

int EditTimelineAdapter::tilesExpected() const noexcept {
    return tiles_expected_;
}

int EditTimelineAdapter::tilesReady() const noexcept {
    return tiles_ready_;
}

bool EditTimelineAdapter::generatingPreviews() const noexcept {
    return tiles_expected_ > 0 && tiles_ready_ < tiles_expected_;
}

void EditTimelineAdapter::setAudioTrackLabels(const QStringList& labels) {
    QStringList named;
    named.reserve(labels.size());
    for (int i = 0; i < labels.size(); ++i) {
        // An empty entry is labelled positionally rather than guessed from track
        // order, which the container does not define.
        named.append(labels.at(i).isEmpty() ? QStringLiteral("Audio %1").arg(i + 1) : labels.at(i));
    }
    if (named == audio_track_labels_)
        return;
    audio_track_labels_ = named;
    emit audioTracksChanged();
}

void EditTimelineAdapter::openClip(const QString& master_path, qint64 duration_ms) {
    clip_path_ = master_path;
    duration_ms_ = duration_ms;
    fixture_tile_count_.reset();
    tiles_ready_ = 0;
    tiles_expected_ = 0;
    tile_model_.clear();
    if (provider_ != nullptr)
        provider_->clear();
    setAudioTrackLabels({});
    emit tileProgressChanged();
    refreshMarkers();
    scheduleTileRun();
}

void EditTimelineAdapter::closeClip() {
    clip_path_.clear();
    // The strip is gone, but the clip's length is not: a context without a
    // decodable master (the visual fixture) still has a duration and markers to
    // lay out against.
    duration_ms_ = session_ != nullptr ? session_->durationMs() : 0;
    tiles_ready_ = 0;
    tiles_expected_ = 0;
    tile_model_.clear();
    if (source_.has_value())
        source_->cancel();
    if (provider_ != nullptr)
        provider_->clear();
    setAudioTrackLabels({});
    emit tileProgressChanged();
    refreshMarkers();
}

void EditTimelineAdapter::refreshMarkers() {
    if (session_ == nullptr) {
        marker_model_.setMarkers({}, 0, track_width_);
        return;
    }
    marker_model_.setMarkers(session_->markers(), session_->durationMs(), track_width_);
}

void EditTimelineAdapter::scheduleTileRun() {
    tile_width_ = TimelineTileWidth(kTimelineVideoRowHeight, clip_video_width_, clip_video_height_);
    emit layoutChanged();
    resize_debounce_.start();
}

void EditTimelineAdapter::startTileRun() {
    const int tile_count = TimelineTileCount(track_width_, tile_width_);
    std::vector<int64_t> keyframes;
    if (session_ != nullptr)
        keyframes = session_->keyframeTimestamps();
    std::vector<qint64> times = TimelineTileTimesMs(tile_count, duration_ms_, keyframes);

    tiles_ready_ = 0;
    tiles_expected_ = static_cast<int>(times.size());

    if (fixture_tile_count_.has_value()) {
        const int requested = *fixture_tile_count_;
        // A count below what the row holds renders the partly filled state a
        // real decode passes through, hint included.
        const int count = requested < 0 ? tiles_expected_ : std::min(requested, tiles_expected_);
        ++active_run_;
        tile_model_.beginRun(active_run_);
        for (int i = 0; i < count; ++i) {
            if (provider_ != nullptr)
                provider_->submitTile(active_run_, i, fixtureTile(i, tile_width_, kTimelineVideoRowHeight));
            tile_model_.appendTile(times.at(static_cast<size_t>(i)));
            ++tiles_ready_;
        }
        emit tileProgressChanged();
        return;
    }

    if (!source_.has_value() || times.empty() || clip_path_.isEmpty()) {
        tile_model_.clear();
        emit tileProgressChanged();
        return;
    }

    active_run_ = source_->requestTiles(times, kTimelineVideoRowHeight);
    tile_model_.beginRun(active_run_);
    if (provider_ != nullptr)
        provider_->clear();
    emit tileProgressChanged();
}

void EditTimelineAdapter::setFixture(const QStringList& audio_track_labels, int tile_count) {
    setAudioTrackLabels(audio_track_labels);
    fixture_tile_count_ = tile_count;
    if (tile_width_ <= 0)
        tile_width_ = TimelineTileWidth(kTimelineVideoRowHeight, clip_video_width_, clip_video_height_);
    startTileRun();
}

} // namespace exosnap::quick

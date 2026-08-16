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

bool TimelineTileRunAllowed(bool has_source, const QString& clip_path, const QString& source_clip_path,
                            int tile_count) {
    if (!has_source || tile_count <= 0 || clip_path.isEmpty())
        return false;
    return source_clip_path == clip_path;
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
        ensureThumbnailSource();
        // Recorded before the request, not after it: the worker handles the open
        // before any tile job queued behind it, so every run started from here on
        // decodes out of this clip.
        source_clip_path_ = clip_path_;
        source_->openClip(clip_path_, session_->keyframeTimestamps());
    });
}

// The thumbnail source is created and wired exactly once, on the first clip that
// reaches a completed keyframe scan. It used to be (re)connected on every
// trimSnapReadyChanged with Qt::UniqueConnection — which is not a thing Qt
// supports for a lambda: uniqueness requires a pointer-to-member-function
// receiver. A debug build asserts and abort()s on the first real clip; a release
// build, where the assert is compiled out, instead accumulates a duplicate pair
// of connections per opened clip, so the second clip counts every decoded tile
// twice. Neither showed up in the adapter tests or the visual harness because
// both drive setFixture(), which never reaches this path — only genuinely
// decoded media does.
void EditTimelineAdapter::ensureThumbnailSource() {
    if (source_.has_value())
        return;
    source_.emplace();
    connect(&source_.value(), &TimelineThumbnailSource::clipOpened, this, &EditTimelineAdapter::handleClipOpened);
    connect(&source_.value(), &TimelineThumbnailSource::tileReady, this, &EditTimelineAdapter::handleTileReady);
    connect(&source_.value(), &TimelineThumbnailSource::runFinished, this, &EditTimelineAdapter::handleRunFinished);
}

void EditTimelineAdapter::handleClipOpened(int width, int height, const QStringList& audio_track_names) {
    clip_video_width_ = width;
    clip_video_height_ = height;
    setAudioTrackLabels(audio_track_names);
    // QCR-307. 0x0 is the worker's answer for "could not open, or carries no
    // video". Either way no tile can ever arrive for this clip, and saying so
    // here means the strip does not have to wait for a run that will decode
    // nothing to prove it. Everything else about the Edit surface — trim,
    // playback, markers, export — is unaffected and stays usable.
    if (width <= 0 || height <= 0) {
        if (!previews_unavailable_) {
            previews_unavailable_ = true;
            emit tileProgressChanged();
        }
    }
    scheduleTileRun();
}

// QCR-307. The end of a run, and the only place "this clip carries nothing
// decodable" may be concluded from a run rather than from the open.
void EditTimelineAdapter::handleRunFinished(quint64 run_id, int tiles_emitted, bool cancelled) {
    if (run_id == 0 || run_id != active_run_)
        return; // a run the strip has already moved past
    // A cancelled run is a resize, a clip switch or a close. It says nothing at
    // all about the clip, and reporting it as a failure would put "Preview
    // unavailable" on a perfectly good recording the user merely resized.
    if (cancelled)
        return;
    if (tiles_emitted > 0 || tiles_ready_ > 0)
        return; // it produced something; the strip is ready, not unavailable
    if (previews_unavailable_)
        return;
    previews_unavailable_ = true;
    emit tileProgressChanged();
}

void EditTimelineAdapter::handleTileReady(qint64 time_ms, const QImage& image, quint64 run_id) {
    // A tile carries the run it was decoded for. Cancelling a run cannot stop the
    // tile already on its way to this thread, so the identity is checked here as
    // well: after a clip switch or a close, active_run_ is 0 and nothing in
    // flight can reach the strip.
    if (run_id == 0 || run_id != active_run_)
        return;
    if (provider_ != nullptr)
        provider_->submitTile(run_id, tiles_ready_, image);
    ++tiles_ready_;
    tile_model_.appendTile(time_ms);
    emit tileProgressChanged();
}

void EditTimelineAdapter::invalidateRun() {
    active_run_ = 0;
    if (source_.has_value())
        source_->cancel();
}

void EditTimelineAdapter::deliverTileForTest(qint64 time_ms, const QImage& image, quint64 run_id) {
    handleTileReady(time_ms, image, run_id);
}

quint64 EditTimelineAdapter::activeRunForTest() const noexcept {
    return active_run_;
}

void EditTimelineAdapter::deliverRunFinishedForTest(quint64 run_id, int tiles_emitted, bool cancelled) {
    handleRunFinished(run_id, tiles_emitted, cancelled);
}

void EditTimelineAdapter::deliverClipOpenedForTest(int video_width, int video_height,
                                                   const QStringList& audio_track_names) {
    handleClipOpened(video_width, video_height, audio_track_names);
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
    return !previews_unavailable_ && tiles_expected_ > 0 && tiles_ready_ < tiles_expected_;
}

bool EditTimelineAdapter::previewsUnavailable() const noexcept {
    return previews_unavailable_;
}

QString EditTimelineAdapter::previewState() const {
    if (previews_unavailable_)
        return QStringLiteral("unavailable");
    if (tiles_expected_ <= 0)
        return QStringLiteral("idle");
    return tiles_ready_ < tiles_expected_ ? QStringLiteral("generating") : QStringLiteral("ready");
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
    // Before anything is published for the new clip: the previous run is dropped
    // here, not when the new one starts. Between the two lies the debounce and
    // the source's own reopen, and a tile decoded from the previous clip that
    // arrived in that window used to be counted as this clip's.
    invalidateRun();
    source_clip_path_.clear();
    clip_path_ = master_path;
    duration_ms_ = duration_ms;
    clip_video_width_ = 0;
    clip_video_height_ = 0;
    fixture_tile_count_.reset();
    tiles_ready_ = 0;
    tiles_expected_ = 0;
    // A verdict belongs to the clip it was reached for. Clip A being
    // undecodable must not greet clip B.
    previews_unavailable_ = false;
    tile_model_.clear();
    if (provider_ != nullptr)
        provider_->clear();
    setAudioTrackLabels({});
    emit tileProgressChanged();
    refreshMarkers();
    scheduleTileRun();
}

void EditTimelineAdapter::closeClip() {
    invalidateRun();
    source_clip_path_.clear();
    clip_path_.clear();
    resize_debounce_.stop();
    fixture_tile_count_.reset();
    clip_video_width_ = 0;
    clip_video_height_ = 0;
    // The strip is gone, but the clip's length is not: a context without a
    // decodable master (the visual fixture) still has a duration and markers to
    // lay out against.
    duration_ms_ = session_ != nullptr ? session_->durationMs() : 0;
    tiles_ready_ = 0;
    tiles_expected_ = 0;
    previews_unavailable_ = false;
    tile_model_.clear();
    // Not just cancelled: the worker's engine keeps the container open until it
    // is told to close it, and a recording the Edit surface is done with must be
    // movable and deletable again.
    if (source_.has_value())
        source_->closeClip();
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

    if (!TimelineTileRunAllowed(source_.has_value(), clip_path_, source_clip_path_, static_cast<int>(times.size()))) {
        active_run_ = 0;
        // A clip whose source is still being reopened keeps its expected count:
        // the strip is genuinely mid-decode and says so. Without a clip at all
        // there is nothing to wait for.
        if (clip_path_.isEmpty() || times.empty())
            tiles_expected_ = 0;
        tile_model_.clear();
        emit tileProgressChanged();
        return;
    }

    // QCR-307. A fresh run over a clip the engine DID open gets a fresh verdict:
    // a resize is a new decode and may well succeed where the last one did not.
    // A clip that failed to open keeps its verdict, because re-running it would
    // only replace "Preview unavailable" with "Generating previews…" for the
    // length of a run that is guaranteed to decode nothing — which is the exact
    // lie this item exists to remove, once per resize.
    if (previews_unavailable_ && clip_video_width_ > 0 && clip_video_height_ > 0)
        previews_unavailable_ = false;

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

// QCR-307. Puts the strip in its terminal state for a --visual-test capture.
// The state is otherwise unreachable from a fixture by construction: the
// fixture path never touches the decoder, and the only clip that can produce
// this verdict is one the decoder actually failed on.
void EditTimelineAdapter::applyUnavailablePreviewsForHarness() {
    if (previews_unavailable_)
        return;
    previews_unavailable_ = true;
    emit tileProgressChanged();
}

} // namespace exosnap::quick

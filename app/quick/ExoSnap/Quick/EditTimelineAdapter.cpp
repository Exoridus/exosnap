#include "EditTimelineAdapter.h"
#include <QCoreApplication>

#include "EditSessionAdapter.h"

#include <QByteArray>
#include <QColor>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

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

EditTimelineAdapter::EditTimelineAdapter(QObject* parent)
    : QObject(parent), broker_(std::make_shared<ThumbnailRequestBroker>()),
      waveforms_(std::make_unique<WaveformCacheService>()) {
    resize_debounce_.setSingleShot(true);
    resize_debounce_.setInterval(kResizeDebounceMs);
    connect(&resize_debounce_, &QTimer::timeout, this, &EditTimelineAdapter::startTileRun);
}

EditTimelineAdapter::~EditTimelineAdapter() = default;

QString EditTimelineAdapter::posterSource() const {
    if (session_)
        for (const auto& asset : session_->workspace().assets())
            if (QString::fromStdWString(asset.path) == clip_path_) {
                const auto source = posterForIdentity(session_->sourcePreviewIdentity(asset.id));
                if (!source.isEmpty())
                    return source;
            }
    if (tile_model_.rowCount() <= 0)
        return {};
    return tile_model_.data(tile_model_.index(0), EditTimelineTileModel::SourceRole).toString();
}

void EditTimelineAdapter::setTileProvider(EditTimelineTileProvider* provider) {
    provider_ = provider;
}

void EditTimelineAdapter::setSession(EditSessionAdapter* session) {
    if (session_ == session)
        return;
    if (session_)
        disconnect(session_, nullptr, this, nullptr);
    broker_->ReleaseSources();
    clip_thumbnail_interests_.clear();
    waveforms_->releaseSources();
    registered_sources_.clear();
    if (source_)
        source_->closeClip();
    session_ = session;
    source_revision_.reset();
    if (session_ == nullptr)
        return;
    connect(session_, &EditSessionAdapter::mediaChanged, this, &EditTimelineAdapter::syncSourceRegistrations);
    connect(session_, &EditSessionAdapter::sourceVerificationChanged, this, [this](qulonglong asset) {
        registerSessionSource(asset);
        ++source_registration_revision_;
        emit sourcesChanged();
        emit tileProgressChanged();
    });
    connect(session_, &QObject::destroyed, this, [this] {
        session_ = nullptr;
        broker_->ReleaseSources();
        waveforms_->releaseSources();
        registered_sources_.clear();
        if (source_)
            source_->closeClip();
        ++source_registration_revision_;
        emit sourcesChanged();
    });
    syncSourceRegistrations();
    connect(session_, &EditSessionAdapter::clipOpened, this, &EditTimelineAdapter::openClip);
    connect(session_, &EditSessionAdapter::clipClosed, this, &EditTimelineAdapter::closeClip);
    connect(session_, &EditSessionAdapter::clipChanged, this, &EditTimelineAdapter::refreshMarkers);
    connect(session_, &EditSessionAdapter::markersChanged, this, &EditTimelineAdapter::refreshMarkers);
    // A fixture clip (visual harness, adapter tests) carries a duration but no
    // master path, so it never reaches clipOpened -- the strip layout still
    // needs the length to place its tiles.
    connect(session_, &EditSessionAdapter::durationChanged, this, [this]() {
        duration_ms_ = static_cast<qint64>(session_->editContext().duration_seconds * 1000.0);
        refreshMarkers();
        scheduleTileRun();
    });
}

void EditTimelineAdapter::registerSessionSource(qulonglong asset_id) {
    if (!session_)
        return;
    const auto access = session_->sourceAccess(asset_id);
    const auto metadata = session_->sourceMetadata(asset_id);
    const auto identity = session_->sourcePreviewIdentity(asset_id);
    const auto cache_identity = ParseContentIdentity(identity);
    if (!access || !metadata || !cache_identity || session_->sourceVerificationFailed(asset_id)) {
        if (const auto prior = registered_sources_.find(asset_id); prior != registered_sources_.end()) {
            broker_->RemoveSource(prior->second.toStdString());
            if (const auto retired_identity = ParseContentIdentity(prior->second))
                waveforms_->unregisterSource(*retired_identity);
            registered_sources_.erase(prior);
            if (source_)
                source_->closeClip();
        }
        return;
    }
    const auto proof = session_->sourceProof(asset_id);
    if (const auto prior = broker_->Source(identity.toStdString());
        prior && prior->access == access && prior->proof == proof)
        return;
    ensureThumbnailSource();
    broker_->RegisterSource(identity.toStdString(),
                            ThumbnailSourceInfo{access->Path(), *cache_identity, metadata->width, metadata->height,
                                                metadata->fps_num, metadata->fps_den, access, proof, bool(proof)});
    waveforms_->registerSource(*cache_identity, access, proof);
    registered_sources_[asset_id] = identity;
    refreshClipMetadata();
}

void EditTimelineAdapter::syncSourceRegistrations() {
    if (!session_ || (source_revision_ && *source_revision_ == session_->workspace().revision()))
        return;
    source_revision_ = session_->workspace().revision();
    if (session_->workspace().assets().empty()) {
        broker_->ReleaseSources();
        waveforms_->releaseSources();
        registered_sources_.clear();
        if (source_)
            source_->closeClip();
        ++source_registration_revision_;
        emit sourcesChanged();
        return;
    }
    for (const auto& asset : session_->workspace().assets())
        registerSessionSource(asset.id);
    ++source_registration_revision_;
    emit sourcesChanged();
}

void EditTimelineAdapter::ensureThumbnailSource() {
    if (source_.has_value())
        return;
    source_.emplace(broker_);
    source_->setAnalysisPressure(playback_pressure_, exclusive_pressure_);
    connect(&source_.value(), &TimelineThumbnailSource::brokerProgressChanged, this,
            &EditTimelineAdapter::tileProgressChanged);
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

int EditTimelineAdapter::tilesExpected() const {
    return fixture_tile_count_ ? tiles_expected_ : broker_->Coverage().expected;
}

int EditTimelineAdapter::tilesReady() const {
    return fixture_tile_count_ ? tiles_ready_ : broker_->Coverage().ready;
}

bool EditTimelineAdapter::generatingPreviews() const {
    if (previews_unavailable_)
        return false;
    if (fixture_tile_count_)
        return tiles_expected_ > 0 && tiles_ready_ < tiles_expected_;
    const auto coverage = broker_->Coverage();
    return coverage.ready + coverage.unavailable < coverage.expected;
}

bool EditTimelineAdapter::previewsUnavailable() const {
    if (previews_unavailable_)
        return true;
    if (fixture_tile_count_)
        return false;
    const auto coverage = broker_->Coverage();
    return coverage.expected > 0 && coverage.unavailable == coverage.expected;
}

QString EditTimelineAdapter::previewState() const {
    if (previews_unavailable_)
        return QStringLiteral("unavailable");
    if (fixture_tile_count_)
        return tiles_expected_ <= 0             ? QStringLiteral("idle")
               : tiles_ready_ < tiles_expected_ ? QStringLiteral("generating")
                                                : QStringLiteral("ready");
    const auto coverage = broker_->Coverage();
    if (!coverage.expected)
        return QStringLiteral("idle");
    if (coverage.unavailable == coverage.expected)
        return QStringLiteral("unavailable");
    return coverage.ready + coverage.unavailable < coverage.expected ? QStringLiteral("generating")
                                                                     : QStringLiteral("ready");
}

void EditTimelineAdapter::refreshClipMetadata() {
    if (!session_ || clip_path_.isEmpty() || fixture_tile_count_)
        return;
    for (const auto& asset : session_->workspace().assets()) {
        if (QString::fromStdWString(asset.path) != clip_path_)
            continue;
        if (const auto metadata = session_->sourceMetadata(asset.id)) {
            clip_video_width_ = metadata->width;
            clip_video_height_ = metadata->height;
            QStringList labels;
            for (const auto& track : metadata->audio_metadata.actual_tracks)
                labels.append(QString::fromStdString(track.name));
            setAudioTrackLabels(labels);
            scheduleTileRun();
        }
        break;
    }
}

void EditTimelineAdapter::setAudioTrackLabels(const QStringList& labels) {
    QStringList named;
    named.reserve(labels.size());
    for (int i = 0; i < labels.size(); ++i) {
        // An empty entry is labelled positionally rather than guessed from track
        // order, which the container does not define.
        named.append(labels.at(i).isEmpty() ? QCoreApplication::translate("EditTimelineAdapter", "Audio %1").arg(i + 1)
                                            : labels.at(i));
    }
    if (named == audio_track_labels_)
        return;
    audio_track_labels_ = named;
    emit audioTracksChanged();
}

void EditTimelineAdapter::openClip(const QString& master_path, qint64 duration_ms) {
    invalidateRun();
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
    refreshClipMetadata();
    emit tileProgressChanged();
    refreshMarkers();
    scheduleTileRun();
}

void EditTimelineAdapter::closeClip() {
    invalidateRun();
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
    if (fixture_tile_count_)
        resize_debounce_.start();
}

void EditTimelineAdapter::startTileRun() {
    if (!fixture_tile_count_)
        return;
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

void EditTimelineAdapter::configureThumbnailCache(const ThumbnailRequestBroker::Config& config) {
    if (source_.has_value())
        return;
    broker_ = std::make_shared<ThumbnailRequestBroker>(config);
}

void EditTimelineAdapter::configureWaveformCache(const WaveformCacheService::Config& config) {
    waveforms_ = std::make_unique<WaveformCacheService>(config, WaveformCacheService::DecoderFactory{});
}

void EditTimelineAdapter::useSyntheticMediaForHarness(ThumbnailRequestBroker::SyntheticImages images,
                                                      WaveformCacheService::DecoderFactory decoders,
                                                      const std::filesystem::path& waveform_root) {
    broker_->UseSyntheticImagesForHarness(std::move(images));
    WaveformCacheService::Config config;
    config.root = waveform_root;
    waveforms_ = std::make_unique<WaveformCacheService>(config, std::move(decoders));
}

bool EditTimelineAdapter::registerThumbnailSource(const QString& identityHex, const QString& path, int videoWidth,
                                                  int videoHeight, int fpsNum, int fpsDen) {
    const auto identity = ParseContentIdentity(identityHex);
    if (!identity || path.isEmpty())
        return false;
    ensureThumbnailSource();
    if (const auto prior = broker_->Source(identityHex.toLower().toStdString());
        prior && prior->path == std::filesystem::path(path.toStdWString()) &&
        (prior->access || (prior->video_width == videoWidth && prior->video_height == videoHeight &&
                           prior->fps_num == fpsNum && prior->fps_den == fpsDen)))
        return true;
    broker_->RegisterSource(identityHex.toLower().toStdString(),
                            ThumbnailSourceInfo{std::filesystem::path(path.toStdWString()), *identity, videoWidth,
                                                videoHeight, fpsNum, fpsDen});
    ++source_registration_revision_;
    emit sourcesChanged();
    return true;
}

QVariantList EditTimelineAdapter::thumbnailTiles(const QString& identityHex, qint64 sourceInUs, qint64 sourceOutUs,
                                                 qreal widthPx, int heightPx) {
    ++thumbnail_queries_;
    QVariantList result;
    const QString identity = identityHex.toLower();
    const auto source = broker_->Source(identity.toStdString());
    if (!source)
        return result;
    const auto tiles = ThumbnailTilesForWindow(sourceInUs, sourceOutUs, widthPx, heightPx, source->video_width,
                                               source->video_height, source->fps_num, source->fps_den);
    result.reserve(static_cast<qsizetype>(tiles.size()));
    for (const auto& tile : tiles) {
        // The source time is the tile's identity within one strip, so a keyed
        // model keeps the tiles a changed window still shows.
        result.append(QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(tile.time_us)},
                                  {QStringLiteral("timeUs"), QVariant::fromValue<qint64>(tile.time_us)},
                                  {QStringLiteral("x"), tile.x},
                                  {QStringLiteral("width"), tile.width},
                                  {QStringLiteral("source"), EditThumbnailUrl(identity, tile.time_us, heightPx)}});
    }
    // One window of lookahead on each side covers a pan or scroll by a view
    // width without waiting for a decode.
    constexpr std::size_t kPrefetchLimit = 64;
    const qint64 span = sourceOutUs - sourceInUs;
    if (span > 0 && widthPx > 0) {
        const auto times = ThumbnailGridTimes(std::max<qint64>(0, sourceInUs - span), sourceOutUs + span,
                                              static_cast<double>(span) / widthPx, heightPx, source->video_width,
                                              source->video_height, source->fps_num, source->fps_den, kPrefetchLimit);
        broker_->Prefetch(identity.toStdString(), times, heightPx);
    }
    return result;
}

QString EditTimelineAdapter::posterForIdentity(const QString& identityHex, int heightPx) const {
    if (!broker_->Source(identityHex.toLower().toStdString()))
        return {};
    constexpr std::array<int, 5> buckets{32, 48, 64, 96, 128};
    const auto desired = std::clamp(heightPx, 1, 128);
    return EditThumbnailUrl(identityHex, 0, *std::lower_bound(buckets.begin(), buckets.end(), desired));
}

void EditTimelineAdapter::retireThumbnailInterest(qulonglong clipInterestId) {
    clip_thumbnail_interests_.erase(clipInterestId);
    broker_->RemoveInterest(clipInterestId);
    waveforms_->removeVisibleInterest(clipInterestId);
    emit tileProgressChanged();
}

QVariantList EditTimelineAdapter::thumbnailTilesForClip(qulonglong clipInterestId, const QString& identityHex,
                                                        qint64 sourceInUs, qint64 sourceOutUs, qreal widthPx,
                                                        int heightPx, qreal devicePixelRatio, qint64 playheadSourceUs,
                                                        qint64 trimSourceUs) {
    ++thumbnail_queries_;
    QVariantList result;
    const auto source = broker_->Source(identityHex.toLower().toStdString());
    if (!source || !clipInterestId || !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0)
        return result;
    constexpr std::array<int, 5> buckets{32, 48, 64, 96, 128};
    const auto desired = static_cast<int>(std::clamp(std::ceil(heightPx * devicePixelRatio), 1.0, 128.0));
    const auto bucket = *std::lower_bound(buckets.begin(), buckets.end(), desired);
    const auto tiles = ThumbnailTilesForWindow(sourceInUs, sourceOutUs, widthPx, heightPx, source->video_width,
                                               source->video_height, source->fps_num, source->fps_den);
    std::vector<ThumbnailRequestBroker::InterestTile> interests;
    result.reserve(static_cast<qsizetype>(tiles.size()));
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        const auto& tile = tiles[index];
        const int priority = index == 0 || index + 1 == tiles.size() ? 1 : index % 4 == 0 ? 2 : 3;
        interests.push_back(
            {tile.time_us, bucket, priority, std::abs(tile.time_us - (sourceInUs + (sourceOutUs - sourceInUs) / 2))});
        result.append(QVariantMap{{QStringLiteral("id"), QVariant::fromValue<qulonglong>(tile.time_us)},
                                  {QStringLiteral("timeUs"), QVariant::fromValue<qint64>(tile.time_us)},
                                  {QStringLiteral("x"), tile.x},
                                  {QStringLiteral("width"), tile.width},
                                  {QStringLiteral("source"), EditThumbnailUrl(identityHex, tile.time_us, bucket)}});
    }
    const auto span = sourceOutUs - sourceInUs;
    if (span > 0 && widthPx > 0) {
        const auto lookahead = ThumbnailGridTimes(std::max<qint64>(0, sourceInUs - span), sourceOutUs + span,
                                                  static_cast<double>(span) / widthPx, heightPx, source->video_width,
                                                  source->video_height, source->fps_num, source->fps_den, 128);
        for (const auto time : lookahead)
            if (time < sourceInUs || time >= sourceOutUs)
                interests.push_back({time, bucket, 4});
    }
    clip_thumbnail_interests_[clipInterestId] = {identityHex.toLower().toStdString(), sourceInUs, sourceOutUs,
                                                 std::move(interests)};
    updateThumbnailRelevance(clipInterestId, playheadSourceUs, trimSourceUs);
    return result;
}

void EditTimelineAdapter::updateThumbnailRelevance(qulonglong clipInterestId, qint64 playheadSourceUs,
                                                   qint64 trimSourceUs) {
    const auto found = clip_thumbnail_interests_.find(clipInterestId);
    if (found == clip_thumbnail_interests_.end())
        return;
    const auto& interest = found->second;
    auto tiles = interest.tiles;
    for (int target = 0; target < 2; ++target) {
        const auto urgent = target == 0 ? playheadSourceUs : trimSourceUs;
        if (urgent < 0 || (target == 0 && (urgent < interest.from_us || urgent >= interest.to_us)))
            continue;
        auto nearest = tiles.end();
        for (auto tile = tiles.begin(); tile != tiles.end(); ++tile)
            if ((target == 1 || tile->priority <= 3) &&
                (nearest == tiles.end() || std::abs(tile->time_us - urgent) < std::abs(nearest->time_us - urgent)))
                nearest = tile;
        if (nearest != tiles.end())
            nearest->priority = 1;
    }
    broker_->ReplaceInterest(clipInterestId, interest.identity, tiles, ++view_generation_);
    emit tileProgressChanged();
}

void EditTimelineAdapter::setForegroundBusy(bool busy) {
    setAnalysisPressure(false, busy);
}

void EditTimelineAdapter::setAnalysisPressure(bool playback, bool exclusive) {
    waveforms_->setAnalysisPressure(playback, exclusive);
    playback_pressure_ = playback;
    exclusive_pressure_ = exclusive;
    if (source_.has_value())
        source_->setAnalysisPressure(playback, exclusive);
    else
        broker_->SetAnalysisPressure(playback, exclusive);
}

qint64 EditTimelineAdapter::clearThumbnailCache() {
    const auto released = static_cast<qint64>(broker_->Clear());
    emit mediaCachesCleared();
    ++source_registration_revision_;
    emit sourcesChanged();
    return released;
}

EditThumbnailImageProvider* EditTimelineAdapter::createThumbnailImageProvider() {
    return new EditThumbnailImageProvider(broker_);
}

QObject* EditTimelineAdapter::waveformService() const {
    return waveforms_.get();
}

QString EditTimelineAdapter::waveformSource(const QString& identityHex, const QString& path, int containerIndex,
                                            const QString& streamIdentityHex, qint64 sourceEndUs,
                                            qulonglong clipInterestId, qint64 distanceUs) {
    const auto identity = ParseContentIdentity(identityHex);
    if (!identity || path.isEmpty() || containerIndex < 0 || sourceEndUs <= 0)
        return {};
    engine::WaveformSourceKey source;
    source.content_sha256 = *identity;
    source.container_index = static_cast<std::uint32_t>(containerIndex);
    if (!streamIdentityHex.isEmpty()) {
        const auto bytes = QByteArray::fromHex(streamIdentityHex.toLatin1());
        if (bytes.size() != static_cast<qsizetype>(source.stream_identity.size()) ||
            bytes.toHex() != streamIdentityHex.toLower().toLatin1())
            return {};
        std::memcpy(source.stream_identity.data(), bytes.constData(), source.stream_identity.size());
    }
    const auto key = waveforms_->request(source, path, sourceEndUs);
    waveforms_->setVisibleInterest(clipInterestId, key, distanceUs);
    return key;
}

qint64 EditTimelineAdapter::clearMediaCaches() {
    const auto released = static_cast<qint64>(broker_->Clear()) + waveforms_->clearCache();
    emit mediaCachesCleared();
    ++source_registration_revision_;
    emit sourcesChanged();
    return released;
}

} // namespace exosnap::quick

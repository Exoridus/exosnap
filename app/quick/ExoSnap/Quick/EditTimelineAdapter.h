#pragma once

#include "EditThumbnailImageProvider.h"
#include "EditTimelineModels.h"

#include "services/ThumbnailCache.h"
#include "services/TimelineThumbnailSource.h"
#include "services/WaveformCacheService.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace exosnap::quick {

class EditSessionAdapter;

// Row geometry of the timeline stack. Lifted from the Widgets EditTimeline so
// both frontends stack the same way; the audio budget in particular is a product
// constraint (three unmerged rows must not squeeze the player out at the 700 px
// minimum window), not a painting detail.
inline constexpr int kTimelineVideoRowHeight = 48;
inline constexpr int kTimelineAudioRowHeight = 20;
inline constexpr int kTimelineAudioRowMinHeight = 12;
inline constexpr int kTimelineRowGap = 2;
inline constexpr int kTimelineAudioStackBudget = 3 * kTimelineAudioRowHeight + 2 * kTimelineRowGap;

// Height of one audio row when `row_count` of them share the stack budget.
[[nodiscard]] int TimelineAudioRowHeight(int row_count);
// Total height of the audio stack (rows plus the gap above each).
[[nodiscard]] int TimelineAudioStackHeight(int row_count);

// Whether a tile run may be started right now.
//
// The decoder source is reopened asynchronously, one clip behind the adapter:
// the adapter knows about clip B the moment the session announces it, while the
// source still has clip A open until the new clip's keyframe scan lands. A run
// started in that window would decode B's timestamps out of A's file and publish
// the result as B's strip. So the run waits until the source is provably on the
// clip the adapter is showing.
[[nodiscard]] bool TimelineTileRunAllowed(bool has_source, const QString& clip_path, const QString& source_clip_path,
                                          int tile_count);

// Feeds the timeline: the thumbnail strip, the audio row labels and the marker
// verticals.
//
// The strip has no cache anywhere in the stack -- TimelineThumbnailSource
// re-decodes every run -- so a run is started once per (clip, track width) and
// its tiles are published through an image provider keyed by run id. The
// "Generating previews…" state was derived inside the Widgets paintEvent, which
// QML cannot do; it is `tilesExpected` / `tilesReady` here.
class EditTimelineAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditTimelineAdapter is provided by the application")

    // Declared as the Qt base type: qmltyperegistrar records the concrete subclass
    // under its namespaced C++ name while moc writes it unqualified, so a concrete
    // spelling here is unresolvable for qmllint.
    Q_PROPERTY(QAbstractItemModel* tileModel READ tileModel CONSTANT FINAL)
    Q_PROPERTY(QAbstractItemModel* markerModel READ markerModel CONSTANT FINAL)
    Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY tileProgressChanged FINAL)
    Q_PROPERTY(QString posterSource READ posterSource NOTIFY tileProgressChanged FINAL)
    Q_PROPERTY(QObject* waveformService READ waveformService CONSTANT FINAL)

    Q_PROPERTY(int trackWidth READ trackWidth WRITE setTrackWidth NOTIFY trackWidthChanged FINAL)
    Q_PROPERTY(int tileWidth READ tileWidth NOTIFY layoutChanged FINAL)
    Q_PROPERTY(int videoRowHeight READ videoRowHeight CONSTANT FINAL)
    Q_PROPERTY(int audioRowHeight READ audioRowHeight NOTIFY audioTracksChanged FINAL)
    Q_PROPERTY(int audioStackHeight READ audioStackHeight NOTIFY audioTracksChanged FINAL)
    Q_PROPERTY(QStringList audioTrackLabels READ audioTrackLabels NOTIFY audioTracksChanged FINAL)

    Q_PROPERTY(int tilesExpected READ tilesExpected NOTIFY tileProgressChanged FINAL)
    Q_PROPERTY(int tilesReady READ tilesReady NOTIFY tileProgressChanged FINAL)
    Q_PROPERTY(bool generatingPreviews READ generatingPreviews NOTIFY tileProgressChanged FINAL)
    // QCR-307. The strip's lifecycle as a state rather than as an inference from
    // two counters:
    //
    //   "idle"        — no clip, or nothing to lay out yet.
    //   "generating"  — tiles are expected and still arriving.
    //   "ready"       — every expected tile is on the strip.
    //   "unavailable" — this clip carries nothing decodable, and no tile will
    //                   ever arrive. TERMINAL, and reached only from a run that
    //                   ran to the end, never from a cancel or a clip switch.
    //
    // The two counters alone could not express the last one: an undecodable clip
    // sits at tilesExpected > 0 / tilesReady == 0 exactly like a clip that is
    // mid-decode, so "Generating previews…" stayed on screen for the rest of the
    // session for something that was never going to finish.
    Q_PROPERTY(QString previewState READ previewState NOTIFY tileProgressChanged FINAL)
    Q_PROPERTY(bool previewsUnavailable READ previewsUnavailable NOTIFY tileProgressChanged FINAL)

  public:
    explicit EditTimelineAdapter(QObject* parent = nullptr);
    ~EditTimelineAdapter() override;

    // The provider is owned by the QML engine (addImageProvider takes ownership).
    // Only ever dereferenced on the GUI thread, from queued tile deliveries.
    void setTileProvider(EditTimelineTileProvider* provider);
    void setSession(EditSessionAdapter* session);

    [[nodiscard]] QAbstractItemModel* tileModel() noexcept;
    [[nodiscard]] const QString& sourcePath() const {
        return clip_path_;
    }
    [[nodiscard]] QString posterSource() const;
    [[nodiscard]] QAbstractItemModel* markerModel() noexcept;

    [[nodiscard]] int trackWidth() const noexcept;
    void setTrackWidth(int width);
    [[nodiscard]] int tileWidth() const noexcept;
    [[nodiscard]] static int videoRowHeight() noexcept;
    [[nodiscard]] int audioRowHeight() const noexcept;
    [[nodiscard]] int audioStackHeight() const noexcept;
    [[nodiscard]] const QStringList& audioTrackLabels() const noexcept;

    [[nodiscard]] int tilesExpected() const noexcept;
    [[nodiscard]] int tilesReady() const noexcept;
    [[nodiscard]] bool generatingPreviews() const noexcept;
    [[nodiscard]] QString previewState() const;
    [[nodiscard]] bool previewsUnavailable() const noexcept;

    // Harness / test seam: replaces the strip with `tile_count` deterministic
    // placeholder tiles (-1 = as many as the row holds) and the given audio rows.
    // A real decode needs a clip no fixture carries.
    void setFixture(const QStringList& audio_track_labels, int tile_count);

    // Harness seam (--visual-test): puts the strip in its terminal "this clip
    // carries nothing decodable" state. Not reachable through setFixture, which
    // bypasses the decoder that produces the verdict.
    void applyUnavailablePreviewsForHarness();

    // Test seam: the same delivery path a decoded tile takes, without a decoder.
    // Exists because the generation check that drops a previous clip's tiles can
    // otherwise only be reached through real media.
    void deliverTileForTest(qint64 time_ms, const QImage& image, quint64 run_id);
    // Test seam: the run whose tiles the strip currently accepts. 0 means none.
    [[nodiscard]] quint64 activeRunForTest() const noexcept;
    // Test seams for QCR-307's two terminal endings. Both take the same path the
    // worker's signals do; a real one needs media a fixture cannot carry, and the
    // undecodable case needs media that is deliberately broken.
    void deliverRunFinishedForTest(quint64 run_id, int tiles_emitted, bool cancelled);
    void deliverClipOpenedForTest(int video_width, int video_height, const QStringList& audio_track_names);

    // ---- Per-clip thumbnails ----
    //
    // Tiles for any visible clip, keyed by the source's validated content
    // identity rather than by the single clip the strip above follows. Tiles are
    // drawn through the image provider created by createThumbnailImageProvider(),
    // which must be installed under kEditThumbnailProviderId.

    // Replaces the thumbnail cache configuration. Only effective before the
    // first source is registered or clip opened; a test seam and the hook for a
    // non-default cache root.
    void configureThumbnailCache(const ThumbnailRequestBroker::Config& config);
    // Configure before loading QML or requesting waveforms; existing service pointers become invalid.
    void configureWaveformCache(const WaveformCacheService::Config& config);
    // Harness seam (--visual-test): thumbnail tiles are drawn by `images` and
    // waveforms analysed from `decoders` into `waveform_root`, through the same
    // broker, service and image provider the editor uses. Same timing rule as
    // configureWaveformCache.
    void useSyntheticMediaForHarness(ThumbnailRequestBroker::SyntheticImages images,
                                     WaveformCacheService::DecoderFactory decoders,
                                     const std::filesystem::path& waveform_root);

    // Makes a source drawable. `identityHex` is the 64-character content SHA-256.
    // Frame size and rate shape the tile grid; 0 means unknown. Returns false
    // for a malformed identity.
    Q_INVOKABLE bool registerThumbnailSource(const QString& identityHex, const QString& path, int videoWidth,
                                             int videoHeight, int fpsNum, int fpsDen);

    // Tiles covering one clip's source window drawn `widthPx` wide and
    // `heightPx` high: a list of {id, timeUs, x, width, source}, `x` relative to
    // the clip's left edge, `source` an image URL. `id` is the tile's source
    // time and stays the same while a changed window still shows that tile,
    // so a keyed model can keep its image. Also queues a bounded
    // prefetch of the neighbouring window on either side. Empty for an
    // unregistered source.
    Q_INVOKABLE QVariantList thumbnailTiles(const QString& identityHex, qint64 sourceInUs, qint64 sourceOutUs,
                                            qreal widthPx, int heightPx);

    // Pauses thumbnail decoding (strip and per-clip) between frames while
    // playback, export or recording is active. Cached tiles are still served.
    Q_INVOKABLE void setForegroundBusy(bool busy);

    // Deletes every persisted thumbnail and the memory tier. Returns released
    // disk bytes.
    Q_INVOKABLE qint64 clearThumbnailCache();

    // A new provider for QQmlEngine::addImageProvider(kEditThumbnailProviderId),
    // which takes ownership. It shares this adapter's broker and stays valid
    // after the adapter is destroyed; requests then resolve as unavailable.
    [[nodiscard]] EditThumbnailImageProvider* createThumbnailImageProvider();
    [[nodiscard]] QObject* waveformService() const;
    Q_INVOKABLE QString waveformSource(const QString& identityHex, const QString& path, int containerIndex,
                                       const QString& streamIdentityHex, qint64 sourceEndUs);
    Q_INVOKABLE qint64 clearMediaCaches();

    [[nodiscard]] const std::shared_ptr<ThumbnailRequestBroker>& thumbnailBroker() const noexcept {
        return broker_;
    }
    [[nodiscard]] TimelineThumbnailSource* thumbnailSourceForTest() noexcept {
        return source_.has_value() ? &source_.value() : nullptr;
    }

  signals:
    void trackWidthChanged();
    void layoutChanged();
    void audioTracksChanged();
    void tileProgressChanged();

  private:
    void openClip(const QString& master_path, qint64 duration_ms);
    void closeClip();
    void handleTileReady(qint64 time_ms, const QImage& image, quint64 run_id);
    void handleClipOpened(int video_width, int video_height, const QStringList& audio_track_names);
    void handleRunFinished(quint64 run_id, int tiles_emitted, bool cancelled);
    // Drops the run in flight: nothing it still delivers belongs to what the
    // strip shows next.
    void invalidateRun();
    void refreshMarkers();
    void scheduleTileRun();
    void startTileRun();
    // Creates and wires the decoding thumbnail source on first use. Idempotent,
    // and deliberately not re-connected per clip — see the definition.
    void ensureThumbnailSource();
    void setAudioTrackLabels(const QStringList& labels);

    EditSessionAdapter* session_ = nullptr;
    EditTimelineTileProvider* provider_ = nullptr;
    EditTimelineTileModel tile_model_;
    EditMarkerModel marker_model_;
    // Declared before source_ so the worker detaches from it before it goes.
    std::shared_ptr<ThumbnailRequestBroker> broker_;
    std::unique_ptr<WaveformCacheService> waveforms_;
    bool foreground_busy_ = false;
    std::optional<TimelineThumbnailSource> source_;

    QTimer resize_debounce_;
    QString clip_path_;
    // The clip the decoder source was last asked to open. Empty means the source
    // holds nothing this adapter may decode from.
    QString source_clip_path_;
    QStringList audio_track_labels_;
    qint64 duration_ms_ = 0;
    int track_width_ = 0;
    int tile_width_ = 0;
    int tiles_expected_ = 0;
    int tiles_ready_ = 0;
    int clip_video_width_ = 0;
    int clip_video_height_ = 0;
    // The only run whose tiles the strip accepts. 0 means none: run ids start at
    // 1, so a clip switch or a close can silence every run in flight by resetting
    // this rather than by waiting for the worker.
    quint64 active_run_ = 0;
    // Set while placeholder tiles stand in for a decode (-1 = fill the row).
    std::optional<int> fixture_tile_count_;
    // QCR-307. Latched once this clip has PROVEN it carries nothing decodable:
    // the engine could not open it (or it has no video stream), or a run reached
    // its end having produced no tile at all. Cleared by the next clip and by the
    // start of any new run, so a resize or a reopen gets a fresh verdict.
    bool previews_unavailable_ = false;
};

} // namespace exosnap::quick

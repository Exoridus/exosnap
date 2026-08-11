#pragma once

#include "EditTimelineModels.h"

#include "services/TimelineThumbnailSource.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
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

    Q_PROPERTY(int trackWidth READ trackWidth WRITE setTrackWidth NOTIFY trackWidthChanged FINAL)
    Q_PROPERTY(int tileWidth READ tileWidth NOTIFY layoutChanged FINAL)
    Q_PROPERTY(int videoRowHeight READ videoRowHeight CONSTANT FINAL)
    Q_PROPERTY(int audioRowHeight READ audioRowHeight NOTIFY audioTracksChanged FINAL)
    Q_PROPERTY(int audioStackHeight READ audioStackHeight NOTIFY audioTracksChanged FINAL)
    Q_PROPERTY(QStringList audioTrackLabels READ audioTrackLabels NOTIFY audioTracksChanged FINAL)

    Q_PROPERTY(int tilesExpected READ tilesExpected NOTIFY tileProgressChanged FINAL)
    Q_PROPERTY(int tilesReady READ tilesReady NOTIFY tileProgressChanged FINAL)
    Q_PROPERTY(bool generatingPreviews READ generatingPreviews NOTIFY tileProgressChanged FINAL)

  public:
    explicit EditTimelineAdapter(QObject* parent = nullptr);
    ~EditTimelineAdapter() override;

    // The provider is owned by the QML engine (addImageProvider takes ownership).
    // Only ever dereferenced on the GUI thread, from queued tile deliveries.
    void setTileProvider(EditTimelineTileProvider* provider);
    void setSession(EditSessionAdapter* session);

    [[nodiscard]] QAbstractItemModel* tileModel() noexcept;
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

    // Harness / test seam: replaces the strip with `tile_count` deterministic
    // placeholder tiles (-1 = as many as the row holds) and the given audio rows.
    // A real decode needs a clip no fixture carries.
    void setFixture(const QStringList& audio_track_labels, int tile_count);

  signals:
    void trackWidthChanged();
    void layoutChanged();
    void audioTracksChanged();
    void tileProgressChanged();

  private:
    void openClip(const QString& master_path, qint64 duration_ms);
    void closeClip();
    void refreshMarkers();
    void scheduleTileRun();
    void startTileRun();
    void setAudioTrackLabels(const QStringList& labels);

    EditSessionAdapter* session_ = nullptr;
    EditTimelineTileProvider* provider_ = nullptr;
    EditTimelineTileModel tile_model_;
    EditMarkerModel marker_model_;
    std::optional<TimelineThumbnailSource> source_;

    QTimer resize_debounce_;
    QString clip_path_;
    QStringList audio_track_labels_;
    qint64 duration_ms_ = 0;
    int track_width_ = 0;
    int tile_width_ = 0;
    int tiles_expected_ = 0;
    int tiles_ready_ = 0;
    int clip_video_width_ = 0;
    int clip_video_height_ = 0;
    quint64 active_run_ = 0;
    // Set while placeholder tiles stand in for a decode (-1 = fill the row).
    std::optional<int> fixture_tile_count_;
};

} // namespace exosnap::quick

#pragma once

#include "models/RecordingMarker.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QtQmlIntegration/qqmlintegration.h>

#include <QList>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace exosnap::quick {

class EditSessionAdapter;

// URI scheme the tile model's `source` role resolves against.
inline constexpr char kEditTileProviderId[] = "exoedittile";

// Upper bound on marker rectangles the timeline ever instantiates. A session may
// legitimately carry up to kMaxRecordingMarkers (10 000); a delegate per marker
// would then be 10 000 QML items over a 48 px strip, every one of them narrower
// than a pixel. Markers that collapse onto the same pixel column are dropped
// here, in C++, so the view can never see more than it can draw.
inline constexpr int kMaxRenderedMarkers = 512;

// Decoded tile strip for the timeline's video row. The pixels themselves never
// enter a model role -- a QImage in a role is copied into a QVariant on every
// data() call and again into the delegate. Delegates get a provider URL keyed by
// run id and index instead, and the provider hands out the one decoded copy.
class EditTimelineTileModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditTimelineTileModel is provided by EditTimelineAdapter")

  public:
    enum Role {
        SourceRole = Qt::UserRole + 1,
        TimeMsRole,
    };

    explicit EditTimelineTileModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Starts a new run: every previously published tile is dropped, and the
    // delegates' provider URLs change with the run id so no stale image is
    // resolved out of the provider cache.
    void beginRun(quint64 run_id);
    void appendTile(qint64 time_ms);
    void clear();
    [[nodiscard]] quint64 runId() const noexcept;

  private:
    struct Tile {
        qint64 time_ms = 0;
    };

    std::vector<Tile> tiles_;
    quint64 run_id_ = 0;
};

// Marker verticals. Render-only: markers are not editable on this surface.
class EditMarkerModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditMarkerModel is provided by EditTimelineAdapter")

  public:
    enum Role {
        TimeMsRole = Qt::UserRole + 1,
        LabelRole,
        TypeRole,
    };

    explicit EditMarkerModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Re-derives the visible set from `markers` for a track `track_width` px
    // wide over `duration_ms`. Both inputs matter: the same marker list thins
    // differently on a narrow rail than on a wide one.
    void setMarkers(const std::vector<RecordingMarker>& markers, qint64 duration_ms, int track_width);

  private:
    std::vector<RecordingMarker> visible_;
};

// Thins `markers` down to what a `track_width` px track can actually show:
// at most one per pixel column, at most kMaxRenderedMarkers in total. Pure, so
// the thinning rule is testable without a view.
[[nodiscard]] std::vector<RecordingMarker> VisibleTimelineMarkers(const std::vector<RecordingMarker>& markers,
                                                                  qint64 duration_ms, int track_width);

// The clip rows a timeline view currently shows, keyed by clip id. A sync turns
// a fresh row list into removals, moves, insertions and per-row changes instead
// of a reset, so a clip's delegate (with its thumbnail and waveform requests)
// survives every edit, selection change and scroll that keeps the clip in view.
// The single `row` role surfaces as the delegate's modelData.
class EditTimelineClipModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    Q_MOC_INCLUDE("EditSessionAdapter.h")

  public:
    enum Role {
        RowRole = Qt::UserRole + 1,
    };

    explicit EditTimelineClipModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // Replaces the rows with `session`'s clips intersecting [fromMs, toMs].
    // A null session clears the model.
    Q_INVOKABLE void sync(EditSessionAdapter* session, qint64 fromMs, qint64 toMs);
    Q_INVOKABLE void syncUs(EditSessionAdapter* session, qint64 fromUs, qint64 toUs);
    // Explicit capture IDs remain resident when navigation moves them outside either axis.
    Q_INVOKABLE void syncViewportUs(EditSessionAdapter* session, qint64 fromUs, qint64 toUs,
                                    const QVariantList& visibleTrackIds, const QVariantList& retainedClipIds = {});
    [[nodiscard]] quint64 syncCountForTest() const noexcept {
        return sync_count_;
    }
    // Same, from rows that each carry a unique "id". Rows whose id survives keep
    // their delegate; only changed rows are reported as changed.
    Q_INVOKABLE void setRows(const QVariantList& rows);

  private:
    quint64 sync_count_ = 0;
    std::vector<std::pair<qulonglong, QVariantMap>> rows_;
};

class EditFolderModel final : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditFolderModel is provided by EditSessionAdapter")

  public:
    explicit EditFolderModel(QString settings_path = {}, QObject* parent = nullptr);
    ~EditFolderModel() override;
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE void addRoot(const QUrl& directory);
    Q_INVOKABLE void toggle(qulonglong entry_id);
    Q_INVOKABLE void refresh(qulonglong entry_id);
    Q_INVOKABLE void removeRoot(qulonglong entry_id);
    Q_INVOKABLE void inspectDirectory(const QUrl& directory);
    static QList<QUrl> DirectoryCandidates(const QUrl& directory, const std::shared_ptr<std::atomic<bool>>& cancelled,
                                           QString& reason);

  signals:
    void candidatesReady(const QUrl& directory, const QVariantList& files, const QString& reason);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Hands out decoded tiles by "<run id>/<index>". Thread-safe: Qt resolves image
// provider requests off the GUI thread.
class EditTimelineTileProvider final : public QQuickImageProvider {
  public:
    EditTimelineTileProvider();

    [[nodiscard]] QImage requestImage(const QString& id, QSize* size, const QSize& requested_size) override;

    // Publishes one tile of `run_id`. Tiles of any older run are discarded, so
    // the provider holds one strip at a time rather than growing per resize.
    void submitTile(quint64 run_id, int index, QImage image);
    void clear();

  private:
    mutable QMutex mutex_;
    QHash<int, QImage> tiles_;
    quint64 run_id_ = 0;
};

} // namespace exosnap::quick

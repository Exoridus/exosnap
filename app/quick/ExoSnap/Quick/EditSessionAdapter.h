#pragma once

#include "models/EditContext.h"
#include "models/EditWorkspace.h"

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <exosnap/engine/mp4_remuxer.h>

#include <cstdint>
#include <vector>

namespace exosnap::quick {

// Window a trim boundary is pulled onto a marker inside. Lifted from
// EditExportPage::onTrimHandleReleased, where it was a bare 50000 µs literal.
inline constexpr int64_t kMarkerSnapWindowUs = 50000;

// Snap one requested trim boundary: first back to the nearest keyframe at or
// before it (a stream copy can only cut on a keyframe), then forward or back
// onto a marker within kMarkerSnapWindowUs. Pure, so the ordering of the two
// snaps is testable without a clip, a decoder, or a widget.
[[nodiscard]] int64_t SnapTrimBoundaryUs(int64_t requested_us, const std::vector<int64_t>& keyframes_us,
                                         const std::vector<RecordingMarker>& markers);

// Owns the app-lifetime C++ workspace and exposes presentation projections.
// Container probing and keyframe indexing run on a serialized worker pool.
class EditSessionAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("EditSessionAdapter is provided by the application")

    Q_PROPERTY(bool open READ open NOTIFY openChanged FINAL)
    Q_PROPERTY(QString clipTitle READ clipTitle NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString clipPath READ clipPath NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString playerMetaText READ playerMetaText NOTIFY clipChanged FINAL)
    Q_PROPERTY(QVariantList facts READ facts NOTIFY clipChanged FINAL)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged FINAL)

    Q_PROPERTY(qint64 trimStartMs READ trimStartMs NOTIFY trimChanged FINAL)
    Q_PROPERTY(qint64 trimEndMs READ trimEndMs NOTIFY trimChanged FINAL)
    Q_PROPERTY(bool trimmed READ trimmed NOTIFY trimChanged FINAL)
    Q_PROPERTY(bool trimSnapReady READ trimSnapReady NOTIFY trimSnapReadyChanged FINAL)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged FINAL)

    Q_PROPERTY(int reportSeverity READ reportSeverityValue NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString reportLabel READ reportLabel NOTIFY reportChanged FINAL)
    Q_PROPERTY(QString reportTooltip READ reportTooltip NOTIFY reportChanged FINAL)

    Q_PROPERTY(bool exportRunning READ exportRunning NOTIFY exportRunningChanged FINAL)
    Q_PROPERTY(bool hasUnsavedEdits READ hasUnsavedEdits NOTIFY unsavedEditsChanged FINAL)
    Q_PROPERTY(QVariantList media READ media NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(qulonglong selectedClip READ selectedClip NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(QString workspaceError READ workspaceError NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(QVariantList transitions READ transitions NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(qulonglong selectedTransition READ selectedTransition NOTIFY workspaceChanged FINAL)

  public:
    // Severity of the post-flight report, as carried by the header badge.
    enum ReportSeverity {
        Neutral = 0, // Good / Unavailable / no snapshot: quiet info glyph
        Warning = 1, // amber, short label
        Critical = 2 // coral, short label
    };
    Q_ENUM(ReportSeverity)

    explicit EditSessionAdapter(QObject* parent = nullptr);
    ~EditSessionAdapter() override;

    [[nodiscard]] const edit::Workspace& workspace() const {
        return workspace_;
    }
    [[nodiscard]] QVariantList media() const;
    [[nodiscard]] QVariantList tracks() const;
    [[nodiscard]] bool canUndo() const {
        return workspace_.canUndo();
    }
    [[nodiscard]] bool canRedo() const {
        return workspace_.canRedo();
    }
    [[nodiscard]] qulonglong selectedClip() const {
        return workspace_.selection();
    }
    [[nodiscard]] const QString& workspaceError() const {
        return workspace_error_;
    }
    Q_INVOKABLE QVariantList visibleClips(qint64 from_ms, qint64 to_ms) const;
    QVariantList transitions() const;
    qulonglong selectedTransition() const {
        return selected_transition_;
    }
    Q_INVOKABLE void selectTransition(qulonglong outgoing);
    Q_INVOKABLE bool applyCrossfade(qulonglong outgoing = 0, qint64 duration_ms = 500);
    Q_INVOKABLE bool canCrossfade(qulonglong outgoing = 0) const;
    Q_INVOKABLE bool dropCrossfade(qint64 at_ms, qint64 tolerance_ms);
    Q_INVOKABLE void removeTransition();
    Q_INVOKABLE void selectClip(qulonglong id);
    Q_INVOKABLE void appendAsset(qulonglong id, qint64 at_ms = -1);
    Q_INVOKABLE void importMedia(const QUrl& url, bool append = false, qint64 at_ms = -1);
    Q_INVOKABLE void importMediaBatch(const QList<QUrl>& urls, bool append = false, qint64 at_ms = -1);
    Q_INVOKABLE void addHistory(const QString& path, qint64 at_ms = -1);
    Q_INVOKABLE void moveClip(qulonglong id, qint64 at_ms, bool snap = true);
    Q_INVOKABLE void trimClip(qulonglong id, qint64 in_ms, qint64 out_ms);
    Q_INVOKABLE void splitSelected();
    Q_INVOKABLE void deleteSelected(bool ripple = false);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void selectAdjacentClip(int direction);
    Q_INVOKABLE void nudgeSelected(qint64 delta_ms, int edge = 0);

    // Primary entry point. Resets trim, markers, report and position, and starts
    // the asynchronous keyframe index read.
    void setEditContext(const EditContext& context, qint64 timeline_start_ms = -1);
    [[nodiscard]] const EditContext& editContext() const noexcept;

    [[nodiscard]] bool open() const noexcept;
    [[nodiscard]] QString clipTitle() const;
    [[nodiscard]] const QString& clipPath() const noexcept;
    [[nodiscard]] QString playerMetaText() const;
    [[nodiscard]] const QVariantList& facts() const noexcept;
    [[nodiscard]] qint64 durationMs() const noexcept;

    [[nodiscard]] qint64 trimStartMs() const noexcept;
    [[nodiscard]] qint64 trimEndMs() const noexcept;
    [[nodiscard]] bool trimmed() const noexcept;
    [[nodiscard]] bool trimSnapReady() const noexcept;
    [[nodiscard]] qint64 positionMs() const noexcept;

    // Authoritative export range. TrimRange::kNoTimestamp means "no cut here".
    [[nodiscard]] int64_t trimStartUs() const noexcept;
    [[nodiscard]] int64_t trimEndUs() const noexcept;
    [[nodiscard]] const std::vector<RecordingMarker>& markers() const noexcept;
    // Sorted cue table of the open clip, empty until the index read lands.
    [[nodiscard]] const std::vector<int64_t>& keyframeTimestamps() const noexcept;

    [[nodiscard]] int reportSeverityValue() const noexcept;
    [[nodiscard]] const QString& reportLabel() const noexcept;
    [[nodiscard]] QString reportTooltip() const;

    [[nodiscard]] bool exportRunning() const noexcept;
    // Mirrored from EditExportAdapter; the flag itself is owned there.
    void setExportRunning(bool running);
    [[nodiscard]] bool hasUnsavedEdits() const;

    // A trim handle was released at these millisecond positions. Clamps against
    // the other handle and the clip length, snaps, stores, and reports the frame
    // the boundary landed on through trimBoundaryPreviewRequested().
    Q_INVOKABLE void requestTrim(qint64 start_ms, qint64 end_ms);
    // Scrub / playhead move. Clamped to the clip; forwarded as a seek request.
    Q_INVOKABLE void requestSeek(qint64 position_ms);
    // Position reported back by the player's own clock (no seek is implied).
    void setPositionMs(qint64 position_ms);
    Q_INVOKABLE void close();

    // Presentation and clamping helpers, forwarded to models/EditTimelineModel.h
    // so the formats and the minimum handle gap have one definition rather than
    // a second, drifting one written in JavaScript.
    Q_INVOKABLE QString formatTimestamp(qint64 position_ms) const;
    Q_INVOKABLE QString formatClock(qint64 position_ms) const;
    Q_INVOKABLE qint64 clampTrimStartMs(qint64 requested_ms, qint64 trim_end_ms) const;
    Q_INVOKABLE qint64 clampTrimEndMs(qint64 requested_ms, qint64 trim_start_ms) const;

    // Test/harness seam: injects a keyframe table without touching a container.
    void setKeyframeTimestampsForTest(std::vector<int64_t> keyframes_us);

  signals:
    void workspaceChanged();
    void historyRequested(const QString& path, qint64 at_ms);
    void editPageRequested();
    void openChanged();
    void clipChanged();
    void durationChanged();
    void trimChanged();
    void trimSnapReadyChanged();
    void positionChanged();
    void reportChanged();
    void exportRunningChanged();
    void unsavedEditsChanged();

    // A clip was opened / cleared. The player and the timeline both hang off this
    // rather than reaching into the context themselves.
    void clipOpened(const QString& master_path, qint64 duration_ms);
    void clipClosed();
    // Show the frame at `position_ms` (scrub, or a released trim handle).
    void seekRequested(qint64 position_ms);
    void closeRequested();

  private:
    edit::Id selected_transition_ = 0;
    void publishWorkspace(bool changed);
    void applyReport(const EditContext& context);
    void rebuildFacts();
    void loadMarkers();
    void startKeyframeScan();
    void setTrimUs(int64_t start_us, int64_t end_us);

    EditContext context_;
    edit::Workspace workspace_;
    QString workspace_error_;
    QVariantList facts_;
    QString report_drops_text_;
    QString report_drift_text_;
    QString report_health_text_;
    QString report_label_;
    ReportSeverity report_severity_ = ReportSeverity::Neutral;

    std::vector<int64_t> keyframe_timestamps_;
    std::vector<RecordingMarker> markers_;
    int64_t trim_start_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    int64_t trim_end_us_ = exosnap::engine::TrimRange::kNoTimestamp;
    qint64 duration_ms_ = 0;
    qint64 position_ms_ = 0;
    quint64 clip_generation_ = 0;
    bool open_ = false;
    bool trim_snap_ready_ = false;
    bool export_running_ = false;

    // Declared last so it is destroyed FIRST: its destructor waits for the
    // in-flight index read, which still references the members above.
    QThreadPool keyframe_pool_;
};

} // namespace exosnap::quick

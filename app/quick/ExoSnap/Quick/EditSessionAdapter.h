#pragma once

#include "models/EditContext.h"
#include "models/EditSourceImport.h"
#include "models/EditWorkspace.h"

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <exosnap/engine/mp4_remuxer.h>

#include <cstdint>
#include <memory>
#include <optional>
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
    Q_PROPERTY(QUrl layoutSettingsLocation READ layoutSettingsLocation CONSTANT FINAL)
    Q_PROPERTY(bool deliveryPending READ deliveryPending NOTIFY clipChanged FINAL)
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
    Q_PROPERTY(qint64 positionUs READ positionUs NOTIFY positionChanged FINAL)

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
    Q_PROPERTY(bool snapping MEMBER snapping NOTIFY interactionSettingsChanged FINAL)
    Q_PROPERTY(bool ripple MEMBER ripple NOTIFY interactionSettingsChanged FINAL)
    Q_PROPERTY(bool autoCrossfade MEMBER autoCrossfade NOTIFY interactionSettingsChanged FINAL)
    Q_PROPERTY(bool linkedSelection MEMBER linkedSelection NOTIFY interactionSettingsChanged FINAL)
    Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragChanged FINAL)
    Q_PROPERTY(bool dragValid READ dragValid NOTIFY dragChanged FINAL)
    Q_PROPERTY(QVariantList dragClips READ dragClips NOTIFY dragChanged FINAL)
    Q_PROPERTY(QVariantList dragTracks READ dragTracks NOTIFY dragChanged FINAL)
    Q_PROPERTY(QVariantList dragTransitions READ dragTransitions NOTIFY dragChanged FINAL)
    Q_PROPERTY(QVariantMap selectedClipDetails READ selectedClipDetails NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(QVariantMap selectedJunctionDetails READ selectedJunctionDetails NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(qint64 timeSelectionInMs READ timeSelectionInMs NOTIFY timeSelectionChanged FINAL)
    Q_PROPERTY(qint64 timeSelectionOutMs READ timeSelectionOutMs NOTIFY timeSelectionChanged FINAL)
    Q_PROPERTY(qint64 timeSelectionInUs READ timeSelectionInUs NOTIFY timeSelectionChanged FINAL)
    Q_PROPERTY(qint64 timeSelectionOutUs READ timeSelectionOutUs NOTIFY timeSelectionChanged FINAL)
    Q_PROPERTY(QVariantList spacers READ spacers NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(QVariantList dragSpacers READ dragSpacers NOTIFY dragChanged FINAL)
    Q_PROPERTY(qulonglong selectedSpacer READ selectedSpacer NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(QVariantMap selectedGap READ selectedGap NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(bool fixedTimelineEnd READ fixedTimelineEnd NOTIFY workspaceChanged FINAL)

  public:
    bool snapping = true;
    bool ripple = false;
    bool autoCrossfade = true;
    bool linkedSelection = true;
    bool dragActive() const {
        return bool(drag_snapshot_);
    }
    bool dragValid() const {
        return drag_preview_.valid && drag_snapshot_ && workspace_.revision() == drag_snapshot_->revision;
    }
    QVariantList dragClips() const;
    QVariantList dragTracks() const;
    QVariantList dragTransitions() const;
    QVariantMap selectedClipDetails() const;
    QVariantMap selectedJunctionDetails() const;
    qint64 timeSelectionInUs() const {
        return time_selection_in_us_;
    }
    qint64 timeSelectionOutUs() const {
        return time_selection_out_us_;
    }
    qint64 timeSelectionInMs() const {
        return time_selection_in_us_ < 0 ? -1 : time_selection_in_us_ / 1000;
    }
    qint64 timeSelectionOutMs() const {
        return time_selection_out_us_ < 0 ? -1 : time_selection_out_us_ / 1000;
    }
    QVariantList spacers() const;
    QVariantList dragSpacers() const;
    qulonglong selectedSpacer() const {
        return selected_spacer_;
    }
    QVariantMap selectedGap() const;
    bool fixedTimelineEnd() const {
        return workspace_.snapshot()->timeline_end.mode == edit::TimelineEndMode::FixedEnd;
    }
    Q_INVOKABLE void selectSpacer(qulonglong id);
    Q_INVOKABLE void beginSpacerDrag(qulonglong id, int edge = -1);
    Q_INVOKABLE void selectGap(qulonglong track, qint64 at_ms);
    Q_INVOKABLE void copyBlankDuration();
    Q_INVOKABLE void pasteSpacer();
    Q_INVOKABLE void previewCloseGap();
    Q_INVOKABLE void setFixedTimelineEnd(bool fixed, qint64 time_ms);
    Q_INVOKABLE void beginClipDrag(qulonglong id, int edge = -1);
    Q_INVOKABLE void beginJunctionDrag(qulonglong outgoing, int edge);
    Q_INVOKABLE void updateClipDrag(qint64 delta_ms, qulonglong track = 0, int new_track = -1, bool insert = false,
                                    qint64 tolerance_ms = 100);
    Q_INVOKABLE bool commitClipDrag();
    Q_INVOKABLE void cancelClipDrag();
    Q_INVOKABLE void requestFrameStep(int direction);
    Q_INVOKABLE void setSelectionBoundary(bool out);
    Q_INVOKABLE void setJunctionWindow(qint64 start_ms, qint64 end_ms);
    Q_INVOKABLE void setJunctionCurve(int kind, double bias = 0);
    Q_INVOKABLE void setJunctionAudioLaw(int law);
    Q_INVOKABLE void setJunctionCut(qint64 at_ms);
    Q_INVOKABLE void setClipFade(qulonglong id, bool incoming, qint64 duration_ms);
    Q_INVOKABLE bool deleteTrack(qulonglong id, bool confirmed = false);
    // Severity of the post-flight report, as carried by the header badge.
    enum ReportSeverity {
        Neutral = 0, // Good / Unavailable / no snapshot: quiet info glyph
        Warning = 1, // amber, short label
        Critical = 2 // coral, short label
    };
    Q_ENUM(ReportSeverity)

    explicit EditSessionAdapter(QObject* parent = nullptr);
    ~EditSessionAdapter() override;
    [[nodiscard]] QUrl layoutSettingsLocation() const;

    [[nodiscard]] const edit::Workspace& workspace() const {
        return workspace_;
    }
    // Last command's model transaction and synchronous publication, in nanoseconds.
    [[nodiscard]] QVariantMap commandPerformance() const {
        return {{"modelNs", command_model_ns_}, {"publishNs", command_publish_ns_}};
    }
    void setAssetMonitorSelection(edit::Id asset, engine::MonitorSelectionSnapshot selection);
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
    // A media import or recording append is still probing its sources off the
    // GUI thread. Its result lands as a workspace change or a workspace error.
    [[nodiscard]] bool importPending() const noexcept {
        return pending_imports_ > 0;
    }
    // Advances once per timeline edit attempt that reached the model or was
    // refused by it, whether or not anything changed. Compared before and after
    // an intent, it separates "the edit was refused" from "nothing was
    // attempted", which the workspace error text alone cannot: that text
    // outlives the attempt that set it.
    [[nodiscard]] quint64 editOutcomeSequence() const noexcept {
        return edit_outcome_sequence_;
    }
    [[nodiscard]] bool lastEditAccepted() const noexcept {
        return last_edit_accepted_;
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
    Q_INVOKABLE void deleteSelected(bool ripple_delete = false);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void selectAdjacentClip(int direction);
    Q_INVOKABLE void nudgeSelected(qint64 delta_ms, int edge = 0);

    // Primary entry point. Resets trim, markers, report and position, and starts
    // the asynchronous keyframe index read.
    void setEditContext(const EditContext& context, qint64 timeline_start_ms = -1);
    [[nodiscard]] bool deliveryPending() const noexcept {
        return context_.delivery_pending;
    }
    void setDeliveryPending(bool pending);
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
    [[nodiscard]] qint64 positionUs() const noexcept {
        return workspace_.playhead();
    }

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
    Q_INVOKABLE void requestSeekUs(qint64 position_us);
    void setPositionUs(qint64 position_us);
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
    // Harness seam (--visual-test): adds the media an import of `path` would
    // add had its probe returned `metadata`, without reading the file. The
    // source is available; `append` also places it at the timeline end.
    edit::Id addProbedMediaForHarness(const QString& path, const engine::EditMediaMetadata& metadata, bool append);

  signals:
    void interactionSettingsChanged();
    void dragChanged();
    void timeSelectionChanged();
    void workspaceChanged();
    void importPendingChanged();
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
    void seekRequestedUs(qint64 position_us);
    void inspectionRequested();
    void closeRequested();

  private:
    qint64 command_model_ns_ = 0;
    qint64 command_publish_ns_ = 0;
    std::shared_ptr<const edit::EditSnapshot> drag_snapshot_;
    std::optional<edit::EditCommand> drag_command_;
    edit::EditPreview drag_preview_;
    edit::Id drag_clip_ = 0;
    edit::Id drag_junction_ = 0;
    edit::Id drag_spacer_ = 0;
    edit::Time drag_playhead_ = 0;
    edit::Id selected_spacer_ = 0;
    edit::Id selected_gap_track_ = 0;
    edit::Span selected_gap_;
    edit::Time copied_blank_duration_ = 0;
    edit::Id copied_blank_track_ = 0;
    edit::Id pending_track_delete_ = 0;
    edit::Revision pending_track_delete_revision_ = 0;
    int drag_edge_ = -1;
    qint64 time_selection_in_us_ = -1;
    qint64 time_selection_out_us_ = -1;
    int pending_imports_ = 0;
    quint64 edit_outcome_sequence_ = 0;
    bool last_edit_accepted_ = true;
    void beginImport();
    void endImport();
    // Every refusal that does not go through publishWorkspace(false).
    void refuseEdit(const QString& message);
    bool applyCommand(const edit::EditCommand& command);
    void applyEditContext(const EditContext& context, qint64 timeline_start_ms,
                          std::vector<edit::SourceImport> imports);
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

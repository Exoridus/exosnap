#pragma once

#include "models/EditClipboard.h"
#include "models/EditContext.h"
#include "models/EditPointerGeometry.h"
#include "models/EditSelection.h"
#include "models/EditSourceImport.h"
#include "models/EditWorkspace.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <exosnap/engine/mp4_remuxer.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace exosnap {
class BackgroundStorageBudget;
}

namespace exosnap::quick {

class EditTrackLayoutModel;
class EditTimelineClipModel;
class EditFolderModel;

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
    Q_MOC_INCLUDE("EditTrackLayoutModel.h")
    Q_MOC_INCLUDE("EditTimelineModels.h")

    Q_PROPERTY(bool open READ open NOTIFY openChanged FINAL)
    Q_PROPERTY(QUrl layoutSettingsLocation READ layoutSettingsLocation CONSTANT FINAL)
    Q_PROPERTY(bool deliveryPending READ deliveryPending NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString clipTitle READ clipTitle NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString clipPath READ clipPath NOTIFY clipChanged FINAL)
    Q_PROPERTY(QString playerMetaText READ playerMetaText NOTIFY clipChanged FINAL)
    Q_PROPERTY(QVariantList facts READ facts NOTIFY clipChanged FINAL)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationChanged FINAL)
    Q_PROPERTY(qint64 durationUs READ durationUs NOTIFY durationChanged FINAL)
    Q_PROPERTY(int defaultTimeFormat READ defaultTimeFormat NOTIFY workspaceChanged FINAL)

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
    Q_PROPERTY(QVariantList media READ media NOTIFY mediaChanged FINAL)
    Q_PROPERTY(exosnap::quick::EditTimelineClipModel* mediaModel READ mediaModel CONSTANT FINAL)
    Q_PROPERTY(exosnap::quick::EditFolderModel* folderModel READ folderModel CONSTANT FINAL)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(exosnap::quick::EditTrackLayoutModel* trackLayout READ trackLayout CONSTANT FINAL)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(qulonglong selectedClip READ selectedClip NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(int selectionMode READ selectionMode NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList selectedClipIds READ selectedClipIds NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantMap editRange READ editRange NOTIFY selectionChanged FINAL)
    Q_PROPERTY(qulonglong activeTrack READ activeTrack NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QVariantList soloTrackIds READ soloTrackIds NOTIFY auditionChanged FINAL)
    Q_PROPERTY(bool hasClipClipboard READ hasClipClipboard NOTIFY clipboardChanged FINAL)
    Q_PROPERTY(QString workspaceError READ workspaceError NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(bool hasPendingImportedMedia READ hasPendingImportedMedia NOTIFY pendingPlacementChanged FINAL)
    Q_PROPERTY(bool importPending READ importPending NOTIFY importPendingChanged FINAL)
    Q_PROPERTY(QVariantList transitions READ transitions NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(qulonglong selectedTransition READ selectedTransition NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(bool snapping READ snappingEnabled WRITE setSnapping NOTIFY interactionSettingsChanged FINAL)
    Q_PROPERTY(bool ripple READ rippleEnabled WRITE setRipple NOTIFY interactionSettingsChanged FINAL)
    Q_PROPERTY(
        bool autoCrossfade READ autoCrossfadeEnabled WRITE setAutoCrossfade NOTIFY interactionSettingsChanged FINAL)
    Q_PROPERTY(bool linkedSelection READ linkedSelectionEnabled WRITE setLinkedSelection NOTIFY
                   interactionSettingsChanged FINAL)
    Q_PROPERTY(bool rangeGestureActive READ rangeGestureActive NOTIFY rangeGestureChanged FINAL)
    Q_PROPERTY(bool rangeGestureValid READ rangeGestureValid NOTIFY rangeGestureChanged FINAL)
    Q_PROPERTY(bool rangeScopeReviewActive READ rangeScopeReviewActive NOTIFY rangeGestureChanged FINAL)
    Q_PROPERTY(QVariantMap rangeGestureCandidate READ rangeGestureCandidate NOTIFY rangeGestureChanged FINAL)
    Q_PROPERTY(QString rangeGestureReason READ rangeGestureReason NOTIFY rangeGestureChanged FINAL)
    Q_PROPERTY(bool dragActive READ dragActive NOTIFY dragChanged FINAL)
    Q_PROPERTY(bool dragValid READ dragValid NOTIFY dragChanged FINAL)
    Q_PROPERTY(qint64 dragDurationUs READ dragDurationUs NOTIFY dragChanged FINAL)
    Q_PROPERTY(QString dragReason READ dragReason NOTIFY dragChanged FINAL)
    Q_PROPERTY(QVariantList dragClips READ dragClips NOTIFY dragChanged FINAL)
    Q_PROPERTY(QVariantMap dragDonorIds READ dragDonorIds NOTIFY dragChanged FINAL)
    Q_PROPERTY(QVariantList dragTracks READ dragTracks NOTIFY dragChanged FINAL)
    Q_PROPERTY(QVariantList dragTransitions READ dragTransitions NOTIFY dragChanged FINAL)
    Q_PROPERTY(QVariantMap selectedClipDetails READ selectedClipDetails NOTIFY workspaceChanged FINAL)
    Q_PROPERTY(QVariantMap selectedClipSummary READ selectedClipSummary NOTIFY selectedClipSummaryChanged FINAL)
    Q_PROPERTY(QVariantList overviewSegments READ overviewSegments NOTIFY overviewChanged FINAL)
    Q_PROPERTY(QVariantList sourceAudioChoices READ sourceAudioChoices NOTIFY sourceAudioChoiceChanged FINAL)
    Q_PROPERTY(QVariantMap sourceAudioChoice READ sourceAudioChoice NOTIFY sourceAudioChoiceChanged FINAL)
    Q_PROPERTY(QVariantMap sourceReplacement READ sourceReplacement NOTIFY sourceReplacementChanged FINAL)
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
    enum RangeBandTarget {
        RangeBandNone = static_cast<int>(edit::RangePointerTarget::None),
        RangeBandCreate = static_cast<int>(edit::RangePointerTarget::Create),
        RangeBandStart = static_cast<int>(edit::RangePointerTarget::Start),
        RangeBandEnd = static_cast<int>(edit::RangePointerTarget::End),
        RangeBandBody = static_cast<int>(edit::RangePointerTarget::Body)
    };
    Q_ENUM(RangeBandTarget)
    enum CommandSelectionMode {
        NoSelection = static_cast<int>(edit::SelectionMode::None),
        ClipSelection = static_cast<int>(edit::SelectionMode::Clips),
        RangeSelection = static_cast<int>(edit::SelectionMode::Range),
        JunctionSelection = static_cast<int>(edit::SelectionMode::Junction),
        GapSelection = static_cast<int>(edit::SelectionMode::Gap),
        SpacerSelection = static_cast<int>(edit::SelectionMode::Spacer),
        TrackSelection = static_cast<int>(edit::SelectionMode::Track)
    };
    Q_ENUM(CommandSelectionMode)
    int selectionMode() const {
        return static_cast<int>(selection_.mode);
    }
    QVariantList selectedClipIds() const;
    QVariantMap editRange() const;
    qulonglong activeTrack() const {
        return selection_.active_track;
    }
    Q_INVOKABLE void selectClipWithModifiers(qulonglong id, bool toggle = false, bool extend = false);
    Q_INVOKABLE bool beginClipPointerSelection(qulonglong id, bool toggle = false, bool extend = false,
                                               bool preserve_selection = true);
    Q_INVOKABLE bool beginLanePointerSelection(qulonglong track_id, qint64 at_us, bool seek = true);
    // Explicit link-group edits on the selected clips, one undo step each.
    // Linking needs at least two clips.
    Q_INVOKABLE bool linkSelectedClips();
    Q_INVOKABLE bool unlinkSelectedClips();
    // Selects the clips a marquee touches, on top of `base_ids`. Selection only:
    // no undo step. `exact` skips link-group expansion for this selection.
    Q_INVOKABLE void selectClipArea(qint64 start_us, qint64 end_us, const QVariantList& track_ids,
                                    const QVariantList& base_ids, bool exact = false);
    Q_INVOKABLE bool hasPointerSelection() const {
        return bool(pointer_selection_);
    }
    Q_INVOKABLE void finishPointerSelection(bool cancel = false, bool dragged = false);
    Q_INVOKABLE bool executeTimelineAction(const QString& action);
    Q_INVOKABLE QVariantMap selectionBounds() const;
    Q_INVOKABLE bool beginClipGainDrag(qulonglong id, bool fine = false);
    Q_INVOKABLE void updateClipGainDrag(qreal delta_y);
    Q_INVOKABLE void updateClipFadeDragPixels(qreal delta_x, qreal pixels_per_us, bool fine = false);
    void setLoopSnapBounds(qint64 start_us, qint64 end_us);
    Q_INVOKABLE void selectAllClips();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void setActiveTrack(qulonglong id);
    Q_INVOKABLE bool setEditRangeUs(qint64 start_us, qint64 end_us, const QVariantList& track_ids,
                                    qint64 anchor_us = -1);
    Q_INVOKABLE void clearEditRange();
    Q_INVOKABLE bool applyRangeGain(double delta_db);
    Q_INVOKABLE QVariantMap parseTimeValue(const QString& text, qint64 current_us, bool sample_context = false,
                                           int format = -1) const;
    [[nodiscard]] int defaultTimeFormat() const;
    // Requested timecode uses exact seconds when no valid output frame rate is available.
    Q_INVOKABLE QString formatTimeValue(qint64 value_us, int format = 0) const;
    Q_INVOKABLE QVariantMap parseGainValue(const QString& text, bool track = false) const;
    Q_INVOKABLE QVariantMap parseCurveBiasValue(const QString& text) const;
    Q_INVOKABLE QString formatGainValue(double value) const;
    Q_INVOKABLE void setPendingDraftOwner(QObject* owner, qulonglong target_id = 0);
    Q_INVOKABLE void clearPendingDraftOwner(QObject* owner);
    Q_INVOKABLE bool preflightPendingDraft();
    Q_INVOKABLE bool applyTimeValue(qulonglong target_id, const QString& field, qint64 value_us);
    Q_INVOKABLE bool timeValueUsesSamples(qulonglong target_id, const QString& field) const;
    Q_INVOKABLE bool beginRangeGestureUs(qint64 anchor_us, const QVariantList& track_ids, bool extend_scope = false,
                                         bool scope_only = false);
    Q_INVOKABLE QVariantList rangeTrackSpan(qulonglong first_track, qulonglong last_track) const;
    Q_INVOKABLE void updateRangeGestureToTrackUs(qint64 endpoint_us, qulonglong current_track);
    Q_INVOKABLE void updateRangeGestureUs(qint64 endpoint_us, const QVariantList& current_span = {});
    Q_INVOKABLE bool commitRangeGesture();
    Q_INVOKABLE bool previewRangeScope(const QVariantList& track_ids);
    Q_INVOKABLE bool includeLinkedRangeTracks();
    bool rangeScopeReviewActive() const {
        return range_scope_review_;
    }
    Q_INVOKABLE void cancelRangeGesture();
    Q_INVOKABLE bool beginRangeBandGestureUs(qint64 at_us, qint64 hit_tolerance_us = 0);
    Q_INVOKABLE void updateRangeBandGestureUs(qint64 at_us);
    Q_INVOKABLE int rangeBandTarget() const {
        return range_band_target_;
    }
    bool rangeGestureActive() const {
        return bool(range_gesture_snapshot_);
    }
    bool rangeGestureValid() const;
    QVariantMap rangeGestureCandidate() const;
    QString rangeGestureReason() const;
    Q_INVOKABLE bool setClipGain(qulonglong id, double gain_db);
    Q_INVOKABLE bool setClipGains(const QVariantList& ids, double gain_db);
    Q_INVOKABLE bool setTrackGain(qulonglong id, double gain_db);
    Q_INVOKABLE bool setTrackEnabled(qulonglong id, bool enabled);
    Q_INVOKABLE bool setTrackName(qulonglong id, const QString& name);
    Q_INVOKABLE bool reorderTrack(qulonglong id, int index);
    Q_INVOKABLE bool setTrackLocked(qulonglong id, bool locked);
    Q_INVOKABLE bool trackLocked(qulonglong id) const {
        return workspace_.trackLocked(id);
    }
    QVariantList soloTrackIds() const;
    Q_INVOKABLE bool setTrackSolo(qulonglong id, bool solo);
    Q_INVOKABLE bool trackSolo(qulonglong id) const {
        return solo_tracks_.contains(id);
    }
    bool hasClipClipboard() const {
        return bool(clip_clipboard_);
    }
    Q_INVOKABLE bool copyClips();
    Q_INVOKABLE bool cutClips();
    Q_INVOKABLE bool previewPasteClipsUs(qint64 at_us, qulonglong track = 0, bool insert = false);
    Q_INVOKABLE bool pasteClips(bool insert = false);
    bool snappingEnabled() const {
        return snapping;
    }
    bool rippleEnabled() const {
        return ripple;
    }
    bool autoCrossfadeEnabled() const {
        return autoCrossfade;
    }
    bool linkedSelectionEnabled() const {
        return linkedSelection;
    }
    void setSnapping(bool value);
    void setRipple(bool value);
    void setAutoCrossfade(bool value);
    void setLinkedSelection(bool value);
    bool snapping = true;
    bool ripple = false;
    bool autoCrossfade = true;
    bool linkedSelection = true;
    bool dragActive() const {
        return bool(drag_snapshot_);
    }
    bool dragValid() const;
    qint64 dragDurationUs() const;
    Q_INVOKABLE QVariantMap dragBounds() const;
    QVariantList dragClips() const;
    QVariantMap dragDonorIds() const;
    QString dragReason() const;
    QVariantList dragTracks() const;
    QVariantList dragTransitions() const;
    QVariantMap selectedClipDetails() const;
    QVariantMap selectedClipSummary() const;
    QVariantList overviewSegments() const {
        return overview_segments_;
    }
    QVariantList sourceAudioChoices() const;
    QVariantMap sourceAudioChoice() const;
    Q_INVOKABLE bool beginSourceAudioChoice(qulonglong asset);
    Q_INVOKABLE void setSourceAudioTrack(const QString& track, bool selected);
    Q_INVOKABLE void setSourceAudioOverlapConfirmed(bool confirmed);
    Q_INVOKABLE bool applySourceAudioChoice();
    Q_INVOKABLE void cancelSourceAudioChoice();
    QVariantMap sourceReplacement() const;
    Q_INVOKABLE void beginSourceReplacement(qulonglong asset_id, const QUrl& replacement,
                                            bool adopt_changed_bytes = false);
    Q_INVOKABLE bool applySourceReplacement();
    Q_INVOKABLE void cancelSourceReplacement();
    Q_INVOKABLE void setSourceReplacementTrack(const QString& identity, bool selected);
    Q_INVOKABLE void setSourceReplacementOverlapConfirmed(bool confirmed);
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
    Q_INVOKABLE void selectGapUs(qulonglong track, qint64 at_us);
    Q_INVOKABLE void copyBlankDuration();
    Q_INVOKABLE void pasteSpacer();
    Q_INVOKABLE void previewCloseGap();
    Q_INVOKABLE void setFixedTimelineEnd(bool fixed, qint64 time_ms);
    Q_INVOKABLE void beginClipDrag(qulonglong id, int edge = -1, bool unlink_member = false);
    Q_INVOKABLE void beginJunctionDrag(qulonglong outgoing, int edge);
    Q_INVOKABLE void updateClipDrag(qint64 delta_ms, qulonglong track = 0, int new_track = -1, bool insert = false,
                                    qint64 tolerance_ms = 100);
    Q_INVOKABLE void updateClipDragUs(qint64 delta_us, qulonglong track = 0, int new_track = -1, bool insert = false,
                                      qint64 tolerance_us = 100000);
    Q_INVOKABLE bool commitClipDrag();
    Q_INVOKABLE void cancelClipDrag();
    Q_INVOKABLE void requestFrameStep(int direction);
    Q_INVOKABLE void setSelectionBoundary(bool out);
    Q_INVOKABLE void setJunctionWindow(qint64 start_ms, qint64 end_ms);
    Q_INVOKABLE void setJunctionCurve(int kind, double bias = 0);
    Q_INVOKABLE bool setJunctionCurveFor(qulonglong id, int kind, double bias);
    Q_INVOKABLE void setJunctionAudioLaw(int law);
    Q_INVOKABLE void setJunctionCut(qint64 at_ms);
    Q_INVOKABLE void setClipFade(qulonglong id, bool incoming, qint64 duration_ms);
    Q_INVOKABLE bool setClipFadeCurve(qulonglong id, bool incoming, int kind, double bias);
    Q_INVOKABLE bool inspectClip(qulonglong id);
    Q_INVOKABLE QVariantMap occupiedBounds() const;
    Q_INVOKABLE bool previewRemoveTimeOnAllTracks();
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
    [[nodiscard]] EditTrackLayoutModel* trackLayout() const {
        return track_layout_;
    }

    [[nodiscard]] const edit::Workspace& workspace() const {
        return workspace_;
    }
    // Last command's model transaction and synchronous publication, in nanoseconds.
    [[nodiscard]] QVariantMap commandPerformance() const {
        return {{"modelNs", command_model_ns_}, {"publishNs", command_publish_ns_}};
    }
    void setAssetMonitorSelection(edit::Id asset, engine::MonitorSelectionSnapshot selection);
    // Transient playback workspaces may replace imported stream enablement on every audition.
    void setMonitoringControlsInputMix(bool enabled) {
        monitoring_controls_input_mix_ = enabled;
    }
    [[nodiscard]] QVariantList media() const;
    [[nodiscard]] EditTimelineClipModel* mediaModel() const {
        return media_model_;
    }
    [[nodiscard]] EditFolderModel* folderModel() const {
        return folder_model_;
    }
    [[nodiscard]] std::shared_ptr<const engine::MediaReadCapability> sourceAccess(edit::Id asset) const;
    [[nodiscard]] std::shared_ptr<const engine::VerifiedMediaIdentity> sourceProof(edit::Id asset) const;
    [[nodiscard]] std::shared_ptr<const engine::EditMediaMetadata> sourceMetadata(edit::Id asset) const;
    [[nodiscard]] QString sourcePreviewIdentity(edit::Id asset) const;
    [[nodiscard]] bool sourceVerificationFailed(edit::Id asset) const;
    [[nodiscard]] QString sourceVerificationReason(edit::Id asset) const;
    Q_INVOKABLE void cancelPendingImports();
    Q_INVOKABLE void retrySourceVerification(qulonglong asset);
    void setBackgroundIoAdmission(engine::BackgroundIoAdmission admission) {
        background_io_ = std::move(admission);
    }
    void setBackgroundAnalysisSuspended(bool suspended);
    bool hasPendingImportedMedia() const {
        return !pending_placement_assets_.empty();
    }
    Q_INVOKABLE bool placeImportedMedia(qint64 at_us = -1, bool auto_crossfade = true);
    Q_INVOKABLE void routeMediaDrop(const QList<QUrl>& urls, bool append = true, qint64 at_us = -1);
    Q_INVOKABLE void resolveMediaDrop(qulonglong request_id, bool insert_directories);
    Q_INVOKABLE void cancelMediaDrop(qulonglong request_id);
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
    Q_INVOKABLE QVariantList visibleClipsUs(qint64 from_us, qint64 to_us) const;
    QVariantList visibleClipsOnTracksUs(qint64 from_us, qint64 to_us, const QVariantList& track_ids,
                                        const QVariantList& retained_clip_ids = {}) const;
    QVariantList transitions() const;
    Q_INVOKABLE QVariantList visibleTransitionsUs(qint64 from, qint64 to, const QVariantList& tracks = {},
                                                  bool include_envelope = true,
                                                  const QVariantList& retained_ids = {}) const;
    Q_INVOKABLE QVariantMap junctionEnvelopePoints(qulonglong id, qreal from = 0, qreal to = 1, int count = 64) const;
    qulonglong selectedTransition() const {
        return selected_transition_;
    }
    Q_INVOKABLE void selectTransition(qulonglong outgoing);
    Q_INVOKABLE bool applyCrossfade(qulonglong outgoing = 0, qint64 duration_ms = 500);
    Q_INVOKABLE bool canCrossfade(qulonglong outgoing = 0) const;
    Q_INVOKABLE bool previewCrossfadeAtCut(qulonglong outgoing = 0, qint64 duration_us = 500000);
    Q_INVOKABLE bool previewRemoveOverlap(qulonglong junction_id = 0);
    Q_INVOKABLE bool canCrossfadeAtCut(qulonglong outgoing = 0) const;
    Q_INVOKABLE bool dropCrossfade(qint64 at_ms, qint64 tolerance_ms);
    // Tolerance is limited to 250 ms.
    Q_INVOKABLE bool dropCrossfadeUs(qint64 at_us, qint64 tolerance_us);
    Q_INVOKABLE void removeTransition();
    Q_INVOKABLE void selectClip(qulonglong id);
    Q_INVOKABLE void appendAsset(qulonglong id, qint64 at_ms = -1);
    // Negative placement appends at the current content end.
    Q_INVOKABLE void appendAssetUs(qulonglong id, qint64 at_us = -1);
    Q_INVOKABLE void importMedia(const QUrl& url, bool append = false, qint64 at_ms = -1);
    Q_INVOKABLE void importMediaBatch(const QList<QUrl>& urls, bool append = false, qint64 at_ms = -1);
    // When appending, negative placement resolves against content end after probing.
    Q_INVOKABLE void importMediaBatchUs(const QList<QUrl>& urls, bool append = false, qint64 at_us = -1);
    Q_INVOKABLE void addHistory(const QString& path, qint64 at_ms = -1);
    Q_INVOKABLE void addHistoryUs(const QString& path, qint64 at_us = -1);
    Q_INVOKABLE void moveClip(qulonglong id, qint64 at_ms, bool snap = true);
    Q_INVOKABLE void moveClipUs(qulonglong id, qint64 at_us, bool snap = true);
    Q_INVOKABLE void trimClip(qulonglong id, qint64 in_ms, qint64 out_ms);
    Q_INVOKABLE void trimClipUs(qulonglong id, qint64 in_us, qint64 out_us);
    Q_INVOKABLE void splitSelected();
    Q_INVOKABLE void deleteSelected(bool ripple_delete = false);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void selectAdjacentClip(int direction);
    Q_INVOKABLE void nudgeSelected(qint64 delta_ms, int edge = 0);
    Q_INVOKABLE void nudgeSelectedUs(qint64 delta_us, int edge = 0);

    // Adds recording segments to the resident workspace and refreshes the source context.
    // Negative placement resolves against content end after probing.
    void setEditContext(const EditContext& context, qint64 timeline_start_ms = -1);
    // Initial position is relative to the added recording and survives asynchronous probing.
    void setEditContextUs(const EditContext& context, qint64 timeline_start_us = -1, qint64 initial_position_us = 0);
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
    [[nodiscard]] qint64 durationUs() const noexcept {
        return workspace_.duration();
    }

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
    Q_INVOKABLE void finishPointerSeek();
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
    void selectionChanged();
    void editRangeCommitted(qint64 start_us, qint64 end_us);
    void editRangeCleared();
    void auditionChanged();
    void clipboardChanged();
    void rangeGestureChanged();
    void interactionSettingsChanged();
    void dragChanged();
    void timeSelectionChanged();
    void workspaceChanged();
    void mediaChanged();
    void overviewChanged();
    void sourceAudioChoiceChanged();
    void sourceReplacementChanged();
    void selectedClipSummaryChanged();
    void sourceVerificationChanged(qulonglong asset);
    void importPendingChanged();
    void pendingPlacementChanged();
    void directoryDropRequested(qulonglong request_id, const QVariantList& roots, const QVariantList& candidates,
                                qint64 at_us, const QString& reason);
    void historyRequestedUs(const QString& path, qint64 at_us);
    void editPageRequested();
    void openChanged();
    void clipChanged();
    void markersChanged();
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
    friend class EditSessionAdapterTestPeer;
    void scheduleSourceVerification(std::function<void()> task);
    QVariantMap projectClipRow(const edit::Clip& clip) const;
    void syncOverview();
    edit::Revision overview_revision_ = (std::numeric_limits<edit::Revision>::max)();
    using OverviewSegment = std::tuple<qint64, qint64, int, bool, bool>;
    std::vector<OverviewSegment> overview_state_;
    QVariantList overview_segments_;
    edit::Id source_audio_asset_ = 0;
    std::vector<engine::AudioTrackId> source_audio_selected_;
    bool source_audio_overlap_confirmed_ = false;
    bool source_audio_busy_ = false;
    QString source_audio_reason_;
    std::uint64_t source_audio_generation_ = 0;
    std::shared_ptr<std::atomic<bool>> source_audio_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    engine::BackgroundIoAdmission optionalIoAdmission() const;
    QPointer<QObject> pending_draft_owner_;
    edit::Id pending_draft_target_ = 0;
    bool committing_pending_draft_ = false;
    void cancelPendingDraft(const QString& reason);
    edit::EditSelection selection_;
    std::set<edit::Id> solo_tracks_;
    std::shared_ptr<const edit::ClipClipboard> clip_clipboard_;
    std::shared_ptr<const edit::EditSnapshot> range_gesture_snapshot_;
    std::optional<edit::EditRange> range_gesture_candidate_;
    std::optional<edit::EditRangePreview> range_gesture_preview_;
    bool range_scope_review_ = false;
    bool range_scope_action_ = false;
    void setRangeGestureCandidate(edit::Time start, edit::Time end, const std::vector<edit::Id>& tracks,
                                  edit::Time anchor);
    std::vector<edit::Id> range_gesture_tracks_;
    edit::Time range_gesture_anchor_ = 0;
    edit::Id range_gesture_first_track_ = 0;
    std::optional<edit::EditRange> range_gesture_base_;
    bool range_gesture_extend_scope_ = false;
    bool range_gesture_scope_only_ = false;
    int range_band_target_ = 0;
    edit::Time range_band_press_ = 0;
    int last_range_edge_ = 0;
    void publishSelection();
    void syncSelection();
    qint64 command_model_ns_ = 0;
    qint64 command_publish_ns_ = 0;
    struct GestureSelection {
        edit::EditSelection selection;
        edit::Id transition = 0;
        edit::Id spacer = 0;
        edit::Id gap_track = 0;
        edit::Span gap;
    };
    std::optional<GestureSelection> drag_selection_;
    std::optional<GestureSelection> range_selection_;
    struct PointerSelection {
        GestureSelection original;
        edit::Revision revision = 0;
        edit::Id deferred_toggle = 0;
        bool linked = true;
        bool snapping = true;
        bool ripple = false;
        bool auto_crossfade = true;
        std::optional<edit::Span> loop_bounds;
        std::optional<edit::Span> range_bounds;
        edit::Time playhead = 0;
    };
    std::optional<PointerSelection> pointer_selection_;
    std::optional<edit::Span> loop_snap_bounds_;
    std::optional<edit::Span> drag_loop_snap_bounds_;
    std::optional<edit::Span> drag_range_snap_bounds_;
    std::optional<edit::Time> drag_snap_marker_;
    bool drag_audio_grid_ = false;
    edit::Time drag_domain_end_ = 0;
    void restoreGestureSelection(GestureSelection original);
    bool drag_gain_ = false;
    bool drag_gain_fine_ = false;
    std::vector<edit::Id> drag_targets_;
    std::vector<edit::Id> drag_operation_targets_;
    bool drag_expand_linked_ = true;
    bool drag_snapping_ = true;
    bool drag_ripple_ = false;
    bool drag_auto_crossfade_ = true;
    std::shared_ptr<const edit::EditSnapshot> drag_snapshot_;
    std::optional<edit::EditCommand> drag_command_;
    QString dragSourceFailure() const;
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
    std::vector<edit::Id> pending_placement_assets_;
    EditFolderModel* folder_model_ = nullptr;
    struct PendingMediaDrop {
        qulonglong request_id = 0;
        std::uint64_t session = 0;
        bool append = true;
        qint64 at_us = -1;
        QList<QUrl> roots;
        QList<QUrl> direct_files;
        QList<QUrl> all_files;
    };
    PendingMediaDrop pending_media_drop_;
    qulonglong drop_sequence_ = 0;
    void importMediaWithRevision(const QList<QUrl>& urls, bool append, qint64 at_us, edit::Revision revision);
    struct SourcePublication {
        std::shared_ptr<const engine::MediaReadCapability> access;
        std::shared_ptr<const engine::VerifiedMediaIdentity> proof;
        std::shared_ptr<const engine::EditMediaMetadata> metadata;
        QString reason;
        bool verifying = false;
        bool failed = false;
        uint64_t version = 0;
    };
    std::map<edit::Id, SourcePublication> source_publications_;
    std::map<std::pair<edit::Id, uint64_t>, SourcePublication> source_versions_;
    void syncSourceVersions();
    struct SourceReplacementDraft {
        edit::Id asset_id = 0;
        uint64_t source_version = 0;
        std::shared_ptr<const edit::EditSnapshot> snapshot;
        QString path;
        bool adopt = false;
        bool busy = false;
        QString stage;
        QString reason;
        QVariantList affected_clips;
        qint64 required_source_end_us = 0;
        std::optional<edit::Asset> candidate;
        SourcePublication publication;
        std::vector<engine::AudioTrackId> selected;
        bool overlap_confirmed = false;
    };
    std::optional<SourceReplacementDraft> source_replacement_;
    uint64_t source_replacement_generation_ = 0;
    std::shared_ptr<std::atomic<bool>> source_replacement_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    std::optional<edit::ReplaceAssetSource> sourceReplacementCommand() const;
    bool commitSourceReplacement(uint64_t generation, std::optional<engine::MonitorSelectionSnapshot> selection = {});
    engine::BackgroundIoAdmission background_io_;
    std::shared_ptr<BackgroundStorageBudget> optional_io_budget_;
    std::shared_ptr<std::atomic<bool>> keyframe_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> import_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> source_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> source_verification_cancelled_ = std::make_shared<std::atomic<bool>>(false);
    std::uint64_t import_session_ = 0;
    edit::Revision media_revision_ = (std::numeric_limits<edit::Revision>::max)();
    QVariantList media_rows_;
    EditTimelineClipModel* media_model_ = nullptr;
    void syncMedia(bool force = false);
    void revokePendingImports();
    quint64 edit_outcome_sequence_ = 0;
    bool last_edit_accepted_ = true;
    static bool FitsMilliseconds(qint64 value);
    void beginImport();
    void endImport();
    // Every refusal that does not go through publishWorkspace(false).
    void refuseEdit(const QString& message);
    void setInteractionSetting(bool& setting, bool value);
    bool applyCommand(const edit::EditCommand& command, std::function<void()> after_apply = {});
    void applyEditContextUs(const EditContext& context, qint64 timeline_start_us, qint64 initial_position_us,
                            std::vector<edit::SourceImport> imports, std::vector<SourcePublication> publications = {});
    edit::Id selected_transition_ = 0;
    void publishWorkspace(bool changed, const QString& error = {});
    void applyReport(const EditContext& context);
    void rebuildFacts();
    void loadMarkers();
    void startKeyframeScan(std::shared_ptr<const engine::MediaReadCapability> source = {});
    void setTrimUs(int64_t start_us, int64_t end_us);

    EditContext context_;
    edit::Workspace workspace_;
    EditTrackLayoutModel* track_layout_ = nullptr;
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
    bool monitoring_controls_input_mix_ = false;
    bool trim_snap_ready_ = false;
    bool export_running_ = false;

    // Worker pools are destroyed before the state retained by their callbacks.
    QThreadPool keyframe_pool_;
    QThreadPool metadata_pool_;
    QThreadPool verification_pool_;
};

} // namespace exosnap::quick

#pragma once

#include <QHash>
#include <QObject>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQmlIntegration/qqmlintegration.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace exosnap {

class RecordViewModel;

namespace engine {
struct CaptureTarget;
}

namespace quick {

// Narrow QObject boundary over the shared, pure-C++ RecordViewModel. It maps
// presentation-ready state and emits typed command requests; orchestration and
// service ownership remain in QuickApplication.
class RecordViewModelAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RecordViewModelAdapter is provided by the application")

    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged FINAL)
    Q_PROPERTY(int state READ state NOTIFY changed FINAL)
    Q_PROPERTY(QString stateText READ stateText NOTIFY stateTextChanged FINAL)
    Q_PROPERTY(QString stateTone READ stateTone NOTIFY changed FINAL)
    Q_PROPERTY(QString capabilityText READ capabilityText NOTIFY changed FINAL)
    Q_PROPERTY(QString elapsedText READ elapsedText NOTIFY elapsedTextChanged FINAL)
    Q_PROPERTY(QString outputSizeText READ outputSizeText NOTIFY outputSizeTextChanged FINAL)
    Q_PROPERTY(QString bitrateText READ bitrateText NOTIFY changed FINAL)
    // Measured capture rate, from SessionStats::video_frames_captured over the
    // elapsed clock. Sampled as a delta between stats callbacks rather than as a
    // session average, so a dip is visible instead of being smoothed away.
    Q_PROPERTY(QString capturedFpsText READ capturedFpsText NOTIFY changed FINAL)
    Q_PROPERTY(QString droppedFramesText READ droppedFramesText NOTIFY changed FINAL)
    Q_PROPERTY(QString driftText READ driftText NOTIFY changed FINAL)
    Q_PROPERTY(bool liveStatsAvailable READ liveStatsAvailable NOTIFY liveStatsAvailableChanged FINAL)

    Q_PROPERTY(bool canStart READ canStart NOTIFY changed FINAL)
    Q_PROPERTY(bool canStop READ canStop NOTIFY changed FINAL)
    Q_PROPERTY(bool canPause READ canPause NOTIFY changed FINAL)
    Q_PROPERTY(bool canResume READ canResume NOTIFY changed FINAL)
    Q_PROPERTY(bool canSelectSource READ canSelectSource NOTIFY changed FINAL)
    Q_PROPERTY(bool recording READ recording NOTIFY changed FINAL)
    Q_PROPERTY(bool paused READ paused NOTIFY changed FINAL)
    Q_PROPERTY(bool countdownActive READ countdownActive NOTIFY changed FINAL)
    Q_PROPERTY(bool preparing READ preparing NOTIFY changed FINAL)
    Q_PROPERTY(bool finalizing READ finalizing NOTIFY changed FINAL)
    // How far the post-recording remux has got, 0..1, or -1 while no fraction is
    // known -- which is every moment of Stopping, and the first instant of
    // Saving before the remuxer has reported. Deliberately NOT folded into
    // `finalizing`: a bar that starts at 0 % because nothing has been measured
    // yet claims progress it does not have.
    Q_PROPERTY(qreal savingProgress READ savingProgress NOTIFY savingProgressChanged FINAL)
    Q_PROPERTY(bool blocked READ blocked NOTIFY changed FINAL)
    Q_PROPERTY(bool failed READ failed NOTIFY changed FINAL)
    // A run that is over, one way or the other. The transport swaps its whole
    // right-hand cluster on this: the way OUT of a finished run is a separate
    // action from starting the next one.
    Q_PROPERTY(bool resultPending READ resultPending NOTIFY changed FINAL)

    Q_PROPERTY(QVariantList targetOptions READ targetOptions NOTIFY targetOptionsChanged FINAL)
    Q_PROPERTY(QVariantList displayTargetOptions READ displayTargetOptions NOTIFY targetOptionsChanged FINAL)
    Q_PROPERTY(QVariantList windowTargetOptions READ windowTargetOptions NOTIFY targetOptionsChanged FINAL)
    Q_PROPERTY(int targetCount READ targetCount NOTIFY targetOptionsChanged FINAL)
    Q_PROPERTY(QString selectedTargetIdentity READ selectedTargetIdentity NOTIFY targetOptionsChanged FINAL)
    Q_PROPERTY(bool selectedTargetAvailable READ selectedTargetAvailable NOTIFY targetOptionsChanged FINAL)
    Q_PROPERTY(int selectedTargetIndex READ selectedTargetIndex NOTIFY changed FINAL)
    Q_PROPERTY(int captureMode READ captureMode NOTIFY changed FINAL)
    Q_PROPERTY(QString sourceName READ sourceName NOTIFY changed FINAL)
    Q_PROPERTY(QString sourceKindText READ sourceKindText NOTIFY changed FINAL)
    Q_PROPERTY(QString sourceDetailText READ sourceDetailText NOTIFY changed FINAL)
    Q_PROPERTY(QString formatText READ formatText NOTIFY changed FINAL)
    Q_PROPERTY(QRectF normalizedSourceRect READ normalizedSourceRect NOTIFY changed FINAL)
    Q_PROPERTY(bool regionSelectionNeeded READ regionSelectionNeeded NOTIFY changed FINAL)
    // Region-tab preset cards, in the order the Region tab shows them: Draw
    // custom first and strongest, then the four aspect presets. Product policy,
    // so it lives here rather than in the QML document.
    Q_PROPERTY(QVariantList regionPresetOptions READ regionPresetOptions NOTIFY changed FINAL)
    // False while the user is still composing the rectangle (the overlay's
    // handles, dimension label and confirm stay live); true from the countdown
    // onward, when the capture owns the region and editing must hide and lock.
    Q_PROPERTY(bool regionEditingLocked READ regionEditingLocked NOTIFY changed FINAL)

    Q_PROPERTY(bool systemAudioEnabled READ systemAudioEnabled NOTIFY changed FINAL)
    Q_PROPERTY(bool appAudioEnabled READ appAudioEnabled NOTIFY changed FINAL)
    Q_PROPERTY(bool microphoneEnabled READ microphoneEnabled NOTIFY changed FINAL)
    Q_PROPERTY(bool webcamEnabled READ webcamEnabled NOTIFY changed FINAL)
    Q_PROPERTY(bool appAudioVisible READ appAudioVisible NOTIFY changed FINAL)
    // The source keys ("system", "app", "microphone") whose toggle still acts
    // while a recording runs. A source that was off when the session started has
    // no track to put silence into, so it is absent here and stays locked for
    // the run.
    Q_PROPERTY(QStringList liveToggleableSources READ liveToggleableSources NOTIFY changed FINAL)
    Q_PROPERTY(bool microphoneAvailable READ microphoneAvailable NOTIFY changed FINAL)
    Q_PROPERTY(bool webcamAvailable READ webcamAvailable NOTIFY changed FINAL)
    Q_PROPERTY(bool webcamError READ webcamError NOTIFY changed FINAL)
    Q_PROPERTY(QString webcamErrorText READ webcamErrorText NOTIFY changed FINAL)
    Q_PROPERTY(QString webcamFrameSource READ webcamFrameSource NOTIFY webcamFrameChanged FINAL)
    Q_PROPERTY(QRectF webcamOverlayRect READ webcamOverlayRect NOTIFY changed FINAL)
    Q_PROPERTY(bool webcamOverlayEditable READ webcamOverlayEditable NOTIFY changed FINAL)
    Q_PROPERTY(bool webcamMirror READ webcamMirror NOTIFY changed FINAL)
    Q_PROPERTY(double webcamOpacity READ webcamOpacity NOTIFY changed FINAL)
    Q_PROPERTY(double systemMeter READ systemMeter NOTIFY metersChanged FINAL)
    Q_PROPERTY(double appMeter READ appMeter NOTIFY metersChanged FINAL)
    Q_PROPERTY(double microphoneMeter READ microphoneMeter NOTIFY metersChanged FINAL)

    Q_PROPERTY(int countdownSeconds READ countdownSeconds NOTIFY changed FINAL)
    Q_PROPERTY(int countdownRemaining READ countdownRemaining NOTIFY changed FINAL)
    // Fraction of the countdown still to run, 1.0 down to 0.0, at the resolution
    // the coordinator actually counts in. `countdownRemaining` is the same value
    // rounded to whole seconds for the digit; a ring driven from THAT jumps
    // three times and is right only at the instant it moves. Carried separately
    // rather than derived in QML, because the millisecond clock exists in C++
    // and rounding it away first cannot be undone by any amount of animation.
    Q_PROPERTY(double countdownProgress READ countdownProgress NOTIFY changed FINAL)
    Q_PROPERTY(bool captureFrameEnabled READ captureFrameEnabled NOTIFY changed FINAL)
    Q_PROPERTY(bool splitEnabled READ splitEnabled NOTIFY changed FINAL)
    Q_PROPERTY(QString resultText READ resultText NOTIFY changed FINAL)
    // Whether the finished recording can be opened in the Edit surface. False
    // for a split recording (no single edit master), a missing file, a failed
    // run, and while a capture still owns the Record surface. The authoritative
    // gate is QuickApplication::canOpenEditorForCurrentRecording(); this mirrors
    // the part of it that depends only on the view model, so the affordance can
    // be a binding rather than a button that does nothing when pressed.
    Q_PROPERTY(bool canOpenEditor READ canOpenEditor NOTIFY changed FINAL)
    // The Record context strip's Recent menu: the finished recordings this
    // session knows about, newest first, each carrying the path the menu acts
    // on and the label the shared resolver produced for the run's target. Rows
    // whose file no longer exists are published as unavailable rather than
    // dropped, so a menu entry never silently changes meaning between the frame
    // the user read it in and the frame they pressed it.
    Q_PROPERTY(QVariantList recentRecordingOptions READ recentRecordingOptions NOTIFY recentRecordingsChanged FINAL)

  public:
    explicit RecordViewModelAdapter(const RecordViewModel* source = nullptr, QObject* parent = nullptr);

    [[nodiscard]] bool active() const noexcept;
    void setActive(bool active);
    [[nodiscard]] int state() const noexcept;
    [[nodiscard]] const QString& stateText() const noexcept;
    [[nodiscard]] QString stateTone() const;
    [[nodiscard]] QString capabilityText() const;
    [[nodiscard]] QString elapsedText() const;
    [[nodiscard]] const QString& outputSizeText() const noexcept;
    [[nodiscard]] QString bitrateText() const;
    [[nodiscard]] const QString& capturedFpsText() const noexcept;
    [[nodiscard]] QString droppedFramesText() const;
    [[nodiscard]] QString driftText() const;
    [[nodiscard]] bool liveStatsAvailable() const noexcept;
    [[nodiscard]] bool canStart() const noexcept;
    [[nodiscard]] bool canStop() const noexcept;
    [[nodiscard]] bool canPause() const noexcept;
    [[nodiscard]] bool canResume() const noexcept;
    [[nodiscard]] bool canSelectSource() const noexcept;
    [[nodiscard]] bool recording() const noexcept;
    [[nodiscard]] bool paused() const noexcept;
    [[nodiscard]] bool countdownActive() const noexcept;
    [[nodiscard]] bool preparing() const noexcept;
    [[nodiscard]] bool finalizing() const noexcept;
    [[nodiscard]] qreal savingProgress() const noexcept;
    // Fed by RecordingCoordinator's remux progress. Negative clears it, which is
    // what leaving the Saving state does.
    void setSavingProgress(float fraction);
    [[nodiscard]] bool blocked() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] bool resultPending() const noexcept;
    [[nodiscard]] const QVariantList& targetOptions() const noexcept;
    [[nodiscard]] const QVariantList& displayTargetOptions() const noexcept;
    [[nodiscard]] const QVariantList& windowTargetOptions() const noexcept;
    [[nodiscard]] int targetCount() const noexcept;
    [[nodiscard]] const QString& selectedTargetIdentity() const noexcept;
    [[nodiscard]] bool selectedTargetAvailable() const noexcept;
    [[nodiscard]] Q_INVOKABLE QVariantList filteredTargetOptions(const QString& kind, const QString& query) const;
    [[nodiscard]] int selectedTargetIndex() const noexcept;
    [[nodiscard]] int captureMode() const noexcept;
    [[nodiscard]] const QString& sourceName() const noexcept;
    [[nodiscard]] const QString& sourceKindText() const noexcept;
    [[nodiscard]] const QString& sourceDetailText() const noexcept;
    [[nodiscard]] const QString& formatText() const noexcept;
    [[nodiscard]] QRectF normalizedSourceRect() const noexcept;
    [[nodiscard]] bool regionSelectionNeeded() const noexcept;
    [[nodiscard]] const QVariantList& regionPresetOptions() const noexcept;
    [[nodiscard]] const QVariantList& recentRecordingOptions() const noexcept;
    [[nodiscard]] bool regionEditingLocked() const noexcept;
    [[nodiscard]] bool systemAudioEnabled() const noexcept;
    [[nodiscard]] bool appAudioEnabled() const noexcept;
    [[nodiscard]] bool microphoneEnabled() const noexcept;
    [[nodiscard]] bool webcamEnabled() const noexcept;
    [[nodiscard]] bool appAudioVisible() const noexcept;
    [[nodiscard]] const QStringList& liveToggleableSources() const noexcept;
    void setLiveToggleableSources(QStringList keys);
    [[nodiscard]] bool microphoneAvailable() const noexcept;
    [[nodiscard]] bool webcamAvailable() const noexcept;
    [[nodiscard]] bool webcamError() const noexcept;
    [[nodiscard]] const QString& webcamErrorText() const noexcept;
    [[nodiscard]] const QString& webcamFrameSource() const noexcept;
    [[nodiscard]] QRectF webcamOverlayRect() const noexcept;
    [[nodiscard]] bool webcamOverlayEditable() const noexcept;
    [[nodiscard]] bool webcamMirror() const noexcept;
    [[nodiscard]] double webcamOpacity() const noexcept;
    [[nodiscard]] double systemMeter() const noexcept;
    [[nodiscard]] double appMeter() const noexcept;
    [[nodiscard]] double microphoneMeter() const noexcept;
    [[nodiscard]] int countdownSeconds() const noexcept;
    [[nodiscard]] int countdownRemaining() const noexcept;
    [[nodiscard]] double countdownProgress() const noexcept;
    [[nodiscard]] bool captureFrameEnabled() const noexcept;
    [[nodiscard]] bool splitEnabled() const noexcept;
    [[nodiscard]] QString resultText() const;
    [[nodiscard]] bool canOpenEditor() const noexcept;

    void setSource(const RecordViewModel* source);
    void setFormatText(QString text);
    void setDeviceState(bool microphone_available, bool webcam_available, bool webcam_enabled,
                        QString webcam_error = {});
    void setWebcamPresentation(QRectF overlay_rect, bool mirror, double opacity);
    void setWebcamFrameSource(QString source);
    void setCountdownState(int configured_seconds, int remaining_seconds, double progress);
    void setRegionState(QRectF normalized_rect, bool selection_needed);
    void setPreviewFrameReady(bool ready);
    void setSplitEnabled(bool enabled);
    void setMeters(double system, double app, double microphone);
    // `tone` defaults to "warning" so an existing caller that never named one
    // keeps the banner it already had; the callers that state a tone are the
    // ones whose message is not a warning.
    void setNoticeText(QString text, QString tone = QStringLiteral("warning"));
    // The identity the picker rows carry. Public because the still service is
    // told which identities are visible and has to resolve them back to the
    // capture targets they name.
    [[nodiscard]] static QString TargetIdentity(const exosnap::engine::CaptureTarget& target);
    void setTargetStill(QString identity, QString source);
    // A target whose grab failed twice in a row. Its last still is kept and the
    // card marks it stale; a target that never had one stays a placeholder.
    void setTargetStillUnavailable(const QString& identity);
    void synchronize();

    Q_INVOKABLE void requestStart();
    Q_INVOKABLE void requestStop();
    Q_INVOKABLE void requestPause();
    Q_INVOKABLE void requestResume();
    Q_INVOKABLE void requestCaptureFrame();
    Q_INVOKABLE void requestAddMarker();
    Q_INVOKABLE void requestSplit();
    Q_INVOKABLE void requestSelectTarget(int target_index, int capture_mode);
    Q_INVOKABLE void requestSelectRegion(QRectF normalized_rect);
    // The Region tab's preset choice. "custom" means draw-from-scratch; the
    // aspect keys carry an editable starting rectangle. Routing through the
    // adapter (rather than direct QML-to-QML wiring) keeps the picker and the
    // overlay decoupled surfaces that only share the C++ boundary.
    Q_INVOKABLE void requestRegionPreset(const QString& key);
    Q_INVOKABLE void requestToggleSource(const QString& key);
    Q_INVOKABLE void requestWebcamOverlayRect(QRectF normalized_rect);
    Q_INVOKABLE void requestCountdownSeconds(int seconds);
    Q_INVOKABLE void requestOpenEditor();
    Q_INVOKABLE void requestDismissResult();
    Q_INVOKABLE void requestRevealRecording();
    Q_INVOKABLE void requestOpenRecent(const QString& file_path);
    Q_INVOKABLE void requestRevealRecent(const QString& file_path);
    // The identities the picker currently has on screen, in layout order. The
    // still service walks exactly this set, so scrolling past a card is what
    // stops paying for it.
    Q_INVOKABLE void setVisibleTargetIdentities(const QStringList& identities);

  signals:
    void savingProgressChanged();
    void activeChanged();
    void stateTextChanged();
    void elapsedTextChanged();
    void outputSizeTextChanged();
    void liveStatsAvailableChanged();
    void webcamFrameChanged();
    void targetOptionsChanged();
    void recentRecordingsChanged();
    void visibleTargetIdentitiesChanged(QStringList identities);
    void metersChanged();
    void changed();

    void startRequested();
    void stopRequested();
    void pauseRequested();
    void resumeRequested();
    void captureFrameRequested();
    void addMarkerRequested();
    void splitRequested();
    void selectTargetRequested(int target_index, int capture_mode);
    void selectRegionRequested(QRectF normalized_rect);
    void regionPresetRequested(QString key);
    void toggleSourceRequested(QString key);
    void webcamOverlayRectRequested(QRectF normalized_rect);
    void countdownSecondsRequested(int seconds);
    void openEditorRequested();
    void dismissResultRequested();
    void revealRecordingRequested();
    void openRecentRequested(QString file_path);
    void revealRecentRequested(QString file_path);

  private:
    // -1 means "not known", which is the resting value; see the property.
    qreal saving_progress_ = -1.0;

    // Reads every property whose NOTIFY is `changed()`, in declaration order.
    [[nodiscard]] QVariantList changedPropertySnapshot() const;
    // Emits `changed()` only when that snapshot actually moved. The single funnel
    // for the broad signal out of synchronize().
    void publishChanged();
    void rebuildPresentation();
    void rebuildRecentRecordings();
    // Advances the fps delta window. Called from synchronize(), i.e. on the
    // engine's stats cadence, so the window is measured against the same clock
    // the frame counter is.
    void updateCapturedFps();

    const RecordViewModel* source_ = nullptr;
    // Property indices carrying NOTIFY changed, resolved once from the static
    // meta-object, plus the last values published under that signal.
    std::vector<int> changed_property_indices_;
    QVariantList changed_property_values_;
    // The RecordViewModel::targets_revision the three option lists were built
    // from. Unset means "not built for this source yet".
    std::optional<std::uint64_t> target_options_revision_;
    std::optional<int> target_options_selected_index_;
    bool active_ = false;
    QString state_text_;
    QString elapsed_text_;
    QString output_size_text_;
    bool live_stats_available_ = false;
    // Delta window for capturedFpsText. `sample_seconds_ < 0` means "no sample
    // taken yet in this session", which is what makes the first reading an em
    // dash rather than a figure derived from a single point.
    QString captured_fps_text_;
    double fps_sample_seconds_ = -1.0;
    quint64 fps_sample_frames_ = 0;
    QVariantList target_options_;
    QVariantList display_target_options_;
    QVariantList window_target_options_;
    QHash<QString, QString> target_stills_;
    QSet<QString> stale_target_stills_;
    QStringList visible_target_identities_;
    QString selected_target_identity_;
    bool selected_target_available_ = false;
    QString source_name_;
    QString source_kind_text_;
    QString source_detail_text_;
    QString format_text_;
    QRectF normalized_source_rect_{0.0, 0.0, 1.0, 1.0};
    bool region_selection_needed_ = false;
    QVariantList region_preset_options_;
    QVariantList recent_recording_options_;
    QStringList live_toggleable_sources_;
    bool microphone_available_ = true;
    bool webcam_available_ = true;
    bool webcam_enabled_ = false;
    QString webcam_error_text_;
    QString webcam_frame_source_;
    QRectF webcam_overlay_rect_{0.0, 0.0, 0.25, 0.25};
    bool webcam_mirror_ = false;
    double webcam_opacity_ = 1.0;
    double system_meter_ = 0.0;
    double app_meter_ = 0.0;
    double microphone_meter_ = 0.0;
    int countdown_seconds_ = 0;
    int countdown_remaining_ = 0;
    double countdown_progress_ = 1.0;
    bool preview_frame_ready_ = false;
    bool split_enabled_ = false;
};

} // namespace quick
} // namespace exosnap

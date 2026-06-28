#pragma once
#include <QElapsedTimer>
#include <QWidget>
#include <filesystem>
#include <memory>
#include <optional>
#include <recorder_core/audio_input_device.h>
#include <vector>

#include "../diagnostics/DiagnosticResult.h"

#include "../models/OutputSettingsModel.h"
#include "../models/RecordingPreset.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"
#include "../services/AudioDeviceNotifier.h"
#include "../services/DisplayDeviceNotifier.h"
#include "../services/PreviewHelpers.h"
#include "../services/PreviewService.h"
#include "../services/RecordingCoordinator.h"
#include "../services/RecordingCountdownController.h"
#include "../services/WebcamDeviceNotifier.h"
#include "../settings/RecordingHistoryStore.h"
#include "../settings/RecoveryManifestStore.h"
#include "../ui/widgets/RegionSelectionOverlay.h"
#include "../viewmodels/RecordViewModel.h"

#include <capability/capability_set.h>
#include <capability/config_types.h>

// Full include required: SourcePickerDialog::SelectionResult is used in private slot signature.
#include "../ui/dialogs/SourcePickerDialog.h"

// Full include required: RecordingErrorModel is passed by value in the recordingFailed signal.
#include "../ui/dialogs/RecordingErrorPanel.h"

class QAbstractButton;
class QComboBox;
class QBoxLayout;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QResizeEvent;
class QSlider;
class QSpinBox;
class QTimer;
class QVBoxLayout;

namespace exosnap {

enum class InteractionMode {
    None,
    Countdown,
    RegionSelecting,
    RegionMoving,
    RegionResizing,
};

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
namespace visual {
struct VisualScenario;
}
#endif

namespace ui::widgets {
class AudioSourceRow;
class CaptureTargetCard;
class ExoCheckBox;
class PreviewSurface;
class SectionRuleHeader;
class TransportDock;
class VUMeterWidget;
} // namespace ui::widgets

namespace ui::dialogs {
class SourcePickerOverlay;
}

class RecordPage : public QWidget {
    Q_OBJECT
  public:
    explicit RecordPage(QWidget* parent = nullptr);
    ~RecordPage() override;
    void setOutputSettings(const OutputSettingsModel& settings);
    void setVideoSettings(const VideoSettingsModel& settings);
    void setWebcamSettings(const WebcamSettings& settings);
    void setActiveProfileName(const std::string& profile_name);
    void applyPersistedAudioSettings(const capability::AudioUiState& state);
    void setRuntimeCapabilities(const capability::CapabilitySet& caps);
    // Called on the UI thread when the async capability probe FAILED (threw). Drives
    // the coordinator into its capability-failure state so init never hangs armed.
    void setRuntimeCapabilitiesFailed(const QString& reason);
    void rebroadcastChromeState();
    void restoreRecordingHistory();

    // ---- Preset capture/countdown API (Stage 1) ----

    // Build a PresetCaptureTarget from current view_model_ state.
    // Never stores raw platform handles; keys are description-based.
    [[nodiscard]] PresetCaptureTarget currentCapturePolicy() const;

    // Apply a saved capture policy:
    //   - Empty key     → no stored preference: auto-pick primary/first target (OK).
    //   - Non-empty key, match found  → select it.
    //   - Non-empty key, no match     → leave selection cleared / unresolved;
    //                                   do NOT auto-pick a different target.
    // Clears stale Region crop when switching away from Region mode.
    // Always restarts the preview via startPreviewIfIdle() when appropriate.
    void applyCapturePolicy(const PresetCaptureTarget& cap);

    // Returns selected_countdown_seconds_.
    [[nodiscard]] int countdownSeconds() const;

    // Snaps to {0,3,5,10}; pushes the value to the dock's split button + stored field.
    // Does NOT emit recordingConfigChanged (programmatic change).
    void setCountdownSeconds(int seconds);

    // True iff the recording state allows a preset switch.
    [[nodiscard]] bool canApplyPresetNow() const;

    // ADR-0014: Cancel an in-progress MP4 remux job.  Safe to call when not
    // remuxing (no-op).  Called by MainWindow::closeEvent when the user chooses
    // "Cancel save and close".
    void cancelRemux();

    // Inject the recovery manifest store. Must be called before initCoordinator
    // is triggered (i.e. before first show). Pointer is stored; the store must
    // outlive this page.
    void setRecoveryManifestStore(RecoveryManifestStore* store);
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    void applyVisualScenario(const visual::VisualScenario& scenario);
#endif

  signals:
    // Emitted as the LAST statement of initCoordinator(), once all init work
    // (targets enumerated, coordinator ready) is complete.  MainWindow connects
    // to this to re-apply the selected preset after deferred init clobbers it.
    void coordinatorInitialized();

    // Live recording-pipeline diagnostics, forwarded from the coordinator on the UI
    // thread. MainWindow routes these to the Diagnostics page.
    void diagnosticsUpdated(const recorder_core::RecordingDiagnosticsSnapshot& snapshot);

    void chromeStateChanged(bool recording, const QString& status_label, const QString& context_text);
    void chromeRuntimeMetricsChanged(const QString& elapsed_text, const QString& bitrate_text, const QString& drop_text,
                                     const QString& size_text, double av_drift_ms);
    void navigateToOutputPage();
    void navigateToDiagnosticsPage();
    void audioSettingsChanged(const capability::AudioUiState& state);
    // Emitted when the webcam PiP placement is confirmed in the Record preview
    // (drag/resize release). Carries the full settings so MainWindow can persist
    // and propagate to the Settings/Webcam surfaces.
    void webcamSettingsChanged(const WebcamSettings& settings);
    // Emitted at ~30 Hz during recording (via recording-meter callback) and at ~preflight cadence
    // during Ready/Idle (via individual source meter callbacks). All three sources are included in
    // every emission so the Settings Audio card can update all rows atomically.
    // sys01/app01/mic01: pre-computed 0..1 dock-level values (0 = inactive or silence).
    // sys/app/mic_active: true when the meter service is running for that source.
    void audioMeterLevelsUpdated(float sys01, float app01, float mic01, bool sys_active, bool app_active,
                                 bool mic_active);
    // Emitted when the USER changes target, region, or countdown (not when
    // applyCapturePolicy / setCountdownSeconds drives them programmatically).
    void recordingConfigChanged();

    // RECORDING-OVERLAY-R1: emitted alongside chromeStateChanged to tell
    // MainWindow which monitor geometry to use for overlay positioning.
    // rect is the virtual-screen geometry of the captured monitor (Monitor/Region
    // targets), or a null QRect for Window targets (caller uses primary screen).
    void recordingMonitorGeometryChanged(const QRect& monitor_rect);

    // NOTIFY-TOASTS-R1: emitted when a recording result becomes available so
    // MainWindow can enqueue the appropriate notification toast.
    // output_path: final output file path (non-empty on success).
    // error_phase: engine error phase string (non-empty on failure; "DiskSpace" for
    //   disk-monitor auto-stop, other values for engine failures).
    void recordingResultReady(bool succeeded, const QString& output_path, const QString& error_phase);

    // RECORDING-ERROR-MODAL-R1: emitted on a recording FAILURE that is not the
    // disk-space auto-stop (which has its own actionable "Storage running low"
    // notification). MainWindow shows a modal RecordingErrorOverlay. Carries the
    // full failure detail so the dialog can render phase/code/detail and offer an
    // opt-in Sentry report. Independent of the show_notifications toast setting —
    // a failed recording always warrants a prominent, dismissible dialog.
    void recordingFailed(const ui::dialogs::RecordingErrorModel& model);

    // CAPTURE-FRAME-BUTTON-R1: emitted on a successful frame capture so MainWindow
    // can enqueue a "Frame saved" success toast.
    // frame_path: full path to the saved PNG file.
    void captureFrameSaved(const QString& frame_path);

    // COUNTDOWN-OVERLAY-R1: emitted whenever countdown state changes.
    // active=true while the pre-record countdown is running.
    // remaining_seconds: current digit value (1..duration_seconds).
    // duration_seconds: total countdown length for ring-progress computation.
    // When active=false both second values are 0.
    void countdownStateChanged(bool active, int remaining_seconds, int duration_seconds);

    // PHASE-G-EDIT-EXPORT-R1: emitted when the user wants to edit/export a recording.
    void editExportRequested(const QString& file_path, const QString& duration, const QString& size,
                             const QString& resolution, const QString& fps, const QString& video_codec,
                             const QString& audio_codec, const QString& container);

  public slots:
    void onHotkeyToggle();
    void onHotkeyPauseToggle();
    void onHotkeyCaptureFrame();
    void onHotkeyAddMarker();
    void onHotkeySplitRecording();
    void setSourcePickerOverlay(ui::dialogs::SourcePickerOverlay* overlay);

    // ADR-0015: arm a recovery candidate for "Continue" continuation.
    // Delegates to the coordinator's ArmFromRecovery(). No-op when coordinator
    // is not yet initialised (deferred init); the overlay can only appear after
    // coordinatorInitialized() fires so this is safe in practice.
    void armFromRecovery(const RecoveryManifestEntry& entry);
    // Refresh display/window source list; call on screen add/remove events.
    void refreshDisplayTargets();

    // Reactive device-change handlers (driven by MainWindow from the three notifiers).
    // Each handler preserves the configured selection by stable ID; never emits
    // audioSettingsChanged / webcamSettingsChanged; never dirties the preset.
    void onAudioDevicesChanged(const exosnap::AudioDeviceSnapshot& snap);
    void onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap);
    void onDisplaysChanged(const exosnap::DisplaySnapshot& snap);

  private slots:
    void onStart();
    void onStop();
    void onPause();
    void onResume();
    void onSelectMonitorTarget();
    void onSelectWindowTarget();
    void onSelectRegionTarget();
    void onOpenSourcePicker();
    void onSourcePickerAccepted(ui::dialogs::SourcePickerDialog::SelectionResult result);
    void onTargetPickerChanged(int index);
    void onRefreshTargets();
    void onRegionSelected(QRect region_virtual_screen);
    void onRegionCancelled();
    void onSourceDataRequested();

  private:
    void startRecordingFlow();
    void startCountdown(int seconds, std::optional<recorder_core::CaptureRegion> crop_region = std::nullopt);
    void cancelCountdown();
    void finishCountdown();
    void updateCountdown();
    bool isCountdownActive() const noexcept;
    void cancelActiveInteraction();
    void setInteractionMode(InteractionMode mode);
    // Resolve target and start recording (after any overlay selection is complete).
    void doStartRecording(std::optional<recorder_core::CaptureRegion> crop_region = std::nullopt);
    // Ensure the region overlay widget exists.
    void ensureRegionOverlay();
    QRect selectedMonitorRect() const;
    QRect currentRegionRect() const;

    struct ReadinessRow {
        QLabel* icon = nullptr;
        QLabel* title = nullptr;
        QLabel* detail = nullptr;
    };

    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void ensureCoordinatorInit();
    void initCoordinator();
    // Resolve+validate shared_runtime_caps_ and hand them to the coordinator
    // (OnCapabilitiesReady), catching probe/resolver failures. Used both from
    // initCoordinator (when caps are already present) and from the deferred
    // delivery in setRuntimeCapabilities() (when the async probe lands later).
    void deliverCapabilitiesToCoordinator();
    void refresh();
    void updateStatsDisplay();
    void updateResultDisplay();
    void updateTransportDock();
    void onDockSourceToggle(const QString& key);
    void onDockFilenameActivated();
    void updateTargetCards();
    void updateReadinessRows();
    void updateRecReadiness(); // runs RecommendationEngine, caches rec_checklist_
    void updateResponsiveLayout();
    void updateAudioMeterLevels();
    void updateAudioControls();
    void updateAudioControlsVisibility();
    void updateAudioTrackPreview();
    void updateHeroButton();
    void updateRailSourceStatusChips();
    void updateSourceChip();
    void updateOpenFolderButtonState();
    void updateDestinationMeta();
    void updateResultDetailsPanel();
    void hideResultDetailsPanel();
    void updateReportCard(); // populates pipeline stats in resultDetailsPanel
    void syncTargetSelectionToCombo(int target_index);
    // When allow_fallback is false the reactive path gets no silent switch:
    // a vanished target becomes unresolved (selected_target_index = -1) instead
    // of auto-picking the next monitor/window.
    void enumerateTargets(bool preserve_current_selection, bool allow_fallback_to_other_target = true);
    void rebuildTargetPicker();
    void pushSourceDataToPicker();
    void onAudioRowEnabledChanged(int row_index, bool enabled);
    void onAudioRowMergeChanged(int row_index, bool merge);
    void onAudioRowGainChanged(int row_index, float gain_db);
    void onAudioRowMutedChanged(int row_index, bool muted);
    void swapAudioSourceRows(int a, int b);
    void rebuildAudioRowWidgets();
    void updateAudioRowMergeVisibility();
    void onMicDeviceChanged(int index);
    void onMicChannelChanged(int index);
    void onMicGainChanged(int db_value);
    void onAudioBitrateChanged(int kbps);
    void onOpusFrameDurationChanged(int index);
    void onOpusComplexityChanged(int value);
    void openOutputFolder();
    void onCopyFilePath();
    void onRenameFile();
    void onDeleteFile();
    void onRecentItemOpen(int history_index);
    void onRecentItemOpenFolder(int history_index);
    void setOutputSettingsSummary(const OutputSettingsModel& settings);
    void populateMicDeviceCombo();
    void updateMicDeviceNoteLabel();
    void syncMicMeterService();
    void syncSysMeterService();
    void syncAppMeterService();
    void emitAudioSettingsChanged();
    void emitChromeState();
    void syncCoordinatorTargetContext();
    void startPreviewIfIdle();
    // Push webcam enable/mirror/aspect/placement and the state-driven edit lock to
    // the preview surface. Live-editable states are defined by IsWebcamOverlayEditable().
    void updateWebcamOverlay();
    // Start/stop the shared idle webcam capture so the Ready preview shows a live PiP.
    void syncWebcamPreviewCapture();
    // Persist a confirmed PiP placement change from the preview (marks user-placed).
    void onWebcamOverlayMoved(QRectF rect_norm);
    void updatePreviewHeightClamp();
    QString buildChromeStatusLabel() const;
    QString buildPreviewBottomLeftText(bool recording) const;
    QString buildPreviewBottomRightText(bool recording) const;
    QString buildTimerText(bool recording) const;
    bool isSourceSelectionLocked() const;
    void showCaptureFrameStatus(bool success, const QString& path, const QString& error);
    void onDockAddMarker();
    void onDockSplit();
    void requestSplit(recorder_core::SplitTriggerSource source);
    void showMarkerFeedback(const QString& text);

    int monitor_target_index_ = -1;
    int window_target_index_ = -1;
    std::vector<int> monitor_target_indices_;
    std::vector<int> window_target_indices_;
    std::vector<recorder_core::AudioInputDeviceInfo> mic_devices_;

    RecordViewModel view_model_;
    RecordingHistoryStore history_store_;
    std::unique_ptr<RecordingCoordinator> coordinator_;
    std::unique_ptr<PreviewService> preview_service_;
    // Injected by MainWindow before first show; forwarded to the coordinator.
    RecoveryManifestStore* recovery_manifest_store_ = nullptr;

    QLabel* capability_label_ = nullptr;
    QComboBox* target_combo_ = nullptr;
    QBoxLayout* cockpit_split_layout_ = nullptr;
    QWidget* preview_column_ = nullptr;
    QWidget* preview_surface_host_ = nullptr;
    QFrame* target_picker_panel_ = nullptr;
    QLabel* target_picker_kind_label_ = nullptr;
    QComboBox* target_picker_combo_ = nullptr;
    QPushButton* target_refresh_btn_ = nullptr;
    QLabel* target_picker_note_label_ = nullptr;
    ui::widgets::PreviewSurface* preview_surface_ = nullptr;
    QLabel* control_state_label_ = nullptr;
    QLabel* timer_label_ = nullptr;
    ui::widgets::SectionRuleHeader* capture_header_ = nullptr;
    QWidget* source_row_ = nullptr;
    QFrame* source_chip_panel_ = nullptr;
    QLabel* source_name_label_ = nullptr;
    QLabel* source_lock_label_ = nullptr;
    QPushButton* change_source_btn_ = nullptr;
    ui::dialogs::SourcePickerOverlay* source_picker_overlay_ = nullptr;
    ui::widgets::CaptureTargetCard* monitor_card_ = nullptr;
    ui::widgets::CaptureTargetCard* window_card_ = nullptr;
    ui::widgets::CaptureTargetCard* region_card_ = nullptr;
    QPushButton* region_pick_btn_ = nullptr;
    QLabel* region_summary_label_ = nullptr;
    QWidget* region_options_panel_ = nullptr;
    ui::widgets::RegionSelectionOverlay* region_overlay_ = nullptr;
    ui::widgets::ExoCheckBox* select_on_record_check_ = nullptr;
    ui::widgets::SectionRuleHeader* readiness_header_ = nullptr;
    QFrame* readiness_panel_ = nullptr;
    QFrame* readiness_rule_ = nullptr;
    QWidget* readiness_rows_container_ = nullptr;
    QPushButton* readiness_diagnostics_btn_ = nullptr;
    std::vector<ReadinessRow> readiness_rows_;
    ui::widgets::SectionRuleHeader* audio_settings_header_ = nullptr;
    QWidget* audio_rows_container_ = nullptr;
    QVBoxLayout* audio_rows_layout_ = nullptr;
    std::vector<ui::widgets::AudioSourceRow*> audio_source_rows_;
    int drag_source_index_ = -1;
    int drag_start_y_ = 0;
    QWidget* mic_device_row_ = nullptr;
    QComboBox* mic_device_combo_ = nullptr;
    QPushButton* mic_refresh_btn_ = nullptr;
    QLabel* mic_device_note_label_ = nullptr;
    QWidget* mic_channel_row_ = nullptr;
    QComboBox* mic_channel_combo_ = nullptr;
    QWidget* mic_gain_row_ = nullptr;
    QSlider* mic_gain_slider_ = nullptr;
    QLabel* mic_gain_value_label_ = nullptr;
    // Audio encoding params (ADR 0019).
    QWidget* audio_bitrate_row_ = nullptr;
    QSpinBox* audio_bitrate_spin_ = nullptr;
    QWidget* opus_frame_duration_row_ = nullptr;
    QComboBox* opus_frame_duration_combo_ = nullptr;
    QWidget* opus_complexity_row_ = nullptr;
    QSpinBox* opus_complexity_spin_ = nullptr;
    QFrame* track_preview_panel_ = nullptr;
    QVBoxLayout* track_preview_layout_ = nullptr;
    ui::widgets::SectionRuleHeader* audio_header_ = nullptr;
    ui::widgets::VUMeterWidget* app_meter_ = nullptr;
    ui::widgets::VUMeterWidget* mic_meter_ = nullptr;
    ui::widgets::VUMeterWidget* sys_meter_ = nullptr;
    QLabel* app_db_label_ = nullptr;
    QLabel* mic_db_label_ = nullptr;
    QLabel* sys_db_label_ = nullptr;
    ui::widgets::SectionRuleHeader* destination_header_ = nullptr;
    QLabel* output_path_label_ = nullptr;
    QLabel* output_meta_label_ = nullptr;
    QPushButton* open_folder_btn_ = nullptr;
    QPushButton* destination_settings_btn_ = nullptr;
    QFrame* result_panel_ = nullptr;
    QLabel* result_title_label_ = nullptr;
    QLabel* result_message_label_ = nullptr;
    QLabel* result_action_label_ = nullptr;
    QLabel* result_file_label_ = nullptr;
    QLabel* result_stats_label_ = nullptr;
    QLabel* result_path_label_ = nullptr;
    QLabel* result_technical_label_ = nullptr;
    QFrame* result_technical_separator_ = nullptr;
    std::filesystem::path last_output_folder_;
    OutputSettingsModel current_output_settings_;
    capability::Container current_container_ = capability::Container::Matroska;
    capability::VideoCodec current_video_codec_ = capability::VideoCodec::H264Nvenc;
    capability::AudioCodec current_audio_codec_ = capability::AudioCodec::AacMf;
    OutputResolutionSettings current_output_resolution_{};
    uint32_t current_frame_rate_num_ = 60;
    uint32_t current_frame_rate_den_ = 1;
    bool current_cfr_ = true;
    WebcamSettings current_webcam_settings_{};
    std::wstring active_profile_name_;
    float preflight_mic_rms_ = 0.0f;
    float preflight_sys_rms_ = 0.0f;
    float preflight_app_rms_ = 0.0f;
    uint32_t preflight_app_pid_ = 0;
    bool coordinator_needs_init_ = true;
    // True while the Record page is the active page in the QStackedWidget.
    // Meter services (especially mic) run when this is true even if the
    // corresponding source toggle is off, so the dock shows a live (grey)
    // level preview before the user enables recording.
    bool record_page_visible_ = false;
    capability::CapabilitySet shared_runtime_caps_{};
    // True once VALID runtime caps have been delivered to the coordinator (success).
    // Stays false if the async probe fails — updateRecReadiness() / the recommendation
    // engine key off this, so they must only see genuinely-resolved caps.
    bool shared_runtime_caps_received_ = false;
    // True when initCoordinator() built the coordinator before the async capability
    // probe resolved. Cleared on BOTH success (caps delivered) and failure. Gates the
    // early-start latch below: a Record click is latched only while this is true.
    bool coordinator_awaiting_caps_ = false;
    // Latched intent for a recording start requested before caps resolved; replayed
    // from the caps-delivery path (setRuntimeCapabilities) so the click is never dropped.
    bool start_requested_awaiting_caps_ = false;
    std::optional<recorder_core::CaptureRegion> pending_start_crop_region_{};

    // Rail dashboard controls
    QWidget* audio_settings_panel_ = nullptr;
    QFrame* destination_panel_ = nullptr;
    QPushButton* hero_action_btn_ = nullptr;
    QPushButton* secondary_action_btn_ = nullptr;
    QPushButton* rail_diagnostics_btn_ = nullptr;
    QFrame* rail_control_panel_ = nullptr;
    QFrame* rail_stats_grid_ = nullptr;
    QWidget* rail_source_status_panel_ = nullptr;
    QLabel* rail_source_status_summary_label_ = nullptr;
    QLabel* rail_sys_audio_chip_ = nullptr;
    QLabel* rail_app_audio_chip_ = nullptr;
    QLabel* rail_mic_chip_ = nullptr;
    QLabel* rail_webcam_chip_ = nullptr;
    QLabel* rail_size_value_label_ = nullptr;
    QLabel* rail_drop_value_label_ = nullptr;
    QFrame* rail_fps_stat_cell_ = nullptr;
    QLabel* rail_fps_value_label_ = nullptr;
    QLabel* rail_readiness_label_ = nullptr;
    QLabel* rail_summary_label_ = nullptr;
    QLabel* rail_stats_label_ = nullptr;
    QLabel* readiness_summary_label_ = nullptr;
    QPushButton* result_open_folder_btn_ = nullptr;
    QPushButton* result_record_again_btn_ = nullptr;
    QPushButton* result_copy_path_btn_ = nullptr;
    QPushButton* result_rename_btn_ = nullptr;
    QPushButton* result_delete_btn_ = nullptr;

    // Inline rename overlay
    QWidget* rename_overlay_ = nullptr;
    QLineEdit* rename_edit_ = nullptr;
    QPushButton* rename_confirm_btn_ = nullptr;
    QPushButton* rename_cancel_btn_ = nullptr;
    QLabel* rename_error_label_ = nullptr;

    // Delete confirmation overlay
    QWidget* delete_confirm_overlay_ = nullptr;
    QPushButton* delete_confirm_yes_btn_ = nullptr;
    QPushButton* delete_confirm_no_btn_ = nullptr;

    // Hybrid v3 preview-first chrome (HYBRID-PORT-R2).
    ui::widgets::TransportDock* transport_dock_ = nullptr;
    QWidget* legacy_host_ = nullptr;
    QLabel* capture_frame_status_label_ = nullptr;
    QTimer* capture_frame_status_timer_ = nullptr;

    // Marker feedback label (brief flash near the dock)
    QLabel* marker_feedback_label_ = nullptr;
    QTimer* marker_feedback_timer_ = nullptr;

    // Latest preview frame for Ready-state Capture Frame.
    QImage latest_preview_frame_;

    // View-layer elapsed-time fallback used while live backend stats are pending.
    // Starts when recording begins; pauses/resumes with the recording state.
    QElapsedTimer recording_wall_clock_;
    qint64 wall_elapsed_before_pause_ms_ = 0; // accumulated ms before most recent pause
    QTimer* ui_clock_timer_ = nullptr;        // 1 Hz tick → updateTransportDock() during recording
    QTimer* countdown_timer_ = nullptr;       // monotonic countdown refresh
    QElapsedTimer countdown_clock_;
    RecordingCountdownController countdown_;
    int selected_countdown_seconds_ = 0;
    int countdown_remaining_seconds_ = 0;
    std::optional<recorder_core::CaptureRegion> pending_countdown_region_;
    InteractionMode interaction_mode_ = InteractionMode::None;

    // Tracks the configuration of the last successfully started DXGI preview.
    // Used by startPreviewIfIdle() to skip redundant restarts when the target
    // and crop are unchanged.  Reset to default when the preview is stopped.
    exosnap::PreviewConfigKey last_preview_key_{};

    // True while applyCapturePolicy() or setCountdownSeconds() is driving
    // widgets programmatically.  Prevents recordingConfigChanged() from being
    // emitted for these non-user changes.
    bool applying_external_config_ = false;

    // v0.8.0-D: Pre-flight readiness gate
    // Cached recommendation checklist (quick sync; refreshed on settings/caps change)
    diagnostics::DiagnosticChecklist rec_checklist_;
    bool rec_checklist_valid_ = false;

    // v0.8.0-D: Post-flight report card — pipeline stats accumulated during recording
    double peak_av_drift_ms_ = 0.0;
    bool av_drift_ever_available_ = false;
    recorder_core::PipelineHealth last_pipeline_health_ = recorder_core::PipelineHealth::Idle;
    recorder_core::RecordingDiagnosticsSnapshot last_completed_snapshot_;
    // Report card dismiss button (inside resultDetailsPanel)
    QPushButton* report_card_dismiss_btn_ = nullptr;

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    bool visual_test_mode_ = false;
#endif
};

} // namespace exosnap

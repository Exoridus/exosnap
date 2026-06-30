#pragma once
#include <QWidget>

#include "../../../libs/capability/include/capability/audio_ui_state.h"
#include "../../../libs/recorder_core/include/recorder_core/audio_input_device.h"
#include "../models/OutputSettingsModel.h"
#include "../models/VideoSettingsModel.h"
#include "../models/WebcamSettings.h"
#include "../services/AudioDeviceNotifier.h"
#include "../services/WebcamDeviceNotifier.h"
#include "../viewmodels/PresentationState.h"

#include <filesystem>
#include <string>
#include <vector>

class QAction;
class QBoxLayout;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QSlider;
class QSpinBox;
class QString;
class QToolButton;

namespace exosnap {
class GlobalHotkeyService;
} // namespace exosnap

namespace exosnap::ui::widgets {
class CompareHint;
class ExoCheckBox;
class ExoSlider;
class ExoToggle;
class HotkeysSettingsPanel;
class SettingsPopoverRow;
class VUMeterWidget;
class WebcamSetupPanel;
} // namespace exosnap::ui::widgets

namespace exosnap {

class ConfigPage : public QWidget {
    Q_OBJECT
  public:
    struct ProfileOption {
        QString id;
        QString label;
        bool built_in = false;
        bool modified = false;
        bool available = true;
        QString availability_reason;
    };

    explicit ConfigPage(const OutputSettingsModel& initial_settings, const VideoSettingsModel& initial_video,
                        QWidget* parent = nullptr);

    void setOutputSettings(const OutputSettingsModel& settings);
    void setVideoSettings(const VideoSettingsModel& settings);
    void setOutputFolder(const std::filesystem::path& folder);
    void setAudioUiState(const capability::AudioUiState& state);
    void setWebcamSettings(const WebcamSettings& settings);
    void setReadinessStatus(const QString& status_label);
    // Preset card contract: options = presets (id + label); selected_id = active preset;
    // default_id = startup-default preset (shown with a badge); dirty = unsaved changes.
    void setPresetOptions(const std::vector<ProfileOption>& options, const QString& selected_id,
                          const QString& default_id, bool dirty);
    // Lightweight dirty-only update (avoids rebuilding the full combo).
    void setPresetDirty(bool dirty);
    void setActiveProfileName(const QString& profile_name);
    void setRecordingControlsLocked(bool locked);

    // PS-PHASE-C: Wire the embedded hotkeys panel to the live global hotkey service.
    void setHotkeyService(GlobalHotkeyService* service);
    // PS-PHASE-C: Lock/unlock hotkey rebinding (forwarded from recording state).
    void setHotkeyEditingLocked(bool locked);

    // PS-PHASE-E: deep-link target support — scrolls the Settings page to the named section.
    // Target strings: "settings/audio", "settings/output", "settings/video", "settings/webcam",
    //                 "settings/presence", "settings/appearance", "settings/hotkeys".
    // No-op for unknown targets.
    void scrollToSection(const QString& section_target);

    // SETTINGS-TIERS-R1: Expert mode toggle (persisted by MainWindow).
    void setExpertModeEnabled(bool enabled);
    [[nodiscard]] bool expertModeEnabled() const noexcept;

    // SETTINGS-TIERS-R1: Per-card expander state — no-op stubs kept for MainWindow compat.
    // Wave 2: the output-split expander was dissolved; split controls are now expert-gated.
    void setOutputSplitExpanderExpanded(bool expanded);
    [[nodiscard]] bool outputSplitExpanderExpanded() const noexcept;
    void setAudioSeparateExpanderExpanded(bool expanded);
    [[nodiscard]] bool audioSeparateExpanderExpanded() const noexcept;

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    // Drive the embedded Webcam card deterministically for visual-test scenarios.
    void applyVisualWebcamState(bool available, bool mirror);

    // Drive the inline preset save-error affordance for visual-test scenarios.
    // Passing true shows a deterministic error label (name-conflict copy);
    // passing false hides it.  No real save is performed.
    void applyVisualPresetSaveError(bool show);
#endif

    // Live audio meter update forwarded from RecordPage via MainWindow.
    // sys01/app01/mic01: pre-computed 0..1 dock-level values (0 = inactive or silence).
    // sys/app/mic_active: true when the meter service is running for that source.
    void setAudioMeterLevels(float sys01, float app01, float mic01, bool sys_active, bool app_active, bool mic_active);

    // SETTINGS-TIERS-P3: presence + appearance setters (moved from AdvancedPage).
    void setShowOverlay(bool show);
    void setShowDiagnosticsOverlay(bool show);
    void setShowNotifications(bool show);
    void setKeepRunningInTray(bool keep);
    void setShowQuickControls(bool show);
    // ADR 0033: seeds the "Present & tearing diagnostics" opt-in toggle from
    // persisted settings (no signal emitted).
    void setPresentDiagnosticsOptIn(bool on);
    void setThemeId(const QString& theme_id);

    // Drives the visible Updates card (ADR 0034 Phase A). state is one of
    // "checking" | "uptodate" | "available" | "error". When "available",
    // available_version fills the "Update to vX.Y" action; on "error", detail
    // is shown. last_checked is a human string ("Just now" / "Never").
    void setUpdateStatus(const QString& state, const QString& available_version, const QString& last_checked,
                         const QString& detail = QString());
    // Seeds the "Check for updates automatically" toggle from persisted settings
    // (no signal emitted).
    void setAutoUpdateCheck(bool on);

    // Reactive device-change handlers (driven by MainWindow from the three notifiers).
    // These preserve selection state and never emit settings-changed or dirty the preset.
    void onAudioDevicesChanged(const exosnap::AudioDeviceSnapshot& snap);
    void onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap);

  signals:
    void formatSettingsChanged(const OutputSettingsModel& settings);
    // Preset-selection signal.
    void presetSelected(const QString& id);
    void videoSettingsChanged(const VideoSettingsModel& settings);
    void audioSettingsChanged(const capability::AudioUiState& state);
    void webcamSettingsChanged(const WebcamSettings& settings);
    void diagnosticsRequested();
    void webcamDetailsRequested();

    // SETTINGS-TIERS-R1: emitted when Expert mode changes via the toggle button.
    void expertModeChanged(bool enabled);
    // Emitted when the output-split expander is toggled.
    void outputSplitExpanderChanged(bool expanded);
    // Emitted when the audio-separate expander is toggled.
    void audioSeparateExpanderChanged(bool expanded);

    // Emitted when the user presses the Rescan button in the Audio card.
    // MainWindow connects this to audio_notifier_.rescan() so Rescan and the
    // reactive path share the same canonical refresh, with no duplicate
    // enumeration and no duplicate devices.
    void audioRescanRequested();

    // ---- Updates card signals (ADR 0034 Phase A) ----
    // Manual "Check for updates" press; MainWindow flags this as a user-initiated
    // check so the resulting available-state does NOT also raise a notification.
    void checkForUpdatesRequested();
    // Primary action while an update is available ("Update to vX.Y"). Phase A
    // hands off to the releases page; Phase B starts the in-app download.
    void updatePrimaryActionRequested();
    // "Check for updates automatically" toggle changed.
    void autoUpdateCheckToggled(bool enabled);

    // SETTINGS-TIERS-P3: presence + appearance signals (moved from AdvancedPage).
    void showOverlayChanged(bool show);
    void showDiagnosticsOverlayChanged(bool show);
    void showNotificationsChanged(bool show);
    void keepRunningInTrayChanged(bool keep);
    void showQuickControlsChanged(bool show);
    // ADR 0033: emitted when the user toggles the present-diagnostics opt-in.
    void presentDiagnosticsOptInToggled(bool enabled);
    void themeIdChanged(const QString& theme_id);

    // ---- Preset management signals ----
    void savePresetRequested();
    void savePresetAsRequested(const QString& name);
    void newPresetRequested();
    void duplicatePresetRequested();
    void renamePresetRequested(const QString& name);
    void deletePresetRequested();
    void resetChangesRequested();
    void resetToDefaultsRequested();
    void setDefaultPresetRequested();
    // Emitted when the user opens the Manage presets overlay.
    void managePresetsRequested();
    // Emitted when the user clicks Export in the preset toolbar.
    void exportCurrentPresetRequested(const QString& path);
    // Emitted when the user clicks Import in the preset toolbar.
    void importPresetsRequested(const QString& path);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void updateResponsiveLayout();
    void onContainerChanged(int id);
    void onVideoCodecChanged(int index);
    void onVideoBitDepthChanged(int index);
    void onVideoColorRangeChanged(int index);
    void onAudioCodecChanged(int index);
    void onProfileSelectionChanged(int index);
    void onQualityChanged(int index);
    void onQualitySegmentSelected(int preset_id);
    void onFrameRateChanged(int index);
    void onTimingSelected(int timing_id);
    void onOutputResolutionSelected(int mode_id);
    void onSplitModeChanged(int index);
    void updateSplitSelection();
    void onSplitSizeModeChanged(int index);
    void updateSplitSizeSelection();
    void onCursorChanged();
    void updateQualitySegmentSelection();
    void updateFrameRateSelection();
    void updateTimingSelection();
    void updateOutputResolutionSelection();
    void updateCustomResolutionVisibility();
    void updateCustomResolutionValidation();
    void onCustomWidthChanged(int value);
    void onCustomHeightChanged(int value);
    void updateEffectiveOutputSummary();
    void onBrowse();
    void onDestinationEditingFinished();
    void onPatternEditingFinished();
    void emitCurrentFormatSettings();
    void emitCurrentVideoSettings();
    void reconcileContainerCodecRules();
    void updateVideoCodecChoices();
    // Syncs the video bit-depth combo to the model and capability-gates the 10-bit
    // item (selectable only for HEVC / AV1). Single source of truth: caps QueryCombo.
    void updateVideoBitDepthControl();
    // Syncs the colour-range combo to the model. NOT capability-gated — both Full
    // and Limited are always valid; only the recording lock disables it.
    void updateVideoColorRangeControl();
    // Syncs the frame-pacing combo to the model. Not capability-gated — both modes
    // are always valid; only the recording lock disables it.
    void updateFramePacingControl();
    void updateAudioCodecChoices();
    void updateFormatDisplay();
    void updateCompatCallout();
    void updateOutputValidationState();
    void updateExampleFilename();

    // Single entry point for atomic audio widget state application.
    // Derives widget visibility, enabled, checked, and label states from the
    // stored audio_ui_state_ and controls_locked_ in one pass.  Both
    // setAudioUiState() and setRecordingControlsLocked() delegate here after
    // updating their respective stored value, ensuring call order cannot produce
    // inconsistent widget state.
    void applyAudioConfigurationState();

    void onAudioAppToggled();
    void onAudioMicToggled();
    void onAudioSysToggled();
    void onAudioAppSeparateToggled();
    void onAudioMicSeparateToggled();
    void onAudioSysSeparateToggled();
    void onMicDeviceChanged(int index);
    void refreshMicDevices();
    void emitCurrentAudioSettings();
    // Update codec-gated visibility for the four ADR 0030 audio format controls.
    void updateAudioFormatControlVisibility();

    // Preset management handlers.
    void onSavePreset();
    void onSavePresetAs();
    void onNewPreset();
    void onDuplicatePreset();
    void onRenamePreset();
    void onDeletePreset();
    void onResetChanges();
    void onResetToDefaults();
    void onSetDefaultPreset();
    void onManagePresets();
    void updatePresetActionState();
    void updateExpertModeVisibility();

    capability::AudioUiState audio_ui_state_;
    WebcamSettings webcam_settings_;

    OutputSettingsModel format_settings_;
    VideoSettingsModel video_settings_;
    QString active_profile_name_;
    std::vector<ProfileOption> profile_options_;
    QString active_preset_id_;
    QString default_preset_id_;
    bool preset_dirty_ = false;
    // Current selected preset's built_in/available flags (set by setPresetOptions).
    bool active_preset_is_built_in_ = false;
    bool active_preset_is_available_ = true;

    QScrollArea* scroll_area_ = nullptr;        // main scroll area (for scrollToSection)
    QBoxLayout* columns_layout_ = nullptr;      // host for the two-column card grid
    QBoxLayout* output_split_layout_ = nullptr; // inner field/help split inside Output card

    // v10/Canon: Container is a dropdown (SSelect), not a segmented button group.
    QComboBox* container_combo_ = nullptr;
    QComboBox* video_codec_combo_ = nullptr;
    QComboBox* audio_codec_combo_ = nullptr;
    QComboBox* profile_combo_ = nullptr;

    // D6: CompareHint pointers for setCurrentValue sync
    ui::widgets::CompareHint* container_compare_hint_ = nullptr;
    ui::widgets::CompareHint* video_codec_compare_hint_ = nullptr;
    ui::widgets::CompareHint* audio_codec_compare_hint_ = nullptr;
    ui::widgets::CompareHint* quality_compare_hint_ = nullptr;
    ui::widgets::CompareHint* timing_compare_hint_ = nullptr;
    ui::widgets::CompareHint* resolution_compare_hint_ = nullptr;

    // D6: compat callout widgets (the visible format summary + warning)
    QFrame* compat_callout_widget_ = nullptr;
    QLabel* callout_text_ = nullptr;
    QLabel* compat_ok_label_ = nullptr;

    QComboBox* quality_combo_ = nullptr;
    QComboBox* frame_rate_combo_ = nullptr;
    QButtonGroup* quality_segment_group_ = nullptr;
    QPushButton* quality_segment_small_ = nullptr;
    QPushButton* quality_segment_balanced_ = nullptr;
    QPushButton* quality_segment_high_ = nullptr;
    // v10/Canon: Frame timing is a dropdown (SSelect), not a segmented button group.
    QComboBox* timing_combo_ = nullptr;
    ui::widgets::ExoToggle* cursor_check_ = nullptr;

    QLabel* audio_summary_label_ = nullptr;

    // Per-source compact mono meters in the Settings Audio card.
    ui::widgets::VUMeterWidget* audio_sys_meter_ = nullptr;
    ui::widgets::VUMeterWidget* audio_app_meter_ = nullptr;
    ui::widgets::VUMeterWidget* audio_mic_meter_ = nullptr;
    QLabel* audio_sys_db_label_ = nullptr;
    QLabel* audio_app_db_label_ = nullptr;
    QLabel* audio_mic_db_label_ = nullptr;

    // Application audio section — shown for Window targets, hidden for Display/Region.
    QWidget* app_row_section_ = nullptr;

    ui::widgets::ExoCheckBox* app_enabled_check_ = nullptr;
    ui::widgets::ExoToggle* app_separate_check_ = nullptr; // DF-12: pill toggle replaces QCheckBox
    QLabel* app_source_label_ = nullptr;

    ui::widgets::ExoCheckBox* mic_enabled_check_ = nullptr;
    ui::widgets::ExoToggle* mic_separate_check_ = nullptr; // DF-12: pill toggle replaces QCheckBox
    QComboBox* mic_device_combo_ = nullptr;
    // Compact Rescan affordance for the Settings Audio card.
    // Emits audioRescanRequested() which MainWindow routes to audio_notifier_.rescan().
    QPushButton* audio_rescan_btn_ = nullptr;
    QLabel* mic_source_label_ = nullptr;

    std::vector<recorder_core::AudioInputDeviceInfo> mic_devices_;

    ui::widgets::ExoCheckBox* sys_enabled_check_ = nullptr;
    ui::widgets::ExoToggle* sys_separate_check_ = nullptr; // DF-12: pill toggle replaces QCheckBox
    QLabel* sys_source_label_ = nullptr;

    QLineEdit* destination_edit_ = nullptr;
    QPushButton* browse_btn_ = nullptr;
    QLineEdit* naming_edit_ = nullptr;
    QComboBox* output_res_combo_ = nullptr;
    QLabel* output_effective_summary_label_ = nullptr;

    // Split recording widgets (SPLIT-RECORDING-R1 / SPLIT-BY-SIZE-R1).
    QComboBox* split_mode_combo_ = nullptr;
    QSpinBox* split_custom_minutes_spin_ = nullptr;
    QWidget* split_custom_widget_ = nullptr;
    QLabel* split_summary_label_ = nullptr;
    // Size-split controls (SPLIT-BY-SIZE-R1).
    QComboBox* split_size_mode_combo_ = nullptr;
    QSpinBox* split_custom_size_spin_ = nullptr;
    QWidget* split_size_custom_widget_ = nullptr;

    // Custom resolution widgets (CUSTOM-OUTPUT-RESOLUTION-R1).
    QWidget* custom_resolution_widget_ = nullptr;
    QSpinBox* custom_width_spin_ = nullptr;
    QSpinBox* custom_height_spin_ = nullptr;
    QLabel* custom_resolution_validation_label_ = nullptr;
    // Stashed values preserved across mode switches.
    uint32_t stashed_custom_width_ = 0;
    uint32_t stashed_custom_height_ = 0;
    QLabel* folder_validation_label_ = nullptr;
    QLabel* pattern_validation_label_ = nullptr;
    QLabel* example_filename_label_ = nullptr;

    QFrame* readiness_panel_ = nullptr;
    QLabel* readiness_badge_label_ = nullptr;
    QLabel* readiness_detail_label_ = nullptr;
    QPushButton* view_details_btn_ = nullptr;

    // Preset card widgets.
    QLabel* profile_status_label_ = nullptr;
    QLabel* preset_dirty_indicator_ = nullptr;
    QPushButton* preset_save_btn_ = nullptr;
    QPushButton* preset_save_as_btn_ = nullptr;
    QPushButton* preset_export_btn_ = nullptr;
    QPushButton* preset_import_btn_ = nullptr;
    QToolButton* profile_overflow_btn_ = nullptr;
    // Preset management actions in the overflow menu.
    QAction* save_preset_action_ = nullptr;
    QAction* save_preset_as_action_ = nullptr;
    QAction* new_preset_action_ = nullptr;
    QAction* duplicate_preset_action_ = nullptr;
    QAction* rename_preset_action_ = nullptr;
    QAction* delete_preset_action_ = nullptr;
    QAction* reset_changes_action_ = nullptr;
    QAction* reset_to_defaults_action_ = nullptr;
    QAction* set_default_preset_action_ = nullptr;
    QAction* manage_presets_action_ = nullptr;

    ui::widgets::WebcamSetupPanel* webcam_setup_panel_ = nullptr;

    QLabel* lock_note_label_ = nullptr;
    bool controls_locked_ = false;

    QWidget* token_chip_flow_ = nullptr; // v10: always visible below pattern input

    // SETTINGS-TIERS-R1 / D6: Expert mode toggle (ExoToggle in D6 header zone).
    ui::widgets::ExoToggle* expert_mode_toggle_ = nullptr;
    QLabel* expert_mode_label_ = nullptr;   // "Expert mode" label (mut -> accent when on)
    QWidget* expert_warn_banner_ = nullptr; // amber banner above grid, visible only in expert mode
    bool expert_mode_enabled_ = false;
    // Wave 2: split recording controls moved out of expander; now expert-gated section.
    QWidget* split_expert_section_ = nullptr;

    // Wave 2: Part B — CQ precision spinbox row.
    QWidget* quality_expert_widget_ = nullptr; // CQ spinbox row shown in expert mode
    QSpinBox* quality_cq_spin_ = nullptr;      // precision CQ input (range 1–51)
    QLabel* quality_cq_tier_label_ = nullptr;  // S3: "· High / Balanced / Small / Custom" tier label

    // audio_separate_expander_ is null (Phase 1b); kept as no-op for compat.
    // output_split_expander_ removed in Wave 2; split_expert_section_ replaces it.

    // SETTINGS-TIERS-P3: presence + appearance controls (moved from AdvancedPage).
    ui::widgets::ExoToggle* overlay_check_ = nullptr;
    ui::widgets::ExoToggle* diagnostics_overlay_check_ = nullptr;
    ui::widgets::ExoToggle* notifications_check_ = nullptr;
    ui::widgets::ExoToggle* keep_in_tray_check_ = nullptr;
    ui::widgets::ExoToggle* quick_controls_check_ = nullptr;
    // ADR 0033: present & tearing diagnostics opt-in (elevation-gated).
    ui::widgets::ExoToggle* present_diag_check_ = nullptr;
    // THEME-SLICE-1: theme picker (replaces accent_combo_).
    QButtonGroup* theme_button_group_ = nullptr;
    QWidget* theme_picker_widget_ = nullptr;
    QString current_theme_id_ = QStringLiteral("dark-default");
    // Expert-gated developer card (hidden when expert_mode_enabled_ == false).
    QWidget* developer_card_ = nullptr;

    // PS-PHASE-C: Embedded hotkeys panel — v10: single-width card in the LEFT column.
    ui::widgets::HotkeysSettingsPanel* hotkeys_settings_panel_ = nullptr;
    QWidget* hotkeys_panel_ = nullptr; // card wrapper (for search filtering + scrollToSection)
    // Updates card (right column, between Presence and Appearance).
    QWidget* updates_panel_ = nullptr;
    // ADR 0034 Phase A: live Updates-card controls.
    ui::widgets::ExoToggle* updates_auto_toggle_ = nullptr;
    QLabel* updates_status_label_ = nullptr;
    QPushButton* updates_action_btn_ = nullptr;
    QString updates_available_version_; // last advertised "vX.Y" (Available state)

    // v10 split: the old "Format & encoding" mega-card is split into
    // "Container & codecs" (fmt_panel_) and "Quality & timing" (quality_panel_).
    QWidget* quality_panel_ = nullptr; // "Quality & timing" card host
    // v10: Default Quality presentation is a single "Balanced · CQ 24" dropdown.
    // quality_combo_ stays the hidden model seam; quality_preset_combo_ is the
    // visible Default control mirroring it. quality_segment_group_ is preserved
    // (hidden) as a test seam.
    QComboBox* quality_preset_combo_ = nullptr;
    QWidget* quality_preset_row_widget_ = nullptr; // visible Default dropdown row
    // v10: rate-control + bitrate moved into the Quality & timing card (expert).
    QWidget* quality_rate_section_ = nullptr;
    // v10: Output "Saves to …\path" resolved footer (mirrors the Quality footer).
    QLabel* output_saves_to_label_ = nullptr;
    // PS-PHASE-C: Expert Format section — rate control (CQ/VBR/CBR) + bitrate + placeholders.
    QWidget* fmt_expert_section_ = nullptr; // container for rate control, bitrate, and Format placeholders
    QWidget* rate_control_row_widget_ = nullptr;
    // v10/Canon: Rate control is a dropdown (SSelect), not a segmented button group.
    QComboBox* rate_control_combo_ = nullptr;
    QWidget* bitrate_row_widget_ = nullptr;
    QSpinBox* bitrate_kbps_spin_ = nullptr;
    // Video bit depth (0.7.0 — S7): 8-bit / 10-bit selector, capability-gated.
    QWidget* video_bit_depth_row_ = nullptr;
    QComboBox* video_bit_depth_combo_ = nullptr;
    // Colour range (0.7.0): Full (PC) / Limited (TV) selector. Never gated.
    QWidget* video_color_range_row_ = nullptr;
    QComboBox* video_color_range_combo_ = nullptr;
    // Frame pacing (ADR 0035 Slice 2): Smooth / Newest selector. Never gated.
    QWidget* frame_pacing_row_ = nullptr;
    QComboBox* frame_pacing_combo_ = nullptr;
    // Keyframe interval (0.9.0 S1): 2 s / 1 s / 0.5 s. Expert only.
    QComboBox* keyframe_interval_combo_ = nullptr;

    // PS-PHASE-C: Expert Audio section — mic gain, channel mode, bitrate, Opus params + placeholders.
    QWidget* audio_expert_section_ = nullptr;
    // Mic-gain: ExoSlider (–12…+12 dB, step 1) + read-only dB label (Polish-R1: Slider per mockup).
    // S3: upgraded from QSlider to ExoSlider for gradient groove + tick marks.
    ui::widgets::ExoSlider* mic_gain_slider_ = nullptr;
    QLabel* mic_gain_db_label_ = nullptr;
    QComboBox* mic_channel_mode_combo_ = nullptr;
    QSpinBox* audio_bitrate_kbps_spin_ = nullptr;
    QComboBox* opus_frame_duration_combo_ = nullptr;
    QSpinBox* opus_complexity_spin_ = nullptr;
    // Brickwall limiter (Audio v2 — 0.6.0).
    ui::widgets::ExoCheckBox* limiter_check_ = nullptr;
    QDoubleSpinBox* limiter_ceiling_spin_ = nullptr;
    // Microphone high-pass filter (Audio v2 — 0.6.0).
    ui::widgets::ExoCheckBox* mic_hpf_check_ = nullptr;
    QDoubleSpinBox* mic_hpf_cutoff_spin_ = nullptr;
    // Microphone noise gate (Audio v2 — 0.6.0).
    ui::widgets::ExoCheckBox* mic_gate_check_ = nullptr;
    QDoubleSpinBox* mic_gate_threshold_spin_ = nullptr;
    // Microphone automatic gain control (Audio v2 — 0.6.0).
    ui::widgets::ExoCheckBox* mic_agc_check_ = nullptr;
    QDoubleSpinBox* mic_agc_target_spin_ = nullptr;
    // Microphone RNNoise neural noise suppression (Audio v2 — 0.6.0). Bool only.
    ui::widgets::ExoCheckBox* mic_rnnoise_check_ = nullptr;
    // S5: Collapsed mic post-processing popover row (HPF + Gate + AGC + RNNoise).
    ui::widgets::SettingsPopoverRow* mic_post_processing_row_ = nullptr;

    // Channel / sample-format model (ADR 0030 — 0.6.0).
    QComboBox* audio_sample_rate_combo_ = nullptr;
    QWidget* audio_sample_rate_row_ = nullptr;
    QComboBox* audio_channels_combo_ = nullptr;
    QWidget* audio_channels_row_ = nullptr;
    QComboBox* audio_bit_depth_combo_ = nullptr;
    QWidget* audio_bit_depth_row_ = nullptr;
    QSpinBox* flac_compression_spin_ = nullptr;
    QWidget* flac_compression_row_ = nullptr;

    // Card panel pointers (developer_card_ is already above; remaining cards are stored here).
    QWidget* fmt_panel_ = nullptr;
    QWidget* audio_panel_ = nullptr;
    QWidget* webcam_panel_ = nullptr;
    QWidget* out_panel_ = nullptr;
    QWidget* presence_panel_ = nullptr;
    QWidget* appearance_panel_ = nullptr;

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    // Inline error label for preset save-error visual-test scenario.
    // Created lazily on first applyVisualPresetSaveError(true) call and placed
    // below the preset selector row.  Hidden in all non-error-scenario states.
    QLabel* visual_preset_error_label_ = nullptr;
#endif
};

} // namespace exosnap

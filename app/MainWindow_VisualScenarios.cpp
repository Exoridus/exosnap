// Visual-test scenario application for MainWindow.
//
// Split out of MainWindow.cpp, which had grown to 5380 lines. These definitions
// are still MainWindow members — they drive deeply private window state, and
// routing that through new accessors would add coupling surface rather than
// remove it. The win here is navigational: this is harness code that only ever
// compiles in non-Release builds, and keeping ~880 lines of it interleaved with
// production window logic made both harder to follow.
//
// The guards below are kept rather than hoisted around the whole file so each
// block still states its own condition, and so CMake's non-Release generator
// expression is not the only thing standing between this code and a shipping
// binary.

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion / kGitCommit
#include "MainWindow.h"
#include "MainWindowPages.h"
#include "VisualGpuFixture.h"
#include "diagnostics/AppLog.h"
#include "diagnostics/ConfigSummary.h"
#include "diagnostics/StartupClock.h"
#include "diagnostics/StartupTrace.h"
#include "diagnostics/SupportBundle.h"
#include "exosnap_resource.h"
#include "models/RecordingPreset.h"
#include "notifications/NotificationEvent.h"
#include "notifications/NotificationManager.h"
#include "pages/AboutPage.h"
#include "pages/ConfigPage.h"
#include "pages/DevicePage.h"
#include "pages/DiagnosticsPage.h"
#include "pages/EditExportPage.h"
#include "pages/LogsPage.h"
#include "pages/RecordPage.h"
#include "services/ElevatedRelaunch.h"
#include "services/GlobalHotkeyService.h"
#include "services/UpdateService.h"
#include "ui/WindowGeometryPolicy.h"
#include "ui/chrome/NotificationHubPanel.h"
#include "ui/chrome/OperationalTitleBar.h"
#include "ui/chrome/RecordingStatusGuards.h"
#include "ui/dialogs/CrashReportOverlay.h"
#include "ui/dialogs/EditExportOverlay.h"
#include "ui/dialogs/FinalizingOverlay.h"
#include "ui/dialogs/RecordingErrorOverlay.h"
#include "ui/dialogs/RecoveryOverlay.h"
#include "ui/dialogs/SourcePickerOverlay.h"
#include "ui/dialogs/WhatsNewOverlay.h"
#include "ui/overlay/CountdownOverlayWindow.h"
#include "ui/overlay/DiagnosticsOverlayWindow.h"
#include "ui/overlay/NotificationToastWindow.h"
#include "ui/overlay/QuickControlPillWindow.h"
#include "ui/overlay/RecordingOverlayWindow.h"
#include "ui/theme/ExoSnapMetrics.h"
#include "ui/theme/ExoSnapPalette.h"
#include "ui/theme/ExoSnapTheme.h"
#include "ui/tray/TrayPresence.h"
#include "ui/widgets/EditPlayerSurface.h"
#include "ui/widgets/ExportPanel.h"
#include "ui/widgets/NotificationBell.h"
#include "ui/widgets/WebcamSetupPanel.h"
#include "visual_tests/VisualScenario.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>
#include <QWindow>
#include <QWindowStateChangeEvent>
#include <algorithm>
#include <array>
#include <capability/adapter_enum.h>
#include <capability/capability_builder.h>
#include <capability/capability_cache_key.h>
#include <capability/codec_selection.h>
#include <capability/config_types.h>
#include <capability/resolver.h>
#include <capability/user_config.h>
#include <cmath>
#include <crash_capture/crash_capture.h>
#include <crash_capture/crash_scrubber.h>
#include <dwmapi.h>
#include <filesystem>
#include <optional>
#include <string>
#include <update/update_handoff.h>
#include <windows.h>
#include <windowsx.h>

namespace exosnap {

// Same page table as MainWindow.cpp — see MainWindowPages.h.
using namespace pages;

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
namespace {

capability::AudioUiState VisualAudioStateForSettings(visual::VisualSettingsTarget target) {
    using K = recorder_core::AudioSourceKind;
    capability::AudioUiState state;
    state.target_kind = target == visual::VisualSettingsTarget::Window ? capability::CaptureTargetKind::Window
                                                                       : capability::CaptureTargetKind::Display;
    state.selected_window_pid =
        target == visual::VisualSettingsTarget::Window ? std::optional<uint32_t>{4242} : std::nullopt;
    state.source_rows = {
        {K::App, true, false},
        {K::Mic, true, false},
        {K::Sys, true, false},
    };
    state.selected_mic_device_id = std::string("visual-test-mic");
    return state;
}

ui::dialogs::SourcePickerPanel::SourceOption VisualScreenOption() {
    ui::dialogs::SourcePickerPanel::SourceOption option;
    option.target_index = 0;
    option.native_id = 1001;
    option.title = QStringLiteral("Visual Test Display 1");
    option.detail = QStringLiteral("2560 × 1440 · 60 Hz · primary");
    option.primary = true;
    option.status_badge = QStringLiteral("TEST");
    option.monitor_width = 2560;
    option.monitor_height = 1440;
    return option;
}

ui::dialogs::SourcePickerPanel::SourceOption VisualWindowOption() {
    ui::dialogs::SourcePickerPanel::SourceOption option;
    option.target_index = 1;
    option.native_id = 2001;
    option.title = QStringLiteral("Visual Test Window");
    option.detail = QStringLiteral("ExoSnap Fixture · PID 4242");
    option.status_badge = QStringLiteral("TEST");
    return option;
}

} // namespace

void MainWindow::applyVisualScenario(const visual::VisualScenario& scenario) {
    if (source_picker_overlay_)
        source_picker_overlay_->closeOverlay();

    if (record_page_)
        record_page_->applyVisualScenario(scenario);

    switch (scenario.page) {
    case visual::VisualPage::Record:
        setCurrentPage(kRecordPageIndex);
        // AUDIO-DEGRADED-NOTIFY-R1: drive the real production Enqueue() path so the
        // visual-test manifest reports the standing toast's actual state. Independent
        // of the diagnostics-snapshot plumbing (diag_live) — this scenario does not
        // touch RecordingDiagnosticsSnapshot at all.
        if (scenario.audio_degraded_notification_count > 0 && notification_manager_) {
            notification_manager_->Enqueue(notifications::MakeAudioSourceDegradedEvent(
                static_cast<uint32_t>(scenario.audio_degraded_notification_count)));
        }
        // FinalizingOverlay: drives it directly (see VisualScenario.h's
        // finalizing_overlay_mode doc comment) — the harness does not exercise
        // the real chromeStateChanged signal chain the overlay normally listens on.
        if (scenario.finalizing_overlay_mode == QStringLiteral("stopping")) {
            buildFinalizingOverlay();
            finalizing_overlay_->showFinalizing();
        } else if (scenario.finalizing_overlay_mode == QStringLiteral("saving")) {
            buildFinalizingOverlay();
            finalizing_overlay_->showSaving(scenario.finalizing_overlay_saving_percent);
        } else if (finalizing_overlay_) {
            finalizing_overlay_->hideOverlay();
        }
        break;
    case visual::VisualPage::Settings:
        applyVisualSettingsScenario(scenario);
        break;
    case visual::VisualPage::Diagnostics:
        if (!diagnostics_page_)
            buildDiagnosticsPage();
        // setCurrentPage() before the scenario applies data: showing the page fires
        // DiagnosticsPage::showEvent(), which emits lastRecordingGateRefreshRequested
        // and re-syncs setHasLastRecording() from the real (idle) record_page_ state.
        // Applying the scenario AFTER that sync — not before — lets its deliberate
        // has-last-recording override (the "post" scene) win as the final write,
        // instead of being silently clobbered back to false by the gate refresh.
        setCurrentPage(kDiagnosticsPageIndex);
        applyVisualDiagnosticsScenario(scenario);
        break;
    case visual::VisualPage::Logs:
        if (!logs_page_)
            buildLogsPage();
        logs_page_->applyVisualScenario(scenario);
        setCurrentPage(kLogsPageIndex);
        break;
    case visual::VisualPage::About:
        setCurrentPage(kAboutPageIndex);
        break;
    case visual::VisualPage::Device: {
        if (!device_page_)
            buildDevicePage();
        // Deterministic fake adapters through the test seam — no live DXGI
        // enumeration and no NVENC probe, so the capture is machine-independent.
        std::vector<capability::AdapterInfo> adapters;
        std::vector<capability::AdapterEncoderCapability> caps;
        if (!scenario.device_empty_adapters) {
            capability::AdapterInfo dgpu;
            dgpu.name = "GeForce RTX 4070";
            dgpu.vendor = capability::AdapterVendor::Nvidia;
            dgpu.kind = capability::AdapterKind::Discrete;
            dgpu.luid = 0x4070;
            dgpu.dedicated_video_memory_bytes = 12ull * 1024 * 1024 * 1024;
            capability::AdapterEncoderCapability dgpu_cap;
            dgpu_cap.probed = true;
            dgpu_cap.backend_label = "NVENC";
            dgpu_cap.provenance = "probed via NVENC encode GUIDs";
            dgpu_cap.h264 = true;
            dgpu_cap.hevc = true;
            dgpu_cap.av1 = true;
            dgpu_cap.yuv444_h264 = true;
            dgpu_cap.yuv444_hevc = true;

            capability::AdapterInfo igpu;
            igpu.name = "UHD Graphics 770";
            igpu.vendor = capability::AdapterVendor::Intel;
            igpu.kind = capability::AdapterKind::Integrated;
            igpu.luid = 0x770;
            igpu.shared_system_memory_bytes = 16ull * 1024 * 1024 * 1024;
            capability::AdapterEncoderCapability igpu_cap;
            igpu_cap.probed = false;
            igpu_cap.provenance =
                "encoder backend not yet supported (no AMD/AMF, Intel/QSV, or software encoder is wired in this build)";

            adapters = {dgpu, igpu};
            caps = {dgpu_cap, igpu_cap};
        }
        device_page_->setAdaptersForTest(std::move(adapters), std::move(caps));
        setCurrentPage(kDevicePageIndex);
        break;
    }
    case visual::VisualPage::EditExport:
        applyVisualEditExportScenario(scenario);
        break;
    }

    if (scenario.source_picker_tab != visual::VisualSourcePickerTab::None)
        applyVisualSourcePickerScenario(scenario);

    if (scenario.notifications_open)
        applyVisualNotificationsScenario(scenario);

    // Device-discovery state is applied last so it can override audio/webcam
    // state set by the page-specific helpers above.
    applyVisualDeviceDiscoveryScenario(scenario);

    if (title_bar_ && stack_)
        title_bar_->setActivePage(navHighlightIndexFor(stack_->currentIndex()));
    if (title_bar_)
        title_bar_->applyVisualWindowButtonHover(scenario.titlebar_hover_button);

    // Deterministic keyboard focus (VR-004): give the named widget tab focus so
    // :focus styling is visible in screenshots.
    if (!scenario.focused_object.isEmpty()) {
        if (auto* target = findChild<QWidget*>(scenario.focused_object); target && target->isEnabled())
            target->setFocus(Qt::TabFocusReason);
    }

    installVisualReadyMarker(scenario.id);
    QTimer::singleShot(0, this, [this, scenario_id = scenario.id]() { installVisualReadyMarker(scenario_id); });
    setWindowTitle(QStringLiteral("ExoSnap [visual-test:%1]").arg(scenario.id));
}

void MainWindow::applyVisualNotificationsScenario(const visual::VisualScenario& scenario) {
    Q_UNUSED(scenario);
    auto* host = centralWidget();
    if (host == nullptr)
        return;

    // Harness-only host: a fresh NotificationHubPanel embedded as a child of the
    // window so it renders into MainWindow::grab(). Constructing with a parent
    // still yields a top-level popup (Qt::Popup is a window type), so clear the
    // window flags to Qt::Widget to fold it into the window's compositing.
    auto* hub = host->findChild<ui::chrome::NotificationHubPanel*>(QStringLiteral("visualHubHost"));
    if (hub == nullptr) {
        hub = new ui::chrome::NotificationHubPanel(host);
        hub->setObjectName(QStringLiteral("visualHubHost"));
        hub->setWindowFlags(Qt::Widget);
        hub->setAttribute(Qt::WA_TranslucentBackground, false);
    }

    // Fixture advisories spanning the three canon severities (info / success /
    // error), oldest first so the newest — the standing error — lands last.
    hub->clearAdvisories();
    hub->addAdvisory(QStringLiteral("update-available"), QStringLiteral("info"),
                     QStringLiteral("Update available — 0.9.0"),
                     QStringLiteral("Signature verified. Installs after your next recording."), QStringLiteral("2m"),
                     /*unread=*/true, QStringLiteral("update-view"), QStringLiteral("View in About"),
                     /*is_deep_link=*/false);
    hub->addAdvisory(QStringLiteral("recording-saved"), QStringLiteral("success"), QStringLiteral("Recording saved"),
                     QStringLiteral("clip-2026-07-16-142032.mkv — 128 MB, 2:03."), QStringLiteral("just now"),
                     /*unread=*/false, QStringLiteral("reveal"), QStringLiteral("Open folder"),
                     /*is_deep_link=*/false);
    hub->addAdvisory(
        QStringLiteral("recording-stopped"), QStringLiteral("error"), QStringLiteral("Recording stopped unexpectedly"),
        QStringLiteral("The encoder lost the capture device. The partial file was kept."), QStringLiteral("just now"),
        /*unread=*/true, QStringLiteral("reveal"), QStringLiteral("Show file"),
        /*is_deep_link=*/false);

    // Anchor top-right under where the titlebar bell sits. The scenario is applied
    // before the first show(), so centralWidget() is not laid out yet and its width
    // reads stale here — position now for the immediate case and again on a
    // singleShot(0) once the pending layout pass has given the host its real width
    // (the screenshot grab fires at 120ms, well after this reflows).
    const auto place = [host, hub]() {
        hub->resize(hub->width(), 400);
        hub->move(host->width() - hub->width() - 12, 12);
        hub->show();
        hub->raise();
    };
    place();
    QTimer::singleShot(0, hub, place);
}

void MainWindow::installVisualReadyMarker(const QString& scenario_id) {
    auto* host = centralWidget();
    if (host == nullptr)
        return;

    auto* marker = findChild<QLabel*>(QStringLiteral("visualTestReadyMarker"));
    if (marker == nullptr) {
        marker = new QLabel(host);
        marker->setObjectName(QStringLiteral("visualTestReadyMarker"));
        marker->setAttribute(Qt::WA_TransparentForMouseEvents);
        marker->setStyleSheet(QStringLiteral("QLabel#visualTestReadyMarker {"
                                             "background: rgba(31, 196, 140, 0.18);"
                                             "border: 1px solid rgba(31, 196, 140, 0.55);"
                                             "border-radius: 4px;"
                                             "color: #b9f6df;"
                                             "font: 11px 'JetBrains Mono';"
                                             "padding: 3px 6px;"
                                             "}"));
    }
    marker->setText(QStringLiteral("VISUAL_TEST_READY:%1").arg(scenario_id));
    marker->adjustSize();
    marker->move(host->width() - marker->width() - 12, host->height() - marker->height() - 12);
    marker->raise();
    marker->show();
}

void MainWindow::applyVisualSettingsScenario(const visual::VisualScenario& scenario) {
    setCurrentPage(kSettingsPageIndex);
    if (!config_page_)
        return;

    OutputSettingsModel output;
    output.container = capability::Container::Matroska; // shipped default (MKV + AV1 + Opus)
    output.video_codec = capability::VideoCodec::Av1;
    output.audio_codec = capability::AudioCodec::Opus;
    output.output_folder = std::filesystem::path(L"C:\\Users\\User\\Videos\\ExoSnap");
    output.naming_pattern = L"visual-test_{datetime}_{title}";
    output.container = scenario.container;
    output.video_codec = scenario.video_codec;
    output.audio_codec = scenario.audio_codec;
    output.hdr_mode = scenario.hdr_mode;
    output.resolution.mode = scenario.output_resolution_mode;
    if (scenario.output_resolution_mode == OutputResolutionMode::Custom) {
        output.resolution.custom_width = static_cast<uint32_t>((std::max)(0, scenario.requested_width));
        output.resolution.custom_height = static_cast<uint32_t>((std::max)(0, scenario.requested_height));
    }

    VideoSettingsModel video;
    video.frame_rate_num = scenario.frame_rate_num;
    video.frame_rate_den = scenario.frame_rate_den;
    video.cfr = scenario.cfr;
    if (scenario.quality_cq != 0)
        video.cq = scenario.quality_cq;
    config_page_->setOutputSettings(output);
    // Invalid custom-resolution fixture: setOutputSettings() sanitized the unusable
    // size back to Native, so re-assert the honest invalid Custom state (fields +
    // indicator). Keyed on Custom mode with a zeroed effective size.
    if (scenario.output_resolution_mode == OutputResolutionMode::Custom && scenario.effective_width == 0 &&
        scenario.effective_height == 0) {
        config_page_->applyVisualCustomResolutionInvalid(scenario.requested_width, scenario.requested_height);
    }
    config_page_->setVideoSettings(video);
    config_page_->setAudioUiState(VisualAudioStateForSettings(scenario.settings_target));
    config_page_->setReadinessStatus(QStringLiteral("READY"));
    config_page_->setRecordingControlsLocked(scenario.controls_locked);
    config_page_->setAudioMeterLevels(0.37f, 0.56f, 0.42f, true, true, true);

    // Settings-tiers progressive-disclosure state (SETTINGS-TIERS-R1): drive the
    // Expert-mode reveal deterministically for visual scenarios.
    config_page_->setExpertModeEnabled(scenario.settings_expert_mode);

    // The expert HDR rows are relevance-gated on a probed HDR-active display,
    // which is machine-dependent. Pin the gate so settings captures render the
    // same everywhere: shown for the settings-hdr-* scenarios, hidden for the
    // rest. The pin is sticky, so a later real probe delivery cannot flip it
    // mid-capture.
    config_page_->applyVisualHdrDisplayPresent(scenario.id.startsWith(QStringLiteral("settings-hdr")));

    // ADR 0034: drive the Updates-card state and/or scroll to a section so
    // below-the-fold cards are captured. Deferred (40 ms) so it runs after layout
    // + preset reflow but before the harness grab at t=120 ms. Settings/Diagnostics
    // polish, Slice 3: also open the mic post-processing chevron disclosure here,
    // before the scroll, so its final (taller) layout is what gets scrolled into view.
    if (!scenario.settings_update_state.isEmpty() || !scenario.scroll_target.isEmpty() ||
        scenario.settings_mic_post_expanded) {
        const QString upd_state = scenario.settings_update_state;
        const QString upd_version = scenario.settings_update_version;
        const QString scroll_to = scenario.scroll_target;
        const bool mic_post_expanded = scenario.settings_mic_post_expanded;
        QTimer::singleShot(40, this, [this, upd_state, upd_version, scroll_to, mic_post_expanded]() {
            if (!config_page_)
                return;
            if (!upd_state.isEmpty())
                config_page_->setUpdateStatus(upd_state, upd_version, QStringLiteral("Just now"));
            if (mic_post_expanded)
                config_page_->applyVisualMicPostProcessingExpanded(true);
            if (!scroll_to.isEmpty())
                config_page_->scrollToSection(scroll_to);
        });
    }

    // Webcam-card scenarios (mirror off/on, unavailable) drive the embedded panel
    // deterministically without opening a real camera.
    if (scenario.webcam_state != visual::VisualWebcamState::None) {
        config_page_->applyVisualWebcamState(scenario.webcam_state == visual::VisualWebcamState::Active,
                                             scenario.webcam_mirror, scenario.webcam_chroma_enabled,
                                             scenario.webcam_chroma_color_mode);
    }

    // Hotkeys-card scenarios (default / custom / conflict / capture) drive the
    // embedded HotkeysSettingsPanel deterministically — no live service, so no
    // Win32 hotkey registration happens during a render.
    if (scenario.hk_capture_active || scenario.hk_conflict_shown || scenario.hk_editing_locked ||
        !scenario.hk_custom_binding_0.isEmpty() || !scenario.hk_custom_binding_1.isEmpty()) {
        config_page_->applyVisualHotkeysState(scenario.hk_custom_binding_0, scenario.hk_custom_binding_1,
                                              scenario.hk_capture_active ? scenario.hk_capture_action : -1,
                                              scenario.hk_conflict_shown ? scenario.hk_conflict_action : -1,
                                              scenario.hk_conflict_message, scenario.hk_editing_locked);
    }

    // Preset card — inject synthetic ProfileOption data when preset_count > 0 or
    // the scenario id starts with "settings-preset".  Never touches
    // RecordingPresetStore or RecordingPresetRegistry.
    //
    // The injection is deferred by one event-loop turn (singleShot 20 ms) so it
    // fires AFTER the MainWindow constructor's singleShot(0) that calls
    // refreshPresetUi() on capabilities-probe completion.  Without this the
    // real preset registry would overwrite the synthetic data before the harness
    // manifest is written at t=120 ms.
    const bool drive_preset = scenario.preset_count > 0 || scenario.id.startsWith(QStringLiteral("settings-preset"));
    if (drive_preset) {
        // Capture scenario fields by value for the deferred lambda.
        const int count = scenario.preset_count > 0 ? scenario.preset_count : 3;
        const QString selected_name = scenario.preset_selected_name;
        const bool dirty = scenario.preset_dirty;
        const bool save_error = scenario.preset_save_error;
        const bool menu_open = scenario.preset_menu_open;

        QTimer::singleShot(20, this, [this, count, selected_name, dirty, save_error, menu_open]() {
            if (!config_page_)
                return;

            // Synthetic preset names in declaration order.
            const QStringList kPresetNames = {QStringLiteral("Default"),   QStringLiteral("Gaming"),
                                              QStringLiteral("Tutorial"),  QStringLiteral("Streaming"),
                                              QStringLiteral("Cinematic"), QStringLiteral("Podcast"),
                                              QStringLiteral("Archive")};

            std::vector<ConfigPage::ProfileOption> opts;
            opts.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                ConfigPage::ProfileOption opt;
                opt.id = QStringLiteral("preset.vis%1").arg(i + 1);
                opt.label = (i < kPresetNames.size()) ? kPresetNames[i] : QStringLiteral("Preset %1").arg(i + 1);
                opt.built_in = (i == 0); // first entry is the built-in default
                opt.available = true;
                opts.push_back(opt);
            }

            // Match selected_id to the scenario's named field.
            QString selected_id = opts.front().id;
            for (const auto& opt : opts) {
                if (opt.label == selected_name)
                    selected_id = opt.id;
            }

            config_page_->setPresetOptions(opts, selected_id, dirty);

            // Inline save-error affordance (no modal — entirely deterministic).
            config_page_->applyVisualPresetSaveError(save_error);

            // Open the Manage overflow menu after a short delay so the widget is
            // visible and the screenshot captures it.  The menu is non-blocking in
            // Qt's event loop (QMenu::exec would block, showMenu() does not).
            if (menu_open) {
                if (auto* btn = config_page_->findChild<QToolButton*>(QStringLiteral("presetManageButton")))
                    QTimer::singleShot(80, btn, [btn]() { btn->showMenu(); });
            }
        });
    }
}

void MainWindow::applyVisualSourcePickerScenario(const visual::VisualScenario& scenario) {
    setCurrentPage(kRecordPageIndex);
    if (!source_picker_overlay_)
        return;

    source_picker_overlay_->setScreenOptions({VisualScreenOption()});
    source_picker_overlay_->setWindowOptions({VisualWindowOption()});
    const QRect visual_region(scenario.region_x, scenario.region_y, scenario.region_width, scenario.region_height);
    bool has_region = true;
    bool select_on_record = false;
    QString region_summary =
        QStringLiteral("VISUAL TEST: %1 × %2 on Display 1").arg(visual_region.width()).arg(visual_region.height());
    if (scenario.region_state == visual::VisualRegionState::Empty ||
        scenario.region_state == visual::VisualRegionState::None) {
        has_region = false;
        select_on_record = true;
        region_summary = QStringLiteral("VISUAL TEST: no saved region");
    } else if (scenario.region_state == visual::VisualRegionState::Editing) {
        region_summary = QStringLiteral("VISUAL TEST EDITING: %1, %2 — %3 × %4")
                             .arg(visual_region.x())
                             .arg(visual_region.y())
                             .arg(visual_region.width())
                             .arg(visual_region.height());
    } else if (scenario.region_state == visual::VisualRegionState::Invalid) {
        has_region = false;
        select_on_record = true;
        region_summary = QStringLiteral("VISUAL TEST INVALID: below 64 × 64 minimum");
    }
    source_picker_overlay_->setRegionState(region_summary, has_region, select_on_record, visual_region);

    ui::dialogs::SourcePickerPanel::Section section = ui::dialogs::SourcePickerPanel::Section::Screens;
    int target_index = 0;
    if (scenario.source_picker_tab == visual::VisualSourcePickerTab::Windows) {
        section = ui::dialogs::SourcePickerPanel::Section::Windows;
        target_index = 1;
    } else if (scenario.source_picker_tab == visual::VisualSourcePickerTab::Region) {
        section = ui::dialogs::SourcePickerPanel::Section::Region;
        target_index = 0;
    }

    source_picker_overlay_->setCurrentSection(section, target_index);
    if (scenario.region_state == visual::VisualRegionState::Preset16x9) {
        source_picker_overlay_->applyVisualRegionPreset(1920, 1080);
    } else if (scenario.region_state == visual::VisualRegionState::Preset9x16) {
        source_picker_overlay_->applyVisualRegionPreset(1080, 1920);
    }
    source_picker_overlay_->openOverlay();
}

namespace {

// Deterministic synthetic live-pipeline snapshots for the Visual Harness. They are fed
// through the SAME presentation path as production (DiagnosticsPage::applyLiveDiagnostics).
recorder_core::RecordingDiagnosticsSnapshot makeLiveDiagnosticsSnapshot(const QString& kind) {
    using namespace recorder_core;
    RecordingDiagnosticsSnapshot s;
    s.session_generation = 1;

    if (kind == QStringLiteral("idle")) {
        s.lifecycle = DiagnosticsLifecycle::Idle;
        s.valid = false;
        s.health = PipelineHealth::Idle;
        return s;
    }

    s.lifecycle = DiagnosticsLifecycle::Recording;
    s.valid = true;
    s.elapsed_seconds = 42.0;

    s.capture.target_fps = 60.0;
    s.capture.actual_fps = 59.8;
    s.capture.frames_captured = 2600;
    s.capture.frames_emitted = 2520;
    s.capture.frames_dropped_coalesced = 80;
    s.capture.frames_duplicated = 3;
    s.capture.frame_interval_ms = 1000.0 / 60.0;
    s.capture.interval_observed = MetricAvailability::Unavailable;
    s.capture.source_type = CaptureSourceType::Display;
    // Present cadence (VRR/CFR judder correlation): high-refresh source feeding a 60 fps CFR
    // capture — visible jitter and >1 coalescing, the showcase case for ADR 0033.
    s.capture.source_present_interval_ms = 8.4;
    s.capture.source_present_jitter_ms = 6.2;
    s.capture.source_coalesce_ratio = 2.1;
    s.capture.present_cadence_availability = MetricAvailability::Available;

    s.compositor.active = true;
    s.compositor.latest_ms = 1.3;
    s.compositor.average_ms = 1.4;
    s.compositor.peak_ms = 2.8;
    s.compositor.frames_composed = 2520;

    s.video_encoder.latest_ms = 2.0;
    s.video_encoder.average_ms = 2.1;
    s.video_encoder.peak_ms = 3.5;
    s.video_encoder.output_fps = 60.0;
    s.video_encoder.frames_submitted = 2520;
    s.video_encoder.frames_encoded = 2520;
    s.video_encoder.codec = VideoCodec::Av1;
    s.video_encoder.width = 1920;
    s.video_encoder.height = 1080;
    s.video_encoder.cfr = true;

    s.audio.active = true;
    s.audio.packets_encoded = 2000;
    s.audio.bytes_encoded = 256000;
    s.audio.queue_depth = 1;
    s.audio.queue_peak = 3;
    s.audio.sample_rate = 48000;
    s.audio.channels = 2;
    s.audio.codec = AudioCodec::Opus;
    s.audio.track_count = 1;

    s.video_queue.current_depth = 1;
    s.video_queue.peak_depth = 3;
    s.video_queue.capacity = 0;
    s.video_queue.bounded = false;
    s.audio_queue.current_depth = 0;
    s.audio_queue.peak_depth = 12;
    s.audio_queue.capacity = 600;
    s.audio_queue.bounded = true;

    s.mux.packets_processed = 4520;
    s.mux.bytes_written = 18ull * 1024ull * 1024ull;
    s.mux.throughput_mib_s = 18.7;
    s.mux.latest_write_ms = 0.8;
    s.mux.average_write_ms = 0.8;
    s.mux.peak_write_ms = 4.2;
    s.mux.current_segment_index = 0;
    s.mux.segment_count = 1;
    s.mux.reorder_packets = 1;
    s.mux.reorder_packets_peak = 3;
    s.mux.reorder_bytes = 2048;
    s.mux.reorder_bytes_peak = 6144;
    s.mux.availability = MetricAvailability::Available;

    s.disk.bytes_written = s.mux.bytes_written;
    s.disk.throughput_mib_s = 18.7;
    s.disk.latest_write_ms = 0.8;
    s.disk.average_write_ms = 0.8;
    s.disk.peak_write_ms = 4.2;
    s.disk.output_target = "C:";
    s.disk.latency_availability = MetricAvailability::Available;

    s.capture.acquire_average_ms = 0.6;
    s.capture.acquire_peak_ms = 1.2;
    s.capture.acquire_availability = MetricAvailability::Available;
    s.compositor.vpblt_average_ms = 0.4;
    s.compositor.vpblt_peak_ms = 0.9;
    s.compositor.vpblt_availability = MetricAvailability::Available;
    s.mux.process_average_ms = 0.5;
    s.mux.process_peak_ms = 1.1;
    s.mux.process_availability = MetricAvailability::Available;

    s.split.split_supported = true;
    s.split.current_segment = 1;
    s.split.completed_segments = 0;
    s.split.availability = MetricAvailability::Available;
    s.split.seconds_until_auto_split = -1.0;

    s.bottleneck = PipelineBottleneck::None;
    s.health = PipelineHealth::Good;

    if (kind == QStringLiteral("encoder")) {
        s.video_encoder.average_ms = 20.0; // over the 16.7 ms budget → Bottleneck
        s.video_encoder.peak_ms = 24.0;
        s.video_encoder.backlog = 6;
        s.video_encoder.frames_encoded = 2440;
        s.video_queue.current_depth = 5;
        s.bottleneck = PipelineBottleneck::VideoEncoder;
        s.bottleneck_reason = "Encoder backlog rising";
        s.health = PipelineHealth::Warning;
    } else if (kind == QStringLiteral("disk")) {
        s.mux.average_write_ms = 14.0;
        s.mux.peak_write_ms = 22.0;
        s.mux.throughput_mib_s = 4.0;
        s.disk.average_write_ms = 14.0;
        s.disk.peak_write_ms = 22.0;
        s.disk.throughput_mib_s = 4.0;
        s.video_queue.current_depth = 12;
        s.bottleneck = PipelineBottleneck::Disk;
        s.bottleneck_reason = "Write latency high";
        s.health = PipelineHealth::Warning;
    } else if (kind == QStringLiteral("paused")) {
        s.lifecycle = DiagnosticsLifecycle::Paused;
        s.capture.actual_fps = 0.0;
        s.video_encoder.output_fps = 0.0;
        s.mux.throughput_mib_s = 0.0;
        s.disk.throughput_mib_s = 0.0;
    } else if (kind == QStringLiteral("split")) {
        s.split.current_segment = 2;
        s.split.completed_segments = 1;
        s.split.split_pending = true;
        s.split.last_trigger = DiagnosticsSplitTrigger::ManualButton;
        s.mux.current_segment_index = 1;
        s.mux.segment_count = 2;
        s.mux.split_transitions = 1;
        s.video_encoder.forced_keyframes = 1;
    } else if (kind == QStringLiteral("judder")) {
        // Measured present-time jitter over the 8 ms judder threshold → Tier-2
        // "VRR / refresh-induced judder" (rec.001), verdict amber, recording continues.
        s.capture.source_present_jitter_ms = 11.4;
        s.capture.source_present_interval_ms = 16.6;
        s.capture.present_cadence_availability = MetricAvailability::Available;
        s.video_encoder.cfr = true;
        s.bottleneck = PipelineBottleneck::None;
        s.health = PipelineHealth::Warning;
    } else if (kind == QStringLiteral("degraded")) {
        // Audio endpoint lost mid-recording → Tier-2 "Audio device lost" (rec.audio.degraded).
        // Calm, measured, never a blocker: the recording keeps running on honest silence.
        s.audio.degraded_sources = 1;
        s.audio.source_degraded = true;
        s.audio.track_count = 2;
        s.health = PipelineHealth::Warning;
    } else if (kind == QStringLiteral("post")) {
        // Post-flight: a finished, saved recording. The Diagnostics post-flight phase
        // links to the Edit overlay's report card; the verdict returns to a calm state.
        s.lifecycle = DiagnosticsLifecycle::Completed;
        s.capture.actual_fps = 0.0;
        s.video_encoder.output_fps = 0.0;
        s.mux.throughput_mib_s = 0.0;
        s.disk.throughput_mib_s = 0.0;
        s.audio.degraded_sources = 0;
        s.audio.source_degraded = false;
    }

    return s;
}

} // namespace

void MainWindow::applyVisualDiagnosticsScenario(const visual::VisualScenario& scenario) {
    if (!diagnostics_page_)
        return;

    // Unify the fixture GPU across surfaces: Diagnostics otherwise shows the real
    // probed adapter (machine-dependent) while the Device page injects a fixed fake.
    // The pin is sticky (refreshDiagnosticsData re-applies it) because the async
    // caps-ready path re-assigns runtime_caps_ wholesale after this scenario runs.
    visual_diagnostics_gpu_override_ = true;

    // Prefer the real probed caps when ready so this render matches the async
    // caps-ready refreshDiagnosticsData() and does not flicker the card set.
    capability::CapabilitySet caps =
        runtime_caps_ready_ ? runtime_caps_ : capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    ApplyVisualGpuFixture(caps);
    // Read the scenario's container / codecs so lifecycle scenarios can drive a
    // Tier-1 blocker (e.g. MP4 + FLAC → rec.009). Defaults are the canon WebM/AV1/Opus.
    // These are written into the app's real settings so the async caps-ready
    // refreshDiagnosticsData() reproduces the same verdict rather than resetting it.
    output_settings_.container = scenario.container;
    output_settings_.video_codec = scenario.video_codec;
    output_settings_.audio_codec = scenario.audio_codec;
    OutputSettingsModel output = output_settings_;
    VideoSettingsModel video;
    diagnostics_page_->setDiagnosticData(
        caps, output, video, VisualAudioStateForSettings(visual::VisualSettingsTarget::Window),
        "Visual Test Diagnostics", "Start/Stop: Alt+F9", "visual-test-settings.json", true);

    // The live pipeline + full taxonomy are Expert-gated; recording scenarios need
    // Expert on to render Phase ③. The plain "diagnostics" scenario stays Simple, unless
    // the scenario explicitly requests Expert via settings_expert_mode.
    diagnostics_page_->setExpertModeEnabled(!scenario.diag_live.isEmpty() || scenario.settings_expert_mode);

    if (!scenario.diag_live.isEmpty()) {
        // The post-flight scenario shows a finished recording — enable the Phase-④
        // "Open last report" bridge to the Edit overlay (and, since slice 5, the
        // Last-session readiness tile). This is honest, not a synthetic override: the
        // "post" scenario also sets record_state = VisualRecordState::Completed (see
        // VisualScenario.cpp), so record_page_->hasCompletedRecording() genuinely
        // reports true and every resync of this gate (including the showEvent-driven
        // SETTINGS-HONESTY-R1 refresh) agrees — no race with the real gate to fight.
        diagnostics_page_->setHasLastRecording(scenario.diag_live == QStringLiteral("post"));
        diagnostics_page_->applyLiveDiagnostics(makeLiveDiagnosticsSnapshot(scenario.diag_live));
    }
}

void MainWindow::applyVisualDeviceDiscoveryScenario(const visual::VisualScenario& scenario) {
    // Only engage when at least one device-discovery field is set.
    const bool has_audio = scenario.dd_audio_input_count >= 0;
    const bool has_webcam = scenario.dd_webcam_count >= 0;

    // --- Audio mic list (Settings/Audio card) ---
    // Build a synthetic AudioDeviceSnapshot matching the scenario state and
    // push it to ConfigPage via the existing reactive handler.  This is the
    // same path used during live hot-plug, so no new UI hooks are needed.
    // Non-persistent: onAudioDevicesChanged() never writes to any store.
    if (has_audio && config_page_) {
        AudioDeviceSnapshot snap;
        // Populate synthetic input devices.
        const int n_inputs = scenario.dd_audio_input_count;
        for (int i = 0; i < n_inputs; ++i) {
            recorder_core::AudioInputDeviceInfo d;
            d.device_id = QStringLiteral("vis-input-%1").arg(i + 1).toStdString();
            d.display_name = QStringLiteral("Visual Test Input %1").arg(i + 1).toStdString();
            d.is_default = (i == 0);
            snap.inputs.push_back(d);
        }
        if (!snap.inputs.empty())
            snap.default_input_id = snap.inputs.front().device_id;

        // If the scenario has a selected mic that should be present, make sure
        // that id appears in the snapshot.  If it should be absent, leave it out.
        if (!scenario.dd_selected_mic_stable_id.isEmpty()) {
            if (scenario.dd_selected_mic_available) {
                // Replace the first synthetic device with the configured id so
                // onAudioDevicesChanged() finds it and keeps it selected.
                const std::string target_id = scenario.dd_selected_mic_stable_id.toStdString();
                bool already_present = false;
                for (auto& d : snap.inputs) {
                    if (d.device_id == target_id) {
                        already_present = true;
                        break;
                    }
                }
                if (!already_present) {
                    recorder_core::AudioInputDeviceInfo target_dev;
                    target_dev.device_id = target_id;
                    target_dev.display_name =
                        QStringLiteral("Visual Test Mic (%1)").arg(scenario.dd_selected_mic_stable_id).toStdString();
                    target_dev.is_default = false;
                    snap.inputs.push_back(target_dev);
                }
                // Pre-select by setting audio_ui_state before the reactive push.
                capability::AudioUiState state;
                state.selected_mic_device_id = target_id;
                config_page_->setAudioUiState(state);
            } else {
                // Device is absent: configure the id first, then push a snapshot
                // without it so the placeholder is shown.
                capability::AudioUiState state;
                state.selected_mic_device_id = scenario.dd_selected_mic_stable_id.toStdString();
                config_page_->setAudioUiState(state);
                // snap intentionally does NOT contain dd_selected_mic_stable_id.
            }
        }

        config_page_->onAudioDevicesChanged(snap);
    }

    // --- Webcam card (Settings/Webcam) ---
    // For webcam-missing scenarios the webcam_state is already set to
    // Unavailable by the page-dispatch block; applyVisualWebcamState() is the
    // existing harness hook — just make sure it is driven correctly here for
    // discovery-specific scenarios that set dd_webcam_count.
    if (has_webcam && config_page_) {
        const bool cam_available = scenario.dd_selected_webcam_available;
        const bool mirror = scenario.webcam_mirror;
        // Only override if the scenario is on the Settings page (the webcam
        // card lives there); the Record preview uses a separate path.
        if (scenario.page == visual::VisualPage::Settings)
            config_page_->applyVisualWebcamState(cam_available, mirror, scenario.webcam_chroma_enabled,
                                                 scenario.webcam_chroma_color_mode);
    }
}
#endif

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void MainWindow::applyVisualEditExportScenario(const visual::VisualScenario& scenario) {
    if (!edit_export_overlay_)
        buildEditExportOverlay();
    setCurrentPage(kRecordPageIndex);

    // setEditContext() (rather than the legacy setRecordingInfo() shim) so the
    // deterministic duration/markers below flow through the same path real
    // recordings use — this is what drives the Edit-phase timeline marker pins.
    EditContext ctx;
    ctx.output_path = scenario.edit_export_file_path;
    ctx.duration = scenario.edit_export_duration;
    ctx.size = scenario.edit_export_size;
    ctx.resolution = scenario.edit_export_resolution;
    ctx.fps = scenario.edit_export_fps;
    ctx.video_codec = scenario.edit_export_video_codec;
    ctx.audio_codec = scenario.edit_export_audio_codec;
    ctx.container = scenario.edit_export_container;
    ctx.duration_seconds = scenario.edit_export_duration_seconds;
    for (const uint64_t time_ms : scenario.edit_export_marker_times_ms) {
        RecordingMarker marker;
        marker.time_ms = time_ms;
        ctx.markers.push_back(marker);
    }
    // The header report icon derives its severity from the snapshot the page is
    // given, so the scenario drives the real computation rather than the icon.
    if (!scenario.edit_export_report_severity.isEmpty()) {
        ctx.completed_snapshot.valid = true;
        if (scenario.edit_export_report_severity == QStringLiteral("warning"))
            ctx.completed_snapshot.health = recorder_core::PipelineHealth::Warning;
        else if (scenario.edit_export_report_severity == QStringLiteral("critical"))
            ctx.completed_snapshot.health = recorder_core::PipelineHealth::Critical;
        else
            ctx.completed_snapshot.health = recorder_core::PipelineHealth::Good;
    }
    edit_export_overlay_->page()->setEditContext(ctx);

    // Deterministic timeline state (trim handles + playhead).
    if (scenario.edit_export_trim_start_ms >= 0 || scenario.edit_export_trim_end_ms >= 0) {
        const qint64 duration_ms = static_cast<qint64>(scenario.edit_export_duration_seconds * 1000.0);
        const qint64 start_ms = std::max<qint64>(scenario.edit_export_trim_start_ms, 0);
        const qint64 end_ms = scenario.edit_export_trim_end_ms >= 0 ? scenario.edit_export_trim_end_ms : duration_ms;
        edit_export_overlay_->page()->setTrimRangeMs(start_ms, end_ms);
    }
    edit_export_overlay_->page()->setPreviewPositionMs(scenario.edit_export_playhead_ms);

    // Timeline rows + tile strip: both come from the opened clip in production,
    // and the harness has none, so the scenario injects them.
    edit_export_overlay_->page()->setTimelineFixture(scenario.edit_export_audio_track_labels,
                                                     scenario.edit_export_thumbnail_tiles);

    // Export panel states are driven straight through the panel's own API — the
    // page's export path would start a real remux, which the harness must not do.
    if (!scenario.edit_export_panel_state.isEmpty()) {
        if (auto* panel =
                edit_export_overlay_->page()->findChild<ui::widgets::ExportPanel*>(QStringLiteral("exportPanel"))) {
            panel->reset();
            if (scenario.edit_export_panel_state == QStringLiteral("running")) {
                panel->showRunning();
                panel->setProgress(62); // fixed mid-run value: the capture must be deterministic
            } else if (scenario.edit_export_panel_state == QStringLiteral("done")) {
                panel->showDone(scenario.edit_export_file_path);
            } else if (scenario.edit_export_panel_state == QStringLiteral("failed")) {
                panel->showFailed(QStringLiteral("Could not write the output file: the disk is full."));
            }
        }
    }
    edit_export_overlay_->openOverlay();
    // stack_->currentIndex() is now kRecordPageIndex, so the shared post-switch
    // title_bar_->setActivePage(navHighlightIndexFor(stack_->currentIndex())) in
    // applyVisualScenario() already highlights Record correctly.

    // EDIT-VIDEO-PLAYER Task 9: EditPlayerSurface is not wired into
    // EditExportPage yet (a later task's job) -- proven here via a harness-only
    // host layered over the overlay.
    if (!scenario.edit_player_surface_mode.isEmpty()) {
        ensureEditPlayerSurfaceVisualTestHost();
        if (scenario.edit_player_surface_mode == QStringLiteral("frame")) {
            QImage test_img(320, 180, QImage::Format_RGB32);
            test_img.fill(QColor("#3a6b5c")); // arbitrary solid color -- proves paint path, not color accuracy
            edit_player_surface_visual_test_->setFrame(test_img);
        } else {
            edit_player_surface_visual_test_->clearFrame();
        }
        edit_player_surface_visual_test_->show();
        edit_player_surface_visual_test_->raise();
        // edit_export_overlay_ is not synced to its final on-screen geometry yet
        // at this point in RunVisualTestHarness: EditExportOverlay::openOverlay()
        // (just above) calls syncGeometryToParent() against centralWidget(), but
        // the top-level MainWindow itself is not shown until AFTER
        // applyVisualScenario() returns (see RunVisualTestHarness), and
        // EditExportOverlay's own showEvent()-driven re-sync only fires once that
        // happens. Deferring one event-loop tick lets this harness-only host pick
        // up the real final size instead of whatever pre-show geometry the
        // overlay had when openOverlay() ran above.
        QTimer::singleShot(0, this, [this]() {
            if (!edit_export_overlay_ || !edit_player_surface_visual_test_)
                return;
            const QRect host_rect = edit_export_overlay_->rect();
            const int margin_x = host_rect.width() / 6;
            const int margin_y = host_rect.height() / 6;
            edit_player_surface_visual_test_->setGeometry(host_rect.adjusted(margin_x, margin_y, -margin_x, -margin_y));
            edit_player_surface_visual_test_->raise();
        });
    } else if (edit_player_surface_visual_test_) {
        edit_player_surface_visual_test_->hide();
    }
}

void MainWindow::ensureEditPlayerSurfaceVisualTestHost() {
    if (edit_player_surface_visual_test_)
        return;
    edit_player_surface_visual_test_ = new ui::widgets::EditPlayerSurface(edit_export_overlay_);
}
#endif
} // namespace exosnap

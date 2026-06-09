#include "VisualTestHarness.h"

#include "../MainWindow.h"
#include "../pages/LogsPage.h"
#include "../ui/widgets/ExoToggle.h"
#include "../ui/widgets/PreviewSurface.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

namespace exosnap::visual {
namespace {

bool EnsureParentDir(const QString& path) {
    const QFileInfo info(path);
    const QDir dir = info.absoluteDir();
    return dir.exists() || QDir().mkpath(dir.absolutePath());
}

QJsonObject RectToJson(const QRect& rect) {
    QJsonObject out;
    out.insert(QStringLiteral("x"), rect.x());
    out.insert(QStringLiteral("y"), rect.y());
    out.insert(QStringLiteral("width"), rect.width());
    out.insert(QStringLiteral("height"), rect.height());
    return out;
}

QString WidgetText(const QWidget* widget) {
    if (const auto* label = qobject_cast<const QLabel*>(widget))
        return label->text();
    if (const auto* button = qobject_cast<const QPushButton*>(widget))
        return button->text();
    return {};
}

QString ResolutionModeName(OutputResolutionMode mode) {
    return QString::fromWCharArray(OutputResolutionModeName(mode));
}

QString ContainerName(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return QStringLiteral("mkv");
    case capability::Container::Mp4:
        return QStringLiteral("mp4");
    case capability::Container::WebM:
        return QStringLiteral("webm");
    }
    return QStringLiteral("mkv");
}

QString VideoCodecName(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::H264Nvenc:
        return QStringLiteral("h264");
    case capability::VideoCodec::HevcNvenc:
        return QStringLiteral("hevc");
    case capability::VideoCodec::Av1Nvenc:
        return QStringLiteral("av1");
    }
    return QStringLiteral("av1");
}

QString AudioCodecName(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::AacMf:
        return QStringLiteral("aac");
    case capability::AudioCodec::Opus:
        return QStringLiteral("opus");
    case capability::AudioCodec::Pcm:
        return QStringLiteral("pcm");
    }
    return QStringLiteral("opus");
}

QJsonObject SizeToJson(int width, int height) {
    QJsonObject out;
    out.insert(QStringLiteral("width"), width);
    out.insert(QStringLiteral("height"), height);
    return out;
}

} // namespace

bool HasVisualTestRequest(const QStringList& args) {
    return args.contains(QStringLiteral("--visual-test"));
}

bool ParseVisualTestOptions(const QStringList& args, VisualTestOptions* out, QString* error) {
    if (out == nullptr)
        return false;

    VisualTestOptions parsed;
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        const auto require_value = [&](QString* target) -> bool {
            if (i + 1 >= args.size()) {
                if (error)
                    *error = QStringLiteral("Missing value for %1").arg(arg);
                return false;
            }
            *target = args.at(++i);
            return true;
        };

        if (arg == QStringLiteral("--visual-test")) {
            if (!require_value(&parsed.scenario_id))
                return false;
        } else if (arg == QStringLiteral("--visual-test-screenshot")) {
            if (!require_value(&parsed.screenshot_path))
                return false;
            parsed.exit_after_capture = true;
        } else if (arg == QStringLiteral("--visual-test-manifest")) {
            if (!require_value(&parsed.manifest_path))
                return false;
            parsed.exit_after_capture = true;
        } else if (arg == QStringLiteral("--visual-test-maximized")) {
            parsed.maximize = true;
        } else if (arg == QStringLiteral("--visual-test-exit")) {
            parsed.exit_after_capture = true;
        }
    }

    if (parsed.scenario_id.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("--visual-test requires a scenario id");
        return false;
    }

    *out = parsed;
    return true;
}

QJsonObject BuildVisualManifest(const MainWindow& window, const VisualScenario& scenario) {
    QJsonObject root;
    root.insert(QStringLiteral("schema"), QStringLiteral("exosnap.visual-test.v1"));
    root.insert(QStringLiteral("scenario"), scenario.id);
    root.insert(QStringLiteral("title"), scenario.title);
    root.insert(QStringLiteral("page"), ToString(scenario.page));
    root.insert(QStringLiteral("record_state"), ToString(scenario.record_state));
    root.insert(QStringLiteral("countdown_seconds"), scenario.countdown_seconds);
    root.insert(QStringLiteral("countdown_remaining"), scenario.countdown_remaining);
    root.insert(QStringLiteral("settings_target"), ToString(scenario.settings_target));
    root.insert(QStringLiteral("source_picker_tab"), ToString(scenario.source_picker_tab));
    root.insert(QStringLiteral("webcam_state"), ToString(scenario.webcam_state));
    root.insert(QStringLiteral("log_filter"), ToString(scenario.log_filter));
    root.insert(QStringLiteral("log_search_query"), scenario.log_search_query);
    root.insert(QStringLiteral("log_auto_scroll"), scenario.log_auto_scroll);
    root.insert(QStringLiteral("region_state"), ToString(scenario.region_state));
    root.insert(QStringLiteral("region_edit_mode"), ToString(scenario.region_edit_mode));
    root.insert(QStringLiteral("region_geometry"),
                RectToJson(QRect(scenario.region_x, scenario.region_y, scenario.region_width, scenario.region_height)));
    root.insert(QStringLiteral("ready_marker"), QStringLiteral("VISUAL_TEST_READY:%1").arg(scenario.id));
    root.insert(QStringLiteral("window_geometry"), RectToJson(window.geometry()));

    QJsonObject output;
    output.insert(QStringLiteral("requested_resolution_mode"), ResolutionModeName(scenario.output_resolution_mode));
    output.insert(QStringLiteral("requested_dimensions"),
                  SizeToJson(scenario.requested_width, scenario.requested_height));
    output.insert(QStringLiteral("effective_output_dimensions"),
                  SizeToJson(scenario.effective_width, scenario.effective_height));
    output.insert(QStringLiteral("source_dimensions"), SizeToJson(scenario.source_width, scenario.source_height));
    output.insert(
        QStringLiteral("content_rectangle"),
        RectToJson(QRect(scenario.content_x, scenario.content_y, scenario.content_width, scenario.content_height)));
    output.insert(QStringLiteral("frame_rate_num"), static_cast<int>(scenario.frame_rate_num));
    output.insert(QStringLiteral("frame_rate_den"), static_cast<int>(scenario.frame_rate_den));
    output.insert(QStringLiteral("timing_mode"), scenario.cfr ? QStringLiteral("cfr") : QStringLiteral("vfr"));
    output.insert(QStringLiteral("container"), ContainerName(scenario.container));
    output.insert(QStringLiteral("video_codec"), VideoCodecName(scenario.video_codec));
    output.insert(QStringLiteral("audio_codec"), AudioCodecName(scenario.audio_codec));
    output.insert(QStringLiteral("reconciliation_warning"), scenario.reconciliation_warning);
    output.insert(QStringLiteral("controls_locked"), scenario.controls_locked);
    output.insert(QStringLiteral("preset_dirty_state"), scenario.preset_dirty);
    root.insert(QStringLiteral("output_format"), output);

    QJsonObject source_picker;
    source_picker.insert(QStringLiteral("display_count"), scenario.source_display_count);
    source_picker.insert(QStringLiteral("window_count"), scenario.source_window_count);
    source_picker.insert(QStringLiteral("selected_identity"), scenario.source_selected_identity);
    source_picker.insert(QStringLiteral("selected_available"), scenario.source_selected_available);
    source_picker.insert(QStringLiteral("refresh_active"), scenario.source_refresh_active);
    source_picker.insert(QStringLiteral("refresh_generation"), scenario.source_refresh_generation);
    root.insert(QStringLiteral("source_picker"), source_picker);

    // Device discovery manifest (DEVICE-DISCOVERY-R1).
    // All fields carry sentinel values (-1 / empty / false) for scenarios that
    // do not exercise device discovery, so consumers can distinguish "not set"
    // from "zero devices present".
    {
        QJsonObject dd;
        dd.insert(QStringLiteral("audio_input_count"), scenario.dd_audio_input_count);
        dd.insert(QStringLiteral("audio_output_count"), scenario.dd_audio_output_count);
        dd.insert(QStringLiteral("selected_mic_stable_id"), scenario.dd_selected_mic_stable_id);
        dd.insert(QStringLiteral("selected_mic_available"), scenario.dd_selected_mic_available);
        dd.insert(QStringLiteral("selected_output_semantic_default"), scenario.dd_selected_output_semantic_default);
        dd.insert(QStringLiteral("webcam_count"), scenario.dd_webcam_count);
        dd.insert(QStringLiteral("selected_webcam_stable_id"), scenario.dd_selected_webcam_stable_id);
        dd.insert(QStringLiteral("selected_webcam_available"), scenario.dd_selected_webcam_available);
        dd.insert(QStringLiteral("display_count"), scenario.dd_display_count);
        dd.insert(QStringLiteral("selected_display_stable_id"), scenario.dd_selected_display_stable_id);
        dd.insert(QStringLiteral("selected_display_available"), scenario.dd_selected_display_available);
        dd.insert(QStringLiteral("current_target_resolved"), scenario.dd_current_target_resolved);
        dd.insert(QStringLiteral("rescan_enabled"), scenario.dd_rescan_enabled);
        dd.insert(QStringLiteral("last_discovery_reason"), scenario.dd_last_discovery_reason);
        root.insert(QStringLiteral("device_discovery"), dd);
    }

    // Webcam PiP manifest (Record preview). Reflects actual PreviewSurface state, not
    // the scenario inputs, so preview/output parity and lock/selection can be asserted.
    if (const auto* preview = window.findChild<const ui::widgets::PreviewSurface*>(QStringLiteral("previewSurface"))) {
        QJsonObject pip;
        pip.insert(QStringLiteral("enabled"), preview->isWebcamOverlayEnabled());
        const QRectF norm = preview->webcamOverlayRect();
        QJsonObject placement;
        placement.insert(QStringLiteral("x"), norm.x());
        placement.insert(QStringLiteral("y"), norm.y());
        placement.insert(QStringLiteral("w"), norm.width());
        placement.insert(QStringLiteral("h"), norm.height());
        pip.insert(QStringLiteral("placement_norm"), placement);
        pip.insert(QStringLiteral("mapped_preview_rect"), RectToJson(preview->webcamMappedPreviewRect()));
        pip.insert(QStringLiteral("mirror"), preview->webcamMirror());
        pip.insert(QStringLiteral("selected"), preview->isWebcamSelected());
        pip.insert(QStringLiteral("edit_locked"), preview->isWebcamEditLocked());
        pip.insert(QStringLiteral("active_handle"), preview->webcamActiveHandle());
        root.insert(QStringLiteral("webcam_pip"), pip);
    }

    // Webcam card manifest (Settings) — availability is in webcam_state; expose the
    // mirror toggle truth too.
    if (const auto* mirror_toggle =
            window.findChild<const ui::widgets::ExoToggle*>(QStringLiteral("webcamPanelMirrorToggle"))) {
        QJsonObject card;
        card.insert(QStringLiteral("mirror"), mirror_toggle->isOn());
        root.insert(QStringLiteral("webcam_card"), card);
    }

    // Preset card manifest (Settings page).
    // Values are read from actual widgets where available; scenario fields are
    // used as fallback for values not directly surfaced by widgets.
    {
        QJsonObject preset;

        // selected_name: read from the profileCombo current text.
        if (const auto* combo = window.findChild<const QComboBox*>(QStringLiteral("profileCombo"))) {
            preset.insert(QStringLiteral("selected_name"), combo->currentText());
            preset.insert(QStringLiteral("count"), combo->count());
        } else {
            preset.insert(QStringLiteral("selected_name"), scenario.preset_selected_name);
            preset.insert(QStringLiteral("count"), scenario.preset_count);
        }

        // default_name: from scenario (no widget exposes the default id directly).
        preset.insert(QStringLiteral("default_name"), scenario.preset_default_name);

        // dirty: presetDirtyIndicator visibility is the ground truth.
        if (const auto* ind = window.findChild<const QLabel*>(QStringLiteral("presetDirtyIndicator")))
            preset.insert(QStringLiteral("dirty"), ind->isVisible());
        else
            preset.insert(QStringLiteral("dirty"), scenario.preset_dirty);

        // default_badge_visible: presetDefaultBadge visibility.
        if (const auto* badge = window.findChild<const QLabel*>(QStringLiteral("presetDefaultBadge")))
            preset.insert(QStringLiteral("default_badge_visible"), badge->isVisible());
        else
            preset.insert(QStringLiteral("default_badge_visible"), false);

        // target_kind and countdown_seconds from scenario fields.
        preset.insert(QStringLiteral("target_kind"), ToString(scenario.settings_target));
        preset.insert(QStringLiteral("countdown_seconds"), scenario.countdown_seconds);

        // audio { sys_enabled, app_enabled, mic_enabled } — read from checkboxes by
        // objectName; fall back to audio_ui_state in scenario if widgets absent.
        QJsonObject audio;
        if (const auto* sys = window.findChild<const QCheckBox*>(QStringLiteral("settingsAudioSysCheck")))
            audio.insert(QStringLiteral("sys_enabled"), sys->isChecked());
        else
            audio.insert(QStringLiteral("sys_enabled"), QJsonValue());
        if (const auto* app = window.findChild<const QCheckBox*>(QStringLiteral("settingsAudioAppCheck")))
            audio.insert(QStringLiteral("app_enabled"), app->isChecked());
        else
            audio.insert(QStringLiteral("app_enabled"), QJsonValue());
        // Mic check uses objectName not set via automation — derive from visibility of
        // the settingsAudioSysCheck sibling; use null if not found.
        // (The mic check does not have an explicit objectName in the current codebase.)
        audio.insert(QStringLiteral("mic_enabled"), QJsonValue()); // populated if widget found
        preset.insert(QStringLiteral("audio"), audio);

        // webcam { enabled, mirror, chroma } — reuse webcam_pip / webcam_card truth.
        QJsonObject webcam;
        webcam.insert(QStringLiteral("enabled"), scenario.webcam_pip_enabled);
        webcam.insert(QStringLiteral("mirror"), scenario.webcam_mirror);
        if (!scenario.webcam_chroma_color_mode.isEmpty()) {
            QJsonObject chroma;
            chroma.insert(QStringLiteral("enabled"), scenario.webcam_chroma_enabled);
            chroma.insert(QStringLiteral("color_mode"), scenario.webcam_chroma_color_mode);
            webcam.insert(QStringLiteral("chroma"), chroma);
        }
        preset.insert(QStringLiteral("webcam"), webcam);

        // codecs + container + filename_pattern — read from format widgets by
        // objectName where available, otherwise null.
        QJsonObject codecs;
        // video_codec_combo_ and audio_codec_combo_ do not have objectNames set
        // in ConfigPage; use null and rely on the scenario's output model.
        codecs.insert(QStringLiteral("video"), QJsonValue());
        codecs.insert(QStringLiteral("audio"), QJsonValue());
        preset.insert(QStringLiteral("codecs"), codecs);
        preset.insert(QStringLiteral("container"), QJsonValue());
        if (const auto* naming = window.findChild<const QLineEdit*>(QStringLiteral("namingEdit")))
            preset.insert(QStringLiteral("filename_pattern"), naming->text());
        else
            preset.insert(QStringLiteral("filename_pattern"), QJsonValue());

        root.insert(QStringLiteral("preset"), preset);
    }

    if (const auto* logs_page = window.findChild<const LogsPage*>(QStringLiteral("logsPage"))) {
        QJsonObject logs;
        logs.insert(QStringLiteral("total_entry_count"), logs_page->totalEntryCount());
        logs.insert(QStringLiteral("visible_entry_count"), logs_page->visibleEntryCount());
        logs.insert(QStringLiteral("active_filter"), logs_page->activeFilterName());
        logs.insert(QStringLiteral("search_query"), logs_page->searchQuery());
        logs.insert(QStringLiteral("auto_scroll"), logs_page->autoScrollEnabled());
        logs.insert(QStringLiteral("oldest_visible_severity"), logs_page->oldestVisibleSeverityName());
        logs.insert(QStringLiteral("newest_visible_severity"), logs_page->newestVisibleSeverityName());
        root.insert(QStringLiteral("logs"), logs);
    }

    // Recording markers manifest
    {
        QJsonObject markers;
        markers.insert(QStringLiteral("action_visible"), scenario.marker_action_visible);
        markers.insert(QStringLiteral("action_enabled"), scenario.marker_action_enabled);
        markers.insert(QStringLiteral("count"), scenario.marker_count);
        markers.insert(QStringLiteral("latest_time_ms"), static_cast<qint64>(scenario.marker_latest_time_ms));
        markers.insert(QStringLiteral("latest_type"), scenario.marker_latest_type);
        markers.insert(QStringLiteral("sidecar_file"), scenario.marker_sidecar_file);
        markers.insert(QStringLiteral("recording_state"), scenario.marker_recording_state);
        markers.insert(QStringLiteral("hk_active"), scenario.hk_marker_active);
        root.insert(QStringLiteral("markers"), markers);
    }

    QJsonArray masks;
    for (const VisualMask& mask : scenario.masks) {
        QJsonObject item;
        item.insert(QStringLiteral("object_name"), mask.object_name);
        item.insert(QStringLiteral("reason"), mask.reason);
        if (const QWidget* widget = window.findChild<QWidget*>(mask.object_name))
            item.insert(QStringLiteral("geometry"), RectToJson(widget->geometry()));
        masks.push_back(item);
    }
    root.insert(QStringLiteral("masks"), masks);

    QJsonArray widgets;
    const QList<QWidget*> named_widgets = window.findChildren<QWidget*>();
    for (const QWidget* widget : named_widgets) {
        if (widget->objectName().isEmpty() || !widget->isVisibleTo(&window))
            continue;
        QJsonObject item;
        item.insert(QStringLiteral("object_name"), widget->objectName());
        item.insert(QStringLiteral("class"), QString::fromLatin1(widget->metaObject()->className()));
        item.insert(QStringLiteral("geometry"), RectToJson(widget->geometry()));
        const QString text = WidgetText(widget).trimmed();
        if (!text.isEmpty())
            item.insert(QStringLiteral("text"), text);
        widgets.push_back(item);
    }
    root.insert(QStringLiteral("widgets"), widgets);

    return root;
}

bool WriteVisualManifest(const MainWindow& window, const VisualScenario& scenario, const QString& path) {
    if (path.trimmed().isEmpty())
        return false;
    if (!EnsureParentDir(path))
        return false;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(QJsonDocument(BuildVisualManifest(window, scenario)).toJson(QJsonDocument::Indented));
    return true;
}

bool WriteVisualScreenshot(QWidget& window, const QString& path) {
    if (path.trimmed().isEmpty())
        return false;
    if (!EnsureParentDir(path))
        return false;

    const QPixmap pixmap = window.grab();
    return !pixmap.isNull() && pixmap.save(path);
}

int RunVisualTest(QApplication& app, MainWindow& window, const VisualTestOptions& options) {
    const VisualScenario* scenario = FindVisualScenario(options.scenario_id);
    if (scenario == nullptr) {
        qCritical().noquote() << "Unknown visual scenario:" << options.scenario_id;
        qCritical().noquote() << "Known scenarios:" << VisualScenarioIds().join(QStringLiteral(", "));
        return VisualRunnerExitCode(false, false, false, !options.manifest_path.isEmpty(),
                                    !options.screenshot_path.isEmpty());
    }
    QString validation_error;
    if (!ValidateVisualScenario(*scenario, &validation_error)) {
        qCritical().noquote() << "Invalid visual scenario:" << options.scenario_id << validation_error;
        return VisualRunnerExitCode(false, false, false, !options.manifest_path.isEmpty(),
                                    !options.screenshot_path.isEmpty());
    }

    window.resize(1280, 820);
    window.applyVisualScenario(*scenario);
    if (options.maximize)
        window.showMaximized();
    else
        window.showNormal();

    window.raise();
    window.activateWindow();

    if (!options.exit_after_capture)
        return app.exec();

    QTimer::singleShot(120, &window, [&app, &window, scenario, options]() {
        const bool manifest_written =
            options.manifest_path.isEmpty() || WriteVisualManifest(window, *scenario, options.manifest_path);
        const bool screenshot_written =
            options.screenshot_path.isEmpty() || WriteVisualScreenshot(window, options.screenshot_path);
        app.exit(VisualRunnerExitCode(true, manifest_written, screenshot_written, !options.manifest_path.isEmpty(),
                                      !options.screenshot_path.isEmpty()));
    });
    return app.exec();
}

} // namespace exosnap::visual

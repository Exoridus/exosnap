#include "VisualTestHarness.h"

#include "../MainWindow.h"
#include "../pages/LogsPage.h"
#include "../ui/widgets/ExoToggle.h"
#include "../ui/widgets/PreviewSurface.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
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

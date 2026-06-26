#pragma once

#include "VisualScenario.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

class QApplication;

namespace exosnap {
class MainWindow;
}

namespace exosnap::visual {

struct VisualTestOptions {
    QString scenario_id;
    QString screenshot_path;
    QString manifest_path;
    bool maximize = false;
    bool exit_after_capture = false;
    int window_width = 0;    // 0 = harness default
    int window_height = 0;   // 0 = harness default
    int target_display = -1; // -1 = auto (capture mode prefers a non-primary screen)
};

bool HasVisualTestRequest(const QStringList& args);
bool ParseVisualTestOptions(const QStringList& args, VisualTestOptions* out, QString* error);
int RunVisualTest(QApplication& app, MainWindow& window, const VisualTestOptions& options);

QJsonObject BuildVisualManifest(const MainWindow& window, const VisualScenario& scenario);
bool WriteVisualManifest(const MainWindow& window, const VisualScenario& scenario, const QString& path);
bool WriteVisualScreenshot(QWidget& window, const QString& path);

} // namespace exosnap::visual

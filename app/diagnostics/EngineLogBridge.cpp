#include "diagnostics/EngineLogBridge.h"

#include "diagnostics/AppLog.h"

#include <recorder_core/logging/logging.h>

#include <QFileInfo>
#include <QString>

#include <filesystem>

namespace exosnap {
namespace {

diagnostics::LogSeverity ToAppSeverity(recorder_core::logging::LogLevel level) {
    using recorder_core::logging::LogLevel;
    switch (level) {
    case LogLevel::Trace:
    case LogLevel::Debug:
        return diagnostics::LogSeverity::Debug;
    case LogLevel::Info:
        return diagnostics::LogSeverity::Info;
    case LogLevel::Warn:
        return diagnostics::LogSeverity::Warning;
    case LogLevel::Error:
    case LogLevel::Critical:
        return diagnostics::LogSeverity::Error;
    }
    return diagnostics::LogSeverity::Info;
}

// "resolved mode=hdr10-native peak=1000" — the structured fields are what make an
// engine record readable, so they must survive the flattening into AppLog's text line.
QString Flatten(const recorder_core::logging::LogRecord& record) {
    QString text = QString::fromStdString(record.message);
    for (const auto& field : record.fields) {
        text += QStringLiteral(" %1=%2").arg(QString::fromStdString(field.key), QString::fromStdString(field.value));
    }
    return text;
}

} // namespace

void InitializeEngineLogging() {
    recorder_core::logging::LoggerConfig config;

    // Beside the app log, not inside it: the engine writes JSONL, AppLog writes text.
    const QFileInfo app_log(diagnostics::AppLog::logFilePath());
    config.filePath = std::filesystem::path(app_log.absolutePath().toStdWString()) / L"engine.jsonl";

    config.minimumLevel = recorder_core::logging::LogLevel::Info;

    // Stamp the launch session id onto every JSONL record, so the structured
    // stream, the text log (banner) and a support bundle share one launch key.
    const QString session = diagnostics::AppLog::sessionId();
    if (!session.isEmpty()) {
        config.baseFields.push_back({"session", session.toStdString()});
    }

    config.sink = [](const recorder_core::logging::LogRecord& record) {
        // Runs on whichever thread logged — usually the video thread. AppLog::write
        // takes its own lock and marshals delivery to the main thread.
        diagnostics::AppLog::write(ToAppSeverity(record.level), QString::fromStdString(record.component),
                                   Flatten(record));
    };

    recorder_core::logging::initialize(config);
}

void ShutdownEngineLogging() {
    recorder_core::logging::shutdown();
}

} // namespace exosnap

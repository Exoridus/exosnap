#include "diagnostics/EngineLogBridge.h"

#include "diagnostics/AppLog.h"

#include <exosnap/engine/logging/logging.h>

#include <QFileInfo>
#include <QString>

#include <filesystem>

namespace exosnap {
namespace {

diagnostics::LogSeverity ToAppSeverity(exosnap::engine::logging::LogLevel level) {
    using exosnap::engine::logging::LogLevel;
    switch (level) {
    case LogLevel::Trace:
    case LogLevel::Debug:
        return diagnostics::LogSeverity::Debug;
    case LogLevel::Info:
        return diagnostics::LogSeverity::Info;
    case LogLevel::Warn:
        return diagnostics::LogSeverity::Warning;
    case LogLevel::Error:
        return diagnostics::LogSeverity::Error;
    case LogLevel::Critical:
        return diagnostics::LogSeverity::Critical;
    }
    return diagnostics::LogSeverity::Info;
}

// "resolved mode=hdr10-native peak=1000" — the structured fields are what make an
// engine record readable, so they must survive the flattening into AppLog's text line.
QString Flatten(const exosnap::engine::logging::LogRecord& record) {
    QString text = QString::fromStdString(record.message);
    for (const auto& field : record.fields) {
        text += QStringLiteral(" %1=%2").arg(QString::fromStdString(field.key), QString::fromStdString(field.value));
    }
    return text;
}

} // namespace

void InitializeEngineLogging() {
    exosnap::engine::logging::LoggerConfig config;

    // Beside the app log, not inside it: the engine writes JSONL, AppLog writes text.
    const QFileInfo app_log(diagnostics::AppLog::logFilePath());
    config.filePath = std::filesystem::path(app_log.absolutePath().toStdWString()) / L"engine.jsonl";

    config.minimumLevel = exosnap::engine::logging::LogLevel::Info;

    // Stamp the launch session id onto every JSONL record, so the structured
    // stream, the text log (banner) and a support bundle share one launch key.
    const QString session = diagnostics::AppLog::sessionId();
    if (!session.isEmpty()) {
        config.baseFields.push_back({"session", session.toStdString()});
    }

    config.sink = [](const exosnap::engine::logging::LogRecord& record) {
        // Runs on whichever thread logged — usually the video thread. AppLog::write
        // takes its own lock and marshals delivery to the main thread.
        diagnostics::AppLog::write(ToAppSeverity(record.level), QString::fromStdString(record.component),
                                   Flatten(record));
    };

    exosnap::engine::logging::initialize(config);
}

void ShutdownEngineLogging() {
    exosnap::engine::logging::shutdown();
}

} // namespace exosnap

#include "diagnostics/StructuredLog.h"

#include <span>
#include <vector>

namespace exosnap::diagnostics {
namespace {

recorder_core::logging::LogLevel ToEngineLevel(LogSeverity severity) {
    using recorder_core::logging::LogLevel;
    switch (severity) {
    case LogSeverity::Debug:
        return LogLevel::Debug;
    case LogSeverity::Info:
        return LogLevel::Info;
    case LogSeverity::Warning:
        return LogLevel::Warn;
    case LogSeverity::Error:
        return LogLevel::Error;
    }
    return LogLevel::Info;
}

} // namespace

void logEvent(LogSeverity severity, std::string_view subsystem, std::string_view event_code,
              std::initializer_list<recorder_core::logging::LogField> fields) {
    const std::vector<recorder_core::logging::LogField> owned(fields);
    recorder_core::logging::log(ToEngineLevel(severity), subsystem, event_code,
                                std::span<const recorder_core::logging::LogField>(owned.data(), owned.size()));
}

} // namespace exosnap::diagnostics

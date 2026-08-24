#include "diagnostics/StructuredLog.h"

#include <span>
#include <vector>

namespace exosnap::diagnostics {
namespace {

exosnap::engine::logging::LogLevel ToEngineLevel(LogSeverity severity) {
    using exosnap::engine::logging::LogLevel;
    switch (severity) {
    case LogSeverity::Debug:
        return LogLevel::Debug;
    case LogSeverity::Info:
        return LogLevel::Info;
    case LogSeverity::Warning:
        return LogLevel::Warn;
    case LogSeverity::Error:
        return LogLevel::Error;
    case LogSeverity::Critical:
        return LogLevel::Critical;
    }
    return LogLevel::Info;
}

} // namespace

void logEvent(LogSeverity severity, std::string_view subsystem, std::string_view event_code,
              std::initializer_list<exosnap::engine::logging::LogField> fields) {
    const std::vector<exosnap::engine::logging::LogField> owned(fields);
    exosnap::engine::logging::log(ToEngineLevel(severity), subsystem, event_code,
                                  std::span<const exosnap::engine::logging::LogField>(owned.data(), owned.size()));
}

} // namespace exosnap::diagnostics

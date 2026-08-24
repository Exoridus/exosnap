#pragma once

#include "diagnostics/AppLog.h"

#include <exosnap/engine/logging/logging.h>

#include <initializer_list>
#include <string_view>

namespace exosnap::diagnostics {

// Emit a structured, locale-independent event into the canonical engine JSONL
// stream (with the launch session id stamped on by the bridge).
//
// Forwards ONLY to exosnap::engine::logging::log — it never writes AppLog directly.
// The EngineLogBridge sink is the single source of the flattened text line: one
// record -> one JSONL entry -> one AppLog line. A direct AppLog write here would
// double every event (once from here, once when the bridge flattens the record).
//
// Convention: event_code is a stable token (e.g. "record.start", "encoder.init",
// "audio.discontinuity", "mux.finalize", "disk.hardstop"), and severity must be
// >= Info. The engine logger's minimumLevel is Info, so a Debug event would be
// dropped before it reached either the JSONL or the bridge.
void logEvent(LogSeverity severity, std::string_view subsystem, std::string_view event_code,
              std::initializer_list<exosnap::engine::logging::LogField> fields = {});

} // namespace exosnap::diagnostics

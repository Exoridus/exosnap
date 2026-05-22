#pragma once
#include <QString>

namespace exosnap::diagnostics {

// One-time init: creates the log directory and writes the startup banner.
// Safe to call before QApplication::exec() but requires QCoreApplication to exist.
void AppLogInit();

// Write a timestamped line to the session log.
// Flushed after every call. Thread-safe.
void AppLog(const QString& message);

} // namespace exosnap::diagnostics

#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recorder_core::logging {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Critical };

struct LogField {
    std::string key;
    std::string value;
};

struct LogRecord {
    std::chrono::system_clock::time_point timestamp;
    LogLevel level;
    std::string component;
    std::string message;
    std::vector<LogField> fields;
};

// Receives every accepted record. Invoked on the thread that logged, outside the
// logger's own lock, so a sink may block, marshal, or log again without deadlocking.
using LogSink = std::function<void(const LogRecord&)>;

struct LoggerConfig {
    std::filesystem::path filePath;
    std::size_t ringCapacity = 512;
    LogLevel minimumLevel = LogLevel::Info;

    // Size-based rotation of the on-disk JSONL file. The engine appends across
    // launches (no truncate) and rotates when the live file would exceed
    // maxFileBytes, keeping maxFileCount files total (engine.jsonl[.1][.2]).
    // Mirrors the app text log's 5 MiB / 3-file bound. Kept configurable so a
    // rotation test can drive a small threshold instead of writing megabytes.
    std::size_t maxFileBytes = 5 * 1024 * 1024;
    std::size_t maxFileCount = 3;

    // Fields stamped onto every record's fields{} (e.g. the launch session id),
    // so a single key correlates the JSONL stream, the text log and the session
    // report. Appended after the per-call fields; per-call keys win on collision.
    std::vector<LogField> baseFields;

    // Optional. The engine writes its own JSONL file regardless; a host sets this to
    // also surface engine records in its own log. Without it the engine's decisions
    // are invisible to the application.
    LogSink sink;
};

std::string_view to_string(LogLevel level) noexcept;

void initialize(const LoggerConfig& config);

void shutdown() noexcept;

void log(LogLevel level, std::string_view component, std::string_view message, std::span<const LogField> fields = {});

std::vector<LogRecord> snapshot_ring_buffer();

} // namespace recorder_core::logging

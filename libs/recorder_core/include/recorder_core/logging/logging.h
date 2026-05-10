#pragma once

#include <chrono>
#include <filesystem>
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

struct LoggerConfig {
    std::filesystem::path filePath;
    std::size_t ringCapacity = 512;
    LogLevel minimumLevel = LogLevel::Info;
};

std::string_view to_string(LogLevel level) noexcept;

void initialize(const LoggerConfig& config);

void shutdown() noexcept;

void log(LogLevel level, std::string_view component, std::string_view message, std::span<const LogField> fields = {});

std::vector<LogRecord> snapshot_ring_buffer();

} // namespace recorder_core::logging

#include <recorder_core/logging/logging.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <nlohmann/json.hpp>

#include <deque>
#include <mutex>
#include <stdexcept>

namespace recorder_core::logging {

namespace {

struct InternalState {
    std::mutex mutex;
    std::shared_ptr<spdlog::logger> logger;
    std::deque<LogRecord> ringBuffer;
    std::size_t ringCapacity = 512;
    LogLevel minimumLevel = LogLevel::Info;
    bool initialized = false;
};

InternalState& state() {
    static InternalState s;
    return s;
}

spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return spdlog::level::trace;
    case LogLevel::Debug:
        return spdlog::level::debug;
    case LogLevel::Info:
        return spdlog::level::info;
    case LogLevel::Warn:
        return spdlog::level::warn;
    case LogLevel::Error:
        return spdlog::level::err;
    case LogLevel::Critical:
        return spdlog::level::critical;
    }
    return spdlog::level::info;
}

void reset_state_locked(InternalState& s) noexcept {
    if (s.logger) {
        s.logger->flush();
        s.logger.reset();
    }
    s.ringBuffer.clear();
    s.initialized = false;
}

} // namespace

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "trace";
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    case LogLevel::Critical:
        return "critical";
    }
    return "info";
}

void initialize(const LoggerConfig& config) {
    if (config.ringCapacity == 0) {
        throw std::invalid_argument("ringCapacity must be greater than 0");
    }

    auto& s = state();
    std::lock_guard lock(s.mutex);

    reset_state_locked(s);

    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        config.filePath.string(), true);
    s.logger = std::make_shared<spdlog::logger>("exosnap", sink);
    s.logger->set_pattern("%v");
    s.logger->set_level(spdlog::level::trace);
    s.logger->flush_on(spdlog::level::info);

    s.ringCapacity = config.ringCapacity;
    s.minimumLevel = config.minimumLevel;
    s.initialized = true;
}

void shutdown() noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    reset_state_locked(s);
}

void log(LogLevel level,
         std::string_view component,
         std::string_view message,
         std::span<const LogField> fields) {
    auto& s = state();
    std::lock_guard lock(s.mutex);

    if (!s.initialized) {
        return;
    }
    if (static_cast<int>(level) < static_cast<int>(s.minimumLevel)) {
        return;
    }

    LogRecord record;
    record.timestamp = std::chrono::system_clock::now();
    record.level = level;
    record.component = component;
    record.message = message;
    record.fields.assign(fields.begin(), fields.end());

    if (s.ringCapacity > 0) {
        s.ringBuffer.push_back(record);
        while (s.ringBuffer.size() > s.ringCapacity) {
            s.ringBuffer.pop_front();
        }
    }

    nlohmann::json j;
    j["timestamp_unix_ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            record.timestamp.time_since_epoch())
            .count();
    j["level"] = to_string(record.level);
    j["component"] = record.component;
    j["message"] = record.message;

    nlohmann::json fieldsJson = nlohmann::json::object();
    for (const auto& f : record.fields) {
        fieldsJson[f.key] = f.value;
    }
    j["fields"] = fieldsJson;

    s.logger->log(to_spdlog_level(level), j.dump());
}

std::vector<LogRecord> snapshot_ring_buffer() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return {s.ringBuffer.begin(), s.ringBuffer.end()};
}

} // namespace recorder_core::logging

#pragma once

// ObservabilityJson.h -- the shared serialization primitives every observability
// surface uses, and nothing else.
//
// Two rules live here, and they are the reason this is a header rather than a
// convention:
//
//  1. An unmeasured value serializes as JSON `null`, never as 0/false/"".
//     A zero that means "not measured" is indistinguishable from a zero that was
//     measured, and every consumer downstream -- Diagnostics tiles, the release
//     runner, a future adapter -- would then have to guess. `Metric()` takes the
//     availability with the number so the two cannot be separated by accident.
//
//  2. Availability is a WORD, not a bool. "unsupported on this capture backend"
//     and "not sampled yet" are different answers and send a reader to different
//     places; collapsing them into `false` throws that away.
//
// Nothing here knows what a pipeline, a setting or an environment is. The domain
// surfaces do.

#include <exosnap/engine/pipeline_diagnostics.h>

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <cstdint>

namespace exosnap::observability {

// The truth classes a fact can carry. Deliberately a closed set shared by every
// surface: a reader that learns them once can read all of them.
namespace availability {
inline constexpr const char* kAvailable = "available";
// Measurable in principle, but nothing has been measured yet (warm-up, no
// session, no sample).
inline constexpr const char* kUnavailable = "unavailable";
// Structurally impossible here -- e.g. present cadence under WGC, which exposes
// no present timestamp at all. Never becomes available by waiting.
inline constexpr const char* kUnsupported = "unsupported";
// The measurement exists but this process cannot take it without elevation.
inline constexpr const char* kRequiresElevation = "requiresElevation";
// The measurement exists but the user has not opted in to it.
inline constexpr const char* kRequiresOptIn = "requiresOptIn";
// Sampled, and known to be wrong: the measurement's own preconditions were
// violated. A reader must discard the number rather than wait for a better one.
inline constexpr const char* kFaulted = "faulted";
} // namespace availability

[[nodiscard]] inline QString AvailabilityKey(exosnap::engine::MetricAvailability value) {
    switch (value) {
    case exosnap::engine::MetricAvailability::Available:
        return QString::fromLatin1(availability::kAvailable);
    case exosnap::engine::MetricAvailability::Faulted:
        return QString::fromLatin1(availability::kFaulted);
    case exosnap::engine::MetricAvailability::Unavailable:
        break;
    }
    return QString::fromLatin1(availability::kUnavailable);
}

[[nodiscard]] inline bool IsAvailable(exosnap::engine::MetricAvailability value) noexcept {
    return value == exosnap::engine::MetricAvailability::Available;
}

// A number that is only meaningful when `available`. The whole point of the
// helper is that the caller cannot emit the number without stating that.
[[nodiscard]] inline QJsonValue Metric(double value, bool available) {
    return available ? QJsonValue(value) : QJsonValue(QJsonValue::Null);
}

[[nodiscard]] inline QJsonValue Metric(double value, exosnap::engine::MetricAvailability availability_value) {
    return Metric(value, IsAvailable(availability_value));
}

// Counters that are always real (they start at 0 and only ever count up) do not
// go through Metric(). They go through this, which exists only so a 64-bit count
// does not silently lose precision in a JSON double.
[[nodiscard]] inline QJsonValue Count(std::uint64_t value) {
    return QJsonValue(static_cast<double>(value));
}

// A string that is absent rather than empty.
[[nodiscard]] inline QJsonValue TextOrNull(const QString& value) {
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

[[nodiscard]] inline QJsonValue TextOrNull(const std::string& value) {
    return value.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QString::fromStdString(value));
}

} // namespace exosnap::observability

#pragma once

#include "EditCommandTypes.h"

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace exosnap::edit {

enum class TimeFieldFormat { Timecode, Seconds, Samples };
struct TimeFieldContext {
    RationalFps fps{60, 1};
    Time base_us = 0;
    TimeFieldFormat format = TimeFieldFormat::Seconds;
};
enum class TimeParseError { None, InvalidSyntax, InvalidRate, InvalidFrame, NegativeTime, Overflow, Precision };
struct TimeParseResult {
    Time value_us = 0;
    TimeParseError error = TimeParseError::None;
    explicit operator bool() const {
        return error == TimeParseError::None;
    }
};

namespace time_detail {

inline std::optional<Time> Unsigned(std::string_view value) {
    if (value.empty())
        return std::nullopt;
    Time result = 0;
    for (const char digit : value) {
        if (digit < '0' || digit > '9' || result > ((std::numeric_limits<Time>::max)() - (digit - '0')) / 10)
            return std::nullopt;
        result = result * 10 + digit - '0';
    }
    return result;
}

inline std::optional<Time> MultiplyAdd(Time value, Time multiplier, Time add) {
    if (value < 0 || multiplier < 0 || add < 0 ||
        (multiplier && value > ((std::numeric_limits<Time>::max)() - add) / multiplier))
        return std::nullopt;
    return value * multiplier + add;
}

inline std::string Padded(Time value, size_t digits) {
    auto text = std::to_string(value);
    if (text.size() < digits)
        text.insert(0, digits - text.size(), '0');
    return text;
}

} // namespace time_detail

// Explicit units override the field format. A leading sign applies to the
// current value; frame/sample offsets advance indices rather than rounded durations.
inline TimeParseResult ParseTime(std::string_view text, const TimeFieldContext& context) {
    const auto fail = [](TimeParseError error) { return TimeParseResult{0, error}; };
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos)
        return fail(TimeParseError::InvalidSyntax);
    text = text.substr(first, text.find_last_not_of(whitespace) - first + 1);
    bool relative = false;
    bool subtract = false;
    if (text.front() == '+' || text.front() == '-') {
        relative = true;
        subtract = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.empty())
        return fail(TimeParseError::InvalidSyntax);
    std::optional<Time> value;
    if (text.find(':') != std::string_view::npos) {
        if (relative)
            return fail(TimeParseError::InvalidSyntax);
        if (!context.fps.valid())
            return fail(TimeParseError::InvalidRate);
        std::array<Time, 4> fields{};
        for (size_t index = 0; index < fields.size(); ++index) {
            const auto colon = text.find(':');
            if ((index == fields.size() - 1) != (colon == std::string_view::npos))
                return fail(TimeParseError::InvalidSyntax);
            const auto field = text.substr(0, colon);
            const auto number = time_detail::Unsigned(field);
            if (!number)
                return fail(TimeParseError::InvalidSyntax);
            fields[index] = *number;
            if (colon != std::string_view::npos)
                text.remove_prefix(colon + 1);
        }
        const Time nominal = (static_cast<uint64_t>(context.fps.num) + context.fps.den - 1) / context.fps.den;
        if (fields[1] >= 60 || fields[2] >= 60 || fields[3] >= nominal)
            return fail(TimeParseError::InvalidFrame);
        auto index = time_detail::MultiplyAdd(fields[0], 60, fields[1]);
        index = index ? time_detail::MultiplyAdd(*index, 60, fields[2]) : std::nullopt;
        index = index ? time_detail::MultiplyAdd(*index, nominal, fields[3]) : std::nullopt;
        value = index ? context.fps.frameTime(*index) : std::nullopt;
    } else {
        enum class Unit { Seconds, Milliseconds, Frames, Samples };
        Unit unit = context.format == TimeFieldFormat::Samples ? Unit::Samples : Unit::Seconds;
        if (text.ends_with("smp")) {
            unit = Unit::Samples;
            text.remove_suffix(3);
        } else if (text.ends_with("ms")) {
            unit = Unit::Milliseconds;
            text.remove_suffix(2);
        } else if (text.ends_with('s')) {
            unit = Unit::Seconds;
            text.remove_suffix(1);
        } else if (text.ends_with('f')) {
            unit = Unit::Frames;
            text.remove_suffix(1);
        }
        if (unit == Unit::Frames || unit == Unit::Samples) {
            if (unit == Unit::Frames && !context.fps.valid())
                return fail(TimeParseError::InvalidRate);
            const auto count = time_detail::Unsigned(text);
            if (!count)
                return fail(TimeParseError::InvalidSyntax);
            if (relative) {
                const auto offset = subtract ? -*count : *count;
                value = unit == Unit::Frames ? context.fps.step(context.base_us, offset)
                                             : AudioSampleGrid::step(context.base_us, offset);
                if (!value)
                    return fail(subtract ? TimeParseError::NegativeTime : TimeParseError::Overflow);
                return {*value, TimeParseError::None};
            }
            value = unit == Unit::Frames ? context.fps.frameTime(*count) : AudioSampleGrid::sampleTime(*count);
        } else {
            const auto decimal = text.find_first_of(".,");
            const auto whole = time_detail::Unsigned(decimal == 0 ? std::string_view{"0"} : text.substr(0, decimal));
            if (!whole)
                return fail(TimeParseError::InvalidSyntax);
            const Time multiplier = unit == Unit::Seconds ? 1'000'000 : 1000;
            Time fraction_us = 0;
            if (decimal != std::string_view::npos) {
                const auto fraction = text.substr(decimal + 1);
                if (fraction.empty() || fraction.find_first_of(".,") != std::string_view::npos)
                    return fail(TimeParseError::InvalidSyntax);
                if (fraction.size() > 6)
                    return fail(TimeParseError::Precision);
                const auto number = time_detail::Unsigned(fraction);
                if (!number)
                    return fail(TimeParseError::InvalidSyntax);
                Time divisor = 1;
                for (size_t digit = 0; digit < fraction.size(); ++digit)
                    divisor *= 10;
                if (*number * multiplier % divisor != 0)
                    return fail(TimeParseError::Precision);
                fraction_us = *number * multiplier / divisor;
            }
            value = time_detail::MultiplyAdd(*whole, multiplier, fraction_us);
        }
    }
    if (!value)
        return fail(TimeParseError::Overflow);
    if (relative) {
        if (context.base_us < 0 || (subtract && *value > context.base_us))
            return fail(TimeParseError::NegativeTime);
        if (subtract)
            return {context.base_us - *value, TimeParseError::None};
        value = time_detail::MultiplyAdd(context.base_us, 1, *value);
        if (!value)
            return fail(TimeParseError::Overflow);
    }
    return {*value, TimeParseError::None};
}

inline std::optional<std::string> FormatTime(Time time, RationalFps fps, TimeFieldFormat format) {
    if (time < 0)
        return std::nullopt;
    if (format == TimeFieldFormat::Seconds) {
        auto result = std::to_string(time / 1'000'000);
        if (time % 1'000'000) {
            auto fraction = time_detail::Padded(time % 1'000'000, 6);
            fraction.erase(fraction.find_last_not_of('0') + 1);
            result += '.' + fraction;
        }
        return result;
    }
    if (format == TimeFieldFormat::Samples) {
        const auto sample = AudioSampleGrid::nearestSampleIndex(time);
        return sample ? std::optional<std::string>{std::to_string(*sample)} : std::nullopt;
    }
    const auto frame = fps.nearestFrameIndex(time);
    if (!frame)
        return std::nullopt;
    const Time nominal = (static_cast<uint64_t>(fps.num) + fps.den - 1) / fps.den;
    const auto seconds = *frame / nominal;
    return time_detail::Padded(seconds / 3600, 2) + ':' + time_detail::Padded(seconds / 60 % 60, 2) + ':' +
           time_detail::Padded(seconds % 60, 2) + ':' + time_detail::Padded(*frame % nominal, 2);
}

} // namespace exosnap::edit

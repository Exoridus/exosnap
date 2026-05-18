#pragma once

#include <string>

namespace exosnap::capability {

enum class SupportLevel {
    Available,
    ValidUnvalidated,
    NotImplemented,
    Invalid,
};

struct SupportAnnotation {
    SupportLevel level = SupportLevel::Invalid;
    std::string reason;
};

inline bool IsSelectable(SupportLevel level) noexcept {
    return level == SupportLevel::Available || level == SupportLevel::ValidUnvalidated;
}

inline bool IsSelectable(const SupportAnnotation& annotation) noexcept {
    return IsSelectable(annotation.level);
}

inline bool IsHardInvalid(SupportLevel level) noexcept {
    return level == SupportLevel::Invalid;
}

inline bool IsHardInvalid(const SupportAnnotation& annotation) noexcept {
    return IsHardInvalid(annotation.level);
}

} // namespace exosnap::capability

#include "env_types.h"

#include <array>
#include <tuple>
#include <utility>

namespace exosnap::envctl {
namespace {

constexpr std::array<std::pair<CapabilityClass, std::string_view>, 7> kCapabilityKeys{{
    {CapabilityClass::Read, "ENV_READ"},
    {CapabilityClass::MutateSafe, "ENV_MUTATE_SAFE"},
    {CapabilityClass::MutateTestOnly, "ENV_MUTATE_TESTONLY"},
    {CapabilityClass::Human, "ENV_HUMAN"},
    {CapabilityClass::Physical, "PHYSICAL"},
    {CapabilityClass::Secure, "SECURE"},
    {CapabilityClass::Unavailable, "UNAVAILABLE"},
}};

constexpr std::array<std::pair<TransactionState, std::string_view>, 9> kStateKeys{{
    {TransactionState::Clean, "Clean"},
    {TransactionState::Prepared, "Prepared"},
    {TransactionState::Mutating, "Mutating"},
    {TransactionState::Active, "Active"},
    {TransactionState::Restoring, "Restoring"},
    {TransactionState::Restored, "Restored"},
    {TransactionState::RestorePending, "RestorePending"},
    {TransactionState::RestorePendingDeviceUnavailable, "RestorePendingDeviceUnavailable"},
    {TransactionState::RestoreFailed, "RestoreFailed"},
}};

} // namespace

std::string_view ToKey(CapabilityClass value) {
    for (const auto& [entry, key] : kCapabilityKeys) {
        if (entry == value) {
            return key;
        }
    }
    return "UNAVAILABLE";
}

std::optional<CapabilityClass> CapabilityClassFromKey(std::string_view key) {
    for (const auto& [entry, name] : kCapabilityKeys) {
        if (name == key) {
            return entry;
        }
    }
    return std::nullopt;
}

bool IsMutable(CapabilityClass value) {
    return value == CapabilityClass::MutateSafe || value == CapabilityClass::MutateTestOnly;
}

std::string PropertyId::Key() const {
    return device_alias + ":" + property;
}

bool operator==(const PropertyId& lhs, const PropertyId& rhs) {
    return lhs.device_alias == rhs.device_alias && lhs.property == rhs.property;
}

bool operator!=(const PropertyId& lhs, const PropertyId& rhs) {
    return !(lhs == rhs);
}

bool operator<(const PropertyId& lhs, const PropertyId& rhs) {
    return std::tie(lhs.device_alias, lhs.property) < std::tie(rhs.device_alias, rhs.property);
}

std::optional<PropertyId> PropertyIdFromKey(std::string_view key) {
    const auto separator = key.find(':');
    if (separator == std::string_view::npos) {
        return std::nullopt;
    }
    const auto alias = key.substr(0, separator);
    const auto property = key.substr(separator + 1);
    if (alias.empty() || property.empty()) {
        return std::nullopt;
    }
    // A second ':' would make the key ambiguous on the way back out; reject it
    // rather than silently binding it to the property half.
    if (property.find(':') != std::string_view::npos) {
        return std::nullopt;
    }
    return PropertyId{std::string(alias), std::string(property)};
}

std::string_view ToKey(TransactionState value) {
    for (const auto& [entry, key] : kStateKeys) {
        if (entry == value) {
            return key;
        }
    }
    return "Clean";
}

std::optional<TransactionState> TransactionStateFromKey(std::string_view key) {
    for (const auto& [entry, name] : kStateKeys) {
        if (name == key) {
            return entry;
        }
    }
    return std::nullopt;
}

bool IsDirty(TransactionState value) {
    switch (value) {
    case TransactionState::Clean:
    case TransactionState::Restored:
        return false;
    case TransactionState::Prepared:
    case TransactionState::Mutating:
    case TransactionState::Active:
    case TransactionState::Restoring:
    case TransactionState::RestorePending:
    case TransactionState::RestorePendingDeviceUnavailable:
    case TransactionState::RestoreFailed:
        return true;
    }
    return true;
}

bool IsTerminal(TransactionState value) {
    return value == TransactionState::Clean || value == TransactionState::Restored ||
           value == TransactionState::RestoreFailed || value == TransactionState::RestorePendingDeviceUnavailable;
}

} // namespace exosnap::envctl

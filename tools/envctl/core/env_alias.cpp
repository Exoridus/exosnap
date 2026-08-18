#include "env_alias.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace exosnap::envctl {
namespace {

constexpr int kProfileSchemaVersion = 1;

std::string ReadWholeFile(const std::string& path, bool& found) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        found = false;
        return {};
    }
    found = true;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

} // namespace

std::string_view ToKey(AliasErrorCode value) {
    switch (value) {
    case AliasErrorCode::None:
        return "none";
    case AliasErrorCode::ProfileMissing:
        return "profile_missing";
    case AliasErrorCode::ProfileInvalid:
        return "profile_invalid";
    case AliasErrorCode::UnboundAlias:
        return "unbound_alias";
    case AliasErrorCode::AmbiguousDevice:
        return "ambiguous_device";
    case AliasErrorCode::DeviceNotPresent:
        return "device_not_present";
    case AliasErrorCode::DuplicateAlias:
        return "duplicate_alias";
    }
    return "none";
}

std::string_view ToKey(BindingStatus value) {
    switch (value) {
    case BindingStatus::Ok:
        return "ok";
    case BindingStatus::DeviceNotPresent:
        return "device_not_present";
    case BindingStatus::Ambiguous:
        return "ambiguous_device";
    case BindingStatus::FriendlyNameChanged:
        return "friendly_name_changed";
    }
    return "ok";
}

std::string AliasProfile::DefaultProfileRelativePath() {
    return ".workspace/env-profile.json";
}

std::string UnboundAliasInstruction(std::string_view alias) {
    std::string message = "unbound_alias: the scenario requires alias '";
    message += alias;
    message += "', which the alias profile does not bind. Bind it once on this machine: "
               "run `exosnap-envctl resolve-aliases` to list the candidate stable ids, then "
               "`exosnap-envctl bind-alias --alias ";
    message += alias;
    message += " --stable-id <STABLE_ID>` with the id of the device you mean. ";
    message += "No device is selected automatically.";
    return message;
}

AliasProfile::LoadResult AliasProfile::LoadFromJsonText(std::string_view text) {
    LoadResult result;
    nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        result.error = {AliasErrorCode::ProfileInvalid, {}, "profile_invalid: the alias profile is not a JSON object."};
        return result;
    }

    const auto schema_version = document.value("schemaVersion", 0);
    if (schema_version != kProfileSchemaVersion) {
        result.error = {AliasErrorCode::ProfileInvalid,
                        {},
                        "profile_invalid: unsupported schemaVersion " + std::to_string(schema_version) + " (expected " +
                            std::to_string(kProfileSchemaVersion) + ")."};
        return result;
    }

    const auto aliases = document.find("aliases");
    if (aliases == document.end() || !aliases->is_object()) {
        result.error = {AliasErrorCode::ProfileInvalid, {}, "profile_invalid: 'aliases' must be an object."};
        return result;
    }

    AliasProfile profile;
    for (const auto& [alias, entry] : aliases->items()) {
        if (!entry.is_object()) {
            result.error = {AliasErrorCode::ProfileInvalid, alias,
                            "profile_invalid: binding for '" + alias + "' is not an object."};
            return result;
        }
        AliasBinding binding;
        binding.alias = alias;
        binding.kind = entry.value("kind", std::string{});
        binding.stable_id = entry.value("stableId", std::string{});
        binding.friendly_name = entry.value("friendlyName", std::string{});
        binding.bound_at_utc = entry.value("boundAtUtc", std::string{});
        if (binding.stable_id.empty()) {
            result.error = {AliasErrorCode::ProfileInvalid, alias,
                            "profile_invalid: binding for '" + alias +
                                "' has no stableId. A friendly name is never a matching key; rebind with "
                                "`exosnap-envctl bind-alias --alias " +
                                alias + " --stable-id <STABLE_ID>`."};
            return result;
        }
        if (profile.Find(alias).has_value()) {
            result.error = {AliasErrorCode::DuplicateAlias, alias,
                            "duplicate_alias: '" + alias + "' is bound more than once."};
            return result;
        }
        profile.bindings_.push_back(binding);
    }

    std::sort(profile.bindings_.begin(), profile.bindings_.end(),
              [](const AliasBinding& lhs, const AliasBinding& rhs) { return lhs.alias < rhs.alias; });

    result.ok = true;
    result.profile = std::move(profile);
    return result;
}

AliasProfile::LoadResult AliasProfile::LoadFromFile(const std::string& path) {
    bool found = false;
    const std::string text = ReadWholeFile(path, found);
    if (!found) {
        // An absent profile is the normal state of a fresh machine, not a
        // failure: `resolve-aliases` must still be able to say what to bind.
        LoadResult result;
        result.ok = true;
        result.error = {AliasErrorCode::ProfileMissing,
                        {},
                        "profile_missing: no alias profile at '" + path +
                            "'. It is created by the first `exosnap-envctl bind-alias` run."};
        return result;
    }
    return LoadFromJsonText(text);
}

std::optional<AliasBinding> AliasProfile::Find(std::string_view alias) const {
    const auto match = std::find_if(bindings_.begin(), bindings_.end(),
                                    [&](const AliasBinding& binding) { return binding.alias == alias; });
    if (match == bindings_.end()) {
        return std::nullopt;
    }
    return *match;
}

void AliasProfile::Upsert(const AliasBinding& binding) {
    const auto match = std::find_if(bindings_.begin(), bindings_.end(),
                                    [&](const AliasBinding& entry) { return entry.alias == binding.alias; });
    if (match == bindings_.end()) {
        bindings_.push_back(binding);
        std::sort(bindings_.begin(), bindings_.end(),
                  [](const AliasBinding& lhs, const AliasBinding& rhs) { return lhs.alias < rhs.alias; });
        return;
    }
    *match = binding;
}

std::string AliasProfile::ToJsonText() const {
    nlohmann::ordered_json document;
    document["schemaVersion"] = kProfileSchemaVersion;
    nlohmann::ordered_json aliases = nlohmann::ordered_json::object();
    for (const auto& binding : bindings_) {
        nlohmann::ordered_json entry;
        entry["kind"] = binding.kind;
        entry["stableId"] = binding.stable_id;
        entry["friendlyName"] = binding.friendly_name;
        entry["boundAtUtc"] = binding.bound_at_utc;
        aliases[binding.alias] = entry;
    }
    document["aliases"] = aliases;
    return document.dump(2) + "\n";
}

bool AliasProfile::SaveToFile(const std::string& path, std::string& error) const {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "could not open '" + path + "' for writing.";
        return false;
    }
    const std::string text = ToJsonText();
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream) {
        error = "could not write '" + path + "'.";
        return false;
    }
    return true;
}

AliasProfile::ResolveResult AliasProfile::Resolve(const std::vector<std::string>& required_aliases) const {
    ResolveResult result;
    for (const auto& alias : required_aliases) {
        const auto binding = Find(alias);
        if (!binding.has_value()) {
            result.errors.push_back({AliasErrorCode::UnboundAlias, alias, UnboundAliasInstruction(alias)});
            continue;
        }
        result.resolved.push_back(*binding);
    }
    result.ok = result.errors.empty();
    return result;
}

std::vector<BindingReport>
AliasProfile::ValidateAgainstInventory(const std::vector<DeviceCandidate>& candidates) const {
    std::vector<BindingReport> reports;
    reports.reserve(bindings_.size());
    for (const auto& binding : bindings_) {
        BindingReport report;
        report.binding = binding;
        for (const auto& candidate : candidates) {
            if (candidate.stable_id == binding.stable_id) {
                report.matching_stable_ids.push_back(candidate.stable_id);
                report.current_friendly_name = candidate.friendly_name;
            }
        }
        if (report.matching_stable_ids.empty()) {
            report.status = BindingStatus::DeviceNotPresent;
        } else if (report.matching_stable_ids.size() > 1) {
            // Two devices reporting the same stable id is a host bug, but it is
            // also exactly the situation in which choosing one is unsafe.
            report.status = BindingStatus::Ambiguous;
        } else if (report.current_friendly_name != binding.friendly_name) {
            report.status = BindingStatus::FriendlyNameChanged;
        } else {
            report.status = BindingStatus::Ok;
        }
        reports.push_back(report);
    }
    return reports;
}

BindOutcome BindByFriendlyName(const std::string& alias, const std::string& kind, const std::string& friendly_name,
                               const std::vector<DeviceCandidate>& candidates, const std::string& now_utc) {
    BindOutcome outcome;
    std::vector<const DeviceCandidate*> matches;
    for (const auto& candidate : candidates) {
        if (candidate.friendly_name == friendly_name && (kind.empty() || candidate.kind == kind)) {
            matches.push_back(&candidate);
        }
    }
    if (matches.empty()) {
        outcome.error = {AliasErrorCode::DeviceNotPresent, alias,
                         "device_not_present: no " + (kind.empty() ? std::string("device") : kind) + " named '" +
                             friendly_name + "' is attached right now."};
        return outcome;
    }
    if (matches.size() > 1) {
        std::string message = "ambiguous_device: " + std::to_string(matches.size()) + " devices share the name '" +
                              friendly_name +
                              "'. Nothing was selected. Pick one of these stable ids and rerun with "
                              "`exosnap-envctl bind-alias --alias " +
                              alias + " --stable-id <STABLE_ID>`:";
        for (const auto* match : matches) {
            message += " " + match->stable_id;
        }
        outcome.error = {AliasErrorCode::AmbiguousDevice, alias, message};
        return outcome;
    }
    outcome.ok = true;
    outcome.binding = {alias, matches.front()->kind, matches.front()->stable_id, matches.front()->friendly_name,
                       now_utc};
    return outcome;
}

BindOutcome BindByStableId(const std::string& alias, const std::string& kind, const std::string& stable_id,
                           const std::vector<DeviceCandidate>& candidates, const std::string& now_utc) {
    BindOutcome outcome;
    std::vector<const DeviceCandidate*> matches;
    for (const auto& candidate : candidates) {
        if (candidate.stable_id == stable_id) {
            matches.push_back(&candidate);
        }
    }
    if (matches.empty()) {
        outcome.error = {AliasErrorCode::DeviceNotPresent, alias,
                         "device_not_present: no device with stable id '" + stable_id +
                             "' is attached right now. Run `exosnap-envctl resolve-aliases` for the current list."};
        return outcome;
    }
    if (matches.size() > 1) {
        outcome.error = {AliasErrorCode::AmbiguousDevice, alias,
                         "ambiguous_device: stable id '" + stable_id + "' matches " + std::to_string(matches.size()) +
                             " devices. Nothing was selected."};
        return outcome;
    }
    outcome.ok = true;
    outcome.binding = {alias, kind.empty() ? matches.front()->kind : kind, matches.front()->stable_id,
                       matches.front()->friendly_name, now_utc};
    return outcome;
}

} // namespace exosnap::envctl

// exosnap-envctl -- TEST-ONLY CLI over the environment orchestrator.
//
// Not installed, not a service, never started at logon, never linked into
// exosnap.exe. The PowerShell release-verification runner owns the test itself:
// this executable only moves the machine into a known state, hands back a
// transaction id and a journal path, and puts everything back when the runner
// calls `restore` from its `finally`.
//
// Every subcommand prints ONE JSON document to stdout and exits non-zero on
// failure, so the runner never parses prose.
//
// Exit codes
//   0  ok
//   1  failure
//   2  usage error
//   3  blocked: a dirty journal must be recovered before anything may mutate
//   4  restore failed, or a restore is still owed (device unavailable)

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "env_alias.h"
#include "env_catalogue.h"
#include "env_journal.h"
#include "env_transaction.h"
#include "env_win32_provider.h"

namespace {

using namespace exosnap::envctl;
using exosnap::envctl::win32::Win32EnvironmentProvider;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;
constexpr int kExitBlocked = 3;
constexpr int kExitRestoreOwed = 4;

struct Options {
    std::string command;
    std::string profile_path;
    std::string journal_path;
    std::string scenario;
    std::string run_id;
    std::string desired_path;
    std::string alias;
    std::string stable_id;
    std::string kind;
    std::vector<std::string> aliases;
    long long guard_pid{0};
};

std::string EnvOrEmpty(const char* name) {
    char* buffer = nullptr;
    size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
        return {};
    }
    std::string value(buffer);
    free(buffer);
    return value;
}

std::vector<std::string> SplitCommaList(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char character : text) {
        if (character == ',') {
            if (!current.empty()) {
                parts.push_back(current);
            }
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

void Print(const nlohmann::ordered_json& document) {
    std::fputs((document.dump(2) + "\n").c_str(), stdout);
}

int Fail(const std::string& command, const std::string& code, const std::string& message, int exit_code) {
    nlohmann::ordered_json document;
    document["ok"] = false;
    document["command"] = command;
    document["errorCode"] = code;
    document["error"] = message;
    Print(document);
    return exit_code;
}

nlohmann::ordered_json EvidenceJson(const TransactionEvidence& evidence) {
    return nlohmann::ordered_json::parse(evidence.ToJsonText());
}

nlohmann::ordered_json PendingJson(const std::vector<DevicePendingRestore>& pending) {
    nlohmann::ordered_json array = nlohmann::ordered_json::array();
    for (const auto& entry : pending) {
        nlohmann::ordered_json item;
        item["alias"] = entry.alias;
        item["stableId"] = entry.stable_id;
        item["friendlyName"] = entry.friendly_name;
        item["property"] = entry.property;
        item["originalValue"] = entry.original_value;
        item["remainingAction"] = entry.remaining_action;
        array.push_back(item);
    }
    return array;
}

TransactionConfig MakeConfig(const Options& options, const AliasProfile& profile) {
    TransactionConfig config;
    config.journal_path = options.journal_path;
    config.run_id = options.run_id;
    config.scenario = options.scenario;
    config.owner_pid = static_cast<long long>(GetCurrentProcessId());
    config.alias_lookup = [&profile](const std::string& alias) { return profile.Find(alias); };
    return config;
}

// ------------------------------------------------------------------- describe

int CommandDescribe(const Win32EnvironmentProvider& provider) {
    nlohmann::ordered_json document;
    document["ok"] = true;
    document["command"] = "describe";

    nlohmann::ordered_json catalogue = nlohmann::ordered_json::array();
    for (const auto& entry : WindowsCapabilityCatalogue()) {
        nlohmann::ordered_json item;
        item["deviceKind"] = entry.device_kind;
        item["property"] = entry.property;
        item["capability"] = std::string(ToKey(entry.capability));
        item["valueKind"] = entry.value_kind;
        item["readMechanism"] = entry.read_mechanism;
        item["mutateMechanism"] = entry.mutate_mechanism;
        item["note"] = entry.note;
        catalogue.push_back(item);
    }
    document["catalogue"] = catalogue;

    nlohmann::ordered_json properties = nlohmann::ordered_json::array();
    for (const auto& descriptor : provider.Describe()) {
        nlohmann::ordered_json item;
        item["alias"] = descriptor.id.device_alias;
        item["property"] = descriptor.id.property;
        item["key"] = descriptor.id.Key();
        item["capability"] = std::string(ToKey(descriptor.capability));
        item["valueKind"] = descriptor.kind;
        item["readMechanism"] = descriptor.read_mechanism;
        item["mutateMechanism"] = descriptor.mutate_mechanism;
        item["note"] = descriptor.note;
        properties.push_back(item);
    }
    document["properties"] = properties;
    Print(document);
    return kExitOk;
}

// ------------------------------------------------------------------- snapshot

int CommandSnapshot(const Win32EnvironmentProvider& provider, const Options& options) {
    nlohmann::ordered_json document;
    document["ok"] = true;
    document["command"] = "snapshot";
    document["machine"] = StableMachineHash();
    document["capturedAtUtc"] = NowUtcIso8601();

    nlohmann::ordered_json properties = nlohmann::ordered_json::array();
    bool all_ok = true;
    for (const auto& descriptor : provider.Describe()) {
        if (!options.aliases.empty()) {
            bool wanted = false;
            for (const auto& alias : options.aliases) {
                if (alias == descriptor.id.device_alias) {
                    wanted = true;
                }
            }
            if (!wanted) {
                continue;
            }
        }
        const ReadResult read = provider.Read(descriptor.id);
        nlohmann::ordered_json item;
        item["key"] = descriptor.id.Key();
        item["alias"] = descriptor.id.device_alias;
        item["property"] = descriptor.id.property;
        item["capability"] = std::string(ToKey(descriptor.capability));
        item["valueKind"] = descriptor.kind;
        item["ok"] = read.ok;
        item["devicePresent"] = read.device_present;
        item["value"] = read.value;
        item["error"] = read.error;
        properties.push_back(item);
        if (!read.ok) {
            all_ok = false;
        }
    }
    document["properties"] = properties;
    document["allReadsOk"] = all_ok;
    Print(document);
    return kExitOk;
}

// ------------------------------------------------------------------ list-modes
//
// The vocabulary a refresh-rate transaction may speak on THIS machine.
//
// A caller must be able to say "any supported rate other than the current one"
// without hardcoding a number for one desk, because a number from a datasheet is
// not necessarily a number Windows will ever report back: ChangeDisplaySettingsEx
// accepts a nominal 60 for a 59.94 Hz mode, and the read-back then says 59. Every
// refresh rate below is EnumDisplaySettingsEx's own integer, unrounded -- that is
// the whole point of the list.

nlohmann::ordered_json ModeJson(const DisplayModeFacts& mode) {
    nlohmann::ordered_json item;
    item["width"] = mode.width;
    item["height"] = mode.height;
    item["refreshHz"] = mode.refresh_hz;
    item["bitsPerPixel"] = mode.bits_per_pixel;
    item["orientation"] = mode.orientation_degrees;
    item["mode"] = FormatModeFacts(mode);
    return item;
}

int CommandListModes(const Win32EnvironmentProvider& provider, const Options& options) {
    if (!options.kind.empty() && options.kind != device_kind::kDisplay) {
        return Fail("list-modes", "usage", "list-modes only applies to displays; --kind must be 'display'.",
                    kExitUsage);
    }

    std::vector<AliasBinding> selected;
    for (const auto& binding : provider.Profile().Bindings()) {
        if (binding.kind != device_kind::kDisplay) {
            continue;
        }
        if (!options.alias.empty() && binding.alias != options.alias) {
            continue;
        }
        selected.push_back(binding);
    }
    if (!options.alias.empty() && selected.empty()) {
        // Either unbound, or bound to something that has no display modes. Both
        // are the operator's to fix, and the message says which.
        const auto binding = provider.Profile().Find(options.alias);
        if (!binding.has_value()) {
            return Fail("list-modes", std::string(ToKey(AliasErrorCode::UnboundAlias)),
                        UnboundAliasInstruction(options.alias), kExitFailure);
        }
        return Fail("list-modes", "wrong_kind",
                    "'" + options.alias + "' is bound as kind '" + binding->kind + "', not 'display'.", kExitFailure);
    }

    nlohmann::ordered_json document;
    nlohmann::ordered_json displays = nlohmann::ordered_json::array();
    bool ok = true;
    for (const auto& binding : selected) {
        nlohmann::ordered_json entry;
        entry["alias"] = binding.alias;
        entry["stableId"] = binding.stable_id;

        const auto* target = provider.FindDisplay(binding.alias);
        if (target == nullptr) {
            entry["gdiName"] = "";
            entry["error"] = "no display matches stable id '" + binding.stable_id + "' (or it matches more than one)";
            entry["modes"] = nlohmann::ordered_json::array();
            displays.push_back(entry);
            ok = false;
            continue;
        }
        entry["gdiName"] = target->gdi_name;

        const auto list = win32::EnumerateModes(*target);
        if (!list.ok) {
            entry["error"] = list.error;
            entry["modes"] = nlohmann::ordered_json::array();
            displays.push_back(entry);
            ok = false;
            continue;
        }
        entry["current"] = ModeJson(list.current);
        nlohmann::ordered_json modes = nlohmann::ordered_json::array();
        for (const auto& mode : list.candidates) {
            modes.push_back(ModeJson(mode));
        }
        entry["modes"] = modes;
        displays.push_back(entry);
    }

    document["ok"] = ok;
    document["command"] = "list-modes";
    document["displays"] = displays;
    Print(document);
    return ok ? kExitOk : kExitFailure;
}

// -------------------------------------------------------------- alias binding

int CommandResolveAliases(const Win32EnvironmentProvider& provider, const Options& options) {
    const auto candidates = provider.Inventory();
    const auto reports = provider.Profile().ValidateAgainstInventory(candidates);

    nlohmann::ordered_json document;
    document["command"] = "resolve-aliases";
    document["profilePath"] = options.profile_path;
    if (!provider.InventoryError().empty()) {
        document["inventoryError"] = provider.InventoryError();
    }

    nlohmann::ordered_json bindings = nlohmann::ordered_json::array();
    bool ok = true;
    for (const auto& report : reports) {
        nlohmann::ordered_json item;
        item["alias"] = report.binding.alias;
        item["kind"] = report.binding.kind;
        item["stableId"] = report.binding.stable_id;
        item["friendlyName"] = report.binding.friendly_name;
        item["currentFriendlyName"] = report.current_friendly_name;
        item["boundAtUtc"] = report.binding.bound_at_utc;
        item["status"] = std::string(ToKey(report.status));
        bindings.push_back(item);
        if (report.status == BindingStatus::DeviceNotPresent || report.status == BindingStatus::Ambiguous) {
            ok = false;
        }
    }
    document["bindings"] = bindings;

    nlohmann::ordered_json inventory = nlohmann::ordered_json::array();
    for (const auto& candidate : candidates) {
        nlohmann::ordered_json item;
        item["kind"] = candidate.kind;
        item["stableId"] = candidate.stable_id;
        item["friendlyName"] = candidate.friendly_name;
        item["detail"] = candidate.detail;
        inventory.push_back(item);
    }
    document["candidates"] = inventory;

    nlohmann::ordered_json errors = nlohmann::ordered_json::array();
    if (!options.aliases.empty()) {
        const auto resolved = provider.Profile().Resolve(options.aliases);
        for (const auto& error : resolved.errors) {
            nlohmann::ordered_json item;
            item["code"] = std::string(ToKey(error.code));
            item["alias"] = error.alias;
            item["message"] = error.message;
            errors.push_back(item);
        }
        if (!resolved.ok) {
            ok = false;
        }
    }
    document["errors"] = errors;
    document["ok"] = ok;
    Print(document);
    return ok ? kExitOk : kExitFailure;
}

int CommandBindAlias(const Win32EnvironmentProvider& provider, const Options& options) {
    if (options.alias.empty() || options.stable_id.empty()) {
        return Fail("bind-alias", "usage", "bind-alias requires --alias <name> and --stable-id <id>.", kExitUsage);
    }
    const auto candidates = provider.Inventory();
    const auto outcome = BindByStableId(options.alias, options.kind, options.stable_id, candidates, NowUtcIso8601());
    if (!outcome.ok) {
        return Fail("bind-alias", std::string(ToKey(outcome.error.code)), outcome.error.message, kExitFailure);
    }

    AliasProfile profile = provider.Profile();
    profile.Upsert(outcome.binding);

    const std::filesystem::path path(options.profile_path);
    std::error_code code;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), code);
    }
    std::string error;
    if (!profile.SaveToFile(options.profile_path, error)) {
        return Fail("bind-alias", "profile_write_failed", error, kExitFailure);
    }

    nlohmann::ordered_json document;
    document["ok"] = true;
    document["command"] = "bind-alias";
    document["profilePath"] = options.profile_path;
    nlohmann::ordered_json binding;
    binding["alias"] = outcome.binding.alias;
    binding["kind"] = outcome.binding.kind;
    binding["stableId"] = outcome.binding.stable_id;
    binding["friendlyName"] = outcome.binding.friendly_name;
    binding["boundAtUtc"] = outcome.binding.bound_at_utc;
    document["binding"] = binding;
    Print(document);
    return kExitOk;
}

// ---------------------------------------------------------------- transaction

std::optional<std::map<std::string, std::string>> LoadDesired(const std::string& path, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "could not open desired-state file '" + path + "'.";
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    auto document = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        error = "desired-state file '" + path + "' is not a JSON object.";
        return std::nullopt;
    }
    const auto node = document.contains("desired") ? document["desired"] : document;
    if (!node.is_object()) {
        error = "desired-state 'desired' must be an object of \"<alias>:<property>\": \"<value>\".";
        return std::nullopt;
    }
    std::map<std::string, std::string> desired;
    for (const auto& [key, value] : node.items()) {
        if (!value.is_string()) {
            error = "desired value for '" + key + "' must be a string (values are strings for journal fidelity).";
            return std::nullopt;
        }
        desired[key] = value.get<std::string>();
    }
    return desired;
}

int CommandBegin(Win32EnvironmentProvider& provider, const Options& options) {
    if (options.scenario.empty() || options.run_id.empty() || options.desired_path.empty()) {
        return Fail("begin", "usage", "begin requires --scenario <s> --run-id <r> --desired <file.json>.", kExitUsage);
    }

    std::string error;
    const auto desired = LoadDesired(options.desired_path, error);
    if (!desired.has_value()) {
        return Fail("begin", "desired_invalid", error, kExitUsage);
    }

    // The startup gate is not advisory. A dirty journal blocks every mutating
    // transaction until `recover` has restored and verified.
    auto gate = RecoverIfDirty(provider, MakeConfig(options, provider.Profile()));
    if (!gate.mutation_allowed) {
        nlohmann::ordered_json document;
        document["ok"] = false;
        document["command"] = "begin";
        document["errorCode"] = gate.error_code.empty() ? std::string(error_code::kDirtyJournal) : gate.error_code;
        document["error"] =
            gate.error.empty() ? "a dirty journal must be recovered before anything may mutate." : gate.error;
        document["journalPath"] = options.journal_path;
        document["state"] = std::string(ToKey(gate.state));
        document["pending"] = PendingJson(gate.pending);
        Print(document);
        return kExitBlocked;
    }

    EnvironmentTransaction transaction(provider, MakeConfig(options, provider.Profile()));
    const auto result = transaction.Begin(*desired);

    nlohmann::ordered_json document;
    document["ok"] = result.ok;
    document["command"] = "begin";
    document["transactionId"] = transaction.JournalDocument().transaction_id;
    document["journalPath"] = transaction.JournalPath().string();
    document["state"] = std::string(ToKey(result.state));
    document["runId"] = options.run_id;
    document["scenario"] = options.scenario;
    document["ownerPid"] = transaction.JournalDocument().owner_pid;
    if (!result.ok) {
        document["errorCode"] = result.error_code;
        document["error"] = result.error;
    }
    nlohmann::ordered_json applied = nlohmann::ordered_json::array();
    for (const auto& entry : transaction.JournalDocument().applied) {
        nlohmann::ordered_json item;
        item["property"] = entry.property;
        item["from"] = entry.from;
        item["to"] = entry.to;
        item["atUtc"] = entry.at_utc;
        applied.push_back(item);
    }
    document["applied"] = applied;
    document["evidence"] = EvidenceJson(transaction.Evidence());
    document["pending"] = PendingJson(result.pending);
    Print(document);

    if (result.ok) {
        return kExitOk;
    }
    return result.state == TransactionState::RestoreFailed ||
                   result.state == TransactionState::RestorePendingDeviceUnavailable
               ? kExitRestoreOwed
               : kExitFailure;
}

int CommandRestore(Win32EnvironmentProvider& provider, const Options& options) {
    const auto outcome = RecoverIfDirty(provider, MakeConfig(options, provider.Profile()));

    nlohmann::ordered_json document;
    document["command"] = "restore";
    document["journalPath"] = options.journal_path;
    document["journalPresent"] = outcome.journal_present;
    document["state"] = std::string(ToKey(outcome.state));
    document["pending"] = PendingJson(outcome.pending);
    document["evidence"] = EvidenceJson(outcome.evidence);
    if (!outcome.journal_present) {
        // Idempotent by construction: `restore` from a `finally` after a failed
        // `begin` that already rolled back is a successful no-op.
        document["ok"] = true;
        document["note"] = "no journal; nothing was owed.";
        Print(document);
        return kExitOk;
    }
    document["ok"] = outcome.recovered;
    if (!outcome.recovered) {
        document["errorCode"] = outcome.error_code;
        document["error"] = outcome.error;
    }
    Print(document);
    return outcome.recovered ? kExitOk : kExitRestoreOwed;
}

int CommandRecover(Win32EnvironmentProvider& provider, const Options& options) {
    const auto outcome = RecoverIfDirty(provider, MakeConfig(options, provider.Profile()));

    nlohmann::ordered_json document;
    document["ok"] = outcome.mutation_allowed;
    document["command"] = "recover";
    document["journalPath"] = options.journal_path;
    document["journalPresent"] = outcome.journal_present;
    document["recovered"] = outcome.recovered;
    document["mutationAllowed"] = outcome.mutation_allowed;
    document["state"] = std::string(ToKey(outcome.state));
    document["pending"] = PendingJson(outcome.pending);
    document["evidence"] = EvidenceJson(outcome.evidence);
    if (!outcome.error.empty()) {
        document["errorCode"] = outcome.error_code;
        document["error"] = outcome.error;
    }
    Print(document);
    return outcome.mutation_allowed ? kExitOk : kExitRestoreOwed;
}

int CommandStatus(const Options& options) {
    std::string error;
    const auto journal = ReadJournal(options.journal_path, error);

    nlohmann::ordered_json document;
    document["command"] = "status";
    document["journalPath"] = options.journal_path;
    document["journalPresent"] = journal.has_value() || !error.empty();
    if (!journal.has_value()) {
        document["ok"] = error.empty();
        document["state"] = std::string(ToKey(TransactionState::Clean));
        document["mutationAllowed"] = error.empty();
        if (!error.empty()) {
            document["errorCode"] = std::string(error_code::kJournalReadFailed);
            document["error"] = error;
        }
        Print(document);
        return error.empty() ? kExitOk : kExitBlocked;
    }

    document["ok"] = true;
    document["state"] = std::string(ToKey(journal->state));
    document["mutationAllowed"] = !IsDirty(journal->state);
    document["transactionId"] = journal->transaction_id;
    document["runId"] = journal->run_id;
    document["scenario"] = journal->scenario;
    document["ownerPid"] = journal->owner_pid;
    document["machine"] = journal->machine;
    document["createdAt"] = journal->created_at;
    document["updatedAt"] = journal->updated_at;

    nlohmann::ordered_json original = nlohmann::ordered_json::object();
    for (const auto& [key, value] : journal->original) {
        original[key] = value;
    }
    document["original"] = original;
    nlohmann::ordered_json desired = nlohmann::ordered_json::object();
    for (const auto& [key, value] : journal->desired) {
        desired[key] = value;
    }
    document["desired"] = desired;
    nlohmann::ordered_json applied = nlohmann::ordered_json::array();
    for (const auto& entry : journal->applied) {
        nlohmann::ordered_json item;
        item["property"] = entry.property;
        item["from"] = entry.from;
        item["to"] = entry.to;
        item["atUtc"] = entry.at_utc;
        applied.push_back(item);
    }
    document["applied"] = applied;
    Print(document);
    return kExitOk;
}

// ------------------------------------------------------------------- guardian
//
// Test-only, spawned BY the runner, never installed and never autostarted. It
// waits on the owner process handle and, if the owner dies while a journal is
// dirty, restores from that journal. It is a convenience on top of the real
// guarantee -- persistent startup recovery -- not a replacement for it: a
// machine that loses power still recovers on the next `recover`.
int CommandGuard(Win32EnvironmentProvider& provider, const Options& options) {
    if (options.guard_pid <= 0) {
        return Fail("guard", "usage", "--guard requires the owner process id.", kExitUsage);
    }
    HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(options.guard_pid));
    if (owner != nullptr) {
        WaitForSingleObject(owner, INFINITE);
        CloseHandle(owner);
    }
    // Owner gone (or never openable): whatever the journal still owes is now
    // this process's problem.
    provider.Refresh();
    const auto outcome = RecoverIfDirty(provider, MakeConfig(options, provider.Profile()));

    nlohmann::ordered_json document;
    document["ok"] = outcome.mutation_allowed;
    document["command"] = "guard";
    document["ownerPid"] = options.guard_pid;
    document["journalPath"] = options.journal_path;
    document["journalPresent"] = outcome.journal_present;
    document["recovered"] = outcome.recovered;
    document["state"] = std::string(ToKey(outcome.state));
    document["pending"] = PendingJson(outcome.pending);
    Print(document);
    return outcome.mutation_allowed ? kExitOk : kExitRestoreOwed;
}

const char* kUsage = "exosnap-envctl (TEST-ONLY; never installed, never a service)\n"
                     "  describe                                       capability classification table\n"
                     "  snapshot [--aliases a,b]                       read every bound property\n"
                     "  list-modes [--alias X] [--kind display]        display modes a refresh-hz change may target\n"
                     "  resolve-aliases [--aliases a,b]                bindings + candidate stable ids\n"
                     "  bind-alias --alias X --stable-id Y [--kind K]  one-time binding step\n"
                     "  begin --scenario S --run-id R --desired f.json start a transaction (does NOT run a test)\n"
                     "  restore [--journal p]                          put the exact original back (idempotent)\n"
                     "  recover [--journal p]                          startup recovery gate\n"
                     "  status [--journal p]                           what the journal says is owed\n"
                     "  --guard <owner-pid>                            wait on the owner and restore if it dies\n"
                     "Common: --profile <path> (default .workspace/env-profile.json, EXOSNAP_ENV_PROFILE)\n"
                     "        --journal <path> (default .workspace/env-journal.json, EXOSNAP_ENV_JOURNAL)\n";

} // namespace

int main(int argc, char** argv) {
    Options options;
    options.profile_path = EnvOrEmpty("EXOSNAP_ENV_PROFILE");
    if (options.profile_path.empty()) {
        options.profile_path = AliasProfile::DefaultProfileRelativePath();
    }
    options.journal_path = EnvOrEmpty("EXOSNAP_ENV_JOURNAL");
    if (options.journal_path.empty()) {
        options.journal_path = ".workspace/env-journal.json";
    }

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto next = [&](const char* name) -> std::string {
            if (index + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                return {};
            }
            return argv[++index];
        };
        if (argument == "--help" || argument == "-h") {
            std::fputs(kUsage, stdout);
            return kExitOk;
        }
        if (argument == "--profile") {
            options.profile_path = next("--profile");
        } else if (argument == "--journal") {
            options.journal_path = next("--journal");
        } else if (argument == "--scenario") {
            options.scenario = next("--scenario");
        } else if (argument == "--run-id") {
            options.run_id = next("--run-id");
        } else if (argument == "--desired") {
            options.desired_path = next("--desired");
        } else if (argument == "--alias") {
            options.alias = next("--alias");
        } else if (argument == "--stable-id") {
            options.stable_id = next("--stable-id");
        } else if (argument == "--kind") {
            options.kind = next("--kind");
        } else if (argument == "--aliases") {
            options.aliases = SplitCommaList(next("--aliases"));
        } else if (argument == "--guard") {
            options.command = "guard";
            options.guard_pid = std::strtoll(next("--guard").c_str(), nullptr, 10);
        } else if (!argument.empty() && argument.front() == '-') {
            std::fprintf(stderr, "unknown option '%s'\n", argument.c_str());
            std::fputs(kUsage, stderr);
            return kExitUsage;
        } else if (options.command.empty()) {
            options.command = argument;
        } else {
            std::fprintf(stderr, "unexpected argument '%s'\n", argument.c_str());
            return kExitUsage;
        }
    }

    if (options.command.empty()) {
        std::fputs(kUsage, stderr);
        return kExitUsage;
    }

    const auto load = AliasProfile::LoadFromFile(options.profile_path);
    if (!load.ok) {
        return Fail(options.command, std::string(ToKey(load.error.code)), load.error.message, kExitFailure);
    }
    Win32EnvironmentProvider provider(load.profile);

    if (options.command == "describe") {
        return CommandDescribe(provider);
    }
    if (options.command == "snapshot") {
        return CommandSnapshot(provider, options);
    }
    if (options.command == "list-modes") {
        return CommandListModes(provider, options);
    }
    if (options.command == "resolve-aliases") {
        return CommandResolveAliases(provider, options);
    }
    if (options.command == "bind-alias") {
        return CommandBindAlias(provider, options);
    }
    if (options.command == "begin") {
        return CommandBegin(provider, options);
    }
    if (options.command == "restore") {
        return CommandRestore(provider, options);
    }
    if (options.command == "recover") {
        return CommandRecover(provider, options);
    }
    if (options.command == "status") {
        return CommandStatus(options);
    }
    if (options.command == "guard") {
        return CommandGuard(provider, options);
    }

    std::fprintf(stderr, "unknown command '%s'\n", options.command.c_str());
    std::fputs(kUsage, stderr);
    return kExitUsage;
}

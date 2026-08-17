#include "env_transaction.h"

#include <algorithm>
#include <set>
#include <utility>

namespace exosnap::envctl {
namespace {

std::string CompactTimestamp(const std::string& iso) {
    std::string compact;
    compact.reserve(iso.size());
    for (const char character : iso) {
        if (character != '-' && character != ':' && character != 'Z') {
            compact.push_back(character);
        }
    }
    return compact;
}

} // namespace

EnvironmentTransaction::EnvironmentTransaction(IEnvironmentProvider& provider, TransactionConfig config)
    : provider_(provider), config_(std::move(config)) {
    if (!config_.clock) {
        config_.clock = &NowUtcIso8601;
    }
    if (config_.machine.empty()) {
        config_.machine = StableMachineHash();
    }
    if (config_.transaction_id.empty()) {
        const std::string now = config_.clock();
        config_.transaction_id =
            "tx-" + CompactTimestamp(now) + "-" +
            MachineHashFromSeed(now + "|" + config_.run_id + "|" + config_.scenario + "|" + config_.machine)
                .substr(0, 8);
    }
    journal_.transaction_id = config_.transaction_id;
    journal_.owner_pid = config_.owner_pid;
    journal_.run_id = config_.run_id;
    journal_.machine = config_.machine;
    journal_.scenario = config_.scenario;
    journal_.state = TransactionState::Clean;
}

TransactionResult EnvironmentTransaction::Fail(const char* code, std::string message) {
    TransactionResult result;
    result.ok = false;
    result.state = journal_.state;
    result.error_code = code;
    result.error = std::move(message);
    return result;
}

bool EnvironmentTransaction::PersistJournal(TransactionState state, std::string& error) {
    journal_.state = state;
    journal_.updated_at = config_.clock();
    if (journal_.created_at.empty()) {
        journal_.created_at = journal_.updated_at;
    }
    return WriteJournalAtomic(config_.journal_path, journal_, error);
}

void EnvironmentTransaction::AdoptJournal(Journal journal) {
    journal_ = std::move(journal);
    config_.transaction_id = journal_.transaction_id;
    config_.run_id = journal_.run_id;
    config_.scenario = journal_.scenario;
    config_.machine = journal_.machine;
    terminal_.reset();

    evidence_.clear();
    for (const auto& [key, before] : journal_.original) {
        PropertyEvidence evidence;
        evidence.property = key;
        evidence.before = before;
        const auto desired = journal_.desired.find(key);
        evidence.requested = desired != journal_.desired.end() ? desired->second : before;
        evidence.skipped = true;
        evidence_.emplace(key, evidence);
    }
    for (const auto& entry : journal_.applied) {
        auto& evidence = evidence_[entry.property];
        evidence.property = entry.property;
        if (evidence.before.empty()) {
            evidence.before = entry.from;
        }
        if (evidence.requested.empty()) {
            evidence.requested = entry.to;
        }
        evidence.applied = entry.to;
        evidence.skipped = false;
    }
}

TransactionResult EnvironmentTransaction::Begin(const std::map<std::string, std::string>& desired) {
    // --- Hard gate: an existing journal means this machine is not at its
    // original settings, so there is no honest "original" left to snapshot. ---
    {
        std::string read_error;
        const auto existing = ReadJournal(config_.journal_path, read_error);
        if (existing.has_value() || !read_error.empty()) {
            return Fail(error_code::kDirtyJournal,
                        "dirty_journal: '" + config_.journal_path.string() +
                            "' already exists. The machine may still be off its original settings. Run "
                            "`exosnap-envctl recover --journal " +
                            config_.journal_path.string() +
                            "` first; no new transaction may mutate anything until "
                            "that has restored and verified.");
        }
    }

    // --- Snapshot the original BEFORE anything is written or changed. ---
    journal_.original.clear();
    journal_.desired.clear();
    journal_.applied.clear();
    evidence_.clear();

    std::vector<std::pair<PropertyId, std::string>> requests;
    for (const auto& [key, value] : desired) {
        const auto id = PropertyIdFromKey(key);
        if (!id.has_value()) {
            return Fail(error_code::kBadPropertyKey,
                        "bad_property_key: '" + key + "' is not '<device-alias>:<property>'.");
        }
        const ReadResult read = provider_.Read(*id);
        if (!read.device_present) {
            return Fail(error_code::kDeviceNotPresent,
                        "device_not_present: alias '" + id->device_alias + "' resolves to no device attached now.");
        }
        if (!read.ok) {
            return Fail(error_code::kReadFailed, "read_failed: '" + key + "': " + read.error);
        }
        journal_.original[key] = read.value;
        journal_.desired[key] = value;
        PropertyEvidence evidence;
        evidence.property = key;
        evidence.before = read.value;
        evidence.requested = value;
        evidence.skipped = true;
        evidence_.emplace(key, evidence);
        requests.emplace_back(*id, value);
    }

    // --- Journal on disk before the first mutation. ---
    {
        std::string error;
        if (!PersistJournal(TransactionState::Prepared, error)) {
            return Fail(error_code::kJournalWriteFailed, error);
        }
    }

    // --- Validate the desired state. Only a documented, reversible setter is
    // ever used; anything else is the operator's job by policy, not a gap to be
    // worked around with an undocumented API. ---
    {
        std::map<std::string, PropertyDescriptor> descriptors;
        for (const auto& descriptor : provider_.Describe()) {
            descriptors.emplace(descriptor.id.Key(), descriptor);
        }
        for (const auto& [id, value] : requests) {
            (void)value;
            const auto key = id.Key();
            const auto descriptor = descriptors.find(key);
            if (descriptor == descriptors.end()) {
                std::string ignored;
                DeleteJournal(config_.journal_path, ignored);
                journal_.state = TransactionState::Clean;
                return Fail(error_code::kUnknownProperty,
                            "unknown_property: '" + key + "' is not offered by the provider on this machine.");
            }
            if (!IsMutable(descriptor->second.capability)) {
                std::string ignored;
                DeleteJournal(config_.journal_path, ignored);
                journal_.state = TransactionState::Clean;
                return Fail(error_code::kNotMutable, "not_mutable: '" + key + "' is classified " +
                                                         std::string(ToKey(descriptor->second.capability)) +
                                                         "; envctl reads it and never writes it. " +
                                                         descriptor->second.mutate_mechanism);
            }
        }
    }

    // --- Minimal delta. A property already at its desired value is not touched
    // and produces no journal entry: nothing was changed, so nothing is owed. ---
    std::vector<std::pair<PropertyId, std::string>> delta;
    for (const auto& [id, value] : requests) {
        if (journal_.original[id.Key()] != value) {
            delta.emplace_back(id, value);
        }
    }

    if (delta.empty()) {
        std::string error;
        if (!PersistJournal(TransactionState::Active, error)) {
            return Fail(error_code::kJournalWriteFailed, error);
        }
        TransactionResult result;
        result.ok = true;
        result.state = journal_.state;
        return result;
    }

    {
        std::string error;
        if (!PersistJournal(TransactionState::Mutating, error)) {
            return Fail(error_code::kJournalWriteFailed, error);
        }
    }

    for (const auto& [id, value] : delta) {
        const auto key = id.Key();
        const std::string original = journal_.original[key];

        // Record the obligation BEFORE the setter runs. From the instant Apply()
        // is entered the machine may already have moved -- including when the
        // call then reports failure, lands on a value nobody asked for, or the
        // process dies inside it. A restore obligation that only appears after a
        // SUCCESSFUL verification is exactly the hole through which a partially
        // effective apply escapes unrecorded.
        //
        // Nothing is lost by over-recording: the restore re-reads first and pops
        // an entry that is already at its original value without touching it.
        journal_.applied.push_back({key, original, value, config_.clock()});
        evidence_[key].skipped = false;
        {
            std::string error;
            if (!PersistJournal(TransactionState::Mutating, error)) {
                journal_.applied.pop_back();
                evidence_[key].skipped = true;
                const auto rollback = RestoreInternal();
                auto result = Fail(error_code::kJournalWriteFailed, error);
                result.state = rollback.state;
                result.pending = rollback.pending;
                return result;
            }
        }

        const ApplyResult applied = provider_.Apply(id, value);
        if (!applied.accepted) {
            const auto rollback = RestoreInternal();
            auto result = Fail(error_code::kApplyRejected, "apply_rejected: '" + key + "': " + applied.error);
            result.state = rollback.state;
            result.pending = rollback.pending;
            return result;
        }

        // `accepted` is a claim, not evidence. Re-read through exactly the path
        // that produced the snapshot and compare.
        const ReadResult verify = provider_.Read(id);
        evidence_[key].applied = verify.value;
        if (!verify.ok) {
            const auto rollback = RestoreInternal();
            auto result = Fail(error_code::kReadFailed, "read_failed: '" + key + "' after apply: " + verify.error);
            result.state = rollback.state;
            result.pending = rollback.pending;
            return result;
        }
        if (verify.value != value) {
            const auto rollback = RestoreInternal();
            auto result =
                Fail(error_code::kVerifyMismatch, "verify_mismatch: '" + key + "' reported success but reads back '" +
                                                      verify.value + "', not '" + value + "'.");
            result.state = rollback.state;
            result.pending = rollback.pending;
            return result;
        }

        // Applied AND verified: refresh the journal so updatedAt marks the last
        // point at which the machine was known-good.
        std::string error;
        if (!PersistJournal(TransactionState::Mutating, error)) {
            const auto rollback = RestoreInternal();
            auto result = Fail(error_code::kJournalWriteFailed, error);
            result.state = rollback.state;
            result.pending = rollback.pending;
            return result;
        }
    }

    {
        std::string error;
        if (!PersistJournal(TransactionState::Active, error)) {
            const auto rollback = RestoreInternal();
            auto result = Fail(error_code::kJournalWriteFailed, error);
            result.state = rollback.state;
            result.pending = rollback.pending;
            return result;
        }
    }

    TransactionResult result;
    result.ok = true;
    result.state = journal_.state;
    return result;
}

std::vector<DevicePendingRestore> EnvironmentTransaction::BuildPending(const std::string& missing_alias) const {
    std::vector<DevicePendingRestore> pending;
    // Every outstanding entry is still owed: the pre-flight ran BEFORE the first
    // restore call, so nothing at all was touched on this attempt.
    for (auto entry = journal_.applied.rbegin(); entry != journal_.applied.rend(); ++entry) {
        const auto id = PropertyIdFromKey(entry->property);
        DevicePendingRestore item;
        item.alias = id.has_value() ? id->device_alias : entry->property;
        item.property = entry->property;
        item.original_value = entry->from;
        if (config_.alias_lookup) {
            if (const auto binding = config_.alias_lookup(item.alias); binding.has_value()) {
                item.stable_id = binding->stable_id;
                item.friendly_name = binding->friendly_name;
            }
        }
        if (item.alias == missing_alias) {
            item.remaining_action =
                "Reattach the device bound to alias '" + item.alias + "'" +
                (item.stable_id.empty() ? std::string{} : " (stable id " + item.stable_id + ")") +
                (item.friendly_name.empty() ? std::string{} : ", last seen as '" + item.friendly_name + "'") +
                ", then run `exosnap-envctl recover --journal " + config_.journal_path.string() + "` to set " +
                item.property + " back to '" + item.original_value + "'.";
        } else {
            item.remaining_action = "Untouched on this attempt. Run `exosnap-envctl recover --journal " +
                                    config_.journal_path.string() + "` once alias '" + missing_alias +
                                    "' is attached again; it will set " + item.property + " back to '" +
                                    item.original_value + "'.";
        }
        pending.push_back(item);
    }
    return pending;
}

TransactionResult EnvironmentTransaction::RestoreInternal() {
    if (journal_.applied.empty()) {
        std::string error;
        DeleteJournal(config_.journal_path, error);
        journal_.state = TransactionState::Restored;
        for (auto& [key, evidence] : evidence_) {
            (void)key;
            if (evidence.skipped) {
                evidence.after_restore = evidence.before;
            }
        }
        TransactionResult result;
        result.ok = true;
        result.state = journal_.state;
        return result;
    }

    {
        std::string error;
        if (!PersistJournal(TransactionState::Restoring, error)) {
            return Fail(error_code::kJournalWriteFailed, error);
        }
    }

    // --- Pre-flight. Every device that is owed a value must be present BEFORE
    // the first restore call, so a missing device can never leave the machine
    // half-restored across two devices. ---
    {
        std::set<std::string> aliases;
        for (const auto& entry : journal_.applied) {
            if (const auto id = PropertyIdFromKey(entry.property); id.has_value()) {
                aliases.insert(id->device_alias);
            }
        }
        for (const auto& alias : aliases) {
            if (provider_.DevicePresent(alias)) {
                continue;
            }
            auto pending = BuildPending(alias);
            std::string error;
            PersistJournal(TransactionState::RestorePendingDeviceUnavailable, error);
            TransactionResult result;
            result.ok = false;
            result.state = journal_.state;
            result.error_code = error_code::kDeviceNotPresent;
            result.error = "device_not_present: alias '" + alias +
                           "' is gone; the restore was not started and no other device was touched.";
            result.pending = std::move(pending);
            return result;
        }
    }

    // --- Reverse order of application. Entries are popped as they are restored
    // AND verified, so the journal always lists exactly what is still owed. ---
    while (!journal_.applied.empty()) {
        const AppliedEntry entry = journal_.applied.back();
        const auto id = PropertyIdFromKey(entry.property);
        if (!id.has_value()) {
            journal_.state = TransactionState::RestoreFailed;
            std::string error;
            PersistJournal(TransactionState::RestoreFailed, error);
            return Fail(error_code::kBadPropertyKey, "bad_property_key: '" + entry.property + "' in journal.");
        }

        auto& evidence = evidence_[entry.property];
        evidence.property = entry.property;
        if (evidence.before.empty()) {
            evidence.before = entry.from;
        }

        const ReadResult current = provider_.Read(*id);
        if (current.ok && current.value == entry.from) {
            // Already exactly where it started -- an interrupted earlier restore,
            // or a value the host reverted on its own. Verified, so nothing to
            // apply and nothing left to owe.
            evidence.after_restore = current.value;
            journal_.applied.pop_back();
            std::string error;
            if (!PersistJournal(TransactionState::Restoring, error)) {
                return Fail(error_code::kJournalWriteFailed, error);
            }
            continue;
        }

        const ApplyResult restored = provider_.Apply(*id, entry.from);
        if (!restored.accepted) {
            evidence.after_restore = current.ok ? current.value : std::string{};
            std::string error;
            PersistJournal(TransactionState::RestoreFailed, error);
            return Fail(error_code::kRestoreRejected,
                        "restore_rejected: '" + entry.property + "' -> '" + entry.from + "': " + restored.error);
        }

        const ReadResult verify = provider_.Read(*id);
        evidence.after_restore = verify.ok ? verify.value : std::string{};
        if (!verify.ok || verify.value != entry.from) {
            std::string error;
            PersistJournal(TransactionState::RestoreFailed, error);
            return Fail(error_code::kRestoreVerifyMismatch,
                        "restore_verify_mismatch: '" + entry.property + "' was set back to '" + entry.from +
                            "' and the setter reported success, but it reads back '" + evidence.after_restore + "'.");
        }

        journal_.applied.pop_back();
        std::string error;
        if (!PersistJournal(TransactionState::Restoring, error)) {
            return Fail(error_code::kJournalWriteFailed, error);
        }
    }

    for (auto& [key, evidence] : evidence_) {
        (void)key;
        if (evidence.skipped && evidence.after_restore.empty()) {
            evidence.after_restore = evidence.before;
        }
    }

    journal_.state = TransactionState::Restored;
    std::string error;
    DeleteJournal(config_.journal_path, error);
    TransactionResult result;
    result.ok = true;
    result.state = journal_.state;
    return result;
}

TransactionResult EnvironmentTransaction::Restore() {
    // Idempotent, because the runner calls this from a `finally` that also runs
    // after a failed Begin() already rolled back.
    if (terminal_.has_value()) {
        return *terminal_;
    }
    if (journal_.state == TransactionState::Clean || journal_.state == TransactionState::Restored) {
        TransactionResult result;
        result.ok = true;
        result.state = journal_.state;
        terminal_ = result;
        return result;
    }

    auto result = RestoreInternal();
    if (IsTerminal(result.state)) {
        terminal_ = result;
    }
    return result;
}

TransactionEvidence EnvironmentTransaction::Evidence() const {
    TransactionEvidence evidence;
    evidence.transaction_id = journal_.transaction_id;
    evidence.run_id = journal_.run_id;
    evidence.scenario = journal_.scenario;
    evidence.machine = journal_.machine;
    evidence.final_state = std::string(ToKey(journal_.state));
    for (const auto& [key, entry] : evidence_) {
        (void)key;
        evidence.properties.push_back(entry);
    }
    std::sort(evidence.properties.begin(), evidence.properties.end(),
              [](const PropertyEvidence& lhs, const PropertyEvidence& rhs) { return lhs.property < rhs.property; });
    return evidence;
}

RecoveryOutcome RecoverIfDirty(IEnvironmentProvider& provider, TransactionConfig config) {
    RecoveryOutcome outcome;

    std::string read_error;
    auto journal = ReadJournal(config.journal_path, read_error);
    if (!journal.has_value()) {
        if (!read_error.empty()) {
            // A journal that exists but cannot be parsed is the WORST case: the
            // machine may be mutated and we cannot say how. Never report clean.
            outcome.journal_present = true;
            outcome.mutation_allowed = false;
            outcome.error_code = error_code::kJournalReadFailed;
            outcome.error = read_error + " Resolve it by hand; envctl will not mutate anything until it is gone.";
            return outcome;
        }
        outcome.journal_present = false;
        outcome.recovered = false;
        outcome.mutation_allowed = true;
        outcome.state = TransactionState::Clean;
        return outcome;
    }

    outcome.journal_present = true;
    if (!IsDirty(journal->state)) {
        std::string error;
        DeleteJournal(config.journal_path, error);
        outcome.recovered = true;
        outcome.mutation_allowed = true;
        outcome.state = TransactionState::Restored;
        return outcome;
    }

    EnvironmentTransaction transaction(provider, std::move(config));
    transaction.AdoptJournal(*journal);
    const auto result = transaction.Restore();
    outcome.state = result.state;
    outcome.error_code = result.error_code;
    outcome.error = result.error;
    outcome.pending = result.pending;
    outcome.evidence = transaction.Evidence();
    outcome.recovered = result.ok && result.state == TransactionState::Restored;
    outcome.mutation_allowed = outcome.recovered;
    return outcome;
}

} // namespace exosnap::envctl

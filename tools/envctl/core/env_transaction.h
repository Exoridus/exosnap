// tools/envctl/core/env_transaction.h -- the mandatory sequence, as a state machine.
//
//   snapshot original
//     -> persist journal (Prepared)          <-- NOTHING is mutated before this succeeds
//     -> validate desired                    <-- only ENV_MUTATE_SAFE / ENV_MUTATE_TESTONLY
//     -> Mutating
//     -> apply MINIMAL DELTA                 <-- a property already at the desired value is not touched
//     -> read back -> compare                <-- a setter returning success proves nothing
//     -> journal update after each verified mutation
//     -> Active                              <-- the caller runs its test here
//     -> Restoring
//     -> restore the EXACT original, in reverse order of application
//     -> read back -> compare
//     -> Restored -> journal deleted
//
// Failure terminals:
//   * apply rejected, or applied-but-read-back-differs -> roll back what was
//     already applied. Terminal Restored if the rollback verifies, RestoreFailed
//     if it does not.
//   * restore setter succeeds but read-back differs -> RestoreFailed (terminal).
//   * a device owed a restore is gone -> RestorePendingDeviceUnavailable, and NO
//     other device is touched -- the pre-flight checks every device before the
//     first restore call, so a partial restore cannot start.
//
// Restore() is idempotent and safe to call from a `finally`.

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "env_alias.h"
#include "env_evidence.h"
#include "env_journal.h"
#include "env_provider.h"
#include "env_types.h"

namespace exosnap::envctl {

// Stable machine-readable failure reasons. The PowerShell runner branches on
// these, never on the message text.
namespace error_code {
inline constexpr const char* kNone = "";
inline constexpr const char* kDirtyJournal = "dirty_journal";
inline constexpr const char* kBadPropertyKey = "bad_property_key";
inline constexpr const char* kUnknownProperty = "unknown_property";
inline constexpr const char* kNotMutable = "not_mutable";
inline constexpr const char* kDeviceNotPresent = "device_not_present";
inline constexpr const char* kReadFailed = "read_failed";
inline constexpr const char* kApplyRejected = "apply_rejected";
inline constexpr const char* kVerifyMismatch = "verify_mismatch";
inline constexpr const char* kRestoreVerifyMismatch = "restore_verify_mismatch";
inline constexpr const char* kRestoreRejected = "restore_rejected";
inline constexpr const char* kJournalWriteFailed = "journal_write_failed";
inline constexpr const char* kJournalReadFailed = "journal_read_failed";
} // namespace error_code

struct TransactionConfig {
    std::filesystem::path journal_path;
    std::string transaction_id; // generated when empty
    std::string run_id;
    std::string scenario;
    long long owner_pid{0};
    std::string machine;                // defaults to StableMachineHash()
    std::function<std::string()> clock; // defaults to NowUtcIso8601; injected in tests

    // Optional. Only used to enrich a RestorePendingDeviceUnavailable report
    // with the stable id and friendly name of the device that vanished.
    std::function<std::optional<AliasBinding>(const std::string& alias)> alias_lookup;
};

struct TransactionResult {
    bool ok{false};
    TransactionState state{TransactionState::Clean};
    std::string error_code;
    std::string error;
    std::vector<DevicePendingRestore> pending;

    // Non-fatal, but never silent. The one producer today is a journal file that
    // could not be DELETED after a verified restore: the machine really is back at
    // its originals, so `ok` stays true and the state stays Restored, yet a stale
    // journal left on disk will block the next `begin` with dirty_journal. Swallowing
    // that turned a one-line "delete this file" into a mystery on the next run.
    std::string warning;
};

class EnvironmentTransaction {
  public:
    EnvironmentTransaction(IEnvironmentProvider& provider, TransactionConfig config);

    // Snapshot, journal, validate, apply the minimal delta, verify. On success
    // the machine is in the desired state and the journal says so.
    //
    // Refuses outright (error_code == "dirty_journal") when a journal already
    // exists: an unrecovered journal means the machine is NOT at its original
    // settings, and layering a second transaction on top would snapshot the
    // wrong "original".
    TransactionResult Begin(const std::map<std::string, std::string>& desired);

    // Put the exact originals back, in reverse order of application. Idempotent:
    // calling it on an already-terminal transaction re-reports the terminal
    // result without touching the machine.
    TransactionResult Restore();

    // Take over an on-disk journal (recovery path). The transaction then owes
    // exactly what that journal's `applied` list says it owes.
    void AdoptJournal(Journal journal);

    TransactionState State() const {
        return journal_.state;
    }
    const Journal& JournalDocument() const {
        return journal_;
    }
    const std::filesystem::path& JournalPath() const {
        return config_.journal_path;
    }

    TransactionEvidence Evidence() const;

  private:
    TransactionResult Fail(const char* code, std::string message);
    bool PersistJournal(TransactionState state, std::string& error);
    TransactionResult RestoreInternal();
    std::vector<DevicePendingRestore> BuildPending(const std::string& missing_alias) const;

    IEnvironmentProvider& provider_;
    TransactionConfig config_;
    Journal journal_;
    std::map<std::string, PropertyEvidence> evidence_;
    std::optional<TransactionResult> terminal_;
};

// Startup gate. Not a warning: `mutation_allowed` is false unless the machine is
// provably back at its original settings, and Begin() refuses anyway while a
// journal is on disk.
struct RecoveryOutcome {
    bool journal_present{false};
    bool recovered{false};
    bool mutation_allowed{false};
    TransactionState state{TransactionState::Clean};
    std::string error_code;
    std::string error;
    std::string warning; // see TransactionResult::warning
    std::vector<DevicePendingRestore> pending;
    TransactionEvidence evidence;
};

RecoveryOutcome RecoverIfDirty(IEnvironmentProvider& provider, TransactionConfig config);

} // namespace exosnap::envctl

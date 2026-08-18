// tools/envctl/core/env_evidence.h -- what the transaction proves, per property.
//
// A verification run is only worth something if it can state, afterwards, that
// the machine is back where it started. Four observed values per property say
// that without interpretation:
//
//   before        read before anything was touched
//   requested     what the transaction asked for
//   applied       what the machine reported AFTER the setter, re-read
//   after_restore what the machine reported after the restore, re-read
//
// Acceptance is `before == after_restore`. Not "the restore call succeeded",
// not "no error was logged" -- the same read path that produced `before`
// produced `after_restore`, and they match.

#pragma once

#include <string>
#include <vector>

namespace exosnap::envctl {

struct PropertyEvidence {
    std::string property; // PropertyId::Key()
    std::string before;
    std::string requested;
    std::string applied;
    std::string after_restore;
    bool skipped{false}; // already at the desired value: nothing was applied, nothing is owed.
};

// The acceptance predicate for one exactly-restorable property.
bool IsRestored(const PropertyEvidence& evidence);

// A restore obligation this run could not discharge because the device is no
// longer attached. Everything a human needs to finish it by hand is here --
// including the STABLE id, because the friendly name may well be what changed.
struct DevicePendingRestore {
    std::string alias;
    std::string stable_id;
    std::string friendly_name;
    std::string property; // PropertyId::Key()
    std::string original_value;
    std::string remaining_action;
};

struct TransactionEvidence {
    std::string transaction_id;
    std::string run_id;
    std::string scenario;
    std::string machine;
    std::string final_state; // TransactionState key
    std::vector<PropertyEvidence> properties;
    std::vector<DevicePendingRestore> pending;

    // Every controlled property is back where it started and nothing is owed.
    bool Accepted() const;

    std::string ToJsonText() const;
};

} // namespace exosnap::envctl

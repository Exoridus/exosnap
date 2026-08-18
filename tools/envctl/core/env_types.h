// tools/envctl/core/env_types.h -- vocabulary for the test-only environment orchestrator.
//
// TEST TOOLING ONLY. Nothing in tools/envctl is linked into exosnap.exe or
// exosnap-updater.exe, nothing is installed, and nothing here runs as a service
// or at logon. It exists so a release-verification runner can put this machine
// into a known display/audio state, run one check, and put it back EXACTLY.
//
// Two vocabularies live here and must not be collapsed:
//   * CapabilityClass -- what the orchestrator is ALLOWED to do with a property.
//     A property is only ever mutated when it is classified ENV_MUTATE_SAFE (or
//     ENV_MUTATE_TESTONLY). Everything reachable only through an undocumented
//     mechanism is ENV_HUMAN and implemented read-only, by policy, even when a
//     private API would work.
//   * TransactionState -- where a transaction is in the mandatory
//     snapshot/journal/apply/verify/restore sequence. It is written to the
//     recovery journal, so its string keys are an on-disk contract.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace exosnap::envctl {

// How much authority the orchestrator has over one property.
//
// The distinction that matters is Human vs MutateSafe: "Windows exposes no
// DOCUMENTED setter" is a hard stop, not an invitation to reach for
// IPolicyConfig, a registry write, or UI automation of the Settings app. Such a
// property is read, reported, and left to the operator.
enum class CapabilityClass {
    Read,           // "ENV_READ"             -- observable; no mutation offered at all.
    MutateSafe,     // "ENV_MUTATE_SAFE"      -- documented setter, reversible, exact restore possible.
    MutateTestOnly, // "ENV_MUTATE_TESTONLY"  -- documented, but only acceptable inside a test transaction.
    Human,          // "ENV_HUMAN"            -- the operator changes it; we read and verify only.
    Physical,       // "PHYSICAL"             -- requires physically touching hardware (unplug/replug).
    Secure,         // "SECURE"               -- behind the Secure Desktop / elevation; unscriptable by design.
    Unavailable,    // "UNAVAILABLE"          -- this OS/SDK build cannot even observe it.
};

std::string_view ToKey(CapabilityClass value);
std::optional<CapabilityClass> CapabilityClassFromKey(std::string_view key);

// The ONLY predicate a mutation path may consult. Read/Human/Physical/Secure/
// Unavailable all answer false.
bool IsMutable(CapabilityClass value);

// A machine-local alias plus the property on it. The alias is resolved through
// the alias profile to a stable Windows identifier; it is never itself an
// identifier and never a friendly name.
struct PropertyId {
    std::string device_alias; // e.g. "display.main-hdr"
    std::string property;     // e.g. "hdr"

    // "<device_alias>:<property>" -- the journal and evidence key. Neither half
    // may contain ':'.
    std::string Key() const;
};

bool operator==(const PropertyId& lhs, const PropertyId& rhs);
bool operator!=(const PropertyId& lhs, const PropertyId& rhs);
bool operator<(const PropertyId& lhs, const PropertyId& rhs);

std::optional<PropertyId> PropertyIdFromKey(std::string_view key);

// Values are strings on purpose. The journal has to reproduce the ORIGINAL
// byte-for-byte months later on a machine whose SDK may have moved; a typed
// value would need a schema migration for every new property, and a lossy
// numeric round-trip ("48000/24" -> two doubles -> "48000/24"?) is exactly the
// class of bug that makes a restore silently wrong. `kind` is a tag for the
// reader, never for parsing decisions inside the transaction.
struct PropertyValue {
    std::string value; // "on", "off", "144", "48000/24", "1920x1080@144x32"
    std::string kind;  // "onoff", "hz", "mode", "wave-format", "text", ...
};

struct PropertyDescriptor {
    PropertyId id;
    CapabilityClass capability{CapabilityClass::Unavailable};
    std::string kind;             // value kind tag, matching PropertyValue::kind
    std::string read_mechanism;   // the documented API actually used to read
    std::string mutate_mechanism; // the documented API actually used to write, or why there is none
    std::string note;
};

// Where a transaction is. Serialized into the journal -- the string keys are an
// on-disk contract with the PowerShell runner and with a future recovery run.
enum class TransactionState {
    Clean,                           // no journal, nothing owed.
    Prepared,                        // original snapshotted and on disk; nothing mutated yet.
    Mutating,                        // at least one mutation may be in flight.
    Active,                          // desired state reached and verified; caller runs the test.
    Restoring,                       // restore in progress.
    Restored,                        // original verified back in place; journal deleted.
    RestorePending,                  // restore owed, not yet attempted (recovered journal).
    RestorePendingDeviceUnavailable, // a device that must be restored is gone; NOTHING else was touched.
    RestoreFailed,                   // terminal: a restore setter succeeded but the read-back disagreed.
};

std::string_view ToKey(TransactionState value);
std::optional<TransactionState> TransactionStateFromKey(std::string_view key);

// True for every state whose journal on disk means "this machine may still be
// off its original settings". This is the startup gate's predicate.
bool IsDirty(TransactionState value);

// True for states no further automatic action can improve.
bool IsTerminal(TransactionState value);

} // namespace exosnap::envctl

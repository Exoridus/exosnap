// tools/envctl/core/env_journal.h -- the recovery journal.
//
// The journal is the only reason this tool is safe to run on a developer's own
// machine. It is written BEFORE the first mutation and rewritten after every
// mutation that has been applied AND verified, so a kill -9, a bluescreen or a
// power cut at any instant leaves a file on disk that answers three questions
// without ambiguity:
//
//   original[]  what the machine looked like before anything was touched
//   applied[]   what has been changed and NOT YET restored (entries are removed
//               as they are successfully restored, so this list is always the
//               outstanding debt, never a history log)
//   desired[]   what the transaction was trying to reach
//
// Writes are atomic (temp file in the same directory + rename), because a
// half-written journal is strictly worse than no journal: it would claim the
// machine is clean while it is not.
//
// Privacy: `machine` is a stable, non-identifying hash. No hostname, no user
// name, no path outside the repository is ever written here -- the file is
// scratch, but it is scratch on somebody's personal machine.

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "env_types.h"

namespace exosnap::envctl {

// One outstanding restore obligation.
struct AppliedEntry {
    std::string property; // PropertyId::Key()
    std::string from;     // the ORIGINAL value; restoring means putting this back exactly.
    std::string to;       // what it was changed to, verified by read-back.
    std::string at_utc;
};

struct Journal {
    int schema_version{1};
    std::string transaction_id;
    long long owner_pid{0};
    std::string run_id;
    std::string machine; // stable non-identifying hash
    std::string scenario;
    TransactionState state{TransactionState::Clean};
    std::map<std::string, std::string> original; // ordered: stable diffs, stable tests
    std::map<std::string, std::string> desired;
    std::vector<AppliedEntry> applied;
    std::string created_at;
    std::string updated_at;

    std::string ToJsonText() const;
    static std::optional<Journal> FromJsonText(std::string_view text, std::string& error);
};

// Temp file in the SAME directory + std::filesystem::rename. Same directory
// matters: a rename across volumes is a copy, and a copy is not atomic.
bool WriteJournalAtomic(const std::filesystem::path& path, const Journal& journal, std::string& error);

// std::nullopt with an empty `error` means "no journal" (clean); a non-empty
// `error` means the file is there but unreadable, which is NOT clean.
std::optional<Journal> ReadJournal(const std::filesystem::path& path, std::string& error);

bool DeleteJournal(const std::filesystem::path& path, std::string& error);

// ISO 8601 UTC, second resolution, "Z" suffix.
std::string NowUtcIso8601();

// 16 hex chars. Derived from coarse machine characteristics, one-way, and never
// serialized alongside anything that would let it be reversed. It exists to tell
// "this journal is from another machine" apart from "this journal is mine".
std::string StableMachineHash();

// Deterministic form, for tests and for callers that want to pin the seed.
std::string MachineHashFromSeed(std::string_view seed);

} // namespace exosnap::envctl

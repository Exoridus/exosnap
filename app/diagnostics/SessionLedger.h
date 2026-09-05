#pragma once

#include "DiagnosticResult.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace exosnap::diagnostics {

// One uninterrupted stretch during which a problem was firing. `end_s` equals
// `start_s` while the stretch is still open, so a consumer that wants a live
// duration measures from `start_s` against the current elapsed time rather than
// reading a length that is not final yet.
struct LedgerOccurrence {
    double start_s = 0.0; // session-relative seconds
    double end_s = 0.0;
    double worst = 0.0;

    friend bool operator==(const LedgerOccurrence&, const LedgerOccurrence&) = default;
};

// What one diagnostic id did over a whole recording session: when it was first
// and last measured, how many separate stretches it fired for, and the worst
// value it reached against the budget it was measured against.
struct LedgerEntry {
    std::string id;
    std::string title;
    std::string summary;
    std::string log_excerpt;
    std::optional<double> worst; // max measured_value seen this session
    std::optional<double> budget;
    std::string unit;
    // The rendered current_value of the evaluation that produced `worst`. The
    // fallback for a check that measures something no number can carry.
    std::string worst_text;
    uint32_t count = 0; // occurrences, not evaluations
    double first_seen_s = 0.0;
    double last_seen_s = 0.0;
    double total_active_s = 0.0; // sum over CLOSED occurrences only
    bool active = false;
    bool needs_elevation = false;
    std::vector<LedgerOccurrence> occurrences;

    friend bool operator==(const LedgerEntry&, const LedgerEntry&) = default;
};

// The per-session record of every Tier-2 measured problem, keyed by diagnostic id.
//
// Two properties make this a ledger rather than a filtered view of the current
// checklist. A problem must fire on two consecutive evaluations before it enters,
// so a single spike at the 500 ms cadence never raises a card. And an entry never
// leaves before the next session: once measured, a problem stays on the record for
// the rest of the recording even after it goes quiet, because "it happened" is the
// answer the owner of the finished file needs.
class SessionLedger {
  public:
    // Consecutive firing evaluations an id needs before it earns an entry.
    static constexpr int kEntryEvaluations = 2;

    // Starts a new session. Everything observed so far is discarded: entries are
    // per recording session generation and must never survive into the next one.
    void Reset(uint64_t generation);

    // One evaluation of the live checklist. Every admitted check in `results` is
    // firing at `now_s`; every entered id absent from it goes quiet. Admission is
    // Tier-2 AND RecommendationEngine::IsLiveMeasuredCheck: a Tier-2 check that
    // reports a property of the configuration was already true before the
    // recording started and is not something the recording ran into.
    //
    // Must be called once per distinct evaluation. Calling it twice for the same
    // measurement satisfies the two-evaluation entry rule with a single sample,
    // so the caller is responsible for not re-observing an unchanged snapshot.
    void Observe(const std::vector<DiagnosticResult>& results, double now_s);

    // Ends the session's observation window: closes the occurrences that are still
    // open at `end_s`. Entries stay, so the frozen ledger can be read afterwards.
    void Freeze(double end_s);

    [[nodiscard]] const std::vector<LedgerEntry>& entries() const noexcept;
    [[nodiscard]] int activeCount() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] uint64_t generation() const noexcept;

  private:
    // An id that has fired but has not reached kEntryEvaluations yet. The worst
    // value is carried here too, so an entry created on the second evaluation
    // reports the worst of both and not only the one that tipped it over.
    struct Pending {
        int consecutive = 0;
        double first_seen_s = 0.0;
        std::optional<double> worst;
        std::string worst_text;
    };

    void CloseOccurrence(LedgerEntry& entry, double end_s);
    LedgerEntry* Find(const std::string& id);

    std::vector<LedgerEntry> entries_; // insertion order == first seen; never re-sorted
    std::unordered_map<std::string, Pending> pending_;
    uint64_t generation_ = 0;
};

} // namespace exosnap::diagnostics

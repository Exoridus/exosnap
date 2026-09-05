#include "diagnostics/SessionLedger.h"

#include "diagnostics/RecommendationEngine.h"

#include <algorithm>
#include <unordered_set>

namespace exosnap::diagnostics {
namespace {

void RecordWorst(std::optional<double>& worst, std::string& worst_text, const DiagnosticResult& result) {
    if (!result.measured_value.has_value()) {
        // A check with no number still needs a worst_text, or the entry would
        // report nothing at all about what was measured.
        if (worst_text.empty())
            worst_text = result.current_value;
        return;
    }
    if (!worst.has_value() || *result.measured_value > *worst) {
        worst = *result.measured_value;
        worst_text = result.current_value;
    }
}

} // namespace

void SessionLedger::Reset(uint64_t generation) {
    entries_.clear();
    pending_.clear();
    generation_ = generation;
}

LedgerEntry* SessionLedger::Find(const std::string& id) {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const LedgerEntry& e) { return e.id == id; });
    return it == entries_.end() ? nullptr : &*it;
}

void SessionLedger::CloseOccurrence(LedgerEntry& entry, double end_s) {
    entry.active = false;
    if (entry.occurrences.empty())
        return;
    LedgerOccurrence& open = entry.occurrences.back();
    open.end_s = end_s;
    entry.total_active_s += std::max(0.0, end_s - open.start_s);
}

void SessionLedger::Observe(const std::vector<DiagnosticResult>& results, double now_s) {
    std::unordered_set<std::string> firing;
    for (const DiagnosticResult& result : results) {
        if (result.tier != DiagnosticTier::MeasuredProblem)
            continue;
        // Tier alone is not the gate: several Tier-2 checks report a property of
        // the configuration, which was already true before Record was pressed and
        // did not happen during this recording.
        if (!RecommendationEngine::IsLiveMeasuredCheck(result.id))
            continue;
        firing.insert(result.id);

        if (LedgerEntry* entry = Find(result.id)) {
            if (!entry->active) {
                entry->active = true;
                ++entry->count;
                entry->occurrences.push_back({now_s, now_s, 0.0});
            }
            entry->last_seen_s = now_s;
            RecordWorst(entry->worst, entry->worst_text, result);
            if (result.measured_value.has_value() && !entry->occurrences.empty()) {
                LedgerOccurrence& open = entry->occurrences.back();
                open.worst = std::max(open.worst, *result.measured_value);
            }
            continue;
        }

        Pending& pending = pending_[result.id];
        if (pending.consecutive == 0)
            pending.first_seen_s = now_s;
        ++pending.consecutive;
        RecordWorst(pending.worst, pending.worst_text, result);
        if (pending.consecutive < kEntryEvaluations)
            continue;

        LedgerEntry entry;
        entry.id = result.id;
        entry.title = result.title;
        entry.summary = result.summary;
        entry.log_excerpt = result.detail;
        entry.worst = pending.worst;
        entry.worst_text = pending.worst_text;
        entry.budget = result.budget_value;
        entry.unit = result.value_unit;
        entry.count = 1;
        // The first firing, not the one that tipped the debounce over: the entry
        // reports when the problem started, not when it was believed.
        entry.first_seen_s = pending.first_seen_s;
        entry.last_seen_s = now_s;
        entry.active = true;
        entry.occurrences.push_back({pending.first_seen_s, pending.first_seen_s, entry.worst.value_or(0.0)});
        entries_.push_back(std::move(entry));
        pending_.erase(result.id);
    }

    for (LedgerEntry& entry : entries_) {
        if (entry.active && firing.find(entry.id) == firing.end())
            CloseOccurrence(entry, now_s);
    }
    for (auto it = pending_.begin(); it != pending_.end();)
        it = (firing.find(it->first) == firing.end()) ? pending_.erase(it) : std::next(it);
}

void SessionLedger::Freeze(double end_s) {
    for (LedgerEntry& entry : entries_) {
        if (entry.active)
            CloseOccurrence(entry, end_s);
    }
    pending_.clear();
}

const std::vector<LedgerEntry>& SessionLedger::entries() const noexcept {
    return entries_;
}

int SessionLedger::activeCount() const noexcept {
    return static_cast<int>(
        std::count_if(entries_.begin(), entries_.end(), [](const LedgerEntry& e) { return e.active; }));
}

bool SessionLedger::empty() const noexcept {
    return entries_.empty();
}

uint64_t SessionLedger::generation() const noexcept {
    return generation_;
}

} // namespace exosnap::diagnostics

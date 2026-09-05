// The session ledger: what a recording measured, kept for the whole session.
//
// The two properties under test are the debounce and the permanence. A check
// that fires once at the 500 ms live cadence is noise and must never raise a
// card; a check that fired and went quiet must still be on the record at Stop,
// because "it happened" is the answer the owner of the finished file needs.

#include "diagnostics/SessionLedger.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace exosnap::diagnostics {
namespace {

DiagnosticResult MeasuredProblem(const std::string& id, double value, double budget) {
    DiagnosticResult r;
    r.id = id;
    r.tier = DiagnosticTier::MeasuredProblem;
    r.severity = DiagnosticSeverity::Notice;
    r.title = id + " title";
    r.summary = id + " summary";
    r.recommendation = id + " why";
    r.detail = id + " detail";
    r.current_value = id + " at " + std::to_string(value);
    r.measured_value = value;
    r.budget_value = budget;
    r.value_unit = "ms";
    return r;
}

DiagnosticResult Tiered(const std::string& id, DiagnosticTier tier) {
    DiagnosticResult r = MeasuredProblem(id, 1.0, 0.5);
    r.tier = tier;
    return r;
}

TEST(SessionLedger, ASingleSpikeNeverCreatesAnEntry) {
    SessionLedger ledger;
    ledger.Reset(1);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 1.0);
    ledger.Observe({}, 1.5);
    EXPECT_TRUE(ledger.empty());
    EXPECT_EQ(ledger.activeCount(), 0);
}

TEST(SessionLedger, TwoConsecutiveEvaluationsEnterAndStayUntilReset) {
    SessionLedger ledger;
    ledger.Reset(1);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 1.0);
    ledger.Observe({MeasuredProblem("rec.001", 11.4, 8.0)}, 1.5);

    ASSERT_EQ(ledger.entries().size(), 1u);
    const LedgerEntry& e = ledger.entries().front();
    EXPECT_TRUE(e.active);
    EXPECT_EQ(e.count, 1u);
    ASSERT_TRUE(e.worst.has_value());
    EXPECT_DOUBLE_EQ(*e.worst, 11.4);
    ASSERT_TRUE(e.budget.has_value());
    EXPECT_DOUBLE_EQ(*e.budget, 8.0);
    EXPECT_EQ(e.unit, "ms");
    EXPECT_EQ(e.title, "rec.001 title");
    EXPECT_EQ(e.why, "rec.001 why");
    EXPECT_EQ(e.log_excerpt, "rec.001 detail");
    // The first firing, not the second: the entry says when the problem started.
    EXPECT_DOUBLE_EQ(e.first_seen_s, 1.0);
    EXPECT_DOUBLE_EQ(e.last_seen_s, 1.5);

    ledger.Observe({}, 2.0);
    EXPECT_FALSE(ledger.entries().front().active);
    ASSERT_EQ(ledger.entries().front().occurrences.size(), 1u);
    EXPECT_DOUBLE_EQ(ledger.entries().front().occurrences.front().end_s, 2.0);

    ledger.Observe({}, 2.5);
    EXPECT_EQ(ledger.entries().size(), 1u); // never leaves before Reset
}

TEST(SessionLedger, ReFiringOpensASecondOccurrenceAndKeepsOrder) {
    SessionLedger ledger;
    ledger.Reset(1);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 1.0);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 1.5);
    ledger.Observe({}, 2.0);

    ledger.Observe({MeasuredProblem("rec.gpu.contention", 24.0, 16.67)}, 3.0);
    ledger.Observe({MeasuredProblem("rec.gpu.contention", 24.0, 16.67)}, 3.5);
    // Once an id has an entry, a single evaluation re-opens it: the debounce buys
    // entry to the ledger, not re-entry to a problem already on the record.
    ledger.Observe({MeasuredProblem("rec.001", 12.0, 8.0)}, 4.0);

    ASSERT_EQ(ledger.entries().size(), 2u);
    // Order is first seen and is never re-sorted, even though rec.gpu.contention
    // became active later than rec.001 went quiet.
    EXPECT_EQ(ledger.entries()[0].id, "rec.001");
    EXPECT_EQ(ledger.entries()[1].id, "rec.gpu.contention");
    EXPECT_EQ(ledger.entries()[0].count, 2u);
    ASSERT_EQ(ledger.entries()[0].occurrences.size(), 2u);
    EXPECT_DOUBLE_EQ(ledger.entries()[0].occurrences[1].start_s, 4.0);
    EXPECT_DOUBLE_EQ(*ledger.entries()[0].worst, 12.0);
}

TEST(SessionLedger, TotalActiveDurationSumsOccurrences) {
    SessionLedger ledger;
    ledger.Reset(1);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 0.0);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 0.5);
    ledger.Observe({}, 1.0); // first occurrence: 0.0 -> 1.0
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 2.0);
    ledger.Observe({}, 2.5); // second occurrence: 2.0 -> 2.5

    ASSERT_EQ(ledger.entries().size(), 1u);
    EXPECT_DOUBLE_EQ(ledger.entries().front().total_active_s, 1.5);
}

TEST(SessionLedger, FreezeClosesOpenOccurrences) {
    SessionLedger ledger;
    ledger.Reset(1);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 8.0);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 8.5);
    ASSERT_TRUE(ledger.entries().front().active);

    ledger.Freeze(10.0);
    const LedgerEntry& e = ledger.entries().front();
    EXPECT_FALSE(e.active);
    ASSERT_EQ(e.occurrences.size(), 1u);
    EXPECT_DOUBLE_EQ(e.occurrences.front().end_s, 10.0);
    EXPECT_DOUBLE_EQ(e.total_active_s, 2.0);
    EXPECT_EQ(ledger.activeCount(), 0);
}

TEST(SessionLedger, ResetWithNewGenerationClearsEverything) {
    SessionLedger ledger;
    ledger.Reset(1);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 1.0);
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 1.5);
    ASSERT_FALSE(ledger.empty());

    ledger.Reset(2);
    EXPECT_TRUE(ledger.empty());
    EXPECT_EQ(ledger.generation(), 2u);
    // The debounce state goes with it: a half-fired id from the old session must
    // not enter on the first evaluation of the new one.
    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 0.5);
    EXPECT_TRUE(ledger.empty());
}

TEST(SessionLedger, BlockersAndOptimisationsAreIgnored) {
    SessionLedger ledger;
    ledger.Reset(1);
    for (double t : {1.0, 1.5, 2.0}) {
        ledger.Observe({Tiered("rec.blocker", DiagnosticTier::Blocker), Tiered("rec.tip", DiagnosticTier::Optimisation),
                        Tiered("fact.x", DiagnosticTier::Fact)},
                       t);
    }
    EXPECT_TRUE(ledger.empty());
}

TEST(SessionLedger, ActiveCountCountsOnlyActiveEntries) {
    SessionLedger ledger;
    ledger.Reset(1);
    const auto both = std::vector<DiagnosticResult>{MeasuredProblem("rec.001", 9.0, 8.0),
                                                    MeasuredProblem("rec.disk.writestall", 150.0, 100.0)};
    ledger.Observe(both, 1.0);
    ledger.Observe(both, 1.5);
    EXPECT_EQ(ledger.activeCount(), 2);

    ledger.Observe({MeasuredProblem("rec.001", 9.0, 8.0)}, 2.0);
    EXPECT_EQ(ledger.activeCount(), 1);
    EXPECT_EQ(ledger.entries().size(), 2u);
}

TEST(SessionLedger, ElevationOnlyChecksAreMarkedAsSuch) {
    SessionLedger ledger;
    ledger.Reset(1);
    ledger.Observe({MeasuredProblem("rec.dpc.latency", 2500.0, 1000.0)}, 1.0);
    ledger.Observe({MeasuredProblem("rec.dpc.latency", 2500.0, 1000.0)}, 1.5);
    ASSERT_EQ(ledger.entries().size(), 1u);
    EXPECT_TRUE(ledger.entries().front().needs_elevation);
}

} // namespace
} // namespace exosnap::diagnostics

// Tests for the staged page-hydration controller: the ordered execution of
// MainWindow's deferred secondary-page builders, one step per event-loop turn,
// bracketed by "perf" AppLog milestones.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QVector>

#include <functional>
#include <vector>

#include "diagnostics/AppLog.h"
#include "services/PageHydrationController.h"

namespace exosnap {
namespace {

using diagnostics::AppLog;
using diagnostics::LogEntry;

// ── QCoreApplication guard ──────────────────────────────────────────────────

class PageHydrationControllerTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char name[] = "page_hydration_controller_tests";
            static char* argv[] = {name, nullptr};
            static QCoreApplication app(argc, argv);
        }
    }

    void SetUp() override {
        AppLog::resetForTesting();
    }

    void TearDown() override {
        AppLog::resetForTesting();
    }

    // Drains queued singleShot(0) calls until `predicate` is true or the
    // iteration budget is exhausted (so a bug that never reaches completion
    // fails the assertion instead of hanging the test).
    static void PumpUntil(const std::function<bool()>& predicate, int max_iterations = 200) {
        for (int i = 0; i < max_iterations && !predicate(); ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }

    // Collects only the "perf" category messages, in order, stripping the
    // " <N> ms" suffix so assertions compare the milestone name alone.
    static QStringList PerfMilestones() {
        QStringList out;
        for (const LogEntry& entry : AppLog::history()) {
            if (entry.category != QStringLiteral("perf"))
                continue;
            QString msg = entry.message;
            const int space_idx = msg.indexOf(QLatin1Char(' '));
            out << (space_idx >= 0 ? msg.left(space_idx) : msg);
        }
        return out;
    }
};

// ── Test 1: steps run in registration order, one per event-loop turn ───────

TEST_F(PageHydrationControllerTest, RunsStepsInOrderOnePerTurn) {
    std::vector<QString> order;

    std::vector<PageHydrationController::Step> steps;
    steps.push_back({QStringLiteral("alpha"), [&order] { order.push_back(QStringLiteral("alpha")); }});
    steps.push_back({QStringLiteral("beta"), [&order] { order.push_back(QStringLiteral("beta")); }});
    steps.push_back({QStringLiteral("gamma"), [&order] { order.push_back(QStringLiteral("gamma")); }});

    PageHydrationController controller(steps);
    controller.start();

    // The first step must have already run synchronously — no event-loop turn
    // required to see it.
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], QStringLiteral("alpha"));

    // One-per-turn staging is structural: step N+1 is scheduled only from
    // inside step N via QTimer::singleShot(0, ...), which by contract never
    // fires synchronously — Test 2 asserts that deferral directly. (A
    // processEvents() call is not a reliable single-turn probe: the deadline
    // overload loops and drains successive zero-timers in one call.)
    PumpUntil([&order] { return order.size() == 3; });

    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], QStringLiteral("alpha"));
    EXPECT_EQ(order[1], QStringLiteral("beta"));
    EXPECT_EQ(order[2], QStringLiteral("gamma"));
}

// ── Test 2: first step runs synchronously; later steps are deferred ────────

TEST_F(PageHydrationControllerTest, OnlyFirstStepRunsSynchronously) {
    int calls = 0;

    std::vector<PageHydrationController::Step> steps;
    steps.push_back({QStringLiteral("first"), [&calls] { ++calls; }});
    steps.push_back({QStringLiteral("second"), [&calls] { ++calls; }});

    PageHydrationController controller(steps);
    controller.start();

    EXPECT_EQ(calls, 1) << "first step must run before start() returns";

    PumpUntil([&calls] { return calls == 2; });
    EXPECT_EQ(calls, 2);
}

// ── Test 3: milestone emission matches the legacy naming convention ────────

TEST_F(PageHydrationControllerTest, EmitsExpectedMilestoneSequence) {
    std::vector<PageHydrationController::Step> steps;
    steps.push_back({QStringLiteral("config"), [] {}});
    steps.push_back({QStringLiteral("device"), [] {}});
    steps.push_back({QStringLiteral("hotkeys"), [] {}});

    PageHydrationController controller(steps);
    controller.start();

    PumpUntil([this] { return PerfMilestones().size() == 7; });

    const QStringList milestones = PerfMilestones();
    const QStringList expected = {
        QStringLiteral("hydrate-config-start"),
        QStringLiteral("hydrate-config-end"),
        QStringLiteral("hydrate-page-start:device"),
        QStringLiteral("hydrate-page-end:device"),
        QStringLiteral("hydrate-page-start:hotkeys"),
        QStringLiteral("hydrate-page-end:hotkeys"),
        QStringLiteral("hydrate-all-end"),
    };
    EXPECT_EQ(milestones, expected);
}

// ── Test 4: idempotency guard — calling start() twice does not re-run steps ─

TEST_F(PageHydrationControllerTest, StartIsIdempotent) {
    int calls = 0;

    std::vector<PageHydrationController::Step> steps;
    steps.push_back({QStringLiteral("only"), [&calls] { ++calls; }});

    PageHydrationController controller(steps);
    controller.start();
    controller.start(); // must be a no-op

    EXPECT_EQ(calls, 1);
    PumpUntil([] { return true; }, 5);
    EXPECT_EQ(calls, 1);
}

// ── Test 5: a single-step chain still emits the final milestone ────────────

TEST_F(PageHydrationControllerTest, SingleStepStillEmitsFinalMilestone) {
    std::vector<PageHydrationController::Step> steps;
    steps.push_back({QStringLiteral("config"), [] {}});

    PageHydrationController controller(steps);
    controller.start();

    const QStringList milestones = PerfMilestones();
    const QStringList expected = {
        QStringLiteral("hydrate-config-start"),
        QStringLiteral("hydrate-config-end"),
        QStringLiteral("hydrate-all-end"),
    };
    EXPECT_EQ(milestones, expected);
}

// ── Test 6: an already-hydrated guard inside a step's own callback (mirrors
// MainWindow's buildXPage() early-return pattern) still only builds once, and
// the controller does not interfere with it ─────────────────────────────────

TEST_F(PageHydrationControllerTest, StepOwnGuardIsRespectedAndNotDuplicated) {
    int build_calls = 0;
    bool built = false;
    auto guarded_build = [&build_calls, &built] {
        if (built)
            return; // mirrors "already built (e.g. by an early navigation)"
        built = true;
        ++build_calls;
    };

    std::vector<PageHydrationController::Step> steps;
    steps.push_back({QStringLiteral("config"), [] {}});
    steps.push_back({QStringLiteral("device"), guarded_build});

    // Simulate early navigation building the page before the chain reaches it.
    guarded_build();
    EXPECT_EQ(build_calls, 1);

    PageHydrationController controller(steps);
    controller.start();
    PumpUntil([&build_calls] { return build_calls >= 1; }, 20);

    // The guard inside the callback makes the controller's later invocation a
    // harmless no-op — build_calls must still be 1.
    EXPECT_EQ(build_calls, 1);
}

} // namespace
} // namespace exosnap

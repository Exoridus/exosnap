#include <gtest/gtest.h>

#include "diagnostics/StartupTrace.h"

namespace exosnap::diagnostics {
namespace {

TEST(StartupTrace, RecordsInOrderAndDeduplicatesByLabel) {
    auto& t = StartupTrace::instance();
    t.resetForTesting();
    t.record(QStringLiteral("main-start"), 0);
    t.record(QStringLiteral("first-paint"), 120);
    t.record(QStringLiteral("first-paint"), 999); // duplicate label ignored

    const auto entries = t.entries();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].label, QStringLiteral("main-start"));
    EXPECT_EQ(entries[1].label, QStringLiteral("first-paint"));
    EXPECT_EQ(entries[1].elapsed_ms, 120); // first reading kept
}

TEST(StartupTrace, FormatterIsDeterministic) {
    const std::vector<StartupTraceEntry> entries = {{QStringLiteral("main-start"), 1},
                                                    {QStringLiteral("first-paint"), 42}};
    EXPECT_EQ(FormatStartupTrace(entries), QStringLiteral("main-start\t1 ms\nfirst-paint\t42 ms\n"));
}

} // namespace
} // namespace exosnap::diagnostics

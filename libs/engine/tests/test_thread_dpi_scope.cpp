// The one property the cursor mapping depends on: the scope that makes a pair of
// geometry reads agree must not outlive them. A leaked scope would make the whole
// video thread per-monitor aware, which is the change this design exists to avoid.

#include "thread_dpi_scope.h"

#include <gtest/gtest.h>

using exosnap::engine::ScopedThreadDpiAwareness;

namespace {

bool SameContext(DPI_AWARENESS_CONTEXT a, DPI_AWARENESS_CONTEXT b) {
    return AreDpiAwarenessContextsEqual(a, b) != FALSE;
}

} // namespace

TEST(ThreadDpiScope, RestoresTheContextItFound) {
    const ScopedThreadDpiAwareness outer(DPI_AWARENESS_CONTEXT_UNAWARE);
    ASSERT_TRUE(outer.active());
    const DPI_AWARENESS_CONTEXT before = GetThreadDpiAwarenessContext();

    {
        const ScopedThreadDpiAwareness inner(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ASSERT_TRUE(inner.active());
        EXPECT_TRUE(SameContext(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    }

    EXPECT_TRUE(SameContext(GetThreadDpiAwarenessContext(), before))
        << "a scope that does not restore turns a local fix into a process-wide one";
}

TEST(ThreadDpiScope, RestoresOnEveryExitPath) {
    const ScopedThreadDpiAwareness outer(DPI_AWARENESS_CONTEXT_UNAWARE);
    ASSERT_TRUE(outer.active());
    const DPI_AWARENESS_CONTEXT before = GetThreadDpiAwarenessContext();

    // The capture loop returns from the middle of the scope on four different
    // conditions, so the restore cannot live at the end of the function.
    const auto early_return = [&]() -> bool {
        const ScopedThreadDpiAwareness scope(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (scope.active()) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(early_return());
    EXPECT_TRUE(SameContext(GetThreadDpiAwarenessContext(), before));
}

TEST(ThreadDpiScope, NestingIsUnwoundInOrder) {
    const ScopedThreadDpiAwareness outer(DPI_AWARENESS_CONTEXT_UNAWARE);
    ASSERT_TRUE(outer.active());
    {
        const ScopedThreadDpiAwareness a(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        {
            const ScopedThreadDpiAwareness b(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            EXPECT_TRUE(SameContext(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
        }
        EXPECT_TRUE(SameContext(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_SYSTEM_AWARE));
    }
    EXPECT_TRUE(SameContext(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_UNAWARE));
}

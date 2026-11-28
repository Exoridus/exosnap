#include "diagnostics/PrivilegedDiagnosticsTraceGuard.h"

#include <evntrace.h>
#include <objbase.h>

#include <gtest/gtest.h>

namespace exosnap::diagnostics::privileged {
namespace {

struct TraceProperties {
    EVENT_TRACE_PROPERTIES properties{};
    std::array<wchar_t, 128> name{};

    TraceProperties() {
        properties.Wnode.BufferSize = sizeof(*this);
        properties.LoggerNameOffset = sizeof(properties);
    }
};

TEST(PrivilegedDiagnosticsTraceGuard, StopsOnlyThePrearmedIdentityWithoutAPublishedHandle) {
    const std::wstring nonce = NewNonce();
    ASSERT_FALSE(nonce.empty());
    const std::wstring name = TraceName(TraceSlot::Present, nonce);
    GUID expected{};
    GUID unrelated{};
    ASSERT_TRUE(SUCCEEDED(::CoCreateGuid(&expected)));
    ASSERT_TRUE(SUCCEEDED(::CoCreateGuid(&unrelated)));

    TraceProperties definition;
    definition.properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    definition.properties.Wnode.Guid = expected;
    definition.properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    TRACEHANDLE trace = 0;
    // An empty real-time session exercises native ownership without enabling providers.
    ASSERT_EQ(::StartTraceW(&trace, name.c_str(), &definition.properties), ERROR_SUCCESS);
    const auto query = [&name]() {
        TraceProperties result;
        return ::ControlTraceW(0, name.c_str(), &result.properties, EVENT_TRACE_CONTROL_QUERY);
    };

    StopOwnedTrace(TraceSlot::Present, nonce, unrelated);
    EXPECT_EQ(query(), ERROR_SUCCESS);
    StopOwnedTrace(TraceSlot::Present, nonce, expected);
    const ULONG status = query();
    if (status == ERROR_SUCCESS)
        ::ControlTraceW(trace, nullptr, &definition.properties, EVENT_TRACE_CONTROL_STOP);
    EXPECT_EQ(status, ERROR_WMI_INSTANCE_NOT_FOUND);
}

} // namespace
} // namespace exosnap::diagnostics::privileged

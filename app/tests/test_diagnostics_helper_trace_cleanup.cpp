#include "../../apps/diagnostics_helper/Guardian.h"
#include "../../apps/diagnostics_helper/StartingGuardian.h"

#include <evntrace.h>
#include <objbase.h>

#include <array>
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

namespace exosnap::diagnostics::ipc {
namespace {

struct TraceProperties {
    EVENT_TRACE_PROPERTIES properties{};
    std::array<wchar_t, 256> name{};

    TraceProperties() {
        properties.Wnode.BufferSize = sizeof(*this);
        properties.LoggerNameOffset = offsetof(TraceProperties, name);
    }
};

TEST(DiagnosticsHelperTraceCleanup, GuardianLaunchUsesTheSameGlobalImage) {
    const auto path = GlobalProcessImagePath(GetCurrentProcess());
    ASSERT_TRUE(path.starts_with(LR"(\\?\GLOBALROOT\Device\HarddiskVolume)"));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    ASSERT_TRUE(CreateProcessW(path.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &child));
    Handle observer;
    {
        StartingGuardian pending{Handle(child.hProcess)};
        Handle thread(child.hThread);
        HANDLE duplicate = nullptr;
        ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), pending.get(), GetCurrentProcess(), &duplicate, SYNCHRONIZE,
                                    FALSE, 0));
        observer = Handle(duplicate);
        EXPECT_EQ(GlobalProcessImagePath(pending.get()), path);
    }
    EXPECT_EQ(WaitForSingleObject(observer.get(), 0), WAIT_OBJECT_0);
}

TEST(DiagnosticsHelperTraceCleanup, StopsOnlyThePrearmedIdentityWithoutAPublishedHandle) {
    GUID expected{};
    ASSERT_TRUE(SUCCEEDED(CoCreateGuid(&expected)));
    auto unrelated = expected;
    unrelated.Data1 ^= 1u;
    std::array<wchar_t, 40> guid_text{};
    ASSERT_GT(StringFromGUID2(expected, guid_text.data(), static_cast<int>(guid_text.size())), 0);
    const auto name = std::wstring(L"ExoSnap trace cleanup test ") + guid_text.data();

    TraceProperties definition;
    definition.properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    definition.properties.Wnode.Guid = expected;
    definition.properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    TRACEHANDLE trace = 0;
    // No providers are enabled. The unique name cannot retire a running recorder's session.
    ASSERT_EQ(StartTraceW(&trace, name.c_str(), &definition.properties), ERROR_SUCCESS);
    const auto query = [&name]() {
        TraceProperties result;
        return ControlTraceW(0, name.c_str(), &result.properties, EVENT_TRACE_CONTROL_QUERY);
    };

    StopOwnedSession(name.c_str(), unrelated);
    EXPECT_EQ(query(), ERROR_SUCCESS);
    StopOwnedSession(name.c_str(), expected);
    const auto status = query();
    if (status == ERROR_SUCCESS)
        ControlTraceW(trace, nullptr, &definition.properties, EVENT_TRACE_CONTROL_STOP);
    EXPECT_EQ(status, ERROR_WMI_INSTANCE_NOT_FOUND);
}

} // namespace
} // namespace exosnap::diagnostics::ipc

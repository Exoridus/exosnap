#include "../../apps/diagnostics_helper/Protocol.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include "../../apps/diagnostics_helper/StartingGuardian.h"
#include <string>
#endif

namespace {
using namespace exosnap::diagnostics::ipc;

TEST(DiagnosticsHelperProtocol, BeginHasNoParameters) {
    Record r{};
    r.kind = Kind::Begin;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    r.value = 1;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.value = 0;
    r.related.back() = 1;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

TEST(DiagnosticsHelperProtocol, RejectsUnknownVersionsKindsAndLengths) {
    Record r{};
    r.kind = Kind::Lost;
    EXPECT_FALSE(Valid(r, sizeof(r) - 1));
    EXPECT_FALSE(Valid(r, sizeof(r) + 1));
    r.version++;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.version = kVersion;
    r.kind = static_cast<Kind>(99);
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

TEST(DiagnosticsHelperProtocol, BoundsPresentFieldsBeforeUse) {
    Record r{};
    r.kind = Kind::Present;
    r.value = 1;
    r.process = 12;
    r.related_count = 16;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    ++r.related_count;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.related_count = 0;
    r.flags = 8;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}
TEST(DiagnosticsHelperProtocol, KernelMetricsRejectPathsStaleSamplesAndOverlongNames) {
    Record r{};
    r.kind = Kind::Dpc;
    r.value = 10;
    r.window = 5;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    r.driver[0] = '/';
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.driver.fill('a');
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.driver.fill(0);
    r.process = 15001;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.process = 0;
    r.window = 11;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

TEST(DiagnosticsHelperProtocol, DpcOnlyBeginCannotSpecifyProviderOrParameters) {
    Record r{};
    r.kind = Kind::BeginDpcOnly;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    r.driver[0] = 'x';
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.driver[0] = 0;
    r.window = 3;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

#ifdef _WIN32
TEST(DiagnosticsHelperGuardian, FailedHandoffReapsChildButSuccessfulHandoffTransfersIt) {
    std::wstring image(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    ASSERT_GT(size, 0u);
    ASSERT_LT(size, image.size());
    image.resize(size);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    // Nothing runs in the child. This exercises failure before its entry point,
    // without elevation, UAC, desktop interaction or opening an ETW session.
    ASSERT_TRUE(CreateProcessW(image.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &child));
    Handle thread(child.hThread);
    StartingGuardian cleanup{Handle(child.hProcess)};
    HANDLE duplicated = nullptr;
    ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), cleanup.get(), GetCurrentProcess(), &duplicated, 0, FALSE,
                                DUPLICATE_SAME_ACCESS));
    Handle transferred;
    {
        StartingGuardian starting{Handle(duplicated)};
        transferred = starting.handoff();
    }
    EXPECT_EQ(WaitForSingleObject(cleanup.get(), 0), WAIT_TIMEOUT);
    {
        StartingGuardian failed(std::move(transferred));
    }
    EXPECT_EQ(WaitForSingleObject(cleanup.get(), 0), WAIT_OBJECT_0);
}
#endif

} // namespace

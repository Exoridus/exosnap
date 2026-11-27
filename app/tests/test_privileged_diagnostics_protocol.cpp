#include "diagnostics/PrivilegedDiagnosticsPipe.h"
#include "diagnostics/PrivilegedDiagnosticsProtocol.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace exosnap::diagnostics::privileged {
namespace {

TEST(PrivilegedDiagnosticsProtocol, AcceptsOnlyTheVersionedPollOperations) {
    Request request{};
    EXPECT_TRUE(IsValid(request));
    request.present = 1;
    EXPECT_TRUE(IsValid(request));
    request.present = 2;
    EXPECT_FALSE(IsValid(request));
    request.present = 0;
    ++request.version;
    EXPECT_FALSE(IsValid(request));
}

TEST(PrivilegedDiagnosticsProtocol, ValidatesCountsStatusAndMeasurementsBeforeReadingPayload) {
    Response response{};
    EXPECT_TRUE(IsValid(response));
    response.present_count = kMaxPresents + 1;
    EXPECT_FALSE(IsValid(response));
    response.present_count = 1;
    EXPECT_FALSE(IsValid(response));
    response.present_state = PresentStatus::Open;
    response.frequency = 10000000;
    response.presents[0].pid = 42;
    response.presents[0].qpc = 10;
    EXPECT_TRUE(IsValid(response));
    response.presents[0].related_count = kMaxRelated + 1;
    EXPECT_FALSE(IsValid(response));
    response.presents[0].related_count = 0;
    response.dpc_open = 1;
    response.dpc_qpc = 12;
    response.dpc_max_us = 3;
    response.dpc_average_us = 4;
    EXPECT_FALSE(IsValid(response));
}

TEST(PrivilegedDiagnosticsProtocol, RejectsAnUnterminatedOrNonBasenameDriverLabel) {
    Response response{};
    response.dpc_open = 1;
    response.dpc_qpc = 12;
    response.dpc_driver[0] = '/';
    EXPECT_FALSE(IsValid(response));
    response.dpc_driver.fill('x');
    EXPECT_FALSE(IsValid(response));
}

TEST(PrivilegedDiagnosticsProtocol, NativePipeProvidesPeerIdentityAndPreservesFrameBoundaries) {
    const std::wstring name = kPipePrefix + NewNonce();
    const Handle server(::CreateNamedPipeW(name.c_str(),
                                           PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                           PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                           sizeof(Response), sizeof(Request), 0, nullptr));
    ASSERT_TRUE(server);
    const auto client = OpenPipeClient(name);
    ASSERT_TRUE(client) << ::GetLastError();
    ASSERT_TRUE(Connect(server.get(), nullptr, nullptr));

    ULONG pid = 0;
    ASSERT_TRUE(::GetNamedPipeClientProcessId(server.get(), &pid));
    EXPECT_EQ(pid, ::GetCurrentProcessId());
    ASSERT_TRUE(::GetNamedPipeServerProcessId(client.get(), &pid));
    EXPECT_EQ(pid, ::GetCurrentProcessId());

    Request request{};
    ASSERT_TRUE(Transfer(server.get(), request, true, nullptr));
    ASSERT_TRUE(Transfer(client.get(), request, false, nullptr));
    EXPECT_TRUE(IsValid(request));

    std::array<uint32_t, 4> extra_word{};
    ASSERT_TRUE(Transfer(server.get(), extra_word, true, nullptr));
    EXPECT_FALSE(Transfer(client.get(), request, false, nullptr));
}

TEST(PrivilegedDiagnosticsProtocol, CancelsAWaitingOperationBeforeReturningTheFrame) {
    const std::wstring name = kPipePrefix + NewNonce();
    const Handle server(::CreateNamedPipeW(name.c_str(),
                                           PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                           PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                           sizeof(Response), sizeof(Request), 0, nullptr));
    ASSERT_TRUE(server);
    const Handle cancelled(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const Handle owner(::OpenProcess(SYNCHRONIZE, FALSE, ::GetCurrentProcessId()));
    ASSERT_TRUE(cancelled);
    ASSERT_TRUE(owner);
    const std::jthread cancel([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ::SetEvent(cancelled.get());
    });
    EXPECT_FALSE(Connect(server.get(), owner.get(), cancelled.get()));
}

} // namespace
} // namespace exosnap::diagnostics::privileged

#include "diagnostics/PrivilegedDiagnosticsIntegrity.h"
#include "diagnostics/PrivilegedDiagnosticsPath.h"
#include "diagnostics/PrivilegedDiagnosticsPipe.h"
#include "diagnostics/PrivilegedDiagnosticsProtocol.h"

#include <gtest/gtest.h>
#include <monocypher-ed25519.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <update/file_sha256.h>

namespace exosnap::diagnostics::privileged {
namespace {

TEST(PrivilegedDiagnosticsProtocol, OnlyAcceptsTheSignedCanonicalImageDigests) {
    std::array<uint8_t, 32> key{};
    std::array<uint8_t, 32> seed{};
    std::array<uint8_t, 64> secret{};
    std::array<uint8_t, 64> signature{};
    crypto_ed25519_key_pair(secret.data(), key.data(), seed.data());
    const auto sign = [&](std::string_view bytes) {
        crypto_ed25519_sign(signature.data(), secret.data(), reinterpret_cast<const uint8_t*>(bytes.data()),
                            bytes.size());
    };
    const std::string manifest =
        "EXOSNAP-DIAGNOSTICS-PAIR-V1\n" + std::string(64, 'a') + '\n' + std::string(64, '1') + '\n';
    sign(manifest);
    const auto digests = VerifyDiagnosticsImageDigests(manifest, signature, key);
    ASSERT_TRUE(digests);
    EXPECT_EQ(digests->recorder, std::string(64, 'a'));
    EXPECT_EQ(digests->helper, std::string(64, '1'));

    auto altered = manifest;
    altered[altered.size() - 2] = '2';
    EXPECT_FALSE(VerifyDiagnosticsImageDigests(altered, signature, key));
    EXPECT_FALSE(VerifyDiagnosticsImageDigests(manifest, std::span(signature).first(63), key));

    altered[altered.size() - 2] = 'A';
    sign(altered);
    EXPECT_FALSE(VerifyDiagnosticsImageDigests(altered, signature, key));

    altered = manifest;
    altered[0] = 'X';
    sign(altered);
    EXPECT_FALSE(VerifyDiagnosticsImageDigests(altered, signature, key));
}

TEST(PrivilegedDiagnosticsProtocol, SignedPairMatchesTheOpenExecutableFiles) {
    namespace fs = std::filesystem;
    const auto nonce = NewNonce();
    ASSERT_FALSE(nonce.empty());
    const auto directory = fs::temp_directory_path() / (L"exosnap-diagnostics-pair-" + nonce);
    ASSERT_TRUE(fs::create_directory(directory));
    const auto recorder_path = directory / L"exosnap.exe";
    const auto helper_path = directory / kExecutableName;
    const auto manifest_path = directory / L"diagnostics-pair.txt";
    const auto signature_path = directory / L"diagnostics-pair.txt.sig";
    struct Cleanup {
        const fs::path& directory;
        std::array<fs::path, 4> files;
        ~Cleanup() {
            std::error_code ignored;
            for (const auto& file : files)
                fs::remove(file, ignored);
            fs::remove(directory, ignored);
        }
    } cleanup{directory, {recorder_path, helper_path, manifest_path, signature_path}};

    std::ofstream(recorder_path, std::ios::binary) << "recorder";
    std::ofstream(helper_path, std::ios::binary) << "helper";
    const Handle recorder(::CreateFileW(recorder_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
    const Handle helper(::CreateFileW(helper_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(recorder);
    ASSERT_TRUE(helper);
    const std::string manifest = "EXOSNAP-DIAGNOSTICS-PAIR-V1\n" + update::Sha256HexOfHandle(recorder.get()) + '\n' +
                                 update::Sha256HexOfHandle(helper.get()) + '\n';
    std::array<uint8_t, 32> public_key{};
    std::array<uint8_t, 32> seed{};
    std::array<uint8_t, 64> secret{};
    std::array<uint8_t, 64> signature{};
    crypto_ed25519_key_pair(secret.data(), public_key.data(), seed.data());
    crypto_ed25519_sign(signature.data(), secret.data(), reinterpret_cast<const uint8_t*>(manifest.data()),
                        manifest.size());
    std::ofstream(manifest_path, std::ios::binary) << manifest;
    std::ofstream(signature_path, std::ios::binary)
        .write(reinterpret_cast<const char*>(signature.data()), static_cast<std::streamsize>(signature.size()));

    EXPECT_TRUE(VerifyDiagnosticsImageFiles(directory.native(), recorder.get(), helper.get(), public_key));
    EXPECT_FALSE(VerifyDiagnosticsImageFiles(directory.native(), helper.get(), recorder.get(), public_key));
    fs::resize_file(signature_path, 63);
    EXPECT_FALSE(VerifyDiagnosticsImageFiles(directory.native(), recorder.get(), helper.get(), public_key));
}

TEST(PrivilegedDiagnosticsProtocol, ProcessIdentityMatchesThePinnedImageOnly) {
    std::wstring path(32768, L'\0');
    const auto written = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    ASSERT_GT(written, 0u);
    ASSERT_LT(written, path.size());
    path.resize(written);

    const Handle executable(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(executable);
    path.resize(path.find_last_of(L'\\'));
    const Handle directory(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    ASSERT_TRUE(directory);
    EXPECT_TRUE(ProcessImageMatchesFile(::GetCurrentProcess(), executable.get()));
    EXPECT_FALSE(ProcessImageMatchesFile(::GetCurrentProcess(), directory.get()));
    EXPECT_FALSE(ProcessImageMatchesFile(nullptr, executable.get()));
}

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

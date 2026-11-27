#include "../FixtureStatus.h"
#include "diagnostics/PrivilegedPresentBackend.h"
#include "signed_failure.h"

#include <windows.h>

#include <evntrace.h>
#include <objbase.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <thread>

namespace {
using namespace exosnap::diagnostics;
using namespace std::chrono_literals;

using exosnap::diagnostics::fixture::QueryDpcSession;

bool RunCycle() {
    if (QueryDpcSession() != ERROR_WMI_INSTANCE_NOT_FOUND) {
        std::cerr << "DPC session already exists or cannot be queried\n";
        return false;
    }
    const auto backend = MakePrivilegedDiagnosticsBackend(false);
    if (backend->Open() != PresentTraceOpenResult::Opened)
        return false;
    std::thread consumer([&backend] { backend->Consume(); });
    const auto deadline = std::chrono::steady_clock::now() + 90s;
    auto state = backend->HelperState();
    while (state == DiagnosticsHelperState::Starting && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
        state = backend->HelperState();
    }
    const auto active = QueryDpcSession();
    backend->Close();
    consumer.join();
    const auto cleanup_deadline = std::chrono::steady_clock::now() + 30s;
    auto stopped = QueryDpcSession();
    while (stopped == ERROR_SUCCESS && std::chrono::steady_clock::now() < cleanup_deadline) {
        std::this_thread::sleep_for(100ms);
        stopped = QueryDpcSession();
    }
    const auto final_state = backend->HelperState();
    std::cout << "Ready state=" << static_cast<int>(state) << ", DPC query=" << active
              << ", final state=" << static_cast<int>(final_state) << ", final DPC query=" << stopped << '\n';
    return state == DiagnosticsHelperState::Connected && active == ERROR_SUCCESS &&
           final_state == DiagnosticsHelperState::Stopped && stopped == ERROR_WMI_INSTANCE_NOT_FOUND;
}
bool MissingMachineHelper() {
    const auto backend = MakePrivilegedDiagnosticsBackend(false);
    if (backend->Open() != PresentTraceOpenResult::Opened)
        return false;
    backend->Consume();
    return backend->HelperState() == DiagnosticsHelperState::Unavailable &&
           QueryDpcSession() == ERROR_WMI_INSTANCE_NOT_FOUND;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--hold-owned-cycle")
        return exosnap::diagnostics::fixture::HoldOwnedCycle(argv[2]) ? 0 : 1;
    if (argc == 2 && std::string_view(argv[1]) == "--forced-exit-cases")
        return exosnap::diagnostics::fixture::RunForcedExitCases() ? 0 : 1;
    if (argc == 2) {
        const std::string_view mode(argv[1]);
        if (mode == "--expect-machine-unavailable")
            return MissingMachineHelper() ? 0 : 1;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com))
            return 1;
        bool passed = false;
        if (mode == "--expect-registration-rejection")
            passed = !exosnap::diagnostics::ProbeSignedPackageAcquisition() &&
                     exosnap::diagnostics::ipc::fixture_footprint_mismatch.load();
        else if (mode == "--verify-package-acquisition")
            passed = exosnap::diagnostics::ProbeSignedPackageAcquisition();
        else if (mode == "--expect-invalid-version")
            passed = exosnap::diagnostics::ProbeMachineBootstrap(0, true) == E_INVALIDARG;
        else if (mode == "--expect-wrong-caller")
            passed = exosnap::diagnostics::ProbeMachineBootstrap(1, true) == E_ACCESSDENIED;
        else if (mode == "--empty-activation")
            passed = exosnap::diagnostics::ProbeMachineBootstrap(1, false) == S_OK;
        const auto trace = QueryDpcSession();
        exosnap::diagnostics::ipc::ReportFixtureStatus("fixture DPC query after control", trace);
        CoUninitialize();
        return passed && trace == ERROR_WMI_INSTANCE_NOT_FOUND ? 0 : 1;
    }
    for (int cycle = 1; cycle <= 2; ++cycle) {
        std::cout << "Signed provisioning and DPC cycle " << cycle << std::endl;
        if (!RunCycle())
            return 1;
    }
    return 0;
}

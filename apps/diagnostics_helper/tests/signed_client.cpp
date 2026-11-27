#include "../FixtureStatus.h"
#include "diagnostics/PrivilegedPresentBackend.h"

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

ULONG QueryDpcSession() {
    struct Query {
        EVENT_TRACE_PROPERTIES properties{};
        std::array<wchar_t, 256> name{};
    } query;
    query.properties.Wnode.BufferSize = sizeof(query);
    query.properties.LoggerNameOffset = offsetof(Query, name);
    return ControlTraceW(0, kDpcSessionName, &query.properties, EVENT_TRACE_CONTROL_QUERY);
}

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
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--probe-installed-launch") {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com)) {
            std::cerr << "Launch probe COM initialization failed: " << com << '\n';
            return 1;
        }
        exosnap::diagnostics::ProbeInstalledHelperLaunch();
        CoUninitialize();
        return 0;
    }
    for (int cycle = 1; cycle <= 2; ++cycle) {
        std::cout << "Signed provisioning and DPC cycle " << cycle << std::endl;
        if (!RunCycle())
            return 1;
    }
    return 0;
}

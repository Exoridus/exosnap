#include "../FixtureStatus.h"
#include "../Handle.h"
#include "../ImagePath.h"
#include "diagnostics/PrivilegedPresentBackend.h"

#include <windows.h>

#include <evntrace.h>
#include <msi.h>
#include <msiquery.h>
#include <objbase.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace exosnap::diagnostics;
using namespace std::chrono_literals;

std::wstring FinalPath(HANDLE file, DWORD spelling) {
    std::wstring path(32768, L'\0');
    const auto size = GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), spelling);
    if (!size || size >= path.size())
        return {};
    path.resize(size);
    return path;
}

void Probe(const char* spelling, const wchar_t* path, bool locked) {
    UINT database_status;
    UINT bound_status = ERROR_INVALID_HANDLE;
    {
        PMSIHANDLE database;
        database_status = MsiOpenDatabaseW(path, MSIDBOPEN_READONLY, &database);
        if (database_status == ERROR_SUCCESS) {
            const auto reference = L"#" + std::to_wstring(static_cast<MSIHANDLE>(database));
            PMSIHANDLE bound;
            bound_status = MsiOpenPackageExW(reference.c_str(), MSIOPENPACKAGEFLAGS_IGNOREMACHINESTATE, &bound);
        }
    }
    UINT package_status;
    {
        PMSIHANDLE package;
        package_status = MsiOpenPackageExW(path, MSIOPENPACKAGEFLAGS_IGNOREMACHINESTATE, &package);
    }
    std::cout << "MSI read probe " << spelling << " locked=" << locked << " database=" << database_status
              << " restricted bound package=" << bound_status << " restricted package=" << package_status << '\n';
}

bool ProbePackagePaths() {
    std::wstring image(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    if (!size || size >= image.size())
        return false;
    image.resize(size);
    const auto source = std::filesystem::path(image).parent_path() / L"diagnostics" / L"helper.msi";
    ipc::Handle file(CreateFileW(source.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!file)
        return false;
    const auto volume = FinalPath(file.get(), VOLUME_NAME_GUID);
    const auto native = FinalPath(file.get(), VOLUME_NAME_NT);
    if (volume.empty() || native.empty())
        return false;
    const auto global = ipc::GlobalLocalVolumePath(native);
    if (global.empty())
        return false;
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
        return false;
    for (const bool locked : {true, false}) {
        if (!locked)
            file.reset();
        Probe("DOS", source.c_str(), locked);
        Probe("volume GUID", volume.c_str(), locked);
        Probe("global native", global.c_str(), locked);
    }
    CoUninitialize();
    return true;
}

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
        exosnap::diagnostics::ProbeInstalledHelperLaunch();
        return 0;
    }
    if (!ProbePackagePaths())
        return 1;
    for (int cycle = 1; cycle <= 2; ++cycle) {
        std::cout << "Signed provisioning and DPC cycle " << cycle << std::endl;
        if (!RunCycle())
            return 1;
    }
    return 0;
}

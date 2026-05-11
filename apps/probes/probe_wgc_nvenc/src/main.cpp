#include "wgc_nvenc_probe.h"

#include <winrt/Windows.Foundation.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

static LONG WINAPI UnhandledExceptionFilterProc(_In_ EXCEPTION_POINTERS* exceptionInfo) {
    DWORD code = exceptionInfo->ExceptionRecord->ExceptionCode;
    void* addr = exceptionInfo->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "[probe] FATAL: unhandled SEH exception code=0x%08lX address=%p\n", code, addr);
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main() {
    fprintf(stderr, "[probe] process started\n");
    fflush(stderr);

    SetUnhandledExceptionFilter(UnhandledExceptionFilterProc);

    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (const winrt::hresult_error& e) {
        fwprintf(stderr, L"[probe] FATAL: winrt::init_apartment failed: %s (0x%08X)\n",
                 e.message().c_str(), static_cast<unsigned int>(e.code().value));
        fflush(stderr);
        return 100;
    } catch (...) {
        fprintf(stderr, "[probe] FATAL: winrt::init_apartment failed with unknown exception\n");
        fflush(stderr);
        return 99;
    }

    int exitCode = 0;

    try {
        auto targets = wgc_nvenc::WgcNvencProbe::EnumerateTargets();

        if (targets.empty()) {
            fprintf(stderr, "[probe] FATAL: no capturable targets found\n");
            fflush(stderr);
            return 1;
        }

        wprintf(L"\n%zu capturable targets found:\n\n", targets.size());
        for (std::size_t i = 0; i < targets.size(); ++i) {
            auto kindStr = targets[i].kind == wgc_nvenc::CaptureTarget::Kind::Monitor
                           ? L"MONITOR" : L"WINDOW";
            wprintf(L"  [%zu] %s: %s\n", i, kindStr, targets[i].description.c_str());
        }

        wprintf(L"\nEnter target number (0-%zu) or 'q' to quit: ", targets.size() - 1);
        fflush(stdout);

        std::wstring input;
        std::getline(std::wcin, input);

        if (input.empty() || input[0] == L'q' || input[0] == L'Q') {
            fprintf(stderr, "[probe] user quit\n");
            fflush(stderr);
            return 0;
        }

        int selectedIndex = -1;
        try {
            selectedIndex = std::stoi(input);
        } catch (...) {
            fprintf(stderr, "[probe] FATAL: invalid input\n");
            fflush(stderr);
            return 1;
        }

        if (selectedIndex < 0 || static_cast<std::size_t>(selectedIndex) >= targets.size()) {
            fprintf(stderr, "[probe] FATAL: index out of range\n");
            fflush(stderr);
            return 1;
        }

        const auto& target = targets[static_cast<std::size_t>(selectedIndex)];
        auto kindStr = target.kind == wgc_nvenc::CaptureTarget::Kind::Monitor ? L"monitor" : L"window";
        wprintf(L"\n[probe] target selected: index=%d kind=%s desc=\"%s\"\n",
                selectedIndex, kindStr, target.description.c_str());

        wgc_nvenc::WgcNvencProbe probe(target);
        exitCode = probe.Run();

        fprintf(stderr, "[probe] probe exited normally\n");
        fflush(stderr);
    } catch (const winrt::hresult_error& e) {
        fwprintf(stderr, L"[probe] FATAL WinRT error: %s (0x%08X)\n",
                 e.message().c_str(), static_cast<unsigned int>(e.code().value));
        fflush(stderr);
        return 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "[probe] FATAL C++ exception: %s\n", e.what());
        fflush(stderr);
        return 1;
    } catch (...) {
        fprintf(stderr, "[probe] FATAL unknown C++ exception\n");
        fflush(stderr);
        return 1;
    }

    winrt::uninit_apartment();
    return exitCode;
}

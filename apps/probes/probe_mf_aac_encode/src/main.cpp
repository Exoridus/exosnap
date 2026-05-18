#include "mf_aac_probe.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

int main() {
    fprintf(stderr, "[probe] mf_aac_encode probe started\n");
    fflush(stderr);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FATAL: CoInitializeEx failed 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return 1;
    }

    int exitCode = 0;

    try {
        mf_aac_probe::Probe probe;
        if (!probe.Run()) {
            exitCode = 1;
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[probe] FATAL exception: %s\n", e.what());
        fflush(stderr);
        exitCode = 1;
    } catch (...) {
        fprintf(stderr, "[probe] FATAL unknown exception\n");
        fflush(stderr);
        exitCode = 1;
    }

    CoUninitialize();

    fprintf(stderr, "[probe] exiting with code %d\n", exitCode);
    fflush(stderr);
    return exitCode;
}

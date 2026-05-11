#include "wasapi_endpoint_probe.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace {
constexpr uint32_t kDefaultDurationSec = 30;
}

int main() {
    fprintf(stderr, "[probe] process started\n");
    fflush(stderr);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FATAL: CoInitializeEx failed 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return 1;
    }

    int exitCode = 0;

    try {
        wasapi_probe::Probe probe;

        if (!probe.Start()) {
            exitCode = 1;
        } else {
            probe.Run(kDefaultDurationSec);
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

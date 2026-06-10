#include "nvenc_probe.h"

#include <cstdio>
#include <exception>

int main() {
    try {
        nvenc_probe::BaselineProbe probe;
        return probe.Run();
    } catch (const std::exception& e) {
        fprintf(stderr, "[probe] FATAL exception: %s\n", e.what());
        fflush(stderr);
        return 1;
    } catch (...) {
        fprintf(stderr, "[probe] FATAL unknown exception\n");
        fflush(stderr);
        return 1;
    }
}

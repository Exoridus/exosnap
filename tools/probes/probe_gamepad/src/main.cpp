#include "gamepad/gamepad.h"

#include <windows.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace exosnap::gamepad;

namespace {
void PrintRuntime() {
    wchar_t path[32768]{};
    const auto module = GetModuleHandleW(L"SDL3.dll");
    if (module)
        GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    std::printf("backend=SDL3 runtime=%ls gameinput_loaded=%d gameinput_redist_loaded=%d\n", path,
                GetModuleHandleW(L"GameInput.dll") != nullptr, GetModuleHandleW(L"GameInputRedist.dll") != nullptr);
}

void PrintReading(const GamepadReading& reading) {
    std::printf("reading capture_100ns=%llu connected=%d vid=%04X pid=%04X persistent=%d id=",
                static_cast<unsigned long long>(reading.capture_100ns), reading.connected, reading.device.vendor_id,
                reading.device.product_id, reading.device.persistent);
    for (const auto byte : reading.device.id)
        std::printf("%02x", byte);
    std::printf("\n");
    for (size_t index = 0; index < static_cast<size_t>(Control::Count); ++index) {
        const auto control = static_cast<Control>(index);
        const auto value = ReadControl(reading.state, control);
        const auto name = ControlName(control);
        std::printf("  %.*s status=%u x=%.4f y=%.4f\n", static_cast<int>(name.size()), name.data(),
                    static_cast<unsigned>(value.status), value.x, value.y);
    }
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int seconds = 10;
    int cycles = 1;
    for (int index = 1; index < argc; ++index) {
        int* destination = std::strcmp(argv[index], "--seconds") == 0  ? &seconds
                           : std::strcmp(argv[index], "--cycles") == 0 ? &cycles
                                                                       : nullptr;
        if (!destination || index + 1 >= argc) {
            std::fprintf(stderr, "usage: probe_gamepad [--seconds N] [--cycles N]\n");
            return 2;
        }
        const char* text = argv[++index];
        const auto parsed = std::from_chars(text, text + std::strlen(text), *destination);
        if (parsed.ec != std::errc{} || *parsed.ptr != '\0' || *destination <= 0)
            return 2;
    }
    RegisterInputMainThread();
    for (int cycle = 0; cycle < cycles; ++cycle) {
        std::string error;
        auto source = SdlGamepadSource::Start(error);
        if (!source) {
            std::fprintf(stderr, "FAIL: %s\n", error.c_str());
            return 1;
        }
        PrintRuntime();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        auto next_summary = std::chrono::steady_clock::now();
        std::optional<GamepadReading> previous;
        while (std::chrono::steady_clock::now() < deadline) {
            if (source->State() == InputSourceState::Failed) {
                std::fprintf(stderr, "FAIL: %s\n", source->Error().c_str());
                return 1;
            }
            const auto reading = source->StateAt(NowCapture100ns());
            if (reading && (!previous || reading->device.id != previous->device.id ||
                            reading->connected != previous->connected || reading->state != previous->state)) {
                PrintReading(*reading);
                previous = reading;
            }
            if (std::chrono::steady_clock::now() >= next_summary) {
                std::printf("source phase=%u devices=%zu history=%d\n", static_cast<unsigned>(source->State()),
                            source->Devices().size(), reading.has_value());
                next_summary = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        source->RequestStop();
        source->WaitForStop();
        std::printf("source stopped=%d cycle=%d/%d\n", source->Stopped(), cycle + 1, cycles);
        PrintRuntime();
    }
    return 0;
}

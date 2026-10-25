// Prints gamepad identity and live readings until Ctrl+C or --seconds elapse.
//
//   probe_gamepad [--seconds N]

#include "gamepad/gamepad.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace exosnap::gamepad;

namespace {

const char* LayoutName(LayoutKind layout) {
    switch (layout) {
    case LayoutKind::DualSense:
        return "DualSense";
    case LayoutKind::XboxSeries:
        return "XboxSeries";
    case LayoutKind::XboxOne:
        return "XboxOne";
    case LayoutKind::SwitchPro:
        return "SwitchPro";
    case LayoutKind::Generic:
        break;
    }
    return "Generic";
}

} // namespace

int main(int argc, char** argv) {
    // Line-by-line output even when piped, so a run that is stopped by hand
    // still shows what it saw.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int seconds = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0) {
            seconds = std::atoi(argv[i + 1]);
        }
    }

    std::string error;
    const auto source = GameInputSource::Start(error);
    if (!source) {
        std::fprintf(stderr, "FAIL: %s\n", error.c_str());
        return 1;
    }
    std::printf("GameInput started; waiting for gamepad readings\n");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    uint64_t last_stamp = 0;
    uint16_t last_vid = 0;
    uint16_t last_pid = 0;
    while (seconds <= 0 || std::chrono::steady_clock::now() < deadline) {
        const uint64_t now = GameInputSource::NowCapture100ns();
        const auto reading = source->StateAt(now);
        if (reading && reading->capture_100ns != last_stamp) {
            last_stamp = reading->capture_100ns;
            const DeviceIdentity& d = reading->device;
            if (d.vendor_id != last_vid || d.product_id != last_pid) {
                last_vid = d.vendor_id;
                last_pid = d.product_id;
                std::printf("device vid=%04X pid=%04X layout=%s\n", d.vendor_id, d.product_id, LayoutName(d.layout));
            }
            const GamepadState& s = reading->state;
            std::printf("%s age=%.1fms buttons=%08X L=(%+.3f,%+.3f) R=(%+.3f,%+.3f) LT=%.3f RT=%.3f\n",
                        reading->connected ? "pad " : "lost", static_cast<double>(now - reading->capture_100ns) / 1e4,
                        s.buttons, s.left_x, s.left_y, s.right_x, s.right_y, s.left_trigger, s.right_trigger);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return 0;
}

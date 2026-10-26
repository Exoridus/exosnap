// Prints gamepad identity and live readings until Ctrl+C or --seconds elapse.
//
//   probe_gamepad [--seconds N] [--cycles N] [--no-extra]

#include "gamepad/gamepad.h"

#include <GameInput.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace exosnap::gamepad;

namespace {

struct ExtraProbe {
    Microsoft::WRL::ComPtr<GameInput::v4::IGameInput> input;
    GameInput::v4::GameInputCallbackToken token = 0;
    std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t> previous{};
    bool described = false;
    uint64_t last_raw_us = 0;
    std::mutex device_mutex;
    Microsoft::WRL::ComPtr<GameInput::v4::IGameInputDevice> observed_device;
    std::mutex output_mutex;
    std::string pending_output;
    unsigned omitted_output = 0;

    void QueueOutput(const std::string& text) {
        const std::scoped_lock lock(output_mutex);
        if (pending_output.size() + text.size() <= 65536)
            pending_output += text;
        else
            ++omitted_output;
    }

    void FlushOutput() {
        std::string text;
        unsigned omitted = 0;
        {
            const std::scoped_lock lock(output_mutex);
            text.swap(pending_output);
            std::swap(omitted, omitted_output);
        }
        std::fwrite(text.data(), 1, text.size(), stdout);
        if (omitted != 0)
            std::printf("Diagnostic output omitted: %u reports\n", omitted);
    }

    static void CALLBACK OnReading(GameInput::v4::GameInputCallbackToken, void* context,
                                   GameInput::v4::IGameInputReading* reading) {
        using namespace GameInput::v4;
        auto& self = *static_cast<ExtraProbe*>(context);
        Microsoft::WRL::ComPtr<IGameInputDevice> device;
        reading->GetDevice(&device);
        if (device->GetVendorId() != 0x054C || device->GetProductId() != 0x0CE6)
            return;
        {
            const std::scoped_lock lock(self.device_mutex);
            self.observed_device = device;
        }
        // GameInput serializes callbacks, so console I/O must stay on the probe thread.
        std::string output;
        if (!self.described) {
            self.described = true;
            std::array<GameInputLabel, 64> labels{};
            const auto labels_count = device->GetControllerButtonLabels(64, labels.data());
            output += std::format("DualSense capabilities kinds={:08X} touch_surfaces={} button_labels=",
                                  static_cast<unsigned>(device->GetSupportedInput()), device->GetTouchSurfaceCount());
            for (uint32_t i = 0; i < std::min(labels_count, 64U); ++i)
                output += std::format("{}:{},", i, static_cast<unsigned>(labels[i]));
            output += '\n';
        }
        Microsoft::WRL::ComPtr<IGameInputRawDeviceReport> raw;
        if (reading->GetRawReport(&raw) && raw && reading->GetTimestamp() - self.last_raw_us >= 20000) {
            self.last_raw_us = reading->GetTimestamp();
            std::array<uint8_t, 128> bytes{};
            GameInputRawDeviceReportInfo info{};
            raw->GetReportInfo(&info);
            if (raw->GetRawDataSize() <= bytes.size()) {
                const size_t size = raw->GetRawData(bytes.size(), bytes.data());
                output += std::format("DualSense HID id={} bytes={} data=", info.id, size);
                for (size_t i = 0; i < std::min(size, bytes.size()); ++i)
                    output += std::format("{:02X}", bytes[i]);
                output += '\n';
            }
        }
        std::array<bool, 64> buttons{};
        const uint32_t count = reading->GetControllerButtonState(static_cast<uint32_t>(buttons.size()), buttons.data());
        uint64_t mask = 0;
        for (uint32_t i = 0; i < std::min(count, static_cast<uint32_t>(buttons.size())); ++i)
            if (buttons[i])
                mask |= uint64_t{1} << i;
        GameInputSystemButtons system{};
        const bool system_available = reading->GetSystemButtonState(&system);
        std::array<GameInputTouchContact, 2> touches{};
        const uint32_t contacts =
            device->GetTouchSurfaceCount() == 0 ? 0 : reading->GetTouchSurfaceState(0, 2, touches.data());
        const auto current =
            std::make_tuple(mask, static_cast<uint32_t>(system), contacts, touches[0].padX, touches[0].padY);
        if (current != self.previous) {
            self.previous = current;
            output += std::format("DualSense extras raw={:016X} system={}{:08X} contacts={}", mask,
                                  system_available ? "" : "unavailable:", static_cast<unsigned>(system), contacts);
            for (uint32_t i = 0; i < std::min(contacts, 2U); ++i)
                output += std::format(" touch[{}]=({},{})", i, touches[i].padX, touches[i].padY);
            output += '\n';
        }
        if (!output.empty())
            self.QueueOutput(output);
    }

    void Start() {
        using namespace GameInput::v4;
        if (SUCCEEDED(GameInputCreate(&input))) {
            const HRESULT hr =
                input->RegisterReadingCallback(nullptr,
                                               GameInputKindControllerButton | GameInputKindSystemButton |
                                                   GameInputKindTouch | GameInputKindRawDeviceReport,
                                               this, OnReading, &token);
            std::printf("extra callback result=0x%08lX\n", static_cast<unsigned long>(hr));
        }
    }

    bool EnableEnhanced() {
        Microsoft::WRL::ComPtr<GameInput::v4::IGameInputDevice> device;
        {
            const std::scoped_lock lock(device_mutex);
            device = observed_device;
        }
        if (!device)
            return false;
        Microsoft::WRL::ComPtr<GameInput::v4::IGameInputRawDeviceReport> firmware;
        const HRESULT hr = device->GetRawDeviceFeature(0x20, &firmware);
        std::printf("DualSense enhanced-mode firmware read=0x%08lX\n", static_cast<unsigned long>(hr));
        return true;
    }

    ~ExtraProbe() {
        if (token != 0)
            input->UnregisterCallback(token);
        FlushOutput();
    }
};

void PrintInputEnvironment() {
    wchar_t desktop[128]{};
    DWORD bytes = 0;
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &bytes);
    std::printf("desktop=%ls\n", desktop);
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0) {
        std::printf("RawInput enumeration failed: %lu\n", GetLastError());
        return;
    }
    std::vector<RAWINPUTDEVICELIST> devices(count);
    count = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (count == UINT_MAX) {
        std::printf("RawInput device list failed: %lu\n", GetLastError());
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        if (devices[i].dwType != RIM_TYPEHID)
            continue;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        if (GetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICEINFO, &info, &size) == UINT_MAX)
            continue;
        if (info.hid.usUsagePage == 1 && (info.hid.usUsage == 4 || info.hid.usUsage == 5)) {
            std::printf("RawInput controller vid=%04lX pid=%04lX usage=%u\n", info.hid.dwVendorId, info.hid.dwProductId,
                        info.hid.usUsage);
        }
    }
}

void PrintGameInputAvailability() {
    using namespace GameInput::v4;
    Microsoft::WRL::ComPtr<IGameInput> input;
    if (FAILED(GameInputCreate(&input)))
        return;
    for (const auto kind : {GameInputKindKeyboard, GameInputKindMouse, GameInputKindController, GameInputKindGamepad}) {
        Microsoft::WRL::ComPtr<IGameInputReading> reading;
        const HRESULT hr = input->GetCurrentReading(kind, nullptr, &reading);
        std::printf("GameInput kind=%08X current=0x%08lX\n", static_cast<unsigned>(kind),
                    static_cast<unsigned long>(hr));
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(GetModuleHandleW(L"GameInputRedist.dll"), path, MAX_PATH);
    std::printf("GameInput runtime=%ls\n", path);
}

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
    PrintInputEnvironment();
    int seconds = 0;
    int cycles = 1;
    bool enhanced = false;
    bool extra_readings = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-extra") == 0) {
            extra_readings = false;
            continue;
        }
        if (std::strcmp(argv[i], "--enhanced") == 0) {
            enhanced = true;
            continue;
        }
        int* target = std::strcmp(argv[i], "--seconds") == 0  ? &seconds
                      : std::strcmp(argv[i], "--cycles") == 0 ? &cycles
                                                              : nullptr;
        if (target == nullptr || i + 1 >= argc) {
            std::fprintf(stderr, "usage: probe_gamepad [--seconds N] [--cycles N] [--enhanced] [--no-extra]\n");
            return 2;
        }
        const char* value = argv[++i];
        const auto result = std::from_chars(value, value + std::strlen(value), *target);
        if (result.ec != std::errc{} || *result.ptr != '\0' || *target < 0)
            return 2;
    }
    if (cycles < 1 || (cycles > 1 && seconds < 1)) {
        std::fprintf(stderr, "--cycles requires a positive count and --seconds\n");
        return 2;
    }
    if (enhanced && !extra_readings) {
        std::fprintf(stderr, "--enhanced requires the extra reading probe\n");
        return 2;
    }

    for (int cycle = 0; cycle < cycles; ++cycle) {
        std::string error;
        const auto source = GameInputSource::Start(error);
        if (!source) {
            std::fprintf(stderr, "FAIL: %s\n", error.c_str());
            return 1;
        }
        std::printf("GameInput started; waiting for gamepad readings\n");
        ExtraProbe extras;
        if (extra_readings)
            extras.Start();
        if (cycle == 0)
            PrintGameInputAvailability();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        uint64_t last_stamp = 0;
        uint16_t last_vid = 0;
        uint16_t last_pid = 0;
        bool enhanced_requested = false;
        while (seconds <= 0 || std::chrono::steady_clock::now() < deadline) {
            extras.FlushOutput();
            if (enhanced && !enhanced_requested)
                enhanced_requested = extras.EnableEnhanced();
            const uint64_t now = GameInputSource::NowCapture100ns();
            const auto reading = source->StateAt(now);
            if (reading && reading->capture_100ns != last_stamp) {
                last_stamp = reading->capture_100ns;
                const DeviceIdentity& d = reading->device;
                if (d.vendor_id != last_vid || d.product_id != last_pid) {
                    last_vid = d.vendor_id;
                    last_pid = d.product_id;
                    std::printf("device vid=%04X pid=%04X layout=%s\n", d.vendor_id, d.product_id,
                                LayoutName(d.layout));
                }
                const GamepadState& s = reading->state;
                std::printf("%s age=%.1fms buttons=%08X L=(%+.3f,%+.3f) R=(%+.3f,%+.3f) LT=%.3f RT=%.3f\n",
                            reading->connected ? "pad " : "lost",
                            static_cast<double>(now - reading->capture_100ns) / 1e4, s.buttons, s.left_x, s.left_y,
                            s.right_x, s.right_y, s.left_trigger, s.right_trigger);
                if (s.home.value_or(false) || s.share.value_or(false) || s.touchpad.value_or(false))
                    std::printf("auxiliary home=%d share=%d touchpad=%d\n", s.home.value_or(false),
                                s.share.value_or(false), s.touchpad.value_or(false));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        std::printf("GameInput stopping (cycle %d/%d)\n", cycle + 1, cycles);
    }
    std::printf("GameInput stopped cleanly\n");
    return 0;
}

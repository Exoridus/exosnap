// probe_display_color_event -- does Windows notify a plain Win32 process when a
// display's advanced colour state changes, and is the notification usable from a
// non-UI thread?
//
// The recording engine re-reads the captured display's colour facts on a 2 s
// cadence, so a change to the Windows SDR-content brightness is applied up to
// that late. Windows.Graphics.Display.DisplayInformation raises
// AdvancedColorInfoChanged for exactly this, but the documented registration is
// per CoreWindow, which this process does not have. The Win32 route is
// IDisplayInformationStaticsInterop::GetForMonitor, whose remarks add two
// conditions worth proving before anything depends on them:
//
//   * "If you wish to register for events, then the current thread must have a
//     Windows.System.DispatcherQueue running [...] That DispatcherQueue will be
//     snapped upon the call to GetForMonitor."
//   * "The current thread can be MTA or STA."
//
// So the probe answers, on this machine:
//   1. Is the interop static reachable at all (it needs Windows 11 build 22621)?
//   2. Does registration succeed on a self-created DispatcherQueue thread?
//   3. Does the handler actually run, and how long after the stimulus?
//   4. Which SdrWhiteLevelInNits does the event carry, against the
//      DISPLAYCONFIG value the engine reads today?
//
// Stimulus: toggling the display's HDR changes the advanced colour state, so
// `exosnap-envctl` can drive this run without a person at the keyboard. Moving
// the SDR-content-brightness slider is the other stimulus and needs one.
//
// Usage: probe_display_color_event [seconds]   (default 60)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <dispatcherqueue.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.System.h>

#include <windows.graphics.display.interop.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct MonitorRow {
    HMONITOR handle = nullptr;
    std::wstring device;
};

BOOL CALLBACK CollectMonitor(HMONITOR hmon, HDC, LPRECT, LPARAM ctx) {
    auto* rows = reinterpret_cast<std::vector<MonitorRow>*>(ctx);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi) != FALSE) {
        rows->push_back({hmon, mi.szDevice});
    }
    return TRUE;
}

// The SDR content brightness the engine reads today (DISPLAYCONFIG_SDR_WHITE_LEVEL,
// raw 1000 == 80 nits). Printed next to the event's own value so the two can be
// compared rather than assumed equal.
float QuerySdrWhiteLevelNits(HMONITOR hmonitor) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmonitor, &mi) == FALSE) {
        return 0.0f;
    }
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return 0.0f;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
        return 0.0f;
    }
    paths.resize(path_count);
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        if (wcscmp(source.viewGdiDeviceName, mi.szDevice) != 0) {
            continue;
        }
        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = path.targetInfo.adapterId;
        white.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS) {
            return 0.0f;
        }
        return static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;
    }
    return 0.0f;
}

const char* KindName(winrt::Windows::Graphics::Display::AdvancedColorKind kind) {
    using winrt::Windows::Graphics::Display::AdvancedColorKind;
    switch (kind) {
    case AdvancedColorKind::StandardDynamicRange:
        return "sdr";
    case AdvancedColorKind::WideColorGamut:
        return "wcg";
    case AdvancedColorKind::HighDynamicRange:
        return "hdr";
    }
    return "?";
}

std::atomic<uint32_t> g_events{0};

} // namespace

int wmain(int argc, wchar_t** argv) {
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 60;

    std::vector<MonitorRow> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) {
        std::printf("no monitors enumerated\n");
        return 2;
    }

    // MTA on purpose: the remarks allow either, and the engine thread that would
    // eventually own this is not an STA. Proving it here is the point.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // The DispatcherQueue the registration is about to be snapped onto. It only
    // dispatches while this thread pumps messages, which the loop below does.
    DispatcherQueueOptions options{};
    options.dwSize = sizeof(options);
    options.threadType = DQTYPE_THREAD_CURRENT;
    options.apartmentType = DQTAT_COM_NONE;
    winrt::Windows::System::DispatcherQueueController controller{nullptr};
    HRESULT hr = CreateDispatcherQueueController(
        options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller)));
    if (FAILED(hr)) {
        std::printf("CreateDispatcherQueueController failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return 3;
    }
    std::printf("dispatcher queue: created on this thread (MTA)\n");

    auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Display::DisplayInformation,
                                                 IDisplayInformationStaticsInterop>();
    std::printf("interop static: available\n");

    struct Watch {
        std::wstring device;
        winrt::Windows::Graphics::Display::DisplayInformation info{nullptr};
        winrt::event_token token{};
    };
    std::vector<Watch> watches;

    for (const MonitorRow& row : monitors) {
        Watch w;
        w.device = row.device;
        hr = interop->GetForMonitor(row.handle, winrt::guid_of<winrt::Windows::Graphics::Display::DisplayInformation>(),
                                    winrt::put_abi(w.info));
        if (FAILED(hr)) {
            std::printf("GetForMonitor(%ls) failed 0x%08lX\n", row.device.c_str(), static_cast<unsigned long>(hr));
            continue;
        }
        const auto aci = w.info.GetAdvancedColorInfo();
        std::printf("%ls: kind=%s event_sdr_white=%.1f displayconfig_sdr_white=%.1f\n", row.device.c_str(),
                    KindName(aci.CurrentAdvancedColorKind()), aci.SdrWhiteLevelInNits(),
                    QuerySdrWhiteLevelNits(row.handle));

        const HMONITOR handle = row.handle;
        const std::wstring device = row.device;
        const auto t0 = std::chrono::steady_clock::now();
        try {
            w.token = w.info.AdvancedColorInfoChanged(
                [handle, device, t0](winrt::Windows::Graphics::Display::DisplayInformation const& sender,
                                     winrt::Windows::Foundation::IInspectable const&) {
                    const auto aci = sender.GetAdvancedColorInfo();
                    const double at_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    std::printf("[%7.3fs] EVENT %ls kind=%s event_sdr_white=%.1f displayconfig_sdr_white=%.1f "
                                "thread=%lu\n",
                                at_s, device.c_str(), KindName(aci.CurrentAdvancedColorKind()),
                                aci.SdrWhiteLevelInNits(), QuerySdrWhiteLevelNits(handle), GetCurrentThreadId());
                    std::fflush(stdout);
                    g_events.fetch_add(1, std::memory_order_relaxed);
                });
        } catch (const winrt::hresult_error& e) {
            // The documented failure when no DispatcherQueue was snapped. Reaching
            // it here would mean the queue above is not what the call wanted.
            std::printf("AdvancedColorInfoChanged(%ls) registration failed 0x%08lX: %ls\n", row.device.c_str(),
                        static_cast<unsigned long>(e.code()), e.message().c_str());
            continue;
        }
        std::printf("%ls: subscribed\n", row.device.c_str());
        watches.push_back(std::move(w));
    }

    if (watches.empty()) {
        std::printf("RESULT: no subscription succeeded\n");
        return 4;
    }

    std::printf("main thread=%lu -- pumping for %d s; change HDR or the SDR content brightness now\n",
                GetCurrentThreadId(), seconds);
    std::fflush(stdout);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    MSG msg{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }

    for (Watch& w : watches) {
        w.info.AdvancedColorInfoChanged(w.token);
    }
    watches.clear();

    // ShutdownQueueAsync completes only while this thread keeps pumping, because
    // this is the thread the queue dispatches on. Blocking on the returned
    // operation deadlocks it against itself: the completion it waits for can only
    // run on the thread it just stopped pumping. Pump until the shutdown posts
    // the quit instead, which is also what lets an in-flight handler finish
    // rather than be torn out from under itself.
    controller.DispatcherQueue().ShutdownCompleted(
        [](winrt::Windows::System::DispatcherQueue const&, winrt::Windows::Foundation::IInspectable const&) {
            PostQuitMessage(0);
        });
    controller.ShutdownQueueAsync();
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    const uint32_t n = g_events.load(std::memory_order_relaxed);
    std::printf("RESULT: %u event(s) in %d s\n", n, seconds);
    return n > 0 ? 0 : 1;
}

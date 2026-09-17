// Desktop Duplication acquisition cadence, measured against a stimulus this
// process controls.
//
// The open question is whether DXGI Desktop Duplication can hand over unique
// desktop frames faster than about 60 per second on a 144 Hz display, or
// whether the ceiling earlier measurements found belongs to the API rather
// than to how the product drives it. Two controls, both here:
//
//   * The acquisition loop, calling AcquireNextFrame with a zero timeout --
//     what the product already does -- and counting UNIQUE presents rather
//     than successful calls. Duplication answers a call with the desktop as it
//     stands, so the same image returned twice is one frame, not two;
//     LastPresentTime is what tells them apart.
//   * Frame ownership. `--hold-us N` keeps the frame for N microseconds before
//     ReleaseFrame, which separates a cadence that depends on how long the
//     consumer owns the surface from one that does not.
//
// The stimulus is a borderless window this process presents to at the
// display's refresh rate, with a different colour every present. It lives in
// the same process on purpose: a measurement whose stimulus is whatever
// happened to be on screen measures the screen rather than the API. The window
// never takes focus and goes away with the probe.
//
// No judgement is made here. The probe prints what it counted.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    int seconds = 10;
    int output_index = 0;
    int64_t hold_us = 0;
    bool stimulus = true;
    // Present the stimulus and measure nothing. What a capture-loss run needs:
    // something that keeps changing on the display under test, while the product
    // -- not this probe -- is the one capturing it.
    bool stimulus_only = false;
};

bool ParseOptions(int argc, char** argv, Options* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        int64_t parsed = 0;
        const auto take_value = [&]() -> bool {
            if (i + 1 >= argc)
                return false;
            parsed = std::strtoll(argv[++i], nullptr, 10);
            return true;
        };
        if (arg == "--seconds") {
            if (!take_value())
                return false;
            out->seconds = static_cast<int>(parsed);
        } else if (arg == "--output") {
            if (!take_value())
                return false;
            out->output_index = static_cast<int>(parsed);
        } else if (arg == "--hold-us") {
            if (!take_value())
                return false;
            out->hold_us = parsed;
        } else if (arg == "--no-stimulus") {
            out->stimulus = false;
        } else if (arg == "--stimulus-only") {
            out->stimulus_only = true;
        } else {
            std::printf("usage: probe_ddx_acquire [--seconds N] [--output N] [--hold-us N] [--no-stimulus] "
                        "[--stimulus-only]\n");
            return false;
        }
    }
    return out->seconds > 0;
}

struct OutputTarget {
    winrt::com_ptr<IDXGIAdapter1> adapter;
    winrt::com_ptr<IDXGIOutput1> output;
    DXGI_OUTPUT_DESC desc{};
};

// The nth output across all adapters, in enumeration order, together with the
// adapter that owns it: duplication only works on a device created on that same
// adapter.
bool FindOutput(int wanted, OutputTarget* out) {
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void())))
        return false;

    int seen = 0;
    for (UINT a = 0;; ++a) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.put()) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT o = 0;; ++o) {
            winrt::com_ptr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.put()) == DXGI_ERROR_NOT_FOUND)
                break;
            if (seen++ != wanted)
                continue;
            out->adapter = adapter;
            out->output = output.as<IDXGIOutput1>();
            out->output->GetDesc(&out->desc);
            return true;
        }
    }
    return false;
}

// A borderless, non-activating window presented to at the display's refresh
// rate, with a different colour each time. Present(1, 0) rather than a free
// run: a present the compositor never showed is not a desktop frame, and
// counting duplication against those would measure the swap chain instead.
class Stimulus {
  public:
    void Start(RECT rect, IDXGIOutput1* output, std::atomic<bool>* stop) {
        stop_ = stop;
        output_ = output;
        thread_ = std::thread([this, rect] { Run(rect); });
    }

    void Join() {
        if (thread_.joinable())
            thread_.join();
    }

    [[nodiscard]] uint64_t Presents() const noexcept {
        return presents_.load(std::memory_order_relaxed);
    }

  private:
    void Run(RECT rect) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = L"ExoSnapDdxStimulus";
        RegisterClassExW(&wc);

        // A quarter of the output in its top-left corner: big enough that the
        // compositor cannot treat it as nothing, small enough to leave the
        // screen usable while the probe runs.
        const LONG width = std::max<LONG>(320, (rect.right - rect.left) / 2);
        const LONG height = std::max<LONG>(240, (rect.bottom - rect.top) / 2);
        const HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"ddx stimulus",
                                            WS_POPUP, rect.left, rect.top, static_cast<int>(width),
                                            static_cast<int>(height), nullptr, nullptr, instance, nullptr);
        if (window == nullptr)
            return;
        ShowWindow(window, SW_SHOWNOACTIVATE);

        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                     levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(),
                                     nullptr, context.put()))) {
            DestroyWindow(window);
            return;
        }

        auto dxgi_device = device.as<IDXGIDevice>();
        winrt::com_ptr<IDXGIAdapter> adapter;
        dxgi_device->GetAdapter(adapter.put());
        winrt::com_ptr<IDXGIFactory2> factory;
        adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

        winrt::com_ptr<IDXGISwapChain1> swap_chain;
        if (SUCCEEDED(
                factory->CreateSwapChainForHwnd(device.get(), window, &desc, nullptr, nullptr, swap_chain.put()))) {
            uint32_t tick = 0;
            while (!stop_->load(std::memory_order_relaxed)) {
                winrt::com_ptr<ID3D11Texture2D> back_buffer;
                if (SUCCEEDED(swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), back_buffer.put_void()))) {
                    winrt::com_ptr<ID3D11RenderTargetView> rtv;
                    if (SUCCEEDED(device->CreateRenderTargetView(back_buffer.get(), nullptr, rtv.put()))) {
                        const float colour[4] = {static_cast<float>(tick % 256) / 255.0f,
                                                 static_cast<float>((tick * 7) % 256) / 255.0f,
                                                 static_cast<float>((tick * 13) % 256) / 255.0f, 1.0f};
                        context->ClearRenderTargetView(rtv.get(), colour);
                    }
                }
                if (FAILED(swap_chain->Present(1, 0)))
                    break;
                presents_.fetch_add(1, std::memory_order_relaxed);
                ++tick;
                if ((tick % 64) == 0)
                    FollowOutput(window, &rect);

                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        }

        DestroyWindow(window);
    }

    // Where the window has to be. A display that leaves and comes back returns
    // with a desktop rectangle of its own, and Windows does not move windows
    // back onto it -- a stimulus that stayed put would leave the display under
    // test blank, which is exactly the ambiguity a capture-loss run must avoid.
    void FollowOutput(HWND window, RECT* current) {
        if (output_ == nullptr)
            return;
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output_->GetDesc(&desc)))
            return;
        const RECT& r = desc.DesktopCoordinates;
        if (r.left == current->left && r.top == current->top && r.right == current->right &&
            r.bottom == current->bottom)
            return;
        *current = r;
        const LONG width = std::max<LONG>(320, (r.right - r.left) / 2);
        const LONG height = std::max<LONG>(240, (r.bottom - r.top) / 2);
        SetWindowPos(window, HWND_TOP, r.left, r.top, static_cast<int>(width), static_cast<int>(height),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    std::atomic<bool>* stop_ = nullptr;
    IDXGIOutput1* output_ = nullptr;
    std::atomic<uint64_t> presents_{0};
    std::thread thread_;
};

struct Counters {
    uint64_t calls = 0;
    uint64_t acquired = 0;
    uint64_t timeouts = 0;
    uint64_t unique_presents = 0;
    uint64_t accumulated_frames = 0;
    uint64_t access_lost = 0;
    double acquire_max_ms = 0.0;
    double acquire_total_ms = 0.0;
};

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options))
        return 2;

    OutputTarget target;
    if (!FindOutput(options.output_index, &target)) {
        std::printf("no output %d\n", options.output_index);
        return 3;
    }

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(target.adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, static_cast<UINT>(std::size(levels)),
                                 D3D11_SDK_VERSION, device.put(), nullptr, context.put()))) {
        std::printf("D3D11CreateDevice failed\n");
        return 4;
    }

    winrt::com_ptr<IDXGIOutputDuplication> duplication;
    if (FAILED(target.output->DuplicateOutput(device.get(), duplication.put()))) {
        std::printf("DuplicateOutput failed\n");
        return 5;
    }

    char device_name[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, target.desc.DeviceName, -1, device_name, sizeof(device_name) - 1, nullptr, nullptr);
    std::printf("output=%s rect=%ldx%ld hold_us=%lld stimulus=%s seconds=%d\n", device_name,
                target.desc.DesktopCoordinates.right - target.desc.DesktopCoordinates.left,
                target.desc.DesktopCoordinates.bottom - target.desc.DesktopCoordinates.top,
                static_cast<long long>(options.hold_us), options.stimulus ? "on" : "off", options.seconds);

    std::atomic<bool> stop{false};
    Stimulus stimulus;
    if (options.stimulus)
        stimulus.Start(target.desc.DesktopCoordinates, target.output.get(), &stop);

    if (options.stimulus_only) {
        std::this_thread::sleep_for(std::chrono::seconds(options.seconds));
        stop.store(true, std::memory_order_relaxed);
        stimulus.Join();
        std::printf("RESULT: stimulus only, %llu presents in %d s\n",
                    static_cast<unsigned long long>(stimulus.Presents()), options.seconds);
        return 0;
    }

    Counters counters;
    int64_t last_present_time = 0;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds(options.seconds);

    while (Clock::now() < deadline) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> resource;
        const auto call_start = Clock::now();
        const HRESULT hr = duplication->AcquireNextFrame(0, &info, resource.put());
        const double call_ms = std::chrono::duration<double, std::milli>(Clock::now() - call_start).count();
        ++counters.calls;
        counters.acquire_total_ms += call_ms;
        counters.acquire_max_ms = std::max(counters.acquire_max_ms, call_ms);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            ++counters.timeouts;
            continue;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            ++counters.access_lost;
            duplication = nullptr;
            if (FAILED(target.output->DuplicateOutput(device.get(), duplication.put())))
                break;
            last_present_time = 0;
            continue;
        }
        if (FAILED(hr)) {
            std::printf("AcquireNextFrame failed 0x%08lx\n", static_cast<unsigned long>(hr));
            break;
        }

        ++counters.acquired;
        counters.accumulated_frames += info.AccumulatedFrames;
        // A desktop image that has not been presented since the last one is the
        // same image: the API answered, the desktop did not change.
        if (info.LastPresentTime.QuadPart != 0 && info.LastPresentTime.QuadPart != last_present_time) {
            ++counters.unique_presents;
            last_present_time = info.LastPresentTime.QuadPart;
        }

        if (options.hold_us > 0) {
            const auto hold_until = Clock::now() + std::chrono::microseconds(options.hold_us);
            while (Clock::now() < hold_until) {
            }
        }
        resource = nullptr;
        duplication->ReleaseFrame();
    }

    stop.store(true, std::memory_order_relaxed);
    stimulus.Join();
    if (duplication)
        duplication = nullptr;

    const double elapsed_s = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("calls=%llu acquired=%llu timeouts=%llu unique_presents=%llu accumulated=%llu access_lost=%llu\n",
                static_cast<unsigned long long>(counters.calls), static_cast<unsigned long long>(counters.acquired),
                static_cast<unsigned long long>(counters.timeouts),
                static_cast<unsigned long long>(counters.unique_presents),
                static_cast<unsigned long long>(counters.accumulated_frames),
                static_cast<unsigned long long>(counters.access_lost));
    std::printf("acquire_mean_ms=%.4f acquire_max_ms=%.3f stimulus_presents=%llu (%.1f/s)\n",
                counters.calls > 0 ? counters.acquire_total_ms / static_cast<double>(counters.calls) : 0.0,
                counters.acquire_max_ms, static_cast<unsigned long long>(stimulus.Presents()),
                elapsed_s > 0.0 ? static_cast<double>(stimulus.Presents()) / elapsed_s : 0.0);
    std::printf("RESULT: %.1f unique presents/s over %.2f s (%.1f acquisitions/s)\n",
                elapsed_s > 0.0 ? static_cast<double>(counters.unique_presents) / elapsed_s : 0.0, elapsed_s,
                elapsed_s > 0.0 ? static_cast<double>(counters.acquired) / elapsed_s : 0.0);
    return 0;
}

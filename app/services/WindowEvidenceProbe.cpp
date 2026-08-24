#include "WindowEvidenceProbe.h"

#include "services/CaptureHubRegistry.h"
#include "services/CaptureSourceKey.h"
#include "services/WgcSourceProducer.h"

#include <chrono>
#include <memory>

#include <windows.h>

#include <d3d11.h>

namespace exosnap {
namespace {

using Clock = std::chrono::steady_clock;

// Worker wake cadence — services the apartment message queue (WGC item.Closed).
constexpr auto kTick = std::chrono::milliseconds(50);
// Each hub step copies the source; a probe is not a preview, so step modestly.
constexpr auto kPumpInterval = std::chrono::milliseconds(100);
// Window facts (geometry/style/QUNS) change slowly; poll ~1 Hz.
constexpr auto kFactsInterval = std::chrono::milliseconds(1000);

} // namespace

WindowEvidenceProbe::WindowEvidenceProbe() {
    worker_ = std::jthread([this](std::stop_token st) { WorkerMain(st); });
}

WindowEvidenceProbe::~WindowEvidenceProbe() {
    if (worker_.joinable()) {
        worker_.request_stop();
        cv_.notify_one();
        worker_.join();
    }
}

void WindowEvidenceProbe::SetWindowTarget(uintptr_t hwnd) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_hwnd_ = hwnd;
        pending_dirty_ = true;
    }
    cv_.notify_one();
}

void WindowEvidenceProbe::SetPaused(bool paused) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused_ = paused;
    }
    cv_.notify_one();
}

WindowEvidenceProbe::Snapshot WindowEvidenceProbe::CurrentSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void WindowEvidenceProbe::WorkerMain(std::stop_token stop_token) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_inited = SUCCEEDED(co) || co == RPC_E_CHANGED_MODE;
    if (!com_inited)
        return;

    winrt::com_ptr<ID3D11Device> device;
    {
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const HRESULT hr =
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                              static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
        if (FAILED(hr) || !device) {
            // No device, no measured evidence. The snapshot stays inactive, which
            // resolves to ExclusiveEvidence::None — nothing measured, nothing
            // proven, and the admission gate never blocks on a guess.
            if (co != RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }
    }

    CaptureHubRegistry registry([device](const CaptureSourceKey& key) -> std::unique_ptr<HubSourceProducer> {
        return std::make_unique<WgcSourceProducer>(key, device);
    });

    // The single live subscription and its accumulator. Everything WGC lives and
    // dies on this thread.
    uintptr_t current_hwnd = 0;
    CaptureSubscription subscription;
    diagnostics::WindowEvidenceAccumulator accumulator;

    Clock::time_point last_pump{};
    Clock::time_point last_facts{};
    diagnostics::WindowShape last_shape = diagnostics::WindowShape::Normal;
    diagnostics::WindowTargetFacts last_facts_snapshot;

    while (!stop_token.stop_requested()) {
        uintptr_t want_hwnd = current_hwnd;
        bool dirty = false;
        bool paused = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, kTick, [&] { return pending_dirty_ || stop_token.stop_requested(); });
            if (pending_dirty_) {
                want_hwnd = pending_hwnd_;
                pending_dirty_ = false;
                dirty = true;
            }
            paused = paused_;
        }
        if (stop_token.stop_requested())
            break;

        const Clock::time_point now = Clock::now();

        if (dirty && want_hwnd != current_hwnd) {
            subscription.Reset(); // unsubscribe the old window (WGC dies here)
            current_hwnd = want_hwnd;
            if (current_hwnd != 0) {
                CaptureSourceKey key{CaptureSourceKey::Kind::Window, current_hwnd, {}};
                subscription = registry.Subscribe(key, [](const HubFrame&, exosnap::engine::HubFrameKind) {});
                accumulator.Reset(now);
            }
            // A target switch invalidates every accumulated fact at once: the old
            // window's evidence must never survive one tick into the new target.
            last_facts = {};
            last_shape = diagnostics::WindowShape::Normal;
            last_facts_snapshot = {};
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.active = current_hwnd != 0;
            snapshot_.hwnd = current_hwnd;
            snapshot_.facts = last_facts_snapshot;
            snapshot_.evidence = current_hwnd != 0 ? accumulator.Evidence(now) : diagnostics::WindowHubEvidence{};
        }

        // Pump the apartment queue so item.Closed is observed.
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (current_hwnd == 0)
            continue;

        // Refresh window facts (~1 Hz) — cheap Win32, drives the shape correlation.
        if (now - last_facts >= kFactsInterval) {
            last_facts = now;
            last_facts_snapshot = diagnostics::GatherWindowTargetFacts(reinterpret_cast<HWND>(current_hwnd));
            last_shape = diagnostics::ClassifyWindowShape(last_facts_snapshot);
        }

        // Step the capture unless paused (recording owns it); the subscription is
        // kept so the held frame and accumulated evidence survive the pause.
        if (!paused && now - last_pump >= kPumpInterval) {
            last_pump = now;
            registry.PumpAll();
        }

        accumulator.Update(now, subscription.Frame(), subscription.HeldFrame().generation, last_shape);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.active = true;
            snapshot_.hwnd = current_hwnd;
            snapshot_.facts = last_facts_snapshot;
            snapshot_.evidence = accumulator.Evidence(now);
        }
    }

    subscription.Reset(); // subscription dies before the registry, on this thread
    if (co != RPC_E_CHANGED_MODE)
        CoUninitialize();
}

} // namespace exosnap

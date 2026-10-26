#include "gamepad/gamepad.h"

#include "qpc_100ns.h"

#include <windows.h>
#include <wrl/client.h>

#include <GameInput.h>

#include <algorithm>
#include <array>
#include <cstdio>

using Microsoft::WRL::ComPtr;
using namespace GameInput::v4;

namespace exosnap::gamepad {

namespace {

HRESULT AcquireRuntime(ComPtr<IGameInput>& input) {
    static std::mutex mutex;
    // shortcut: v4 can deadlock on its final Release after reading callbacks;
    // retain one process reference until a runtime supports clean final release.
    static IGameInput* runtime = nullptr;
    const std::scoped_lock lock(mutex);
    if (runtime == nullptr) {
        const HRESULT hr = GameInputCreate(&runtime);
        if (FAILED(hr)) {
            return hr;
        }
    }
    input = runtime;
    return S_OK;
}

} // namespace

struct GameInputSource::Impl {
    ComPtr<IGameInput> input;
    ClockAnchor anchor;
    DeviceHistory history;
    GameInputCallbackToken reading_token = 0;
    GameInputCallbackToken device_token = 0;

    static DeviceIdentity IdentityOf(IGameInputDevice* device) {
        DeviceIdentity identity;
        if (device != nullptr) {
            identity.vendor_id = device->GetVendorId();
            identity.product_id = device->GetProductId();
            APP_LOCAL_DEVICE_ID id{};
            device->GetDeviceId(&id);
            std::copy(std::begin(id.value), std::end(id.value), identity.id.begin());
            identity.layout = LayoutKindFor(identity.vendor_id, identity.product_id);
        }
        return identity;
    }

    void PushReading(IGameInputReading* reading) {
        if (reading == nullptr) {
            return;
        }
        ComPtr<IGameInputDevice> device;
        reading->GetDevice(&device);
        if (!device || (device->GetSupportedInput() & GameInputKindGamepad) == 0)
            return;
        GamepadReading out;
        out.capture_100ns = GameInputToCapture100ns(reading->GetTimestamp(), anchor);
        out.device = IdentityOf(device.Get());
        const auto previous = history.StateFor(out.device.id, out.capture_100ns);
        if (previous && previous->connected)
            out.state = previous->state;
        GameInputGamepadState raw{};
        if (reading->GetGamepadState(&raw)) {
            out.state.buttons = static_cast<uint32_t>(raw.buttons);
            out.state.left_x = raw.leftThumbstickX;
            out.state.left_y = raw.leftThumbstickY;
            out.state.right_x = raw.rightThumbstickX;
            out.state.right_y = raw.rightThumbstickY;
            out.state.left_trigger = raw.leftTrigger;
            out.state.right_trigger = raw.rightTrigger;
            out.state = Normalized(out.state);
        } else if (!previous || !previous->connected) {
            return;
        }
        GameInputSystemButtons system{};
        if (reading->GetSystemButtonState(&system)) {
            out.state.home = (system & GameInputSystemButtonGuide) != 0;
            out.state.share = (system & GameInputSystemButtonShare) != 0;
        }
        if (out.device.layout == LayoutKind::DualSense) {
            // DualSense HID ordering puts PS and touchpad after the twelve common buttons.
            std::array<bool, 14> buttons{};
            if (reading->GetControllerButtonState(static_cast<uint32_t>(buttons.size()), buttons.data()) >= 14) {
                out.state.home = buttons[12];
                out.state.touchpad = buttons[13];
            }
        }
        history.Push(out);
    }

    void PushCurrent(IGameInputDevice* device) {
        ComPtr<IGameInputReading> reading;
        if (SUCCEEDED(input->GetCurrentReading(GameInputKindGamepad, device, &reading))) {
            PushReading(reading.Get());
        }
    }

    static void CALLBACK OnReading(GameInputCallbackToken /*token*/, void* context, IGameInputReading* reading) {
        static_cast<Impl*>(context)->PushReading(reading);
    }

    static void CALLBACK OnDevice(GameInputCallbackToken /*token*/, void* context, GameInputDeviceEvent event,
                                  IGameInputDevice* device) {
        auto* self = static_cast<Impl*>(context);
        if (event == GameInputDeviceEventConnected) {
            // Reading callbacks only report changes; a pad held still since it
            // connected would otherwise have no state at all.
            self->PushCurrent(device);
        } else if (event == GameInputDeviceEventDisconnected) {
            GamepadReading out;
            out.capture_100ns = GameInputToCapture100ns(self->input->GetCurrentTimestamp(), self->anchor);
            out.connected = false;
            out.device = IdentityOf(device);
            self->history.Push(out);
        }
    }

    ~Impl() {
        // UnregisterCallback waits for a running invocation, so `this` stays
        // valid for every callback that can still observe it.
        if (device_token != 0) {
            input->UnregisterCallback(device_token);
        }
        if (reading_token != 0) {
            input->UnregisterCallback(reading_token);
        }
    }
};

uint64_t GameInputSource::NowCapture100ns() noexcept {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }();
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return engine::QpcTicksTo100ns(static_cast<uint64_t>(ticks.QuadPart), frequency);
}

std::unique_ptr<GameInputSource> GameInputSource::Start(std::string& error) {
    auto impl = std::make_unique<Impl>();
    const HRESULT created = AcquireRuntime(impl->input);
    if (FAILED(created)) {
        char text[96];
        std::snprintf(text, sizeof(text), "GameInputCreate failed (0x%08lX)", static_cast<unsigned long>(created));
        error = text;
        return nullptr;
    }
    // A recorder reads the pad while the game, not ExoSnap, has focus.
    impl->input->EnableBackgroundInput(true);

    // Bracket the GameInput sample with two counter reads and take the midpoint.
    const uint64_t before = NowCapture100ns();
    impl->anchor.gameinput_us = impl->input->GetCurrentTimestamp();
    const uint64_t after = NowCapture100ns();
    impl->anchor.capture_100ns = before + (after - before) / 2;

    HRESULT hr = impl->input->RegisterReadingCallback(
        nullptr, GameInputKindGamepad | GameInputKindControllerButton | GameInputKindSystemButton, impl.get(),
        &Impl::OnReading, &impl->reading_token);
    if (SUCCEEDED(hr)) {
        hr = impl->input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, impl.get(), &Impl::OnDevice,
                                                 &impl->device_token);
    }
    if (FAILED(hr)) {
        char text[96];
        std::snprintf(text, sizeof(text), "GameInput callback registration failed (0x%08lX)",
                      static_cast<unsigned long>(hr));
        error = text;
        return nullptr;
    }
    // Registered first so no change between this read and the callback is lost.
    impl->PushCurrent(nullptr);
    return std::unique_ptr<GameInputSource>(new GameInputSource(std::move(impl)));
}

GameInputSource::GameInputSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}

GameInputSource::~GameInputSource() = default;

std::optional<GamepadReading> GameInputSource::StateAt(uint64_t capture_100ns) const {
    return impl_->history.StateAt(capture_100ns);
}

void GameInputSource::SelectDevice(std::optional<DeviceId> device) {
    impl_->history.Select(device);
}

std::vector<DeviceIdentity> GameInputSource::Devices() const {
    return impl_->history.Devices();
}

} // namespace exosnap::gamepad

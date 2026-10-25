#include "gamepad/gamepad.h"

#include "qpc_100ns.h"

#include <windows.h>
#include <wrl/client.h>

#include <GameInput.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;
using namespace GameInput::v4;

namespace exosnap::gamepad {

struct GameInputSource::Impl {
    ComPtr<IGameInput> input;
    ClockAnchor anchor;
    ReadingRing ring;
    GameInputCallbackToken reading_token = 0;
    GameInputCallbackToken device_token = 0;

    static DeviceIdentity IdentityOf(IGameInputDevice* device) {
        DeviceIdentity identity;
        if (device != nullptr) {
            identity.vendor_id = device->GetVendorId();
            identity.product_id = device->GetProductId();
            identity.layout = LayoutKindFor(identity.vendor_id, identity.product_id);
        }
        return identity;
    }

    void PushReading(IGameInputReading* reading) {
        GameInputGamepadState raw{};
        if (reading == nullptr || !reading->GetGamepadState(&raw)) {
            return;
        }
        ComPtr<IGameInputDevice> device;
        reading->GetDevice(&device);
        GamepadReading out;
        out.capture_100ns = GameInputToCapture100ns(reading->GetTimestamp(), anchor);
        out.device = IdentityOf(device.Get());
        out.state = Normalized(GamepadState{
            .buttons = static_cast<uint32_t>(raw.buttons),
            .left_x = raw.leftThumbstickX,
            .left_y = raw.leftThumbstickY,
            .right_x = raw.rightThumbstickX,
            .right_y = raw.rightThumbstickY,
            .left_trigger = raw.leftTrigger,
            .right_trigger = raw.rightTrigger,
        });
        ring.Push(out);
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
            self->ring.Push(out);
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
        // shortcut: with GameInputRedist 4.0.302 loaded application-local, the
        // final Release blocks forever once a reading callback was ever
        // registered, even after it is unregistered. The interface is left to
        // process teardown instead; drop this once a runtime releases cleanly.
        if (reading_token != 0) {
            input.Detach();
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
    const HRESULT created = GameInputCreate(&impl->input);
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

    HRESULT hr = impl->input->RegisterReadingCallback(nullptr, GameInputKindGamepad, impl.get(), &Impl::OnReading,
                                                      &impl->reading_token);
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
    return impl_->ring.StateAt(capture_100ns);
}

} // namespace exosnap::gamepad

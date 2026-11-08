#include "gamepad/gamepad.h"
#include "gamepad/sdl_gamepad_state.h"
#include "qpc_100ns.h"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_version.h>
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace exosnap::gamepad {
namespace {
std::atomic<DWORD> input_main_thread{0};
std::atomic<bool> input_owner_active{false};

struct RuntimeModule {
    HMODULE handle = nullptr;
    bool qualified = false;
    ~RuntimeModule() {
        if (handle)
            FreeLibrary(handle);
    }
};

bool LoadRuntime(std::string& error) {
    // Delay-import function pointers remain valid across provider restarts.
    static RuntimeModule module;
    if (module.qualified)
        return true;
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        error = "Cannot resolve the application directory";
        return false;
    }
    path.resize(length);
    const auto separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        error = "Cannot resolve the application directory";
        return false;
    }
    path.resize(separator + 1);
    path += L"SDL3.dll";
    module.handle =
        LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module.handle) {
        error = "Cannot load the packaged SDL3 runtime (Windows error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    const auto get_version =
        reinterpret_cast<decltype(&SDL_GetVersion)>(GetProcAddress(module.handle, "SDL_GetVersion"));
    if (!get_version || get_version() != SDL_VERSION || !GetProcAddress(module.handle, "SDL_SetMainReady")) {
        error = "The packaged SDL3 runtime version does not match this application";
        FreeLibrary(module.handle);
        module.handle = nullptr;
        return false;
    }
    module.qualified = true;
    SDL_SetMainReady();
    return true;
}

struct SavedHint {
    const char* name = nullptr;
    std::optional<std::string> value;

    explicit SavedHint(const char* key) : name(key) {
        if (const char* current = SDL_GetHint(key))
            value = current;
    }
    void Restore() const {
        if (value)
            SDL_SetHintWithPriority(name, value->c_str(), SDL_HINT_OVERRIDE);
        else
            SDL_ResetHint(name);
    }
};

struct JoystickLock {
    JoystickLock() {
        SDL_LockJoysticks();
    }
    ~JoystickLock() {
        SDL_UnlockJoysticks();
    }
};

constexpr const char* hid_driver_hints[]{SDL_HINT_JOYSTICK_HIDAPI,
                                         SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE,
                                         SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS,
                                         SDL_HINT_JOYSTICK_HIDAPI_LUNA,
                                         SDL_HINT_JOYSTICK_HIDAPI_NINTENDO_CLASSIC,
                                         SDL_HINT_JOYSTICK_HIDAPI_PS3,
                                         SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER,
                                         SDL_HINT_JOYSTICK_HIDAPI_PS4,
                                         SDL_HINT_JOYSTICK_HIDAPI_PS5,
                                         SDL_HINT_JOYSTICK_HIDAPI_SHIELD,
                                         SDL_HINT_JOYSTICK_HIDAPI_STADIA,
                                         SDL_HINT_JOYSTICK_HIDAPI_STEAM,
                                         SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK,
                                         SDL_HINT_JOYSTICK_HIDAPI_STEAM_HORI,
                                         SDL_HINT_JOYSTICK_HIDAPI_LG4FF,
                                         SDL_HINT_JOYSTICK_HIDAPI_8BITDO,
                                         SDL_HINT_JOYSTICK_HIDAPI_SINPUT,
                                         SDL_HINT_JOYSTICK_HIDAPI_ZUIKI,
                                         SDL_HINT_JOYSTICK_HIDAPI_FLYDIGI,
                                         SDL_HINT_JOYSTICK_HIDAPI_SWITCH,
                                         SDL_HINT_JOYSTICK_HIDAPI_SWITCH2,
                                         SDL_HINT_JOYSTICK_HIDAPI_WII,
                                         SDL_HINT_JOYSTICK_HIDAPI_XBOX,
                                         SDL_HINT_JOYSTICK_HIDAPI_XBOX_360,
                                         SDL_HINT_JOYSTICK_HIDAPI_XBOX_360_WIRELESS,
                                         SDL_HINT_JOYSTICK_HIDAPI_XBOX_ONE};

DeviceId SessionIdentity() {
    DeviceId id{};
    if (BCryptGenRandom(nullptr, id.data(), static_cast<ULONG>(id.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("Cannot allocate an unambiguous controller session identity");
    return id;
}
} // namespace

struct SdlGamepadSource::Impl {
    struct Device {
        SDL_Gamepad* handle = nullptr;
        DeviceIdentity identity;
        std::optional<DeviceId> physical_id;
        SdlGamepadSample sample;
        uint64_t snapshot_ticks_ns = 0;
        uint64_t snapshot_capture_100ns = 0;
    };

    std::shared_ptr<DeviceHistory> history = std::make_shared<DeviceHistory>();
    std::map<SDL_JoystickID, Device> devices;
    std::vector<SavedHint> hid_hints;
    std::vector<SavedHint> policy_hints;
    SdlClockAnchor anchor;
    std::atomic<InputSourceState> state{InputSourceState::Starting};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> worker_stopped{false};
    mutable std::mutex error_mutex;
    std::string error;
    std::mutex wake_mutex;
    std::condition_variable wake;
    std::thread worker;
    bool initialized = false;
    bool owns_gate = true;

    void SetFailure(std::string message) {
        {
            const std::scoped_lock lock(error_mutex);
            error = std::move(message);
        }
        state.store(InputSourceState::Failed, std::memory_order_release);
    }

    bool Initialize(std::string& failure) {
        for (const auto* hint : hid_driver_hints) {
            hid_hints.emplace_back(hint);
            if (!SDL_SetHintWithPriority(hint, "0", SDL_HINT_OVERRIDE)) {
                failure = "Cannot disable HID discovery during main-thread initialization";
                return false;
            }
        }
        for (const auto* hint :
             {SDL_HINT_JOYSTICK_GAMEINPUT, SDL_HINT_JOYSTICK_GAMEINPUT_RAW, SDL_HINT_JOYSTICK_WGI,
              SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS}) {
            policy_hints.emplace_back(hint);
            const char* value = std::strcmp(hint, SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS) == 0 ? "1" : "0";
            if (!SDL_SetHintWithPriority(hint, value, SDL_HINT_OVERRIDE)) {
                failure = "Cannot configure SDL input policy";
                return false;
            }
        }
        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
            failure = SDL_GetError();
            return false;
        }
        initialized = true;
        const uint64_t before = NowCapture100ns();
        anchor.ticks_ns = SDL_GetTicksNS();
        const uint64_t after = NowCapture100ns();
        anchor.capture_100ns = before + (after - before) / 2;
        return true;
    }

    void Publish(Device& device, uint64_t timestamp) {
        history->Push({timestamp, true, device.identity, NormalizeSdlControls(device.sample)});
    }

    void Snapshot(Device& device) {
        {
            const JoystickLock lock;
            device.sample = {};
            device.sample.available = SDL_GamepadConnected(device.handle);
            for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
                const auto value = static_cast<SDL_GamepadButton>(button);
                if (SDL_GamepadHasButton(device.handle, value)) {
                    device.sample.supported_buttons |= 1U << button;
                    if (SDL_GetGamepadButton(device.handle, value))
                        device.sample.buttons |= 1U << button;
                }
            }
            for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
                const auto value = static_cast<SDL_GamepadAxis>(axis);
                if (SDL_GamepadHasAxis(device.handle, value)) {
                    device.sample.supported_axes |= static_cast<uint8_t>(1U << axis);
                    device.sample.axes[static_cast<size_t>(axis)] = SDL_GetGamepadAxis(device.handle, value);
                }
            }
            device.snapshot_ticks_ns = SDL_GetTicksNS();
            device.snapshot_capture_100ns = NowCapture100ns();
        }
        // Current getters can include changes newer than a queued remap event.
        // Publish at acquisition time and reject deltas predating this snapshot.
        if (device.sample.available)
            Publish(device, device.snapshot_capture_100ns);
        else
            history->Push({device.snapshot_capture_100ns, false, device.identity, {}});
    }

    void DemoteDuplicate(Device& device, uint64_t now) {
        if (!device.identity.persistent)
            return;
        history->Push({now, false, device.identity, {}});
        device.identity.id = SessionIdentity();
        device.identity.persistent = false;
        Snapshot(device);
    }

    void Open(SDL_JoystickID instance) {
        if (devices.contains(instance) || !SDL_IsGamepad(instance) || stop_requested.load(std::memory_order_acquire))
            return;
        if (devices.size() >= 16)
            throw std::runtime_error("More than sixteen controllers are connected");
        SDL_Gamepad* handle = SDL_OpenGamepad(instance);
        if (!handle)
            throw std::runtime_error(SDL_GetError());
        try {
            Device device;
            device.handle = handle;
            device.identity.vendor_id = SDL_GetGamepadVendor(handle);
            device.identity.product_id = SDL_GetGamepadProduct(handle);
            device.identity.layout =
                SdlLayoutKindFor(SDL_GetGamepadType(handle), device.identity.vendor_id, device.identity.product_id);
            const char* serial = SDL_GetGamepadSerial(handle);
            const char* path = SDL_GetGamepadPath(handle);
            device.physical_id = PersistentSdlDeviceId(device.identity.vendor_id, device.identity.product_id,
                                                       serial ? serial : "", path ? path : "");
            bool duplicate = false;
            const uint64_t now = NowCapture100ns();
            if (device.physical_id) {
                for (auto& [id, existing] : devices) {
                    (void)id;
                    if (existing.physical_id == device.physical_id) {
                        duplicate = true;
                        DemoteDuplicate(existing, now);
                    }
                }
            }
            device.identity.persistent = device.physical_id.has_value() && !duplicate;
            device.identity.id = device.identity.persistent ? *device.physical_id : SessionIdentity();
            auto [position, inserted] = devices.emplace(instance, std::move(device));
            (void)inserted;
            Snapshot(position->second);
        } catch (...) {
            if (!devices.contains(instance))
                SDL_CloseGamepad(handle);
            throw;
        }
    }

    void Close(std::map<SDL_JoystickID, Device>::iterator position, uint64_t timestamp) {
        timestamp = std::max(timestamp, position->second.snapshot_capture_100ns);
        history->Push({timestamp, false, position->second.identity, {}});
        SDL_CloseGamepad(position->second.handle);
        devices.erase(position);
    }

    void Reconcile() {
        for (auto position = devices.begin(); position != devices.end();) {
            if (!SDL_GamepadConnected(position->second.handle)) {
                const auto removed = position++;
                Close(removed, NowCapture100ns());
            } else {
                ++position;
            }
        }
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (!ids)
            throw std::runtime_error(SDL_GetError());
        const std::unique_ptr<SDL_JoystickID, decltype(&SDL_free)> owned(ids, SDL_free);
        for (int index = 0; index < count; ++index)
            Open(ids[index]);
    }

    void Apply(const SDL_Event& event) {
        if (event.type < SDL_EVENT_GAMEPAD_AXIS_MOTION || event.type > SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)
            return;
        const SDL_JoystickID instance = event.gdevice.which;
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            Open(instance);
            return;
        }
        auto position = devices.find(instance);
        if (position == devices.end())
            return;
        auto& device = position->second;
        const uint64_t timestamp = SdlToCapture100ns(event.common.timestamp, anchor);
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_REMOVED:
            Close(position, timestamp);
            break;
        case SDL_EVENT_GAMEPAD_REMAPPED:
            Snapshot(device);
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (event.common.timestamp > device.snapshot_ticks_ns && event.gaxis.axis < device.sample.axes.size()) {
                device.sample.axes[event.gaxis.axis] = event.gaxis.value;
                Publish(device, timestamp);
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (event.common.timestamp > device.snapshot_ticks_ns && event.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) {
                const uint32_t bit = 1U << event.gbutton.button;
                if (event.gbutton.down)
                    device.sample.buttons |= bit;
                else
                    device.sample.buttons &= ~bit;
                Publish(device, timestamp);
            }
            break;
        default:
            break;
        }
    }

    void Run() noexcept {
        try {
            for (const auto& hint : hid_hints)
                hint.Restore();
            SDL_SetGamepadEventsEnabled(true);
            auto next_reconcile = std::chrono::steady_clock::now();
            while (!stop_requested.load(std::memory_order_acquire)) {
                SDL_UpdateGamepads();
                std::array<SDL_Event, 64> events{};
                unsigned processed = 0;
                for (; processed < 2048; processed += static_cast<unsigned>(events.size())) {
                    const int count =
                        SDL_PeepEvents(events.data(), static_cast<int>(events.size()), SDL_GETEVENT,
                                       SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED);
                    if (count < 0)
                        throw std::runtime_error(SDL_GetError());
                    for (int index = 0; index < count; ++index)
                        Apply(events[static_cast<size_t>(index)]);
                    if (count < static_cast<int>(events.size()))
                        break;
                }
                if (processed >= 2048)
                    throw std::runtime_error("Controller event queue exceeded its bounded processing budget");
                if (std::chrono::steady_clock::now() >= next_reconcile) {
                    Reconcile();
                    next_reconcile = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                }
                auto starting = InputSourceState::Starting;
                state.compare_exchange_strong(starting, InputSourceState::Ready, std::memory_order_acq_rel);
                std::unique_lock lock(wake_mutex);
                wake.wait_for(lock, std::chrono::milliseconds(4),
                              [&] { return stop_requested.load(std::memory_order_acquire); });
            }
        } catch (const std::exception& exception) {
            SetFailure(exception.what());
        } catch (...) {
            SetFailure("Unexpected controller input failure");
        }
        try {
            while (!devices.empty())
                Close(devices.begin(), NowCapture100ns());
        } catch (...) {
            for (auto& [id, device] : devices) {
                (void)id;
                SDL_CloseGamepad(device.handle);
            }
            devices.clear();
            SetFailure("Controller history could not be retired");
        }
        if (state.load(std::memory_order_acquire) != InputSourceState::Failed)
            state.store(InputSourceState::Stopped, std::memory_order_release);
        worker_stopped.store(true, std::memory_order_release);
    }

    void Finish() {
        stop_requested.store(true, std::memory_order_release);
        wake.notify_all();
        if (worker.joinable())
            worker.join();
        if (initialized) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
            initialized = false;
        }
        for (const auto& hint : hid_hints)
            hint.Restore();
        for (const auto& hint : policy_hints)
            hint.Restore();
        hid_hints.clear();
        policy_hints.clear();
        if (owns_gate) {
            input_owner_active.store(false, std::memory_order_release);
            owns_gate = false;
        }
        worker_stopped.store(true, std::memory_order_release);
    }
};

void RegisterInputMainThread() noexcept {
    DWORD unbound = 0;
    input_main_thread.compare_exchange_strong(unbound, GetCurrentThreadId(), std::memory_order_acq_rel);
}

uint64_t NowCapture100ns() noexcept {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }();
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return engine::QpcTicksTo100ns(static_cast<uint64_t>(ticks.QuadPart), frequency);
}

std::unique_ptr<SdlGamepadSource> SdlGamepadSource::Start(std::string& error) {
    error.clear();
    if (input_main_thread.load(std::memory_order_acquire) != GetCurrentThreadId()) {
        error = "Controller input must start on the registered main thread";
        return nullptr;
    }
    if (input_owner_active.exchange(true, std::memory_order_acq_rel)) {
        error = "Another controller input owner is still active";
        return nullptr;
    }
    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>();
        if (!LoadRuntime(error) || !impl->Initialize(error)) {
            impl->Finish();
            return nullptr;
        }
        auto* worker_state = impl.get();
        impl->worker = std::thread([worker_state] { worker_state->Run(); });
        return std::unique_ptr<SdlGamepadSource>(new SdlGamepadSource(std::move(impl)));
    } catch (const std::exception& exception) {
        error = exception.what();
        if (impl)
            impl->Finish();
        else
            input_owner_active.store(false, std::memory_order_release);
        return nullptr;
    }
}

SdlGamepadSource::SdlGamepadSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
SdlGamepadSource::~SdlGamepadSource() {
    WaitForStop();
}
InputSourceState SdlGamepadSource::State() const noexcept {
    return impl_->state.load(std::memory_order_acquire);
}
std::string SdlGamepadSource::Error() const {
    const std::scoped_lock lock(impl_->error_mutex);
    return impl_->error;
}
void SdlGamepadSource::RequestStop() noexcept {
    impl_->stop_requested.store(true, std::memory_order_release);
    auto state = State();
    while ((state == InputSourceState::Starting || state == InputSourceState::Ready) &&
           !impl_->state.compare_exchange_weak(state, InputSourceState::Stopping, std::memory_order_acq_rel)) {
    }
    impl_->wake.notify_all();
}
bool SdlGamepadSource::Stopped() const noexcept {
    return impl_->worker_stopped.load(std::memory_order_acquire);
}
void SdlGamepadSource::WaitForStop() {
    if (input_main_thread.load(std::memory_order_acquire) != GetCurrentThreadId())
        std::terminate();
    impl_->Finish();
}
std::optional<GamepadReading> SdlGamepadSource::StateAt(uint64_t timestamp) const {
    return impl_->history->StateAt(timestamp);
}
HistoryReadView SdlGamepadSource::HistoryView() const noexcept {
    return HistoryReadView(impl_->history);
}
void SdlGamepadSource::SelectDevice(std::optional<DeviceId> device) {
    impl_->history->Select(device);
}
std::vector<DeviceIdentity> SdlGamepadSource::Devices() const {
    return impl_->history->Devices();
}

} // namespace exosnap::gamepad

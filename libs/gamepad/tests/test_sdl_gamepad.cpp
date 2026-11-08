#include "gamepad/sdl_gamepad_state.h"

#include <gtest/gtest.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_timer.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace exosnap::gamepad;

namespace {
template <class Predicate> bool WaitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

class SdlProvider : public testing::Test {
  protected:
    void SetUp() override {
        RegisterInputMainThread();
        for (const char* hint : {SDL_HINT_JOYSTICK_HIDAPI, SDL_HINT_JOYSTICK_DIRECTINPUT, SDL_HINT_XINPUT_ENABLED,
                                 SDL_HINT_JOYSTICK_RAWINPUT, SDL_HINT_JOYSTICK_WGI, SDL_HINT_JOYSTICK_GAMEINPUT,
                                 SDL_HINT_JOYSTICK_GAMEINPUT_RAW}) {
            const char* old = SDL_GetHint(hint);
            hints_.emplace_back(hint, old ? std::optional<std::string>(old) : std::nullopt);
            ASSERT_TRUE(SDL_SetHintWithPriority(hint, "0", SDL_HINT_OVERRIDE));
        }
        const char* old = SDL_GetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT);
        hints_.emplace_back(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,
                            old ? std::optional<std::string>(old) : std::nullopt);
        ASSERT_TRUE(
            SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x1209/0x0001", SDL_HINT_OVERRIDE));
        SDL_SetMainReady();
        ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_JOYSTICK)) << SDL_GetError();
        joystick_initialized_ = true;
    }
    void TearDown() override {
        if (joystick_initialized_)
            SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        for (const auto& [hint, value] : hints_) {
            if (value)
                SDL_SetHintWithPriority(hint, value->c_str(), SDL_HINT_OVERRIDE);
            else
                SDL_ResetHint(hint);
        }
    }
    std::unique_ptr<SdlGamepadSource> Start() {
        std::string error;
        auto source = SdlGamepadSource::Start(error);
        EXPECT_TRUE(source) << error;
        if (source)
            EXPECT_TRUE(WaitFor([&] { return source->State() != InputSourceState::Starting; })) << source->Error();
        return source;
    }
    std::vector<std::pair<const char*, std::optional<std::string>>> hints_;
    bool joystick_initialized_ = false;
};

struct JoystickLock {
    JoystickLock() {
        SDL_LockJoysticks();
    }
    ~JoystickLock() {
        SDL_UnlockJoysticks();
    }
};

struct VirtualPad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    VirtualPad(uint32_t buttons = (1U << 26) - 1, uint32_t axes = 0x3F) {
        const JoystickLock lock;
        SDL_VirtualJoystickDesc description{};
        SDL_INIT_INTERFACE(&description);
        description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        description.vendor_id = 0x1209;
        description.product_id = 1;
        description.naxes = axes == 0 ? 0 : 6;
        description.nbuttons = 26;
        description.button_mask = buttons;
        description.axis_mask = axes;
        description.name = "ExoSnap virtual controller";
        id = SDL_AttachVirtualJoystick(&description);
        if (id != 0) {
            joystick = SDL_OpenJoystick(id);
            if (joystick) {
                if (description.naxes >= 6) {
                    SDL_SetJoystickVirtualAxis(joystick, 4, -32768);
                    SDL_SetJoystickVirtualAxis(joystick, 5, -32768);
                }
                SDL_UpdateJoysticks();
            }
        }
    }
    void Detach() {
        if (joystick) {
            SDL_CloseJoystick(joystick);
            joystick = nullptr;
        }
        if (id != 0) {
            SDL_DetachVirtualJoystick(id);
            id = 0;
        }
    }
    ~VirtualPad() {
        Detach();
    }
};
} // namespace

namespace exosnap::gamepad {
class HistoryReadTestAccess {
  public:
    static size_t DeviceCount(const DeviceHistory& history) {
        const std::scoped_lock lock(history.mutex_);
        return history.devices_.size();
    }
};
} // namespace exosnap::gamepad

TEST(SdlControls, EachMappedButtonHasOneCanonicalPosition) {
    for (unsigned button = 0; button < 26; ++button) {
        SdlGamepadSample sample;
        sample.supported_buttons = (1U << 26) - 1;
        sample.buttons = 1U << button;
        const auto state = NormalizeSdlControls(sample);
        ASSERT_TRUE(state.controls_normalized);
        for (unsigned other = 0; other < 26; ++other)
            EXPECT_EQ(state.controls[other].status, other == button ? ControlStatus::Active : ControlStatus::Released)
                << button << ":" << other;
    }
}

TEST(SdlControls, StickEndpointsInvertYAndKeepClicksIndependent) {
    SdlGamepadSample sample;
    sample.supported_axes = 0x0F;
    sample.supported_buttons = (1U << 7) | (1U << 8);
    sample.axes = {-32768, -32768, 32767, 32767, 0, 0};
    auto state = NormalizeSdlControls(sample);
    EXPECT_EQ(ReadControl(state, Control::LeftStick), (ControlValue{ControlStatus::Active, -1, 1}));
    EXPECT_EQ(ReadControl(state, Control::RightStick), (ControlValue{ControlStatus::Active, 1, -1}));
    EXPECT_EQ(ReadControl(state, Control::LeftStickClick).status, ControlStatus::Released);
    sample.buttons = 1U << 7;
    state = NormalizeSdlControls(sample);
    EXPECT_EQ(ReadControl(state, Control::LeftStickClick).status, ControlStatus::Active);
    EXPECT_EQ(ReadControl(state, Control::RightStickClick).status, ControlStatus::Released);
}

TEST(SdlControls, TriggersClampNegativeValuesAndDoNotInventDigitalButtons) {
    SdlGamepadSample sample;
    sample.supported_axes = 0x30;
    sample.axes[4] = -32768;
    sample.axes[5] = 32767;
    const auto state = NormalizeSdlControls(sample);
    EXPECT_EQ(ReadControl(state, Control::LeftTrigger), (ControlValue{ControlStatus::Released, 0, 0}));
    EXPECT_EQ(ReadControl(state, Control::RightTrigger), (ControlValue{ControlStatus::Active, 1, 0}));
    EXPECT_EQ(ReadControl(state, Control::Misc3).status, ControlStatus::Unsupported);
    EXPECT_EQ(ReadControl(state, Control::Misc4).status, ControlStatus::Unsupported);
}

TEST(SdlControls, MissingControlsAndUnavailableReadingsRemainDistinct) {
    SdlGamepadSample sample;
    sample.supported_buttons = 1U << 20;
    sample.buttons = 1U << 20;
    sample.supported_axes = 1;
    sample.available = false;
    const auto state = NormalizeSdlControls(sample);
    EXPECT_EQ(ReadControl(state, Control::Touchpad).status, ControlStatus::Unavailable);
    EXPECT_EQ(ReadControl(state, Control::Guide).status, ControlStatus::Unsupported);
    EXPECT_EQ(ReadControl(state, Control::LeftStick).status, ControlStatus::Unavailable);
    EXPECT_EQ(ReadControl(state, Control::RightStick).status, ControlStatus::Unsupported);
}

TEST(SdlArtwork, StandardizedTypesChooseExistingFamiliesWithoutInventingCapabilities) {
    const std::array cases{std::pair{SDL_GAMEPAD_TYPE_PS4, LayoutKind::DualShock4},
                           std::pair{SDL_GAMEPAD_TYPE_XBOX360, LayoutKind::Xbox360},
                           std::pair{SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT, LayoutKind::JoyConLeft},
                           std::pair{SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT, LayoutKind::JoyConRight},
                           std::pair{SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR, LayoutKind::JoyConPair},
                           std::pair{SDL_GAMEPAD_TYPE_GAMECUBE, LayoutKind::GameCube},
                           std::pair{SDL_GAMEPAD_TYPE_PS5, LayoutKind::DualSense},
                           std::pair{SDL_GAMEPAD_TYPE_XBOXONE, LayoutKind::XboxOne},
                           std::pair{SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO, LayoutKind::SwitchPro}};
    for (const auto& [type, expected] : cases)
        EXPECT_EQ(SdlLayoutKindFor(type, 0, 0), expected);
    EXPECT_EQ(SdlLayoutKindFor(SDL_GAMEPAD_TYPE_PS5, 0x054C, 0x0DF2), LayoutKind::DualSense);
    EXPECT_EQ(SdlLayoutKindFor(SDL_GAMEPAD_TYPE_XBOXONE, 0x045E, 0x02E3), LayoutKind::XboxOne);
    EXPECT_EQ(SdlLayoutKindFor(SDL_GAMEPAD_TYPE_XBOXONE, 0x045E, 0x0B13), LayoutKind::XboxSeries);
    EXPECT_EQ(SdlLayoutKindFor(SDL_GAMEPAD_TYPE_UNKNOWN, 0, 0), LayoutKind::Generic);
    EXPECT_EQ(SdlLayoutKindFor(SDL_GAMEPAD_TYPE_STEAM, 0, 0), LayoutKind::Generic);
    SdlGamepadSample partial;
    partial.supported_buttons = 1U << SDL_GAMEPAD_BUTTON_SOUTH;
    EXPECT_EQ(ReadControl(NormalizeSdlControls(partial), Control::RightStick).status, ControlStatus::Unsupported);
}

TEST(SdlIdentity, SerialAndNativePathSeparateIdenticalModelsAndDuplicateSerials) {
    const auto first = PersistentSdlDeviceId(0x057E, 0x2009, "serial-a", "\\\\?\\hid#physical-one");
    ASSERT_TRUE(first);
    EXPECT_EQ(first, PersistentSdlDeviceId(0x057E, 0x2009, "serial-a", "\\\\?\\HID#PHYSICAL-ONE"));
    EXPECT_NE(first, PersistentSdlDeviceId(0x057E, 0x2009, "serial-a", "\\\\?\\hid#physical-two"));
    EXPECT_NE(first, PersistentSdlDeviceId(0x057E, 0x2009, "serial-b", "\\\\?\\hid#physical-one"));
    EXPECT_NE(first, PersistentSdlDeviceId(0x054C, 0x2009, "serial-a", "\\\\?\\hid#physical-one"));
}

TEST(SdlIdentity, UncorrelatedSerialAndInterfacePathsRemainSessionScoped) {
    EXPECT_FALSE(PersistentSdlDeviceId(0x057E, 0x2009, {}, "\\\\?\\hid#physical-one"));
    EXPECT_FALSE(PersistentSdlDeviceId(0x057E, 0x2009, "factory-serial", {}));
    EXPECT_FALSE(PersistentSdlDeviceId(0x057E, 0x2009, "factory-serial", "interface-a"));
    EXPECT_FALSE(PersistentSdlDeviceId(0x057E, 0x2009, {}, {}));
    EXPECT_FALSE(PersistentSdlDeviceId(0x045E, 0x028E, {}, "XInput#0"));
    EXPECT_FALSE(PersistentSdlDeviceId(0x045E, 0x028E, "factory-serial", "XInput#0"));
}

TEST(SdlClock, EventObservationTimestampsMapToCaptureTimeAndSaturate) {
    const SdlClockAnchor anchor{50000, 100};
    EXPECT_EQ(SdlToCapture100ns(50000, anchor), 100U);
    EXPECT_EQ(SdlToCapture100ns(52000, anchor), 120U);
    EXPECT_EQ(SdlToCapture100ns(48000, anchor), 80U);
    EXPECT_EQ(SdlToCapture100ns(0, anchor), 0U);
    EXPECT_EQ(SdlToCapture100ns(200, {0, std::numeric_limits<uint64_t>::max() - 1}),
              std::numeric_limits<uint64_t>::max());
}

TEST(DeviceHistory, CanonicalOnlyAuxiliaryInputClaimsAutomaticSelection) {
    DeviceHistory history;
    GamepadReading quiet;
    quiet.capture_100ns = 100;
    quiet.device.id[0] = 1;
    quiet.state.controls_normalized = true;
    history.Push(quiet);
    auto active = quiet;
    active.capture_100ns = 200;
    active.device.id[0] = 2;
    active.state.controls[static_cast<size_t>(Control::Guide)] = {ControlStatus::Active, 1, 0};
    history.Push(active);
    ASSERT_TRUE(history.StateAt(200));
    EXPECT_EQ(history.StateAt(200)->device.id, active.device.id);
}

TEST(DeviceHistory, RetiredDeviceChurnIsBoundedWithoutEvictingLiveOrSelectedHistory) {
    DeviceHistory history;
    GamepadReading quiet;
    quiet.capture_100ns = 1;
    quiet.device.id[0] = 1;
    history.Push(quiet);
    auto selected = quiet;
    selected.device.id[0] = 2;
    selected.capture_100ns = 2;
    selected.state.buttons = 4;
    history.Push(selected);
    history.Select(selected.device.id);
    selected.capture_100ns = 3;
    selected.connected = false;
    history.Push(selected);
    for (unsigned id = 3; id < 100; ++id) {
        auto churn = quiet;
        churn.device.id[0] = static_cast<uint8_t>(id);
        churn.capture_100ns = id * 2;
        history.Push(churn);
        ++churn.capture_100ns;
        churn.connected = false;
        history.Push(churn);
    }
    EXPECT_LE(HistoryReadTestAccess::DeviceCount(history), 64U);
    const auto last = std::numeric_limits<uint64_t>::max();
    ASSERT_TRUE(history.StateFor(quiet.device.id, last));
    EXPECT_TRUE(history.StateFor(quiet.device.id, last)->connected);
    ASSERT_TRUE(history.StateAt(last));
    EXPECT_FALSE(history.StateAt(last)->connected);
    EXPECT_EQ(history.StateAt(last)->device.id, selected.device.id);
    const auto generation = history.SelectionGeneration();
    selected.capture_100ns = 300;
    selected.connected = true;
    selected.state = {};
    history.Push(selected);
    ASSERT_TRUE(history.StateAt(last));
    EXPECT_TRUE(history.StateAt(last)->connected);
    EXPECT_EQ(history.StateAt(last)->state.buttons, 0U);
    EXPECT_GT(history.SelectionGeneration(), generation);
}

TEST_F(SdlProvider, HidapiDriverHintsAreNotToggledDuringStartup) {
    // Our virtual-controller fixture disables HIDAPI before joystick initialization.
    // The production provider must not flip a device driver on/off during startup:
    // doing so can expose the same physical device via two SDL backend paths.
    std::atomic<unsigned> unexpected_changes{0};
    const auto callback = [](void* userdata, const char*, const char* previous, const char* current) {
        if (current && (!previous || std::strcmp(previous, current) != 0))
            static_cast<std::atomic<unsigned>*>(userdata)->fetch_add(1, std::memory_order_relaxed);
    };
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, callback, &unexpected_changes);
    auto source = Start();
    if (source) {
        source->RequestStop();
        source->WaitForStop();
    }
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, callback, &unexpected_changes);
    ASSERT_TRUE(source);
    EXPECT_EQ(unexpected_changes.load(), 0U);
}

TEST_F(SdlProvider, StartsAndRetiresOnMainThreadWithRetainedHistory) {
    RegisterInputMainThread();
    std::string error;
    auto source = SdlGamepadSource::Start(error);
    ASSERT_TRUE(source) << error;
    const auto view = source->HistoryView();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (source->State() == InputSourceState::Starting && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_EQ(source->State(), InputSourceState::Ready) << source->Error();
    source->RequestStop();
    source->WaitForStop();
    EXPECT_TRUE(source->Stopped());
    source.reset();
    GamepadReading reading;
    EXPECT_EQ(view.TryStateAt(std::numeric_limits<uint64_t>::max(), reading), HistoryReadResult::Empty);
}

TEST_F(SdlProvider, RejectsWorkerThreadInitializationWithoutTakingMainThreadIdentity) {
    RegisterInputMainThread();
    std::string error;
    bool started = false;
    std::thread worker([&] {
        const auto source = SdlGamepadSource::Start(error);
        started = static_cast<bool>(source);
    });
    worker.join();
    EXPECT_FALSE(started);
    EXPECT_NE(error.find("main thread"), std::string::npos);
}

TEST_F(SdlProvider, VirtualQueuedShortPressRemainsInCaptureHistory) {
    auto source = Start();
    ASSERT_TRUE(source);
    VirtualPad pad;
    ASSERT_NE(pad.id, 0U) << SDL_GetError();
    ASSERT_TRUE(WaitFor([&] { return source->Devices().size() == 1; }));
    uint64_t during_press = 0;
    {
        const JoystickLock lock;
        ASSERT_TRUE(SDL_SetJoystickVirtualButton(pad.joystick, 0, true));
        SDL_UpdateJoysticks();
        SDL_Delay(3);
        during_press = NowCapture100ns();
        SDL_Delay(3);
        ASSERT_TRUE(SDL_SetJoystickVirtualButton(pad.joystick, 0, false));
        SDL_UpdateJoysticks();
    }
    ASSERT_TRUE(WaitFor([&] {
        const auto pressed = source->StateAt(during_press);
        const auto latest = source->StateAt(std::numeric_limits<uint64_t>::max());
        return pressed && latest && ReadControl(pressed->state, Control::South).status == ControlStatus::Active &&
               ReadControl(latest->state, Control::South).status == ControlStatus::Released;
    })) << source->Error();
}

TEST_F(SdlProvider, RemapSnapshotCannotPublishALaterPressAtAnOlderEventTime) {
    auto source = Start();
    ASSERT_TRUE(source);
    VirtualPad pad;
    ASSERT_NE(pad.id, 0U) << SDL_GetError();
    ASSERT_TRUE(WaitFor([&] { return source->Devices().size() == 1; }));
    uint64_t before_press = 0;
    {
        const JoystickLock lock;
        char* raw = SDL_GetGamepadMapping(SDL_GetGamepadFromID(pad.id));
        ASSERT_NE(raw, nullptr);
        std::string mapping(raw);
        SDL_free(raw);
        const auto old_east = mapping.find("b:b1,");
        ASSERT_NE(old_east, std::string::npos) << mapping;
        mapping.erase(old_east, 5);
        const auto south = mapping.find("a:b0,");
        ASSERT_NE(south, std::string::npos) << mapping;
        mapping[south] = 'b';
        ASSERT_TRUE(SDL_SetGamepadMapping(pad.id, mapping.c_str())) << SDL_GetError();
        SDL_Delay(3);
        before_press = NowCapture100ns();
        SDL_Delay(3);
        ASSERT_TRUE(SDL_SetJoystickVirtualButton(pad.joystick, 0, true));
        SDL_UpdateJoysticks();
    }
    ASSERT_TRUE(WaitFor([&] {
        const auto latest = source->StateAt(std::numeric_limits<uint64_t>::max());
        return latest && ReadControl(latest->state, Control::East).status == ControlStatus::Active;
    })) << source->Error();
    const auto past = source->StateAt(before_press);
    ASSERT_TRUE(past);
    EXPECT_NE(ReadControl(past->state, Control::East).status, ControlStatus::Active);
    const auto latest = source->StateAt(std::numeric_limits<uint64_t>::max());
    ASSERT_TRUE(latest);
    EXPECT_EQ(ReadControl(latest->state, Control::South).status, ControlStatus::Unsupported);
}

TEST_F(SdlProvider, IdenticalVirtualModelsRemainSelectableAndUncertainReconnectDoesNotRebind) {
    auto source = Start();
    ASSERT_TRUE(source);
    VirtualPad first;
    VirtualPad second;
    ASSERT_NE(first.id, 0U);
    ASSERT_NE(second.id, 0U);
    ASSERT_TRUE(WaitFor([&] { return source->Devices().size() == 2; }));
    const auto devices = source->Devices();
    EXPECT_NE(devices[0].id, devices[1].id);
    EXPECT_FALSE(devices[0].persistent);
    EXPECT_FALSE(devices[1].persistent);
    source->SelectDevice(devices[0].id);
    ASSERT_TRUE(source->StateAt(std::numeric_limits<uint64_t>::max()));
    EXPECT_EQ(source->StateAt(std::numeric_limits<uint64_t>::max())->device.id, devices[0].id);
    first.Detach();
    second.Detach();
    ASSERT_TRUE(WaitFor([&] { return source->Devices().empty(); }));
    VirtualPad replacement;
    ASSERT_NE(replacement.id, 0U);
    ASSERT_TRUE(WaitFor([&] { return source->Devices().size() == 1; }));
    const auto selected = source->StateAt(std::numeric_limits<uint64_t>::max());
    ASSERT_TRUE(selected);
    EXPECT_FALSE(selected->connected);
    EXPECT_EQ(selected->device.id, devices[0].id);
    EXPECT_NE(source->Devices()[0].id, devices[0].id);
    EXPECT_NE(source->Devices()[0].id, devices[1].id);
}

TEST_F(SdlProvider, PartialCapabilitiesRemainUnavailableOrUnsupportedAfterDisconnect) {
    auto source = Start();
    ASSERT_TRUE(source);
    VirtualPad pad(1, 0);
    ASSERT_NE(pad.id, 0U);
    ASSERT_TRUE(WaitFor([&] { return source->Devices().size() == 1; }));
    const auto active = source->StateAt(std::numeric_limits<uint64_t>::max());
    ASSERT_TRUE(active);
    EXPECT_EQ(ReadControl(active->state, Control::South).status, ControlStatus::Released);
    EXPECT_EQ(ReadControl(active->state, Control::Guide).status, ControlStatus::Unsupported);
    EXPECT_EQ(ReadControl(active->state, Control::LeftStick).status, ControlStatus::Unsupported);
    source->SelectDevice(active->device.id);
    pad.Detach();
    ASSERT_TRUE(WaitFor([&] { return source->Devices().empty(); }));
    const auto disconnected = source->StateAt(std::numeric_limits<uint64_t>::max());
    ASSERT_TRUE(disconnected);
    EXPECT_FALSE(disconnected->connected);
    EXPECT_TRUE(disconnected->state.controls_normalized);
    EXPECT_EQ(ReadControl(disconnected->state, Control::South).status, ControlStatus::Unavailable);
    EXPECT_EQ(ReadControl(disconnected->state, Control::Guide).status, ControlStatus::Unsupported);
    EXPECT_EQ(ReadControl(disconnected->state, Control::LeftStick).status, ControlStatus::Unsupported);
}

TEST_F(SdlProvider, RepeatedStopRequestsPreserveTerminalStateAndExternalJoystickOwnership) {
    auto source = Start();
    ASSERT_TRUE(source);
    std::thread requester([&] {
        for (unsigned count = 0; count < 10000; ++count)
            source->RequestStop();
    });
    source->WaitForStop();
    requester.join();
    EXPECT_TRUE(source->Stopped());
    EXPECT_EQ(source->State(), InputSourceState::Stopped);
    source->RequestStop();
    source->WaitForStop();
    EXPECT_EQ(source->State(), InputSourceState::Stopped);
    EXPECT_NE(SDL_WasInit(SDL_INIT_JOYSTICK), 0U);
    EXPECT_EQ(SDL_WasInit(SDL_INIT_GAMEPAD), 0U);
}

TEST_F(SdlProvider, DeviceLimitFailsVisiblyAndRetiresExistingReadings) {
    auto source = Start();
    ASSERT_TRUE(source);
    std::vector<std::unique_ptr<VirtualPad>> pads;
    for (unsigned count = 0; count < 17; ++count) {
        pads.push_back(std::make_unique<VirtualPad>());
        ASSERT_NE(pads.back()->id, 0U);
    }
    ASSERT_TRUE(WaitFor([&] { return source->Stopped(); }));
    EXPECT_EQ(source->State(), InputSourceState::Failed);
    EXPECT_FALSE(source->Error().empty());
    EXPECT_TRUE(source->Devices().empty());
    const auto view = source->HistoryView();
    source->RequestStop();
    source->WaitForStop();
    EXPECT_EQ(source->State(), InputSourceState::Failed);
    source.reset();
    GamepadReading reading;
    if (view.TryStateAt(std::numeric_limits<uint64_t>::max(), reading) == HistoryReadResult::Found)
        EXPECT_FALSE(reading.connected);
}

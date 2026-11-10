#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap::gamepad {

enum class Control : uint8_t {
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStickClick,
    RightStickClick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Misc1,
    RightPaddle1,
    LeftPaddle1,
    RightPaddle2,
    LeftPaddle2,
    Touchpad,
    Misc2,
    Misc3,
    Misc4,
    Misc5,
    Misc6,
    LeftStick,
    RightStick,
    LeftTrigger,
    RightTrigger,
    Count
};

enum class ControlStatus : uint8_t { Unsupported, Unavailable, Released, Active };

/// Analog sticks use [-1, 1], with positive y up. Triggers use [0, 1].
/// Values are meaningful only for Released and Active states.
struct ControlValue {
    ControlStatus status = ControlStatus::Unsupported;
    float x = 0;
    float y = 0;
    bool operator==(const ControlValue&) const = default;
};

[[nodiscard]] std::string_view ControlName(Control control) noexcept;
[[nodiscard]] Control ControlFromName(std::string_view name) noexcept;
[[nodiscard]] Control ControlFromLegacyButton(uint32_t mask) noexcept;

/// Silhouette family a controller is drawn with.
enum class LayoutKind : uint8_t {
    Generic,
    DualSense,
    XboxSeries,
    XboxOne,
    SwitchPro,
    DualShock4,
    Xbox360,
    JoyConLeft,
    JoyConRight,
    JoyConPair,
    GameCube
};

/// Maps a USB/Bluetooth vendor and product id to a layout. Unknown devices are
/// `Generic`.
[[nodiscard]] LayoutKind LayoutKindFor(uint16_t vendor_id, uint16_t product_id) noexcept;

using DeviceId = std::array<uint8_t, 32>;

struct DeviceIdentity {
    DeviceId id{};
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    LayoutKind layout = LayoutKind::Generic;
    bool persistent = true;
    // SDL's logical assignment, not a readback of the physical LED pattern.
    // -1 means unknown/unassigned; indices are zero-based.
    int player_index = -1;
    bool player_led_supported = false;
};

/// One gamepad state. `buttons` carries the legacy overlay button-mask bits
/// unchanged. Sticks are in [-1, 1] with +y up, triggers in [0, 1].
struct GamepadState {
    /// Canonical controls are authoritative when populated by an input provider.
    /// Raw fields below remain a compatibility input for existing callers.
    bool controls_normalized = false;
    std::array<ControlValue, static_cast<size_t>(Control::Count)> controls{};
    uint32_t buttons = 0;
    float left_x = 0.0F;
    float left_y = 0.0F;
    float right_x = 0.0F;
    float right_y = 0.0F;
    float left_trigger = 0.0F;
    float right_trigger = 0.0F;
    // Empty when the device/reading does not expose the auxiliary control.
    std::optional<bool> home;
    std::optional<bool> share;
    std::optional<bool> touchpad;
    bool operator==(const GamepadState&) const = default;
};

[[nodiscard]] ControlValue ReadControl(const GamepadState& state, Control control) noexcept;

/// Converts legacy overlay positions without inferring a physical adapter's wiring.
/// Unsupported buttons are excluded by the device's reported capability mask.
void NormalizeLegacyControls(GamepadState& state, uint32_t supported_buttons, bool gamepad_available = true,
                             bool separately_reported_trigger_clicks = false) noexcept;

/// Clamps every axis into its documented range and replaces NaN with the rest
/// position, so a misbehaving driver cannot push a stick cap out of its well.
[[nodiscard]] GamepadState Normalized(const GamepadState& state) noexcept;

/// A state stamped on the recorder's capture clock (QueryPerformanceCounter in
/// 100 ns units). A disconnect is recorded as a rest state with
/// `connected == false`.
struct GamepadReading {
    uint64_t capture_100ns = 0;
    bool connected = true;
    DeviceIdentity device;
    GamepadState state;
};

/// The same instant read on both clocks.
struct ClockAnchor {
    uint64_t input_us = 0;
    uint64_t capture_100ns = 0;
};

enum class HistoryReadResult { Found, Empty, Busy };
class DeviceHistory;

// Retains only history data, independently of native input and callback owners.
class HistoryReadView {
  public:
    HistoryReadView() noexcept = default;
    explicit HistoryReadView(std::shared_ptr<const DeviceHistory>) noexcept;
    [[nodiscard]] HistoryReadResult TryStateAt(uint64_t capture_100ns, GamepadReading&) const noexcept;
    [[nodiscard]] uint64_t SelectionGeneration() const noexcept;
    bool operator==(const HistoryReadView&) const noexcept = default;

  private:
    std::shared_ptr<const DeviceHistory> history_;
};

/// Converts an input timestamp (microseconds) to the capture clock through
/// `anchor`. Results that would be negative on the capture clock saturate to
/// zero.
[[nodiscard]] uint64_t InputToCapture100ns(uint64_t input_us, const ClockAnchor& anchor) noexcept;

/// Fixed-capacity history of readings. Thread-safe: one short critical section
/// per call, never held across a callback. When full, the oldest pushed reading
/// is overwritten.
class ReadingRing {
  public:
    explicit ReadingRing(std::size_t capacity = 256);

    void Push(const GamepadReading& reading);

    /// The newest reading stamped at or before `capture_100ns`, or nothing when
    /// no retained reading is that old. Readings may be pushed out of order.
    [[nodiscard]] std::optional<GamepadReading> StateAt(uint64_t capture_100ns) const;
    [[nodiscard]] HistoryReadResult TryStateAt(uint64_t capture_100ns, GamepadReading&) const noexcept;

  private:
    friend class HistoryReadTestAccess;
    [[nodiscard]] std::optional<GamepadReading> StateAtLocked(uint64_t capture_100ns) const noexcept;
    mutable std::mutex mutex_;
    std::vector<GamepadReading> slots_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

/// Independent bounded histories for each physical device. Automatic selection
/// stays on one connected device. A manual selection never falls back.
/// Retains at most 64 device histories, evicting only disconnected, unselected
/// entries. A new device is ignored when all retained entries are protected.
class DeviceHistory {
  public:
    void Push(GamepadReading reading);
    void Select(std::optional<DeviceId> device);
    [[nodiscard]] std::vector<DeviceIdentity> Devices() const;
    [[nodiscard]] std::optional<GamepadReading> StateAt(uint64_t capture_100ns) const;
    [[nodiscard]] std::optional<GamepadReading> StateFor(const DeviceId& device, uint64_t capture_100ns) const;
    [[nodiscard]] HistoryReadResult TryStateAt(uint64_t capture_100ns, GamepadReading&) const noexcept;
    [[nodiscard]] uint64_t SelectionGeneration() const noexcept;

  private:
    friend class HistoryReadTestAccess;
    void AdvanceSelectionGeneration() noexcept;
    struct Entry {
        ReadingRing history;
        DeviceIdentity identity;
        uint64_t latest_100ns = 0;
        bool connected = false;
    };
    mutable std::mutex mutex_;
    std::map<DeviceId, Entry> devices_;
    std::optional<DeviceId> manual_;
    std::optional<DeviceId> automatic_;
    uint64_t automatic_since_ = 0;
    bool automatic_engaged_ = false;
    std::atomic<uint64_t> selection_generation_{1};
};

/// Binds provider initialization and retirement to the real application main thread.
/// Call once from the application's entry point before starting input.
void RegisterInputMainThread() noexcept;

[[nodiscard]] uint64_t NowCapture100ns() noexcept;

// Result of an explicit player-index request. Other controllers can be
// reassigned by SDL when the requested slot is already occupied.
struct PlayerIndexSetResult {
    DeviceId device{};
    int requested = -1;
    int observed = -1;
    bool success = false;
    std::string error;
};

[[nodiscard]] constexpr bool SelectablePlayerIndex(int index) noexcept {
    return index >= -1 && index <= 3;
}

enum class InputSourceState { Starting, Ready, Failed, Stopping, Stopped };

/// Owns SDL gamepad input. Start, destruction and WaitForStop require the bound
/// main thread. Consumers retain HistoryReadView instead of native ownership.
class SdlGamepadSource {
  public:
    [[nodiscard]] static std::unique_ptr<SdlGamepadSource> Start(std::string& error);
    ~SdlGamepadSource();
    SdlGamepadSource(const SdlGamepadSource&) = delete;
    SdlGamepadSource& operator=(const SdlGamepadSource&) = delete;

    [[nodiscard]] InputSourceState State() const noexcept;
    [[nodiscard]] std::string Error() const;
    void RequestStop() noexcept;
    [[nodiscard]] bool Stopped() const noexcept;
    void WaitForStop();
    [[nodiscard]] std::optional<GamepadReading> StateAt(uint64_t capture_100ns) const;
    [[nodiscard]] HistoryReadView HistoryView() const noexcept;
    void SelectDevice(std::optional<DeviceId> device);
    [[nodiscard]] std::vector<DeviceIdentity> Devices() const;
    // Explicit user action only; queued on the existing SDL input worker.
    // A true return means queued, NOT that the hardware accepted the change.
    [[nodiscard]] bool RequestPlayerIndex(DeviceId device, int index);
    [[nodiscard]] std::optional<PlayerIndexSetResult> TakePlayerIndexResult();

    struct Impl;

  private:
    explicit SdlGamepadSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace exosnap::gamepad

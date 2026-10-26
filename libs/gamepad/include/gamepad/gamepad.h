#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace exosnap::gamepad {

/// Silhouette family a controller is drawn with.
enum class LayoutKind : uint8_t { Generic, DualSense, XboxSeries, XboxOne, SwitchPro };

/// Maps a USB/Bluetooth vendor and product id to a layout. Unknown devices are
/// `Generic`.
[[nodiscard]] LayoutKind LayoutKindFor(uint16_t vendor_id, uint16_t product_id) noexcept;

using DeviceId = std::array<uint8_t, 32>;

struct DeviceIdentity {
    DeviceId id{};
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    LayoutKind layout = LayoutKind::Generic;
};

/// One gamepad state. `buttons` carries the GameInputGamepadButtons bits
/// unchanged. Sticks are in [-1, 1] with +y up, triggers in [0, 1].
struct GamepadState {
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
    uint64_t gameinput_us = 0;
    uint64_t capture_100ns = 0;
};

/// Converts a GameInput timestamp (microseconds) to the capture clock through
/// `anchor`. Results that would be negative on the capture clock saturate to
/// zero.
[[nodiscard]] uint64_t GameInputToCapture100ns(uint64_t gameinput_us, const ClockAnchor& anchor) noexcept;

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

  private:
    mutable std::mutex mutex_;
    std::vector<GamepadReading> slots_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

/// Independent bounded histories for each physical device. Automatic selection
/// stays on one connected device. A manual selection never falls back.
class DeviceHistory {
  public:
    void Push(GamepadReading reading);
    void Select(std::optional<DeviceId> device);
    [[nodiscard]] std::vector<DeviceIdentity> Devices() const;
    [[nodiscard]] std::optional<GamepadReading> StateAt(uint64_t capture_100ns) const;
    [[nodiscard]] std::optional<GamepadReading> StateFor(const DeviceId& device, uint64_t capture_100ns) const;

  private:
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
};

/// Live gamepad input through Microsoft GameInput, including background input
/// while another application has focus. Readings arrive on GameInput's own
/// threads and are queried from any thread.
class GameInputSource {
  public:
    /// Returns nullptr and fills `error` when the GameInput v4 runtime is not
    /// available (the in-box Windows DLL only implements v0; the
    /// GameInputRedist runtime is required).
    [[nodiscard]] static std::unique_ptr<GameInputSource> Start(std::string& error);

    ~GameInputSource();
    GameInputSource(const GameInputSource&) = delete;
    GameInputSource& operator=(const GameInputSource&) = delete;

    [[nodiscard]] std::optional<GamepadReading> StateAt(uint64_t capture_100ns) const;
    void SelectDevice(std::optional<DeviceId> device);
    [[nodiscard]] std::vector<DeviceIdentity> Devices() const;

    /// The capture clock's current time.
    [[nodiscard]] static uint64_t NowCapture100ns() noexcept;

    struct Impl;

  private:
    explicit GameInputSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace exosnap::gamepad

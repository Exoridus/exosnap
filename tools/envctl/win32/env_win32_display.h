// tools/envctl/win32/env_win32_display.h -- documented display reads and the two
// documented display writes.
//
// Identity: a display is keyed by the MONITOR DEVICE PATH that
// DISPLAYCONFIG_TARGET_DEVICE_NAME reports (e.g.
// \\?\DISPLAY#DEL41A1#5&1a2b&UID4353#{e6f07b5f-...}). The adapter LUID + target
// id pair is carried alongside as `session_id`, but it is NOT the key: a LUID is
// only stable for as long as the adapter stays enumerated in this boot, so a
// journal written before a reboot would rebind to the wrong panel afterwards.
// The friendly name is display-only -- two identical monitors report the same
// one.

#pragma once

#include <string>
#include <vector>

#include <windows.h>

#include "env_display_mode.h"

namespace exosnap::envctl::win32 {

struct DisplayTarget {
    std::string stable_id;     // monitor device path -- the matching key
    std::string friendly_name; // monitorFriendlyDeviceName; display only
    std::string gdi_name;      // "\\.\DISPLAY1", for EnumDisplaySettingsExW
    std::string session_id;    // "LUID:hi:lo/TARGET:n" -- this boot only
    LUID adapter_id{};
    UINT32 target_id{0};
    UINT32 source_id{0};
};

std::vector<DisplayTarget> EnumerateDisplays(std::string& error);

struct AdvancedColorInfo {
    bool ok{false};
    bool info2_available{false}; // the SDK/OS offers GET_ADVANCED_COLOR_INFO_2
    bool hdr_supported{false};
    bool hdr_user_enabled{false};
    bool advanced_color_active{false};
    std::string active_color_mode; // "sdr" | "wcg" | "hdr" | "unavailable"
    std::string acm;               // "on" | "off" | "unavailable"  (wide-colour user toggle)
    std::string error;
};

// GET_ADVANCED_COLOR_INFO_2 when available, GET_ADVANCED_COLOR_INFO otherwise.
// `acm` and `active_color_mode` report "unavailable" on the fallback path rather
// than being inferred from the HDR flag -- they are different toggles.
AdvancedColorInfo ReadAdvancedColor(const DisplayTarget& target);

// SET_HDR_STATE when available, SET_ADVANCED_COLOR_STATE otherwise. Returns only
// whether the SETTER accepted; verification is the transaction's job.
bool SetHdrState(const DisplayTarget& target, bool enable, std::string& error);

struct DisplayMode {
    bool ok{false};
    DEVMODEW devmode{};
    std::string error;
};

// The WHOLE DEVMODE, never just dmDisplayFrequency: a refresh-rate change has to
// be able to hold every coupled field at its original value.
DisplayMode ReadCurrentMode(const DisplayTarget& target);

// "<w>x<h>@<hz>x<bpp>/<orientation>"
std::string FormatMode(const DEVMODEW& mode);

// A DEVMODE reduced to the five fields the mode fingerprint is made of. Nothing
// is rounded: dmDisplayFrequency is carried across verbatim.
DisplayModeFacts ToModeFacts(const DEVMODEW& mode);

struct DisplayModeList {
    bool ok{false};
    DisplayModeFacts current{};
    // Every mode EnumDisplaySettingsExW reports for this display, in enumeration
    // order and unfiltered. The refresh rates are Windows' own integers.
    std::vector<DisplayModeFacts> all;
    // The subset a refresh-rate transaction may target -- see
    // core/env_display_mode.h for why the geometry is pinned.
    std::vector<DisplayModeFacts> candidates;
    std::string error;
};

// Walks EnumDisplaySettingsExW over increasing iModeNum until it returns 0.
// READ ONLY.
DisplayModeList EnumerateModes(const DisplayTarget& target);

// Applies `hz` with width/height/bpp/orientation/position pinned to their current
// values, then re-reads. If any coupled field moved, the ORIGINAL full mode is
// put back and this returns false with an error naming what moved -- changing
// somebody's resolution as a side effect of a refresh-rate test is not an
// acceptable outcome.
//
// `hz` must be a rate this display ENUMERATES (see EnumerateModes). Windows
// accepts a nominal rate it will never report back -- asking for 60 on a
// 59.94 Hz mode succeeds here and then reads back as 59 -- and the transaction's
// read-back comparison then correctly refuses the whole thing.
bool SetRefreshHz(const DisplayTarget& target, DWORD hz, std::string& error);

// Effective per-monitor DPI as a percentage ("100", "125", "150"). READ ONLY:
// the per-monitor DPI setter is an undocumented DISPLAYCONFIG_DEVICE_INFO_TYPE.
std::string ReadDpiScalePercent(const DisplayTarget& target, std::string& error);

// A stable rendering of the active desktop topology. READ ONLY.
std::string ReadTopology(std::string& error);

std::string WideToUtf8(const wchar_t* text);

} // namespace exosnap::envctl::win32

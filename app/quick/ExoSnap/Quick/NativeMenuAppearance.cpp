#include "NativeMenuAppearance.h"

#include <QOperatingSystemVersion>

#include <windows.h>

namespace exosnap::quick {

namespace {

// uxtheme.dll, exported by ordinal since Windows 10 1903 (build 18362). The
// values are the shell's own PreferredAppMode enumeration.
enum class PreferredAppMode : int {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
};

using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode mode);
using FlushMenuThemesFn = void(WINAPI*)();

constexpr WORD kOrdinalSetPreferredAppMode = 135;
constexpr WORD kOrdinalFlushMenuThemes = 136;

struct MenuThemeEntryPoints {
    SetPreferredAppModeFn set_preferred_app_mode = nullptr;
    FlushMenuThemesFn flush_menu_themes = nullptr;
};

// Resolved once: the module stays loaded for the life of the process, and the
// ordinals do not move underneath a running Windows.
const MenuThemeEntryPoints& EntryPoints() {
    static const MenuThemeEntryPoints points = [] {
        MenuThemeEntryPoints resolved;
        // The ordinal 135 slot meant something else before 18362
        // (AllowDarkModeForApp, a different signature), so the version gate is
        // what keeps this from calling the wrong function on an older build.
        if (QOperatingSystemVersion::current() <
            QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 18362))
            return resolved;
        HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (uxtheme == nullptr)
            return resolved;
        resolved.set_preferred_app_mode = reinterpret_cast<SetPreferredAppModeFn>(
            GetProcAddress(uxtheme, MAKEINTRESOURCEA(kOrdinalSetPreferredAppMode)));
        resolved.flush_menu_themes =
            reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(kOrdinalFlushMenuThemes)));
        if (resolved.set_preferred_app_mode == nullptr || resolved.flush_menu_themes == nullptr)
            resolved = {};
        return resolved;
    }();
    return points;
}

} // namespace

bool ApplyNativeMenuAppearance() {
    const MenuThemeEntryPoints& points = EntryPoints();
    if (points.set_preferred_app_mode == nullptr)
        return false;
    // AllowDark, not a forced mode: it tells Windows this process supports its
    // dark menus and lets Settings > Colors decide, the same opt-in every other
    // application's popup menu makes.
    points.set_preferred_app_mode(PreferredAppMode::AllowDark);
    points.flush_menu_themes();
    return true;
}

} // namespace exosnap::quick

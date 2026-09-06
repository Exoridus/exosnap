#pragma once

// Opts this process's native Win32 popup menus into the system dark-mode
// support.
//
// The tray menu is one of these. Qt.labs.platform asks the Windows platform
// theme for its menu and tray icon, and on Windows both are native: the menu
// is an HMENU shown through TrackPopupMenu by the tray icon's own message
// window. Nothing Qt draws is involved, so neither the application palette nor
// a QMenu style sheet nor the Widgets style can reach it -- user32 paints it
// from the system menu theme, which is light unless the process opts in.
//
// It follows WINDOWS rather than the application's own appearance. The menu
// pops out of the tray icon onto the taskbar, and the icon already follows the
// shell's own light/dark setting for the same reason: both are composited onto
// a ground the product does not own, next to every other tray icon and every
// other context menu on the desktop, which all read the system setting too. So
// the opt-in below is AllowDark, not a forced choice: it lets Windows decide
// from Settings > Colors, the same thing every other application's popup menu
// does, rather than pinning the process to one mode regardless of it.
//
// The opt-in is the same uxtheme entry point Explorer and the shell use for
// their own menus. It is exported by ordinal only and is not in the SDK, which
// is why it is resolved at runtime and why every failure here is a no-op that
// leaves the menu exactly as it was: light, the way it always was, never broken.

namespace exosnap::quick {

// Returns false when the running Windows has no such setting (before
// 10.0.18362) or the entry points could not be resolved; the menu then keeps
// the system default. Call once at startup -- Windows re-themes an
// AllowDark-opted-in process's menus itself when the user changes Settings >
// Colors, so there is nothing here to re-apply on a settings change.
bool ApplyNativeMenuAppearance();

} // namespace exosnap::quick

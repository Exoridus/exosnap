#pragma once

// The appearance of the process's native Win32 popup menus.
//
// The tray menu is one of these. Qt.labs.platform asks the Windows platform
// theme for its menu and tray icon, and on Windows both are native: the menu
// is an HMENU shown through TrackPopupMenu by the tray icon's own message
// window. Nothing Qt draws is involved, so neither the application palette nor
// a QMenu style sheet nor the Widgets style can reach it -- user32 paints it
// from the system menu theme, which is light unless the process opts into the
// dark one.
//
// The opt-in is the same uxtheme entry point Explorer and the shell use for
// their own menus. It is exported by ordinal only and is not in the SDK, which
// is why it is resolved at runtime and why every failure here is a no-op that
// leaves the menu exactly as it was: light, the way it always was, never broken.

namespace exosnap::quick {

// Asks user32 to draw this process's popup menus dark or light, and flushes the
// menu theme so an already-built menu repaints. Returns false when the
// running Windows has no such setting (before 10.0.18362) or the entry points
// could not be resolved; the menu then keeps the system default.
bool ApplyNativeMenuAppearance(bool dark);

} // namespace exosnap::quick

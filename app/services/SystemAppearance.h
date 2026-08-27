#pragma once

#include <QString>

namespace exosnap::services {

// Which appearance the WINDOWS SHELL is drawing, as an ExoSnap appearance id
// ("light" or "dark").
//
// This is not the application's appearance and must not be confused with it. The
// notification-area icon and the taskbar button are painted onto shell chrome,
// not onto an ExoSnap surface: a product running in Light on a dark taskbar was
// drawing its mark for a ground it is never composited against.
//
// Windows keeps the two apart as well -- `SystemUsesLightTheme` governs the
// taskbar, tray and Start, `AppsUseLightTheme` governs application windows -- and
// this reads the first one on purpose.
//
// `fallback` is returned when the value is absent or unreadable, which is what a
// stripped or policy-managed profile looks like. Deliberately a parameter rather
// than a hardcoded default: with nothing to go on, following the application is
// a better guess than asserting a shell theme nobody measured.
[[nodiscard]] QString ShellAppearanceId(const QString& fallback);

} // namespace exosnap::services

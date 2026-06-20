#pragma once

#include "ExoSnapThemes.h"

#include <QColor>

class QApplication;
class QString;

namespace exosnap::ui::theme {

// Apply the full ExoSnap theme using the default theme (dark-default).
// Call once at startup before the main window is shown.
void ApplyExoSnapTheme(QApplication& app);

// Re-apply the QSS and palette with the theme identified by theme_id.
// Falls back to dark-default when theme_id is not found.
// Safe to call on the UI thread at any time after ApplyExoSnapTheme().
void ReapplyTheme(QApplication& app, const QString& theme_id);

// Returns the currently active theme. Valid after ApplyExoSnapTheme().
const ExoTheme& ActiveTheme();

// Parse a CSS colour string (supports #rrggbb, #rgb, rgba(r,g,b,a) where a is 0..1).
// Returns an invalid QColor if parsing fails.
QColor ParseThemeColor(const char* css_color);

// Legacy compat shim: ReapplyAccent -> ReapplyTheme (maps old accent ids to themes).
// Kept for a short transition period; callers should migrate to ReapplyTheme.
void ReapplyAccent(QApplication& app, const QString& accent_id);

} // namespace exosnap::ui::theme

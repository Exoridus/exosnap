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

// ── Derived colour helpers ─────────────────────────────────────────────────────
// Use these in widgets that build inline stylesheets, to avoid coupling to the
// BuildTokens() internals or to ExoSnapPalette:: static constants.

// Derive bg4 (raise-hover): Lighten(raise, 0.10) for dark, Darken(raise, 0.04) for light.
// Equivalent to the ${bg4} QSS token. Returns an HTML hex string (#rrggbb).
QString ThemeBg4Color(const ExoTheme& theme);

// Derive text1 (body text): blend(ink, mut, 0.42). Same as ${text1}.
// Returns an HTML hex string (#rrggbb).
QString ThemeText1Color(const ExoTheme& theme);

// Derive accent hover (Lighten(ac, 0.14)). Same as ${accent-hover}.
QString ThemeAccentHover(const ExoTheme& theme);

// Derive accent pressed (Darken(ac, 0.09)). Same as ${accent-pressed}.
QString ThemeAccentPressed(const ExoTheme& theme);

// Format "rgba(r, g, b, alpha)" from a QColor base and 0..1 alpha.
// Convenience for inline stylesheet construction in widgets.
QString ThemeRgba(const QColor& base, double alpha);

} // namespace exosnap::ui::theme

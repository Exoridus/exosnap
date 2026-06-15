#pragma once

class QApplication;
class QString;

namespace exosnap::ui::theme {

// Apply the full ExoSnap theme using the default accent (Studio Mint).
// Call once at startup before the main window is shown.
void ApplyExoSnapTheme(QApplication& app);

// Re-apply the QSS portion of the theme with the accent identified by
// accent_id (a key from kExoSnapAccents, e.g. "mint", "azure").
// All other palette tokens (backgrounds, text, semantic colors) remain
// unchanged — only the accent-family tokens are substituted.
// Falls back to the default accent when accent_id is not found in the table.
// Safe to call on the UI thread at any time after ApplyExoSnapTheme().
void ReapplyAccent(QApplication& app, const QString& accent_id);

} // namespace exosnap::ui::theme

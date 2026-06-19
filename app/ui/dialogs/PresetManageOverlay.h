#pragma once
#include <QWidget>

#include "../../models/RecordingPresetRegistry.h"

class QFrame;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;

namespace exosnap::ui::dialogs {

class PresetManagePanel;

// In-window Preset Manager surface. Renders as a translucent backdrop with a
// centered panel inside the application window — no native OS dialog.
//
// Parent to the host whose client area it should cover (typically the central
// widget). Open with openOverlay(); dismisses on Escape, backdrop click, or
// the × button, emitting closed() each time.
//
// All mutating actions are surfaced as signals that MainWindow connects to
// the existing registry/store/persistence handlers — no business logic lives
// here. After any action the caller must call refreshPresets() to update the
// list to reflect the new registry state.
class PresetManageOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit PresetManageOverlay(QWidget* parent = nullptr);

    void openOverlay();
    void closeOverlay();
    bool isOpen() const noexcept;

    // Push a fresh snapshot of the registry into the panel list.
    // Call this after every registry mutation (same pattern as refreshPresetUi).
    void refreshPresets(const RecordingPresetRegistry& registry);

  signals:
    void closed();

    // ---- Preset lifecycle signals (routed to existing MainWindow handlers) ----
    void duplicatePresetRequested();
    void renamePresetRequested(const QString& name);
    void deletePresetRequested();
    void setDefaultPresetRequested();
    // Export the currently-selected preset to path (caller picks path via dialog).
    void exportSelectedPresetRequested(const QString& path);
    // Export all presets to path.
    void exportAllPresetsRequested(const QString& path);
    // Import presets from path.
    void importPresetsRequested(const QString& path);
    // User wants to switch the active preset selection.
    void presetSelectionRequested(const QString& id);

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();

    PresetManagePanel* panel_ = nullptr;
};

} // namespace exosnap::ui::dialogs

#pragma once
#include <QWidget>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;

namespace exosnap {
class EditExportPage;
}

namespace exosnap::ui::dialogs {

// EDIT-OVERLAY-R1 (ADR 0022 update): in-window overlay that hosts EditExportPage
// over the Record page instead of swapping it in as a QStackedWidget page. Same
// technical recipe as SourcePickerOverlay: a plain QWidget sibling parented to the
// MainWindow central widget, which Qt correctly composites over the native DXGI
// live-preview HWND (the overlapping sibling gets auto-promoted to its own native
// window). Do not change this parenting recipe.
//
// It spans the client area minus a top inset (setTopInset), so the real title bar
// stays reachable — the window can be moved and minimized during an edit session.
// MainWindow owns that value; the overlay never reaches for the title bar itself.
//
// The hosted page() is built once and kept alive for the app's lifetime (lazy:
// the overlay itself is constructed on first use, same as the former
// buildEditExportPage()). Callers populate page() (setEditContext /
// setRecordingInfo) BEFORE calling openOverlay().
//
// Dismiss rules: Escape or a backdrop click closes the overlay unless an export is
// running, where it must not be silently abandoned — only the export card's own
// Cancel can end that flow. Trim points and markers that have not been exported are
// not dropped silently either: closing asks first (EditExportPage::hasUnsavedEdits).
class EditExportOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit EditExportOverlay(QWidget* parent = nullptr);

    // Shows the overlay over its parent's client area. Populate page() first.
    void openOverlay();
    // Closes without asking. Used where a prompt would be worse than the lost
    // trim — a recording start must never be blocked by a modal.
    void closeOverlay();
    // Closes through the discard guard. Returns false when the user chose to keep
    // editing, so callers can cancel whatever they were closing the overlay for.
    bool requestCloseOverlay();
    bool isOpen() const noexcept;

    // Height of the band at the top of the parent that the overlay leaves free
    // (the real title bar). Re-applied on every geometry sync.
    void setTopInset(int pixels);

    // True while a dismiss (Escape / backdrop click / nav-away) must be ignored
    // because the hosted page is actively exporting.
    bool isDismissBlocked() const noexcept;

    exosnap::EditExportPage* page() const noexcept {
        return page_;
    }

  signals:
    // Emitted on the hidden -> open transition (openOverlay). MainWindow uses the
    // opened()/closed() pair to suspend/resume RecordPage's visibility-gated meter
    // monitoring (mic privacy) while the overlay covers the still-visible page.
    void opened();
    void closed();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();

    exosnap::EditExportPage* page_ = nullptr;
    int top_inset_ = 0;
};

} // namespace exosnap::ui::dialogs

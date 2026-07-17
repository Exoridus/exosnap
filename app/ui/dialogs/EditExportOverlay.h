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

// EDIT-OVERLAY-R1 (ADR 0022 update): in-window overlay that hosts the existing,
// functionally-unchanged EditExportPage over the Record page instead of swapping
// it in as a QStackedWidget page. Same technical recipe as SourcePickerOverlay: a
// plain QWidget sibling parented to the MainWindow central widget so it covers the
// full client area (title bar included) with an opaque backdrop, which Qt correctly
// composites over the native DXGI live-preview HWND (the overlapping sibling gets
// auto-promoted to its own native window). Do not change this parenting recipe.
//
// The hosted page() is built once and kept alive for the app's lifetime (lazy:
// the overlay itself is constructed on first use, same as the former
// buildEditExportPage()). Callers populate page() (setEditContext/setRecordingInfo
// + setPhase) BEFORE calling openOverlay().
//
// Dismiss rules: Escape or a backdrop click closes the overlay in every phase
// EXCEPT Phase::Exporting, where a running export must not be silently abandoned —
// only the page's own controls (Cancel / Retry) can end that flow. Any not-yet
// -exported trim/marker edits are discarded on dismiss without a confirmation
// prompt: this mirrors the page's pre-existing Back-button behavior (onBackClicked
// already discards silently) and markers are persisted immediately on add via
// EditExportPage::saveMarkers(), so nothing already-committed is at risk.
class EditExportOverlay : public QWidget {
    Q_OBJECT
  public:
    explicit EditExportOverlay(QWidget* parent = nullptr);

    // Shows the overlay over its parent's full client area. Populate page() first.
    void openOverlay();
    void closeOverlay();
    bool isOpen() const noexcept;

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
};

} // namespace exosnap::ui::dialogs

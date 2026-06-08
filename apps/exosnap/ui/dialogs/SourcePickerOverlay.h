#pragma once
#include "SourcePickerPanel.h"
#include <QWidget>
#include <vector>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;

namespace exosnap::ui::dialogs {

// In-window Source Picker surface. Replaces the former native QDialog so the
// picker appears inside the application window without a separate OS titlebar.
// Parent to the host whose client area it should cover (typically the central
// stack_). Open with openOverlay(); the overlay dismisses on Escape, on a
// backdrop click, on Cancel, or after a successful selection, emitting closed()
// each time it is dismissed and sourceSelected() on a confirmed selection.
class SourcePickerOverlay : public QWidget {
    Q_OBJECT
  public:
    // Re-export from SourcePickerPanel for callers that hold the overlay.
    using Section = SourcePickerPanel::Section;
    using SourceOption = SourcePickerPanel::SourceOption;
    using SelectionResult = SourcePickerPanel::SelectionResult;

    explicit SourcePickerOverlay(QWidget* parent = nullptr);

    void openOverlay();
    void closeOverlay();
    bool isOpen() const noexcept;

    void setScreenOptions(const std::vector<SourceOption>& options);
    void setWindowOptions(const std::vector<SourceOption>& options);
    void setRegionState(const QString& summary, bool has_region, bool select_on_record,
                        const QRect& region_rect = QRect());
    void setCurrentSection(Section section, int target_index);
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    void applyVisualRegionPreset(int preset_w, int preset_h);
#endif

  signals:
    void sourceSelected(SourcePickerPanel::SelectionResult result);
    void closed();

  protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;

  private:
    void syncGeometryToParent();
    void onPanelAccepted();

    SourcePickerPanel* panel_ = nullptr;
};

} // namespace exosnap::ui::dialogs

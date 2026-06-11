#pragma once

#include <QDialog>
#include <vector>

#include "SourcePickerPanel.h"

namespace exosnap::ui::dialogs {

// Thin QDialog shell around SourcePickerPanel. Kept as a QDialog so existing
// tests that check result()/QDialog::Accepted continue to work unchanged.
// The actual picker UI lives in SourcePickerPanel; SourcePickerOverlay uses
// SourcePickerPanel directly to avoid native OS window chrome.
class SourcePickerDialog : public QDialog {
    Q_OBJECT
  public:
    // Re-export types from SourcePickerPanel for backward compatibility.
    using Section = SourcePickerPanel::Section;
    using SourceOption = SourcePickerPanel::SourceOption;
    using SelectionResult = SourcePickerPanel::SelectionResult;

    static QRect ComputePresetRegionRect(int preset_w, int preset_h, const QRect& monitor,
                                         const QRect& existing_region = QRect());

    explicit SourcePickerDialog(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());

    void setScreenOptions(const std::vector<SourceOption>& options);
    void setWindowOptions(const std::vector<SourceOption>& options);
    void setRegionState(const QString& summary, bool has_region, bool select_on_record,
                        const QRect& region_rect = QRect());
    void setCurrentSelection(Section section, int target_index);
    bool selectSource(Section section, int target_index);
    SelectionResult selectionResult() const;

  signals:
    void sourceDataRequested();

  private:
    SourcePickerPanel* panel_ = nullptr;
};

} // namespace exosnap::ui::dialogs

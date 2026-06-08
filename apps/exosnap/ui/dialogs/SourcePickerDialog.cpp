#include "SourcePickerDialog.h"

#include <QVBoxLayout>

namespace exosnap::ui::dialogs {

QRect SourcePickerDialog::ComputePresetRegionRect(int preset_w, int preset_h, const QRect& monitor,
                                                  const QRect& existing_region) {
    return SourcePickerPanel::ComputePresetRegionRect(preset_w, preset_h, monitor, existing_region);
}

SourcePickerDialog::SourcePickerDialog(QWidget* parent, Qt::WindowFlags flags) : QDialog(parent, flags) {
    setObjectName("sourcePickerDialog");
    setWindowTitle(QStringLiteral("Choose what to record"));
    setModal(true);
    resize(980, 700);

    panel_ = new SourcePickerPanel(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(panel_);

    connect(panel_, &SourcePickerPanel::accepted, this, &QDialog::accept);
    connect(panel_, &SourcePickerPanel::rejected, this, &QDialog::reject);
}

void SourcePickerDialog::setScreenOptions(const std::vector<SourceOption>& options) {
    panel_->setScreenOptions(options);
}

void SourcePickerDialog::setWindowOptions(const std::vector<SourceOption>& options) {
    panel_->setWindowOptions(options);
}

void SourcePickerDialog::setRegionState(const QString& summary, bool has_region, bool select_on_record,
                                        const QRect& region_rect) {
    panel_->setRegionState(summary, has_region, select_on_record, region_rect);
}

void SourcePickerDialog::setCurrentSelection(Section section, int target_index) {
    panel_->setCurrentSelection(section, target_index);
}

bool SourcePickerDialog::selectSource(Section section, int target_index) {
    return panel_->selectSource(section, target_index);
}

SourcePickerDialog::SelectionResult SourcePickerDialog::selectionResult() const {
    return panel_->selectionResult();
}

} // namespace exosnap::ui::dialogs

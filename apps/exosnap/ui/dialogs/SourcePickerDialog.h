#pragma once

#include <QDialog>
#include <QSize>

#include <memory>
#include <vector>

class QCheckBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

namespace exosnap {
class ThumbnailCapture;
}

namespace exosnap::ui::widgets {
class CaptureTargetCard;
}

namespace exosnap::ui::dialogs {

class SourcePickerDialog : public QDialog {
    Q_OBJECT
  public:
    enum class Section {
        Screens,
        Windows,
        Region,
    };

    struct SourceOption {
        int target_index = -1;
        uintptr_t native_id = 0;
        QString title;
        QString detail;
        bool primary = false;
        QString status_badge;
        bool selectable = true;
        bool unavailable = false;
        QString validation_summary;
        QString minimum_detail;
        QString help_text;
    };

    struct SelectionResult {
        Section section = Section::Screens;
        int target_index = -1;
        bool valid = false;
        bool select_on_record = true;
        bool pick_region_now = false;
    };

    explicit SourcePickerDialog(QWidget* parent = nullptr);

    void setScreenOptions(const std::vector<SourceOption>& options);
    void setWindowOptions(const std::vector<SourceOption>& options);
    void setRegionState(const QString& summary, bool has_region, bool select_on_record);
    void setCurrentSelection(Section section, int target_index);

    bool selectSource(Section section, int target_index);
    SelectionResult selectionResult() const;

  private slots:
    void onUseSelected();
    void onPickRegionNow();
    void onThumbnailReady(int target_index, QImage thumbnail);
    void onThumbnailFailed(int target_index);

  private:
    struct OptionCard {
        Section section = Section::Screens;
        int target_index = -1;
        ui::widgets::CaptureTargetCard* card = nullptr;
    };

    void rebuildOptionCards();
    void rebuildOptionCardsForSection(Section section);
    void clearLayout(QVBoxLayout* layout);
    void setActiveSection(Section section);
    void refreshSelectionVisuals();
    void updateSummaryLabel();
    bool hasValidSelection() const;
    bool hasTargetInSection(Section section, int target_index) const;
    bool findOption(Section section, int target_index, SourceOption* out) const;
    OptionCard* findOptionCard(Section section, int target_index);
    void requestThumbnailsForSection(Section section);

    static constexpr QSize kThumbnailSize{320, 180};

    QPushButton* screens_button_ = nullptr;
    QPushButton* windows_button_ = nullptr;
    QPushButton* region_button_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QVBoxLayout* screens_layout_ = nullptr;
    QVBoxLayout* windows_layout_ = nullptr;
    QLabel* region_summary_value_label_ = nullptr;
    QCheckBox* region_select_on_record_check_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QPushButton* use_button_ = nullptr;
    QPushButton* pick_region_now_button_ = nullptr;

    std::vector<SourceOption> screen_options_;
    std::vector<SourceOption> window_options_;
    std::vector<OptionCard> option_cards_;

    Section selected_section_ = Section::Screens;
    int selected_target_index_ = -1;
    bool has_region_ = false;
    QString region_summary_;
    bool pick_region_now_ = false;

    ThumbnailCapture* thumbnail_capture_ = nullptr;
};

} // namespace exosnap::ui::dialogs

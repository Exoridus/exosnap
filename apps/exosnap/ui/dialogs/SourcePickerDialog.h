#pragma once

#include <QDialog>
#include <QSize>

#include <memory>
#include <vector>

class QCheckBox;
class QBoxLayout;
class QGridLayout;
class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QResizeEvent;
class QEvent;
class QHideEvent;
class QShowEvent;
class QLayout;

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
        bool hidden_by_default = false;
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
    void onRefreshRequested();
    void onPeriodicThumbnailRefresh();

  private:
    struct SectionGrid {
        QScrollArea* scroll = nullptr;
        QVBoxLayout* content_layout = nullptr;
        QWidget* host = nullptr;
        QGridLayout* grid = nullptr;
        QLabel* empty_label = nullptr;
    };

    struct OptionCard {
        Section section = Section::Screens;
        int target_index = -1;
        ui::widgets::CaptureTargetCard* card = nullptr;
    };

    void rebuildOptionCards();
    void rebuildOptionCardsForSection(Section section);
    void relayoutSection(Section section);
    void clearLayout(QLayout* layout);
    void setActiveSection(Section section);
    void refreshSelectionVisuals();
    void updateSummaryLabel();
    bool hasValidSelection() const;
    bool hasTargetInSection(Section section, int target_index) const;
    bool findOption(Section section, int target_index, SourceOption* out) const;
    OptionCard* findOptionCard(Section section, int target_index);
    void requestThumbnailsForSection(Section section);
    bool shouldShowOption(const SourceOption& option, Section section) const;
    void updateWindowsUnavailableToggle();
    SectionGrid* sectionGrid(Section section);
    const SectionGrid* sectionGrid(Section section) const;
    std::vector<OptionCard*> cardsForSection(Section section);
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void updateResponsiveLayout();

    static constexpr QSize kThumbnailSize{320, 180};

    QPushButton* screens_button_ = nullptr;
    QPushButton* windows_button_ = nullptr;
    QPushButton* region_button_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
    QPushButton* windows_unavailable_toggle_ = nullptr;
    QBoxLayout* section_row_layout_ = nullptr;
    QBoxLayout* footer_layout_ = nullptr;
    QWidget* footer_actions_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    SectionGrid screens_grid_;
    SectionGrid windows_grid_;
    QLabel* region_summary_value_label_ = nullptr;
    QCheckBox* region_select_on_record_check_ = nullptr;
    QLabel* summary_label_ = nullptr;
    QPushButton* use_button_ = nullptr;
    QPushButton* pick_region_now_button_ = nullptr;
    QTimer* thumbnail_refresh_timer_ = nullptr;

    std::vector<SourceOption> screen_options_;
    std::vector<SourceOption> window_options_;
    std::vector<OptionCard> option_cards_;

    Section selected_section_ = Section::Screens;
    int selected_target_index_ = -1;
    bool has_region_ = false;
    QString region_summary_;
    bool pick_region_now_ = false;
    bool show_unavailable_windows_ = false;

    ThumbnailCapture* thumbnail_capture_ = nullptr;
};

} // namespace exosnap::ui::dialogs

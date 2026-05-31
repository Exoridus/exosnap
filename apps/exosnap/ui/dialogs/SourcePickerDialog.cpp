#include "SourcePickerDialog.h"

#include "../widgets/CaptureTargetCard.h"
#include "../widgets/SectionRuleHeader.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

QWidget* makeScrollableCardColumn(QWidget* parent, QVBoxLayout** out_layout) {
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addStretch(1);

    scroll->setWidget(content);
    *out_layout = layout;
    return scroll;
}

QPushButton* makeSectionButton(const QString& object_name, const QString& text, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(object_name);
    button->setCheckable(true);
    button->setProperty("sourcePickerSection", true);
    return button;
}

QString ElideCardTitle(const QString& title) {
    constexpr int kMaxChars = 68;
    const QString compact = title.simplified();
    if (compact.size() <= kMaxChars) {
        return compact;
    }
    return compact.left(kMaxChars - 1) + QStringLiteral("…");
}

void RestyleCard(QWidget* card, const char* tone) {
    card->setProperty("captureCardTone", tone);
    card->style()->unpolish(card);
    card->style()->polish(card);
    card->update();
}

} // namespace

SourcePickerDialog::SourcePickerDialog(QWidget* parent) : QDialog(parent) {
    setObjectName("sourcePickerDialog");
    setWindowTitle(QStringLiteral("Choose capture source"));
    setModal(true);
    resize(920, 640);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto* title_label = new QLabel(QStringLiteral("Choose capture source"), this);
    title_label->setObjectName("sourcePickerTitle");
    title_label->setProperty("labelRole", "captureCardTitle");
    root->addWidget(title_label);

    auto* section_row = new QHBoxLayout();
    section_row->setContentsMargins(0, 0, 0, 0);
    section_row->setSpacing(8);
    screens_button_ = makeSectionButton("sourcePickerScreensButton", QStringLiteral("Screens"), this);
    windows_button_ = makeSectionButton("sourcePickerWindowsButton", QStringLiteral("Windows"), this);
    region_button_ = makeSectionButton("sourcePickerRegionButton", QStringLiteral("Region"), this);
    section_row->addWidget(screens_button_);
    section_row->addWidget(windows_button_);
    section_row->addWidget(region_button_);
    section_row->addStretch(1);
    root->addLayout(section_row);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName("sourcePickerPages");
    pages_->addWidget(makeScrollableCardColumn(pages_, &screens_layout_));
    pages_->addWidget(makeScrollableCardColumn(pages_, &windows_layout_));

    auto* region_page = new QWidget(pages_);
    auto* region_layout = new QVBoxLayout(region_page);
    region_layout->setContentsMargins(0, 0, 0, 0);
    region_layout->setSpacing(12);

    auto* region_note = new QFrame(region_page);
    region_note->setProperty("panelRole", "note");
    auto* region_note_layout = new QVBoxLayout(region_note);
    region_note_layout->setContentsMargins(12, 10, 12, 10);
    region_note_layout->setSpacing(6);

    auto* region_title = new QLabel(QStringLiteral("Region capture"), region_note);
    region_title->setProperty("labelRole", "captureCardTitle");
    auto* region_copy =
        new QLabel(QStringLiteral("Region selection runs in the existing overlay outside this dialog. "
                                  "Use Pick region now... to launch it, or leave select-on-record enabled."),
                   region_note);
    region_copy->setWordWrap(true);
    region_copy->setProperty("labelRole", "captureTargetPickerNote");
    auto* region_cancel_copy =
        new QLabel(QStringLiteral("Press Esc in the overlay to cancel and return."), region_note);
    region_cancel_copy->setWordWrap(true);
    region_cancel_copy->setProperty("labelRole", "captureTargetPickerNote");
    region_note_layout->addWidget(region_title);
    region_note_layout->addWidget(region_copy);
    region_note_layout->addWidget(region_cancel_copy);
    region_layout->addWidget(region_note);

    auto* region_summary_panel = new QFrame(region_page);
    region_summary_panel->setProperty("panelRole", "panel");
    auto* region_summary_layout = new QVBoxLayout(region_summary_panel);
    region_summary_layout->setContentsMargins(12, 10, 12, 10);
    region_summary_layout->setSpacing(4);
    auto* region_summary_key = new QLabel(QStringLiteral("Current region"), region_summary_panel);
    region_summary_key->setProperty("labelRole", "captureTargetPickerLabel");
    region_summary_value_label_ = new QLabel(QStringLiteral("No region saved yet."), region_summary_panel);
    region_summary_value_label_->setObjectName("sourcePickerRegionSummary");
    region_summary_value_label_->setWordWrap(true);
    region_summary_value_label_->setProperty("labelRole", "captureTargetPickerNote");
    region_summary_layout->addWidget(region_summary_key);
    region_summary_layout->addWidget(region_summary_value_label_);
    region_layout->addWidget(region_summary_panel);

    region_select_on_record_check_ = new QCheckBox(QStringLiteral("Select region when recording starts"), region_page);
    region_select_on_record_check_->setObjectName("sourcePickerRegionSelectOnRecord");
    region_select_on_record_check_->setChecked(true);
    region_layout->addWidget(region_select_on_record_check_);

    pick_region_now_button_ = new QPushButton(QStringLiteral("Pick region now..."), region_page);
    pick_region_now_button_->setObjectName("sourcePickerPickRegionButton");
    pick_region_now_button_->setProperty("role", "ghost");
    region_layout->addWidget(pick_region_now_button_, 0, Qt::AlignLeft);
    region_layout->addStretch(1);

    pages_->addWidget(region_page);
    root->addWidget(pages_, 1);

    auto* footer = new QHBoxLayout();
    footer->setContentsMargins(0, 0, 0, 0);
    footer->setSpacing(8);
    summary_label_ = new QLabel(this);
    summary_label_->setObjectName("sourcePickerSummary");
    summary_label_->setProperty("labelRole", "captureTargetPickerNote");
    summary_label_->setWordWrap(true);
    footer->addWidget(summary_label_, 1);

    auto* cancel_button = new QPushButton(QStringLiteral("Cancel"), this);
    cancel_button->setObjectName("sourcePickerCancelButton");
    use_button_ = new QPushButton(QStringLiteral("Use selected source"), this);
    use_button_->setObjectName("sourcePickerUseButton");
    use_button_->setProperty("heroRole", "start");
    footer->addWidget(cancel_button);
    footer->addWidget(use_button_);
    root->addLayout(footer);

    connect(screens_button_, &QPushButton::clicked, this, [this]() {
        setActiveSection(Section::Screens);
        if (!hasTargetInSection(selected_section_, selected_target_index_) &&
            hasTargetInSection(Section::Screens, selected_target_index_)) {
            selected_section_ = Section::Screens;
        }
        refreshSelectionVisuals();
        updateSummaryLabel();
    });
    connect(windows_button_, &QPushButton::clicked, this, [this]() {
        setActiveSection(Section::Windows);
        if (!hasTargetInSection(selected_section_, selected_target_index_) &&
            hasTargetInSection(Section::Windows, selected_target_index_)) {
            selected_section_ = Section::Windows;
        }
        refreshSelectionVisuals();
        updateSummaryLabel();
    });
    connect(region_button_, &QPushButton::clicked, this, [this]() {
        selected_section_ = Section::Region;
        selected_target_index_ = -1;
        pick_region_now_ = false;
        setActiveSection(Section::Region);
        refreshSelectionVisuals();
        updateSummaryLabel();
    });
    connect(region_select_on_record_check_, &QAbstractButton::toggled, this, [this]() { updateSummaryLabel(); });
    connect(pick_region_now_button_, &QPushButton::clicked, this, &SourcePickerDialog::onPickRegionNow);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(use_button_, &QPushButton::clicked, this, &SourcePickerDialog::onUseSelected);

    screens_button_->setChecked(true);
    setActiveSection(Section::Screens);
    updateSummaryLabel();
}

void SourcePickerDialog::setScreenOptions(const std::vector<SourceOption>& options) {
    screen_options_ = options;
    rebuildOptionCards();
}

void SourcePickerDialog::setWindowOptions(const std::vector<SourceOption>& options) {
    window_options_ = options;
    rebuildOptionCards();
}

void SourcePickerDialog::setRegionState(const QString& summary, bool has_region, bool select_on_record) {
    has_region_ = has_region;
    region_summary_ = summary;
    region_select_on_record_check_->setChecked(select_on_record);
    if (region_summary_.trimmed().isEmpty()) {
        region_summary_value_label_->setText(QStringLiteral("No region saved yet."));
    } else {
        region_summary_value_label_->setText(region_summary_);
    }
    updateSummaryLabel();
}

void SourcePickerDialog::setCurrentSelection(Section section, int target_index) {
    pick_region_now_ = false;
    selected_section_ = section;
    selected_target_index_ = target_index;

    if (section != Section::Region && !hasTargetInSection(section, target_index)) {
        const auto& fallback = (section == Section::Windows) ? window_options_ : screen_options_;
        selected_target_index_ = -1;
        for (const auto& option : fallback) {
            if (option.selectable) {
                selected_target_index_ = option.target_index;
                break;
            }
        }
        if (selected_target_index_ < 0 && !fallback.empty()) {
            selected_target_index_ = fallback.front().target_index;
        }
    }

    setActiveSection(section);
    refreshSelectionVisuals();
    updateSummaryLabel();
}

bool SourcePickerDialog::selectSource(Section section, int target_index) {
    if (section == Section::Region) {
        setCurrentSelection(Section::Region, -1);
        return true;
    }
    SourceOption option;
    if (!findOption(section, target_index, &option) || !option.selectable) {
        return false;
    }
    setCurrentSelection(section, target_index);
    return true;
}

SourcePickerDialog::SelectionResult SourcePickerDialog::selectionResult() const {
    SelectionResult result;
    result.section = selected_section_;
    result.target_index = selected_target_index_;
    result.valid = hasValidSelection();
    result.select_on_record = region_select_on_record_check_ ? region_select_on_record_check_->isChecked() : true;
    result.pick_region_now = pick_region_now_;
    return result;
}

void SourcePickerDialog::onUseSelected() {
    pick_region_now_ = false;
    if (!hasValidSelection()) {
        return;
    }
    accept();
}

void SourcePickerDialog::onPickRegionNow() {
    selected_section_ = Section::Region;
    selected_target_index_ = -1;
    pick_region_now_ = true;
    setActiveSection(Section::Region);
    refreshSelectionVisuals();
    updateSummaryLabel();
    accept();
}

void SourcePickerDialog::rebuildOptionCards() {
    clearLayout(screens_layout_);
    clearLayout(windows_layout_);
    option_cards_.clear();

    rebuildOptionCardsForSection(Section::Screens);
    rebuildOptionCardsForSection(Section::Windows);
    refreshSelectionVisuals();
    updateSummaryLabel();
}

void SourcePickerDialog::rebuildOptionCardsForSection(Section section) {
    auto* layout = section == Section::Screens ? screens_layout_ : windows_layout_;
    if (!layout) {
        return;
    }

    const auto& options = section == Section::Screens ? screen_options_ : window_options_;
    if (options.empty()) {
        auto* empty_label = new QLabel(section == Section::Screens ? QStringLiteral("No displays detected.")
                                                                   : QStringLiteral("No capturable windows found."),
                                       pages_);
        empty_label->setProperty("labelRole", "captureTargetPickerNote");
        layout->addWidget(empty_label);
        layout->addStretch(1);
        return;
    }

    for (const auto& option : options) {
        auto* card = new ui::widgets::CaptureTargetCard(pages_);
        card->setTitle(ElideCardTitle(option.title));
        card->setToolTip(option.title);
        QString subtitle = option.detail;
        if (option.primary && section == Section::Screens) {
            subtitle =
                subtitle.isEmpty() ? QStringLiteral("Primary display") : (subtitle + QStringLiteral(" · Primary"));
        }
        if (!option.minimum_detail.trimmed().isEmpty()) {
            subtitle =
                subtitle.isEmpty() ? option.minimum_detail : (subtitle + QStringLiteral("\n") + option.minimum_detail);
        }
        if (subtitle.isEmpty()) {
            subtitle =
                section == Section::Screens ? QStringLiteral("Display capture") : QStringLiteral("Window capture");
        }
        card->setSubtitle(subtitle);
        card->setStatusText(option.status_badge.isEmpty()
                                ? (section == Section::Screens ? QStringLiteral("SCREEN") : QStringLiteral("WINDOW"))
                                : option.status_badge);
        RestyleCard(card, option.selectable ? "default" : "warning");
        card->setAccessibleName(
            (section == Section::Screens ? QStringLiteral("Screen source: ") : QStringLiteral("Window source: ")) +
            option.title);
        layout->addWidget(card);
        option_cards_.push_back({section, option.target_index, card});

        connect(card, &ui::widgets::CaptureTargetCard::clicked, this,
                [this, section, target_index = option.target_index]() {
                    selected_section_ = section;
                    selected_target_index_ = target_index;
                    pick_region_now_ = false;
                    setActiveSection(section);
                    refreshSelectionVisuals();
                    updateSummaryLabel();
                });
    }

    layout->addStretch(1);
}

void SourcePickerDialog::clearLayout(QVBoxLayout* layout) {
    if (!layout) {
        return;
    }

    QLayoutItem* item = nullptr;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (auto* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }
}

void SourcePickerDialog::setActiveSection(Section section) {
    selected_section_ = section;

    screens_button_->setChecked(section == Section::Screens);
    windows_button_->setChecked(section == Section::Windows);
    region_button_->setChecked(section == Section::Region);

    switch (section) {
    case Section::Screens:
        pages_->setCurrentIndex(0);
        break;
    case Section::Windows:
        pages_->setCurrentIndex(1);
        break;
    case Section::Region:
        pages_->setCurrentIndex(2);
        break;
    }
}

void SourcePickerDialog::refreshSelectionVisuals() {
    for (auto& option_card : option_cards_) {
        if (!option_card.card) {
            continue;
        }
        const bool selected =
            option_card.section == selected_section_ && option_card.target_index == selected_target_index_;
        option_card.card->setSelected(selected);
    }
    use_button_->setEnabled(hasValidSelection());
}

void SourcePickerDialog::updateSummaryLabel() {
    if (!summary_label_) {
        return;
    }

    if (selected_section_ == Section::Region) {
        const QString region_text = (has_region_ && !region_summary_.trimmed().isEmpty())
                                        ? QStringLiteral("Region · %1").arg(region_summary_)
                                        : QStringLiteral("Region · no saved region");
        const QString select_mode = region_select_on_record_check_ && region_select_on_record_check_->isChecked()
                                        ? QStringLiteral(" · overlay opens when recording starts")
                                        : QStringLiteral(" · use Pick region now... to set one");
        summary_label_->setText(region_text + select_mode);
        return;
    }

    SourceOption option;
    if (!findOption(selected_section_, selected_target_index_, &option)) {
        summary_label_->setText(selected_section_ == Section::Windows ? QStringLiteral("Choose a window source")
                                                                      : QStringLiteral("Choose a screen source"));
        return;
    }
    if (!option.selectable) {
        QString summary = option.validation_summary.trimmed();
        if (summary.isEmpty()) {
            summary = QStringLiteral("Selected source is not valid for the active encoder.");
        }
        if (!option.minimum_detail.trimmed().isEmpty()) {
            summary += QStringLiteral(" ") + option.minimum_detail;
        }
        summary_label_->setText(summary);
        return;
    }

    QString summary = option.title;
    if (!option.detail.trimmed().isEmpty()) {
        summary += QStringLiteral(" · ") + option.detail;
    }
    summary_label_->setText(summary);
}

bool SourcePickerDialog::hasValidSelection() const {
    if (selected_section_ == Section::Region) {
        return true;
    }
    SourceOption option;
    return findOption(selected_section_, selected_target_index_, &option) && option.selectable;
}

bool SourcePickerDialog::hasTargetInSection(Section section, int target_index) const {
    const auto& options = section == Section::Windows ? window_options_ : screen_options_;
    for (const auto& option : options) {
        if (option.target_index == target_index) {
            return true;
        }
    }
    return false;
}

bool SourcePickerDialog::findOption(Section section, int target_index, SourceOption* out) const {
    const auto& options = section == Section::Windows ? window_options_ : screen_options_;
    for (const auto& option : options) {
        if (option.target_index == target_index) {
            if (out) {
                *out = option;
            }
            return true;
        }
    }
    return false;
}

} // namespace exosnap::ui::dialogs

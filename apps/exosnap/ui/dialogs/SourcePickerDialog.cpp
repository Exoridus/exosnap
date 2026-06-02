#include "SourcePickerDialog.h"

#include "../../services/ThumbnailCapture.h"
#include "../widgets/CaptureTargetCard.h"

#include <QCheckBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace exosnap::ui::dialogs {
namespace {

struct GridPageWidgets {
    QScrollArea* scroll = nullptr;
    QVBoxLayout* content_layout = nullptr;
    QWidget* host = nullptr;
    QGridLayout* grid = nullptr;
    QLabel* empty_label = nullptr;
};

GridPageWidgets makeScrollableCardGrid(QWidget* parent, const QString& hint_text, const QString& empty_text) {
    GridPageWidgets result;

    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scroll);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(10);

    auto* hint_label = new QLabel(hint_text, content);
    hint_label->setWordWrap(true);
    hint_label->setProperty("labelRole", "captureTargetPickerNote");
    content_layout->addWidget(hint_label);

    auto* empty_label = new QLabel(empty_text, content);
    empty_label->setWordWrap(true);
    empty_label->setProperty("labelRole", "captureTargetPickerNote");
    empty_label->setVisible(false);
    content_layout->addWidget(empty_label);

    auto* host = new QWidget(content);
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);
    grid->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    content_layout->addWidget(host, 1);

    scroll->setWidget(content);

    result.scroll = scroll;
    result.content_layout = content_layout;
    result.host = host;
    result.grid = grid;
    result.empty_label = empty_label;
    return result;
}

QPushButton* makeSectionButton(const QString& object_name, const QString& text, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(object_name);
    button->setCheckable(true);
    button->setProperty("sourcePickerSection", true);
    return button;
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
    resize(980, 700);

    thumbnail_capture_ = new ThumbnailCapture(this);
    connect(thumbnail_capture_, &ThumbnailCapture::thumbnailReady, this, &SourcePickerDialog::onThumbnailReady);
    connect(thumbnail_capture_, &ThumbnailCapture::thumbnailFailed, this, &SourcePickerDialog::onThumbnailFailed);

    thumbnail_refresh_timer_ = new QTimer(this);
    thumbnail_refresh_timer_->setInterval(6000);
    connect(thumbnail_refresh_timer_, &QTimer::timeout, this, &SourcePickerDialog::onPeriodicThumbnailRefresh);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto* header_panel = new QFrame(this);
    header_panel->setProperty("panelRole", "panel");
    auto* header_layout = new QVBoxLayout(header_panel);
    header_layout->setContentsMargins(12, 10, 12, 10);
    header_layout->setSpacing(4);

    auto* title_label = new QLabel(QStringLiteral("Choose capture source"), header_panel);
    title_label->setObjectName("sourcePickerTitle");
    title_label->setProperty("labelRole", "captureCardTitle");
    header_layout->addWidget(title_label);

    const QString subtitle_text = QStringLiteral(
        "Pick what to record using visual previews. Thumbnails refresh when opening tabs or rescanning.");
    auto* subtitle_label = new QLabel(subtitle_text, header_panel);
    subtitle_label->setWordWrap(true);
    subtitle_label->setProperty("labelRole", "captureTargetPickerNote");
    header_layout->addWidget(subtitle_label);
    root->addWidget(header_panel);

    auto* section_tabs = new QWidget(this);
    section_tabs->setObjectName("sourcePickerSectionTabs");
    auto* section_row = new QHBoxLayout(section_tabs);
    section_row->setContentsMargins(0, 0, 0, 0);
    section_row->setSpacing(8);
    screens_button_ = makeSectionButton("sourcePickerScreensButton", QStringLiteral("Screens"), section_tabs);
    windows_button_ = makeSectionButton("sourcePickerWindowsButton", QStringLiteral("Windows"), section_tabs);
    region_button_ = makeSectionButton("sourcePickerRegionButton", QStringLiteral("Region"), section_tabs);
    section_row->addWidget(screens_button_);
    section_row->addWidget(windows_button_);
    section_row->addWidget(region_button_);
    section_row->addSpacing(8);
    refresh_button_ = new QPushButton(QStringLiteral("Refresh previews"), section_tabs);
    refresh_button_->setObjectName("sourcePickerRefreshButton");
    refresh_button_->setProperty("role", "utility");
    section_row->addWidget(refresh_button_);
    section_row->addStretch(1);
    root->addWidget(section_tabs);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName("sourcePickerPages");

    const auto screens_page =
        makeScrollableCardGrid(pages_, QStringLiteral("Choose a display visually. Primary displays are marked."),
                               QStringLiteral("No displays detected."));
    screens_grid_.scroll = screens_page.scroll;
    screens_grid_.content_layout = screens_page.content_layout;
    screens_grid_.host = screens_page.host;
    screens_grid_.grid = screens_page.grid;
    screens_grid_.empty_label = screens_page.empty_label;
    pages_->addWidget(screens_grid_.scroll);

    const auto windows_page =
        makeScrollableCardGrid(pages_, QStringLiteral("Find the right app/window quickly from preview cards."),
                               QStringLiteral("No capturable windows found."));
    windows_grid_.scroll = windows_page.scroll;
    windows_grid_.content_layout = windows_page.content_layout;
    windows_grid_.host = windows_page.host;
    windows_grid_.grid = windows_page.grid;
    windows_grid_.empty_label = windows_page.empty_label;
    windows_unavailable_toggle_ = new QPushButton(QStringLiteral("Show unavailable (0)"), pages_);
    windows_unavailable_toggle_->setObjectName("sourcePickerShowUnavailableButton");
    windows_unavailable_toggle_->setProperty("role", "utility");
    windows_unavailable_toggle_->setCheckable(true);
    windows_unavailable_toggle_->setVisible(false);
    if (windows_grid_.content_layout) {
        windows_grid_.content_layout->addWidget(windows_unavailable_toggle_, 0, Qt::AlignLeft);
    }
    pages_->addWidget(windows_grid_.scroll);

    if (screens_grid_.scroll && screens_grid_.scroll->viewport()) {
        screens_grid_.scroll->viewport()->installEventFilter(this);
    }
    if (windows_grid_.scroll && windows_grid_.scroll->viewport()) {
        windows_grid_.scroll->viewport()->installEventFilter(this);
    }

    auto* region_page = new QWidget(pages_);
    auto* region_layout = new QVBoxLayout(region_page);
    region_layout->setContentsMargins(0, 0, 0, 0);
    region_layout->setSpacing(8);

    auto* region_note = new QFrame(region_page);
    region_note->setProperty("panelRole", "note");
    auto* region_note_layout = new QVBoxLayout(region_note);
    region_note_layout->setContentsMargins(12, 10, 12, 10);
    region_note_layout->setSpacing(6);

    auto* region_title = new QLabel(QStringLiteral("Region capture"), region_note);
    region_title->setProperty("labelRole", "captureCardTitle");
    auto* region_copy =
        new QLabel(QStringLiteral("Use this when you need only part of a screen. The region overlay appears outside "
                                  "this dialog and can be triggered now or when recording starts."),
                   region_note);
    region_copy->setWordWrap(true);
    region_copy->setProperty("labelRole", "captureTargetPickerNote");
    region_note_layout->addWidget(region_title);
    region_note_layout->addWidget(region_copy);
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
    pick_region_now_button_->setProperty("role", "primary");
    region_layout->addWidget(pick_region_now_button_, 0, Qt::AlignLeft);

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
    use_button_->setProperty("role", "primary");
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
    connect(refresh_button_, &QPushButton::clicked, this, &SourcePickerDialog::onRefreshRequested);
    connect(windows_unavailable_toggle_, &QPushButton::clicked, this, [this]() {
        show_unavailable_windows_ = windows_unavailable_toggle_ && windows_unavailable_toggle_->isChecked();
        rebuildOptionCardsForSection(Section::Windows);
        updateWindowsUnavailableToggle();
        refreshSelectionVisuals();
        if (isVisible() && selected_section_ == Section::Windows) {
            requestThumbnailsForSection(Section::Windows);
        }
    });
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
    if (isVisible() && selected_section_ == Section::Screens) {
        requestThumbnailsForSection(Section::Screens);
    }
}

void SourcePickerDialog::setWindowOptions(const std::vector<SourceOption>& options) {
    window_options_ = options;
    const bool has_hidden = std::any_of(window_options_.begin(), window_options_.end(),
                                        [](const SourceOption& option) { return option.hidden_by_default; });
    if (!has_hidden) {
        show_unavailable_windows_ = false;
    }
    if (windows_unavailable_toggle_) {
        windows_unavailable_toggle_->setChecked(show_unavailable_windows_);
    }
    rebuildOptionCards();
    if (isVisible() && selected_section_ == Section::Windows) {
        requestThumbnailsForSection(Section::Windows);
    }
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

void SourcePickerDialog::onThumbnailReady(int target_index, const QImage thumbnail) {
    auto* card = findOptionCard(Section::Screens, target_index);
    if (!card) {
        card = findOptionCard(Section::Windows, target_index);
    }
    if (!card || !card->card) {
        return;
    }

    QPixmap pixmap = QPixmap::fromImage(thumbnail);
    card->card->setThumbnail(pixmap);
}

void SourcePickerDialog::onThumbnailFailed(int target_index) {
    auto* card = findOptionCard(Section::Screens, target_index);
    if (!card) {
        card = findOptionCard(Section::Windows, target_index);
    }
    if (!card || !card->card) {
        return;
    }
    if (card->card->isUnavailable()) {
        return;
    }
    card->card->setThumbnailFailureText(QStringLiteral("Preview unavailable"));
}

void SourcePickerDialog::onRefreshRequested() {
    if (selected_section_ == Section::Region) {
        return;
    }
    requestThumbnailsForSection(selected_section_);
}

void SourcePickerDialog::onPeriodicThumbnailRefresh() {
    if (!isVisible() || selected_section_ == Section::Region) {
        return;
    }
    requestThumbnailsForSection(selected_section_);
}

void SourcePickerDialog::rebuildOptionCards() {
    clearLayout(screens_grid_.grid);
    clearLayout(windows_grid_.grid);
    option_cards_.clear();

    rebuildOptionCardsForSection(Section::Screens);
    rebuildOptionCardsForSection(Section::Windows);
    updateWindowsUnavailableToggle();
    refreshSelectionVisuals();
    updateSummaryLabel();
}

void SourcePickerDialog::rebuildOptionCardsForSection(Section section) {
    auto* grid_info = sectionGrid(section);
    if (!grid_info || !grid_info->grid) {
        return;
    }

    option_cards_.erase(std::remove_if(option_cards_.begin(), option_cards_.end(),
                                       [section](const OptionCard& option_card) {
                                           if (option_card.section != section) {
                                               return false;
                                           }
                                           if (option_card.card) {
                                               delete option_card.card;
                                           }
                                           return true;
                                       }),
                        option_cards_.end());

    const auto& all_options = section == Section::Screens ? screen_options_ : window_options_;

    std::vector<const SourceOption*> visible_options;
    visible_options.reserve(all_options.size());
    for (const auto& option : all_options) {
        if (shouldShowOption(option, section)) {
            visible_options.push_back(&option);
        }
    }

    if (grid_info->empty_label) {
        grid_info->empty_label->setVisible(visible_options.empty());
    }
    if (grid_info->host) {
        grid_info->host->setVisible(!visible_options.empty());
    }
    if (visible_options.empty()) {
        relayoutSection(section);
        return;
    }

    for (const SourceOption* option_ptr : visible_options) {
        if (!option_ptr) {
            continue;
        }
        const SourceOption& option = *option_ptr;
        auto* card = new ui::widgets::CaptureTargetCard(pages_);
        card->setTitle(option.title);
        card->setToolTip(option.title);

        auto stripJargon = [](QString s) -> QString {
            static const QStringList kJargon = {
                QStringLiteral("DXGI OD monitor capture"),
                QStringLiteral("WGC monitor capture"),
                QStringLiteral("WGC window capture"),
            };
            for (const QString& j : kJargon) {
                int pos = s.indexOf(j, 0, Qt::CaseInsensitive);
                while (pos >= 0) {
                    s.remove(pos, j.length());
                    while (pos < s.length() &&
                           (s[pos] == QLatin1Char(' ') || s[pos] == QChar(0xB7) || s[pos] == QLatin1Char('\n')))
                        s.remove(pos, 1);
                    pos = s.indexOf(j, pos, Qt::CaseInsensitive);
                }
            }
            s = s.trimmed();
            while (s.startsWith(QStringLiteral(" · ")))
                s = s.mid(3);
            while (s.endsWith(QStringLiteral(" · ")))
                s.chop(3);
            return s;
        };

        QString subtitle = stripJargon(option.detail);

        if (option.primary && section == Section::Screens) {
            const bool already_primary = subtitle.contains(QStringLiteral("Primary"), Qt::CaseInsensitive);
            if (!already_primary) {
                if (subtitle.isEmpty()) {
                    subtitle = QStringLiteral("Primary display");
                } else {
                    subtitle += QStringLiteral(" · Primary");
                }
            }
        }
        if (!option.minimum_detail.trimmed().isEmpty()) {
            const QString min_detail = stripJargon(option.minimum_detail);
            subtitle = subtitle.isEmpty() ? min_detail : (subtitle + QStringLiteral("\n") + min_detail);
        }
        if (subtitle.isEmpty()) {
            subtitle =
                section == Section::Screens ? QStringLiteral("Display capture") : QStringLiteral("Window capture");
        }
        card->setSubtitle(subtitle);

        card->setStatusText(option.status_badge.trimmed());

        QString state = QStringLiteral("normal");
        const QString status_detail =
            QStringList({option.status_badge, option.validation_summary, option.minimum_detail, option.help_text})
                .join(QStringLiteral(" "))
                .toLower();

        if (option.unavailable) {
            if (status_detail.contains(QStringLiteral("minim"))) {
                state = QStringLiteral("minimized");
            } else {
                state = QStringLiteral("unavailable");
            }
            card->setProperty("captureCardState", state);
            RestyleCard(card, "warning");
            card->setUnavailable(true);
            card->setHelpText(option.help_text);
            card->setThumbnailUnavailableText(option.status_badge.isEmpty() ? QStringLiteral("Unavailable")
                                                                            : option.status_badge);
        } else {
            if (!option.selectable) {
                state = status_detail.contains(QStringLiteral("small")) ? QStringLiteral("too-small")
                                                                        : QStringLiteral("warning");
            }
            card->setProperty("captureCardState", state);
            RestyleCard(card, option.selectable ? "default" : "warning");
            QString help_text = option.help_text;
            if (!option.selectable && help_text.isEmpty() && !option.validation_summary.trimmed().isEmpty()) {
                help_text = option.validation_summary;
            }
            card->setHelpText(help_text);
            card->setThumbnailLoadingText(QStringLiteral("Loading preview..."));
        }

        card->setAccessibleName(
            (section == Section::Screens ? QStringLiteral("Screen source: ") : QStringLiteral("Window source: ")) +
            option.title);
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

    relayoutSection(section);
}

void SourcePickerDialog::relayoutSection(Section section) {
    auto* grid_info = sectionGrid(section);
    if (!grid_info || !grid_info->grid) {
        return;
    }

    QLayoutItem* item = nullptr;
    while ((item = grid_info->grid->takeAt(0)) != nullptr) {
        delete item;
    }

    auto cards = cardsForSection(section);
    if (cards.empty()) {
        return;
    }

    const int spacing = std::max(8, grid_info->grid->horizontalSpacing());
    const int viewport_width =
        (grid_info->scroll && grid_info->scroll->viewport()) ? grid_info->scroll->viewport()->width() : width();
    const int min_card_width = section == Section::Screens ? 332 : 300;
    const int max_card_width = section == Section::Screens ? 460 : 380;
    const int max_columns = section == Section::Screens ? 3 : 4;
    const int usable_width = std::max(220, viewport_width - 4);

    int columns = std::max(1, (usable_width + spacing) / (min_card_width + spacing));
    columns = std::clamp(columns, 1, max_columns);

    int card_width = (usable_width - ((columns - 1) * spacing)) / columns;
    card_width = std::clamp(card_width, min_card_width, max_card_width);
    if (columns == 1) {
        card_width = std::min(max_card_width, usable_width);
    }

    for (int i = 0; i <= max_columns; ++i) {
        grid_info->grid->setColumnStretch(i, 0);
    }
    grid_info->grid->setColumnStretch(columns, 1);

    for (std::size_t i = 0; i < cards.size(); ++i) {
        auto* option_card = cards[i];
        if (!option_card || !option_card->card) {
            continue;
        }
        option_card->card->setFixedWidth(card_width);
        const int row = static_cast<int>(i) / columns;
        const int col = static_cast<int>(i) % columns;
        grid_info->grid->addWidget(option_card->card, row, col);
    }
}

void SourcePickerDialog::clearLayout(QLayout* layout) {
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
        relayoutSection(Section::Screens);
        break;
    case Section::Windows:
        pages_->setCurrentIndex(1);
        relayoutSection(Section::Windows);
        break;
    case Section::Region:
        pages_->setCurrentIndex(2);
        break;
    }

    if (refresh_button_) {
        refresh_button_->setEnabled(section != Section::Region);
    }
    if (section != Section::Region && isVisible()) {
        requestThumbnailsForSection(section);
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
        const bool help_duplicates_minimum =
            !option.minimum_detail.trimmed().isEmpty() &&
            option.help_text.trimmed().compare(option.minimum_detail.trimmed(), Qt::CaseInsensitive) == 0;
        if (!option.help_text.isEmpty() && !help_duplicates_minimum) {
            summary += QStringLiteral(" ") + option.help_text;
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

SourcePickerDialog::OptionCard* SourcePickerDialog::findOptionCard(Section section, int target_index) {
    for (auto& oc : option_cards_) {
        if (oc.section == section && oc.target_index == target_index) {
            return &oc;
        }
    }
    return nullptr;
}

bool SourcePickerDialog::shouldShowOption(const SourceOption& option, Section section) const {
    if (section != Section::Windows) {
        return true;
    }
    if (!option.hidden_by_default) {
        return true;
    }
    if (show_unavailable_windows_) {
        return true;
    }
    return selected_section_ == Section::Windows && selected_target_index_ == option.target_index;
}

void SourcePickerDialog::updateWindowsUnavailableToggle() {
    if (!windows_unavailable_toggle_) {
        return;
    }

    const int hidden_count =
        static_cast<int>(std::count_if(window_options_.begin(), window_options_.end(),
                                       [](const SourceOption& option) { return option.hidden_by_default; }));

    if (hidden_count <= 0) {
        show_unavailable_windows_ = false;
        windows_unavailable_toggle_->setChecked(false);
        windows_unavailable_toggle_->setVisible(false);
        return;
    }

    windows_unavailable_toggle_->setVisible(true);
    windows_unavailable_toggle_->setChecked(show_unavailable_windows_);
    windows_unavailable_toggle_->setText(show_unavailable_windows_
                                             ? QStringLiteral("Hide unavailable (%1)").arg(hidden_count)
                                             : QStringLiteral("Show unavailable (%1)").arg(hidden_count));
}

void SourcePickerDialog::requestThumbnailsForSection(Section section) {
    if (!thumbnail_capture_ || section == Section::Region) {
        return;
    }

    thumbnail_capture_->cancelAll();

    const auto& options = section == Section::Screens ? screen_options_ : window_options_;
    for (const auto& option : options) {
        if (!shouldShowOption(option, section)) {
            continue;
        }
        if (option.native_id == 0) {
            continue;
        }
        if (section == Section::Windows && option.unavailable) {
            continue;
        }

        if (auto* oc = findOptionCard(section, option.target_index); oc && oc->card && !oc->card->hasThumbnail()) {
            oc->card->setThumbnailLoadingText(QStringLiteral("Loading preview..."));
        }

        if (section == Section::Screens) {
            thumbnail_capture_->requestMonitorThumbnail(option.target_index, option.native_id, kThumbnailSize);
        } else {
            thumbnail_capture_->requestWindowThumbnail(option.target_index, option.native_id, kThumbnailSize);
        }
    }
}

SourcePickerDialog::SectionGrid* SourcePickerDialog::sectionGrid(Section section) {
    switch (section) {
    case Section::Screens:
        return &screens_grid_;
    case Section::Windows:
        return &windows_grid_;
    case Section::Region:
        return nullptr;
    }
    return nullptr;
}

const SourcePickerDialog::SectionGrid* SourcePickerDialog::sectionGrid(Section section) const {
    switch (section) {
    case Section::Screens:
        return &screens_grid_;
    case Section::Windows:
        return &windows_grid_;
    case Section::Region:
        return nullptr;
    }
    return nullptr;
}

std::vector<SourcePickerDialog::OptionCard*> SourcePickerDialog::cardsForSection(Section section) {
    std::vector<OptionCard*> cards;
    cards.reserve(option_cards_.size());
    for (auto& card : option_cards_) {
        if (card.section == section) {
            cards.push_back(&card);
        }
    }
    return cards;
}

bool SourcePickerDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event && event->type() == QEvent::Resize) {
        if (screens_grid_.scroll && watched == screens_grid_.scroll->viewport()) {
            relayoutSection(Section::Screens);
        } else if (windows_grid_.scroll && watched == windows_grid_.scroll->viewport()) {
            relayoutSection(Section::Windows);
        }
    }
    return QDialog::eventFilter(watched, event);
}

void SourcePickerDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (thumbnail_refresh_timer_) {
        thumbnail_refresh_timer_->start();
    }
    if (selected_section_ != Section::Region) {
        requestThumbnailsForSection(selected_section_);
    }
}

void SourcePickerDialog::hideEvent(QHideEvent* event) {
    if (thumbnail_refresh_timer_) {
        thumbnail_refresh_timer_->stop();
    }
    if (thumbnail_capture_) {
        thumbnail_capture_->cancelAll();
    }
    QDialog::hideEvent(event);
}

} // namespace exosnap::ui::dialogs

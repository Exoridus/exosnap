#include "SourcePickerPanel.h"

#include "../../services/ThumbnailCapture.h"
#include "../widgets/CaptureTargetCard.h"
#include "../widgets/ExoCheckBox.h"
#include "../widgets/RegionGeometry.h"
#include "../widgets/RegionPresetCard.h"

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
#include <array>
#include <cmath>

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

// Mirrors recorder_core::CaptureRegion::kMinDimension. Kept local so the
// dialog stays free of the engine headers.
constexpr int kRegionMinDimension = 64;

// Strip backend capture-method jargon (DXGI/WGC) so it never leaks into the
// visible source metadata or the selection summary footer.
QString StripCaptureJargon(QString s) {
    static const QStringList kJargon = {
        QStringLiteral("DXGI OD monitor capture"),
        QStringLiteral("WGC monitor capture"),
        QStringLiteral("WGC window capture"),
    };
    auto isSep = [](QChar c) { return c == QLatin1Char(' ') || c == QChar(0xB7) || c == QLatin1Char('\n'); };
    for (const QString& j : kJargon) {
        int pos = s.indexOf(j, 0, Qt::CaseInsensitive);
        while (pos >= 0) {
            s.remove(pos, j.length());
            while (pos < s.length() && isSep(s[pos]))
                s.remove(pos, 1);
            pos = s.indexOf(j, pos, Qt::CaseInsensitive);
        }
    }
    s = s.trimmed();
    while (!s.isEmpty() && isSep(s.front()))
        s.remove(0, 1);
    while (!s.isEmpty() && isSep(s.back()))
        s.chop(1);
    return s;
}

} // namespace

QRect SourcePickerPanel::ComputePresetRegionRect(int preset_w, int preset_h, const QRect& monitor,
                                                 const QRect& existing_region) {
    return ui::widgets::PresetRegionWithinMonitor(preset_w, preset_h, monitor, existing_region, kRegionMinDimension);
}

SourcePickerPanel::SourcePickerPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("sourcePickerDialog");
    // Plain QWidgets ignore stylesheet backgrounds without this attribute; the
    // panel card (bg + border, incl. the footer zone) must paint opaquely so
    // underlying page content cannot bleed through the overlay (VR-010).
    setAttribute(Qt::WA_StyledBackground, true);

    thumbnail_capture_ = new ThumbnailCapture(this);
    connect(thumbnail_capture_, &ThumbnailCapture::thumbnailReady, this, &SourcePickerPanel::onThumbnailReady);
    connect(thumbnail_capture_, &ThumbnailCapture::thumbnailFailed, this, &SourcePickerPanel::onThumbnailFailed);

    thumbnail_refresh_timer_ = new QTimer(this);
    thumbnail_refresh_timer_->setInterval(6000);
    connect(thumbnail_refresh_timer_, &QTimer::timeout, this, &SourcePickerPanel::onPeriodicThumbnailRefresh);

    source_refresh_timer_ = new QTimer(this);
    source_refresh_timer_->setInterval(1500);
    source_refresh_timer_->setSingleShot(false);
    connect(source_refresh_timer_, &QTimer::timeout, this, &SourcePickerPanel::onSourceRefreshTimer);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(12);

    auto* header_panel = new QFrame(this);
    header_panel->setObjectName("sourcePickerHeader");
    header_panel->setProperty("panelRole", "panel");
    auto* header_layout = new QHBoxLayout(header_panel);
    header_layout->setContentsMargins(16, 12, 16, 12);
    header_layout->setSpacing(16);

    auto* header_text = new QVBoxLayout();
    header_text->setContentsMargins(0, 0, 0, 0);
    header_text->setSpacing(2);
    auto* title_label = new QLabel(QStringLiteral("Choose what to record"), header_panel);
    title_label->setObjectName("sourcePickerTitle");
    title_label->setProperty("labelRole", "sourcePickerTitle");
    auto* subtitle_label = new QLabel(QStringLiteral("Pick a display, window, or region."), header_panel);
    subtitle_label->setObjectName("sourcePickerSubtitle");
    subtitle_label->setProperty("labelRole", "sourcePickerSubtitle");
    header_text->addWidget(title_label);
    header_text->addWidget(subtitle_label);
    header_layout->addLayout(header_text, 1);

    auto* section_tabs = new QWidget(header_panel);
    section_tabs->setObjectName("sourcePickerSectionTabs");
    auto* section_row = new QHBoxLayout(section_tabs);
    section_row->setContentsMargins(3, 3, 3, 3);
    section_row->setSpacing(4);
    screens_button_ = makeSectionButton("sourcePickerScreensButton", QStringLiteral("Displays"), section_tabs);
    windows_button_ = makeSectionButton("sourcePickerWindowsButton", QStringLiteral("Windows"), section_tabs);
    region_button_ = makeSectionButton("sourcePickerRegionButton", QStringLiteral("Region"), section_tabs);
    section_row->addWidget(screens_button_);
    section_row->addWidget(windows_button_);
    section_row->addWidget(region_button_);
    header_layout->addWidget(section_tabs, 0, Qt::AlignVCenter);

    refresh_button_ = new QPushButton(QStringLiteral("Rescan"), header_panel);
    refresh_button_->setObjectName("sourcePickerRefreshButton");
    refresh_button_->setProperty("role", "utility");
    header_layout->addWidget(refresh_button_, 0, Qt::AlignVCenter);

    root->addWidget(header_panel);

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
    auto* region_page_layout = new QVBoxLayout(region_page);
    region_page_layout->setContentsMargins(0, 0, 0, 0);
    region_page_layout->setSpacing(0);

    auto* region_scroll = new QScrollArea(region_page);
    region_scroll->setObjectName("sourcePickerRegionScroll");
    region_scroll->setWidgetResizable(true);
    region_scroll->setFrameShape(QFrame::NoFrame);
    region_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* region_content = new QWidget(region_scroll);
    auto* region_layout = new QVBoxLayout(region_content);
    region_layout->setContentsMargins(0, 0, 0, 0);
    region_layout->setSpacing(14);

    auto* region_grid_host = new QWidget(region_content);
    auto* region_grid = new QGridLayout(region_grid_host);
    region_grid->setContentsMargins(0, 0, 0, 0);
    region_grid->setHorizontalSpacing(12);
    region_grid->setVerticalSpacing(12);
    region_grid->setAlignment(Qt::AlignTop);

    struct RegionPresetDef {
        const char* title;
        const char* detail;
        int width; // 0 for the draw card
        int height;
        double aspect;
        bool draw;
    };
    const std::array<RegionPresetDef, 6> region_presets = {{
        {"Draw custom region", "Select an area", 0, 0, 16.0 / 9.0, true},
        {"16:9 Landscape", "1920 × 1080", 1920, 1080, 16.0 / 9.0, false},
        {"16:9 HD", "1280 × 720", 1280, 720, 16.0 / 9.0, false},
        {"9:16 Vertical", "1080 × 1920", 1080, 1920, 9.0 / 16.0, false},
        {"1:1 Square", "1080 × 1080", 1080, 1080, 1.0, false},
        {"4:5 Portrait", "1080 × 1350", 1080, 1350, 4.0 / 5.0, false},
    }};

    constexpr int kRegionColumns = 3;
    for (int i = 0; i < static_cast<int>(region_presets.size()); ++i) {
        const RegionPresetDef& def = region_presets[static_cast<std::size_t>(i)];
        auto* card = new ui::widgets::RegionPresetCard(region_grid_host);
        card->setTitle(QString::fromUtf8(def.title));
        card->setDetail(QString::fromUtf8(def.detail));
        card->setAspectRatio(def.aspect);
        card->setDrawVariant(def.draw);
        // Presets start planned until display geometry is known (see
        // updateRegionPresetAvailability); the draw card is always active.
        card->setPlanned(!def.draw);
        const int entry_index = static_cast<int>(region_preset_entries_.size());
        region_preset_entries_.push_back({card, def.width, def.height, def.draw});
        if (def.draw) {
            region_draw_card_ = card;
            card->setObjectName("sourcePickerRegionDrawCard");
            connect(card, &ui::widgets::RegionPresetCard::clicked, this, [this]() { selectRegionDraw(); });
        } else {
            connect(card, &ui::widgets::RegionPresetCard::clicked, this,
                    [this, entry_index]() { selectRegionPreset(entry_index); });
        }
        region_grid->addWidget(card, i / kRegionColumns, i % kRegionColumns);
    }
    for (int col = 0; col < kRegionColumns; ++col) {
        region_grid->setColumnStretch(col, 1);
    }
    region_layout->addWidget(region_grid_host);

    auto* region_controls = new QFrame(region_content);
    region_controls->setObjectName("sourcePickerRegionControls");
    region_controls->setProperty("panelRole", "compactRow");
    auto* region_controls_layout = new QVBoxLayout(region_controls);
    region_controls_layout->setContentsMargins(12, 10, 12, 10);
    region_controls_layout->setSpacing(8);

    region_summary_value_label_ = new QLabel(QStringLiteral("No region saved yet."), region_controls);
    region_summary_value_label_->setObjectName("sourcePickerRegionSummary");
    region_summary_value_label_->setWordWrap(true);
    region_summary_value_label_->setProperty("labelRole", "captureTargetPickerNote");
    region_controls_layout->addWidget(region_summary_value_label_);

    auto* region_action_row = new QHBoxLayout();
    region_action_row->setContentsMargins(0, 0, 0, 0);
    region_action_row->setSpacing(10);
    region_select_on_record_check_ =
        new widgets::ExoCheckBox(QStringLiteral("Select region when recording starts"), region_controls);
    region_select_on_record_check_->setObjectName("sourcePickerRegionSelectOnRecord");
    region_select_on_record_check_->setChecked(true);
    pick_region_now_button_ = new QPushButton(QStringLiteral("Pick region now..."), region_controls);
    pick_region_now_button_->setObjectName("sourcePickerPickRegionButton");
    pick_region_now_button_->setProperty("role", "utility");
    region_action_row->addWidget(region_select_on_record_check_, 1);
    region_action_row->addWidget(pick_region_now_button_, 0, Qt::AlignRight);
    region_controls_layout->addLayout(region_action_row);

    region_layout->addWidget(region_controls);
    region_layout->addStretch(1);

    region_scroll->setWidget(region_content);
    region_page_layout->addWidget(region_scroll);
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
        // Default the region choice to the manual-draw card when nothing in the
        // Region tab has been chosen yet (keeps the source valid).
        if (region_choice_ == kRegionChoiceNone) {
            region_choice_ = kRegionChoiceDraw;
        }
        setActiveSection(Section::Region);
        refreshSelectionVisuals();
        updateSummaryLabel();
    });
    connect(region_select_on_record_check_, &QAbstractButton::toggled, this, [this]() { updateSummaryLabel(); });
    connect(refresh_button_, &QPushButton::clicked, this, &SourcePickerPanel::onRefreshRequested);
    connect(windows_unavailable_toggle_, &QPushButton::clicked, this, [this]() {
        show_unavailable_windows_ = windows_unavailable_toggle_ && windows_unavailable_toggle_->isChecked();
        rebuildOptionCardsForSection(Section::Windows);
        updateWindowsUnavailableToggle();
        refreshSelectionVisuals();
        if (isVisible() && selected_section_ == Section::Windows) {
            requestThumbnailsForSection(Section::Windows);
        }
    });
    connect(pick_region_now_button_, &QPushButton::clicked, this, &SourcePickerPanel::onPickRegionNow);
    connect(cancel_button, &QPushButton::clicked, this, [this]() { emit rejected(); });
    connect(use_button_, &QPushButton::clicked, this, &SourcePickerPanel::onUseSelected);

    screens_button_->setChecked(true);
    setActiveSection(Section::Screens);
    updateSummaryLabel();
}

void SourcePickerPanel::setScreenOptions(const std::vector<SourceOption>& options) {
    if (snapshotEqualsLast(Section::Screens, options)) {
        return;
    }
    screen_options_ = options;
    last_screen_snapshot_ = buildSnapshot(options);
    rebuildOptionCards();
    updateRegionPresetAvailability();
    if (isVisible() && selected_section_ == Section::Screens) {
        requestThumbnailsForSection(Section::Screens);
    }
}

void SourcePickerPanel::setWindowOptions(const std::vector<SourceOption>& options) {
    if (snapshotEqualsLast(Section::Windows, options)) {
        return;
    }
    window_options_ = options;
    last_window_snapshot_ = buildSnapshot(options);
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

void SourcePickerPanel::setRegionState(const QString& summary, bool has_region, bool select_on_record,
                                       const QRect& region_rect) {
    has_region_ = has_region;
    current_region_rect_ = has_region ? region_rect : QRect();
    region_summary_ = summary;
    region_select_on_record_check_->setChecked(select_on_record);
    if (region_summary_.trimmed().isEmpty()) {
        region_summary_value_label_->setText(QStringLiteral("No region saved yet."));
    } else {
        region_summary_value_label_->setText(region_summary_);
    }
    updateSummaryLabel();
}

void SourcePickerPanel::setCurrentSelection(Section section, int target_index) {
    pick_region_now_ = false;
    selected_section_ = section;
    selected_target_index_ = target_index;

    if (section == Section::Region && region_choice_ == kRegionChoiceNone) {
        // Default to the manual-draw card so the Region source stays valid.
        region_choice_ = kRegionChoiceDraw;
    }

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

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void SourcePickerPanel::applyVisualRegionPreset(int preset_w, int preset_h) {
    for (int i = 0; i < static_cast<int>(region_preset_entries_.size()); ++i) {
        const RegionPresetEntry& entry = region_preset_entries_[static_cast<std::size_t>(i)];
        if (!entry.draw && entry.width == preset_w && entry.height == preset_h) {
            selectRegionPreset(i);
            return;
        }
    }
}
#endif

bool SourcePickerPanel::selectSource(Section section, int target_index) {
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

SourcePickerPanel::SelectionResult SourcePickerPanel::selectionResult() const {
    SelectionResult result;
    result.section = selected_section_;
    result.target_index = selected_target_index_;
    result.valid = hasValidSelection();
    result.select_on_record = region_select_on_record_check_ ? region_select_on_record_check_->isChecked() : true;
    result.pick_region_now = pick_region_now_;

    if (selected_section_ == Section::Region && region_choice_ >= 0 && pending_region_rect_.isValid()) {
        result.region_preset = true;
        result.region_x = pending_region_rect_.x();
        result.region_y = pending_region_rect_.y();
        result.region_width = pending_region_rect_.width();
        result.region_height = pending_region_rect_.height();
        result.region_base_target_index = pending_region_base_index_;
        // An explicit preset rectangle is its own selection — never defer it to
        // an at-record overlay.
        result.select_on_record = false;
        result.pick_region_now = false;
    }
    return result;
}

void SourcePickerPanel::onUseSelected() {
    pick_region_now_ = false;
    if (!hasValidSelection()) {
        return;
    }
    emit accepted();
}

void SourcePickerPanel::onPickRegionNow() {
    selected_section_ = Section::Region;
    selected_target_index_ = -1;
    // "Pick region now" is the manual-draw-immediately action, so it overrides
    // any chosen preset rectangle.
    region_choice_ = kRegionChoiceDraw;
    pending_region_rect_ = {};
    pending_region_base_index_ = -1;
    pick_region_now_ = true;
    setActiveSection(Section::Region);
    refreshSelectionVisuals();
    updateSummaryLabel();
    emit accepted();
}

void SourcePickerPanel::onThumbnailReady(int target_index, int token, const QImage thumbnail) {
    if (!isVisible() || token != refresh_generation_)
        return;
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

void SourcePickerPanel::onThumbnailFailed(int target_index, int token) {
    if (!isVisible() || token != refresh_generation_)
        return;
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

void SourcePickerPanel::onRefreshRequested() {
    emit sourceDataRequested();
    if (selected_section_ == Section::Region) {
        return;
    }
    requestThumbnailsForSection(selected_section_);
}

void SourcePickerPanel::onPeriodicThumbnailRefresh() {
    if (!isVisible() || selected_section_ == Section::Region) {
        return;
    }
    requestThumbnailsForSection(selected_section_);
}

void SourcePickerPanel::rebuildOptionCards() {
    clearLayout(screens_grid_.grid);
    clearLayout(windows_grid_.grid);
    option_cards_.clear();

    rebuildOptionCardsForSection(Section::Screens);
    rebuildOptionCardsForSection(Section::Windows);
    updateWindowsUnavailableToggle();
    refreshSelectionVisuals();
    updateSummaryLabel();
}

void SourcePickerPanel::rebuildOptionCardsForSection(Section section) {
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

        QString subtitle = StripCaptureJargon(option.detail);

        // The "Primary" badge already marks the primary display, so do not also
        // append "· Primary" to the subtitle (avoids duplicate Primary markers).
        if (!option.minimum_detail.trimmed().isEmpty()) {
            const QString min_detail = StripCaptureJargon(option.minimum_detail);
            subtitle = subtitle.isEmpty() ? min_detail : (subtitle + QStringLiteral("\n") + min_detail);
        }
        if (subtitle.isEmpty()) {
            subtitle =
                section == Section::Screens ? QStringLiteral("Display capture") : QStringLiteral("Window capture");
        }
        card->setSubtitle(subtitle);

        // Keep the Primary marker on displays, but drop the generic "Screen"
        // badge — a badge on every display card is noise, not signal.
        QString status_badge = option.status_badge.trimmed();
        if (section == Section::Screens && status_badge.compare(QStringLiteral("Screen"), Qt::CaseInsensitive) == 0) {
            status_badge.clear();
        }
        card->setStatusText(status_badge);

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
                    // A single click on a screen/window source commits immediately and
                    // returns to the preview — no separate "Use selected source" step.
                    // Unavailable sources fail hasValidSelection() and stay put.
                    if (hasValidSelection()) {
                        emit accepted();
                    }
                });
    }

    relayoutSection(section);
}

void SourcePickerPanel::relayoutSection(Section section) {
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

void SourcePickerPanel::clearLayout(QLayout* layout) {
    if (!layout) {
        return;
    }

    QLayoutItem* item = nullptr;
    while ((item = layout->takeAt(0)) != nullptr) {
        if (auto* widget = item->widget()) {
            widget->setParent(nullptr);
            delete widget;
        }
        delete item;
    }
}

void SourcePickerPanel::setActiveSection(Section section) {
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
    // Screens/Windows commit on a single card click, so the explicit
    // "Use selected source" button is only needed for the Region section
    // (which is a multi-step config: pick a preset / draw / defer to record).
    if (use_button_) {
        use_button_->setVisible(section == Section::Region);
    }
    if (section != Section::Region && isVisible()) {
        requestThumbnailsForSection(section);
    }
}

void SourcePickerPanel::refreshSelectionVisuals() {
    for (auto& option_card : option_cards_) {
        if (!option_card.card) {
            continue;
        }
        const bool selected =
            option_card.section == selected_section_ && option_card.target_index == selected_target_index_;
        option_card.card->setSelected(selected);
    }

    const bool region_active = selected_section_ == Section::Region;
    for (int i = 0; i < static_cast<int>(region_preset_entries_.size()); ++i) {
        auto* card = region_preset_entries_[static_cast<std::size_t>(i)].card;
        if (!card) {
            continue;
        }
        const bool is_draw = region_preset_entries_[static_cast<std::size_t>(i)].draw;
        bool selected = false;
        if (region_active) {
            selected = is_draw ? (region_choice_ == kRegionChoiceDraw) : (region_choice_ == i);
        }
        card->setSelected(selected);
    }
    use_button_->setEnabled(hasValidSelection());
}

bool SourcePickerPanel::regionPresetsEnabled() const {
    SourceOption base;
    return findBaseDisplay(&base);
}

bool SourcePickerPanel::findBaseDisplay(SourceOption* out) const {
    const QPoint current_center = current_region_rect_.isValid()
                                      ? QPoint(current_region_rect_.x() + current_region_rect_.width() / 2,
                                               current_region_rect_.y() + current_region_rect_.height() / 2)
                                      : QPoint();
    const bool has_current_center = current_region_rect_.isValid();
    const SourceOption* preferred = nullptr;
    const SourceOption* current_region_display = nullptr;
    const SourceOption* primary = nullptr;
    const SourceOption* first = nullptr;
    for (const auto& option : screen_options_) {
        if (option.monitor_width <= 0 || option.monitor_height <= 0) {
            continue;
        }
        if (!first) {
            first = &option;
        }
        if (option.primary && !primary) {
            primary = &option;
        }
        if (option.target_index == selected_target_index_ && !preferred) {
            preferred = &option;
        }
        if (has_current_center && !current_region_display &&
            QRect(option.monitor_x, option.monitor_y, option.monitor_width, option.monitor_height)
                .contains(current_center)) {
            current_region_display = &option;
        }
    }
    const SourceOption* chosen =
        current_region_display ? current_region_display : (preferred ? preferred : (primary ? primary : first));
    if (!chosen) {
        return false;
    }
    if (out) {
        *out = *chosen;
    }
    return true;
}

void SourcePickerPanel::updateRegionPresetAvailability() {
    const bool enabled = regionPresetsEnabled();
    for (auto& entry : region_preset_entries_) {
        if (!entry.card || entry.draw) {
            continue;
        }
        entry.card->setPlanned(!enabled);
    }
    // If presets just became unavailable, drop any pending preset selection.
    if (!enabled && region_choice_ >= 0) {
        region_choice_ = kRegionChoiceDraw;
        pending_region_rect_ = {};
        pending_region_base_index_ = -1;
        refreshSelectionVisuals();
        if (selected_section_ == Section::Region) {
            updateSummaryLabel();
        }
    }
}

void SourcePickerPanel::selectRegionDraw() {
    selected_section_ = Section::Region;
    selected_target_index_ = -1;
    region_choice_ = kRegionChoiceDraw;
    pending_region_rect_ = {};
    pending_region_base_index_ = -1;
    pick_region_now_ = false;
    setActiveSection(Section::Region);
    refreshSelectionVisuals();
    updateSummaryLabel();
}

void SourcePickerPanel::selectRegionPreset(int entry_index) {
    if (entry_index < 0 || entry_index >= static_cast<int>(region_preset_entries_.size())) {
        return;
    }
    const RegionPresetEntry& entry = region_preset_entries_[static_cast<std::size_t>(entry_index)];
    if (entry.draw || entry.width <= 0 || entry.height <= 0) {
        return;
    }

    SourceOption base;
    if (!findBaseDisplay(&base)) {
        // No display geometry — cannot place a preset; fall back to draw.
        selectRegionDraw();
        return;
    }

    const QRect monitor(base.monitor_x, base.monitor_y, base.monitor_width, base.monitor_height);
    const QRect rect = ComputePresetRegionRect(entry.width, entry.height, monitor, current_region_rect_);
    if (!rect.isValid() || rect.width() < kRegionMinDimension || rect.height() < kRegionMinDimension) {
        return;
    }

    selected_section_ = Section::Region;
    selected_target_index_ = base.target_index;
    region_choice_ = entry_index;
    pending_region_rect_ = rect;
    pending_region_base_index_ = base.target_index;
    pick_region_now_ = false;
    setActiveSection(Section::Region);
    refreshSelectionVisuals();
    updateSummaryLabel();
}

void SourcePickerPanel::updateSummaryLabel() {
    if (!summary_label_) {
        return;
    }

    if (selected_section_ == Section::Region) {
        // A chosen preset reports its actual (post-scale) rectangle.
        if (region_choice_ >= 0 && pending_region_rect_.isValid()) {
            const QString actual =
                QStringLiteral("Region · %1 × %2").arg(pending_region_rect_.width()).arg(pending_region_rect_.height());
            summary_label_->setText(actual);
            if (region_summary_value_label_) {
                region_summary_value_label_->setText(QStringLiteral("%1, %2 — %3 × %4")
                                                         .arg(pending_region_rect_.x())
                                                         .arg(pending_region_rect_.y())
                                                         .arg(pending_region_rect_.width())
                                                         .arg(pending_region_rect_.height()));
            }
            return;
        }
        // Restore the saved-region text in the in-tab summary (a preset may have
        // overwritten it).
        if (region_summary_value_label_) {
            region_summary_value_label_->setText(
                region_summary_.trimmed().isEmpty() ? QStringLiteral("No region saved yet.") : region_summary_);
        }
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
    const QString detail = StripCaptureJargon(option.detail);
    if (!detail.isEmpty()) {
        summary += QStringLiteral(" · ") + detail;
    }
    summary_label_->setText(summary);
}

bool SourcePickerPanel::hasValidSelection() const {
    if (selected_section_ == Section::Region) {
        // A chosen preset is valid only once it resolves to a real rectangle;
        // the manual-draw choice stays valid (the overlay produces the rect).
        if (region_choice_ >= 0) {
            return pending_region_rect_.isValid() && pending_region_rect_.width() >= kRegionMinDimension &&
                   pending_region_rect_.height() >= kRegionMinDimension;
        }
        return true;
    }
    SourceOption option;
    return findOption(selected_section_, selected_target_index_, &option) && option.selectable;
}

bool SourcePickerPanel::hasTargetInSection(Section section, int target_index) const {
    const auto& options = section == Section::Windows ? window_options_ : screen_options_;
    for (const auto& option : options) {
        if (option.target_index == target_index) {
            return true;
        }
    }
    return false;
}

bool SourcePickerPanel::findOption(Section section, int target_index, SourceOption* out) const {
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

SourcePickerPanel::OptionCard* SourcePickerPanel::findOptionCard(Section section, int target_index) {
    for (auto& oc : option_cards_) {
        if (oc.section == section && oc.target_index == target_index) {
            return &oc;
        }
    }
    return nullptr;
}

bool SourcePickerPanel::shouldShowOption(const SourceOption& option, Section section) const {
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

void SourcePickerPanel::updateWindowsUnavailableToggle() {
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

void SourcePickerPanel::requestThumbnailsForSection(Section section) {
    if (!thumbnail_capture_ || section == Section::Region) {
        return;
    }

    thumbnail_capture_->cancelAll();
    const int token = ++refresh_generation_;

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
            thumbnail_capture_->requestMonitorThumbnail(option.target_index, option.native_id, kThumbnailSize, token);
        } else {
            thumbnail_capture_->requestWindowThumbnail(option.target_index, option.native_id, kThumbnailSize, token);
        }
    }
}

SourcePickerPanel::SectionGrid* SourcePickerPanel::sectionGrid(Section section) {
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

const SourcePickerPanel::SectionGrid* SourcePickerPanel::sectionGrid(Section section) const {
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

std::vector<SourcePickerPanel::OptionCard*> SourcePickerPanel::cardsForSection(Section section) {
    std::vector<OptionCard*> cards;
    cards.reserve(option_cards_.size());
    for (auto& card : option_cards_) {
        if (card.section == section) {
            cards.push_back(&card);
        }
    }
    return cards;
}

bool SourcePickerPanel::eventFilter(QObject* watched, QEvent* event) {
    if (event && event->type() == QEvent::Resize) {
        if (screens_grid_.scroll && watched == screens_grid_.scroll->viewport()) {
            relayoutSection(Section::Screens);
        } else if (windows_grid_.scroll && watched == windows_grid_.scroll->viewport()) {
            relayoutSection(Section::Windows);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SourcePickerPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (thumbnail_refresh_timer_) {
        thumbnail_refresh_timer_->start();
    }
    if (source_refresh_timer_) {
        source_refresh_timer_->start();
    }
    if (selected_section_ != Section::Region) {
        requestThumbnailsForSection(selected_section_);
    }
    emit sourceDataRequested();
}

void SourcePickerPanel::hideEvent(QHideEvent* event) {
    if (thumbnail_refresh_timer_) {
        thumbnail_refresh_timer_->stop();
    }
    if (source_refresh_timer_) {
        source_refresh_timer_->stop();
    }
    ++refresh_generation_;
    if (thumbnail_capture_) {
        thumbnail_capture_->cancelAll();
    }
    QWidget::hideEvent(event);
}

void SourcePickerPanel::onSourceRefreshTimer() {
    emit sourceDataRequested();
}

bool SourcePickerPanel::snapshotEqualsLast(Section section, const std::vector<SourceOption>& options) const {
    const std::vector<SourceCardSnapshot> current = buildSnapshot(options);
    const auto& last = section == Section::Screens ? last_screen_snapshot_ : last_window_snapshot_;
    if (current.size() != last.size())
        return false;
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (!(current[i] == last[i]))
            return false;
    }
    return true;
}

std::vector<SourceCardSnapshot> SourcePickerPanel::buildSnapshot(const std::vector<SourceOption>& options) {
    std::vector<SourceCardSnapshot> snapshots;
    snapshots.reserve(options.size());
    for (const auto& opt : options) {
        SourceCardSnapshot snap;
        snap.target_index = opt.target_index;
        snap.native_id = opt.native_id;
        snap.title = opt.title;
        snap.detail = opt.detail;
        snap.unavailable = opt.unavailable;
        snap.selectable = opt.selectable;
        snap.hidden_by_default = opt.hidden_by_default;
        snap.primary = opt.primary;
        snapshots.push_back(snap);
    }
    return snapshots;
}

bool operator==(const std::vector<SourceCardSnapshot>& a, const std::vector<SourceCardSnapshot>& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!(a[i] == b[i]))
            return false;
    }
    return true;
}

} // namespace exosnap::ui::dialogs

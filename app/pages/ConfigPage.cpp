#include "ConfigPage.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSize>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include "../../../libs/recorder_core/include/recorder_core/audio_track_model.h"
#include "../diagnostics/AppLog.h"
#include "../models/FilenameBuilder.h"
#include "../models/OutputPathPolicy.h"
#include "../models/RecordingPreset.h"
#include "../models/SettingsHintText.h"
#include "../services/GlobalHotkeyService.h"
#include "../services/WebcamService.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/ExoSnapTheme.h"
#include "../ui/theme/ExoSnapThemes.h"
#include "../ui/theme/LucideIcon.h"
#include "../ui/widgets/ComboBoxWheelFilter.h"
#include "../ui/widgets/CompareHint.h"
#include "../ui/widgets/ExoCheckBox.h"
#include "../ui/widgets/ExoToggle.h"
#include "../ui/widgets/HotkeysSettingsPanel.h"
#include "../ui/widgets/InfoHintIcon.h"
#include "../ui/widgets/PlaceholderRow.h"
#include "../ui/widgets/SettingsCardExpander.h"
#include "../ui/widgets/VUMeterWidget.h"
#include "../ui/widgets/WebcamSetupPanel.h"
#include "../viewmodels/PresentationStateBuilder.h"

#include <cmath>
#include <ctime>
#include <optional>

namespace exosnap {

namespace {

using M = ui::theme::ExoSnapMetrics;

// Upper bound for the Config form width. Settings is a wide product surface;
// the cap prevents absurd stretching on ultra-wide displays while preserving
// the full two-column desktop rhythm at typical window sizes.
constexpr int kMaxContentWidth = 1440;

// ---- Responsive layout threshold (D6 wave-2) ----
// Single-column layout kicks in below this width so both columns always have
// enough space when the two-column view is shown.  Must be larger than
// kMinWindowWidth so the app cannot be resized into a broken two-column state.
// 2×360 card min-width + 18 gap + two 24px outer margins ≈ 810; we use 1280 as
// a comfortable threshold that gives each card ~600px at the breakpoint.
constexpr int kColumnBreakThreshold = 1280;

// ---- Chip flow widget (D6 wave-2 responsive) ----
// A simple flow-wrap container: children are arranged left-to-right and wrapped
// to the next row when they would overflow the available width.  This replaces
// the fixed "4 chips per row" QHBoxLayout in the filename-token help section.
class ChipFlowWidget : public QWidget {
  public:
    explicit ChipFlowWidget(QWidget* parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    }

    void addChip(QWidget* chip) {
        chip->setParent(this);
        chips_.append(chip);
        updateGeometry();
    }

    QSize sizeHint() const override {
        return doLayout(rect(), /*apply=*/false);
    }
    QSize minimumSizeHint() const override {
        // Minimum: as narrow as the widest single chip.
        int max_w = 0;
        for (auto* c : chips_)
            max_w = qMax(max_w, c->sizeHint().width());
        int total_h = 0;
        for (auto* c : chips_)
            total_h += c->sizeHint().height();
        const int row_gap = 6;
        if (!chips_.isEmpty())
            total_h += row_gap * (chips_.size() - 1);
        return {max_w, total_h};
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        doLayout(rect(), /*apply=*/true);
    }

  private:
    // Lays out chips in rows.  If apply==false, only measures and returns the
    // needed size without moving widgets.
    QSize doLayout(const QRect& area, bool apply) const {
        const int h_gap = 6;
        const int v_gap = 6;
        int x = area.x();
        int y = area.y();
        int row_h = 0;

        for (auto* chip : chips_) {
            const QSize sh = chip->sizeHint();
            if (x + sh.width() > area.right() + 1 && x != area.x()) {
                // Wrap to next row.
                x = area.x();
                y += row_h + v_gap;
                row_h = 0;
            }
            if (apply)
                chip->setGeometry(QRect(QPoint(x, y), sh));
            x += sh.width() + h_gap;
            row_h = qMax(row_h, sh.height());
        }
        return {area.width(), y + row_h - area.y()};
    }

    QVector<QWidget*> chips_;
};

// ── ThemePreviewSwatch ────────────────────────────────────────────────────────
// Mini UI preview painted from an ExoTheme's colour tokens.
// Mirrors the ThemePreview component in themes.jsx (Slice 1B design).
// Layout (116×70):
//   title strip (h=16, surf bg): ac dot (6×6) · mut bar (22×3) · stretch · success dot (5×5)
//   body (padding=7, surf2 rows): two bars (h=7, surf2/line), ac pill + error pill
//   right column: ac square (10×10) + caution bar (22×6) + ac2 bar (22×6)
class ThemePreviewSwatch : public QWidget {
  public:
    explicit ThemePreviewSwatch(const exosnap::ui::theme::ExoTheme& theme, QWidget* parent = nullptr)
        : QWidget(parent), theme_(theme) {
        setFixedSize(116, 70);
        setAttribute(Qt::WA_NoSystemBackground, true);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // ExoTheme colour values may be "#rrggbb" or "rgba(r, g, b, alpha_float)".
        // Qt 6 QColor(QString) handles "#rrggbb" but not "rgba(...)" CSS format.
        // parseCssColor handles both.
        const auto bg = parseCssColor(theme_.bg);
        const auto surf = parseCssColor(theme_.surf);
        const auto surf2 = parseCssColor(theme_.surf2);
        const auto line = parseCssColor(theme_.line);
        const auto mut = parseCssColor(theme_.mut);
        const auto ac = parseCssColor(theme_.ac);
        const auto ac2 = parseCssColor(theme_.ac2);
        const auto success = parseCssColor(theme_.success);
        const auto err = parseCssColor(theme_.error);
        const auto caution = parseCssColor(theme_.caution);

        const int W = width();
        const int H = height();

        // ── Outer rounded rect (bg + border) ──
        QPainterPath outline;
        outline.addRoundedRect(QRectF(0, 0, W, H), 9, 9);
        p.fillPath(outline, bg);
        p.setPen(QPen(line, 1));
        p.drawPath(outline);
        p.setClipPath(outline);

        // ── Title strip (h=16, surf bg) ──
        p.fillRect(0, 0, W, 16, surf);
        p.setPen(QPen(line, 1));
        p.drawLine(0, 16, W, 16);

        // ac dot (6×6, r=3) at x=6
        p.setPen(Qt::NoPen);
        p.setBrush(ac);
        p.drawEllipse(QRectF(6, 5, 6, 6));

        // mut bar (22×3, r=2) at x=16
        auto mutFill = mut;
        mutFill.setAlphaF(0.6f);
        p.setBrush(mutFill);
        p.drawRoundedRect(QRectF(16, 6.5, 22, 3), 2, 2);

        // success dot (5×5, r=2.5) right-aligned at x=W-11
        p.setBrush(success);
        p.drawEllipse(QRectF(W - 11, 5.5, 5, 5));

        // ── Body (padding=7, below strip) ──
        const int bodyY = 16 + 7; // 23
        const int rightColW = 26;
        const int bodyW = W - 7 - 7;                // usable body width
        const int leftColW = bodyW - rightColW - 6; // gap=6 between left and right

        // Left column: two surf2 bars
        p.setBrush(surf2);
        p.setPen(QPen(line, 1));
        p.drawRoundedRect(QRectF(7, bodyY, leftColW, 7), 3, 3);
        p.drawRoundedRect(QRectF(7, bodyY + 12, leftColW, 7), 3, 3);

        // ac pill (22×9, r=5)
        p.setBrush(ac);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(7, bodyY + 24, 22, 9), 4.5, 4.5);

        // error pill (16×9, r=5)
        p.setBrush(err);
        p.drawRoundedRect(QRectF(7 + 26, bodyY + 24, 16, 9), 4.5, 4.5);

        // Right column
        const int rightX = W - 7 - rightColW;

        // ac square (10×10, r=3)
        p.setBrush(ac);
        p.drawRoundedRect(QRectF(rightX + 8, bodyY, 10, 10), 3, 3);

        // caution bar (22×6, r=3, 85% alpha)
        auto cautionFill = caution;
        cautionFill.setAlphaF(0.85f);
        p.setBrush(cautionFill);
        p.drawRoundedRect(QRectF(rightX, bodyY + 14, 22, 6), 3, 3);

        // ac2 bar (22×6, r=3, 70% alpha)
        auto ac2Fill = ac2;
        ac2Fill.setAlphaF(0.70f);
        p.setBrush(ac2Fill);
        p.drawRoundedRect(QRectF(rightX, bodyY + 24, 22, 6), 3, 3);
    }

  private:
    // Parses "#rrggbb" or "rgba(r, g, b, alpha_float)" CSS colour strings.
    // The ExoTheme struct uses rgba(...) for line/line2 tokens.
    static QColor parseCssColor(const char* css) {
        const QString s = QString::fromLatin1(css).trimmed();
        if (s.startsWith(QLatin1Char('#'))) {
            return QColor(s);
        }
        if (s.startsWith(QStringLiteral("rgba(")) && s.endsWith(QLatin1Char(')'))) {
            const QString inner = s.mid(5, s.size() - 6);
            const QStringList parts = inner.split(QLatin1Char(','));
            if (parts.size() == 4) {
                const int r = parts[0].trimmed().toInt();
                const int g = parts[1].trimmed().toInt();
                const int b = parts[2].trimmed().toInt();
                const qreal a = parts[3].trimmed().toDouble();
                return QColor(r, g, b, qRound(a * 255.0));
            }
        }
        // Fallback: Qt may handle "rgb(...)" etc.
        return QColor(s);
    }

    const exosnap::ui::theme::ExoTheme& theme_;
};

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

// Card title: 15/600 per the design system "Section/card title" role.
QLabel* makeCardTitle(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "cardTitle");
    return l;
}

// Mono uppercase "eyebrow" label that sits directly above a form control.
// Used outside the Settings Output card (e.g. Advanced / Developer section).
QLabel* makeFieldLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text.toUpper(), parent);
    l->setProperty("labelRole", "fieldLabel");
    return l;
}

// D6: Normal-case sub-section label for the Output card (no mono/uppercase).
// Matches the settingsRowLabel role used by makeSettingsRow left-side labels.
QLabel* makeOutputSubLabel(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "settingsRowLabel");
    return l;
}

// Thin in-card divider, matching the prototype `.hr` rule.
QFrame* makeHRule(QWidget* parent) {
    auto* rule = new QFrame(parent);
    rule->setFrameShape(QFrame::HLine);
    rule->setProperty("frameRole", "sectionRuleLine");
    return rule;
}

QLabel* makeHint(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("labelRole", "muted");
    l->setWordWrap(true);
    return l;
}

// D6: Creates a "quiet row": hairline on top (unless first=true), label left, control right.
// Returns the container QWidget* (parent is `parent`).
QWidget* makeSettingsRow(QWidget* parent, const QString& label, QWidget* hint_widget, const QString& sub_label,
                         QWidget* control, bool first = false) {
    auto* container = new QWidget(parent);
    auto* vl = new QVBoxLayout(container);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    if (!first) {
        auto* rule = new QFrame(container);
        rule->setFrameShape(QFrame::HLine);
        rule->setProperty("frameRole", "sectionRuleLine");
        vl->addWidget(rule);
    }

    auto* content = new QWidget(container);
    auto* hl = new QHBoxLayout(content);
    hl->setContentsMargins(0, 12, 0, 12);
    hl->setSpacing(14);

    // Left side: label block
    auto* left = new QWidget(content);
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(2);

    auto* label_row = new QWidget(left);
    auto* lrl = new QHBoxLayout(label_row);
    lrl->setContentsMargins(0, 0, 0, 0);
    lrl->setSpacing(4);

    auto* lbl = new QLabel(label, label_row);
    lbl->setProperty("labelRole", "settingsRowLabel");
    lrl->addWidget(lbl);

    if (hint_widget) {
        lrl->addWidget(hint_widget, 0, Qt::AlignVCenter);
    }
    lrl->addStretch();
    ll->addWidget(label_row);

    if (!sub_label.isEmpty()) {
        auto* sub = new QLabel(sub_label, left);
        sub->setProperty("labelRole", "muted");
        sub->setWordWrap(true);
        ll->addWidget(sub);
    }

    hl->addWidget(left, 1);

    // Right side: control
    if (control) {
        hl->addWidget(control, 0, Qt::AlignVCenter);
    }

    vl->addWidget(content);
    container->setProperty("settingsRow", true);
    return container;
}

// Build a QWidget containing a fieldLabel + an InfoHintIcon side-by-side.
// Use this wherever a plain makeFieldLabel would be placed; the result is
// reparented to parent and can be inserted into any layout.
QWidget* makeFieldLabelWithHint(const QString& text, const QString& hint_text, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* hl = new QHBoxLayout(row);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(4);
    auto* label = new QLabel(text.toUpper(), row);
    label->setProperty("labelRole", "fieldLabel");
    auto* hint = new ui::widgets::InfoHintIcon(hint_text, row);
    hl->addWidget(label);
    hl->addWidget(hint, 0, Qt::AlignVCenter);
    hl->addStretch();
    return row;
}

// D6: Normal-case sub-section label + InfoHintIcon for the Output card.
// Like makeFieldLabelWithHint but uses settingsRowLabel (no mono/uppercase).
QWidget* makeOutputSubLabelWithHint(const QString& text, const QString& hint_text, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* hl = new QHBoxLayout(row);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(4);
    auto* label = new QLabel(text, row);
    label->setProperty("labelRole", "settingsRowLabel");
    auto* hint = new ui::widgets::InfoHintIcon(hint_text, row);
    hl->addWidget(label);
    hl->addWidget(hint, 0, Qt::AlignVCenter);
    hl->addStretch();
    return row;
}

QString VideoCodecLabel(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::H264Nvenc:
        return QStringLiteral("H.264");
    case capability::VideoCodec::HevcNvenc:
        return QStringLiteral("HEVC");
    case capability::VideoCodec::Av1Nvenc:
        return QStringLiteral("AV1");
    }
    return QStringLiteral("H.264");
}

QString AudioCodecLabel(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::AacMf:
        return QStringLiteral("AAC");
    case capability::AudioCodec::Opus:
        return QStringLiteral("Opus");
    case capability::AudioCodec::Pcm:
        return QStringLiteral("PCM");
    case capability::AudioCodec::Flac:
        return QStringLiteral("FLAC");
    }
    return QStringLiteral("AAC");
}

QString ContainerLabel(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return QStringLiteral("MKV");
    case capability::Container::Mp4:
        return QStringLiteral("MP4");
    case capability::Container::WebM:
        return QStringLiteral("WebM");
    }
    return QStringLiteral("MKV");
}

QString ResolutionLabel(const OutputResolutionSettings& resolution) {
    if (resolution.mode == OutputResolutionMode::Custom && resolution.custom_width > 0 &&
        resolution.custom_height > 0) {
        return QStringLiteral("%1×%2").arg(resolution.custom_width).arg(resolution.custom_height);
    }
    return QString::fromWCharArray(OutputResolutionModeName(resolution.mode));
}

QString FrameRateLabel(uint32_t numerator, uint32_t denominator) {
    if (numerator == 0 || denominator == 0) {
        return QStringLiteral("60 fps");
    }
    if (denominator == 1) {
        return QStringLiteral("%1 fps").arg(numerator);
    }
    return QStringLiteral("%1/%2 fps").arg(numerator).arg(denominator);
}

int VideoCodecToInt(capability::VideoCodec codec) {
    return static_cast<int>(codec);
}

int AudioCodecToInt(capability::AudioCodec codec) {
    return static_cast<int>(codec);
}

capability::VideoCodec IntToVideoCodec(int value) {
    if (value == static_cast<int>(capability::VideoCodec::Av1Nvenc))
        return capability::VideoCodec::Av1Nvenc;
    if (value == static_cast<int>(capability::VideoCodec::HevcNvenc))
        return capability::VideoCodec::HevcNvenc;
    return capability::VideoCodec::H264Nvenc;
}

capability::AudioCodec IntToAudioCodec(int value) {
    if (value == static_cast<int>(capability::AudioCodec::Opus))
        return capability::AudioCodec::Opus;
    if (value == static_cast<int>(capability::AudioCodec::Pcm))
        return capability::AudioCodec::Pcm;
    if (value == static_cast<int>(capability::AudioCodec::Flac))
        return capability::AudioCodec::Flac;
    return capability::AudioCodec::AacMf;
}

FilenameTargetContext ExamplePreviewContext(const QString& profile_name, const OutputSettingsModel& settings) {
    FilenameTargetContext context;
    context.target_name = L"Desktop - Display 1";
    context.app_name = L"Desktop";
    context.window_title = L"Display 1";
    context.process_name = L"desktop";
    context.profile_name = profile_name.toStdWString();
    context.video_codec = settings.video_codec;
    context.audio_codec = settings.audio_codec;
    return context;
}

} // namespace

ConfigPage::ConfigPage(const OutputSettingsModel& initial_settings, const VideoSettingsModel& initial_video,
                       QWidget* parent)
    : QWidget(parent), format_settings_(initial_settings), video_settings_(initial_video) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    scroll_area_ = new QScrollArea(this);
    auto* scroll = scroll_area_;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(28, 24, 28, 36);
    layout->setSpacing(18);

    // ---- READINESS BANNER (full width) ----
    readiness_panel_ = makePanel(content);
    readiness_panel_->setProperty("panelRole", "readinessBanner");
    auto* status_layout = new QVBoxLayout(readiness_panel_);
    status_layout->setContentsMargins(18, 14, 18, 14);
    status_layout->setSpacing(6);

    auto* status_head = new QHBoxLayout();
    status_head->setSpacing(12);
    auto* status_text = new QVBoxLayout();
    status_text->setSpacing(2);

    readiness_badge_label_ = new QLabel(readiness_panel_);
    readiness_badge_label_->setProperty("labelRole", "cardTitle");
    status_text->addWidget(readiness_badge_label_);

    readiness_detail_label_ = new QLabel(readiness_panel_);
    readiness_detail_label_->setProperty("labelRole", "muted");
    readiness_detail_label_->setWordWrap(true);
    status_text->addWidget(readiness_detail_label_);

    status_head->addLayout(status_text, 1);

    view_details_btn_ = new QPushButton(QStringLiteral("Open Diagnostics..."), readiness_panel_);
    view_details_btn_->setProperty("role", "ghost");
    view_details_btn_->setVisible(false);
    status_head->addWidget(view_details_btn_, 0, Qt::AlignTop);
    status_layout->addLayout(status_head);

    lock_note_label_ = new QLabel(readiness_panel_);
    lock_note_label_->setObjectName(QStringLiteral("lockNoteLabel"));
    lock_note_label_->setProperty("labelRole", "muted");
    lock_note_label_->setWordWrap(true);
    lock_note_label_->setText(QStringLiteral("Recording settings are locked while recording."));
    lock_note_label_->setVisible(false);
    status_layout->addWidget(lock_note_label_);

    layout->addWidget(readiness_panel_);

    // ---- SLIM TOOLBAR (Settings 1B redesign) ----
    // One row: [Preset] quiet label · combo · Save (dirty-gated) · Save As… · Manage · dirty hint
    //          · stretch · (expert-only) search pill · Expert mode label + toggle
    // Below: match-count / expert-hint / expert-warn labels
    // No page title — the active nav tab already reads "Settings".
    {
        auto* header_zone = new QWidget(content);
        header_zone->setObjectName(QStringLiteral("settingsHeaderZone"));
        auto* header_vl = new QVBoxLayout(header_zone);
        header_vl->setContentsMargins(0, 0, 0, 0);
        header_vl->setSpacing(8);

        // ---- Slim toolbar row ----
        // preset_panel_ points at the toolbar so applySettingsSearch can hide it
        // when the query does not match "preset" keywords.
        auto* toolbar_row = new QWidget(header_zone);
        toolbar_row->setObjectName(QStringLiteral("settingsSlimToolbar"));
        toolbar_row->setProperty("role", "settingsToolbar");
        // preset_panel_ intentionally left nullptr: the slim toolbar is always visible
        // (it carries the Expert toggle/search pill that must not be hidden during search).
        // applySettingsSearch null-checks preset_panel_ and is a no-op when it is nullptr.

        auto* toolbar_hl = new QHBoxLayout(toolbar_row);
        toolbar_hl->setContentsMargins(14, 8, 10, 8);
        toolbar_hl->setSpacing(8);

        // "Preset" quiet label (visible text "Preset" kept for test assertions)
        auto* preset_label = new QLabel(QStringLiteral("Preset"), toolbar_row);
        preset_label->setProperty("labelRole", "presetToolbarLabel");
        toolbar_hl->addWidget(preset_label, 0, Qt::AlignVCenter);

        // Combo (stable objectNames preserved for tests)
        profile_combo_ = new QComboBox(toolbar_row);
        profile_combo_->setObjectName(QStringLiteral("profileCombo"));
        profile_combo_->setAccessibleName(QStringLiteral("presetCombo"));
        profile_combo_->setProperty("presetComboAlias", QStringLiteral("presetCombo"));
        profile_combo_->setMinimumWidth(0);
        profile_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        profile_combo_->setMaximumWidth(260);
        toolbar_hl->addWidget(profile_combo_, 1, Qt::AlignVCenter);

        // Default badge (inline, right of combo)
        preset_default_badge_ = new QLabel(toolbar_row);
        preset_default_badge_->setObjectName(QStringLiteral("presetDefaultBadge"));
        preset_default_badge_->setProperty("labelRole", "profileStatusBadge");
        preset_default_badge_->setAlignment(Qt::AlignCenter);
        preset_default_badge_->setText(QStringLiteral("Default"));
        preset_default_badge_->setVisible(false);
        toolbar_hl->addWidget(preset_default_badge_, 0, Qt::AlignVCenter);

        profile_status_label_ = new QLabel(toolbar_row);
        profile_status_label_->setProperty("labelRole", "profileStatusBadge");
        profile_status_label_->setAlignment(Qt::AlignCenter);
        profile_status_label_->setVisible(false);
        toolbar_hl->addWidget(profile_status_label_, 0, Qt::AlignVCenter);

        // Save button (dirty-gated)
        preset_save_btn_ = new QPushButton(QStringLiteral("Save"), toolbar_row);
        preset_save_btn_->setObjectName(QStringLiteral("presetSaveButton"));
        preset_save_btn_->setProperty("role", "ghost");
        preset_save_btn_->setEnabled(false);
        preset_save_btn_->setVisible(false);
        toolbar_hl->addWidget(preset_save_btn_, 0, Qt::AlignVCenter);

        // Save As... button
        preset_save_as_btn_ = new QPushButton(QStringLiteral("Save As\xe2\x80\xa6"), toolbar_row);
        preset_save_as_btn_->setObjectName(QStringLiteral("presetSaveAsButton"));
        preset_save_as_btn_->setProperty("role", "ghost");
        toolbar_hl->addWidget(preset_save_as_btn_, 0, Qt::AlignVCenter);

        // Manage overflow button
        profile_overflow_btn_ = new QToolButton(toolbar_row);
        profile_overflow_btn_->setObjectName(QStringLiteral("presetManageButton"));
        profile_overflow_btn_->setText(QStringLiteral("Manage presets"));
        profile_overflow_btn_->setPopupMode(QToolButton::InstantPopup);
        profile_overflow_btn_->setToolButtonStyle(Qt::ToolButtonTextOnly);

        auto* profile_menu = new QMenu(profile_overflow_btn_);
        // Section 1: Save actions.
        save_preset_action_ = profile_menu->addAction(QStringLiteral("Save preset"));
        save_preset_as_action_ = profile_menu->addAction(QStringLiteral("Save as new preset\xe2\x80\xa6"));
        profile_menu->addSeparator();
        // Section 2: Preset lifecycle.
        new_preset_action_ = profile_menu->addAction(QStringLiteral("New preset from default\xe2\x80\xa6"));
        duplicate_preset_action_ = profile_menu->addAction(QStringLiteral("Duplicate preset"));
        rename_preset_action_ = profile_menu->addAction(QStringLiteral("Rename preset\xe2\x80\xa6"));
        delete_preset_action_ = profile_menu->addAction(QStringLiteral("Delete preset"));
        profile_menu->addSeparator();
        // Section 3: Default assignment.
        set_default_preset_action_ = profile_menu->addAction(QStringLiteral("Set as default preset"));
        profile_menu->addSeparator();
        // Section 4: Reset -- two CLEARLY SEPARATE actions.
        reset_changes_action_ = profile_menu->addAction(QStringLiteral("Reset changes"));
        profile_menu->addSeparator();
        // Destructive reset is separated so it cannot be confused with "Reset changes".
        reset_to_defaults_action_ =
            profile_menu->addAction(QStringLiteral("Reset all presets to factory defaults\xe2\x80\xa6"));
        profile_menu->addSeparator();
        // Section 5: Manage overlay.
        manage_presets_action_ = profile_menu->addAction(QStringLiteral("Manage presets\xe2\x80\xa6"));
        profile_overflow_btn_->setMenu(profile_menu);
        toolbar_hl->addWidget(profile_overflow_btn_, 0, Qt::AlignVCenter);

        // Dirty hint (inline, shown when dirty)
        preset_dirty_indicator_ = new QLabel(toolbar_row);
        preset_dirty_indicator_->setObjectName(QStringLiteral("presetDirtyIndicator"));
        preset_dirty_indicator_->setProperty("labelRole", "presetDirtyIndicator");
        preset_dirty_indicator_->setText(QStringLiteral("\xc2\xb7 Unsaved"));
        preset_dirty_indicator_->setVisible(false);
        toolbar_hl->addWidget(preset_dirty_indicator_, 0, Qt::AlignVCenter);

        // Stretch pushes expert controls to the right
        toolbar_hl->addStretch(1);

        // Expert-only search pill (hidden until expert mode enabled)
        auto* search_pill = new QWidget(toolbar_row);
        settings_search_pill_ = search_pill;
        search_pill->setObjectName(QStringLiteral("settingsSearchPill"));
        search_pill->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        search_pill->setFixedHeight(34);
        search_pill->setVisible(false);
        auto* pill_hl = new QHBoxLayout(search_pill);
        pill_hl->setContentsMargins(10, 6, 10, 6);
        pill_hl->setSpacing(6);

        auto* search_icon = new QLabel(search_pill);
        search_icon->setFixedSize(15, 15);
        search_icon->setScaledContents(true);
        search_icon->setPixmap(ui::theme::lucidePixmap(QStringLiteral("search"),
                                                       QString::fromUtf8(ui::theme::ActiveTheme().dim), 15,
                                                       search_icon->devicePixelRatioF()));
        pill_hl->addWidget(search_icon);

        settings_search_box_ = new QLineEdit(search_pill);
        settings_search_box_->setObjectName(QStringLiteral("settingsSearchBox"));
        settings_search_box_->setPlaceholderText(QStringLiteral("Search settings\xe2\x80\xa6"));
        settings_search_box_->setClearButtonEnabled(true);
        settings_search_box_->setFrame(false);
        settings_search_box_->setProperty("role", "pillInput");
        settings_search_box_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        pill_hl->addWidget(settings_search_box_, 1);
        toolbar_hl->addWidget(search_pill, 0, Qt::AlignVCenter);

        // "Expert mode" label + toggle
        auto* expert_label = new QLabel(QStringLiteral("Expert mode"), toolbar_row);
        expert_label->setProperty("labelRole", "muted");
        toolbar_hl->addWidget(expert_label, 0, Qt::AlignVCenter);

        expert_mode_toggle_ = new ui::widgets::ExoToggle(toolbar_row);
        expert_mode_toggle_->setObjectName(QStringLiteral("expertModeToggleBtn"));
        expert_mode_toggle_->setOn(false);
        toolbar_hl->addWidget(expert_mode_toggle_, 0, Qt::AlignVCenter);

        header_vl->addWidget(toolbar_row);

        // Sub-row: match-count, expert-hint, expert-warn
        settings_search_count_label_ = new QLabel(header_zone);
        settings_search_count_label_->setObjectName(QStringLiteral("settingsSearchCountLabel"));
        settings_search_count_label_->setProperty("labelRole", "muted");
        settings_search_count_label_->setVisible(false);
        header_vl->addWidget(settings_search_count_label_);

        search_expert_hint_label_ = new QLabel(header_zone);
        search_expert_hint_label_->setObjectName(QStringLiteral("searchExpertHintLabel"));
        search_expert_hint_label_->setProperty("labelRole", "muted");
        search_expert_hint_label_->setWordWrap(true);
        search_expert_hint_label_->setVisible(false);
        header_vl->addWidget(search_expert_hint_label_);

        expert_warn_label_ = new QLabel(header_zone);
        expert_warn_label_->setObjectName(QStringLiteral("expertWarnLabel"));
        expert_warn_label_->setText(QStringLiteral("Expert mode reveals lower-level controls that can produce "
                                                   "incompatible files. Enable only if you know why."));
        expert_warn_label_->setProperty("labelRole", "warnHint");
        expert_warn_label_->setWordWrap(true);
        expert_warn_label_->setVisible(false);
        header_vl->addWidget(expert_warn_label_);

        layout->addWidget(header_zone);
    }
    // ---- TWO-COLUMN CARD GRID (D6 design: Format | Audio / Webcam | Output / Presence | Appearance) ----
    // Left column: Format & encoding, Webcam, Presence.
    // Right column: Audio, Output, Appearance.
    // On narrow viewports updateResponsiveLayout() flips both columns to a single stacked column.
    // Updates card is placed full-width below the grid; Developer is full-width below that
    // (expert-gated). The six main cards are individually visibility-controlled by
    // applySettingsSearch so the grid host (columns_widget_) is only hidden when all six are
    // hidden (cosmetic gap limitation is acceptable — see applySettingsSearch).
    auto* columns = new QWidget(content);
    columns_widget_ = columns;
    columns_layout_ = new QHBoxLayout(columns);
    columns_layout_->setContentsMargins(0, 0, 0, 0);
    columns_layout_->setSpacing(18);

    auto* left_col = new QWidget(columns);
    auto* left_layout = new QVBoxLayout(left_col);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(18);

    auto* right_col = new QWidget(columns);
    auto* right_layout = new QVBoxLayout(right_col);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(18);

    columns_layout_->addWidget(left_col, 1);
    columns_layout_->addWidget(right_col, 1);
    layout->addWidget(columns);

    // ---- FORMAT & ENCODING CARD (left) — D6: flat SRows ----
    auto* fmt_panel = makePanel(left_col);
    fmt_panel_ = fmt_panel;
    auto* fmt_layout = new QVBoxLayout(fmt_panel);
    fmt_layout->setContentsMargins(18, 16, 18, 18);
    fmt_layout->setSpacing(0);
    fmt_layout->addWidget(makeCardTitle(QStringLiteral("Format & encoding"), fmt_panel));

    // format_display_label_ kept for backward compat (hidden)
    format_display_label_ = new QLabel(fmt_panel);
    format_display_label_->setProperty("labelRole", "muted");
    format_display_label_->setVisible(false);
    fmt_layout->addWidget(format_display_label_);

    // --- Container row ---
    container_group_ = new QButtonGroup(this);
    container_group_->setExclusive(true);
    auto* container_segmented = new QWidget(fmt_panel);
    container_segmented->setObjectName(QStringLiteral("containerSegmented"));
    auto* container_row_layout = new QHBoxLayout();
    container_row_layout->setContentsMargins(3, 3, 3, 3);
    container_row_layout->setSpacing(0);
    container_segmented->setLayout(container_row_layout);
    auto makeContainerSegment = [&](const QString& object_name, const QString& label,
                                    capability::Container container) -> QPushButton* {
        auto* segment = new QPushButton(label, container_segmented);
        segment->setObjectName(object_name);
        segment->setAccessibleName(label);
        segment->setCheckable(true);
        segment->setAutoDefault(false);
        segment->setDefault(false);
        segment->setCursor(Qt::PointingHandCursor);
        segment->setProperty("qualitySegment", true);
        segment->setProperty("qualitySegmentSelected", false);
        segment->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        container_group_->addButton(segment, static_cast<int>(container));
        container_row_layout->addWidget(segment);
        return segment;
    };
    mkv_radio_ = makeContainerSegment(QStringLiteral("containerMkvButton"), QStringLiteral("MKV"),
                                      capability::Container::Matroska);
    webm_radio_ = makeContainerSegment(QStringLiteral("containerWebmButton"), QStringLiteral("WebM"),
                                       capability::Container::WebM);
    mp4_radio_ =
        makeContainerSegment(QStringLiteral("containerMp4Button"), QStringLiteral("MP4"), capability::Container::Mp4);

    container_compare_hint_ =
        new ui::widgets::CompareHint(QStringLiteral("container"), QStringLiteral("MKV"), fmt_panel);
    fmt_layout->addWidget(makeSettingsRow(fmt_panel, QStringLiteral("Container"), container_compare_hint_, QString(),
                                          container_segmented, /*first=*/true));

    // --- Video codec row ---
    video_codec_combo_ = new QComboBox(fmt_panel);
    video_codec_compare_hint_ =
        new ui::widgets::CompareHint(QStringLiteral("videoCodec"), QStringLiteral("AV1"), fmt_panel);
    fmt_layout->addWidget(makeSettingsRow(fmt_panel, QStringLiteral("Video codec"), video_codec_compare_hint_,
                                          QString(), video_codec_combo_));

    // --- Audio codec row ---
    audio_codec_combo_ = new QComboBox(fmt_panel);
    audio_codec_compare_hint_ =
        new ui::widgets::CompareHint(QStringLiteral("audioCodec"), QStringLiteral("Opus"), fmt_panel);
    fmt_layout->addWidget(makeSettingsRow(fmt_panel, QStringLiteral("Audio codec"), audio_codec_compare_hint_,
                                          QString(), audio_codec_combo_));

    // --- Quality row ---
    // Hidden combo is the single model-change emitter (existing test seam).
    quality_combo_ = new QComboBox(fmt_panel);
    quality_combo_->setObjectName(QStringLiteral("videoQualityCombo"));
    quality_combo_->addItem(QStringLiteral("High Quality"), static_cast<int>(recorder_core::NvencQualityPreset::High));
    quality_combo_->addItem(QStringLiteral("Balanced"), static_cast<int>(recorder_core::NvencQualityPreset::Balanced));
    quality_combo_->addItem(QStringLiteral("Small"), static_cast<int>(recorder_core::NvencQualityPreset::Small));
    quality_combo_->setVisible(false);
    quality_combo_->setFocusPolicy(Qt::NoFocus);
    fmt_layout->addWidget(quality_combo_);

    auto* quality_segmented = new QWidget(fmt_panel);
    quality_segmented->setObjectName(QStringLiteral("qualitySegmented"));
    auto* quality_segmented_layout = new QHBoxLayout(quality_segmented);
    quality_segmented_layout->setContentsMargins(3, 3, 3, 3);
    quality_segmented_layout->setSpacing(0);

    quality_segment_group_ = new QButtonGroup(this);
    quality_segment_group_->setExclusive(true);

    auto makeQualitySegment = [&](const QString& object_name, const QString& label,
                                  recorder_core::NvencQualityPreset preset) -> QPushButton* {
        auto* segment = new QPushButton(label, quality_segmented);
        segment->setObjectName(object_name);
        segment->setCheckable(true);
        segment->setAutoDefault(false);
        segment->setDefault(false);
        segment->setCursor(Qt::PointingHandCursor);
        segment->setProperty("qualitySegment", true);
        segment->setProperty("qualitySegmentSelected", false);
        segment->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        quality_segment_group_->addButton(segment, static_cast<int>(preset));
        quality_segmented_layout->addWidget(segment);
        return segment;
    };

    quality_segment_small_ =
        makeQualitySegment(QStringLiteral("qualitySegmentSmall"), QStringLiteral("Small \xC2\xB7 CQ30"),
                           recorder_core::NvencQualityPreset::Small);
    quality_segment_balanced_ =
        makeQualitySegment(QStringLiteral("qualitySegmentBalanced"), QStringLiteral("Balanced \xC2\xB7 CQ24"),
                           recorder_core::NvencQualityPreset::Balanced);
    quality_segment_high_ =
        makeQualitySegment(QStringLiteral("qualitySegmentHigh"), QStringLiteral("High \xC2\xB7 CQ19"),
                           recorder_core::NvencQualityPreset::High);

    quality_badge_label_ = new QLabel(fmt_panel);
    quality_badge_label_->setObjectName(QStringLiteral("qualityBadgeLabel"));
    quality_badge_label_->setProperty("labelRole", "muted");

    quality_settings_label_ = new QLabel(fmt_panel);
    quality_settings_label_->setObjectName(QStringLiteral("qualitySettingsLabel"));
    quality_settings_label_->setProperty("labelRole", "muted");

    // Quality sub-label widget combining both badge+settings labels
    auto* quality_sub_widget = new QWidget(fmt_panel);
    {
        auto* qsl = new QVBoxLayout(quality_sub_widget);
        qsl->setContentsMargins(0, 0, 0, 0);
        qsl->setSpacing(2);
        qsl->addWidget(quality_badge_label_);
        qsl->addWidget(quality_settings_label_);
    }

    quality_compare_hint_ =
        new ui::widgets::CompareHint(QStringLiteral("quality"), QStringLiteral("Balanced"), fmt_panel);

    // Quality row: segmented on right, compare hint in label area
    // Wave 2: quality_row_widget_ promoted to member so expert-mode can show/hide it.
    {
        quality_row_widget_ = new QWidget(fmt_panel);
        auto* qvl = new QVBoxLayout(quality_row_widget_);
        qvl->setContentsMargins(0, 0, 0, 0);
        qvl->setSpacing(0);
        // hairline
        auto* qrule = new QFrame(quality_row_widget_);
        qrule->setFrameShape(QFrame::HLine);
        qrule->setProperty("frameRole", "sectionRuleLine");
        qvl->addWidget(qrule);
        // content
        auto* qcontent = new QWidget(quality_row_widget_);
        auto* qhl = new QHBoxLayout(qcontent);
        qhl->setContentsMargins(0, 12, 0, 12);
        qhl->setSpacing(14);
        // left: label + hint + sub labels
        auto* qleft = new QWidget(qcontent);
        auto* qll = new QVBoxLayout(qleft);
        qll->setContentsMargins(0, 0, 0, 0);
        qll->setSpacing(2);
        auto* qlabel_row = new QWidget(qleft);
        auto* qlrl = new QHBoxLayout(qlabel_row);
        qlrl->setContentsMargins(0, 0, 0, 0);
        qlrl->setSpacing(4);
        auto* qlbl = new QLabel(QStringLiteral("Quality"), qlabel_row);
        qlbl->setProperty("labelRole", "settingsRowLabel");
        qlrl->addWidget(qlbl);
        qlrl->addWidget(quality_compare_hint_, 0, Qt::AlignVCenter);
        qlrl->addStretch();
        qll->addWidget(qlabel_row);
        qll->addWidget(quality_sub_widget);
        qhl->addWidget(qleft, 1);
        qhl->addWidget(quality_segmented, 0, Qt::AlignVCenter);
        qvl->addWidget(qcontent);
        quality_row_widget_->setProperty("settingsRow", true);
        fmt_layout->addWidget(quality_row_widget_);
    }

    // Wave 2 Part B: CQ precision spinbox row — shown in expert mode, hidden otherwise.
    {
        quality_expert_widget_ = new QWidget(fmt_panel);
        quality_expert_widget_->setObjectName(QStringLiteral("qualityExpertWidget"));
        auto* qevl = new QVBoxLayout(quality_expert_widget_);
        qevl->setContentsMargins(0, 0, 0, 0);
        qevl->setSpacing(0);
        // hairline
        auto* qerule = new QFrame(quality_expert_widget_);
        qerule->setFrameShape(QFrame::HLine);
        qerule->setProperty("frameRole", "sectionRuleLine");
        qevl->addWidget(qerule);
        // content row
        auto* qecontent = new QWidget(quality_expert_widget_);
        auto* qehl = new QHBoxLayout(qecontent);
        qehl->setContentsMargins(0, 12, 0, 12);
        qehl->setSpacing(14);
        auto* qelbl = new QLabel(QStringLiteral("Quality (CQ)"), qecontent);
        qelbl->setProperty("labelRole", "settingsRowLabel");
        qehl->addWidget(qelbl, 1);
        quality_cq_spin_ = new QSpinBox(qecontent);
        quality_cq_spin_->setObjectName(QStringLiteral("qualityCqSpin"));
        quality_cq_spin_->setRange(1, 51);
        quality_cq_spin_->setSuffix(QStringLiteral(" (CQ)"));
        quality_cq_spin_->setToolTip(QStringLiteral("NVENC Constant Quality value (1=best, 51=worst). "
                                                    "Low=19 (High), 24 (Balanced), 30 (Small)."));
        qehl->addWidget(quality_cq_spin_, 0, Qt::AlignVCenter);
        qevl->addWidget(qecontent);
        quality_expert_widget_->setProperty("settingsRow", true);
        quality_expert_widget_->setVisible(false); // hidden until expert mode is on
        fmt_layout->addWidget(quality_expert_widget_);
    }

    // --- Frame rate row ---
    frame_rate_combo_ = new QComboBox(fmt_panel);
    frame_rate_combo_->setObjectName(QStringLiteral("frameRateCombo"));
    frame_rate_combo_->setAccessibleName(QStringLiteral("Frame rate"));
    for (const int fps : {24, 25, 30, 50, 60}) {
        frame_rate_combo_->addItem(QStringLiteral("%1 fps").arg(fps), fps);
    }
    frame_rate_combo_->addItem(QStringLiteral("120 fps (unavailable)"), 120);
    if (auto* model = qobject_cast<QStandardItemModel*>(frame_rate_combo_->model())) {
        if (auto* item = model->item(frame_rate_combo_->count() - 1)) {
            item->setEnabled(false);
            item->setToolTip(QStringLiteral("120 fps is hidden from runtime use until hardware support is proven."));
        }
    }

    fmt_layout->addWidget(makeSettingsRow(fmt_panel, QStringLiteral("Frame rate"),
                                          new ui::widgets::InfoHintIcon(ui::hints::kFrameRate, fmt_panel), QString(),
                                          frame_rate_combo_));

    // --- Frame timing row ---
    auto* timing_segmented = new QWidget(fmt_panel);
    timing_segmented->setObjectName(QStringLiteral("timingSegmented"));
    auto* timing_segmented_layout = new QHBoxLayout(timing_segmented);
    timing_segmented_layout->setContentsMargins(3, 3, 3, 3);
    timing_segmented_layout->setSpacing(0);
    timing_group_ = new QButtonGroup(this);
    timing_group_->setExclusive(true);
    auto makeTimingSegment = [&](const QString& object_name, const QString& label, int id) -> QPushButton* {
        auto* segment = new QPushButton(label, timing_segmented);
        segment->setObjectName(object_name);
        segment->setAccessibleName(label);
        segment->setCheckable(true);
        segment->setAutoDefault(false);
        segment->setDefault(false);
        segment->setCursor(Qt::PointingHandCursor);
        segment->setProperty("qualitySegment", true);
        segment->setProperty("qualitySegmentSelected", false);
        segment->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        timing_group_->addButton(segment, id);
        timing_segmented_layout->addWidget(segment);
        return segment;
    };
    timing_cfr_btn_ = makeTimingSegment(QStringLiteral("timingCfrButton"), QStringLiteral("CFR"), 1);
    timing_vfr_btn_ = makeTimingSegment(QStringLiteral("timingVfrButton"), QStringLiteral("VFR"), 0);

    timing_compare_hint_ = new ui::widgets::CompareHint(QStringLiteral("timing"), QStringLiteral("CFR"), fmt_panel);
    fmt_layout->addWidget(
        makeSettingsRow(fmt_panel, QStringLiteral("Frame timing"), timing_compare_hint_, QString(), timing_segmented));

    // --- Capture cursor row ---
    cursor_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("Capture cursor"), fmt_panel);
    cursor_check_->setObjectName(QStringLiteral("captureCursorCheck"));
    cursor_check_->setChecked(video_settings_.capture_cursor);
    fmt_layout->addWidget(makeSettingsRow(fmt_panel, QStringLiteral("Capture cursor"),
                                          new ui::widgets::InfoHintIcon(ui::hints::kCaptureCursor, fmt_panel),
                                          QString(), cursor_check_));

    // --- PS-PHASE-C: Expert Format section — rate control, bitrate, v1.0 placeholders ---
    // Shown only when expert_mode_enabled_ == true. rate_control is the active row;
    // quality_expert_widget_ (existing CQ spinbox) stays visible when rate=CQ and hidden
    // when rate=VBR/CBR (reusing existing logic). bitrate_row is shown for VBR/CBR.
    {
        fmt_expert_section_ = new QWidget(fmt_panel);
        fmt_expert_section_->setObjectName(QStringLiteral("fmtExpertSection"));
        fmt_expert_section_->setVisible(false); // hidden until expert mode on
        auto* fes_layout = new QVBoxLayout(fmt_expert_section_);
        fes_layout->setContentsMargins(0, 0, 0, 0);
        fes_layout->setSpacing(0);

        // --- Rate control segmented (CQ / VBR / CBR) ---
        auto* rc_segmented = new QWidget(fmt_expert_section_);
        rc_segmented->setObjectName(QStringLiteral("rateControlSegmented"));
        auto* rc_segmented_layout = new QHBoxLayout(rc_segmented);
        rc_segmented_layout->setContentsMargins(3, 3, 3, 3);
        rc_segmented_layout->setSpacing(0);

        rate_control_group_ = new QButtonGroup(this);
        rate_control_group_->setExclusive(true);

        auto makeRcSegment = [&](const QString& object_name, const QString& label,
                                 recorder_core::RateControlMode mode) -> QPushButton* {
            auto* seg = new QPushButton(label, rc_segmented);
            seg->setObjectName(object_name);
            seg->setCheckable(true);
            seg->setAutoDefault(false);
            seg->setDefault(false);
            seg->setCursor(Qt::PointingHandCursor);
            seg->setProperty("qualitySegment", true);
            seg->setProperty("qualitySegmentSelected", false);
            seg->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            rate_control_group_->addButton(seg, static_cast<int>(mode));
            rc_segmented_layout->addWidget(seg);
            return seg;
        };
        makeRcSegment(QStringLiteral("rateControlCqButton"), QStringLiteral("CQ"),
                      recorder_core::RateControlMode::ConstantQuality);
        makeRcSegment(QStringLiteral("rateControlVbrButton"), QStringLiteral("VBR"),
                      recorder_core::RateControlMode::VariableBitrate);
        makeRcSegment(QStringLiteral("rateControlCbrButton"), QStringLiteral("CBR"),
                      recorder_core::RateControlMode::ConstantBitrate);

        rate_control_row_widget_ = new QWidget(fmt_expert_section_);
        {
            auto* rvl = new QVBoxLayout(rate_control_row_widget_);
            rvl->setContentsMargins(0, 0, 0, 0);
            rvl->setSpacing(0);
            auto* rrule = new QFrame(rate_control_row_widget_);
            rrule->setFrameShape(QFrame::HLine);
            rrule->setProperty("frameRole", "sectionRuleLine");
            rvl->addWidget(rrule);
            auto* rhl = new QHBoxLayout();
            rhl->setContentsMargins(0, 12, 0, 12);
            rhl->setSpacing(14);
            auto* rlbl = new QLabel(QStringLiteral("Rate control"), rate_control_row_widget_);
            rlbl->setProperty("labelRole", "settingsRowLabel");
            rhl->addWidget(rlbl, 1);
            rhl->addWidget(rc_segmented, 0, Qt::AlignVCenter);
            rvl->addLayout(rhl);
            rate_control_row_widget_->setProperty("settingsRow", true);
        }
        fes_layout->addWidget(rate_control_row_widget_);

        // --- Bitrate spinbox (VBR / CBR only) ---
        bitrate_row_widget_ = new QWidget(fmt_expert_section_);
        bitrate_row_widget_->setObjectName(QStringLiteral("bitrateRowWidget"));
        bitrate_row_widget_->setVisible(false);
        {
            auto* bvl = new QVBoxLayout(bitrate_row_widget_);
            bvl->setContentsMargins(0, 0, 0, 0);
            bvl->setSpacing(0);
            auto* brule = new QFrame(bitrate_row_widget_);
            brule->setFrameShape(QFrame::HLine);
            brule->setProperty("frameRole", "sectionRuleLine");
            bvl->addWidget(brule);
            auto* bhl = new QHBoxLayout();
            bhl->setContentsMargins(0, 12, 0, 12);
            bhl->setSpacing(14);
            auto* blbl = new QLabel(QStringLiteral("Bitrate"), bitrate_row_widget_);
            blbl->setProperty("labelRole", "settingsRowLabel");
            bhl->addWidget(blbl, 1);
            bitrate_kbps_spin_ = new QSpinBox(bitrate_row_widget_);
            bitrate_kbps_spin_->setObjectName(QStringLiteral("bitrateKbpsSpin"));
            bitrate_kbps_spin_->setRange(1000, 100000);
            bitrate_kbps_spin_->setSuffix(QStringLiteral(" kbps"));
            bitrate_kbps_spin_->setValue(static_cast<int>(video_settings_.bitrate_kbps));
            bhl->addWidget(bitrate_kbps_spin_, 0, Qt::AlignVCenter);
            bvl->addLayout(bhl);
            bitrate_row_widget_->setProperty("settingsRow", true);
        }
        fes_layout->addWidget(bitrate_row_widget_);

        // --- v1.0 Format PlaceholderRows ---
        {
            auto* ph1 = new ui::widgets::PlaceholderRow(fmt_expert_section_);
            ph1->setLabel(QStringLiteral("Encoder preset (NVENC)"));
            ph1->setVersionTag(QStringLiteral("0.5"));
            fes_layout->addWidget(ph1);

            auto* ph2 = new ui::widgets::PlaceholderRow(fmt_expert_section_);
            ph2->setLabel(QStringLiteral("HEVC codec"));
            ph2->setVersionTag(QStringLiteral("0.7"));
            fes_layout->addWidget(ph2);

            auto* ph3 = new ui::widgets::PlaceholderRow(fmt_expert_section_);
            ph3->setLabel(QStringLiteral("Bit depth"));
            ph3->setVersionTag(QStringLiteral("0.7"));
            fes_layout->addWidget(ph3);

            auto* ph4 = new ui::widgets::PlaceholderRow(fmt_expert_section_);
            ph4->setLabel(QStringLiteral("HDR10 + colour metadata"));
            ph4->setVersionTag(QStringLiteral("0.7"));
            fes_layout->addWidget(ph4);

            auto* ph5 = new ui::widgets::PlaceholderRow(fmt_expert_section_);
            ph5->setObjectName(QStringLiteral("chromaSubsamplingPlaceholder"));
            ph5->setLabel(QStringLiteral("Chroma subsampling (4:2:0 \xc2\xb7 4:2:2 \xc2\xb7 4:4:4)"));
            ph5->setVersionTag(QStringLiteral("0.7"));
            fes_layout->addWidget(ph5);
        }

        fmt_layout->addWidget(fmt_expert_section_);
    }

    // --- Compat callout (D6: replaces format_display_label_ visually) ---
    compat_callout_widget_ = new QFrame(fmt_panel);
    compat_callout_widget_->setObjectName(QStringLiteral("compatCalloutWidget"));
    compat_callout_widget_->setProperty("panelRole", "compatCallout");
    compat_callout_widget_->setProperty("stateRole", "caution");
    {
        auto* callout_layout = new QHBoxLayout(compat_callout_widget_);
        callout_layout->setContentsMargins(12, 8, 12, 8);
        callout_layout->setSpacing(8);
        auto* callout_icon = new QLabel(compat_callout_widget_);
        callout_icon->setText(QStringLiteral("\xe2\x9a\xa0"));
        callout_layout->addWidget(callout_icon);
        callout_text_ = new QLabel(compat_callout_widget_);
        callout_text_->setObjectName(QStringLiteral("compatCalloutText"));
        callout_text_->setWordWrap(true);
        callout_layout->addWidget(callout_text_, 1);
        auto* fix_btn = new QPushButton(QStringLiteral("Fix codecs"), compat_callout_widget_);
        fix_btn->setObjectName(QStringLiteral("fixCodecsButton"));
        fix_btn->setProperty("role", "ghost");
        fix_btn->setCursor(Qt::PointingHandCursor);
        connect(fix_btn, &QPushButton::clicked, this, [this]() {
            reconcileContainerCodecRules();
            updateCompatCallout();
            emitCurrentFormatSettings();
        });
        callout_layout->addWidget(fix_btn);
    }
    compat_callout_widget_->setVisible(false);
    fmt_layout->addWidget(compat_callout_widget_);

    compat_ok_label_ = new QLabel(fmt_panel);
    compat_ok_label_->setObjectName(QStringLiteral("compatOkLabel"));
    compat_ok_label_->setProperty("labelRole", "muted");
    fmt_layout->addWidget(compat_ok_label_);

    fmt_layout->addWidget(
        makeHint(QStringLiteral("VFR is available for MKV/WebM. MP4 uses CFR for editor compatibility."), fmt_panel));

    // Pre-fill codec combos (D6: free choice, fills once)
    updateVideoCodecChoices();
    updateAudioCodecChoices();

    left_layout->addWidget(fmt_panel);

    // ---- AUDIO CARD (right column) ----
    auto* audio_panel = makePanel(right_col);
    audio_panel_ = audio_panel;
    auto* audio_panel_layout = new QVBoxLayout(audio_panel);
    audio_panel_layout->setContentsMargins(18, 16, 18, 18);
    audio_panel_layout->setSpacing(10);
    audio_panel_layout->addWidget(makeCardTitle(QStringLiteral("Audio"), audio_panel));

    // Helper: build a source row directly into a given layout+parent.
    // DF-12: separate_check is now an ExoToggle pill (was QCheckBox "Separate track").
    // SETTINGS-TIERS-R2: InfoHintIcon added after enabled check and after separate_check.
    auto makeSourceRowInto = [&](QVBoxLayout* target_layout, QWidget* target_parent, const QString& title,
                                 ui::widgets::ExoCheckBox*& enabled_check, ui::widgets::ExoToggle*& separate_check,
                                 QLabel*& source_label, ui::widgets::VUMeterWidget*& meter_out, QLabel*& db_label_out) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);

        enabled_check = new ui::widgets::ExoCheckBox(title, target_parent);
        db_label_out = new QLabel(QStringLiteral("–"), target_parent);
        db_label_out->setProperty("labelRole", "muted");
        db_label_out->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        db_label_out->setMinimumWidth(52);

        // DF-12: pill toggle + label pair replaces the "Separate track" QCheckBox.
        separate_check = new ui::widgets::ExoToggle(target_parent);
        QLabel* separate_label = new QLabel(QStringLiteral("Separate track"), target_parent);
        separate_label->setProperty("labelRole", "muted");

        row->addWidget(enabled_check);
        // InfoHint for the source enable toggle (next to the checkbox).
        row->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kAudioSourceEnable, target_parent), 0,
                       Qt::AlignVCenter);
        row->addStretch();
        row->addWidget(db_label_out);
        row->addWidget(separate_label);
        row->addWidget(separate_check);
        // InfoHint for the "Separate track" toggle.
        row->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kSeparateTrack, target_parent), 0, Qt::AlignVCenter);
        target_layout->addLayout(row);

        meter_out = new ui::widgets::VUMeterWidget(target_parent);
        meter_out->setActive(false);
        target_layout->addWidget(meter_out);

        source_label = new QLabel(target_parent);
        source_label->setProperty("labelRole", "muted");
        source_label->setWordWrap(true);
        target_layout->addWidget(source_label);
    };

    // System audio row (label + description change based on capture target kind):
    //   Display/Region → "Computer audio"
    //   Window        → "Other system audio"
    makeSourceRowInto(audio_panel_layout, audio_panel, QStringLiteral("Computer audio"), sys_enabled_check_,
                      sys_separate_check_, sys_source_label_, audio_sys_meter_, audio_sys_db_label_);
    sys_enabled_check_->setObjectName(QStringLiteral("settingsAudioSysCheck"));
    audio_sys_meter_->setObjectName(QStringLiteral("settingsAudioSysMeter"));
    audio_sys_db_label_->setObjectName(QStringLiteral("settingsAudioSysDbLabel"));

    // Application audio section — wrapped in a container widget that is shown
    // for Window targets and hidden for Display/Region targets.
    app_row_section_ = new QWidget(audio_panel);
    app_row_section_->setObjectName(QStringLiteral("settingsAudioAppSection"));
    {
        auto* app_section_layout = new QVBoxLayout(app_row_section_);
        app_section_layout->setContentsMargins(0, 0, 0, 0);
        app_section_layout->setSpacing(audio_panel_layout->spacing());

        auto* app_rule = new QFrame(app_row_section_);
        app_rule->setFrameShape(QFrame::HLine);
        app_rule->setProperty("frameRole", "sectionRuleLine");
        app_section_layout->addWidget(app_rule);

        makeSourceRowInto(app_section_layout, app_row_section_, QStringLiteral("Application audio"), app_enabled_check_,
                          app_separate_check_, app_source_label_, audio_app_meter_, audio_app_db_label_);
        app_enabled_check_->setObjectName(QStringLiteral("settingsAudioAppCheck"));
        audio_app_meter_->setObjectName(QStringLiteral("settingsAudioAppMeter"));
        audio_app_db_label_->setObjectName(QStringLiteral("settingsAudioAppDbLabel"));
    }
    audio_panel_layout->addWidget(app_row_section_);
    // Hidden by default — shown when target kind is Window.
    app_row_section_->setVisible(false);

    audio_panel_layout->addWidget(makeHRule(audio_panel));
    makeSourceRowInto(audio_panel_layout, audio_panel, QStringLiteral("Microphone"), mic_enabled_check_,
                      mic_separate_check_, mic_source_label_, audio_mic_meter_, audio_mic_db_label_);
    audio_mic_meter_->setObjectName(QStringLiteral("settingsAudioMicMeter"));
    audio_mic_db_label_->setObjectName(QStringLiteral("settingsAudioMicDbLabel"));

    // Mic device row: combo + compact Rescan button (routes through notifier).
    {
        auto* mic_row = new QWidget(audio_panel);
        auto* mic_rl = new QHBoxLayout(mic_row);
        mic_rl->setContentsMargins(0, 0, 0, 0);
        mic_rl->setSpacing(6);
        mic_device_combo_ = new QComboBox(mic_row);
        mic_device_combo_->setObjectName(QStringLiteral("micDeviceCombo"));
        mic_device_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        mic_device_combo_->setMinimumWidth(0); // Wave 2 Part C: let it shrink with the layout
        mic_rl->addWidget(mic_device_combo_, 1);
        audio_rescan_btn_ = new QPushButton(mic_row); // #09: icon-only rescan button
        audio_rescan_btn_->setObjectName(QStringLiteral("audioRescanBtn"));
        audio_rescan_btn_->setProperty("role", "ghost");
        audio_rescan_btn_->setToolTip(QStringLiteral("Rescan audio devices"));
        audio_rescan_btn_->setFixedWidth(36);
        audio_rescan_btn_->setCursor(Qt::PointingHandCursor);
        {
            const QIcon rescan_icon(QStringLiteral(":/theme/icons/rescan.svg"));
            if (!rescan_icon.isNull()) {
                audio_rescan_btn_->setIcon(rescan_icon);
                audio_rescan_btn_->setIconSize(QSize(14, 14));
            } else {
                audio_rescan_btn_->setText(QStringLiteral("\xe2\x86\xba"));
            }
        }
        mic_rl->addWidget(audio_rescan_btn_);
        audio_panel_layout->addWidget(mic_row);
    }

    // SETTINGS-TIERS-R1 Phase 1b: the "Separate track" toggles stay in their own
    // source rows (beside the enabled check and dB label).  An expander is wrong
    // here because each toggle is a per-row control, not a cohesive Advanced group.
    // audio_separate_expander_ remains null; the store field is harmless / unused.

    // PS-PHASE-C: Expert Audio section — mic gain, channel mode, audio bitrate, Opus params + placeholders.
    {
        audio_expert_section_ = new QWidget(audio_panel);
        audio_expert_section_->setObjectName(QStringLiteral("audioExpertSection"));
        audio_expert_section_->setVisible(false); // hidden until expert mode on
        auto* aes_layout = new QVBoxLayout(audio_expert_section_);
        aes_layout->setContentsMargins(0, 0, 0, 0);
        aes_layout->setSpacing(0);

        auto* aes_rule_top = new QFrame(audio_expert_section_);
        aes_rule_top->setFrameShape(QFrame::HLine);
        aes_rule_top->setProperty("frameRole", "sectionRuleLine");
        aes_layout->addWidget(aes_rule_top);

        // Mic gain — QSlider (–12…+12 dB, step 1) + read-only dB label.
        // Polish-R1: switched from QSpinBox to QSlider per mockup (suite-settings.jsx).
        {
            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(14);
            auto* lbl = new QLabel(QStringLiteral("Mic gain"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 1);

            const int init_db =
                static_cast<int>(std::roundf(20.f * std::log10f(std::max(0.001f, audio_ui_state_.mic_gain_linear))));

            mic_gain_slider_ = new QSlider(Qt::Horizontal, row);
            mic_gain_slider_->setObjectName(QStringLiteral("micGainSlider"));
            mic_gain_slider_->setRange(-12, 12);
            mic_gain_slider_->setSingleStep(1);
            mic_gain_slider_->setPageStep(3);
            mic_gain_slider_->setValue(init_db);
            mic_gain_slider_->setFixedWidth(120);
            hl->addWidget(mic_gain_slider_, 0, Qt::AlignVCenter);

            mic_gain_db_label_ = new QLabel(row);
            mic_gain_db_label_->setObjectName(QStringLiteral("micGainDbLabel"));
            mic_gain_db_label_->setProperty("labelRole", "settingsValueLabel");
            mic_gain_db_label_->setFixedWidth(42);
            mic_gain_db_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            mic_gain_db_label_->setText(QStringLiteral("%1 dB").arg(init_db));
            hl->addWidget(mic_gain_db_label_, 0, Qt::AlignVCenter);

            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);
        }

        // Mic channel mode
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(14);
            auto* lbl = new QLabel(QStringLiteral("Mic channel mode"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 1);
            mic_channel_mode_combo_ = new QComboBox(row);
            mic_channel_mode_combo_->setObjectName(QStringLiteral("micChannelModeCombo"));
            mic_channel_mode_combo_->addItem(QStringLiteral("Auto"),
                                             static_cast<int>(recorder_core::MicChannelMode::Auto));
            mic_channel_mode_combo_->addItem(QStringLiteral("Mono mix"),
                                             static_cast<int>(recorder_core::MicChannelMode::MonoMix));
            mic_channel_mode_combo_->addItem(QStringLiteral("Preserve stereo"),
                                             static_cast<int>(recorder_core::MicChannelMode::PreserveStereo));
            mic_channel_mode_combo_->addItem(QStringLiteral("L \xe2\x86\x92 Stereo"),
                                             static_cast<int>(recorder_core::MicChannelMode::LeftToStereo));
            mic_channel_mode_combo_->addItem(QStringLiteral("R \xe2\x86\x92 Stereo"),
                                             static_cast<int>(recorder_core::MicChannelMode::RightToStereo));
            hl->addWidget(mic_channel_mode_combo_, 0, Qt::AlignVCenter);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);
        }

        // Audio bitrate
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(14);
            auto* lbl = new QLabel(QStringLiteral("Audio bitrate"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 1);
            audio_bitrate_kbps_spin_ = new QSpinBox(row);
            audio_bitrate_kbps_spin_->setObjectName(QStringLiteral("audioBitrateKbpsSpin"));
            audio_bitrate_kbps_spin_->setRange(32, 510);
            audio_bitrate_kbps_spin_->setSuffix(QStringLiteral(" kbps"));
            audio_bitrate_kbps_spin_->setValue(static_cast<int>(audio_ui_state_.audio_bitrate_kbps));
            hl->addWidget(audio_bitrate_kbps_spin_, 0, Qt::AlignVCenter);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);
        }

        // Opus frame duration
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(14);
            auto* lbl = new QLabel(QStringLiteral("Opus frame duration"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 1);
            opus_frame_duration_combo_ = new QComboBox(row);
            opus_frame_duration_combo_->setObjectName(QStringLiteral("opusFrameDurationCombo"));
            opus_frame_duration_combo_->addItem(QStringLiteral("20 ms"),
                                                static_cast<int>(recorder_core::OpusFrameDuration::Ms20));
            opus_frame_duration_combo_->addItem(QStringLiteral("10 ms"),
                                                static_cast<int>(recorder_core::OpusFrameDuration::Ms10));
            opus_frame_duration_combo_->addItem(QStringLiteral("5 ms"),
                                                static_cast<int>(recorder_core::OpusFrameDuration::Ms5));
            hl->addWidget(opus_frame_duration_combo_, 0, Qt::AlignVCenter);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);
        }

        // Opus complexity
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(14);
            auto* lbl = new QLabel(QStringLiteral("Opus complexity"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 1);
            opus_complexity_spin_ = new QSpinBox(row);
            opus_complexity_spin_->setObjectName(QStringLiteral("opusComplexitySpin"));
            opus_complexity_spin_->setRange(0, 10);
            opus_complexity_spin_->setValue(audio_ui_state_.opus_complexity);
            hl->addWidget(opus_complexity_spin_, 0, Qt::AlignVCenter);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);
        }

        // Brickwall limiter (Audio v2 — 0.6.0)
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(14);
            limiter_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("Brickwall limiter"), row);
            limiter_check_->setObjectName(QStringLiteral("limiterCheck"));
            limiter_check_->setChecked(audio_ui_state_.limiter_enabled);
            hl->addWidget(limiter_check_, 1);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);

            auto* crow = new QWidget(audio_expert_section_);
            auto* chl = new QHBoxLayout(crow);
            chl->setContentsMargins(0, 12, 0, 12);
            chl->setSpacing(14);
            auto* clbl = new QLabel(QStringLiteral("Limiter ceiling"), crow);
            clbl->setProperty("labelRole", "settingsRowLabel");
            chl->addWidget(clbl, 1);
            limiter_ceiling_spin_ = new QDoubleSpinBox(crow);
            limiter_ceiling_spin_->setObjectName(QStringLiteral("limiterCeilingSpin"));
            limiter_ceiling_spin_->setRange(-60.0, 0.0);
            limiter_ceiling_spin_->setSingleStep(0.5);
            limiter_ceiling_spin_->setDecimals(1);
            limiter_ceiling_spin_->setSuffix(QStringLiteral(" dB"));
            limiter_ceiling_spin_->setValue(static_cast<double>(audio_ui_state_.limiter_ceiling_db));
            limiter_ceiling_spin_->setEnabled(audio_ui_state_.limiter_enabled);
            chl->addWidget(limiter_ceiling_spin_, 0, Qt::AlignVCenter);
            crow->setProperty("settingsRow", true);
            aes_layout->addWidget(crow);
        }

        // Microphone high-pass filter (Audio v2 — 0.6.0)
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(14);
            mic_hpf_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("High-pass filter"), row);
            mic_hpf_check_->setObjectName(QStringLiteral("micHpfCheck"));
            mic_hpf_check_->setChecked(audio_ui_state_.mic_hpf_enabled);
            hl->addWidget(mic_hpf_check_, 1);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);

            auto* crow = new QWidget(audio_expert_section_);
            auto* chl = new QHBoxLayout(crow);
            chl->setContentsMargins(0, 12, 0, 12);
            chl->setSpacing(14);
            auto* clbl = new QLabel(QStringLiteral("HPF cutoff"), crow);
            clbl->setProperty("labelRole", "settingsRowLabel");
            chl->addWidget(clbl, 1);
            mic_hpf_cutoff_spin_ = new QDoubleSpinBox(crow);
            mic_hpf_cutoff_spin_->setObjectName(QStringLiteral("micHpfCutoffSpin"));
            mic_hpf_cutoff_spin_->setRange(20.0, 1000.0);
            mic_hpf_cutoff_spin_->setSingleStep(5.0);
            mic_hpf_cutoff_spin_->setDecimals(0);
            mic_hpf_cutoff_spin_->setSuffix(QStringLiteral(" Hz"));
            mic_hpf_cutoff_spin_->setValue(static_cast<double>(audio_ui_state_.mic_hpf_cutoff_hz));
            mic_hpf_cutoff_spin_->setEnabled(audio_ui_state_.mic_hpf_enabled);
            chl->addWidget(mic_hpf_cutoff_spin_, 0, Qt::AlignVCenter);
            crow->setProperty("settingsRow", true);
            aes_layout->addWidget(crow);
        }

        // Microphone noise gate (Audio v2 — 0.6.0)
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(14);
            mic_gate_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("Noise gate"), row);
            mic_gate_check_->setObjectName(QStringLiteral("micGateCheck"));
            mic_gate_check_->setChecked(audio_ui_state_.mic_gate_enabled);
            hl->addWidget(mic_gate_check_, 1);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);

            auto* trow = new QWidget(audio_expert_section_);
            auto* thl = new QHBoxLayout(trow);
            thl->setContentsMargins(0, 12, 0, 12);
            thl->setSpacing(14);
            auto* tlbl = new QLabel(QStringLiteral("Gate threshold"), trow);
            tlbl->setProperty("labelRole", "settingsRowLabel");
            thl->addWidget(tlbl, 1);
            mic_gate_threshold_spin_ = new QDoubleSpinBox(trow);
            mic_gate_threshold_spin_->setObjectName(QStringLiteral("micGateThresholdSpin"));
            mic_gate_threshold_spin_->setRange(-80.0, 0.0);
            mic_gate_threshold_spin_->setSingleStep(1.0);
            mic_gate_threshold_spin_->setDecimals(0);
            mic_gate_threshold_spin_->setSuffix(QStringLiteral(" dB"));
            mic_gate_threshold_spin_->setValue(static_cast<double>(audio_ui_state_.mic_gate_threshold_db));
            mic_gate_threshold_spin_->setEnabled(audio_ui_state_.mic_gate_enabled);
            thl->addWidget(mic_gate_threshold_spin_, 0, Qt::AlignVCenter);
            trow->setProperty("settingsRow", true);
            aes_layout->addWidget(trow);
        }

        // Microphone automatic gain control (Audio v2 — 0.6.0).
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(14);
            mic_agc_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("Automatic gain control"), row);
            mic_agc_check_->setObjectName(QStringLiteral("micAgcCheck"));
            mic_agc_check_->setChecked(audio_ui_state_.mic_agc_enabled);
            hl->addWidget(mic_agc_check_, 1);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);

            auto* trow = new QWidget(audio_expert_section_);
            auto* thl = new QHBoxLayout(trow);
            thl->setContentsMargins(0, 12, 0, 12);
            thl->setSpacing(14);
            auto* tlbl = new QLabel(QStringLiteral("AGC target level"), trow);
            tlbl->setProperty("labelRole", "settingsRowLabel");
            thl->addWidget(tlbl, 1);
            mic_agc_target_spin_ = new QDoubleSpinBox(trow);
            mic_agc_target_spin_->setObjectName(QStringLiteral("micAgcTargetSpin"));
            mic_agc_target_spin_->setRange(-40.0, 0.0);
            mic_agc_target_spin_->setSingleStep(1.0);
            mic_agc_target_spin_->setDecimals(0);
            mic_agc_target_spin_->setSuffix(QStringLiteral(" dB"));
            mic_agc_target_spin_->setValue(static_cast<double>(audio_ui_state_.mic_agc_target_db));
            mic_agc_target_spin_->setEnabled(audio_ui_state_.mic_agc_enabled);
            thl->addWidget(mic_agc_target_spin_, 0, Qt::AlignVCenter);
            trow->setProperty("settingsRow", true);
            aes_layout->addWidget(trow);
        }

        // Audio PlaceholderRows
        {
            auto* ph1 = new ui::widgets::PlaceholderRow(audio_expert_section_);
            ph1->setLabel(QStringLiteral("PCM / FLAC codecs"));
            ph1->setVersionTag(QStringLiteral("0.6"));
            aes_layout->addWidget(ph1);

            auto* ph2 = new ui::widgets::PlaceholderRow(audio_expert_section_);
            ph2->setLabel(QStringLiteral("RNNoise"));
            ph2->setVersionTag(QStringLiteral("0.6"));
            aes_layout->addWidget(ph2);
        }

        audio_panel_layout->addWidget(audio_expert_section_);
    }

    audio_panel_layout->addWidget(
        makeHint(QStringLiteral("Separate tracks keep each source on its own channel for editing."), audio_panel));

    audio_summary_label_ = new QLabel(audio_panel);
    audio_summary_label_->setProperty("labelRole", "muted");
    audio_summary_label_->setWordWrap(true);
    audio_summary_label_->setVisible(false);
    audio_panel_layout->addWidget(audio_summary_label_);
    right_layout->addWidget(audio_panel);

    // ---- WEBCAM CARD (left column — D6: in 2-column grid below Format) ----
    auto* webcam_panel = makePanel(left_col);
    webcam_panel_ = webcam_panel;
    auto* webcam_panel_layout = new QVBoxLayout(webcam_panel);
    webcam_panel_layout->setContentsMargins(18, 16, 18, 18);
    webcam_panel_layout->setSpacing(10);
    webcam_panel_layout->addWidget(makeCardTitle(QStringLiteral("Webcam"), webcam_panel));

    webcam_setup_panel_ = new ui::widgets::WebcamSetupPanel(webcam_panel);
    webcam_setup_panel_->setObjectName(QStringLiteral("settingsWebcamSetupPanel"));
    webcam_panel_layout->addWidget(webcam_setup_panel_);
    left_layout->addWidget(webcam_panel);

    // ---- OUTPUT CARD (right column — D6: in 2-column grid below Audio) ----
    auto* out_panel = makePanel(right_col);
    out_panel_ = out_panel;
    auto* out_panel_layout = new QVBoxLayout(out_panel);
    out_panel_layout->setContentsMargins(18, 16, 18, 18);
    out_panel_layout->setSpacing(12);
    out_panel_layout->addWidget(makeCardTitle(QStringLiteral("Output"), out_panel));

    // D6: CompareHint for Output resolution (replaces plain InfoHintIcon).
    resolution_compare_hint_ =
        new ui::widgets::CompareHint(QStringLiteral("resolution"), QStringLiteral("Native"), out_panel);

    // Label + CompareHint side by side (matches makeFieldLabelWithHint layout but with CompareHint).
    {
        auto* res_label_row = new QWidget(out_panel);
        auto* rll = new QHBoxLayout(res_label_row);
        rll->setContentsMargins(0, 0, 0, 0);
        rll->setSpacing(4);
        auto* res_lbl = new QLabel(QStringLiteral("Output resolution"), res_label_row);
        res_lbl->setProperty("labelRole", "settingsRowLabel");
        rll->addWidget(res_lbl);
        rll->addWidget(resolution_compare_hint_, 0, Qt::AlignVCenter);
        rll->addStretch();
        out_panel_layout->addWidget(res_label_row);
    }
    auto* out_res_segmented = new QWidget(out_panel);
    out_res_segmented->setObjectName(QStringLiteral("outputResSegmented"));
    auto* out_res_layout = new QHBoxLayout(out_res_segmented);
    out_res_layout->setContentsMargins(3, 3, 3, 3);
    out_res_layout->setSpacing(0);
    output_resolution_group_ = new QButtonGroup(this);
    output_resolution_group_->setExclusive(true);
    auto makeOutputResolutionSegment = [&](const QString& object_name, const QString& label,
                                           OutputResolutionMode mode) -> QPushButton* {
        auto* seg = new QPushButton(label, out_res_segmented);
        seg->setObjectName(object_name);
        seg->setAccessibleName(label);
        seg->setCheckable(true);
        seg->setAutoDefault(false);
        seg->setDefault(false);
        seg->setCursor(Qt::PointingHandCursor);
        seg->setProperty("qualitySegment", true);
        seg->setProperty("qualitySegmentSelected", false);
        seg->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        output_resolution_group_->addButton(seg, static_cast<int>(mode));
        out_res_layout->addWidget(seg);
        return seg;
    };
    output_res_native_btn_ = makeOutputResolutionSegment(QStringLiteral("outputResNativeButton"),
                                                         QStringLiteral("Native"), OutputResolutionMode::Native);
    output_res_4k_btn_ = makeOutputResolutionSegment(QStringLiteral("outputRes4kButton"), QStringLiteral("4K"),
                                                     OutputResolutionMode::UHD2160);
    output_res_1440_btn_ = makeOutputResolutionSegment(QStringLiteral("outputRes1440Button"), QStringLiteral("1440p"),
                                                       OutputResolutionMode::QHD1440);
    output_res_1080_btn_ = makeOutputResolutionSegment(QStringLiteral("outputRes1080Button"), QStringLiteral("1080p"),
                                                       OutputResolutionMode::FHD1080);
    output_res_720_btn_ = makeOutputResolutionSegment(QStringLiteral("outputRes720Button"), QStringLiteral("720p"),
                                                      OutputResolutionMode::HD720);
    output_res_custom_btn_ = makeOutputResolutionSegment(QStringLiteral("outputResCustomButton"),
                                                         QStringLiteral("Custom"), OutputResolutionMode::Custom);
    out_panel_layout->addWidget(out_res_segmented);

    // Custom resolution width/height fields (CUSTOM-OUTPUT-RESOLUTION-R1).
    custom_resolution_widget_ = new QWidget(out_panel);
    custom_resolution_widget_->setObjectName(QStringLiteral("customResolutionWidget"));
    custom_resolution_widget_->setVisible(false);
    auto* custom_res_layout = new QHBoxLayout(custom_resolution_widget_);
    custom_res_layout->setContentsMargins(0, 0, 0, 0);
    custom_res_layout->setSpacing(8);

    auto* width_label = new QLabel(QStringLiteral("Width"), custom_resolution_widget_);
    width_label->setProperty("labelRole", "settingsRowLabel");
    custom_width_spin_ = new QSpinBox(custom_resolution_widget_);
    custom_width_spin_->setObjectName(QStringLiteral("customWidthSpin"));
    custom_width_spin_->setRange(320, 7680);
    custom_width_spin_->setSingleStep(2);
    custom_width_spin_->setSuffix(QStringLiteral(" px"));
    custom_width_spin_->setToolTip(QStringLiteral("Custom output width (320–7680)"));

    auto* height_label = new QLabel(QStringLiteral("Height"), custom_resolution_widget_);
    height_label->setProperty("labelRole", "settingsRowLabel");
    custom_height_spin_ = new QSpinBox(custom_resolution_widget_);
    custom_height_spin_->setObjectName(QStringLiteral("customHeightSpin"));
    custom_height_spin_->setRange(180, 7680);
    custom_height_spin_->setSingleStep(2);
    custom_height_spin_->setSuffix(QStringLiteral(" px"));
    custom_height_spin_->setToolTip(QStringLiteral("Custom output height (180–7680)"));

    custom_res_layout->addWidget(width_label);
    custom_res_layout->addWidget(custom_width_spin_);
    custom_res_layout->addWidget(height_label);
    custom_res_layout->addWidget(custom_height_spin_);
    custom_res_layout->addStretch();
    out_panel_layout->addWidget(custom_resolution_widget_);

    custom_resolution_validation_label_ = makeHint(QString(), out_panel);
    custom_resolution_validation_label_->setObjectName(QStringLiteral("customResolutionValidationLabel"));
    custom_resolution_validation_label_->setVisible(false);
    out_panel_layout->addWidget(custom_resolution_validation_label_);

    output_effective_summary_label_ = makeHint(QString(), out_panel);
    output_effective_summary_label_->setObjectName(QStringLiteral("outputEffectiveSummaryLabel"));
    out_panel_layout->addWidget(output_effective_summary_label_);

    // ---- Split recording (SPLIT-RECORDING-R1 / SPLIT-BY-SIZE-R1) — Wave 2: expert-gated section ----
    // Wave 2: the SettingsCardExpander was dissolved. Split controls now live in a plain
    // QWidget that is shown/hidden by updateExpertModeVisibility() together with the
    // developer card (same expert-mode gate, no per-card expander).
    split_expert_section_ = new QWidget(out_panel);
    split_expert_section_->setObjectName(QStringLiteral("splitExpertSection"));
    split_expert_section_->setVisible(false); // hidden until expert mode is on
    {
        auto* split_expert_layout = new QVBoxLayout(split_expert_section_);
        split_expert_layout->setContentsMargins(0, 0, 0, 0);
        split_expert_layout->setSpacing(8);

        split_expert_layout->addWidget(makeOutputSubLabelWithHint(QStringLiteral("Split recording"),
                                                                  ui::hints::kSplitRecording, split_expert_section_));
        auto* split_row = new QHBoxLayout();
        split_row->setContentsMargins(0, 0, 0, 0);
        split_row->setSpacing(8);
        split_mode_combo_ = new QComboBox(split_expert_section_);
        split_mode_combo_->setObjectName(QStringLiteral("splitModeCombo"));
        split_mode_combo_->addItem(QStringLiteral("Off"), static_cast<int>(SplitRecordingMode::Off));
        split_mode_combo_->addItem(QStringLiteral("Every 15 min"), static_cast<int>(SplitRecordingMode::Every15Min));
        split_mode_combo_->addItem(QStringLiteral("Every 30 min"), static_cast<int>(SplitRecordingMode::Every30Min));
        split_mode_combo_->addItem(QStringLiteral("Every 60 min"), static_cast<int>(SplitRecordingMode::Every60Min));
        split_mode_combo_->addItem(QStringLiteral("Custom"), static_cast<int>(SplitRecordingMode::Custom));
        split_mode_combo_->setToolTip(
            QStringLiteral("Automatically start a new file at the chosen interval (manual splits always work)."));
        split_row->addWidget(split_mode_combo_, 0);

        split_custom_widget_ = new QWidget(split_expert_section_);
        auto* split_custom_layout = new QHBoxLayout(split_custom_widget_);
        split_custom_layout->setContentsMargins(0, 0, 0, 0);
        split_custom_layout->setSpacing(8);
        auto* split_every_label = new QLabel(QStringLiteral("Every"), split_custom_widget_);
        split_every_label->setProperty("labelRole", "settingsRowLabel");
        split_custom_minutes_spin_ = new QSpinBox(split_custom_widget_);
        split_custom_minutes_spin_->setObjectName(QStringLiteral("splitCustomMinutesSpin"));
        split_custom_minutes_spin_->setRange(static_cast<int>(SplitRecordingSettings::kMinMinutes),
                                             static_cast<int>(SplitRecordingSettings::kMaxMinutes));
        split_custom_minutes_spin_->setSuffix(QStringLiteral(" min"));
        split_custom_minutes_spin_->setToolTip(QStringLiteral("Custom split interval (1 min – 24 h)"));
        split_custom_layout->addWidget(split_every_label);
        split_custom_layout->addWidget(split_custom_minutes_spin_);
        split_custom_widget_->setVisible(false);
        split_row->addWidget(split_custom_widget_, 0);
        split_row->addStretch();
        split_expert_layout->addLayout(split_row);

        split_summary_label_ = makeHint(QString(), split_expert_section_);
        split_summary_label_->setObjectName(QStringLiteral("splitSummaryLabel"));
        split_expert_layout->addWidget(split_summary_label_);

        // ---- Split recording by size (SPLIT-BY-SIZE-R1) ----
        split_expert_layout->addWidget(makeOutputSubLabel(QStringLiteral("Split by size"), split_expert_section_));
        auto* split_size_row = new QHBoxLayout();
        split_size_row->setContentsMargins(0, 0, 0, 0);
        split_size_row->setSpacing(8);
        split_size_mode_combo_ = new QComboBox(split_expert_section_);
        split_size_mode_combo_->setObjectName(QStringLiteral("splitSizeModeCombo"));
        split_size_mode_combo_->addItem(QStringLiteral("Off"), static_cast<int>(SplitSizeMode::Off));
        split_size_mode_combo_->addItem(QStringLiteral("Custom"), static_cast<int>(SplitSizeMode::Custom));
        split_size_mode_combo_->setToolTip(
            QStringLiteral("Automatically start a new file when the segment reaches the chosen size."));
        split_size_row->addWidget(split_size_mode_combo_, 0);

        split_size_custom_widget_ = new QWidget(split_expert_section_);
        auto* split_size_custom_layout = new QHBoxLayout(split_size_custom_widget_);
        split_size_custom_layout->setContentsMargins(0, 0, 0, 0);
        split_size_custom_layout->setSpacing(8);
        auto* split_size_label = new QLabel(QStringLiteral("Every"), split_size_custom_widget_);
        split_size_label->setProperty("labelRole", "settingsRowLabel");
        split_custom_size_spin_ = new QSpinBox(split_expert_section_);
        split_custom_size_spin_->setObjectName(QStringLiteral("splitCustomSizeSpin"));
        // kMaxSizeMb = 1024*1024 = 1048576 which fits in int (< 2147483647).
        split_custom_size_spin_->setRange(static_cast<int>(SplitRecordingSettings::kMinSizeMb),
                                          static_cast<int>(SplitRecordingSettings::kMaxSizeMb));
        split_custom_size_spin_->setSuffix(QStringLiteral(" MB"));
        split_custom_size_spin_->setToolTip(
            QStringLiteral("Split segment size in MiB (50 MiB – 1 TiB). Whichever limit (time or size) "
                           "is reached first triggers the split."));
        split_size_custom_layout->addWidget(split_size_label);
        split_size_custom_layout->addWidget(split_custom_size_spin_);
        split_size_custom_widget_->setVisible(false);
        split_size_row->addWidget(split_size_custom_widget_, 0);
        split_size_row->addStretch();
        split_expert_layout->addLayout(split_size_row);
    }

    out_panel_layout->addWidget(split_expert_section_);

    // PS-PHASE-C: Output v1.0 placeholder — expert-gated.
    {
        auto* out_ph_section = new QWidget(out_panel);
        out_ph_section->setObjectName(QStringLiteral("outputV1PlaceholderSection"));
        out_ph_section->setVisible(false); // hidden until expert mode on
        auto* out_ph_layout = new QVBoxLayout(out_ph_section);
        out_ph_layout->setContentsMargins(0, 0, 0, 0);
        out_ph_layout->setSpacing(0);
        auto* ph_auto_open = new ui::widgets::PlaceholderRow(out_ph_section);
        ph_auto_open->setObjectName(QStringLiteral("autoOpenPlaceholderRow"));
        ph_auto_open->setLabel(QStringLiteral("Auto-open after recording"));
        ph_auto_open->setVersionTag(QStringLiteral("1.0"));
        out_ph_layout->addWidget(ph_auto_open);
        out_panel_layout->addWidget(out_ph_section);
        // Wire expert-mode visibility — we store the pointer as local; find it via objectName in
        // updateExpertModeVisibility Instead, connect via findChild in updateExpertModeVisibility.
        (void)out_ph_section; // stored as child of out_panel; found by objectName in updateExpertModeVisibility
    }

    auto* output_split = new QWidget(out_panel);
    output_split_layout_ = new QHBoxLayout(output_split);
    output_split_layout_->setContentsMargins(0, 0, 0, 0);
    output_split_layout_->setSpacing(24);

    auto* output_fields = new QWidget(output_split);
    auto* output_fields_layout = new QVBoxLayout(output_fields);
    output_fields_layout->setContentsMargins(0, 0, 0, 0);
    output_fields_layout->setSpacing(8);

    output_fields_layout->addWidget(
        makeOutputSubLabelWithHint(QStringLiteral("Destination folder"), ui::hints::kOutputFolder, output_fields));
    auto* dest_row = new QHBoxLayout();
    dest_row->setSpacing(8);
    destination_edit_ = new QLineEdit(output_fields);
    destination_edit_->setObjectName(QStringLiteral("destinationEdit"));
    destination_edit_->setPlaceholderText(QString::fromStdWString(format_settings_.output_folder.wstring()));
    browse_btn_ = new QPushButton(QStringLiteral("Browse..."), output_fields);
    browse_btn_->setProperty("role", "ghost");
    dest_row->addWidget(destination_edit_, 1);
    dest_row->addWidget(browse_btn_);
    output_fields_layout->addLayout(dest_row);

    folder_validation_label_ = makeHint(QString(), output_fields);
    folder_validation_label_->setVisible(false);
    output_fields_layout->addWidget(folder_validation_label_);

    output_fields_layout->addWidget(
        makeOutputSubLabelWithHint(QStringLiteral("Filename pattern"), ui::hints::kFilenamePattern, output_fields));
    naming_edit_ = new QLineEdit(output_fields);
    naming_edit_->setObjectName(QStringLiteral("namingEdit"));
    naming_edit_->setPlaceholderText(QStringLiteral("{datetime}_{app}_{title}"));
    output_fields_layout->addWidget(naming_edit_);

    pattern_validation_label_ = makeHint(QString(), output_fields);
    pattern_validation_label_->setVisible(false);
    output_fields_layout->addWidget(pattern_validation_label_);

    example_filename_label_ = makeHint(QString(), output_fields);
    output_fields_layout->addWidget(example_filename_label_);
    output_fields_layout->addStretch();

    auto* output_help = new QWidget(output_split);
    auto* output_help_layout = new QVBoxLayout(output_help);
    output_help_layout->setContentsMargins(0, 0, 0, 0);
    output_help_layout->setSpacing(8);

    output_help_layout->addWidget(makeOutputSubLabel(QStringLiteral("Filename tokens"), output_help));

    // Compact chips of the most common real tokens; the full reference stays behind the toggle.
    // Only tokens the FilenameBuilder actually resolves are shown (e.g. {target}/{profile}).
    // D6 wave-2: replaced fixed "4 per row" QHBoxLayouts with a ChipFlowWidget so chips
    // wrap naturally at any available width without overflow or clipping.
    const QStringList token_chips = {QStringLiteral("{datetime}"), QStringLiteral("{date}"),
                                     QStringLiteral("{time}"),     QStringLiteral("{app}"),
                                     QStringLiteral("{title}"),    QStringLiteral("{target}"),
                                     QStringLiteral("{profile}"),  QStringLiteral("{container}")};
    auto* chip_flow = new ChipFlowWidget(output_help);
    for (const QString& token : token_chips) {
        auto* chip = new QLabel(token, chip_flow);
        chip->setProperty("labelRole", "tokenChip");
        chip_flow->addChip(chip);
    }
    output_help_layout->addWidget(chip_flow);

    token_help_toggle_btn_ = new QPushButton(QStringLiteral("Show token reference"), output_help);
    token_help_toggle_btn_->setObjectName(QStringLiteral("tokenHelpToggle"));
    token_help_toggle_btn_->setProperty("role", "ghost");
    output_help_layout->addWidget(token_help_toggle_btn_, 0, Qt::AlignLeft);

    token_help_label_ = makeHint(
        QStringLiteral("Tokens: {datetime}, {date}, {time}, {timestamp}, {YYYY}, {YY}, {MM}, {DD}, {hh}, {mm}, {ss}, "
                       "{app}, {title}, {process}, {target}, {profile}, {container}, {video}, {audio}"),
        output_help);
    token_help_label_->setVisible(false);
    output_help_layout->addWidget(token_help_label_);

    output_help_layout->addWidget(
        makeHint(QStringLiteral("Tokens auto-fill names from the date, app, and capture target."), output_help));
    output_help_layout->addStretch();

    output_split_layout_->addWidget(output_fields, 3);
    output_split_layout_->addWidget(output_help, 2);
    out_panel_layout->addWidget(output_split);
    right_layout->addWidget(out_panel);

    // ---- PRESENCE CARD (left column — SETTINGS-TIERS-P3) — D6: flat rows, below Webcam ----
    {
        auto* presence_panel = makePanel(left_col);
        presence_panel_ = presence_panel;
        auto* presence_layout = new QVBoxLayout(presence_panel);
        presence_layout->setContentsMargins(18, 14, 18, 14);
        presence_layout->setSpacing(0);
        presence_layout->addWidget(makeCardTitle(QStringLiteral("Presence"), presence_panel));

        overlay_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("Show on-screen status overlay during recording"),
                                                      presence_panel);
        overlay_check_->setChecked(true);
        presence_layout->addWidget(
            makeSettingsRow(presence_panel, QStringLiteral("Recording overlay"),
                            new ui::widgets::InfoHintIcon(ui::hints::kRecordingOverlay, presence_panel), QString(),
                            overlay_check_, /*first=*/true));

        diagnostics_overlay_check_ = new ui::widgets::ExoCheckBox(
            QStringLiteral("Show live diagnostics on the recorded monitor during recording"), presence_panel);
        diagnostics_overlay_check_->setChecked(false);
        presence_layout->addWidget(
            makeSettingsRow(presence_panel, QStringLiteral("Diagnostics overlay"),
                            new ui::widgets::InfoHintIcon(
                                ui::hints::kDiagnosticsOverlay +
                                    QStringLiteral("\n\nRead-only and capture-excluded — injects nothing into any "
                                                   "process. Some anti-cheat systems may still flag third-party "
                                                   "overlays; disable it if you hit issues."),
                                presence_panel),
                            QString(), diagnostics_overlay_check_));

        notifications_check_ =
            new ui::widgets::ExoCheckBox(QStringLiteral("Show on-screen notification toasts"), presence_panel);
        notifications_check_->setChecked(true);
        presence_layout->addWidget(makeSettingsRow(
            presence_panel, QStringLiteral("Notifications"),
            new ui::widgets::InfoHintIcon(ui::hints::kNotifications, presence_panel), QString(), notifications_check_));

        keep_in_tray_check_ =
            new ui::widgets::ExoCheckBox(QStringLiteral("Keep running in tray when window closed"), presence_panel);
        keep_in_tray_check_->setChecked(false);
        presence_layout->addWidget(makeSettingsRow(
            presence_panel, QStringLiteral("Tray behavior"),
            new ui::widgets::InfoHintIcon(ui::hints::kCloseToTray, presence_panel), QString(), keep_in_tray_check_));

        quick_controls_check_ =
            new ui::widgets::ExoCheckBox(QStringLiteral("Show quick-control pill during recording"), presence_panel);
        quick_controls_check_->setChecked(false);
        presence_layout->addWidget(
            makeSettingsRow(presence_panel, QStringLiteral("Quick controls"),
                            new ui::widgets::InfoHintIcon(ui::hints::kQuickControlPill, presence_panel), QString(),
                            quick_controls_check_));

        // PS-PHASE-C: Presence v0.6 placeholder (expert-gated).
        {
            auto* presence_ph_section = new QWidget(presence_panel);
            presence_ph_section->setObjectName(QStringLiteral("presenceV1PlaceholderSection"));
            presence_ph_section->setVisible(false);
            auto* ppl = new QVBoxLayout(presence_ph_section);
            ppl->setContentsMargins(0, 0, 0, 0);
            ppl->setSpacing(0);
            auto* ph = new ui::widgets::PlaceholderRow(presence_ph_section);
            ph->setLabel(QStringLiteral("Per-track gain / mute"));
            ph->setVersionTag(QStringLiteral("0.6"));
            ppl->addWidget(ph);
            presence_layout->addWidget(presence_ph_section);
        }

        left_layout->addWidget(presence_panel);
        left_layout->addStretch();
    }

    // ---- APPEARANCE CARD (right column — THEME-SLICE-1) ----
    {
        auto* appearance_panel = makePanel(right_col);
        appearance_panel_ = appearance_panel;
        auto* appearance_layout = new QVBoxLayout(appearance_panel);
        appearance_layout->setContentsMargins(18, 14, 18, 14);
        appearance_layout->setSpacing(0);
        appearance_layout->addWidget(makeCardTitle(QStringLiteral("Appearance"), appearance_panel));

        // Brief description
        auto* appearance_desc = new QLabel(
            QStringLiteral("Four curated themes \xE2\x80\x94 two dark, two light. Each is a complete colour set. "
                           "Status colours stay on-meaning in every theme."),
            appearance_panel);
        appearance_desc->setWordWrap(true);
        appearance_desc->setProperty("labelRole", "body");
        appearance_desc->setContentsMargins(0, 4, 0, 10);
        appearance_layout->addWidget(appearance_desc);

        // Build theme picker cards
        auto* theme_grid = new QWidget(appearance_panel);
        theme_picker_widget_ = theme_grid;
        auto* theme_grid_layout = new QVBoxLayout(theme_grid);
        theme_grid_layout->setContentsMargins(0, 0, 0, 0);
        theme_grid_layout->setSpacing(10);

        theme_button_group_ = new QButtonGroup(this);
        theme_button_group_->setExclusive(true);

        auto makeGroupLabel = [&](const QString& label) -> QLabel* {
            auto* lbl = new QLabel(label, theme_grid);
            lbl->setProperty("labelRole", "fieldLabel");
            return lbl;
        };

        auto makeThemeCard = [&](const ui::theme::ExoTheme& t) -> QPushButton* {
            auto* card = new QPushButton(theme_grid);
            card->setCheckable(true);
            card->setAutoDefault(false);
            card->setDefault(false);
            card->setCursor(Qt::PointingHandCursor);
            card->setProperty("themePickerCard", true);
            card->setProperty("themeId", QString::fromUtf8(t.id));
            card->setObjectName(QStringLiteral("themeCard_") + QString::fromUtf8(t.id));

            auto* card_layout = new QHBoxLayout(card);
            card_layout->setContentsMargins(10, 10, 10, 10);
            card_layout->setSpacing(10);

            // ThemePreviewSwatch: mini-UI preview painted from the theme's colour tokens
            // (replaces flat single-colour block — fixes the two-dark-themes-look-identical problem).
            auto* swatch = new ThemePreviewSwatch(t, card);

            // Text area
            auto* text_col = new QWidget(card);
            auto* text_layout = new QVBoxLayout(text_col);
            text_layout->setContentsMargins(0, 0, 0, 0);
            text_layout->setSpacing(2);

            auto* name_lbl = new QLabel(QString::fromUtf8(t.name), text_col);
            name_lbl->setProperty("labelRole", "settingsRowLabel");

            auto* intent_lbl = new QLabel(QString::fromUtf8(t.intent), text_col);
            intent_lbl->setProperty("labelRole", "subtle");
            intent_lbl->setWordWrap(true);

            text_layout->addWidget(name_lbl);
            text_layout->addWidget(intent_lbl);
            text_layout->addStretch();

            card_layout->addWidget(swatch, 0, Qt::AlignTop);
            card_layout->addWidget(text_col, 1);

            return card;
        };

        // Dark group
        auto* dark_label = makeGroupLabel(QStringLiteral("Dark"));
        theme_grid_layout->addWidget(dark_label);
        auto* dark_row = new QWidget(theme_grid);
        auto* dark_row_layout = new QHBoxLayout(dark_row);
        dark_row_layout->setContentsMargins(0, 0, 0, 0);
        dark_row_layout->setSpacing(8);

        // Light group
        auto* light_label = makeGroupLabel(QStringLiteral("Light"));
        auto* light_row = new QWidget(theme_grid);
        auto* light_row_layout = new QHBoxLayout(light_row);
        light_row_layout->setContentsMargins(0, 0, 0, 0);
        light_row_layout->setSpacing(8);

        int btn_id = 0;
        for (const auto& t : ui::theme::kExoThemes) {
            auto* card = makeThemeCard(t);
            theme_button_group_->addButton(card, btn_id++);
            if (QString::fromUtf8(t.group) == QStringLiteral("Dark"))
                dark_row_layout->addWidget(card);
            else
                light_row_layout->addWidget(card);
        }

        theme_grid_layout->addWidget(dark_row);
        theme_grid_layout->addSpacing(8);
        theme_grid_layout->addWidget(light_label);
        theme_grid_layout->addWidget(light_row);

        appearance_layout->addWidget(theme_grid);
        right_layout->addWidget(appearance_panel);
        right_layout->addStretch();
    }

    // ---- PS-PHASE-C: HOTKEYS CARD (full width, below the two-column grid) ----
    {
        auto* hotkeys_panel = makePanel(content);
        hotkeys_panel->setObjectName(QStringLiteral("settingsHotkeysCard"));
        auto* hotkeys_panel_layout = new QVBoxLayout(hotkeys_panel);
        hotkeys_panel_layout->setContentsMargins(18, 16, 18, 18);
        hotkeys_panel_layout->setSpacing(10);
        hotkeys_panel_layout->addWidget(makeCardTitle(QStringLiteral("Hotkeys"), hotkeys_panel));

        hotkeys_settings_panel_ = new ui::widgets::HotkeysSettingsPanel(hotkeys_panel);
        hotkeys_settings_panel_->setObjectName(QStringLiteral("settingsHotkeysPanel"));
        hotkeys_panel_layout->addWidget(hotkeys_settings_panel_);

        layout->addWidget(hotkeys_panel);
    }

    // ---- DEVELOPER CARD (Expert-gated, full width — SETTINGS-TIERS-P3) ----
    // UI-only debug stubs (log level, NVTX). No persistence — local variables only.
    {
        developer_card_ = makePanel(content);
        developer_card_->setObjectName(QStringLiteral("settingsDeveloperCard"));
        auto* dev_layout = new QVBoxLayout(developer_card_);
        dev_layout->setContentsMargins(18, 14, 18, 14);
        dev_layout->setSpacing(M::kSpaceSm);
        dev_layout->addWidget(makeCardTitle(QStringLiteral("Developer"), developer_card_));
        dev_layout->addWidget(
            makeHint(QStringLiteral("Expert debug controls — not persisted between sessions."), developer_card_));

        // Log level (UI stub — not wired to any backend)
        {
            auto* row = new QFrame(developer_card_);
            row->setProperty("panelRole", "compactRow");
            auto* rl = new QVBoxLayout(row);
            rl->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
            rl->setSpacing(M::kSpaceXs);
            rl->addWidget(makeFieldLabel(QStringLiteral("Developer logging level"), row));
            auto* log_level_combo = new QComboBox(row);
            log_level_combo->setMinimumWidth(220);
            log_level_combo->setMaximumWidth(320);
            log_level_combo->addItems({"Off", "Error", "Warning", "Info", "Debug", "Trace"});
            log_level_combo->setCurrentIndex(3);
            rl->addWidget(log_level_combo);
            dev_layout->addWidget(row);
        }

        // NVTX profiling markers (UI stub — not wired to any backend)
        {
            auto* row = new QFrame(developer_card_);
            row->setProperty("panelRole", "compactRow");
            auto* rl = new QVBoxLayout(row);
            rl->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
            rl->setSpacing(M::kSpaceXs);
            rl->addWidget(makeFieldLabel(QStringLiteral("Profiling"), row));
            auto* nvtx_check = new ui::widgets::ExoCheckBox(QStringLiteral("Enable NVTX / profiling markers"), row);
            rl->addWidget(nvtx_check);
            dev_layout->addWidget(row);
        }

        developer_card_->setVisible(expert_mode_enabled_);
        layout->addWidget(developer_card_);
    }

    layout->addStretch();

    content->setMaximumWidth(kMaxContentWidth);
    // Wave 2 Part C: ensure content never narrows past a single comfortable card column.
    content->setMinimumWidth(360);
    {
        auto* centering_host = new QWidget();
        auto* ch = new QHBoxLayout(centering_host);
        ch->setContentsMargins(0, 0, 0, 0);
        ch->addStretch(1);
        ch->addWidget(content, 0);
        ch->addStretch(1);
        scroll->setWidget(centering_host);
    }
    outer->addWidget(scroll);

    // SETTINGS-TIERS-P3: presence + appearance control connections.
    connect(overlay_check_, &ui::widgets::ExoCheckBox::toggled, this, &ConfigPage::showOverlayChanged);
    connect(diagnostics_overlay_check_, &ui::widgets::ExoCheckBox::toggled, this,
            &ConfigPage::showDiagnosticsOverlayChanged);
    connect(notifications_check_, &ui::widgets::ExoCheckBox::toggled, this, &ConfigPage::showNotificationsChanged);
    connect(keep_in_tray_check_, &ui::widgets::ExoCheckBox::toggled, this, &ConfigPage::keepRunningInTrayChanged);
    connect(quick_controls_check_, &ui::widgets::ExoCheckBox::toggled, this, &ConfigPage::showQuickControlsChanged);
    connect(theme_button_group_, &QButtonGroup::idClicked, this, [this](int btn_id) {
        auto* btn = theme_button_group_->button(btn_id);
        if (!btn)
            return;
        const QString id = btn->property("themeId").toString();
        if (!id.isEmpty()) {
            current_theme_id_ = id;
            emit themeIdChanged(id);
        }
    });

    connect(container_group_, &QButtonGroup::idClicked, this, &ConfigPage::onContainerChanged);
    connect(video_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onVideoCodecChanged);
    connect(audio_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onAudioCodecChanged);
    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onProfileSelectionChanged);
    connect(quality_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ConfigPage::onQualityChanged);
    connect(quality_segment_group_, &QButtonGroup::idClicked, this, &ConfigPage::onQualitySegmentSelected);
    connect(frame_rate_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onFrameRateChanged);
    connect(timing_group_, &QButtonGroup::idClicked, this, &ConfigPage::onTimingSelected);
    connect(output_resolution_group_, &QButtonGroup::idClicked, this, &ConfigPage::onOutputResolutionSelected);
    connect(split_mode_combo_, &QComboBox::currentIndexChanged, this, &ConfigPage::onSplitModeChanged);
    connect(split_custom_minutes_spin_, &QSpinBox::valueChanged, this, [this](int minutes) {
        format_settings_.split.custom_minutes = static_cast<uint32_t>(minutes);
        SanitizeSplitSettings(format_settings_.split);
        updateSplitSelection();
        emitCurrentFormatSettings();
    });
    connect(split_size_mode_combo_, &QComboBox::currentIndexChanged, this, &ConfigPage::onSplitSizeModeChanged);
    connect(split_custom_size_spin_, &QSpinBox::valueChanged, this, [this](int size_mb) {
        format_settings_.split.custom_size_mb = static_cast<uint32_t>(size_mb);
        SanitizeSplitSettings(format_settings_.split);
        updateSplitSizeSelection();
        emitCurrentFormatSettings();
    });
    connect(custom_width_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &ConfigPage::onCustomWidthChanged);
    connect(custom_height_spin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &ConfigPage::onCustomHeightChanged);
    connect(cursor_check_, &QAbstractButton::toggled, this, &ConfigPage::onCursorChanged);
    connect(browse_btn_, &QPushButton::clicked, this, &ConfigPage::onBrowse);
    connect(destination_edit_, &QLineEdit::editingFinished, this, &ConfigPage::onDestinationEditingFinished);
    connect(naming_edit_, &QLineEdit::editingFinished, this, &ConfigPage::onPatternEditingFinished);
    connect(app_enabled_check_, &QAbstractButton::toggled, this, &ConfigPage::onAudioAppToggled);
    connect(mic_enabled_check_, &QAbstractButton::toggled, this, &ConfigPage::onAudioMicToggled);
    connect(sys_enabled_check_, &QAbstractButton::toggled, this, &ConfigPage::onAudioSysToggled);
    // DF-12: ExoToggle inherits QAbstractButton::toggled — same connection pattern.
    connect(app_separate_check_, &QAbstractButton::toggled, this, &ConfigPage::onAudioAppSeparateToggled);
    connect(mic_separate_check_, &QAbstractButton::toggled, this, &ConfigPage::onAudioMicSeparateToggled);
    connect(sys_separate_check_, &QAbstractButton::toggled, this, &ConfigPage::onAudioSysSeparateToggled);
    connect(mic_device_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onMicDeviceChanged);
    connect(audio_rescan_btn_, &QPushButton::clicked, this, &ConfigPage::audioRescanRequested);
    connect(webcam_setup_panel_, &ui::widgets::WebcamSetupPanel::settingsChanged, this,
            [this](const WebcamSettings& settings) {
                webcam_settings_ = settings;
                emit webcamSettingsChanged(webcam_settings_);
            });
    // Preset management connections — overflow menu.
    connect(save_preset_action_, &QAction::triggered, this, &ConfigPage::onSavePreset);
    connect(save_preset_as_action_, &QAction::triggered, this, &ConfigPage::onSavePresetAs);
    connect(new_preset_action_, &QAction::triggered, this, &ConfigPage::onNewPreset);
    connect(duplicate_preset_action_, &QAction::triggered, this, &ConfigPage::onDuplicatePreset);
    connect(rename_preset_action_, &QAction::triggered, this, &ConfigPage::onRenamePreset);
    connect(delete_preset_action_, &QAction::triggered, this, &ConfigPage::onDeletePreset);
    connect(set_default_preset_action_, &QAction::triggered, this, &ConfigPage::onSetDefaultPreset);
    connect(reset_changes_action_, &QAction::triggered, this, &ConfigPage::onResetChanges);
    connect(reset_to_defaults_action_, &QAction::triggered, this, &ConfigPage::onResetToDefaults);
    connect(manage_presets_action_, &QAction::triggered, this, &ConfigPage::onManagePresets);
    // Preset management connections — primary action buttons.
    connect(preset_save_btn_, &QPushButton::clicked, this, &ConfigPage::onSavePreset);
    connect(preset_save_as_btn_, &QPushButton::clicked, this, &ConfigPage::onSavePresetAs);
    connect(view_details_btn_, &QPushButton::clicked, this, &ConfigPage::diagnosticsRequested);
    connect(token_help_toggle_btn_, &QPushButton::clicked, this, [this]() {
        const bool now_visible = !token_help_label_->isVisible();
        token_help_label_->setVisible(now_visible);
        token_help_toggle_btn_->setText(now_visible ? QStringLiteral("Hide token reference")
                                                    : QStringLiteral("Show token reference"));
    });

    // D6: ExoToggle replaces QPushButton for Expert-Mode.
    connect(expert_mode_toggle_, &QAbstractButton::clicked, this, [this]() {
        expert_mode_enabled_ = !expert_mode_enabled_;
        {
            const QSignalBlocker b(expert_mode_toggle_);
            expert_mode_toggle_->setOn(expert_mode_enabled_);
        }
        updateExpertModeVisibility();
        emit expertModeChanged(expert_mode_enabled_);
    });

    // Wave 2: output_split_expander_ dissolved; outputSplitExpanderChanged is kept as
    // a no-op signal for MainWindow compat (AppSettingsStore field still persists).

    // Wave 2 Part B: CQ spinbox — find nearest NvencQualityPreset from CQ value.
    connect(quality_cq_spin_, &QSpinBox::valueChanged, this, [this](int cq) {
        // Nearest preset: |cq-19|→High, |cq-24|→Balanced, |cq-30|→Small
        const int d_high = std::abs(cq - 19);
        const int d_balanced = std::abs(cq - 24);
        const int d_small = std::abs(cq - 30);
        if (d_high <= d_balanced && d_high <= d_small) {
            video_settings_.quality = recorder_core::NvencQualityPreset::High;
        } else if (d_balanced <= d_small) {
            video_settings_.quality = recorder_core::NvencQualityPreset::Balanced;
        } else {
            video_settings_.quality = recorder_core::NvencQualityPreset::Small;
        }
        // Sync the hidden combo so onQualityChanged path stays consistent.
        if (quality_combo_) {
            const QSignalBlocker qb(quality_combo_);
            const int idx = quality_combo_->findData(static_cast<int>(video_settings_.quality));
            if (idx >= 0)
                quality_combo_->setCurrentIndex(idx);
        }
        updateQualitySegmentSelection();
        updateQualitySummary();
        emitCurrentVideoSettings();
    });

    // PS-PHASE-C: Rate control segmented — updates video_settings_.rate_control.
    connect(rate_control_group_, &QButtonGroup::idClicked, this, [this](int id) {
        video_settings_.rate_control = static_cast<recorder_core::RateControlMode>(id);
        const bool rate_is_cq = (video_settings_.rate_control == recorder_core::RateControlMode::ConstantQuality);
        const bool needs_bitrate = !rate_is_cq;
        if (quality_expert_widget_)
            quality_expert_widget_->setVisible(rate_is_cq);
        if (bitrate_row_widget_)
            bitrate_row_widget_->setVisible(needs_bitrate);
        emitCurrentVideoSettings();
    });

    // PS-PHASE-C: Bitrate spinbox.
    connect(bitrate_kbps_spin_, &QSpinBox::valueChanged, this, [this](int kbps) {
        video_settings_.bitrate_kbps = static_cast<uint32_t>(kbps);
        emitCurrentVideoSettings();
    });

    // PS-PHASE-C: Audio expert controls (Polish-R1: slider replaces spinbox for mic gain).
    connect(mic_gain_slider_, &QSlider::valueChanged, this, [this](int db) {
        if (mic_gain_db_label_)
            mic_gain_db_label_->setText(QStringLiteral("%1 dB").arg(db));
        audio_ui_state_.mic_gain_linear = std::powf(10.f, static_cast<float>(db) / 20.f);
        emitCurrentAudioSettings();
    });
    connect(mic_channel_mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx < 0)
            return;
        audio_ui_state_.mic_channel_mode =
            static_cast<recorder_core::MicChannelMode>(mic_channel_mode_combo_->itemData(idx).toInt());
        emitCurrentAudioSettings();
    });
    connect(audio_bitrate_kbps_spin_, &QSpinBox::valueChanged, this, [this](int kbps) {
        audio_ui_state_.audio_bitrate_kbps = static_cast<uint32_t>(kbps);
        emitCurrentAudioSettings();
    });
    connect(opus_frame_duration_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx < 0)
            return;
        audio_ui_state_.opus_frame_duration =
            static_cast<recorder_core::OpusFrameDuration>(opus_frame_duration_combo_->itemData(idx).toInt());
        emitCurrentAudioSettings();
    });
    connect(opus_complexity_spin_, &QSpinBox::valueChanged, this, [this](int val) {
        audio_ui_state_.opus_complexity = val;
        emitCurrentAudioSettings();
    });
    connect(limiter_check_, &ui::widgets::ExoCheckBox::toggled, this, [this](bool on) {
        audio_ui_state_.limiter_enabled = on;
        if (limiter_ceiling_spin_)
            limiter_ceiling_spin_->setEnabled(on);
        emitCurrentAudioSettings();
    });
    connect(limiter_ceiling_spin_, &QDoubleSpinBox::valueChanged, this, [this](double db) {
        audio_ui_state_.limiter_ceiling_db = static_cast<float>(db);
        emitCurrentAudioSettings();
    });
    connect(mic_hpf_check_, &ui::widgets::ExoCheckBox::toggled, this, [this](bool on) {
        audio_ui_state_.mic_hpf_enabled = on;
        if (mic_hpf_cutoff_spin_)
            mic_hpf_cutoff_spin_->setEnabled(on);
        emitCurrentAudioSettings();
    });
    connect(mic_hpf_cutoff_spin_, &QDoubleSpinBox::valueChanged, this, [this](double hz) {
        audio_ui_state_.mic_hpf_cutoff_hz = static_cast<float>(hz);
        emitCurrentAudioSettings();
    });
    connect(mic_gate_check_, &ui::widgets::ExoCheckBox::toggled, this, [this](bool on) {
        audio_ui_state_.mic_gate_enabled = on;
        if (mic_gate_threshold_spin_)
            mic_gate_threshold_spin_->setEnabled(on);
        emitCurrentAudioSettings();
    });
    connect(mic_gate_threshold_spin_, &QDoubleSpinBox::valueChanged, this, [this](double db) {
        audio_ui_state_.mic_gate_threshold_db = static_cast<float>(db);
        emitCurrentAudioSettings();
    });
    connect(mic_agc_check_, &ui::widgets::ExoCheckBox::toggled, this, [this](bool on) {
        audio_ui_state_.mic_agc_enabled = on;
        if (mic_agc_target_spin_)
            mic_agc_target_spin_->setEnabled(on);
        emitCurrentAudioSettings();
    });
    connect(mic_agc_target_spin_, &QDoubleSpinBox::valueChanged, this, [this](double db) {
        audio_ui_state_.mic_agc_target_db = static_cast<float>(db);
        emitCurrentAudioSettings();
    });

    // SETTINGS-SEARCH-R1: live search filter wired to textChanged.
    connect(settings_search_box_, &QLineEdit::textChanged, this, &ConfigPage::applySettingsSearch);

    // Search pill focus indicator: install an event filter on the QLineEdit so we
    // can set a "focused" dynamic property on the pill when focus enters/leaves.
    // (Qt QSS does not support :focus-within; a dynamic property + repolish is the
    // standard Qt workaround.)
    settings_search_box_->installEventFilter(this);

    // audio_separate_expander_ is null (removed in Phase 1b); audioSeparateExpanderChanged
    // is kept in the header for backward compatibility but is never emitted.

    // Prevent accidental value changes when the mouse wheel scrolls the (long) Config
    // page while the cursor happens to be over a combo box. The filter forwards the
    // wheel event to the scroll area instead of changing the combo selection.
    auto* combo_wheel_filter = new ui::widgets::ComboBoxWheelFilter(this);
    combo_wheel_filter->installOn(profile_combo_);
    combo_wheel_filter->installOn(video_codec_combo_);
    combo_wheel_filter->installOn(audio_codec_combo_);
    combo_wheel_filter->installOn(quality_combo_);
    combo_wheel_filter->installOn(frame_rate_combo_);
    combo_wheel_filter->installOn(mic_device_combo_);

    // D6: CompareHint → format change connections.
    connect(container_compare_hint_, &ui::widgets::CompareHint::optionSelected, this, [this](const QString& v) {
        if (v == QLatin1String("MKV"))
            onContainerChanged(static_cast<int>(capability::Container::Matroska));
        else if (v == QLatin1String("WebM"))
            onContainerChanged(static_cast<int>(capability::Container::WebM));
        else if (v == QLatin1String("MP4"))
            onContainerChanged(static_cast<int>(capability::Container::Mp4));
    });
    connect(video_codec_compare_hint_, &ui::widgets::CompareHint::optionSelected, this, [this](const QString& v) {
        if (v == QLatin1String("AV1")) {
            const QSignalBlocker b(video_codec_combo_);
            const int idx = video_codec_combo_->findData(VideoCodecToInt(capability::VideoCodec::Av1Nvenc));
            if (idx >= 0) {
                video_codec_combo_->setCurrentIndex(idx);
                onVideoCodecChanged(idx);
            }
        } else if (v == QLatin1String("H.264")) {
            const QSignalBlocker b(video_codec_combo_);
            const int idx = video_codec_combo_->findData(VideoCodecToInt(capability::VideoCodec::H264Nvenc));
            if (idx >= 0) {
                video_codec_combo_->setCurrentIndex(idx);
                onVideoCodecChanged(idx);
            }
        }
        // HEVC → no-op
    });
    connect(audio_codec_compare_hint_, &ui::widgets::CompareHint::optionSelected, this, [this](const QString& v) {
        if (v == QLatin1String("Opus")) {
            const QSignalBlocker b(audio_codec_combo_);
            const int idx = audio_codec_combo_->findData(AudioCodecToInt(capability::AudioCodec::Opus));
            if (idx >= 0) {
                audio_codec_combo_->setCurrentIndex(idx);
                onAudioCodecChanged(idx);
            }
        } else if (v == QLatin1String("AAC")) {
            const QSignalBlocker b(audio_codec_combo_);
            const int idx = audio_codec_combo_->findData(AudioCodecToInt(capability::AudioCodec::AacMf));
            if (idx >= 0) {
                audio_codec_combo_->setCurrentIndex(idx);
                onAudioCodecChanged(idx);
            }
        }
        // PCM/FLAC → no-op
    });
    connect(timing_compare_hint_, &ui::widgets::CompareHint::optionSelected, this, [this](const QString& v) {
        if (v == QLatin1String("CFR"))
            onTimingSelected(1);
        else if (v == QLatin1String("VFR"))
            onTimingSelected(0);
    });
    // D6 Task C: resolution CompareHint → output resolution selection.
    // Options: "Native" (Native), "1080p" (FHD1080), "720p" (HD720), "Custom" (Custom).
    // 4K and 1440p are intentionally not in the compare data; they fall through as no-op.
    connect(resolution_compare_hint_, &ui::widgets::CompareHint::optionSelected, this, [this](const QString& v) {
        if (v == QLatin1String("Native"))
            onOutputResolutionSelected(static_cast<int>(OutputResolutionMode::Native));
        else if (v == QLatin1String("1080p"))
            onOutputResolutionSelected(static_cast<int>(OutputResolutionMode::FHD1080));
        else if (v == QLatin1String("720p"))
            onOutputResolutionSelected(static_cast<int>(OutputResolutionMode::HD720));
        else if (v == QLatin1String("Custom"))
            onOutputResolutionSelected(static_cast<int>(OutputResolutionMode::Custom));
        // 4K / 1440p: no-op (not in compare data)
    });

    setReadinessStatus(QStringLiteral("CHECKING"));

    {
        const QSignalBlocker dd(destination_edit_);
        destination_edit_->setText(QString::fromStdWString(format_settings_.output_folder.wstring()));
    }
    {
        const QSignalBlocker np(naming_edit_);
        naming_edit_->setText(QString::fromStdWString(format_settings_.naming_pattern));
    }
    applyAudioConfigurationState();
    updateFormatDisplay();
    updateExampleFilename();
    updateQualitySummary();
    updateFrameRateSelection();
    updateTimingSelection();
    updateOutputResolutionSelection();
    updateEffectiveOutputSummary();
    updateResponsiveLayout();

    QPointer<ConfigPage> safe = this;
    QTimer::singleShot(0, this, [safe]() {
        if (safe)
            safe->refreshMicDevices();
    });
}

void ConfigPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

bool ConfigPage::eventFilter(QObject* watched, QEvent* event) {
    if (watched == settings_search_box_ && settings_search_pill_) {
        const auto t = event->type();
        if (t == QEvent::FocusIn || t == QEvent::FocusOut) {
            const bool focused = (t == QEvent::FocusIn);
            settings_search_pill_->setProperty("focused", focused);
            settings_search_pill_->style()->unpolish(settings_search_pill_);
            settings_search_pill_->style()->polish(settings_search_pill_);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ConfigPage::updateResponsiveLayout() {
    // D6 wave-2 responsive: raised threshold to kColumnBreakThreshold (1280) so
    // two-column view only activates when there is comfortably enough room for
    // both cards without overflow.  At the new minimum window width (860) and at
    // the old minimum (1120) the layout is always single-column.
    const bool narrow = width() < kColumnBreakThreshold;
    const QBoxLayout::Direction desired = narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;

    if (columns_layout_ && columns_layout_->direction() != desired)
        columns_layout_->setDirection(desired);
    if (output_split_layout_ && output_split_layout_->direction() != desired)
        output_split_layout_->setDirection(desired);
}

void ConfigPage::emitCurrentFormatSettings() {
    if (destination_edit_) {
        const auto folder_normalized = NormalizeOutputFolderInput(destination_edit_->text().toStdWString());
        if (folder_normalized.result == OutputFolderPolicyResult::Ok) {
            format_settings_.output_folder = folder_normalized.resolved_path;
        }
    }
    if (naming_edit_) {
        const auto pattern_normalized = NormalizeFilenamePatternInput(naming_edit_->text().toStdWString());
        if (pattern_normalized.result == FilenamePatternPolicyResult::Ok) {
            format_settings_.naming_pattern = pattern_normalized.normalized_pattern;
        }
    }
    const bool cfr_before = video_settings_.cfr;
    reconcileContainerCodecRules();
    updateFormatDisplay();
    updateTimingSelection();
    updateSplitSelection();
    updateEffectiveOutputSummary();
    updateOutputValidationState();
    updateExampleFilename();
    emit formatSettingsChanged(format_settings_);
    if (cfr_before != video_settings_.cfr) {
        emitCurrentVideoSettings();
    }
}

void ConfigPage::emitCurrentVideoSettings() {
    emit videoSettingsChanged(video_settings_);
}

void ConfigPage::onQualityChanged(int index) {
    if (index < 0)
        return;
    video_settings_.quality = static_cast<recorder_core::NvencQualityPreset>(quality_combo_->itemData(index).toInt());
    updateQualitySummary();
    emitCurrentVideoSettings();
}

void ConfigPage::onQualitySegmentSelected(int preset_id) {
    if (!quality_combo_)
        return;

    const int idx = quality_combo_->findData(preset_id);
    if (idx < 0)
        return;
    if (quality_combo_->currentIndex() == idx) {
        updateQualitySegmentSelection();
        return;
    }
    quality_combo_->setCurrentIndex(idx);
}

void ConfigPage::onFrameRateChanged(int index) {
    if (index < 0)
        return;
    const int fps = frame_rate_combo_->itemData(index).toInt();
    if (fps <= 0 || fps == 120)
        return;
    video_settings_.frame_rate_num = static_cast<uint32_t>(fps);
    video_settings_.frame_rate_den = 1;
    updateQualitySummary();
    updateEffectiveOutputSummary();
    emitCurrentVideoSettings();
}

void ConfigPage::onTimingSelected(int timing_id) {
    const bool want_cfr = timing_id != 0;
    if (!want_cfr && format_settings_.container == capability::Container::Mp4) {
        video_settings_.cfr = true;
    } else {
        video_settings_.cfr = want_cfr;
    }
    updateTimingSelection();
    updateQualitySummary();
    updateEffectiveOutputSummary();
    emitCurrentVideoSettings();
}

void ConfigPage::onOutputResolutionSelected(int mode_id) {
    const auto new_mode = static_cast<OutputResolutionMode>(mode_id);
    const auto old_mode = format_settings_.resolution.mode;

    if (old_mode == OutputResolutionMode::Custom && new_mode != OutputResolutionMode::Custom) {
        stashed_custom_width_ = format_settings_.resolution.custom_width;
        stashed_custom_height_ = format_settings_.resolution.custom_height;
    }

    if (new_mode == OutputResolutionMode::Custom) {
        if (stashed_custom_width_ > 0 || stashed_custom_height_ > 0) {
            format_settings_.resolution.custom_width = stashed_custom_width_;
            format_settings_.resolution.custom_height = stashed_custom_height_;
        } else {
            format_settings_.resolution.custom_width = 1920;
            format_settings_.resolution.custom_height = 1080;
        }
    }

    format_settings_.resolution.mode = new_mode;
    SanitizeOutputResolution(format_settings_.resolution);

    updateCustomResolutionVisibility();
    updateOutputResolutionSelection();
    emitCurrentFormatSettings();
}

void ConfigPage::onSplitModeChanged(int index) {
    if (!split_mode_combo_)
        return;
    const auto mode = static_cast<SplitRecordingMode>(split_mode_combo_->itemData(index).toInt());
    format_settings_.split.mode = mode;
    SanitizeSplitSettings(format_settings_.split);
    updateSplitSelection();
    emitCurrentFormatSettings();
}

void ConfigPage::updateSplitSelection() {
    if (!split_mode_combo_)
        return;
    const SplitRecordingSettings& s = format_settings_.split;
    // MP4 cannot produce segmented output (IMF Sink Writer); the configured split
    // mode is preserved untouched so switching back to MKV/WebM restores it.
    const bool split_supported = format_settings_.container != capability::Container::Mp4;
    {
        QSignalBlocker block_combo(split_mode_combo_);
        const int idx = split_mode_combo_->findData(static_cast<int>(s.mode));
        if (idx >= 0)
            split_mode_combo_->setCurrentIndex(idx);
    }
    if (split_custom_minutes_spin_) {
        QSignalBlocker block_spin(split_custom_minutes_spin_);
        split_custom_minutes_spin_->setValue(static_cast<int>(s.custom_minutes));
    }
    split_mode_combo_->setEnabled(split_supported);
    if (split_custom_minutes_spin_)
        split_custom_minutes_spin_->setEnabled(split_supported);
    if (split_custom_widget_)
        split_custom_widget_->setVisible(split_supported && s.mode == SplitRecordingMode::Custom);

    if (split_summary_label_) {
        if (!split_supported) {
            split_summary_label_->setText(
                QStringLiteral("Automatic split is available for MKV/WebM. MP4 records a single file."));
        } else if (s.mode == SplitRecordingMode::Off) {
            split_summary_label_->setText(QStringLiteral("Single file (split off). Manual splits still work."));
        } else {
            const uint64_t ms = SplitDurationMs(s);
            const uint64_t minutes = ms / 60000ull;
            split_summary_label_->setText(
                QStringLiteral("New file automatically every %1 min.").arg(static_cast<qulonglong>(minutes)));
        }
    }
    // Also keep size controls in sync.
    updateSplitSizeSelection();
}

void ConfigPage::onSplitSizeModeChanged(int index) {
    if (!split_size_mode_combo_)
        return;
    const auto mode = static_cast<SplitSizeMode>(split_size_mode_combo_->itemData(index).toInt());
    format_settings_.split.size_mode = mode;
    SanitizeSplitSettings(format_settings_.split);
    updateSplitSizeSelection();
    emitCurrentFormatSettings();
}

void ConfigPage::updateSplitSizeSelection() {
    if (!split_size_mode_combo_)
        return;
    const SplitRecordingSettings& s = format_settings_.split;
    const bool split_supported = format_settings_.container != capability::Container::Mp4;
    {
        QSignalBlocker block_combo(split_size_mode_combo_);
        const int idx = split_size_mode_combo_->findData(static_cast<int>(s.size_mode));
        if (idx >= 0)
            split_size_mode_combo_->setCurrentIndex(idx);
    }
    if (split_custom_size_spin_) {
        QSignalBlocker block_spin(split_custom_size_spin_);
        split_custom_size_spin_->setValue(static_cast<int>(s.custom_size_mb));
    }
    split_size_mode_combo_->setEnabled(split_supported);
    if (split_custom_size_spin_)
        split_custom_size_spin_->setEnabled(split_supported);
    if (split_size_custom_widget_)
        split_size_custom_widget_->setVisible(split_supported && s.size_mode == SplitSizeMode::Custom);
}

void ConfigPage::onCursorChanged() {
    video_settings_.capture_cursor = cursor_check_->isChecked();
    updateQualitySummary();
    emitCurrentVideoSettings();
}

void ConfigPage::reconcileContainerCodecRules() {
    ReconcileContainerCodecs(format_settings_);
    SanitizeOutputResolution(format_settings_.resolution);
    if (format_settings_.container == capability::Container::Mp4) {
        video_settings_.cfr = true;
    }
    updateVideoCodecChoices();
    updateAudioCodecChoices();
}

void ConfigPage::updateVideoCodecChoices() {
    const QSignalBlocker blocker(video_codec_combo_);
    if (video_codec_combo_->count() == 0) {
        video_codec_combo_->addItem(QStringLiteral("AV1"), VideoCodecToInt(capability::VideoCodec::Av1Nvenc));
        video_codec_combo_->addItem(QStringLiteral("H.264"), VideoCodecToInt(capability::VideoCodec::H264Nvenc));
    }
    const int vidx = video_codec_combo_->findData(VideoCodecToInt(format_settings_.video_codec));
    if (vidx >= 0)
        video_codec_combo_->setCurrentIndex(vidx);
}

void ConfigPage::updateAudioCodecChoices() {
    const QSignalBlocker blocker(audio_codec_combo_);
    // Rebuild the list so the lossless codecs (PCM + FLAC, MKV-only) appear only
    // when MKV is selected. The common Opus/AAC entries are always present; PCM
    // and FLAC are appended for Matroska.
    const bool mkv = (format_settings_.container == capability::Container::Matroska);
    const int desired = mkv ? 4 : 2;
    const bool has_pcm = audio_codec_combo_->findData(AudioCodecToInt(capability::AudioCodec::Pcm)) >= 0;
    if (audio_codec_combo_->count() != desired || (mkv && !has_pcm) || (!mkv && has_pcm)) {
        audio_codec_combo_->clear();
        audio_codec_combo_->addItem(QStringLiteral("Opus"), AudioCodecToInt(capability::AudioCodec::Opus));
        audio_codec_combo_->addItem(QStringLiteral("AAC"), AudioCodecToInt(capability::AudioCodec::AacMf));
        if (mkv) {
            audio_codec_combo_->addItem(QStringLiteral("PCM (uncompressed)"),
                                        AudioCodecToInt(capability::AudioCodec::Pcm));
            audio_codec_combo_->addItem(QStringLiteral("FLAC (lossless)"),
                                        AudioCodecToInt(capability::AudioCodec::Flac));
        }
    }
    const int aidx = audio_codec_combo_->findData(AudioCodecToInt(format_settings_.audio_codec));
    if (aidx >= 0)
        audio_codec_combo_->setCurrentIndex(aidx);
}

void ConfigPage::updateFormatDisplay() {
    updateCompatCallout();
}

void ConfigPage::updateCompatCallout() {
    const auto c = format_settings_.container;
    const auto v = format_settings_.video_codec;
    const auto a = format_settings_.audio_codec;
    bool video_bad = false, audio_bad = false;
    if (c == capability::Container::WebM) {
        video_bad = (v != capability::VideoCodec::Av1Nvenc);
        audio_bad = (a != capability::AudioCodec::Opus);
    } else if (c == capability::Container::Mp4) {
        video_bad = (v != capability::VideoCodec::H264Nvenc);
        audio_bad = (a != capability::AudioCodec::AacMf);
    }
    const bool compat_bad = video_bad || audio_bad;

    const QString summary = ContainerLabel(c) + QStringLiteral(" \xC2\xB7 ") + VideoCodecLabel(v) +
                            QStringLiteral(" \xC2\xB7 ") + AudioCodecLabel(a) + QStringLiteral(" \xC2\xB7 ") +
                            FrameRateLabel(video_settings_.frame_rate_num, video_settings_.frame_rate_den) +
                            QStringLiteral(" \xC2\xB7 ") +
                            (video_settings_.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR"));

    if (format_display_label_) {
        format_display_label_->setText(QStringLiteral("Current format: ") + summary);
    }

    if (compat_callout_widget_)
        compat_callout_widget_->setVisible(compat_bad);
    if (compat_ok_label_)
        compat_ok_label_->setVisible(!compat_bad);

    if (compat_bad && callout_text_) {
        QStringList bad_parts;
        if (video_bad)
            bad_parts << VideoCodecLabel(v);
        if (audio_bad)
            bad_parts << AudioCodecLabel(a);
        const QString bad_str = bad_parts.join(QStringLiteral(" + "));
        QString hint;
        if (c == capability::Container::WebM)
            hint = QStringLiteral("WebM supports AV1 + Opus only.");
        else if (c == capability::Container::Mp4)
            hint = QStringLiteral("MP4 supports H.264 + AAC only.");
        callout_text_->setText(ContainerLabel(c) + QStringLiteral(" can't hold ") + bad_str + QStringLiteral(". ") +
                               hint);
    }
    if (!compat_bad && compat_ok_label_) {
        compat_ok_label_->setText(QStringLiteral("\xe2\x9c\x93 Current format: ") + summary);
    }

    // Container segment sync (was in updateFormatDisplay)
    const auto sync_container = [this](QPushButton* segment, capability::Container container) {
        if (!segment)
            return;
        const bool selected = format_settings_.container == container;
        segment->setChecked(selected);
        segment->setProperty("qualitySegmentSelected", selected);
        segment->style()->unpolish(segment);
        segment->style()->polish(segment);
    };
    const QSignalBlocker blocker(container_group_);
    sync_container(mkv_radio_, capability::Container::Matroska);
    sync_container(webm_radio_, capability::Container::WebM);
    sync_container(mp4_radio_, capability::Container::Mp4);

    // CompareHint value sync
    if (container_compare_hint_)
        container_compare_hint_->setCurrentValue(ContainerLabel(format_settings_.container));
    if (video_codec_compare_hint_)
        video_codec_compare_hint_->setCurrentValue(VideoCodecLabel(format_settings_.video_codec));
    if (audio_codec_compare_hint_)
        audio_codec_compare_hint_->setCurrentValue(AudioCodecLabel(format_settings_.audio_codec));
}

void ConfigPage::onContainerChanged(int id) {
    format_settings_.container = static_cast<capability::Container>(id);
    // PCM and FLAC are MKV-only: rebuild the audio-codec list so the options
    // appear/disappear with the container. If a lossless codec was selected and
    // we leave MKV, fall back to Opus.
    if (format_settings_.container != capability::Container::Matroska &&
        (format_settings_.audio_codec == capability::AudioCodec::Pcm ||
         format_settings_.audio_codec == capability::AudioCodec::Flac)) {
        format_settings_.audio_codec = capability::AudioCodec::Opus;
    }
    updateAudioCodecChoices();
    emitCurrentFormatSettings();
}

void ConfigPage::onVideoCodecChanged(int index) {
    if (index < 0)
        return;
    format_settings_.video_codec = IntToVideoCodec(video_codec_combo_->itemData(index).toInt());
    emitCurrentFormatSettings();
}

void ConfigPage::onAudioCodecChanged(int index) {
    if (index < 0)
        return;
    format_settings_.audio_codec = IntToAudioCodec(audio_codec_combo_->itemData(index).toInt());
    emitCurrentFormatSettings();
}

void ConfigPage::onProfileSelectionChanged(int index) {
    if (index < 0 || index >= static_cast<int>(profile_options_.size()))
        return;
    const auto& opt = profile_options_[static_cast<std::size_t>(index)];
    active_preset_is_built_in_ = opt.built_in;
    active_preset_is_available_ = opt.available;
    active_preset_id_ = opt.id;
    updatePresetActionState();
    emit presetSelected(opt.id);
}

void ConfigPage::setOutputSettings(const OutputSettingsModel& settings) {
    format_settings_.container = settings.container;
    format_settings_.video_codec = settings.video_codec;
    format_settings_.audio_codec = settings.audio_codec;
    format_settings_.output_folder = settings.output_folder;
    format_settings_.naming_pattern = settings.naming_pattern;
    format_settings_.resolution = settings.resolution;
    format_settings_.split = settings.split;
    SanitizeOutputResolution(format_settings_.resolution);
    SanitizeSplitSettings(format_settings_.split);
    if (settings.resolution.mode == OutputResolutionMode::Custom) {
        stashed_custom_width_ = settings.resolution.custom_width;
        stashed_custom_height_ = settings.resolution.custom_height;
    }

    const QSignalBlocker blocker(container_group_);
    if (settings.container == capability::Container::Matroska)
        mkv_radio_->setChecked(true);
    else if (settings.container == capability::Container::WebM)
        webm_radio_->setChecked(true);
    else
        mp4_radio_->setChecked(true);

    updateVideoCodecChoices();
    updateAudioCodecChoices();
    updateFormatDisplay();
    updateOutputResolutionSelection();
    updateCustomResolutionVisibility();
    updateEffectiveOutputSummary();
    updateSplitSelection();

    if (destination_edit_) {
        const QSignalBlocker db(destination_edit_);
        destination_edit_->setText(QString::fromStdWString(settings.output_folder.wstring()));
    }
    if (naming_edit_) {
        const QSignalBlocker nb(naming_edit_);
        naming_edit_->setText(QString::fromStdWString(settings.naming_pattern));
    }
    updateOutputValidationState();
    updateExampleFilename();
}

void ConfigPage::setVideoSettings(const VideoSettingsModel& settings) {
    video_settings_ = settings;

    const QSignalBlocker qb(quality_combo_);
    const int qidx = quality_combo_->findData(static_cast<int>(settings.quality));
    if (qidx >= 0)
        quality_combo_->setCurrentIndex(qidx);

    // Wave 2 Part B: sync CQ spinbox from loaded preset.
    if (quality_cq_spin_) {
        const QSignalBlocker sb(quality_cq_spin_);
        switch (settings.quality) {
        case recorder_core::NvencQualityPreset::High:
            quality_cq_spin_->setValue(19);
            break;
        case recorder_core::NvencQualityPreset::Small:
            quality_cq_spin_->setValue(30);
            break;
        default: // Balanced
            quality_cq_spin_->setValue(24);
            break;
        }
    }

    updateFrameRateSelection();
    updateTimingSelection();

    const QSignalBlocker crb(cursor_check_);
    cursor_check_->setChecked(settings.capture_cursor);

    // PS-PHASE-C: sync expert rate control + bitrate from loaded preset.
    if (rate_control_group_) {
        auto* btn = rate_control_group_->button(static_cast<int>(settings.rate_control));
        if (btn) {
            const QSignalBlocker rb(rate_control_group_);
            btn->setChecked(true);
        }
    }
    if (bitrate_kbps_spin_) {
        const QSignalBlocker bb(bitrate_kbps_spin_);
        bitrate_kbps_spin_->setValue(static_cast<int>(settings.bitrate_kbps));
    }
    if (bitrate_row_widget_) {
        const bool needs_bitrate = (settings.rate_control == recorder_core::RateControlMode::VariableBitrate ||
                                    settings.rate_control == recorder_core::RateControlMode::ConstantBitrate);
        bitrate_row_widget_->setVisible(expert_mode_enabled_ && needs_bitrate);
    }

    updateQualitySummary();
    updateEffectiveOutputSummary();
}

void ConfigPage::updateQualitySummary() {
    if (!quality_badge_label_ || !quality_settings_label_)
        return;

    switch (video_settings_.quality) {
    case recorder_core::NvencQualityPreset::High:
        quality_badge_label_->setText(QStringLiteral("Sharper · larger files"));
        break;
    case recorder_core::NvencQualityPreset::Balanced:
        quality_badge_label_->setText(QStringLiteral("General purpose"));
        break;
    case recorder_core::NvencQualityPreset::Small:
        quality_badge_label_->setText(QStringLiteral("Smaller files"));
        break;
    }

    const QString cq = [](recorder_core::NvencQualityPreset p) -> QString {
        switch (p) {
        case recorder_core::NvencQualityPreset::High:
            return QStringLiteral("CQ 19");
        case recorder_core::NvencQualityPreset::Balanced:
            return QStringLiteral("CQ 24");
        case recorder_core::NvencQualityPreset::Small:
            return QStringLiteral("CQ 30");
        }
        return QStringLiteral("CQ 24");
    }(video_settings_.quality);

    const QString cfr_text = (video_settings_.cfr ? QStringLiteral("CFR ") : QStringLiteral("VFR ")) +
                             FrameRateLabel(video_settings_.frame_rate_num, video_settings_.frame_rate_den);
    const QString cursor_text =
        video_settings_.capture_cursor ? QStringLiteral("Cursor on") : QStringLiteral("Cursor off");
    quality_settings_label_->setText(cq + QStringLiteral(" · ") + cfr_text + QStringLiteral(" · ") + cursor_text);

    if (quality_compare_hint_) {
        switch (video_settings_.quality) {
        case recorder_core::NvencQualityPreset::High:
            quality_compare_hint_->setCurrentValue(QStringLiteral("High"));
            break;
        case recorder_core::NvencQualityPreset::Balanced:
            quality_compare_hint_->setCurrentValue(QStringLiteral("Balanced"));
            break;
        case recorder_core::NvencQualityPreset::Small:
            quality_compare_hint_->setCurrentValue(QStringLiteral("Small"));
            break;
        }
    }

    // Wave 2 Part B: keep CQ spinbox in sync with model (under blocker to avoid feedback loop).
    if (quality_cq_spin_) {
        const QSignalBlocker b(quality_cq_spin_);
        switch (video_settings_.quality) {
        case recorder_core::NvencQualityPreset::High:
            quality_cq_spin_->setValue(19);
            break;
        case recorder_core::NvencQualityPreset::Small:
            quality_cq_spin_->setValue(30);
            break;
        default: // Balanced
            quality_cq_spin_->setValue(24);
            break;
        }
    }

    updateQualitySegmentSelection();
}

void ConfigPage::updateQualitySegmentSelection() {
    if (!quality_segment_group_)
        return;

    const auto sync_segment = [this](QPushButton* segment, recorder_core::NvencQualityPreset preset) {
        if (!segment)
            return;

        const bool selected = video_settings_.quality == preset;
        segment->setChecked(selected);
        segment->setProperty("qualitySegmentSelected", selected);
        segment->style()->unpolish(segment);
        segment->style()->polish(segment);
    };

    const QSignalBlocker blocker(quality_segment_group_);
    sync_segment(quality_segment_small_, recorder_core::NvencQualityPreset::Small);
    sync_segment(quality_segment_balanced_, recorder_core::NvencQualityPreset::Balanced);
    sync_segment(quality_segment_high_, recorder_core::NvencQualityPreset::High);
}

void ConfigPage::updateFrameRateSelection() {
    if (!frame_rate_combo_)
        return;

    const QSignalBlocker blocker(frame_rate_combo_);
    const int idx = frame_rate_combo_->findData(static_cast<int>(video_settings_.frame_rate_num));
    if (idx >= 0 && video_settings_.frame_rate_num != 120) {
        frame_rate_combo_->setCurrentIndex(idx);
    }
}

void ConfigPage::updateTimingSelection() {
    if (!timing_group_)
        return;

    const bool vfr_available = format_settings_.container != capability::Container::Mp4;
    if (!vfr_available && !video_settings_.cfr) {
        video_settings_.cfr = true;
    }

    auto sync_segment = [this](QPushButton* segment, bool selected, bool enabled, const QString& unavailable_reason) {
        if (!segment)
            return;
        segment->setChecked(selected);
        segment->setEnabled(enabled && !controls_locked_);
        segment->setToolTip(enabled ? QString() : unavailable_reason);
        segment->setProperty("qualitySegmentSelected", selected);
        segment->style()->unpolish(segment);
        segment->style()->polish(segment);
    };

    const QSignalBlocker blocker(timing_group_);
    sync_segment(timing_cfr_btn_, video_settings_.cfr, true, QString());
    sync_segment(timing_vfr_btn_, !video_settings_.cfr, vfr_available,
                 QStringLiteral("VFR is not available for MP4 in the current mux path."));

    if (timing_compare_hint_)
        timing_compare_hint_->setCurrentValue(video_settings_.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR"));
}

void ConfigPage::updateOutputResolutionSelection() {
    if (!output_resolution_group_)
        return;

    auto sync_segment = [this](QPushButton* segment, OutputResolutionMode mode) {
        if (!segment)
            return;
        const bool selected = format_settings_.resolution.mode == mode;
        segment->setChecked(selected);
        segment->setProperty("qualitySegmentSelected", selected);
        segment->style()->unpolish(segment);
        segment->style()->polish(segment);
    };

    const QSignalBlocker blocker(output_resolution_group_);
    sync_segment(output_res_native_btn_, OutputResolutionMode::Native);
    sync_segment(output_res_4k_btn_, OutputResolutionMode::UHD2160);
    sync_segment(output_res_1440_btn_, OutputResolutionMode::QHD1440);
    sync_segment(output_res_1080_btn_, OutputResolutionMode::FHD1080);
    sync_segment(output_res_720_btn_, OutputResolutionMode::HD720);
    sync_segment(output_res_custom_btn_, OutputResolutionMode::Custom);

    // D6 Task C: keep resolution CompareHint highlighted row in sync.
    if (resolution_compare_hint_) {
        const auto mode = format_settings_.resolution.mode;
        QString label;
        switch (mode) {
        case OutputResolutionMode::Native:
            label = QStringLiteral("Native");
            break;
        case OutputResolutionMode::FHD1080:
            label = QStringLiteral("1080p");
            break;
        case OutputResolutionMode::HD720:
            label = QStringLiteral("720p");
            break;
        case OutputResolutionMode::Custom:
            label = QStringLiteral("Custom");
            break;
        default:
            label = QStringLiteral("Native");
            break;
        }
        resolution_compare_hint_->setCurrentValue(label);
    }
}

void ConfigPage::updateCustomResolutionVisibility() {
    if (!custom_resolution_widget_)
        return;
    const bool is_custom = format_settings_.resolution.mode == OutputResolutionMode::Custom;
    custom_resolution_widget_->setVisible(is_custom);

    if (is_custom && custom_width_spin_ && custom_height_spin_) {
        const QSignalBlocker wb(custom_width_spin_);
        const QSignalBlocker hb(custom_height_spin_);
        const int w = static_cast<int>(format_settings_.resolution.custom_width);
        const int h = static_cast<int>(format_settings_.resolution.custom_height);
        if (w >= custom_width_spin_->minimum() && w <= custom_width_spin_->maximum())
            custom_width_spin_->setValue(w);
        if (h >= custom_height_spin_->minimum() && h <= custom_height_spin_->maximum())
            custom_height_spin_->setValue(h);
    }

    updateCustomResolutionValidation();
}

void ConfigPage::updateCustomResolutionValidation() {
    if (!custom_resolution_validation_label_)
        return;
    if (format_settings_.resolution.mode != OutputResolutionMode::Custom) {
        custom_resolution_validation_label_->clear();
        custom_resolution_validation_label_->setVisible(false);
        return;
    }

    const uint32_t w = format_settings_.resolution.custom_width;
    const uint32_t h = format_settings_.resolution.custom_height;

    if (w < 320) {
        custom_resolution_validation_label_->setText(QStringLiteral("Width must be at least 320 px."));
        custom_resolution_validation_label_->setVisible(true);
    } else if (w > 7680) {
        custom_resolution_validation_label_->setText(QStringLiteral("Width must not exceed 7680 px."));
        custom_resolution_validation_label_->setVisible(true);
    } else if (h < 180) {
        custom_resolution_validation_label_->setText(QStringLiteral("Height must be at least 180 px."));
        custom_resolution_validation_label_->setVisible(true);
    } else if (h > 7680) {
        custom_resolution_validation_label_->setText(QStringLiteral("Height must not exceed 7680 px."));
        custom_resolution_validation_label_->setVisible(true);
    } else if (w % 2 != 0 || h % 2 != 0) {
        custom_resolution_validation_label_->setText(
            QStringLiteral("Dimensions will be aligned to even values (%1×%2).").arg(w & ~1u).arg(h & ~1u));
        custom_resolution_validation_label_->setVisible(true);
    } else {
        custom_resolution_validation_label_->clear();
        custom_resolution_validation_label_->setVisible(false);
    }
}

void ConfigPage::onCustomWidthChanged(int value) {
    if (value <= 0)
        return;
    format_settings_.resolution.custom_width = static_cast<uint32_t>(value);
    SanitizeOutputResolution(format_settings_.resolution);
    updateCustomResolutionVisibility();
    updateEffectiveOutputSummary();
    emitCurrentFormatSettings();
}

void ConfigPage::onCustomHeightChanged(int value) {
    if (value <= 0)
        return;
    format_settings_.resolution.custom_height = static_cast<uint32_t>(value);
    SanitizeOutputResolution(format_settings_.resolution);
    updateCustomResolutionVisibility();
    updateEffectiveOutputSummary();
    emitCurrentFormatSettings();
}

void ConfigPage::updateEffectiveOutputSummary() {
    if (!output_effective_summary_label_)
        return;

    const QString summary =
        QStringLiteral("Output: %1 · %2 · %3 · %4 · %5 · %6")
            .arg(ResolutionLabel(format_settings_.resolution),
                 FrameRateLabel(video_settings_.frame_rate_num, video_settings_.frame_rate_den),
                 video_settings_.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR"),
                 VideoCodecLabel(format_settings_.video_codec), AudioCodecLabel(format_settings_.audio_codec),
                 ContainerLabel(format_settings_.container));
    output_effective_summary_label_->setText(summary);
}

void ConfigPage::setOutputFolder(const std::filesystem::path& folder) {
    format_settings_.output_folder = folder;
    if (destination_edit_) {
        const QSignalBlocker blocker(destination_edit_);
        destination_edit_->setText(QString::fromStdWString(folder.wstring()));
    }
    updateOutputValidationState();
    updateExampleFilename();
}

void ConfigPage::setActiveProfileName(const QString& profile_name) {
    active_profile_name_ = profile_name;
    updateExampleFilename();
}

void ConfigPage::setPresetOptions(const std::vector<ProfileOption>& options, const QString& selected_id,
                                  const QString& default_id, bool dirty) {
    profile_options_ = options;
    active_preset_id_ = selected_id;
    default_preset_id_ = default_id;
    preset_dirty_ = dirty;

    const QSignalBlocker blocker(profile_combo_);
    profile_combo_->clear();
    int active_index = -1;
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto& opt = options[i];
        // All non-selected default entries get the ★ suffix so users can identify
        // the startup default while browsing the list.
        QString label = opt.label;
        if (!default_id.isEmpty() && opt.id == default_id && opt.id != selected_id)
            label += QStringLiteral(" ★");
        profile_combo_->addItem(label, opt.id);
        if (opt.id == selected_id) {
            active_index = static_cast<int>(i);
            active_preset_is_built_in_ = opt.built_in;
            active_preset_is_available_ = opt.available;
        }
    }
    if (active_index >= 0)
        profile_combo_->setCurrentIndex(active_index);

    updatePresetActionState();
}

void ConfigPage::setPresetDirty(bool dirty) {
    if (preset_dirty_ == dirty)
        return;
    preset_dirty_ = dirty;
    updatePresetActionState();
}

void ConfigPage::updatePresetActionState() {
    const bool is_default = !default_preset_id_.isEmpty() && (active_preset_id_ == default_preset_id_);
    const bool has_preset = !active_preset_id_.isEmpty();
    const bool locked = controls_locked_;

    // Dirty indicator: amber "● Unsaved" label shown only when dirty.
    if (preset_dirty_indicator_) {
        preset_dirty_indicator_->setVisible(preset_dirty_);
    }

    // Default badge: shown when the selected preset IS the startup default.
    if (preset_default_badge_) {
        preset_default_badge_->setVisible(is_default);
    }

    // Status badge (built-in / unavailable): separate from the default badge.
    if (profile_status_label_) {
        QString badge;
        if (!active_preset_is_available_) {
            badge = QStringLiteral("Unavailable");
            profile_status_label_->setProperty("stateRole", "blocked");
        } else if (active_preset_is_built_in_) {
            badge = QStringLiteral("Built-in preset");
            profile_status_label_->setProperty("stateRole", "ready");
        }
        // For user presets, suppress the badge — the default badge above is enough.
        profile_status_label_->setText(badge);
        profile_status_label_->setVisible(!badge.isEmpty());
        profile_status_label_->style()->unpolish(profile_status_label_);
        profile_status_label_->style()->polish(profile_status_label_);
    }

    // Save button: enabled only when dirty and not locked.
    if (preset_save_btn_) {
        preset_save_btn_->setVisible(preset_dirty_);
        preset_save_btn_->setEnabled(preset_dirty_ && !locked);
    }

    // Save As button: always visible, always enabled (unless locked).
    if (preset_save_as_btn_) {
        preset_save_as_btn_->setEnabled(!locked);
    }

    // Menu actions.
    if (save_preset_action_)
        save_preset_action_->setEnabled(preset_dirty_);
    if (save_preset_as_action_)
        save_preset_as_action_->setEnabled(true);
    if (new_preset_action_)
        new_preset_action_->setEnabled(true);
    if (duplicate_preset_action_)
        duplicate_preset_action_->setEnabled(has_preset);
    if (rename_preset_action_)
        rename_preset_action_->setEnabled(has_preset && !active_preset_is_built_in_);
    if (delete_preset_action_)
        delete_preset_action_->setEnabled(has_preset && !active_preset_is_built_in_);
    // "Set as default" is available only when the selected preset is NOT already the default.
    if (set_default_preset_action_)
        set_default_preset_action_->setEnabled(has_preset && !is_default);
    if (reset_changes_action_)
        reset_changes_action_->setEnabled(preset_dirty_);
    if (reset_to_defaults_action_)
        reset_to_defaults_action_->setEnabled(true);
}

void ConfigPage::onSavePreset() {
    emit savePresetRequested();
}

void ConfigPage::onSavePresetAs() {
    const QString name = QInputDialog::getText(this, QStringLiteral("Save As New Preset"),
                                               QStringLiteral("Preset name:"), QLineEdit::Normal, active_profile_name_);
    if (name.trimmed().isEmpty())
        return;
    emit savePresetAsRequested(name.trimmed());
}

void ConfigPage::onNewPreset() {
    emit newPresetRequested();
}

void ConfigPage::onDuplicatePreset() {
    emit duplicatePresetRequested();
}

void ConfigPage::onRenamePreset() {
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename Preset"), QStringLiteral("New name:"),
                                               QLineEdit::Normal, active_profile_name_);
    if (name.trimmed().isEmpty())
        return;
    emit renamePresetRequested(name.trimmed());
}

void ConfigPage::onDeletePreset() {
    const auto answer =
        QMessageBox::warning(this, QStringLiteral("Delete Preset"),
                             QStringLiteral("Permanently delete this preset? This action cannot be undone."),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    emit deletePresetRequested();
}

void ConfigPage::onResetChanges() {
    emit resetChangesRequested();
}

void ConfigPage::onResetToDefaults() {
    const auto answer = QMessageBox::warning(this, QStringLiteral("Reset All to Factory Defaults"),
                                             QStringLiteral("Reset all presets and settings to factory defaults? "
                                                            "This action cannot be undone."),
                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    emit resetToDefaultsRequested();
}

void ConfigPage::onSetDefaultPreset() {
    emit setDefaultPresetRequested();
}

void ConfigPage::onManagePresets() {
    emit managePresetsRequested();
}

void ConfigPage::onBrowse() {
    const QString dir =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Select Output Directory"), destination_edit_->text());
    if (!dir.isEmpty()) {
        destination_edit_->setText(dir);
        onDestinationEditingFinished();
    }
}

void ConfigPage::onDestinationEditingFinished() {
    const auto normalized = NormalizeOutputFolderInput(destination_edit_->text().toStdWString());
    if (normalized.result == OutputFolderPolicyResult::Ok) {
        destination_edit_->setText(QString::fromStdWString(normalized.normalized_input));
        format_settings_.output_folder = normalized.resolved_path;
    }
    emitCurrentFormatSettings();
}

void ConfigPage::onPatternEditingFinished() {
    const auto normalized = NormalizeFilenamePatternInput(naming_edit_->text().toStdWString());
    if (normalized.result == FilenamePatternPolicyResult::Ok) {
        naming_edit_->setText(QString::fromStdWString(normalized.normalized_pattern));
        format_settings_.naming_pattern = normalized.normalized_pattern;
    }
    emitCurrentFormatSettings();
}

void ConfigPage::updateOutputValidationState() {
    if (!destination_edit_ || !naming_edit_)
        return;

    if (folder_validation_label_) {
        const auto folder_normalized = NormalizeOutputFolderInput(destination_edit_->text().toStdWString());
        if (folder_normalized.result == OutputFolderPolicyResult::Ok) {
            folder_validation_label_->clear();
            folder_validation_label_->setVisible(false);
        } else {
            folder_validation_label_->setText(
                QString::fromStdWString(OutputFolderPolicyMessage(folder_normalized.result)));
            folder_validation_label_->setVisible(true);
        }
    }

    if (pattern_validation_label_) {
        const auto pattern_normalized = NormalizeFilenamePatternInput(naming_edit_->text().toStdWString());
        if (pattern_normalized.result == FilenamePatternPolicyResult::Ok) {
            pattern_validation_label_->clear();
            pattern_validation_label_->setVisible(false);
        } else {
            pattern_validation_label_->setText(
                QString::fromStdWString(FilenamePatternPolicyMessage(pattern_normalized.result)));
            pattern_validation_label_->setVisible(true);
        }
    }
}

void ConfigPage::updateExampleFilename() {
    if (!example_filename_label_)
        return;

    const auto output_path =
        BuildOutputPath(format_settings_.output_folder, format_settings_.naming_pattern, format_settings_.container,
                        std::time(nullptr), ExamplePreviewContext(active_profile_name_, format_settings_));
    example_filename_label_->setText(QStringLiteral("Example: ") +
                                     QString::fromStdWString(output_path.filename().wstring()));
}

void ConfigPage::setAudioUiState(const capability::AudioUiState& state) {
    audio_ui_state_ = state;
    applyAudioConfigurationState();
    // PS-PHASE-C: sync expert audio controls if visible.
    if (expert_mode_enabled_) {
        if (mic_gain_slider_) {
            const QSignalBlocker b(mic_gain_slider_);
            const int db =
                static_cast<int>(std::roundf(20.f * std::log10f(std::max(0.001f, audio_ui_state_.mic_gain_linear))));
            mic_gain_slider_->setValue(db);
            if (mic_gain_db_label_)
                mic_gain_db_label_->setText(QStringLiteral("%1 dB").arg(db));
        }
        if (mic_channel_mode_combo_) {
            const QSignalBlocker b(mic_channel_mode_combo_);
            const int idx = mic_channel_mode_combo_->findData(static_cast<int>(audio_ui_state_.mic_channel_mode));
            if (idx >= 0)
                mic_channel_mode_combo_->setCurrentIndex(idx);
        }
        if (audio_bitrate_kbps_spin_) {
            const QSignalBlocker b(audio_bitrate_kbps_spin_);
            audio_bitrate_kbps_spin_->setValue(static_cast<int>(audio_ui_state_.audio_bitrate_kbps));
        }
        if (opus_frame_duration_combo_) {
            const QSignalBlocker b(opus_frame_duration_combo_);
            const int idx = opus_frame_duration_combo_->findData(static_cast<int>(audio_ui_state_.opus_frame_duration));
            if (idx >= 0)
                opus_frame_duration_combo_->setCurrentIndex(idx);
        }
        if (opus_complexity_spin_) {
            const QSignalBlocker b(opus_complexity_spin_);
            opus_complexity_spin_->setValue(audio_ui_state_.opus_complexity);
        }
        if (limiter_check_) {
            const QSignalBlocker b(limiter_check_);
            limiter_check_->setChecked(audio_ui_state_.limiter_enabled);
        }
        if (limiter_ceiling_spin_) {
            const QSignalBlocker b(limiter_ceiling_spin_);
            limiter_ceiling_spin_->setValue(static_cast<double>(audio_ui_state_.limiter_ceiling_db));
            limiter_ceiling_spin_->setEnabled(audio_ui_state_.limiter_enabled);
        }
        if (mic_hpf_check_) {
            const QSignalBlocker b(mic_hpf_check_);
            mic_hpf_check_->setChecked(audio_ui_state_.mic_hpf_enabled);
        }
        if (mic_hpf_cutoff_spin_) {
            const QSignalBlocker b(mic_hpf_cutoff_spin_);
            mic_hpf_cutoff_spin_->setValue(static_cast<double>(audio_ui_state_.mic_hpf_cutoff_hz));
            mic_hpf_cutoff_spin_->setEnabled(audio_ui_state_.mic_hpf_enabled);
        }
        if (mic_gate_check_) {
            const QSignalBlocker b(mic_gate_check_);
            mic_gate_check_->setChecked(audio_ui_state_.mic_gate_enabled);
        }
        if (mic_gate_threshold_spin_) {
            const QSignalBlocker b(mic_gate_threshold_spin_);
            mic_gate_threshold_spin_->setValue(static_cast<double>(audio_ui_state_.mic_gate_threshold_db));
            mic_gate_threshold_spin_->setEnabled(audio_ui_state_.mic_gate_enabled);
        }
        if (mic_agc_check_) {
            const QSignalBlocker b(mic_agc_check_);
            mic_agc_check_->setChecked(audio_ui_state_.mic_agc_enabled);
        }
        if (mic_agc_target_spin_) {
            const QSignalBlocker b(mic_agc_target_spin_);
            mic_agc_target_spin_->setValue(static_cast<double>(audio_ui_state_.mic_agc_target_db));
            mic_agc_target_spin_->setEnabled(audio_ui_state_.mic_agc_enabled);
        }
    }
}

void ConfigPage::applyAudioConfigurationState() {
    const AudioConfigurationSnapshot snap =
        PresentationStateBuilder::BuildAudioConfiguration(audio_ui_state_, controls_locked_);

    const bool is_window = (snap.target_kind == capability::CaptureTargetKind::Window);

    // App section visibility (target-kind policy).
    if (app_row_section_)
        app_row_section_->setVisible(snap.app.visible);

    // System audio row labels (target-kind-specific).
    if (sys_enabled_check_) {
        sys_enabled_check_->setText(is_window ? QStringLiteral("Other system audio")
                                              : QStringLiteral("Computer audio"));
    }
    if (sys_source_label_) {
        sys_source_label_->setText(
            is_window ? QStringLiteral("Also records audio from other applications and Windows.")
                      : QStringLiteral("Records all sound played through the selected output device."));
    }

    // Apply audio row widget states atomically.
    // Required invariant: controls_enabled = visible && available && !controls_locked_
    {
        const QSignalBlocker ab(app_enabled_check_);
        const QSignalBlocker as(app_separate_check_);
        const QSignalBlocker mb(mic_enabled_check_);
        const QSignalBlocker ms(mic_separate_check_);
        const QSignalBlocker sb(sys_enabled_check_);
        const QSignalBlocker ss(sys_separate_check_);

        app_enabled_check_->setEnabled(snap.app.controls_enabled);
        app_separate_check_->setEnabled(snap.app.controls_enabled);
        app_enabled_check_->setChecked(snap.app.enabled);
        app_separate_check_->setChecked(snap.app.separate_track);

        mic_enabled_check_->setEnabled(snap.mic.controls_enabled);
        mic_separate_check_->setEnabled(snap.mic.controls_enabled);
        mic_enabled_check_->setChecked(snap.mic.enabled);
        mic_separate_check_->setChecked(snap.mic.separate_track);

        sys_enabled_check_->setEnabled(snap.system.controls_enabled);
        sys_separate_check_->setEnabled(snap.system.controls_enabled);
        sys_enabled_check_->setChecked(snap.system.enabled);
        sys_separate_check_->setChecked(snap.system.separate_track);
    }

    // Mic device combo: visible when mic source is in the plan; enabled when interactable.
    if (mic_device_combo_) {
        const QSignalBlocker mc(mic_device_combo_);
        mic_device_combo_->setVisible(snap.mic.available);
        mic_device_combo_->setEnabled(snap.mic.controls_enabled);
        if (snap.selected_mic_device_id.has_value()) {
            const auto& device_id = *snap.selected_mic_device_id;
            int idx = 0;
            for (int i = 1; i < static_cast<int>(mic_devices_.size()); ++i) {
                if (mic_devices_[static_cast<std::size_t>(i)].device_id == device_id) {
                    idx = i;
                    break;
                }
            }
            mic_device_combo_->setCurrentIndex(idx);
        } else {
            mic_device_combo_->setCurrentIndex(0);
        }
    }

    // Source description labels.
    if (app_source_label_)
        app_source_label_->setText(QStringLiteral("Records audio from the selected application."));
    if (mic_source_label_) {
        mic_source_label_->setText(snap.mic.available ? QStringLiteral("Choose the microphone used for recording.")
                                                      : QStringLiteral("Not available"));
    }

    // Summary label when no audio plan rows are present.
    const bool no_rows = audio_ui_state_.source_rows.empty();
    if (audio_summary_label_) {
        audio_summary_label_->setVisible(no_rows);
        if (no_rows)
            audio_summary_label_->setText(
                QStringLiteral("Audio sources are configured on the Record page. Open Record to set up sources."));
    }
}

void ConfigPage::emitCurrentAudioSettings() {
    emit audioSettingsChanged(audio_ui_state_);
}

void ConfigPage::setAudioMeterLevels(float sys01, float app01, float mic01, bool sys_active, bool app_active,
                                     bool mic_active) {
    auto update = [](ui::widgets::VUMeterWidget* meter, QLabel* db_label, float level01, bool active) {
        if (!meter || !db_label)
            return;
        meter->setActive(active);
        meter->setLevel(active ? level01 : 0.0f);
        if (!active) {
            db_label->setText(QStringLiteral("–"));
        } else if (level01 <= 0.0f) {
            db_label->setText(QStringLiteral("−∞"));
        } else {
            const int db_int = qRound(level01 * 60.0f - 60.0f);
            db_label->setText(QString::number(db_int) + QStringLiteral(" dB"));
        }
    };
    update(audio_sys_meter_, audio_sys_db_label_, sys01, sys_active);
    update(audio_app_meter_, audio_app_db_label_, app01, app_active);
    update(audio_mic_meter_, audio_mic_db_label_, mic01, mic_active);
}

// ---- SETTINGS-TIERS-R1: Expert mode + per-card expander public API ----

void ConfigPage::setExpertModeEnabled(bool enabled) {
    if (expert_mode_enabled_ == enabled)
        return;
    expert_mode_enabled_ = enabled;
    if (expert_mode_toggle_) {
        const QSignalBlocker b(expert_mode_toggle_);
        expert_mode_toggle_->setOn(enabled);
    }
    updateExpertModeVisibility();
}

bool ConfigPage::expertModeEnabled() const noexcept {
    return expert_mode_enabled_;
}

void ConfigPage::setOutputSplitExpanderExpanded(bool /*expanded*/) {
    // Wave 2: output_split_expander_ dissolved; split controls are now expert-gated.
    // No-op kept for MainWindow backward compat (store field still persists).
}

bool ConfigPage::outputSplitExpanderExpanded() const noexcept {
    // Wave 2: always false — no expander widget exists.
    return false;
}

void ConfigPage::setAudioSeparateExpanderExpanded(bool /*expanded*/) {
    // Phase 1b + Wave 2: audio_separate_expander_ removed; no-op for compat.
}

bool ConfigPage::audioSeparateExpanderExpanded() const noexcept {
    // Phase 1b: always false — no audio separate expander widget exists.
    return false;
}

void ConfigPage::updateExpertModeVisibility() {
    // SETTINGS-TIERS-P3: show/hide the expert-gated Developer card.
    if (developer_card_)
        developer_card_->setVisible(expert_mode_enabled_);
    // Wave 2 Part A: split recording section is now expert-gated (was behind expander).
    if (split_expert_section_)
        split_expert_section_->setVisible(expert_mode_enabled_);
    // Wave 2 Part B: quality CQ spinbox row shown in expert mode; segment row hidden.
    if (quality_row_widget_)
        quality_row_widget_->setVisible(!expert_mode_enabled_);
    if (quality_expert_widget_) {
        // In expert mode: CQ spinbox visible when rate=CQ, hidden when VBR/CBR.
        // Default: same as before (visible when expert on).
        const bool rate_is_cq = (video_settings_.rate_control == recorder_core::RateControlMode::ConstantQuality);
        quality_expert_widget_->setVisible(expert_mode_enabled_ && rate_is_cq);
        if (expert_mode_enabled_ && rate_is_cq && quality_cq_spin_) {
            // Seed the spinbox from the current model quality on first show.
            const QSignalBlocker b(quality_cq_spin_);
            switch (video_settings_.quality) {
            case recorder_core::NvencQualityPreset::High:
                quality_cq_spin_->setValue(19);
                break;
            case recorder_core::NvencQualityPreset::Small:
                quality_cq_spin_->setValue(30);
                break;
            default: // Balanced
                quality_cq_spin_->setValue(24);
                break;
            }
        }
    }
    // PS-PHASE-C: fmt_expert_section (rate control, bitrate, format placeholders).
    if (fmt_expert_section_)
        fmt_expert_section_->setVisible(expert_mode_enabled_);
    if (expert_mode_enabled_ && rate_control_group_) {
        // Seed rate control selection from model.
        const QSignalBlocker b(rate_control_group_);
        auto* btn = rate_control_group_->button(static_cast<int>(video_settings_.rate_control));
        if (btn) {
            btn->setProperty("qualitySegmentSelected", true);
            btn->setChecked(true);
        }
        // Update bitrate visibility.
        const bool needs_bitrate = (video_settings_.rate_control == recorder_core::RateControlMode::VariableBitrate ||
                                    video_settings_.rate_control == recorder_core::RateControlMode::ConstantBitrate);
        if (bitrate_row_widget_)
            bitrate_row_widget_->setVisible(needs_bitrate);
        if (bitrate_kbps_spin_) {
            const QSignalBlocker bs(bitrate_kbps_spin_);
            bitrate_kbps_spin_->setValue(static_cast<int>(video_settings_.bitrate_kbps));
        }
    }
    // PS-PHASE-C: audio_expert_section.
    if (audio_expert_section_)
        audio_expert_section_->setVisible(expert_mode_enabled_);
    if (expert_mode_enabled_) {
        // Seed audio expert controls from model.
        if (mic_gain_slider_) {
            const QSignalBlocker b(mic_gain_slider_);
            const int db =
                static_cast<int>(std::roundf(20.f * std::log10f(std::max(0.001f, audio_ui_state_.mic_gain_linear))));
            mic_gain_slider_->setValue(db);
            if (mic_gain_db_label_)
                mic_gain_db_label_->setText(QStringLiteral("%1 dB").arg(db));
        }
        if (mic_channel_mode_combo_) {
            const QSignalBlocker b(mic_channel_mode_combo_);
            const int idx = mic_channel_mode_combo_->findData(static_cast<int>(audio_ui_state_.mic_channel_mode));
            if (idx >= 0)
                mic_channel_mode_combo_->setCurrentIndex(idx);
        }
        if (audio_bitrate_kbps_spin_) {
            const QSignalBlocker b(audio_bitrate_kbps_spin_);
            audio_bitrate_kbps_spin_->setValue(static_cast<int>(audio_ui_state_.audio_bitrate_kbps));
        }
        if (opus_frame_duration_combo_) {
            const QSignalBlocker b(opus_frame_duration_combo_);
            const int idx = opus_frame_duration_combo_->findData(static_cast<int>(audio_ui_state_.opus_frame_duration));
            if (idx >= 0)
                opus_frame_duration_combo_->setCurrentIndex(idx);
        }
        if (opus_complexity_spin_) {
            const QSignalBlocker b(opus_complexity_spin_);
            opus_complexity_spin_->setValue(audio_ui_state_.opus_complexity);
        }
        if (limiter_check_) {
            const QSignalBlocker b(limiter_check_);
            limiter_check_->setChecked(audio_ui_state_.limiter_enabled);
        }
        if (limiter_ceiling_spin_) {
            const QSignalBlocker b(limiter_ceiling_spin_);
            limiter_ceiling_spin_->setValue(static_cast<double>(audio_ui_state_.limiter_ceiling_db));
            limiter_ceiling_spin_->setEnabled(audio_ui_state_.limiter_enabled);
        }
        if (mic_hpf_check_) {
            const QSignalBlocker b(mic_hpf_check_);
            mic_hpf_check_->setChecked(audio_ui_state_.mic_hpf_enabled);
        }
        if (mic_hpf_cutoff_spin_) {
            const QSignalBlocker b(mic_hpf_cutoff_spin_);
            mic_hpf_cutoff_spin_->setValue(static_cast<double>(audio_ui_state_.mic_hpf_cutoff_hz));
            mic_hpf_cutoff_spin_->setEnabled(audio_ui_state_.mic_hpf_enabled);
        }
        if (mic_gate_check_) {
            const QSignalBlocker b(mic_gate_check_);
            mic_gate_check_->setChecked(audio_ui_state_.mic_gate_enabled);
        }
        if (mic_gate_threshold_spin_) {
            const QSignalBlocker b(mic_gate_threshold_spin_);
            mic_gate_threshold_spin_->setValue(static_cast<double>(audio_ui_state_.mic_gate_threshold_db));
            mic_gate_threshold_spin_->setEnabled(audio_ui_state_.mic_gate_enabled);
        }
        if (mic_agc_check_) {
            const QSignalBlocker b(mic_agc_check_);
            mic_agc_check_->setChecked(audio_ui_state_.mic_agc_enabled);
        }
        if (mic_agc_target_spin_) {
            const QSignalBlocker b(mic_agc_target_spin_);
            mic_agc_target_spin_->setValue(static_cast<double>(audio_ui_state_.mic_agc_target_db));
            mic_agc_target_spin_->setEnabled(audio_ui_state_.mic_agc_enabled);
        }
    }
    // PS-PHASE-C: Output v1.0 placeholder section.
    if (auto* out_ph =
            out_panel_ ? out_panel_->findChild<QWidget*>(QStringLiteral("outputV1PlaceholderSection")) : nullptr)
        out_ph->setVisible(expert_mode_enabled_);
    // PS-PHASE-C: Presence v0.6 placeholder section.
    if (auto* pres_ph = presence_panel_
                            ? presence_panel_->findChild<QWidget*>(QStringLiteral("presenceV1PlaceholderSection"))
                            : nullptr)
        pres_ph->setVisible(expert_mode_enabled_);
    // PS-PHASE-C: search pill only visible in expert mode.
    if (settings_search_pill_)
        settings_search_pill_->setVisible(expert_mode_enabled_);
    // D6: Expert warn hint — visible when Expert ON and no active search.
    if (expert_warn_label_) {
        const bool searching = settings_search_box_ && !settings_search_box_->text().trimmed().isEmpty();
        expert_warn_label_->setVisible(expert_mode_enabled_ && !searching);
    }
}

// SETTINGS-SEARCH-R1: per-card live settings filter.
//
// Filtering strategy: per-card (whole cards are shown/hidden as a unit).
// Rationale: ConfigPage builds all rows as anonymous widgets inlined in layouts;
// there are no addressable per-row QWidget pointers stored in member variables.
// Refactoring every row into a named container would restructure the entire
// constructor — per-card filtering is the correct pragmatic choice here.
//
// Cards and their searchable keyword sets:
//   Preset          → "preset", "profile", "save", "manage"
//   Format&Encoding → "container", "codec", "quality", "frame rate", "fps", "timing",
//                     "cfr", "vfr", "cursor", "mkv", "mp4", "webm", "h264", "h.264",
//                     "av1", "hevc", "aac", "opus", "pcm", "format", "encoding"
//   Audio           → "audio", "microphone", "mic", "system", "computer", "separate",
//                     "track"
//   Webcam          → "webcam", "camera", "mirror", "pip", "overlay"
//   Output          → "output", "resolution", "destination", "folder", "filename",
//                     "pattern", "split", "recording", "4k", "1080", "1440", "720",
//                     "native", "custom"
//   Updates         → "update", "software", "version", "check", "automatic"
//   Presence        → "presence", "overlay", "notification", "tray", "diagnostics",
//                     "quick", "controls"
//   Appearance      → "appearance", "theme", "color", "colour", "dark", "light"
//   Developer       → "developer", "log", "logging", "nvtx", "profiling", "debug",
//                     "expert"
//
// Auto-open: when a keyword inside the Output Advanced expander content matches
//   ("split", "recording"), the expander is forced open.
// Expert-tier: when a Developer keyword matches and Expert mode is OFF, an inline
//   "Enable Expert mode to show developer settings" hint is shown instead of
//   hiding silently.

void ConfigPage::applySettingsSearch(const QString& query) {
    const QString q = query.trimmed().toLower();
    const bool searching = !q.isEmpty();

    // Keyword table: each entry maps a card widget to the list of search terms.
    // The search checks whether ANY term in the list contains the query string.
    struct CardEntry {
        QWidget* card;
        QStringList keywords;
        bool is_expander_gated = false; // true → also check expander content
        bool is_developer_card = false; // true → expert-mode affordance path
    };

    const QStringList preset_kws = {QStringLiteral("preset"), QStringLiteral("profile"), QStringLiteral("save"),
                                    QStringLiteral("manage")};

    const QStringList fmt_kws = {QStringLiteral("container"),  QStringLiteral("codec"),  QStringLiteral("quality"),
                                 QStringLiteral("frame rate"), QStringLiteral("fps"),    QStringLiteral("timing"),
                                 QStringLiteral("cfr"),        QStringLiteral("vfr"),    QStringLiteral("cursor"),
                                 QStringLiteral("mkv"),        QStringLiteral("mp4"),    QStringLiteral("webm"),
                                 QStringLiteral("h264"),       QStringLiteral("h.264"),  QStringLiteral("av1"),
                                 QStringLiteral("hevc"),       QStringLiteral("aac"),    QStringLiteral("opus"),
                                 QStringLiteral("pcm"),        QStringLiteral("format"), QStringLiteral("encoding")};

    const QStringList audio_kws = {QStringLiteral("audio"),  QStringLiteral("microphone"), QStringLiteral("mic"),
                                   QStringLiteral("system"), QStringLiteral("computer"),   QStringLiteral("separate"),
                                   QStringLiteral("track")};

    const QStringList webcam_kws = {QStringLiteral("webcam"), QStringLiteral("camera"), QStringLiteral("mirror"),
                                    QStringLiteral("pip"), QStringLiteral("overlay")};

    const QStringList output_kws = {
        QStringLiteral("output"), QStringLiteral("resolution"), QStringLiteral("destination"),
        QStringLiteral("folder"), QStringLiteral("filename"),   QStringLiteral("pattern"),
        QStringLiteral("split"),  QStringLiteral("recording"),  QStringLiteral("4k"),
        QStringLiteral("1080"),   QStringLiteral("1440"),       QStringLiteral("720"),
        QStringLiteral("native"), QStringLiteral("custom")};

    const QStringList presence_kws = {QStringLiteral("presence"),     QStringLiteral("overlay"),
                                      QStringLiteral("notification"), QStringLiteral("tray"),
                                      QStringLiteral("diagnostics"),  QStringLiteral("quick"),
                                      QStringLiteral("controls")};

    const QStringList appearance_kws = {QStringLiteral("appearance"), QStringLiteral("theme"), QStringLiteral("color"),
                                        QStringLiteral("colour"),     QStringLiteral("dark"),  QStringLiteral("light")};

    const QStringList developer_kws = {
        QStringLiteral("developer"), QStringLiteral("log"),   QStringLiteral("logging"), QStringLiteral("nvtx"),
        QStringLiteral("profiling"), QStringLiteral("debug"), QStringLiteral("expert")};

    // Output Advanced expander keywords (subset of output_kws).
    // These trigger expander auto-open regardless of which output keyword matched.
    const QStringList expander_trigger_kws = {QStringLiteral("split"), QStringLiteral("recording")};

    auto kwMatch = [&](const QStringList& keywords) -> bool {
        for (const auto& kw : keywords) {
            if (kw.contains(q) || q.contains(kw))
                return true;
        }
        return false;
    };

    if (!searching) {
        // Empty query: restore all cards to visible.
        if (preset_panel_)
            preset_panel_->setVisible(true);
        if (columns_widget_)
            columns_widget_->setVisible(true);
        if (fmt_panel_)
            fmt_panel_->setVisible(true);
        if (audio_panel_)
            audio_panel_->setVisible(true);
        if (webcam_panel_)
            webcam_panel_->setVisible(true);
        if (out_panel_)
            out_panel_->setVisible(true);
        if (presence_panel_)
            presence_panel_->setVisible(true);
        if (appearance_panel_)
            appearance_panel_->setVisible(true);
        // Developer card: restore to expert-mode-gated state.
        if (developer_card_)
            developer_card_->setVisible(expert_mode_enabled_);
        // Wave 2: output_split_expander_ dissolved — no expander restore needed.

        if (settings_search_count_label_)
            settings_search_count_label_->setVisible(false);
        if (search_expert_hint_label_)
            search_expert_hint_label_->setVisible(false);
        // D6: restore expert warn hint to its visibility-gated state.
        if (expert_warn_label_)
            expert_warn_label_->setVisible(expert_mode_enabled_);
        return;
    }

    // Active search: compute matches and adjust visibility.
    const bool preset_match = kwMatch(preset_kws);
    const bool fmt_match = kwMatch(fmt_kws);
    const bool audio_match = kwMatch(audio_kws);
    const bool webcam_match = kwMatch(webcam_kws);
    const bool output_match = kwMatch(output_kws);
    const bool presence_match = kwMatch(presence_kws);
    const bool appearance_match = kwMatch(appearance_kws);
    const bool developer_match = kwMatch(developer_kws);
    // Output expander auto-open: matches if any expander-specific keyword matches
    // OR the general output card already matched (expander is part of Output).
    const bool expander_trigger = kwMatch(expander_trigger_kws);

    // Apply card visibility.
    if (preset_panel_)
        preset_panel_->setVisible(preset_match);

    // D6: all 6 grid cards (fmt, audio, webcam, output, presence, appearance) live in
    // columns_widget_. Hide the host only when all six are hidden to avoid an empty gap.
    // Cosmetic: when only some cards are hidden, the remaining card in each column expands
    // vertically to fill the gap — this is an accepted limitation per the D6 brief.
    const bool any_column_visible =
        fmt_match || audio_match || webcam_match || output_match || presence_match || appearance_match;
    if (columns_widget_)
        columns_widget_->setVisible(any_column_visible);
    if (fmt_panel_)
        fmt_panel_->setVisible(fmt_match);
    if (audio_panel_)
        audio_panel_->setVisible(audio_match);

    if (webcam_panel_)
        webcam_panel_->setVisible(webcam_match);
    if (out_panel_)
        out_panel_->setVisible(output_match);
    if (presence_panel_)
        presence_panel_->setVisible(presence_match);
    if (appearance_panel_)
        appearance_panel_->setVisible(appearance_match);

    // Wave 2: output_split_expander_ dissolved — no expander auto-open needed.
    // Split controls are part of the Output card and visible when expert mode is on.
    (void)expander_trigger; // still declared; suppress unused-variable warning

    // Developer card: expert-mode affordance.
    if (developer_match) {
        if (expert_mode_enabled_) {
            // Expert mode is on → show the card normally.
            if (developer_card_)
                developer_card_->setVisible(true);
            if (search_expert_hint_label_)
                search_expert_hint_label_->setVisible(false);
        } else {
            // Expert mode is off → show an inline affordance, not the card.
            if (developer_card_)
                developer_card_->setVisible(false);
            if (search_expert_hint_label_) {
                search_expert_hint_label_->setText(QStringLiteral("Enable Expert mode to show developer settings."));
                search_expert_hint_label_->setVisible(true);
            }
        }
    } else {
        // No developer match → keep developer card in its normal gated state.
        if (developer_card_)
            developer_card_->setVisible(expert_mode_enabled_);
        if (search_expert_hint_label_)
            search_expert_hint_label_->setVisible(false);
    }

    // D6: hide expert warn hint during active search (it'd compete with hint label).
    if (expert_warn_label_)
        expert_warn_label_->setVisible(false);

    // Match count.
    int match_count = 0;
    // Count each matched card as 1.  This is intentionally coarse (per-card,
    // not per-row) to match the filtering granularity.
    if (preset_match)
        ++match_count;
    if (fmt_match)
        ++match_count;
    if (audio_match)
        ++match_count;
    if (webcam_match)
        ++match_count;
    if (output_match)
        ++match_count;
    if (presence_match)
        ++match_count;
    if (appearance_match)
        ++match_count;
    if (developer_match)
        ++match_count;

    if (settings_search_count_label_) {
        if (match_count == 0) {
            settings_search_count_label_->setText(QStringLiteral("No matches"));
        } else {
            settings_search_count_label_->setText(
                QStringLiteral("%1 %2")
                    .arg(match_count)
                    .arg(match_count == 1 ? QStringLiteral("section") : QStringLiteral("sections")));
        }
        settings_search_count_label_->setVisible(true);
    }
}

// SETTINGS-TIERS-P3: presence + appearance setters (moved from AdvancedPage).

void ConfigPage::setShowOverlay(bool show) {
    if (overlay_check_) {
        const QSignalBlocker blocker(overlay_check_);
        overlay_check_->setChecked(show);
    }
}

void ConfigPage::setShowDiagnosticsOverlay(bool show) {
    if (diagnostics_overlay_check_) {
        const QSignalBlocker blocker(diagnostics_overlay_check_);
        diagnostics_overlay_check_->setChecked(show);
    }
}

void ConfigPage::setShowNotifications(bool show) {
    if (notifications_check_) {
        const QSignalBlocker blocker(notifications_check_);
        notifications_check_->setChecked(show);
    }
}

void ConfigPage::setKeepRunningInTray(bool keep) {
    if (keep_in_tray_check_) {
        const QSignalBlocker blocker(keep_in_tray_check_);
        keep_in_tray_check_->setChecked(keep);
    }
}

void ConfigPage::setShowQuickControls(bool show) {
    if (quick_controls_check_) {
        const QSignalBlocker blocker(quick_controls_check_);
        quick_controls_check_->setChecked(show);
    }
}

void ConfigPage::setThemeId(const QString& theme_id) {
    current_theme_id_ = theme_id;
    if (!theme_button_group_)
        return;
    const QSignalBlocker blocker(theme_button_group_);
    const auto& buttons = theme_button_group_->buttons();
    for (QAbstractButton* btn : buttons) {
        if (btn->property("themeId").toString() == theme_id) {
            btn->setChecked(true);
            return;
        }
    }
    // Unknown id: check the first button (dark-default).
    if (!buttons.isEmpty())
        buttons.first()->setChecked(true);
}

void ConfigPage::onAudioAppToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::App)
            row.enabled = app_enabled_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioMicToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Mic)
            row.enabled = mic_enabled_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioSysToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Sys || row.kind == recorder_core::AudioSourceKind::SystemOutput)
            row.enabled = sys_enabled_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioAppSeparateToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::App)
            row.merge_with_above = !app_separate_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioMicSeparateToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Mic)
            row.merge_with_above = !mic_separate_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioSysSeparateToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Sys || row.kind == recorder_core::AudioSourceKind::SystemOutput)
            row.merge_with_above = !sys_separate_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::refreshMicDevices() {
    if (!mic_device_combo_)
        return;

    // MUST-FIX B: restore selection from audio_ui_state_.selected_mic_device_id,
    // with unavailable-placeholder handling, under QSignalBlocker.  Do NOT reset to
    // index 0 unconditionally, and do NOT emit audioSettingsChanged.
    const auto previous_id = audio_ui_state_.selected_mic_device_id;

    const QSignalBlocker mc(mic_device_combo_);
    mic_device_combo_->clear();
    mic_devices_.clear();

    mic_device_combo_->addItem(QStringLiteral("System Default Microphone"));
    mic_devices_.push_back({});

    const auto devices = recorder_core::EnumerateAudioInputDevices();
    for (const auto& dev : devices) {
        QString label = QString::fromStdString(dev.display_name);
        if (dev.is_default)
            label += QStringLiteral(" (Default)");
        mic_device_combo_->addItem(label);
        mic_devices_.push_back(dev);
    }

    int restore_index = 0;
    bool found = false;
    if (previous_id.has_value()) {
        for (int i = 1; i < static_cast<int>(mic_devices_.size()); ++i) {
            if (mic_devices_[static_cast<std::size_t>(i)].device_id == *previous_id) {
                restore_index = i;
                found = true;
                break;
            }
        }
    }

    if (!found && previous_id.has_value()) {
        // Configured device absent: append placeholder, keep stored id unchanged.
        const QString placeholder = QString::fromStdString(*previous_id) + QStringLiteral(" (unavailable)");
        mic_device_combo_->addItem(placeholder);
        restore_index = mic_device_combo_->count() - 1;
        // Do NOT modify audio_ui_state_.selected_mic_device_id.
    } else if (found) {
        const auto& sel = mic_devices_[static_cast<std::size_t>(restore_index)];
        audio_ui_state_.selected_mic_device_id =
            sel.device_id.empty() ? std::nullopt : std::optional<std::string>(sel.device_id);
    } else {
        // Semantic Default (nullopt): stay at index 0.
        audio_ui_state_.selected_mic_device_id = std::nullopt;
    }

    mic_device_combo_->setCurrentIndex(restore_index);
}

void ConfigPage::onMicDeviceChanged(int index) {
    if (index <= 0 || index >= static_cast<int>(mic_devices_.size())) {
        audio_ui_state_.selected_mic_device_id = std::nullopt;
    } else {
        const auto& dev = mic_devices_[static_cast<std::size_t>(index)];
        audio_ui_state_.selected_mic_device_id =
            dev.device_id.empty() ? std::nullopt : std::optional<std::string>(dev.device_id);
    }
    emitCurrentAudioSettings();
}

void ConfigPage::setWebcamSettings(const WebcamSettings& settings) {
    webcam_settings_ = settings;
    if (webcam_setup_panel_)
        webcam_setup_panel_->applySettings(settings);
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void ConfigPage::applyVisualWebcamState(bool available, bool mirror) {
    if (webcam_setup_panel_)
        webcam_setup_panel_->applyVisualState(available, mirror);
}

void ConfigPage::applyVisualPresetSaveError(bool show) {
    if (show && !visual_preset_error_label_) {
        // Lazily insert the error label directly below the preset selector row in
        // the same parent QWidget.  We locate the preset_save_btn_ parent layout
        // and insert the label after the selector row.
        QWidget* panel = preset_save_btn_ ? preset_save_btn_->parentWidget() : nullptr;
        if (!panel)
            return;
        visual_preset_error_label_ = new QLabel(panel);
        visual_preset_error_label_->setObjectName(QStringLiteral("presetVisualErrorLabel"));
        visual_preset_error_label_->setProperty("labelRole", "validationError");
        visual_preset_error_label_->setWordWrap(true);
        visual_preset_error_label_->setText(
            QStringLiteral("⚠ Name already exists. Choose a different name before saving."));
        // Insert into the panel's layout immediately after the selector row.
        if (QLayout* lay = panel->layout()) {
            // Find the position of profile_overflow_btn_ in the layout and insert
            // the error label right below the row that contains it.
            int insert_pos = lay->count(); // fallback: append
            for (int i = 0; i < lay->count(); ++i) {
                QLayoutItem* item = lay->itemAt(i);
                if (!item)
                    continue;
                if (QLayout* sub = item->layout()) {
                    for (int j = 0; j < sub->count(); ++j) {
                        QLayoutItem* sub_item = sub->itemAt(j);
                        if (sub_item && sub_item->widget() == profile_overflow_btn_) {
                            insert_pos = i + 1;
                            break;
                        }
                    }
                }
            }
            if (auto* vlay = qobject_cast<QVBoxLayout*>(lay))
                vlay->insertWidget(insert_pos, visual_preset_error_label_);
            else
                lay->addWidget(visual_preset_error_label_);
        }
    }
    if (visual_preset_error_label_)
        visual_preset_error_label_->setVisible(show);
}
#endif

void ConfigPage::setReadinessStatus(const QString& status_label) {
    if (!readiness_badge_label_)
        return;

    const QString upper = status_label.trimmed().toUpper();
    const bool blocked = upper == QStringLiteral("BLOCKED") || upper == QStringLiteral("ERROR");
    const bool ready = upper == QStringLiteral("READY");
    const bool checking = upper == QStringLiteral("CHECKING");

    readiness_badge_label_->setText(ready      ? QStringLiteral("Ready to record")
                                    : blocked  ? QStringLiteral("Recording blocked")
                                    : checking ? QStringLiteral("Checking configuration...")
                                               : QStringLiteral("Status: %1").arg(upper));

    if (readiness_detail_label_) {
        readiness_detail_label_->setText(ready ? QStringLiteral("Current configuration is compatible with this system.")
                                         : blocked  ? QStringLiteral("Open Diagnostics to review the top issue.")
                                         : checking ? QStringLiteral("Verifying system capabilities...")
                                                    : QString());
    }

    if (view_details_btn_) {
        view_details_btn_->setVisible(blocked);
    }

    // Tint the banner and colour the title to read like the prototype readiness strip
    // (green = ready, red = blocked, neutral while checking).
    const char* state = ready ? "ready" : blocked ? "blocked" : "checking";
    const char* title_state = ready ? "ready" : blocked ? "blocked" : "muted";
    const auto repolish = [](QWidget* w) {
        if (!w)
            return;
        w->style()->unpolish(w);
        w->style()->polish(w);
    };
    if (readiness_panel_) {
        readiness_panel_->setVisible(!ready);
        readiness_panel_->setProperty("stateRole", state);
        repolish(readiness_panel_);
    }
    readiness_badge_label_->setProperty("stateRole", title_state);
    repolish(readiness_badge_label_);
}

void ConfigPage::setRecordingControlsLocked(bool locked) {
    if (controls_locked_ == locked)
        return;
    controls_locked_ = locked;

    const bool enabled = !locked;

    // Non-audio controls: locked unconditionally (no target-kind policy applies).
    profile_combo_->setEnabled(enabled);
    if (preset_save_btn_)
        preset_save_btn_->setEnabled(enabled && preset_dirty_);
    if (preset_save_as_btn_)
        preset_save_as_btn_->setEnabled(enabled);
    profile_overflow_btn_->setEnabled(enabled);
    mkv_radio_->setEnabled(enabled);
    webm_radio_->setEnabled(enabled);
    mp4_radio_->setEnabled(enabled);
    video_codec_combo_->setEnabled(enabled);
    audio_codec_combo_->setEnabled(enabled);

    quality_combo_->setEnabled(enabled);
    frame_rate_combo_->setEnabled(enabled);
    quality_segment_small_->setEnabled(enabled);
    quality_segment_balanced_->setEnabled(enabled);
    quality_segment_high_->setEnabled(enabled);
    updateTimingSelection();
    cursor_check_->setEnabled(enabled);
    output_res_native_btn_->setEnabled(enabled);
    output_res_4k_btn_->setEnabled(enabled);
    output_res_1440_btn_->setEnabled(enabled);
    output_res_1080_btn_->setEnabled(enabled);
    output_res_720_btn_->setEnabled(enabled);

    if (webcam_setup_panel_)
        webcam_setup_panel_->setControlsLocked(locked);

    destination_edit_->setEnabled(enabled);
    browse_btn_->setEnabled(enabled);
    naming_edit_->setEnabled(enabled);

    if (lock_note_label_)
        lock_note_label_->setVisible(locked);

    // Audio source rows: use the canonical snapshot so the invariant
    //   controls_enabled = visible && available && !controls_locked_
    // holds regardless of call order between setAudioUiState and setRecordingControlsLocked.
    applyAudioConfigurationState();
}

void ConfigPage::onAudioDevicesChanged(const exosnap::AudioDeviceSnapshot& snap) {
    // Rewrite mic device combo preserving selection, under QSignalBlocker.
    // refreshMicDevices() uses EnumerateAudioInputDevices() internally; calling it
    // here means we re-enumerate, but the notifier has already deduplicated the
    // snapshot — this simply rebuilds the UI list in sync.
    refreshMicDevices();

    diagnostics::AppLog::info(
        QStringLiteral("audio"),
        QStringLiteral("Settings audio device list refreshed (inputs=%1)").arg(snap.inputs.size()));
}

void ConfigPage::onWebcamDevicesChanged(const exosnap::WebcamDeviceSnapshot& snap) {
    if (webcam_setup_panel_)
        webcam_setup_panel_->onWebcamDevicesChanged(snap);
}

// PS-PHASE-C: Hotkeys panel wiring.
void ConfigPage::setHotkeyService(GlobalHotkeyService* service) {
    if (hotkeys_settings_panel_)
        hotkeys_settings_panel_->setService(service);
}

void ConfigPage::setHotkeyEditingLocked(bool locked) {
    if (hotkeys_settings_panel_)
        hotkeys_settings_panel_->setEditingLocked(locked);
}

// PS-PHASE-E: deep-link target support — scroll Settings to the named section.
void ConfigPage::scrollToSection(const QString& section_target) {
    if (!scroll_area_)
        return;

    QWidget* target_widget = nullptr;
    if (section_target == QStringLiteral("settings/audio"))
        target_widget = audio_panel_;
    else if (section_target == QStringLiteral("settings/video") || section_target == QStringLiteral("settings/format"))
        target_widget = fmt_panel_;
    else if (section_target == QStringLiteral("settings/output"))
        target_widget = out_panel_;
    else if (section_target == QStringLiteral("settings/webcam"))
        target_widget = webcam_panel_;
    else if (section_target == QStringLiteral("settings/presence"))
        target_widget = presence_panel_;
    else if (section_target == QStringLiteral("settings/appearance"))
        target_widget = appearance_panel_;
    else if (section_target == QStringLiteral("settings/hotkeys"))
        target_widget = hotkeys_settings_panel_;

    if (target_widget)
        scroll_area_->ensureWidgetVisible(target_widget);
}

} // namespace exosnap

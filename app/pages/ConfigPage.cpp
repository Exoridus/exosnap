#include "ConfigPage.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCompleter>
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
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <capability/capability_builder.h>
#include <capability/resolver.h>
#include <capability/support_level.h>

#include "../diagnostics/AppLog.h"
#include "../models/FilenameBuilder.h"
#include "../models/OutputPathPolicy.h"
#include "../models/RecordingPreset.h"
#include "../models/SettingsHintText.h"
#include "../services/GlobalHotkeyService.h"
#include "../services/WebcamService.h"
#include "../ui/CodecLabels.h"
#include "../ui/theme/ExoSnapMetrics.h"
#include "../ui/theme/ExoSnapPalette.h"
#include "../ui/theme/ExoSnapTheme.h"
#include "../ui/theme/ExoSnapThemes.h"
#include "../ui/theme/LucideIcon.h"
#include "../ui/widgets/ComboBoxWheelFilter.h"
#include "../ui/widgets/CompareHint.h"
#include "../ui/widgets/ExoCheckBox.h"
#include "../ui/widgets/ExoSlider.h"
#include "../ui/widgets/ExoToggle.h"
#include "../ui/widgets/HotkeysSettingsPanel.h"
#include "../ui/widgets/InfoHintIcon.h"
#include "../ui/widgets/VUMeterWidget.h"
#include "../ui/widgets/WebcamSetupPanel.h"
#include "../viewmodels/PresentationStateBuilder.h"
#include <recorder_core/audio_track_model.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <optional>

namespace exosnap {

namespace {

using M = ui::theme::ExoSnapMetrics;

// Bit-depth combo ItemData encoding (Float-PCM). The combo stores plain int
// bit-depth values (16/24/32) as ItemData. A fourth "32-bit float" entry needs
// an ItemData that cannot collide with the existing "32-bit" (int) entry --
// findData(32) must not ambiguously match both -- so float uses a negative
// sentinel, out of range of the positive 16/24/32 int values.
constexpr int kFloatBitDepthItemData = -32;

// Encode the (bit_depth, is_float) pair the audio format model carries into
// the single ItemData int the combo box stores.
int EncodeBitDepthComboData(uint32_t bit_depth, bool is_float) {
    return is_float ? kFloatBitDepthItemData : static_cast<int>(bit_depth);
}

// Draws the preset combo's option rows and, for built-in presets, a small
// "Built-in" badge pill at the right edge of the row. Keeping the badge inside
// the option means it no longer sits beside the combo and shifts the toolbar
// layout. Built-in-ness is carried per item in ConfigPage::kPresetBuiltInRole.
class PresetOptionDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

  protected:
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        // Base draws the row background, selection highlight, and label text.
        QStyledItemDelegate::paint(painter, option, index);

        if (!index.data(ConfigPage::kPresetBuiltInRole).toBool())
            return;

        const QString badge = QStringLiteral("Built-in");
        QFont badge_font = option.font;
        const int base_px = badge_font.pixelSize();
        badge_font.setPixelSize(base_px > 0 ? qMax(9, base_px - 2) : 11);
        const QFontMetrics fm(badge_font);

        constexpr int kPadX = 7;
        constexpr int kMargin = 8;
        const int badge_w = fm.horizontalAdvance(badge) + 2 * kPadX;
        const int badge_h = qMin(option.rect.height() - 4, fm.height() + 4);
        const QRect badge_rect(option.rect.right() - badge_w - kMargin,
                               option.rect.top() + (option.rect.height() - badge_h) / 2, badge_w, badge_h);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const auto& th = ui::theme::ActiveTheme();
        QPainterPath pill;
        pill.addRoundedRect(badge_rect, badge_h / 2.0, badge_h / 2.0);
        painter->setPen(QPen(ui::theme::ParseThemeColor(th.line2), 1.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(pill);
        painter->setFont(badge_font);
        painter->setPen(ui::theme::ParseThemeColor(th.mut));
        painter->drawText(badge_rect, Qt::AlignCenter, badge);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        if (index.data(ConfigPage::kPresetBuiltInRole).toBool())
            s.setWidth(s.width() + 72); // reserve room for the "Built-in" badge
        return s;
    }
};

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
        // Minimum: as narrow as the widest single chip, and one row tall. The actual
        // wrapped height comes from the flow layout (sizeHint / resizeEvent). Summing
        // every chip's height reserved a full stacked column of empty space below the
        // wrapped rows (the tokens-disclosure gap this fix removes).
        int max_w = 0;
        int max_h = 0;
        for (auto* c : chips_) {
            const QSize hint = c->sizeHint();
            max_w = qMax(max_w, hint.width());
            max_h = qMax(max_h, hint.height());
        }
        return {max_w, max_h};
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
        : QWidget(parent), theme_(&theme) {
        setFixedSize(116, 70);
        setAttribute(Qt::WA_NoSystemBackground, true);
    }

    // Switch the theme this swatch renders (used by the full-width Appearance
    // preview, which re-renders the selected theme on selection change).
    void setTheme(const exosnap::ui::theme::ExoTheme& theme) {
        theme_ = &theme;
        update();
    }

    // Expand the swatch into a full-width preview (used below the theme selector).
    // Drops the fixed size and scales the painted mini-UI to the available width.
    void setFullWidth(bool full) {
        full_width_ = full;
        if (full) {
            setMinimumSize(0, 96);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        }
        update();
    }

    // Embed the swatch inside a clickable theme card: reserve a fixed preview
    // height (a QPushButton does NOT honour a child layout's height-for-width, so
    // height cannot be derived from width) and paint the 116×70 mini-UI scaled to
    // FIT the widget, centred — never clipped, whatever the card width becomes.
    void setCardFill(bool fill) {
        card_fill_ = fill;
        if (fill) {
            setMinimumWidth(0);
            setMaximumWidth(QWIDGETSIZE_MAX);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            setFixedHeight(82);
        }
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // ExoTheme colour values may be "#rrggbb" or "rgba(r, g, b, alpha_float)".
        // Qt 6 QColor(QString) handles "#rrggbb" but not "rgba(...)" CSS format.
        // parseCssColor handles both.
        const auto bg = parseCssColor(theme_->bg);
        const auto surf = parseCssColor(theme_->surf);
        const auto surf2 = parseCssColor(theme_->surf2);
        const auto line = parseCssColor(theme_->line);
        const auto mut = parseCssColor(theme_->mut);
        const auto ac = parseCssColor(theme_->ac);
        const auto ac2 = parseCssColor(theme_->ac2);
        const auto success = parseCssColor(theme_->success);
        const auto err = parseCssColor(theme_->error);
        const auto caution = parseCssColor(theme_->caution);

        // The mini-UI is designed against a 116×70 canvas. In full-width mode the
        // widget is wider/taller, so scale the painter uniformly to the available
        // height (keeping the designed proportions) and centre horizontally.
        int W = width();
        int H = height();
        if (card_fill_) {
            // Scale the 116×70 mini-UI to FIT the widget (keeping aspect) and
            // centre it — never clips, whatever the card width turns out to be.
            const qreal scale = qMin(static_cast<qreal>(W) / 116.0, static_cast<qreal>(H) / 70.0);
            const qreal scaledW = 116.0 * scale;
            const qreal scaledH = 70.0 * scale;
            p.translate((W - scaledW) / 2.0, (H - scaledH) / 2.0);
            p.scale(scale, scale);
            W = 116;
            H = 70;
        } else if (full_width_) {
            constexpr qreal kDesignW = 116.0;
            constexpr qreal kDesignH = 70.0;
            const qreal scale = qMax(1.0, static_cast<qreal>(H) / kDesignH);
            const qreal scaledW = kDesignW * scale;
            const qreal offX = (W - scaledW) / 2.0;
            p.translate(offX, 0);
            p.scale(scale, scale);
            W = static_cast<int>(kDesignW);
            H = static_cast<int>(kDesignH);
        }

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

    const exosnap::ui::theme::ExoTheme* theme_;
    bool full_width_ = false;
    bool card_fill_ = false;
};

QFrame* makePanel(QWidget* parent) {
    auto* panel = new QFrame(parent);
    panel->setProperty("panelRole", "panel");
    return panel;
}

// Lucide-style 24x24 stroke path data for the per-card glyph chips (v10 design set).
// Path data mirrors shared.jsx ICON_PATHS — kept local so this file owns its glyphs
// (LucideIcon.cpp lacks film/gauge/speaker/bell/keyboard/palette).
QByteArray cardGlyphPathFor(const QString& key) {
    if (key == QLatin1String("film"))
        return QByteArrayLiteral("M3 4h18v16H3zM3 9h18M3 14h18M8 4v16M16 4v16");
    if (key == QLatin1String("gauge"))
        return QByteArrayLiteral("M12 14a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM13.4 10.6L17 7M4.5 18a9 9 0 1 1 15 0");
    if (key == QLatin1String("speaker"))
        return QByteArrayLiteral("M11 5L6 9H2v6h4l5 4V5zM15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13");
    if (key == QLatin1String("folder"))
        return QByteArrayLiteral("M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z");
    if (key == QLatin1String("camera"))
        return QByteArrayLiteral("M3 7h3l2-2h8l2 2h3v12H3zM12 17a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7");
    if (key == QLatin1String("bell"))
        return QByteArrayLiteral("M18 8a6 6 0 0 0-12 0c0 7-3 9-3 9h18s-3-2-3-9M13.7 21a2 2 0 0 1-3.4 0");
    if (key == QLatin1String("keyboard"))
        return QByteArrayLiteral("M3 6h18v12H3zM7 10h.01M11 10h.01M15 10h.01M7 14h10");
    if (key == QLatin1String("download"))
        return QByteArrayLiteral("M12 4v11M7 11l5 5 5-5M5 20h14");
    if (key == QLatin1String("palette"))
        return QByteArrayLiteral(
            "M12 3a9 9 0 1 0 0 18c1.1 0 1.7-1 1.4-2-.4-1.2.5-2 1.6-2H18a3 3 0 0 0 "
            "3-3c0-4.4-4-8-9-8zM7.5 11a1 1 0 1 0 0-.01M12 8a1 1 0 1 0 0-.01M16 11a1 1 0 1 0 0-.01");
    if (key == QLatin1String("bug"))
        return QByteArrayLiteral("M8 6a4 4 0 0 1 8 0M6 10h12v4a6 6 0 0 1-12 0zM6 13H3M21 13h-3M5 7L4 6M19 7l1-1M5 "
                                 "19l-1 1M19 19l1 1");
    return {};
}

// Renders a card glyph into a HiDPI-crisp tinted pixmap (same inline-SVG technique as
// AudioSourceToggle::paintIcon — stroke=color, width 1.7, round caps/joins, fill:none).
QPixmap cardGlyphPixmap(const QString& key, const QColor& color, int size, qreal dpr) {
    if (dpr <= 0.0)
        dpr = 1.0;
    if (size <= 0)
        size = 1;
    const int phys = static_cast<int>(static_cast<qreal>(size) * dpr + 0.5);
    QPixmap pix(phys, phys);
    pix.fill(Qt::transparent);
    pix.setDevicePixelRatio(dpr);
    const QByteArray path = cardGlyphPathFor(key);
    if (path.isEmpty())
        return pix;
    QByteArray svg;
    svg.reserve(path.size() + 220);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='");
    svg.append(color.name(QColor::HexRgb).toUtf8());
    svg.append("' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'><path d='");
    svg.append(path);
    svg.append("'/></svg>");
    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return pix;
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, QRectF(0, 0, size, size));
    return pix;
}

// Card title: 15/600 per the design system "Section/card title" role.
// v10: when `icon_key` is non-empty the title gains a 28x28 glyph chip on its left
// (bg --ac-dim, border --ac-b2, 15px --ac stroke icon). Styling is QSS-driven via the
// `cardGlyphChip` object name; the icon is tinted to the active theme's accent.
// `trailing`, when given, is placed flush-right in the title row as a header badge
// (e.g. the Hotkeys card's "Reset all"). It is reparented into the row.
QWidget* makeCardTitle(const QString& text, QWidget* parent, const QString& icon_key = QString(),
                       QWidget* trailing = nullptr) {
    if (icon_key.isEmpty() && trailing == nullptr) {
        auto* l = new QLabel(text, parent);
        l->setProperty("labelRole", "cardTitle");
        return l;
    }
    auto* row = new QWidget(parent);
    row->setProperty("cardTitleRow", true);
    auto* hl = new QHBoxLayout(row);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(10);

    if (!icon_key.isEmpty()) {
        auto* chip = new QLabel(row);
        chip->setObjectName(QStringLiteral("cardGlyphChip"));
        chip->setFixedSize(28, 28);
        chip->setAlignment(Qt::AlignCenter);
        const QColor accent(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().ac));
        chip->setPixmap(cardGlyphPixmap(icon_key, accent, 18, chip->devicePixelRatioF()));
        hl->addWidget(chip, 0, Qt::AlignVCenter);
    }

    auto* l = new QLabel(text, row);
    l->setProperty("labelRole", "cardTitle");
    hl->addWidget(l, 0, Qt::AlignVCenter);
    hl->addStretch(1);

    if (trailing != nullptr)
        hl->addWidget(trailing, 0, Qt::AlignVCenter); // reparents into the title row
    return row;
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

// Codec/container/frame-rate/resolution labels live in the shared canon header
// (app/ui/CodecLabels.h) — single source of truth so ConfigPage and RecordPage
// can never drift apart on casing again.
using exosnap::ui::audioCodecLabel;
using exosnap::ui::containerLabel;
using exosnap::ui::frameRateLabel;
using exosnap::ui::resolutionLabel;
using exosnap::ui::videoCodecLabel;

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
    //          · stretch · Expert mode label + toggle
    // No page title — the active nav tab already reads "Settings".
    {
        auto* header_zone = new QWidget(content);
        header_zone->setObjectName(QStringLiteral("settingsHeaderZone"));
        auto* header_vl = new QVBoxLayout(header_zone);
        header_vl->setContentsMargins(0, 0, 0, 0);
        header_vl->setSpacing(8);

        // ---- Slim toolbar row ----
        auto* toolbar_row = new QWidget(header_zone);
        toolbar_row->setObjectName(QStringLiteral("settingsSlimToolbar"));
        toolbar_row->setProperty("role", "settingsToolbar");
        // Plain QWidget: needs WA_StyledBackground to paint the QSS slim-toolbar frame.
        toolbar_row->setAttribute(Qt::WA_StyledBackground, true);

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
        // Draw the "Built-in" badge inside the dropdown rows (see PresetOptionDelegate).
        profile_combo_->setItemDelegate(new PresetOptionDelegate(profile_combo_));
        toolbar_hl->addWidget(profile_combo_, 1, Qt::AlignVCenter);

        profile_status_label_ = new QLabel(toolbar_row);
        profile_status_label_->setProperty("labelRole", "profileStatusBadge");
        profile_status_label_->setAlignment(Qt::AlignCenter);
        profile_status_label_->setVisible(false);
        toolbar_hl->addWidget(profile_status_label_, 0, Qt::AlignVCenter);

        // Save as new (shown only while the live config is (changed))
        preset_save_as_btn_ = new QPushButton(QStringLiteral("Save as new\xe2\x80\xa6"), toolbar_row);
        preset_save_as_btn_->setObjectName(QStringLiteral("presetSaveAsButton"));
        preset_save_as_btn_->setProperty("role", "ghost");
        preset_save_as_btn_->setVisible(false);
        toolbar_hl->addWidget(preset_save_as_btn_, 0, Qt::AlignVCenter);

        // Reset (shown only while the live config is (changed))
        preset_reset_btn_ = new QPushButton(QStringLiteral("Reset"), toolbar_row);
        preset_reset_btn_->setObjectName(QStringLiteral("presetResetButton"));
        preset_reset_btn_->setProperty("role", "quiet");
        preset_reset_btn_->setVisible(false);
        toolbar_hl->addWidget(preset_reset_btn_, 0, Qt::AlignVCenter);

        // Delete (shown only for a selected user preset)
        preset_delete_btn_ = new QPushButton(QStringLiteral("Delete"), toolbar_row);
        preset_delete_btn_->setObjectName(QStringLiteral("presetDeleteButton"));
        preset_delete_btn_->setProperty("role", "quiet");
        preset_delete_btn_->setVisible(false);
        toolbar_hl->addWidget(preset_delete_btn_, 0, Qt::AlignVCenter);

        // Overflow menu button
        profile_overflow_btn_ = new QToolButton(toolbar_row);
        profile_overflow_btn_->setObjectName(QStringLiteral("presetManageButton"));
        profile_overflow_btn_->setText(QStringLiteral("\xe2\x80\xa6"));
        profile_overflow_btn_->setPopupMode(QToolButton::InstantPopup);
        profile_overflow_btn_->setToolButtonStyle(Qt::ToolButtonTextOnly);

        auto* profile_menu = new QMenu(profile_overflow_btn_);
        save_preset_as_action_ = profile_menu->addAction(QStringLiteral("Save as new\xe2\x80\xa6"));
        rename_preset_action_ = profile_menu->addAction(QStringLiteral("Rename\xe2\x80\xa6"));
        profile_menu->addSeparator();
        export_preset_action_ = profile_menu->addAction(QStringLiteral("Export\xe2\x80\xa6"));
        import_presets_action_ = profile_menu->addAction(QStringLiteral("Import\xe2\x80\xa6"));
        profile_overflow_btn_->setMenu(profile_menu);
        toolbar_hl->addWidget(profile_overflow_btn_, 0, Qt::AlignVCenter);

        // Stretch pushes expert controls to the right
        toolbar_hl->addStretch(1);

        // "Expert mode" label + toggle. The label tints from muted to accent when expert
        // mode is on (P3: dynamic `expertOn` property + QSS, repolished in updateExpertModeVisibility()).
        expert_mode_label_ = new QLabel(QStringLiteral("Expert mode"), toolbar_row);
        expert_mode_label_->setObjectName(QStringLiteral("expertModeLabel"));
        expert_mode_label_->setProperty("labelRole", "muted");
        expert_mode_label_->setProperty("expertOn", false);
        toolbar_hl->addWidget(expert_mode_label_, 0, Qt::AlignVCenter);

        // Info-i sits directly right of the label so the toggle stays flush to the
        // right edge, vertically aligned with the cards below.
        auto* expert_info = new ui::widgets::InfoHintIcon(ui::hints::kExpertMode, toolbar_row);
        expert_info->setObjectName(QStringLiteral("expertModeInfoHint"));
        toolbar_hl->addWidget(expert_info, 0, Qt::AlignVCenter);

        expert_mode_toggle_ = new ui::widgets::ExoToggle(toolbar_row);
        expert_mode_toggle_->setObjectName(QStringLiteral("expertModeToggleBtn"));
        expert_mode_toggle_->setOn(false);
        toolbar_hl->addWidget(expert_mode_toggle_, 0, Qt::AlignVCenter);

        header_vl->addWidget(toolbar_row);

        layout->addWidget(header_zone);
    }

    // ---- EXPERT WARNING BANNER (P2) ----
    // Amber banner above the card grid, visible only in expert mode. Re-introduces the
    // old expert_warn_label_ banner that was downgraded to an inline InfoHint icon; the
    // InfoHint by the toggle stays for in-place help, the banner restores the prominent
    // "files may not play everywhere" caution.
    {
        expert_warn_banner_ = new QWidget(content);
        expert_warn_banner_->setObjectName(QStringLiteral("expertWarnBanner"));
        auto* ewb_hl = new QHBoxLayout(expert_warn_banner_);
        ewb_hl->setContentsMargins(15, 11, 15, 11);
        ewb_hl->setSpacing(10);

        auto* ewb_icon = new QLabel(expert_warn_banner_);
        ewb_icon->setObjectName(QStringLiteral("expertWarnBannerIcon"));
        ewb_icon->setFixedSize(15, 15);
        ewb_icon->setAlignment(Qt::AlignCenter);
        ewb_icon->setPixmap(ui::theme::lucidePixmap(QStringLiteral("alert-triangle"),
                                                    QString::fromUtf8(ui::theme::ActiveTheme().caution), 15,
                                                    expert_warn_banner_->devicePixelRatioF()));
        ewb_hl->addWidget(ewb_icon, 0, Qt::AlignVCenter);

        auto* ewb_text = new QLabel(QStringLiteral("Expert settings can produce files that won't play everywhere."),
                                    expert_warn_banner_);
        ewb_text->setObjectName(QStringLiteral("expertWarnBannerText"));
        ewb_text->setWordWrap(true);
        ewb_hl->addWidget(ewb_text, 1);

        // v0.9 polish: the terse caution keeps its info-i for the concrete detail (which
        // settings narrow compatibility), so the banner stays a short sentence.
        auto* ewb_info = new ui::widgets::InfoHintIcon(ui::hints::kExpertBannerDetail, expert_warn_banner_);
        ewb_info->setObjectName(QStringLiteral("expertWarnBannerInfoHint"));
        ewb_hl->addWidget(ewb_info, 0, Qt::AlignVCenter);

        expert_warn_banner_->setVisible(expert_mode_enabled_);
        layout->addWidget(expert_warn_banner_);
    }

    // ---- TWO-COLUMN CARD GRID (v10 masonry, fixed-column placement) ----
    // Left column:  Container & codecs · Quality & timing · Audio · Hotkeys · Developer(Expert).
    // Right column: Output · Webcam · Notifications & overlays · Updates · Appearance.
    // On narrow viewports updateResponsiveLayout() flips both columns to a single stacked column.
    auto* columns = new QWidget(content);
    columns_layout_ = new QHBoxLayout(columns);
    columns_layout_->setContentsMargins(0, 0, 0, 0);
    columns_layout_->setSpacing(18);

    auto* left_col = new QWidget(columns);
    auto* left_layout = new QVBoxLayout(left_col);
    left_col_ = left_col; // stashed for the lazy developer-card build
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
    fmt_layout->addWidget(makeCardTitle(QStringLiteral("Container & codecs"), fmt_panel, QStringLiteral("film")));

    // --- Container row ---
    // v10/Canon (SSelect): a compact dropdown, not a full-width segmented group.
    // itemData carries the capability::Container enum consumed by onContainerChanged.
    container_combo_ = new QComboBox(fmt_panel);
    container_combo_->setObjectName(QStringLiteral("containerCombo"));
    container_combo_->addItem(QStringLiteral("MKV"), static_cast<int>(capability::Container::Matroska));
    container_combo_->addItem(QStringLiteral("WebM"), static_cast<int>(capability::Container::WebM));
    container_combo_->addItem(QStringLiteral("MP4"), static_cast<int>(capability::Container::Mp4));
    container_combo_->setFixedWidth(160);
    container_combo_->setProperty("settingsRowInput", true);

    container_compare_hint_ =
        new ui::widgets::CompareHint(QStringLiteral("container"), QStringLiteral("MKV"), fmt_panel);
    fmt_layout->addWidget(makeSettingsRow(fmt_panel, QStringLiteral("Container"), container_compare_hint_, QString(),
                                          container_combo_, /*first=*/true));

    // --- Video codec row ---
    video_codec_combo_ = new QComboBox(fmt_panel);
    video_codec_combo_->setObjectName(QStringLiteral("videoCodecCombo"));
    video_codec_combo_->setFixedWidth(160);
    video_codec_combo_->setProperty("settingsRowInput", true);
    video_codec_compare_hint_ =
        new ui::widgets::CompareHint(QStringLiteral("videoCodec"), QStringLiteral("AV1"), fmt_panel);
    fmt_layout->addWidget(makeSettingsRow(fmt_panel, QStringLiteral("Video codec"), video_codec_compare_hint_,
                                          QString(), video_codec_combo_));

    // --- Audio codec row ---
    audio_codec_combo_ = new QComboBox(fmt_panel);
    audio_codec_combo_->setFixedWidth(160);
    audio_codec_combo_->setProperty("settingsRowInput", true);
    audio_codec_compare_hint_ =
        new ui::widgets::CompareHint(QStringLiteral("audioCodec"), QStringLiteral("Opus"), fmt_panel);
    fmt_layout->addWidget(makeSettingsRow(fmt_panel, QStringLiteral("Audio codec"), audio_codec_compare_hint_,
                                          QString(), audio_codec_combo_));

    // ---- v10: QUALITY & TIMING CARD (left column, below Container & codecs) ----
    // The old "Format & encoding" mega-card is split here. From this point on,
    // Quality / Rate control / Frame rate / Frame timing / Capture cursor rows are
    // built into quality_panel (the second card) rather than fmt_panel. All widget
    // pointers, objectNames, CompareHints and signal wiring are preserved verbatim.
    auto* quality_panel = makePanel(left_col);
    quality_panel_ = quality_panel;
    auto* quality_layout = new QVBoxLayout(quality_panel);
    quality_layout->setContentsMargins(18, 16, 18, 18);
    quality_layout->setSpacing(0);
    quality_layout->addWidget(
        makeCardTitle(QStringLiteral("Quality & timing"), quality_panel, QStringLiteral("gauge")));

    // --- Quality row ---
    // Hidden combo is the single model-change emitter (existing test seam).
    quality_combo_ = new QComboBox(quality_panel);
    quality_combo_->setObjectName(QStringLiteral("videoQualityCombo"));
    quality_combo_->addItem(QStringLiteral("High Quality"), static_cast<int>(recorder_core::NvencQualityPreset::High));
    quality_combo_->addItem(QStringLiteral("Balanced"), static_cast<int>(recorder_core::NvencQualityPreset::Balanced));
    quality_combo_->addItem(QStringLiteral("Small"), static_cast<int>(recorder_core::NvencQualityPreset::Small));
    quality_combo_->setVisible(false);
    quality_combo_->setFocusPolicy(Qt::NoFocus);
    quality_layout->addWidget(quality_combo_);

    // v10: the Small/Balanced/High segmented control is preserved as a hidden test
    // seam (clicks + checked-state still drive the model), but the visible Default
    // presentation is a single "Balanced · CQ 24" dropdown (quality_preset_combo_)
    // built below. The segmented control is created here and added hidden so its
    // objectNames + qualitySegmentSelected property survive for tests.
    auto* quality_segmented = new QWidget(quality_panel);
    quality_segmented->setObjectName(QStringLiteral("qualitySegmented"));
    quality_segmented->setVisible(false);
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

    quality_segment_small_ = makeQualitySegment(QStringLiteral("qualitySegmentSmall"), QStringLiteral("Small"),
                                                recorder_core::NvencQualityPreset::Small);
    quality_segment_balanced_ = makeQualitySegment(QStringLiteral("qualitySegmentBalanced"), QStringLiteral("Balanced"),
                                                   recorder_core::NvencQualityPreset::Balanced);
    quality_segment_high_ = makeQualitySegment(QStringLiteral("qualitySegmentHigh"), QStringLiteral("High"),
                                               recorder_core::NvencQualityPreset::High);

    quality_compare_hint_ =
        new ui::widgets::CompareHint(QStringLiteral("quality"), QStringLiteral("Balanced"), quality_panel);
    // Hidden segmented control still needs to live in a layout so its widgets are
    // laid out / discoverable; park it (hidden) in the quality card.
    quality_layout->addWidget(quality_segmented);

    // v10: Default Quality presentation — a single dropdown "Balanced · CQ 24".
    // It mirrors the hidden quality_combo_ model seam: choosing an item drives the
    // same NvencQualityPreset path (onQualityPresetComboChanged → quality_combo_).
    {
        quality_preset_combo_ = new QComboBox(quality_panel);
        quality_preset_combo_->setObjectName(QStringLiteral("qualityPresetCombo"));
        quality_preset_combo_->addItem(QStringLiteral("Small \xc2\xb7 CQ 30"),
                                       static_cast<int>(recorder_core::NvencQualityPreset::Small));
        quality_preset_combo_->addItem(QStringLiteral("Balanced \xc2\xb7 CQ 24"),
                                       static_cast<int>(recorder_core::NvencQualityPreset::Balanced));
        quality_preset_combo_->addItem(QStringLiteral("High \xc2\xb7 CQ 19"),
                                       static_cast<int>(recorder_core::NvencQualityPreset::High));
        quality_preset_combo_->setFixedWidth(160);
        quality_preset_combo_->setProperty("settingsRowInput", true);

        quality_preset_row_widget_ = new QWidget(quality_panel);
        auto* qvl = new QVBoxLayout(quality_preset_row_widget_);
        qvl->setContentsMargins(0, 0, 0, 0);
        qvl->setSpacing(0);
        auto* qrule = new QFrame(quality_preset_row_widget_);
        qrule->setFrameShape(QFrame::HLine);
        qrule->setProperty("frameRole", "sectionRuleLine");
        qvl->addWidget(qrule);
        auto* qcontent = new QWidget(quality_preset_row_widget_);
        auto* qhl = new QHBoxLayout(qcontent);
        qhl->setContentsMargins(0, 12, 0, 12);
        qhl->setSpacing(14);
        auto* qlabel_row = new QWidget(qcontent);
        auto* qlrl = new QHBoxLayout(qlabel_row);
        qlrl->setContentsMargins(0, 0, 0, 0);
        qlrl->setSpacing(4);
        auto* qlbl = new QLabel(QStringLiteral("Quality"), qlabel_row);
        qlbl->setProperty("labelRole", "settingsRowLabel");
        qlrl->addWidget(qlbl);
        // Single info affordance per row: the CompareHint IS the info-i here (it explains
        // every quality tier in its popover), so no extra InfoHintIcon beside it — two
        // identical "i" glyphs on one row read as a bug.
        qlrl->addWidget(quality_compare_hint_, 0, Qt::AlignVCenter);
        qlrl->addStretch();
        qhl->addWidget(qlabel_row, 1);
        qhl->addWidget(quality_preset_combo_, 0, Qt::AlignVCenter);
        qvl->addWidget(qcontent);
        quality_preset_row_widget_->setProperty("settingsRow", true);
        quality_layout->addWidget(quality_preset_row_widget_);
    }

    // Wave 2 Part B: the CQ precision spinbox row and the two Expert rate/format
    // sections below it are the heaviest interleaved subtree on the Quality and
    // Container cards; they build on first expert-enable (perf — kept off the default
    // non-expert build path, see buildFormatQualityExpertSections()). Record the
    // Quality-card slot where the rate section + CQ row get inserted (right after the
    // Default dropdown, before the frame-rate row).
    quality_expert_insert_index_ = quality_layout->count();

    // --- Frame rate row (Quality & timing card) ---
    frame_rate_combo_ = new QComboBox(quality_panel);
    frame_rate_combo_->setObjectName(QStringLiteral("frameRateCombo"));
    frame_rate_combo_->setAccessibleName(QStringLiteral("Frame rate"));
    frame_rate_combo_->setFixedWidth(160);
    frame_rate_combo_->setProperty("settingsRowInput", true);
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

    quality_layout->addWidget(makeSettingsRow(quality_panel, QStringLiteral("Frame rate"),
                                              new ui::widgets::InfoHintIcon(ui::hints::kFrameRate, quality_panel),
                                              QString(), frame_rate_combo_));

    // --- Frame timing row (Quality & timing card) ---
    // v10/Canon (SSelect): a compact dropdown, not a full-width segmented group.
    // itemData carries the timing id (1 = CFR, 0 = VFR) consumed by onTimingSelected;
    // updateTimingSelection() disables the VFR item when the container can't carry it.
    timing_combo_ = new QComboBox(quality_panel);
    timing_combo_->setObjectName(QStringLiteral("timingCombo"));
    timing_combo_->addItem(QStringLiteral("CFR"), 1);
    timing_combo_->addItem(QStringLiteral("VFR"), 0);
    timing_combo_->setFixedWidth(160);
    timing_combo_->setProperty("settingsRowInput", true);

    timing_compare_hint_ = new ui::widgets::CompareHint(QStringLiteral("timing"), QStringLiteral("CFR"), quality_panel);
    quality_layout->addWidget(
        makeSettingsRow(quality_panel, QStringLiteral("Frame timing"), timing_compare_hint_, QString(), timing_combo_));

    // --- Capture cursor row (Quality & timing card) ---
    cursor_check_ = new ui::widgets::ExoToggle(quality_panel);
    cursor_check_->setObjectName(QStringLiteral("captureCursorCheck"));
    cursor_check_->setOn(video_settings_.capture_cursor);
    // v0.9 polish: no info-i — "Capture cursor" is a self-explanatory boolean, not an
    // A/B tradeoff. Info-i is reserved for rows with a genuine choice to explain.
    // The row pointer is kept: the lazily built expert Frame pacing row is inserted
    // directly above it (buildFormatQualityExpertSections).
    capture_cursor_row_ =
        makeSettingsRow(quality_panel, QStringLiteral("Capture cursor"), nullptr, QString(), cursor_check_);
    quality_layout->addWidget(capture_cursor_row_);

    // --- PS-PHASE-C / v10: Expert sections (split across two cards) ---
    // The two Expert containers (Rate control + Bitrate + Frame pacing in the
    // Quality card; Bit depth, Colour range, Encoder preset, Keyframe interval,
    // HDR and Chroma in the Container card) are the heaviest interleaved subtree
    // here; they build on first expert-enable (buildFormatQualityExpertSections()).
    // Record the Container-card slot so the lazy build inserts the section before
    // the compat callout that follows.
    fmt_expert_insert_index_ = fmt_layout->count();

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

    // v10: the "✓ Current format: …" footer belongs to the Quality & timing card
    // (it summarises the resolved encode), mirroring the suite-settings.jsx footer.
    compat_ok_label_ = new QLabel(quality_panel);
    compat_ok_label_->setObjectName(QStringLiteral("compatOkLabel"));
    compat_ok_label_->setProperty("labelRole", "muted");

    // v0.9 polish: the VFR-availability note moved into the Frame timing compare popover
    // (SettingsCompareData "timing" subtitle) — inline helper text lives in the info-i now.
    quality_layout->addWidget(compat_ok_label_);

    // Pre-fill codec combos (D6: free choice, fills once)
    updateVideoCodecChoices();
    updateAudioCodecChoices();

    // v10 (Delta 5): stable two-column placement, priority-ordered, identical in
    // Default and Expert (Expert only adds rows in place + reveals Developer).
    //   Left  : Container & codecs · Quality & timing · Audio · Hotkeys · Developer(Expert)
    //   Right : Output · Webcam · Notifications & overlays · Updates · Appearance
    // Left cards Container/Quality/Audio are added in build order here; Hotkeys and
    // Developer are appended in the consolidation block (they are built later).
    // Right cards are parented to right_col but added to right_layout in one explicit,
    // target-ordered block (see the consolidation block after the Updates card).
    left_layout->addWidget(fmt_panel);
    left_layout->addWidget(quality_panel);

    // ---- AUDIO CARD (left column — v10) ----
    auto* audio_panel = makePanel(left_col);
    audio_panel_ = audio_panel;
    auto* audio_panel_layout = new QVBoxLayout(audio_panel);
    audio_panel_layout->setContentsMargins(18, 16, 18, 18);
    audio_panel_layout->setSpacing(10);
    audio_panel_layout->addWidget(makeCardTitle(QStringLiteral("Audio"), audio_panel, QStringLiteral("speaker")));

    // Helper: build a source row directly into a given layout+parent.
    // The per-row merge control is an ExoToggle labelled "Merge with above": when it
    // is on, the source folds into the track above instead of producing its own track.
    // The pointer written back through `merge_check` is bound to merge_with_above, so
    // toggle-on maps to merge_with_above = true (see the toggle handlers and sync).
    // SETTINGS-TIERS-R2: InfoHintIcon added after enabled check and after the merge toggle.
    auto makeSourceRowInto = [&](QVBoxLayout* target_layout, QWidget* target_parent, const QString& title,
                                 ui::widgets::ExoCheckBox*& enabled_check, ui::widgets::ExoToggle*& merge_check,
                                 QLabel*& source_label, ui::widgets::VUMeterWidget*& meter_out, QLabel*& db_label_out) {
        auto* row = new QHBoxLayout();
        row->setSpacing(8);

        enabled_check = new ui::widgets::ExoCheckBox(title, target_parent);
        db_label_out = new QLabel(QStringLiteral("–"), target_parent);
        db_label_out->setProperty("labelRole", "muted");
        db_label_out->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        db_label_out->setMinimumWidth(52);

        // Pill toggle + label pair for the per-row merge control (spec label, do not rename).
        merge_check = new ui::widgets::ExoToggle(target_parent);
        QLabel* merge_label = new QLabel(QStringLiteral("Merge with above"), target_parent);
        merge_label->setProperty("labelRole", "muted");

        row->addWidget(enabled_check);
        // v0.9 polish: no info-i on the plain enable toggle — a row carries at most one
        // info-i, and the genuine tradeoff here is the Merge/separate-track control below.
        row->addStretch();
        row->addWidget(db_label_out);
        row->addWidget(merge_label);
        row->addWidget(merge_check);
        // InfoHint for the "Merge with above" toggle.
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

    // Audio source rows follow the documented order APP, SYS, MIC (product-spec §5).
    // The APP row exists only while a specific application window is the capture
    // target; when it is hidden, SYS becomes the first visible row.

    // Application audio section — wrapped in a container widget that is shown for
    // Window targets and hidden for Display/Region targets. Its trailing rule
    // divides it from the SYS row below and disappears together with the section.
    app_row_section_ = new QWidget(audio_panel);
    app_row_section_->setObjectName(QStringLiteral("settingsAudioAppSection"));
    {
        auto* app_section_layout = new QVBoxLayout(app_row_section_);
        app_section_layout->setContentsMargins(0, 0, 0, 0);
        app_section_layout->setSpacing(audio_panel_layout->spacing());

        makeSourceRowInto(app_section_layout, app_row_section_, QStringLiteral("Application audio"), app_enabled_check_,
                          app_separate_check_, app_source_label_, audio_app_meter_, audio_app_db_label_);
        app_enabled_check_->setObjectName(QStringLiteral("settingsAudioAppCheck"));
        app_separate_check_->setObjectName(QStringLiteral("settingsAudioAppMerge"));
        audio_app_meter_->setObjectName(QStringLiteral("settingsAudioAppMeter"));
        audio_app_db_label_->setObjectName(QStringLiteral("settingsAudioAppDbLabel"));

        auto* app_rule = new QFrame(app_row_section_);
        app_rule->setFrameShape(QFrame::HLine);
        app_rule->setProperty("frameRole", "sectionRuleLine");
        app_section_layout->addWidget(app_rule);
    }
    audio_panel_layout->addWidget(app_row_section_);
    // Hidden by default — shown when target kind is Window.
    app_row_section_->setVisible(false);

    // System audio row (label + description change based on capture target kind):
    //   Display/Region → "Computer audio"
    //   Window        → "Other system audio"
    makeSourceRowInto(audio_panel_layout, audio_panel, QStringLiteral("Computer audio"), sys_enabled_check_,
                      sys_separate_check_, sys_source_label_, audio_sys_meter_, audio_sys_db_label_);
    sys_enabled_check_->setObjectName(QStringLiteral("settingsAudioSysCheck"));
    sys_separate_check_->setObjectName(QStringLiteral("settingsAudioSysMerge"));
    audio_sys_meter_->setObjectName(QStringLiteral("settingsAudioSysMeter"));
    audio_sys_db_label_->setObjectName(QStringLiteral("settingsAudioSysDbLabel"));

    audio_panel_layout->addWidget(makeHRule(audio_panel));
    makeSourceRowInto(audio_panel_layout, audio_panel, QStringLiteral("Microphone"), mic_enabled_check_,
                      mic_separate_check_, mic_source_label_, audio_mic_meter_, audio_mic_db_label_);
    mic_enabled_check_->setObjectName(QStringLiteral("settingsAudioMicCheck"));
    mic_separate_check_->setObjectName(QStringLiteral("settingsAudioMicMerge"));
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
        // S3: editable combo with type-to-filter autocomplete for long device name lists.
        mic_device_combo_->setEditable(true);
        mic_device_combo_->setInsertPolicy(QComboBox::NoInsert);
        {
            auto* completer = new QCompleter(mic_device_combo_->model(), mic_device_combo_);
            completer->setFilterMode(Qt::MatchContains);
            completer->setCompletionMode(QCompleter::PopupCompletion);
            completer->setCaseSensitivity(Qt::CaseInsensitive);
            mic_device_combo_->setCompleter(completer);
        }
        mic_rl->addWidget(mic_device_combo_, 1);
        audio_rescan_btn_ = new QPushButton(mic_row); // #09: icon-only rescan button
        audio_rescan_btn_->setObjectName(QStringLiteral("audioRescanBtn"));
        audio_rescan_btn_->setProperty("role", "ghost");
        audio_rescan_btn_->setToolTip(QStringLiteral("Rescan audio devices"));
        audio_rescan_btn_->setFixedWidth(36);
        audio_rescan_btn_->setCursor(Qt::PointingHandCursor);
        {
            // Themed lucide glyph in HT.mut — the previous currentColor SVG inherited
            // the lighter ghost-button text colour (suite-settings.jsx:65 wants mut).
            const qreal dpr = audio_rescan_btn_->devicePixelRatioF();
            audio_rescan_btn_->setIcon(ui::theme::lucideIcon(QStringLiteral("refresh-cw"),
                                                             QString::fromUtf8(ui::theme::ActiveTheme().mut), 14, dpr));
            audio_rescan_btn_->setIconSize(QSize(14, 14));
        }
        mic_rl->addWidget(audio_rescan_btn_);
        audio_panel_layout->addWidget(mic_row);
    }

    // SETTINGS-TIERS-R1 Phase 1b: the "Separate track" toggles stay in their own
    // source rows (beside the enabled check and dB label).  An expander is wrong
    // here because each toggle is a per-row control, not a cohesive Advanced group.
    // audio_separate_expander_ remains null; the store field is harmless / unused.

    // PS-PHASE-C: Expert Audio section is built lazily on first expert-enable
    // (perf — keeps the heaviest ~480 LOC / 24-widget subtree off the default
    // ConfigPage build path; default profile is non-expert). Record the slot so
    // the lazy build inserts it above the trailing hint + summary.
    audio_expert_insert_index_ = audio_panel_layout->count();

    // v0.9 polish: the "separate tracks…" explanation moved into the per-row
    // Merge/separate-track info-i (kSeparateTrack) — no card-level inline helper text.
    audio_summary_label_ = new QLabel(audio_panel);
    audio_summary_label_->setProperty("labelRole", "muted");
    audio_summary_label_->setWordWrap(true);
    audio_summary_label_->setVisible(false);
    audio_panel_layout->addWidget(audio_summary_label_);
    left_layout->addWidget(audio_panel);

    // ---- WEBCAM CARD (right column — v10) ----
    auto* webcam_panel = makePanel(right_col);
    webcam_panel_ = webcam_panel;
    auto* webcam_panel_layout = new QVBoxLayout(webcam_panel);
    webcam_panel_layout->setContentsMargins(18, 16, 18, 18);
    webcam_panel_layout->setSpacing(10);
    webcam_panel_layout->addWidget(
        makeCardTitle(QStringLiteral("Webcam"), webcam_panel, QStringLiteral("camera"),
                      new ui::widgets::InfoHintIcon(ui::hints::kWebcamPlacement, webcam_panel)));

    webcam_setup_panel_ = new ui::widgets::WebcamSetupPanel(webcam_panel);
    webcam_setup_panel_->setObjectName(QStringLiteral("settingsWebcamSetupPanel"));
    webcam_panel_layout->addWidget(webcam_setup_panel_);
    // webcam_panel added to right_layout in the consolidation block below.

    // ---- OUTPUT CARD (right column — v10) ----
    auto* out_panel = makePanel(right_col);
    out_panel_ = out_panel;
    auto* out_panel_layout = new QVBoxLayout(out_panel);
    out_panel_layout->setContentsMargins(18, 16, 18, 18);
    out_panel_layout->setSpacing(12);
    out_panel_layout->addWidget(makeCardTitle(QStringLiteral("Output"), out_panel, QStringLiteral("folder")));

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
        // Canon row language: label left, control right on the same line.
        output_res_combo_ = new QComboBox(res_label_row);
        output_res_combo_->setMinimumWidth(170);
        rll->addWidget(output_res_combo_, 0, Qt::AlignVCenter);
        out_panel_layout->addWidget(res_label_row);
    }
    output_res_combo_->setObjectName(QStringLiteral("outputResCombo"));
    output_res_combo_->addItem(QStringLiteral("Native"), static_cast<int>(OutputResolutionMode::Native));
    output_res_combo_->addItem(QStringLiteral("4K"), static_cast<int>(OutputResolutionMode::UHD2160));
    output_res_combo_->addItem(QStringLiteral("1440p"), static_cast<int>(OutputResolutionMode::QHD1440));
    output_res_combo_->addItem(QStringLiteral("1080p"), static_cast<int>(OutputResolutionMode::FHD1080));
    output_res_combo_->addItem(QStringLiteral("720p"), static_cast<int>(OutputResolutionMode::HD720));
    output_res_combo_->addItem(QStringLiteral("Custom"), static_cast<int>(OutputResolutionMode::Custom));

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

    // ---- Split recording (SPLIT-RECORDING-R1 / SPLIT-BY-SIZE-R1) — Wave 2: expert-gated section ----
    // Wave 2: the SettingsCardExpander was dissolved. Split controls now live in a plain
    // QWidget that is shown/hidden by updateExpertModeVisibility() together with the
    // developer card (same expert-mode gate, no per-card expander).
    // Split recording (expert-gated) — built lazily on first expert-enable.
    // Record the slot so the lazy build inserts it before the output-split field.
    split_expert_insert_index_ = out_panel_layout->count();

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
    browse_btn_->setIcon(ui::theme::lucideIcon(QStringLiteral("folder"),
                                               QString::fromUtf8(ui::theme::ActiveTheme().mut), 14,
                                               browse_btn_->devicePixelRatioF()));
    dest_row->addWidget(destination_edit_, 1);
    dest_row->addWidget(browse_btn_);
    output_fields_layout->addLayout(dest_row);

    folder_validation_label_ = makeHint(QString(), output_fields);
    folder_validation_label_->setVisible(false);
    output_fields_layout->addWidget(folder_validation_label_);

    {
        // Canon (suite-settings.jsx FilenamePatternEditor): the token chips sit
        // behind a small "tokens" disclosure on the label row, not permanently
        // below the input.
        auto* fn_row = new QWidget(output_fields);
        auto* fnl = new QHBoxLayout(fn_row);
        fnl->setContentsMargins(0, 0, 0, 0);
        fnl->setSpacing(4);
        fnl->addWidget(
            makeOutputSubLabelWithHint(QStringLiteral("Filename pattern"), ui::hints::kFilenamePattern, fn_row));
        fnl->addStretch();
        tokens_toggle_ = new QToolButton(fn_row);
        tokens_toggle_->setObjectName(QStringLiteral("tokensToggle"));
        tokens_toggle_->setText(QStringLiteral("tokens"));
        tokens_toggle_->setCheckable(true);
        tokens_toggle_->setChecked(false);
        tokens_toggle_->setCursor(Qt::PointingHandCursor);
        tokens_toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        tokens_toggle_->setIcon(ui::theme::lucideIcon(QStringLiteral("chevron-down"),
                                                      QString::fromUtf8(ui::theme::ActiveTheme().mut), 12,
                                                      tokens_toggle_->devicePixelRatioF()));
        fnl->addWidget(tokens_toggle_, 0, Qt::AlignVCenter);
        output_fields_layout->addWidget(fn_row);
    }
    naming_edit_ = new QLineEdit(output_fields);
    naming_edit_->setObjectName(QStringLiteral("namingEdit"));
    naming_edit_->setPlaceholderText(QStringLiteral("{datetime}_{app}_{title}"));
    output_fields_layout->addWidget(naming_edit_);

    // v10 (Task #4): token chips are ALWAYS VISIBLE below the pattern input.
    // ChipFlowWidget wraps naturally at any width. No toggle button needed.
    // Only tokens the FilenameBuilder actually resolves are shown.
    {
        const QStringList token_chips = {QStringLiteral("{datetime}"), QStringLiteral("{date}"),
                                         QStringLiteral("{time}"),     QStringLiteral("{app}"),
                                         QStringLiteral("{title}"),    QStringLiteral("{target}"),
                                         QStringLiteral("{profile}"),  QStringLiteral("{container}")};
        auto* chip_flow = new ChipFlowWidget(output_fields);
        chip_flow->setObjectName(QStringLiteral("tokenChipFlow"));
        token_chip_flow_ = chip_flow;
        for (const QString& token : token_chips) {
            auto* chip = new QLabel(token, chip_flow);
            chip->setProperty("labelRole", "tokenChip");
            chip_flow->addChip(chip);
        }
        // Collapsed by default; the "tokens" disclosure on the label row opens it.
        chip_flow->setVisible(false);
        if (tokens_toggle_) {
            connect(tokens_toggle_, &QToolButton::toggled, chip_flow, [this, chip_flow](bool open) {
                chip_flow->setVisible(open);
                tokens_toggle_->setIcon(ui::theme::lucideIcon(
                    open ? QStringLiteral("chevron-up") : QStringLiteral("chevron-down"),
                    QString::fromUtf8(ui::theme::ActiveTheme().mut), 12, tokens_toggle_->devicePixelRatioF()));
            });
        }
        output_fields_layout->addWidget(chip_flow);
    }

    pattern_validation_label_ = makeHint(QString(), output_fields);
    pattern_validation_label_->setVisible(false);
    output_fields_layout->addWidget(pattern_validation_label_);

    output_fields_layout->addStretch();

    // v10 (Task #4): output_help panel removed — chips are now in output_fields.
    // The split layout keeps output_fields full-width; output_split_layout_ is still
    // used by updateResponsiveLayout() to flip between horizontal/vertical direction.
    output_split_layout_->addWidget(output_fields, 1);
    out_panel_layout->addWidget(output_split);

    // "Open editor when finished" — a real row in the Output card, off by
    // default. Was an ADR-0031 debug-only roadmap dummy with no engine setting
    // behind it; now backed by PersistedAppSettings::open_editor_when_finished.
    {
        open_editor_when_finished_check_ = new ui::widgets::ExoToggle(out_panel);
        open_editor_when_finished_check_->setObjectName(QStringLiteral("openEditorWhenFinishedCheck"));
        open_editor_when_finished_check_->setOn(false);
        out_panel_layout->addWidget(makeSettingsRow(
            out_panel, QStringLiteral("Open editor when finished"),
            new ui::widgets::InfoHintIcon(
                QStringLiteral("When a recording stops it opens straight in Edit, preloaded with the clip. "
                               "Off: a notification offers Edit and Show-in-folder instead."),
                out_panel),
            QString(), open_editor_when_finished_check_));
    }

    // v10 (Delta 2): resolved "Saves to …\path" footer — mirrors the Quality &
    // timing card's "✓ Current format" footer. Shows the destination folder +
    // filename pattern + container resolved into a concrete example path. Updated by
    // updateExampleFilename() whenever folder / pattern / container change.
    output_saves_to_label_ = new QLabel(out_panel);
    output_saves_to_label_->setObjectName(QStringLiteral("outputSavesToLabel"));
    output_saves_to_label_->setProperty("labelRole", "muted");
    // v0.9 polish: keep the resolved path to one line, middle-elided to the available
    // width (a long path used to wrap to several lines). Ignored width lets the label
    // shrink instead of forcing the card wider; re-elided on resize via eventFilter.
    output_saves_to_label_->setWordWrap(false);
    output_saves_to_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    output_saves_to_label_->installEventFilter(this);
    out_panel_layout->addWidget(output_saves_to_label_);
    // out_panel added to right_layout in the consolidation block below.

    // ---- NOTIFICATIONS & OVERLAYS CARD (right column — SETTINGS-TIERS-P3) ----
    // v0.9 polish: renamed from "Presence" — the card gathers notifications,
    // on-screen overlays, and tray behaviour, which "Presence" undersold. The
    // bell glyph (notifications) stays; internal ids keep the presence_ prefix
    // ("settings/presence" deep-link target included).
    {
        auto* presence_panel = makePanel(right_col);
        presence_panel_ = presence_panel;
        auto* presence_layout = new QVBoxLayout(presence_panel);
        presence_layout->setContentsMargins(18, 14, 18, 14);
        presence_layout->setSpacing(0);
        presence_layout->addWidget(
            makeCardTitle(QStringLiteral("Notifications & overlays"), presence_panel, QStringLiteral("bell")));

        overlay_check_ = new ui::widgets::ExoToggle(presence_panel);
        overlay_check_->setObjectName(QStringLiteral("overlayCheck"));
        overlay_check_->setOn(true);
        presence_layout->addWidget(
            makeSettingsRow(presence_panel, QStringLiteral("Recording overlay"),
                            new ui::widgets::InfoHintIcon(ui::hints::kRecordingOverlay, presence_panel), QString(),
                            overlay_check_, /*first=*/true));

        diagnostics_overlay_check_ = new ui::widgets::ExoToggle(presence_panel);
        diagnostics_overlay_check_->setObjectName(QStringLiteral("diagnosticsOverlayCheck"));
        diagnostics_overlay_check_->setOn(false);
        presence_layout->addWidget(
            makeSettingsRow(presence_panel, QStringLiteral("Diagnostics overlay"),
                            new ui::widgets::InfoHintIcon(
                                ui::hints::kDiagnosticsOverlay +
                                    QStringLiteral("\n\nRead-only and capture-excluded — injects nothing into any "
                                                   "process. Some anti-cheat systems may still flag third-party "
                                                   "overlays; disable it if you hit issues."),
                                presence_panel),
                            QString(), diagnostics_overlay_check_));

        notifications_check_ = new ui::widgets::ExoToggle(presence_panel);
        notifications_check_->setObjectName(QStringLiteral("notificationsCheck"));
        notifications_check_->setOn(true);
        presence_layout->addWidget(makeSettingsRow(
            presence_panel, QStringLiteral("Notifications"),
            new ui::widgets::InfoHintIcon(ui::hints::kNotifications, presence_panel), QString(), notifications_check_));

        keep_in_tray_check_ = new ui::widgets::ExoToggle(presence_panel);
        keep_in_tray_check_->setObjectName(QStringLiteral("keepInTrayCheck"));
        keep_in_tray_check_->setOn(false);
        presence_layout->addWidget(makeSettingsRow(
            presence_panel, QStringLiteral("Tray behavior"),
            new ui::widgets::InfoHintIcon(ui::hints::kCloseToTray, presence_panel), QString(), keep_in_tray_check_));

        quick_controls_check_ = new ui::widgets::ExoToggle(presence_panel);
        quick_controls_check_->setObjectName(QStringLiteral("quickControlsCheck"));
        quick_controls_check_->setOn(false);
        presence_layout->addWidget(
            makeSettingsRow(presence_panel, QStringLiteral("Quick controls"),
                            new ui::widgets::InfoHintIcon(ui::hints::kQuickControlPill, presence_panel), QString(),
                            quick_controls_check_));

        // ADR 0033: present & tearing diagnostics opt-in. Elevation-gated — turning
        // it on while non-elevated raises a hub advisory offering "restart as
        // administrator" (handled in MainWindow). Sub-hint flags the requirement.
        present_diag_check_ = new ui::widgets::ExoToggle(presence_panel);
        present_diag_check_->setObjectName(QStringLiteral("presentDiagnosticsToggle"));
        present_diag_check_->setOn(false);
        presence_layout->addWidget(makeSettingsRow(presence_panel,
                                                   QStringLiteral("Present, tearing & latency diagnostics"), nullptr,
                                                   QStringLiteral("Needs administrator"), present_diag_check_));

        // (0.6.0: the former "Per-track gain / mute" roadmap placeholder was removed —
        // per-track gain and mute shipped in 0.6.0 and are configured in the Record
        // view, so a dimmed "upcoming" row here is stale.)

        // presence_panel added to right_layout in the consolidation block below.
    }

    // ---- APPEARANCE CARD (right column — THEME-SLICE-1) ----
    {
        auto* appearance_panel = makePanel(right_col);
        appearance_panel_ = appearance_panel;
        auto* appearance_layout = new QVBoxLayout(appearance_panel);
        appearance_layout->setContentsMargins(18, 14, 18, 14);
        appearance_layout->setSpacing(0);
        appearance_layout->addWidget(
            makeCardTitle(QStringLiteral("Appearance"), appearance_panel, QStringLiteral("palette")));

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

        // Theme option = a clickable PREVIEW card: a per-theme mini-UI swatch with
        // the theme name + accent dot below it. The four cards remain checkable
        // QPushButtons carrying the themePickerCard + themeId properties (test seam)
        // and feed theme_button_group_. Selecting one applies the theme live; the
        // checked card is marked via the themePickerCard:checked QSS rule.
        auto makeThemeOption = [&](const ui::theme::ExoTheme& t) -> QPushButton* {
            auto* card = new QPushButton(theme_grid);
            card->setCheckable(true);
            card->setAutoDefault(false);
            card->setDefault(false);
            card->setCursor(Qt::PointingHandCursor);
            card->setProperty("themePickerCard", true);
            card->setProperty("themeOption", true);
            card->setProperty("themeId", QString::fromUtf8(t.id));
            card->setObjectName(QStringLiteral("themeCard_") + QString::fromUtf8(t.id));
            card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            // A QPushButton's sizeHint ignores its child layout, so force enough
            // height for the 82px swatch + spacing + name row + margins; otherwise
            // the name overlaps the swatch (see themePickerCard QSS note).
            card->setMinimumHeight(130);

            auto* card_layout = new QVBoxLayout(card);
            card_layout->setContentsMargins(10, 10, 10, 9);
            card_layout->setSpacing(8);

            // Per-theme preview swatch — fills the card width, fixed aspect.
            // Transparent for mouse events so the whole card stays the click target.
            auto* swatch = new ThemePreviewSwatch(t, card);
            swatch->setCardFill(true);
            swatch->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            card_layout->addWidget(swatch);

            // Name row: accent dot + theme name, centred under the swatch.
            auto* name_row = new QWidget(card);
            name_row->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            auto* name_layout = new QHBoxLayout(name_row);
            name_layout->setContentsMargins(0, 0, 0, 0);
            name_layout->setSpacing(8);
            name_layout->addStretch(1);
            auto* dot = new QLabel(name_row);
            dot->setFixedSize(10, 10);
            dot->setStyleSheet(
                QStringLiteral("border-radius:5px; background:%1; border:1px solid rgba(255,255,255,0.18);")
                    .arg(QString::fromUtf8(t.ac)));
            auto* name_lbl = new QLabel(QString::fromUtf8(t.name), name_row);
            name_lbl->setProperty("labelRole", "settingsRowLabel");
            name_layout->addWidget(dot, 0, Qt::AlignVCenter);
            name_layout->addWidget(name_lbl, 0, Qt::AlignVCenter);
            name_layout->addStretch(1);
            card_layout->addWidget(name_row);
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
            auto* card = makeThemeOption(t);
            theme_button_group_->addButton(card, btn_id++);
            if (QString::fromUtf8(t.group) == QStringLiteral("Dark"))
                dark_row_layout->addWidget(card);
            else
                light_row_layout->addWidget(card);
        }

        theme_grid_layout->addWidget(dark_row);
        theme_grid_layout->addSpacing(6);
        theme_grid_layout->addWidget(light_label);
        theme_grid_layout->addWidget(light_row);

        appearance_layout->addWidget(theme_grid);

        // appearance_panel added to right_layout in the consolidation block below.
    }

    // ---- PS-PHASE-C: HOTKEYS CARD (LEFT column — v10, single-width) ----
    // v10 masonry: Hotkeys belongs to left column (below Audio).
    {
        hotkeys_panel_ = makePanel(left_col);
        hotkeys_panel_->setObjectName(QStringLiteral("settingsHotkeysCard"));
        auto* hotkeys_panel_layout = new QVBoxLayout(hotkeys_panel_);
        hotkeys_panel_layout->setContentsMargins(18, 16, 18, 18);
        hotkeys_panel_layout->setSpacing(10);

        // Panel built first so its wired "Reset all" can be hoisted into the card title
        // row as a header badge — canon has no stray reset row and no card-in-card frame.
        hotkeys_settings_panel_ = new ui::widgets::HotkeysSettingsPanel(hotkeys_panel_);
        hotkeys_settings_panel_->setObjectName(QStringLiteral("settingsHotkeysPanel"));
        hotkeys_panel_layout->addWidget(makeCardTitle(QStringLiteral("Hotkeys"), hotkeys_panel_,
                                                      QStringLiteral("keyboard"),
                                                      hotkeys_settings_panel_->resetAllButton()));
        hotkeys_panel_layout->addWidget(hotkeys_settings_panel_);
        // hotkeys_panel_ added to left_layout in the consolidation block below.
    }

    // Developer card (expert-gated, UI-only stubs) is built lazily on first
    // expert-enable — see buildDeveloperCard().

    // ---- UPDATES CARD (right column, between Notifications & overlays and Appearance) ----
    // v10 masonry: Updates lives in the right column. The full update service is not
    // wired through ConfigPage; controls are stubs that preserve the real shape so the
    // layout is stable and future wiring is a drop-in.
    {
        updates_panel_ = makePanel(right_col);
        updates_panel_->setObjectName(QStringLiteral("settingsUpdatesCard"));
        auto* updates_layout = new QVBoxLayout(updates_panel_);
        updates_layout->setContentsMargins(18, 14, 18, 14);
        updates_layout->setSpacing(0);
        updates_layout->addWidget(makeCardTitle(QStringLiteral("Updates"), updates_panel_, QStringLiteral("download")));

        // Auto-check toggle (ADR 0034). We notify on a new release; we never
        // auto-install (no Update-channel control — Stable is the only channel).
        updates_auto_toggle_ = new ui::widgets::ExoToggle(updates_panel_);
        updates_auto_toggle_->setObjectName(QStringLiteral("updatesAutoCheckToggle"));
        updates_auto_toggle_->setOn(true);
        updates_layout->addWidget(makeSettingsRow(
            updates_panel_, QStringLiteral("Check for updates automatically"),
            new ui::widgets::InfoHintIcon(
                QStringLiteral("Check in the background and notify you when a new version is available."),
                updates_panel_),
            QString(), updates_auto_toggle_, /*first=*/true));
        connect(updates_auto_toggle_, &ui::widgets::ExoToggle::toggled, this, &ConfigPage::autoUpdateCheckToggled);

        // Status + primary action (ADR 0034 Phase A): status text left, the
        // "Check for updates" / "Update to vX.Y" button right.
        auto* updates_action_row = new QWidget(updates_panel_);
        auto* updates_action_layout = new QHBoxLayout(updates_action_row);
        updates_action_layout->setContentsMargins(0, 10, 0, 0);
        updates_action_layout->setSpacing(8);
        updates_status_label_ = makeHint(QStringLiteral("\xe2\x9c\x93 Up to date"), updates_action_row);
        updates_status_label_->setObjectName(QStringLiteral("updatesStatusLabel"));
        updates_action_layout->addWidget(updates_status_label_, 1, Qt::AlignVCenter);
        updates_action_btn_ = new QPushButton(QStringLiteral("Check for updates"), updates_action_row);
        updates_action_btn_->setObjectName(QStringLiteral("updatesActionButton"));
        updates_action_btn_->setCursor(Qt::PointingHandCursor);
        updates_action_layout->addWidget(updates_action_btn_, 0, Qt::AlignVCenter);
        updates_layout->addWidget(updates_action_row);
        connect(updates_action_btn_, &QPushButton::clicked, this, [this]() {
            if (updates_available_version_.isEmpty())
                emit checkForUpdatesRequested();
            else
                emit updatePrimaryActionRequested();
        });

        // WHATS-NEW: "What's new in vX.Y" link — shown only in the available state
        // (setUpdateStatus toggles visibility). Opens the gap-aware notes overlay.
        updates_whats_new_link_ = new QPushButton(updates_panel_);
        updates_whats_new_link_->setObjectName(QStringLiteral("updatesWhatsNewLink"));
        updates_whats_new_link_->setProperty("cardTextLink", true);
        updates_whats_new_link_->setFlat(true);
        updates_whats_new_link_->setCursor(Qt::PointingHandCursor);
        updates_whats_new_link_->setVisible(false);
        updates_layout->addWidget(updates_whats_new_link_, 0, Qt::AlignLeft);
        connect(updates_whats_new_link_, &QPushButton::clicked, this, &ConfigPage::whatsNewRequested);

        // updates_panel_ added to right_layout in the consolidation block below.
    }

    // ---- v10 (Delta 5): COLUMN CONSOLIDATION (stable, priority order) ----
    // Left column: Container & codecs · Quality & timing · Audio · Hotkeys · Developer(Expert).
    // Right column: Output · Webcam · Notifications & overlays · Updates · Appearance.
    // Expert only reveals rows in place / shows Developer card — nothing teleports sideways.
    left_layout->addWidget(hotkeys_panel_);
    developer_insert_index_ = left_layout->count(); // slot for the lazy developer card
    left_layout->addStretch();

    right_layout->addWidget(out_panel);
    right_layout->addWidget(webcam_panel);
    right_layout->addWidget(presence_panel_);
    right_layout->addWidget(updates_panel_);
    right_layout->addWidget(appearance_panel_);
    right_layout->addStretch();

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
    connect(overlay_check_, &QAbstractButton::toggled, this, &ConfigPage::showOverlayChanged);
    connect(diagnostics_overlay_check_, &QAbstractButton::toggled, this, &ConfigPage::showDiagnosticsOverlayChanged);
    connect(notifications_check_, &QAbstractButton::toggled, this, &ConfigPage::showNotificationsChanged);
    connect(open_editor_when_finished_check_, &QAbstractButton::toggled, this,
            &ConfigPage::openEditorWhenFinishedChanged);
    connect(keep_in_tray_check_, &QAbstractButton::toggled, this, &ConfigPage::keepRunningInTrayChanged);
    connect(quick_controls_check_, &QAbstractButton::toggled, this, &ConfigPage::showQuickControlsChanged);
    connect(present_diag_check_, &QAbstractButton::toggled, this, &ConfigPage::presentDiagnosticsOptInToggled);
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

    connect(container_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || !container_combo_)
            return;
        onContainerChanged(container_combo_->itemData(index).toInt());
    });
    connect(video_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onVideoCodecChanged);
    // The Container-card expert control connects (bit depth, chroma, colour range,
    // HDR, encoder preset, frame pacing, keyframe interval) live in
    // buildFormatQualityExpertSections() — those widgets don't exist until the first
    // expert-enable builds them lazily.
    connect(audio_codec_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onAudioCodecChanged);
    connect(profile_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onProfileSelectionChanged);
    connect(quality_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ConfigPage::onQualityChanged);
    connect(quality_segment_group_, &QButtonGroup::idClicked, this, &ConfigPage::onQualitySegmentSelected);
    // v10: the visible Default dropdown drives the same model seam (quality_combo_).
    if (quality_preset_combo_) {
        connect(quality_preset_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            if (index < 0 || !quality_combo_)
                return;
            const int preset_id = quality_preset_combo_->itemData(index).toInt();
            const int seam_index = quality_combo_->findData(preset_id);
            if (seam_index >= 0 && quality_combo_->currentIndex() != seam_index)
                quality_combo_->setCurrentIndex(seam_index);
        });
    }
    connect(frame_rate_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onFrameRateChanged);
    connect(timing_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || !timing_combo_)
            return;
        onTimingSelected(timing_combo_->itemData(index).toInt());
    });
    connect(output_res_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0)
            return;
        onOutputResolutionSelected(output_res_combo_->itemData(index).toInt());
    });
    // Split connects live in buildSplitExpertSection() (lazy build).
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
    // Relay the panel's shared-capture consumer request out to MainWindow (→ coordinator).
    connect(webcam_setup_panel_, &ui::widgets::WebcamSetupPanel::previewActiveRequested, this,
            &ConfigPage::webcamPreviewActiveRequested);
    // Preset management connections — toolbar buttons.
    connect(preset_save_as_btn_, &QPushButton::clicked, this, &ConfigPage::onSavePresetAs);
    connect(preset_reset_btn_, &QPushButton::clicked, this, &ConfigPage::resetChangesRequested);
    connect(preset_delete_btn_, &QPushButton::clicked, this, &ConfigPage::onDeletePreset);
    // Preset management connections — overflow menu.
    connect(save_preset_as_action_, &QAction::triggered, this, &ConfigPage::onSavePresetAs);
    connect(rename_preset_action_, &QAction::triggered, this, &ConfigPage::onRenamePreset);
    connect(export_preset_action_, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export Preset"), QString(),
                                                          QStringLiteral("TOML files (*.toml)"));
        if (!path.isEmpty())
            emit exportCurrentPresetRequested(path);
    });
    connect(import_presets_action_, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import Presets"), QString(),
                                                          QStringLiteral("TOML files (*.toml)"));
        if (!path.isEmpty())
            emit importPresetsRequested(path);
    });
    connect(view_details_btn_, &QPushButton::clicked, this, &ConfigPage::diagnosticsRequested);
    // token_help_toggle_btn_ removed (v10: token chips are permanently visible).

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

    // The Quality-card expert control connects (CQ spinbox, Rate control, Bitrate)
    // live in buildFormatQualityExpertSections() — those widgets don't exist until
    // the first expert-enable builds them lazily.

    // PS-PHASE-C: Audio expert control connects live in buildAudioExpertSection()
    // (lazy build — the widgets they bind don't exist until first expert-enable).

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
    // Seed the colour-range combo from the model at construction. Without this
    // the combo silently showed item 0 ("Full (PC)") regardless of the model —
    // a latent hydration bug masked while the model default happened to BE
    // Full; exposed when the default flipped to Limited (fix/color-range-signaling).
    updateVideoColorRangeControl();
    // Seed the encoder-preset combo from the model at construction (same
    // hydration-bug class as the colour-range combo above).
    updateVideoEncoderPresetControl();
    updateExampleFilename();
    updateQualitySegmentSelection();
    updateFrameRateSelection();
    updateTimingSelection();
    updateOutputResolutionSelection();
    updateResponsiveLayout();

    QPointer<ConfigPage> safe = this;
    QTimer::singleShot(0, this, [safe]() {
        if (safe)
            safe->refreshMicDevices();
    });
}

ConfigPage::~ConfigPage() {
    // Qt tears the widget tree down after this body has run, and a hidden child may still
    // emit on its way out: WebcamSetupPanel::hideEvent stops its preview, which relays
    // previewActiveRequested back into this page. By then the dynamic type is no longer
    // ConfigPage, so member-slot dispatch would run on a half-destroyed object. Sever the
    // inbound connections while this is still a ConfigPage.
    if (webcam_setup_panel_)
        webcam_setup_panel_->disconnect(this);
}

void ConfigPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateResponsiveLayout();
}

bool ConfigPage::eventFilter(QObject* watched, QEvent* event) {
    // Swallow wheel events on the CQ spinbox unless it holds focus, so scrolling
    // past it does not change the recording quality unnoticed.
    if (watched == quality_cq_spin_ && event->type() == QEvent::Wheel && !quality_cq_spin_->hasFocus()) {
        event->ignore();
        return true;
    }
    // Re-elide the resolved "Saves as <path>" footer when the label is resized.
    if (watched == output_saves_to_label_ && event->type() == QEvent::Resize) {
        applyOutputSavesToElision();
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
    // A named preset selects its canonical CQ value; CQ stays the single source.
    video_settings_.cq = recorder_core::CanonicalCq(
        static_cast<recorder_core::NvencQualityPreset>(quality_combo_->itemData(index).toInt()));
    updateQualitySegmentSelection();
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
    updateQualitySegmentSelection();
    updateFormatDisplay();
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
    updateQualitySegmentSelection();
    updateFormatDisplay();
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
    video_settings_.capture_cursor = cursor_check_->isOn();
    emitCurrentVideoSettings();
}

void ConfigPage::reconcileContainerCodecRules() {
    // The resolver owns the container × codec and timing rules; the page copies
    // its decision into the fields this reconcile manages (codecs + CFR). Bit
    // depth and chroma stay owned by their dedicated control updaters below,
    // which apply the same resolver predicates.
    const capability::OutputFormatReconciliation reconciled = capability::ReconcileOutputFormat(
        {format_settings_.container, format_settings_.video_codec, format_settings_.audio_codec,
         format_settings_.bit_depth, format_settings_.chroma_subsampling, video_settings_.cfr});
    format_settings_.video_codec = reconciled.resolved.video_codec;
    format_settings_.audio_codec = reconciled.resolved.audio_codec;
    video_settings_.cfr = reconciled.resolved.cfr;
    SanitizeOutputResolution(format_settings_.resolution);
    updateVideoCodecChoices();
    updateAudioCodecChoices();
}

void ConfigPage::updateVideoCodecChoices() {
    const QSignalBlocker blocker(video_codec_combo_);
    if (video_codec_combo_->count() == 0) {
        video_codec_combo_->addItem(QStringLiteral("AV1"), VideoCodecToInt(capability::VideoCodec::Av1Nvenc));
        video_codec_combo_->addItem(QStringLiteral("H.264"), VideoCodecToInt(capability::VideoCodec::H264Nvenc));
        // HEVC (0.7.0 — S3): GPU-verified end-to-end (MKV V_MPEGH/ISO/HEVC + MP4 hvc1).
        // A normal, always-present choice; container reconcile falls back to AV1/H.264
        // when the selected container cannot carry HEVC.
        video_codec_combo_->addItem(QStringLiteral("HEVC"), VideoCodecToInt(capability::VideoCodec::HevcNvenc));
    }
    const int vidx = video_codec_combo_->findData(VideoCodecToInt(format_settings_.video_codec));
    if (vidx >= 0)
        video_codec_combo_->setCurrentIndex(vidx);

    // Bit-depth selectability depends on the selected codec — refresh it here.
    updateVideoBitDepthControl();
    // HDR10-native selectability depends on the selected codec — refresh it here too.
    updateVideoHdrModeControl();
    // 4:4:4 selectability depends on the selected codec — refresh it here too.
    updateVideoChromaControl();
}

void ConfigPage::updateVideoBitDepthControl() {
    if (!video_bit_depth_combo_ || !video_bit_depth_row_)
        return;

    // 10-bit (HEVC Main10 / AV1 10-bit P010, SDR BT.709 — ADR 0032) is valid only
    // for HEVC and AV1, never H.264. The rule lives in the resolver
    // (capability::CodecSupports10Bit); the UI only reads the answer.
    const auto codec = format_settings_.video_codec;
    const bool supports_10bit = capability::CodecSupports10Bit(codec);
    const bool locked = controls_locked_;

    // If 10-bit was selected but is no longer valid for the codec, snap the model
    // back to 8-bit (mirrors SanitizePresetConfig / RecordingCoordinator reconcile).
    if (!supports_10bit && format_settings_.bit_depth == capability::BitDepth::Bit10) {
        format_settings_.bit_depth = capability::BitDepth::Bit8;
    }

    // Relevance gate (v0.9 polish): the row exists only when the selected codec
    // carries 10-bit at all. An 8-bit-only codec (H.264) offers no choice, so the
    // whole row is hidden rather than listed with a permanently disabled item —
    // the snap-back above has already reconciled the stored value, so no stale
    // 10-bit state is reachable while the row is gated out.
    video_bit_depth_row_->setVisible(supports_10bit);

    // Enable/disable the 10-bit item per codec. Disabled items use Forbidden cursor
    // + a tooltip explaining the requirement (existing disabled-row pattern).
    if (auto* model = qobject_cast<QStandardItemModel*>(video_bit_depth_combo_->model())) {
        const int ten_idx = video_bit_depth_combo_->findData(static_cast<int>(capability::BitDepth::Bit10));
        if (auto* item = (ten_idx >= 0) ? model->item(ten_idx) : nullptr) {
            item->setEnabled(supports_10bit);
            item->setToolTip(supports_10bit ? QString() : QStringLiteral("10-bit requires HEVC or AV1"));
        }
    }

    // Sync the combo selection to the model (signal-blocked).
    {
        const QSignalBlocker b(video_bit_depth_combo_);
        const int idx = video_bit_depth_combo_->findData(static_cast<int>(format_settings_.bit_depth));
        video_bit_depth_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* 8-bit */);
    }

    video_bit_depth_combo_->setEnabled(!locked);
    if (!supports_10bit) {
        // Whole-row affordance: tooltip + forbidden cursor explain why 10-bit is out.
        video_bit_depth_combo_->setToolTip(QStringLiteral("10-bit requires HEVC or AV1"));
        video_bit_depth_combo_->setCursor(Qt::ForbiddenCursor);
    } else if (locked) {
        video_bit_depth_combo_->setToolTip(QStringLiteral("Cannot change during recording"));
        video_bit_depth_combo_->setCursor(Qt::ForbiddenCursor);
    } else {
        video_bit_depth_combo_->setToolTip(QString());
        video_bit_depth_combo_->unsetCursor();
    }
}

void ConfigPage::updateVideoChromaControl() {
    if (!video_chroma_combo_ || !video_chroma_row_)
        return;

    // 4:4:4 (AYUV, NVENC High 4:4:4 / HEVC FREXT) is an 8-bit H.264/HEVC-only expert
    // path — AV1 NVENC is 4:2:0 only and 4:4:4 + 10-bit is out of scope. The rule
    // lives in the resolver (capability::CodecSupportsChroma444); the UI reads it.
    const auto codec = format_settings_.video_codec;
    const bool codec_ok = capability::CodecSupportsChroma444(codec);
    const bool eight_bit = format_settings_.bit_depth == capability::BitDepth::Bit8;
    // Once the async probe has delivered runtime capabilities, consult the ACTIVE
    // GPU's real per-codec YUV444 support; before that, gpu_ok is true so the
    // static codec/bit-depth rule stands unchanged (pre-probe behavior).
    const bool gpu_ok = !runtime_caps_set_ || capability::IsSelectable(runtime_caps_.QueryChroma444(codec));
    const bool supports_444 = codec_ok && eight_bit && gpu_ok;
    const bool locked = controls_locked_;

    // Snap the model back to 4:2:0 when 4:4:4 is no longer valid (mirrors bit depth /
    // SanitizePresetConfig / RecordingCoordinator reconcile).
    if (!supports_444 && format_settings_.chroma_subsampling == capability::ChromaSubsampling::Cs444) {
        format_settings_.chroma_subsampling = capability::ChromaSubsampling::Cs420;
    }

    // Relevance gate (v0.9 polish): the row exists only when the selected codec
    // and the active GPU can carry 4:4:4 at all (H.264/HEVC on a GPU with YUV444
    // encode). AV1 — or a GPU whose probe reported no YUV444 path — hides the
    // whole row; the snap-back above already reconciled any stored 4:4:4. A
    // bit-depth conflict (10-bit selected) is user-fixable right here in the
    // expert view, so it stays a disabled item + calm hint instead of hiding.
    const bool row_relevant = codec_ok && gpu_ok;
    video_chroma_row_->setVisible(row_relevant);

    // Reason string is three-tiered: bit-depth/codec first, then the per-GPU
    // downgrade when codec+bit-depth are fine but the active GPU can't carry it.
    QString reason = eight_bit ? QStringLiteral("4:4:4 requires H.264 or HEVC")
                               : QStringLiteral("4:4:4 requires 8-bit H.264 or HEVC");
    if (codec_ok && eight_bit && !gpu_ok) {
        reason = QStringLiteral("4:4:4 is not supported by this GPU");
    }

    // Enable/disable the 4:4:4 item per codec/bit-depth.
    if (auto* model = qobject_cast<QStandardItemModel*>(video_chroma_combo_->model())) {
        const int idx444 = video_chroma_combo_->findData(static_cast<int>(capability::ChromaSubsampling::Cs444));
        if (auto* item = (idx444 >= 0) ? model->item(idx444) : nullptr) {
            item->setEnabled(supports_444);
            item->setToolTip(supports_444 ? QString() : reason);
        }
    }

    // Sync the combo selection to the model (signal-blocked).
    {
        const QSignalBlocker b(video_chroma_combo_);
        const int idx = video_chroma_combo_->findData(static_cast<int>(format_settings_.chroma_subsampling));
        video_chroma_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* 4:2:0 */);
    }

    video_chroma_combo_->setEnabled(!locked);
    if (!supports_444) {
        video_chroma_combo_->setToolTip(reason);
        video_chroma_combo_->setCursor(Qt::ForbiddenCursor);
    } else if (locked) {
        video_chroma_combo_->setToolTip(QStringLiteral("Cannot change during recording"));
        video_chroma_combo_->setCursor(Qt::ForbiddenCursor);
    } else {
        video_chroma_combo_->setToolTip(QString());
        video_chroma_combo_->unsetCursor();
    }

    if (video_chroma_hint_) {
        // The hint only accompanies a visible row (bit-depth conflict); a row
        // gated out for codec/GPU reasons takes its hint with it.
        video_chroma_hint_->setVisible(row_relevant && !supports_444);
    }
}

void ConfigPage::updateVideoColorRangeControl() {
    if (!video_color_range_combo_ || !video_color_range_row_)
        return;

    // Colour range (Full 0-255 / Limited 16-235) is ALWAYS valid for every codec and
    // container — there is no capability gating here (unlike bit depth). Only the
    // recording lock disables it. Keep the combo in sync with the model.
    const bool locked = controls_locked_;
    {
        const QSignalBlocker b(video_color_range_combo_);
        const int idx = video_color_range_combo_->findData(static_cast<int>(format_settings_.color_range));
        video_color_range_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* Full */);
    }

    video_color_range_combo_->setEnabled(!locked);
    if (locked) {
        video_color_range_combo_->setToolTip(QStringLiteral("Cannot change during recording"));
        video_color_range_combo_->setCursor(Qt::ForbiddenCursor);
    } else {
        video_color_range_combo_->setToolTip(QString());
        video_color_range_combo_->unsetCursor();
    }
}

void ConfigPage::updateVideoHdrModeControl() {
    if (!video_hdr_mode_combo_ || !video_hdr_mode_row_)
        return;

    // HDR10-native support is a codec-format fact (HEVC/AV1 carry it, H.264 never
    // does), not a per-GPU probe, so the static capability baseline is authoritative
    // here — gate on capability::QueryHdr10Native rather than comparing codec names
    // locally (same single source of truth the rec.hdr.h264 pre-flight blocker uses).
    static const capability::CapabilitySet kHdrCaps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    const bool supports_hdr10 = capability::IsSelectable(kHdrCaps.QueryHdr10Native(format_settings_.video_codec));
    const bool locked = controls_locked_;

    // Relevance gate (v0.9 polish): the HDR-handling row exists only once a
    // display that actively reports an HDR colour space is present — the same
    // "only an HDR-active display is treated as HDR" rule the engine and the
    // rec.hdr.h264 blocker apply (product-spec §6). On an SDR-only system the
    // choice has no effect either way, so the row is hidden entirely. Display
    // facts arrive with the async runtime probe (setRuntimeCapabilities); until
    // then the row stays hidden. The stored hdr_mode is deliberately left
    // untouched while gated out: without an HDR-active display it changes
    // nothing at recording time, and it comes back when the display does.
    bool hdr_display_present = false;
    if (runtime_caps_set_) {
        const auto& displays = runtime_caps_.runtime.displays;
        hdr_display_present = std::any_of(displays.begin(), displays.end(),
                                          [](const capability::DisplayHdrFacts& d) { return d.hdr_active; });
    }
#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
    if (visual_hdr_display_override_set_)
        hdr_display_present = visual_hdr_display_override_;
#endif
    video_hdr_mode_row_->setVisible(hdr_display_present);

    // Unlike bit depth, an Hdr10 selection is NEVER snapped back to TonemapSdr here
    // when the codec becomes incompatible (e.g. a container reconcile lands on
    // H.264). The stored model value is left alone; the live pre-flight blocker
    // (rec.hdr.h264) owns that conflict at recording time. This control only
    // disables the item and shows a calm hint.

    if (auto* model = qobject_cast<QStandardItemModel*>(video_hdr_mode_combo_->model())) {
        const int hdr10_idx = video_hdr_mode_combo_->findData(static_cast<int>(recorder_core::HdrMode::Hdr10));
        if (auto* item = (hdr10_idx >= 0) ? model->item(hdr10_idx) : nullptr) {
            item->setEnabled(supports_hdr10);
            item->setToolTip(supports_hdr10
                                 ? QString()
                                 : QStringLiteral("Not available with H.264 \xe2\x80\x94 switch to AV1 or HEVC"));
        }
    }

    // Sync the combo selection to the model (signal-blocked). The model value may
    // legitimately be Hdr10 while the item above is disabled (see comment above) —
    // the combo still displays the stored selection rather than lying about it.
    {
        const QSignalBlocker b(video_hdr_mode_combo_);
        const int idx = video_hdr_mode_combo_->findData(static_cast<int>(format_settings_.hdr_mode));
        video_hdr_mode_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* Tone-map to SDR */);
    }

    video_hdr_mode_combo_->setEnabled(!locked);
    if (locked) {
        video_hdr_mode_combo_->setToolTip(QStringLiteral("Cannot change during recording"));
        video_hdr_mode_combo_->setCursor(Qt::ForbiddenCursor);
    } else if (!supports_hdr10) {
        video_hdr_mode_combo_->setToolTip(
            QStringLiteral("Not available with H.264 \xe2\x80\x94 switch to AV1 or HEVC"));
        video_hdr_mode_combo_->unsetCursor();
    } else {
        video_hdr_mode_combo_->setToolTip(QString());
        video_hdr_mode_combo_->unsetCursor();
    }

    if (video_hdr_mode_hint_)
        // The H.264 hint only accompanies a visible row — a row gated out by the
        // display check takes its hint with it.
        video_hdr_mode_hint_->setVisible(hdr_display_present && !supports_hdr10);
}

void ConfigPage::updateVideoEncoderPresetControl() {
    if (!video_encoder_preset_combo_ || !video_encoder_preset_row_)
        return;

    // Every NVENC preset (P1..P7) is ALWAYS valid for every codec and container —
    // no capability gating here. Only the recording lock disables it.
    const bool locked = controls_locked_;
    {
        const QSignalBlocker b(video_encoder_preset_combo_);
        const int idx = video_encoder_preset_combo_->findData(static_cast<int>(format_settings_.nvenc_preset));
        video_encoder_preset_combo_->setCurrentIndex(idx >= 0 ? idx : 3 /* P4 */);
    }

    video_encoder_preset_combo_->setEnabled(!locked);
    if (locked) {
        video_encoder_preset_combo_->setToolTip(QStringLiteral("Cannot change during recording"));
        video_encoder_preset_combo_->setCursor(Qt::ForbiddenCursor);
    } else {
        video_encoder_preset_combo_->setToolTip(QString());
        video_encoder_preset_combo_->unsetCursor();
    }
}

void ConfigPage::updateFramePacingControl() {
    if (!frame_pacing_combo_ || !frame_pacing_row_)
        return;

    // Frame pacing is ALWAYS valid for every codec and container — no capability
    // gating here. Only the recording lock disables it.
    const bool locked = controls_locked_;
    {
        const QSignalBlocker b(frame_pacing_combo_);
        const int idx = frame_pacing_combo_->findData(static_cast<int>(video_settings_.frame_pacing));
        frame_pacing_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* Smooth */);
    }

    frame_pacing_combo_->setEnabled(!locked);
    if (locked) {
        frame_pacing_combo_->setToolTip(QStringLiteral("Cannot change during recording"));
        frame_pacing_combo_->setCursor(Qt::ForbiddenCursor);
    } else {
        frame_pacing_combo_->setToolTip(QString());
        frame_pacing_combo_->unsetCursor();
    }
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

    // ADR 0030: after rebuilding / reselecting the codec, refresh format-control visibility.
    updateAudioFormatControlVisibility();
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

    const QString summary = containerLabel(c) + QStringLiteral(" \xC2\xB7 ") + videoCodecLabel(v) +
                            QStringLiteral(" \xC2\xB7 ") + audioCodecLabel(a) + QStringLiteral(" \xC2\xB7 ") +
                            frameRateLabel(video_settings_.frame_rate_num, video_settings_.frame_rate_den) +
                            QStringLiteral(" \xC2\xB7 ") +
                            (video_settings_.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR"));

    if (compat_callout_widget_)
        compat_callout_widget_->setVisible(compat_bad);
    if (compat_ok_label_)
        compat_ok_label_->setVisible(!compat_bad);

    if (compat_bad && callout_text_) {
        QStringList bad_parts;
        if (video_bad)
            bad_parts << videoCodecLabel(v);
        if (audio_bad)
            bad_parts << audioCodecLabel(a);
        const QString bad_str = bad_parts.join(QStringLiteral(" + "));
        QString hint;
        if (c == capability::Container::WebM)
            hint = QStringLiteral("WebM supports AV1 + Opus only.");
        else if (c == capability::Container::Mp4)
            hint = QStringLiteral("MP4 supports H.264 + AAC only.");
        callout_text_->setText(containerLabel(c) + QStringLiteral(" can't hold ") + bad_str + QStringLiteral(". ") +
                               hint);
    }
    if (!compat_bad && compat_ok_label_) {
        compat_ok_label_->setText(QStringLiteral("\xe2\x9c\x93 Current format: ") + summary);
    }

    // Container combo sync (was in updateFormatDisplay)
    if (container_combo_) {
        const QSignalBlocker blocker(container_combo_);
        const int idx = container_combo_->findData(static_cast<int>(format_settings_.container));
        if (idx >= 0 && container_combo_->currentIndex() != idx)
            container_combo_->setCurrentIndex(idx);
    }

    // CompareHint value sync
    if (container_compare_hint_)
        container_compare_hint_->setCurrentValue(containerLabel(format_settings_.container));
    if (video_codec_compare_hint_)
        video_codec_compare_hint_->setCurrentValue(videoCodecLabel(format_settings_.video_codec));
    if (audio_codec_compare_hint_)
        audio_codec_compare_hint_->setCurrentValue(audioCodecLabel(format_settings_.audio_codec));
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
    // Codec change can invalidate a 10-bit selection — refresh the bit-depth gating
    // (also snaps the model back to 8-bit when the new codec can't carry 10-bit).
    updateVideoBitDepthControl();
    // Codec change can also invalidate an Hdr10 selection — refresh the HDR gating
    // live. Unlike bit depth this does NOT snap the stored value back (see
    // updateVideoHdrModeControl()).
    updateVideoHdrModeControl();
    // 4:4:4 depends on the codec (H.264/HEVC only) — refresh + snap back if needed.
    updateVideoChromaControl();
    emitCurrentFormatSettings();
}

void ConfigPage::onVideoBitDepthChanged(int index) {
    if (index < 0 || !video_bit_depth_combo_)
        return;
    const auto requested = static_cast<capability::BitDepth>(video_bit_depth_combo_->itemData(index).toInt());
    // Guard: 10-bit is only honored for HEVC / AV1 (the disabled item should prevent
    // this, but keep the model authoritative regardless of how the index changed).
    const bool supports_10bit = format_settings_.video_codec == capability::VideoCodec::HevcNvenc ||
                                format_settings_.video_codec == capability::VideoCodec::Av1Nvenc;
    format_settings_.bit_depth = (requested == capability::BitDepth::Bit10 && supports_10bit)
                                     ? capability::BitDepth::Bit10
                                     : capability::BitDepth::Bit8;
    updateVideoBitDepthControl();
    // 4:4:4 is 8-bit only — a 10-bit switch must snap chroma back to 4:2:0.
    updateVideoChromaControl();
    emitCurrentFormatSettings();
}

void ConfigPage::onVideoChromaChanged(int index) {
    if (index < 0 || !video_chroma_combo_)
        return;
    const auto requested = static_cast<capability::ChromaSubsampling>(video_chroma_combo_->itemData(index).toInt());
    // Guard: 4:4:4 is only honored for 8-bit H.264/HEVC AND (once probed) a GPU
    // that supports it. The disabled item should prevent this, but keep the model
    // authoritative regardless of how the index changed. Mirrors updateVideoChromaControl.
    const auto codec = format_settings_.video_codec;
    const bool codec_ok = codec == capability::VideoCodec::HevcNvenc || codec == capability::VideoCodec::H264Nvenc;
    const bool gpu_ok = !runtime_caps_set_ || capability::IsSelectable(runtime_caps_.QueryChroma444(codec));
    const bool supports_444 = codec_ok && format_settings_.bit_depth == capability::BitDepth::Bit8 && gpu_ok;
    format_settings_.chroma_subsampling = (requested == capability::ChromaSubsampling::Cs444 && supports_444)
                                              ? capability::ChromaSubsampling::Cs444
                                              : capability::ChromaSubsampling::Cs420;
    updateVideoChromaControl();
    emitCurrentFormatSettings();
}

void ConfigPage::onVideoColorRangeChanged(int index) {
    if (index < 0 || !video_color_range_combo_)
        return;
    // Both values are always valid — no codec/container gating. Just record the model.
    format_settings_.color_range =
        static_cast<capability::ColorRange>(video_color_range_combo_->itemData(index).toInt());
    updateVideoColorRangeControl();
    emitCurrentFormatSettings();
}

void ConfigPage::onVideoHdrModeChanged(int index) {
    if (index < 0 || !video_hdr_mode_combo_)
        return;
    const auto requested = static_cast<recorder_core::HdrMode>(video_hdr_mode_combo_->itemData(index).toInt());
    // Guard: Hdr10 requires a codec that carries it natively (the disabled item
    // should prevent this from a user click, but keep the model authoritative
    // regardless of how the index changed — mirrors the bit-depth guard).
    static const capability::CapabilitySet kHdrCaps = capability::CapabilityBuilder::BuildStaticValidatedBaseline();
    const bool supports_hdr10 = capability::IsSelectable(kHdrCaps.QueryHdr10Native(format_settings_.video_codec));
    format_settings_.hdr_mode = (requested == recorder_core::HdrMode::Hdr10 && !supports_hdr10)
                                    ? recorder_core::HdrMode::TonemapSdr
                                    : requested;
    updateVideoHdrModeControl();
    emitCurrentFormatSettings();
}

void ConfigPage::onVideoEncoderPresetChanged(int index) {
    if (index < 0 || !video_encoder_preset_combo_)
        return;
    // Every preset is valid for every codec/container — no gating. Just record the model.
    format_settings_.nvenc_preset =
        static_cast<recorder_core::NvencPreset>(video_encoder_preset_combo_->itemData(index).toInt());
    updateVideoEncoderPresetControl();
    emitCurrentFormatSettings();
}

void ConfigPage::onAudioCodecChanged(int index) {
    if (index < 0)
        return;
    format_settings_.audio_codec = IntToAudioCodec(audio_codec_combo_->itemData(index).toInt());
    emitCurrentFormatSettings();
    // ADR 0030: update visibility of bit-depth and FLAC-level controls, and
    // lock / unlock the sample-rate selector based on codec.
    updateAudioFormatControlVisibility();
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
    format_settings_.bit_depth = settings.bit_depth;
    format_settings_.chroma_subsampling = settings.chroma_subsampling;
    format_settings_.color_range = settings.color_range;
    format_settings_.nvenc_preset = settings.nvenc_preset;
    format_settings_.hdr_mode = settings.hdr_mode;
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

    if (container_combo_) {
        const QSignalBlocker blocker(container_combo_);
        const int idx = container_combo_->findData(static_cast<int>(settings.container));
        if (idx >= 0)
            container_combo_->setCurrentIndex(idx);
    }

    updateVideoCodecChoices();
    updateAudioCodecChoices();
    updateVideoColorRangeControl();
    updateVideoEncoderPresetControl();
    updateFormatDisplay();
    updateOutputResolutionSelection();
    updateCustomResolutionVisibility();
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
    const int qidx = quality_combo_->findData(static_cast<int>(recorder_core::NearestQualityPreset(settings.cq)));
    if (qidx >= 0)
        quality_combo_->setCurrentIndex(qidx);

    // Sync the CQ spinbox from the loaded preset's exact value.
    if (quality_cq_spin_) {
        const QSignalBlocker sb(quality_cq_spin_);
        quality_cq_spin_->setValue(static_cast<int>(settings.cq));
    }

    updateFrameRateSelection();
    updateTimingSelection();

    const QSignalBlocker crb(cursor_check_);
    cursor_check_->setOn(settings.capture_cursor);

    // PS-PHASE-C: sync expert rate control + bitrate from loaded preset.
    if (rate_control_combo_) {
        const QSignalBlocker rb(rate_control_combo_);
        const int idx = rate_control_combo_->findData(static_cast<int>(settings.rate_control));
        if (idx >= 0)
            rate_control_combo_->setCurrentIndex(idx);
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

    // ADR 0035 Slice 2: sync frame-pacing combo from loaded preset.
    if (frame_pacing_combo_) {
        const QSignalBlocker pb(frame_pacing_combo_);
        const int idx = frame_pacing_combo_->findData(static_cast<int>(settings.frame_pacing));
        frame_pacing_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* Smooth */);
    }

    updateQualitySegmentSelection();

    // The "✓ Current format" summary reads video_settings_.frame_rate_*/cfr
    // directly (see updateCompatCallout()). setVideoSettings() is the
    // programmatic path (preset load, visual-test harness) and every combo
    // sync above runs behind a QSignalBlocker, so nothing downstream would
    // otherwise refresh the summary label — it used to go stale showing
    // whatever was current when the page was constructed.
    updateFormatDisplay();
}

void ConfigPage::updateQualitySegmentSelection() {
    if (!quality_segment_group_)
        return;

    const auto sync_segment = [this](QPushButton* segment, recorder_core::NvencQualityPreset preset) {
        if (!segment)
            return;

        const bool selected = recorder_core::NearestQualityPreset(video_settings_.cq) == preset;
        segment->setChecked(selected);
        segment->setProperty("qualitySegmentSelected", selected);
        segment->style()->unpolish(segment);
        segment->style()->polish(segment);
    };

    const QSignalBlocker blocker(quality_segment_group_);
    sync_segment(quality_segment_small_, recorder_core::NvencQualityPreset::Small);
    sync_segment(quality_segment_balanced_, recorder_core::NvencQualityPreset::Balanced);
    sync_segment(quality_segment_high_, recorder_core::NvencQualityPreset::High);

    // v10: keep the visible Default dropdown in sync with the model preset.
    if (quality_preset_combo_) {
        const QSignalBlocker b(quality_preset_combo_);
        const int idx =
            quality_preset_combo_->findData(static_cast<int>(recorder_core::NearestQualityPreset(video_settings_.cq)));
        if (idx >= 0)
            quality_preset_combo_->setCurrentIndex(idx);
    }

    if (quality_compare_hint_) {
        switch (recorder_core::NearestQualityPreset(video_settings_.cq)) {
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

    // Mirror the model's CQ value. When the user is typing, this is already the
    // value in the box, so it is a no-op -- it never overwrites the input.
    if (quality_cq_spin_) {
        const QSignalBlocker b(quality_cq_spin_);
        quality_cq_spin_->setValue(static_cast<int>(video_settings_.cq));
    }
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
    if (!timing_combo_)
        return;

    const bool vfr_available = format_settings_.container != capability::Container::Mp4;
    if (!vfr_available && !video_settings_.cfr) {
        video_settings_.cfr = true;
    }

    {
        const QSignalBlocker blocker(timing_combo_);
        // Disable the VFR item (data 0) when the container can't carry VFR (MP4).
        if (auto* model = qobject_cast<QStandardItemModel*>(timing_combo_->model())) {
            const int vfr_idx = timing_combo_->findData(0);
            if (auto* item = (vfr_idx >= 0) ? model->item(vfr_idx) : nullptr) {
                item->setEnabled(vfr_available);
                item->setToolTip(vfr_available
                                     ? QString()
                                     : QStringLiteral("VFR is not available for MP4 in the current mux path."));
            }
        }
        const int idx = timing_combo_->findData(video_settings_.cfr ? 1 : 0);
        if (idx >= 0 && timing_combo_->currentIndex() != idx)
            timing_combo_->setCurrentIndex(idx);
        timing_combo_->setEnabled(!controls_locked_);
    }

    if (timing_compare_hint_)
        timing_compare_hint_->setCurrentValue(video_settings_.cfr ? QStringLiteral("CFR") : QStringLiteral("VFR"));
}

void ConfigPage::updateOutputResolutionSelection() {
    if (!output_res_combo_)
        return;

    {
        const QSignalBlocker blocker(output_res_combo_);
        const int idx = output_res_combo_->findData(static_cast<int>(format_settings_.resolution.mode));
        if (idx >= 0 && output_res_combo_->currentIndex() != idx)
            output_res_combo_->setCurrentIndex(idx);
    }

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
    emitCurrentFormatSettings();
}

void ConfigPage::onCustomHeightChanged(int value) {
    if (value <= 0)
        return;
    format_settings_.resolution.custom_height = static_cast<uint32_t>(value);
    SanitizeOutputResolution(format_settings_.resolution);
    updateCustomResolutionVisibility();
    emitCurrentFormatSettings();
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

void ConfigPage::setPresetOptions(const std::vector<ProfileOption>& options, const QString& selected_id, bool dirty) {
    profile_options_ = options;
    active_preset_id_ = selected_id;
    preset_dirty_ = dirty;

    const QSignalBlocker blocker(profile_combo_);
    profile_combo_->clear();
    int active_index = -1;
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto& opt = options[i];
        profile_combo_->addItem(opt.label, opt.id);
        // Carry built-in-ness so PresetOptionDelegate can badge the option row.
        profile_combo_->setItemData(static_cast<int>(i), opt.built_in, kPresetBuiltInRole);
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
    const bool locked = controls_locked_;
    const bool user_preset = !active_preset_id_.isEmpty() && !active_preset_is_built_in_;

    // Status badge (unavailable only): a separate toolbar label. The built-in
    // marker now renders as a "Built-in" badge inside the combo option rows
    // (PresetOptionDelegate) so it no longer sits beside the combo and shifts
    // the toolbar layout.
    if (profile_status_label_) {
        QString badge;
        if (!active_preset_is_available_) {
            badge = QStringLiteral("Unavailable");
            profile_status_label_->setProperty("stateRole", "blocked");
        }
        profile_status_label_->setText(badge);
        profile_status_label_->setVisible(!badge.isEmpty());
        profile_status_label_->style()->unpolish(profile_status_label_);
        profile_status_label_->style()->polish(profile_status_label_);
    }

    // Save as new / Reset: shown exactly while the live config is (changed).
    if (preset_save_as_btn_) {
        preset_save_as_btn_->setVisible(preset_dirty_);
        preset_save_as_btn_->setEnabled(preset_dirty_ && !locked);
    }
    if (preset_reset_btn_) {
        preset_reset_btn_->setVisible(preset_dirty_);
        preset_reset_btn_->setEnabled(preset_dirty_ && !locked);
    }
    // Delete: shown for a selected user preset, regardless of (changed).
    if (preset_delete_btn_) {
        preset_delete_btn_->setVisible(user_preset);
        preset_delete_btn_->setEnabled(user_preset && !locked);
    }

    // Menu actions.
    if (save_preset_as_action_)
        save_preset_as_action_->setEnabled(!locked); // permanently reachable
    if (rename_preset_action_)
        rename_preset_action_->setEnabled(user_preset && !locked);
    if (export_preset_action_)
        export_preset_action_->setEnabled(!active_preset_id_.isEmpty());
    if (import_presets_action_)
        import_presets_action_->setEnabled(!locked);

    // "(changed)" hint in the combo text — informative, not a warning.
    if (profile_combo_) {
        const int idx = profile_combo_->currentIndex();
        if (idx >= 0 && idx < static_cast<int>(profile_options_.size())) {
            const QSignalBlocker blocker(profile_combo_);
            const QString base = profile_options_[static_cast<std::size_t>(idx)].label;
            profile_combo_->setItemText(idx, preset_dirty_ ? base + QStringLiteral(" (changed)") : base);
        }
    }
}

bool ConfigPage::presetNameRejected(const QString& name, const std::vector<ProfileOption>& options,
                                    const QString& exclude_id) {
    const QString folded = name.trimmed().toCaseFolded();
    if (folded.isEmpty())
        return true;
    for (const auto& opt : options) {
        if (opt.id == exclude_id)
            continue;
        if (opt.label.trimmed().toCaseFolded() == folded)
            return true;
    }
    return false;
}

void ConfigPage::onSavePresetAs() {
    QString name = active_profile_name_;
    for (;;) {
        bool ok = false;
        name = QInputDialog::getText(this, QStringLiteral("Save as new preset"), QStringLiteral("Preset name:"),
                                     QLineEdit::Normal, name, &ok);
        if (!ok)
            return;
        if (!presetNameRejected(name, profile_options_, QString()))
            break;
        QMessageBox::warning(this, QStringLiteral("Save as new preset"),
                             QStringLiteral("That name is empty or already in use. Preset names are unique."));
    }
    emit savePresetAsRequested(name.trimmed());
}

void ConfigPage::onRenamePreset() {
    QString name = active_profile_name_;
    for (;;) {
        bool ok = false;
        name = QInputDialog::getText(this, QStringLiteral("Rename preset"), QStringLiteral("Preset name:"),
                                     QLineEdit::Normal, name, &ok);
        if (!ok)
            return;
        if (!presetNameRejected(name, profile_options_, active_preset_id_))
            break;
        QMessageBox::warning(this, QStringLiteral("Rename preset"),
                             QStringLiteral("That name is empty or already in use. Preset names are unique."));
    }
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
    const auto output_path =
        BuildOutputPath(format_settings_.output_folder, format_settings_.naming_pattern, format_settings_.container,
                        std::time(nullptr), ExamplePreviewContext(active_profile_name_, format_settings_));
    // Canon: one resolved footer line — "✓ Saves as <full path>". The separate
    // "Example:" line said the same thing twice. The path is kept full and
    // middle-elided to the label width (applyOutputSavesToElision).
    output_saves_to_full_ = QStringLiteral("\xe2\x9c\x93 Saves as  ") + QString::fromStdWString(output_path.wstring());
    applyOutputSavesToElision();
}

void ConfigPage::applyOutputSavesToElision() {
    if (!output_saves_to_label_)
        return;
    const int avail = output_saves_to_label_->width();
    const QString elided =
        avail > 0 ? output_saves_to_label_->fontMetrics().elidedText(output_saves_to_full_, Qt::ElideMiddle, avail)
                  : output_saves_to_full_;
    output_saves_to_label_->setText(elided);
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
        if (clock_slaving_check_) {
            const QSignalBlocker b(clock_slaving_check_);
            clock_slaving_check_->setChecked(audio_ui_state_.clock_slaving_enabled);
        }
        if (limiter_ceiling_spin_) {
            const QSignalBlocker b(limiter_ceiling_spin_);
            limiter_ceiling_spin_->setValue(static_cast<double>(audio_ui_state_.limiter_ceiling_db));
            limiter_ceiling_spin_->setVisible(audio_ui_state_.limiter_enabled);
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
        if (mic_rnnoise_check_) {
            const QSignalBlocker b(mic_rnnoise_check_);
            mic_rnnoise_check_->setChecked(audio_ui_state_.mic_rnnoise_enabled);
        }
        // Channel / sample-format model (ADR 0030 — 0.6.0).
        if (audio_sample_rate_combo_) {
            const QSignalBlocker b(audio_sample_rate_combo_);
            const int idx = audio_sample_rate_combo_->findData(static_cast<int>(audio_ui_state_.audio_sample_rate));
            if (idx >= 0)
                audio_sample_rate_combo_->setCurrentIndex(idx);
        }
        if (audio_channels_combo_) {
            const QSignalBlocker b(audio_channels_combo_);
            const int idx = audio_channels_combo_->findData(static_cast<int>(audio_ui_state_.audio_channels));
            if (idx >= 0)
                audio_channels_combo_->setCurrentIndex(idx);
        }
        if (audio_bit_depth_combo_) {
            const QSignalBlocker b(audio_bit_depth_combo_);
            const int idx = audio_bit_depth_combo_->findData(
                EncodeBitDepthComboData(audio_ui_state_.audio_bit_depth, audio_ui_state_.audio_pcm_float));
            if (idx >= 0)
                audio_bit_depth_combo_->setCurrentIndex(idx);
        }
        if (flac_compression_spin_) {
            const QSignalBlocker b(flac_compression_spin_);
            flac_compression_spin_->setValue(audio_ui_state_.flac_compression_level);
        }
        // Reapply codec-gated visibility after state change.
        updateAudioFormatControlVisibility();
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

        // The merge toggle reads "Merge with above": on == merge == !separate_track.
        app_enabled_check_->setEnabled(snap.app.controls_enabled);
        app_separate_check_->setEnabled(snap.app.controls_enabled);
        app_enabled_check_->setChecked(snap.app.enabled);
        app_separate_check_->setChecked(!snap.app.separate_track);

        mic_enabled_check_->setEnabled(snap.mic.controls_enabled);
        mic_separate_check_->setEnabled(snap.mic.controls_enabled);
        mic_enabled_check_->setChecked(snap.mic.enabled);
        mic_separate_check_->setChecked(!snap.mic.separate_track);

        sys_enabled_check_->setEnabled(snap.system.controls_enabled);
        sys_separate_check_->setEnabled(snap.system.controls_enabled);
        sys_enabled_check_->setChecked(snap.system.enabled);
        sys_separate_check_->setChecked(!snap.system.separate_track);
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

    // ADR 0030: update codec-gated visibility for format controls.
    updateAudioFormatControlVisibility();
}

void ConfigPage::emitCurrentAudioSettings() {
    emit audioSettingsChanged(audio_ui_state_);
}

void ConfigPage::updateAudioFormatControlVisibility() {
    // Called whenever the audio codec or recording-lock state changes.
    // Controls not created yet (expert mode off during construction) are guarded
    // by null checks.
    if (!expert_mode_enabled_)
        return;

    const bool is_opus = (format_settings_.audio_codec == capability::AudioCodec::Opus);
    const bool is_pcm = (format_settings_.audio_codec == capability::AudioCodec::Pcm);
    const bool is_flac = (format_settings_.audio_codec == capability::AudioCodec::Flac);
    const bool locked = controls_locked_;

    // Sample rate: present for all codecs; disabled + tooltip for Opus.
    if (audio_sample_rate_combo_) {
        audio_sample_rate_combo_->setEnabled(!locked && !is_opus);
        if (is_opus) {
            // Snap model and combo to 48000 when Opus is active.
            const QSignalBlocker b(audio_sample_rate_combo_);
            const int idx = audio_sample_rate_combo_->findData(48000);
            if (idx >= 0)
                audio_sample_rate_combo_->setCurrentIndex(idx);
            audio_ui_state_.audio_sample_rate = 48000u;
            audio_sample_rate_combo_->setToolTip(QStringLiteral("Opus records at 48\xC2\xa0kHz only"));
            audio_sample_rate_combo_->setCursor(Qt::ForbiddenCursor);
        } else if (locked) {
            audio_sample_rate_combo_->setToolTip(QStringLiteral("Cannot change during recording"));
            audio_sample_rate_combo_->setCursor(Qt::ForbiddenCursor);
        } else {
            audio_sample_rate_combo_->setToolTip(QString());
            audio_sample_rate_combo_->unsetCursor();
        }
    }

    // Channels: enabled for all codecs.
    if (audio_channels_combo_)
        audio_channels_combo_->setEnabled(!locked);

    // Bit depth: visible only for PCM or FLAC; items differ per codec.
    if (audio_bit_depth_row_) {
        const bool show_depth = is_pcm || is_flac;
        audio_bit_depth_row_->setVisible(show_depth);
        if (show_depth && audio_bit_depth_combo_) {
            const QSignalBlocker b(audio_bit_depth_combo_);
            // Rebuild items to match allowed set for this codec.
            audio_bit_depth_combo_->clear();
            audio_bit_depth_combo_->addItem(QStringLiteral("16-bit"), 16);
            audio_bit_depth_combo_->addItem(QStringLiteral("24-bit"), 24);
            if (is_pcm) {
                audio_bit_depth_combo_->addItem(QStringLiteral("32-bit"), 32);
                // 32-bit float (Float-PCM): PCM only -- libFLAC has no float mode.
                audio_bit_depth_combo_->addItem(QStringLiteral("32-bit float"), kFloatBitDepthItemData);
            }
            // Restore stored value; fall back to 16-bit int if not in list.
            const int idx = audio_bit_depth_combo_->findData(
                EncodeBitDepthComboData(audio_ui_state_.audio_bit_depth, audio_ui_state_.audio_pcm_float));
            audio_bit_depth_combo_->setCurrentIndex(idx >= 0 ? idx : 0);
            // Clamp model if stored depth is unavailable for this codec.
            if (idx < 0) {
                audio_ui_state_.audio_bit_depth = 16u;
                audio_ui_state_.audio_pcm_float = false;
            }
            audio_bit_depth_combo_->setEnabled(!locked);
        }
    }

    // FLAC compression level: visible only for FLAC.
    if (flac_compression_row_) {
        flac_compression_row_->setVisible(is_flac);
        if (is_flac && flac_compression_spin_)
            flac_compression_spin_->setEnabled(!locked);
    }

    // Audio bitrate: meaningful only for Opus/AAC (lossy codecs).
    if (audio_bitrate_kbps_spin_) {
        const bool bitrate_active = !is_pcm && !is_flac;
        audio_bitrate_kbps_spin_->setEnabled(bitrate_active && !locked);
        if (!bitrate_active) {
            audio_bitrate_kbps_spin_->setToolTip(
                QStringLiteral("Bitrate does not apply to lossless codecs (PCM/FLAC)"));
            audio_bitrate_kbps_spin_->setCursor(Qt::ForbiddenCursor);
        } else if (locked) {
            audio_bitrate_kbps_spin_->setToolTip(QStringLiteral("Cannot change during recording"));
            audio_bitrate_kbps_spin_->setCursor(Qt::ForbiddenCursor);
        } else {
            audio_bitrate_kbps_spin_->setToolTip(QString());
            audio_bitrate_kbps_spin_->unsetCursor();
        }
    }

    // Opus frame duration: meaningful only for Opus.
    if (opus_frame_duration_combo_) {
        const bool opus_frame_active = is_opus;
        opus_frame_duration_combo_->setEnabled(opus_frame_active && !locked);
        if (!opus_frame_active) {
            opus_frame_duration_combo_->setToolTip(
                QStringLiteral("Opus frame duration applies only when Opus codec is selected"));
            opus_frame_duration_combo_->setCursor(Qt::ForbiddenCursor);
        } else if (locked) {
            opus_frame_duration_combo_->setToolTip(QStringLiteral("Cannot change during recording"));
            opus_frame_duration_combo_->setCursor(Qt::ForbiddenCursor);
        } else {
            opus_frame_duration_combo_->setToolTip(QString());
            opus_frame_duration_combo_->unsetCursor();
        }
    }

    // Opus complexity: meaningful only for Opus.
    if (opus_complexity_spin_) {
        const bool opus_complexity_active = is_opus;
        opus_complexity_spin_->setEnabled(opus_complexity_active && !locked);
        if (!opus_complexity_active) {
            opus_complexity_spin_->setToolTip(
                QStringLiteral("Opus complexity applies only when Opus codec is selected"));
            opus_complexity_spin_->setCursor(Qt::ForbiddenCursor);
        } else if (locked) {
            opus_complexity_spin_->setToolTip(QStringLiteral("Cannot change during recording"));
            opus_complexity_spin_->setCursor(Qt::ForbiddenCursor);
        } else {
            opus_complexity_spin_->setToolTip(QString());
            opus_complexity_spin_->unsetCursor();
        }
    }
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

void ConfigPage::setAudioSeparateExpanderExpanded(bool /*expanded*/) {
    // Phase 1b + Wave 2: audio_separate_expander_ removed; no-op for compat.
}

bool ConfigPage::audioSeparateExpanderExpanded() const noexcept {
    // Phase 1b: always false — no audio separate expander widget exists.
    return false;
}

// SETTINGS-TIERS / startup-perf: the Expert audio subtree (~480 LOC, 24 widgets)
// is built on first expert-enable instead of eagerly-then-hidden, so the default
// non-expert ConfigPage construction never pays for it. All external access to
// these widgets is already null-guarded; updateExpertModeVisibility() re-seeds them.
void ConfigPage::buildAudioExpertSection() {
    if (audio_expert_built_)
        return;
    audio_expert_built_ = true;
    QWidget* audio_panel = audio_panel_; // alias: the moved construction references it
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
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Mic gain"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kMicGain, row), 0, Qt::AlignVCenter);
            hl->addStretch(1);

            const int init_db =
                static_cast<int>(std::roundf(20.f * std::log10f(std::max(0.001f, audio_ui_state_.mic_gain_linear))));

            // S3: ExoSlider with gradient groove + tick marks at -12, -6, 0 dB (unity), 6, 12.
            mic_gain_slider_ = new ui::widgets::ExoSlider(Qt::Horizontal, row);
            mic_gain_slider_->setObjectName(QStringLiteral("micGainSlider"));
            mic_gain_slider_->setRange(-12, 12);
            mic_gain_slider_->setSingleStep(1);
            mic_gain_slider_->setPageStep(3);
            mic_gain_slider_->setValue(init_db);
            mic_gain_slider_->setDefaultValue(0); // 0 dB = unity gain (prominent marker)
            mic_gain_slider_->setTickValues({-12, -6, 0, 6, 12});
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
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Mic channel mode"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kMicChannelMode, row), 0, Qt::AlignVCenter);
            hl->addStretch(1);
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
            mic_channel_mode_combo_->setFixedWidth(160);
            mic_channel_mode_combo_->setProperty("settingsRowInput", true);
            hl->addWidget(mic_channel_mode_combo_, 0, Qt::AlignVCenter);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);
        }

        // Capture the insertion point for the Microphone post-processing row, which is
        // built later but must appear visually here (immediately after Mic channel mode).
        int mic_post_insert_index = aes_layout->count();

        // Audio bitrate
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Audio bitrate"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kAudioBitrate, row), 0, Qt::AlignVCenter);
            hl->addStretch(1);
            audio_bitrate_kbps_spin_ = new QSpinBox(row);
            audio_bitrate_kbps_spin_->setObjectName(QStringLiteral("audioBitrateKbpsSpin"));
            audio_bitrate_kbps_spin_->setRange(32, 510);
            audio_bitrate_kbps_spin_->setSuffix(QStringLiteral(" kbps"));
            audio_bitrate_kbps_spin_->setValue(static_cast<int>(audio_ui_state_.audio_bitrate_kbps));
            audio_bitrate_kbps_spin_->setFixedWidth(160);
            audio_bitrate_kbps_spin_->setProperty("settingsRowInput", true);
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
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Opus frame duration"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kOpusFrameDuration, row), 0, Qt::AlignVCenter);
            hl->addStretch(1);
            opus_frame_duration_combo_ = new QComboBox(row);
            opus_frame_duration_combo_->setObjectName(QStringLiteral("opusFrameDurationCombo"));
            opus_frame_duration_combo_->addItem(QStringLiteral("20 ms"),
                                                static_cast<int>(recorder_core::OpusFrameDuration::Ms20));
            opus_frame_duration_combo_->addItem(QStringLiteral("10 ms"),
                                                static_cast<int>(recorder_core::OpusFrameDuration::Ms10));
            opus_frame_duration_combo_->addItem(QStringLiteral("5 ms"),
                                                static_cast<int>(recorder_core::OpusFrameDuration::Ms5));
            opus_frame_duration_combo_->setFixedWidth(160);
            opus_frame_duration_combo_->setProperty("settingsRowInput", true);
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
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Opus complexity"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kOpusComplexity, row), 0, Qt::AlignVCenter);
            hl->addStretch(1);
            opus_complexity_spin_ = new QSpinBox(row);
            opus_complexity_spin_->setObjectName(QStringLiteral("opusComplexitySpin"));
            opus_complexity_spin_->setRange(0, 10);
            opus_complexity_spin_->setValue(audio_ui_state_.opus_complexity);
            opus_complexity_spin_->setFixedWidth(160);
            opus_complexity_spin_->setProperty("settingsRowInput", true);
            hl->addWidget(opus_complexity_spin_, 0, Qt::AlignVCenter);
            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);
        }

        // Sample rate (ADR 0030 — 0.6.0)
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            audio_sample_rate_row_ = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(audio_sample_rate_row_);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Sample rate"), audio_sample_rate_row_);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kAudioSampleRate, audio_sample_rate_row_), 0,
                          Qt::AlignVCenter);
            hl->addStretch(1);
            audio_sample_rate_combo_ = new QComboBox(audio_sample_rate_row_);
            audio_sample_rate_combo_->setObjectName(QStringLiteral("audioSampleRateCombo"));
            audio_sample_rate_combo_->addItem(QStringLiteral("44100 Hz"), 44100);
            audio_sample_rate_combo_->addItem(QStringLiteral("48000 Hz"), 48000);
            audio_sample_rate_combo_->addItem(QStringLiteral("96000 Hz"), 96000);
            {
                const int idx = audio_sample_rate_combo_->findData(static_cast<int>(audio_ui_state_.audio_sample_rate));
                audio_sample_rate_combo_->setCurrentIndex(idx >= 0 ? idx : 1 /* 48000 */);
            }
            audio_sample_rate_combo_->setFixedWidth(160);
            audio_sample_rate_combo_->setProperty("settingsRowInput", true);
            hl->addWidget(audio_sample_rate_combo_, 0, Qt::AlignVCenter);
            audio_sample_rate_row_->setProperty("settingsRow", true);
            aes_layout->addWidget(audio_sample_rate_row_);
        }

        // Channels (ADR 0030 — 0.6.0)
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            audio_channels_row_ = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(audio_channels_row_);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Channels"), audio_channels_row_);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kAudioChannels, audio_channels_row_), 0,
                          Qt::AlignVCenter);
            hl->addStretch(1);
            audio_channels_combo_ = new QComboBox(audio_channels_row_);
            audio_channels_combo_->setObjectName(QStringLiteral("audioChannelsCombo"));
            audio_channels_combo_->addItem(QStringLiteral("Stereo"), 2);
            audio_channels_combo_->addItem(QStringLiteral("Mono"), 1);
            {
                const int idx = audio_channels_combo_->findData(static_cast<int>(audio_ui_state_.audio_channels));
                audio_channels_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* Stereo */);
            }
            audio_channels_combo_->setFixedWidth(160);
            audio_channels_combo_->setProperty("settingsRowInput", true);
            hl->addWidget(audio_channels_combo_, 0, Qt::AlignVCenter);
            audio_channels_row_->setProperty("settingsRow", true);
            aes_layout->addWidget(audio_channels_row_);
        }

        // Bit depth (ADR 0030 — 0.6.0): visible for PCM/FLAC only
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            audio_bit_depth_row_ = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(audio_bit_depth_row_);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Bit depth"), audio_bit_depth_row_);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kAudioBitDepth, audio_bit_depth_row_), 0,
                          Qt::AlignVCenter);
            hl->addStretch(1);
            audio_bit_depth_combo_ = new QComboBox(audio_bit_depth_row_);
            audio_bit_depth_combo_->setObjectName(QStringLiteral("audioBitDepthCombo"));
            // Items seeded for PCM (16/24/32/32-float); rebuilt when codec changes.
            audio_bit_depth_combo_->addItem(QStringLiteral("16-bit"), 16);
            audio_bit_depth_combo_->addItem(QStringLiteral("24-bit"), 24);
            audio_bit_depth_combo_->addItem(QStringLiteral("32-bit"), 32);
            audio_bit_depth_combo_->addItem(QStringLiteral("32-bit float"), kFloatBitDepthItemData);
            {
                const int idx = audio_bit_depth_combo_->findData(
                    EncodeBitDepthComboData(audio_ui_state_.audio_bit_depth, audio_ui_state_.audio_pcm_float));
                audio_bit_depth_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* 16 */);
            }
            audio_bit_depth_combo_->setFixedWidth(160);
            audio_bit_depth_combo_->setProperty("settingsRowInput", true);
            hl->addWidget(audio_bit_depth_combo_, 0, Qt::AlignVCenter);
            audio_bit_depth_row_->setProperty("settingsRow", true);
            audio_bit_depth_row_->setVisible(false); // shown only for PCM/FLAC
            aes_layout->addWidget(audio_bit_depth_row_);
        }

        // FLAC compression level (ADR 0030 — 0.6.0): visible for FLAC only
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            flac_compression_row_ = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(flac_compression_row_);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("FLAC compression"), flac_compression_row_);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kFlacCompression, flac_compression_row_), 0,
                          Qt::AlignVCenter);
            hl->addStretch(1);
            flac_compression_spin_ = new QSpinBox(flac_compression_row_);
            flac_compression_spin_->setObjectName(QStringLiteral("flacCompressionLevelSpin"));
            flac_compression_spin_->setRange(0, 8);
            flac_compression_spin_->setValue(audio_ui_state_.flac_compression_level);
            flac_compression_spin_->setFixedWidth(160);
            flac_compression_spin_->setProperty("settingsRowInput", true);
            hl->addWidget(flac_compression_spin_, 0, Qt::AlignVCenter);
            flac_compression_row_->setProperty("settingsRow", true);
            flac_compression_row_->setVisible(false); // shown only for FLAC
            aes_layout->addWidget(flac_compression_row_);
        }

        // Brickwall limiter (Audio v2 — 0.6.0) — Slice 3 (cogwheels -> inline): the
        // cogwheel popover is gone. Plain inline toggle; the ceiling spin appears
        // inline, right-aligned in the same row, only while the limiter is on.
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Brickwall limiter"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kBrickwallLimiter, row), 0, Qt::AlignVCenter);
            hl->addStretch(1);

            limiter_ceiling_spin_ = new QDoubleSpinBox(row);
            limiter_ceiling_spin_->setObjectName(QStringLiteral("limiterCeilingSpin"));
            limiter_ceiling_spin_->setRange(-60.0, 0.0);
            limiter_ceiling_spin_->setSingleStep(0.5);
            limiter_ceiling_spin_->setDecimals(1);
            limiter_ceiling_spin_->setSuffix(QStringLiteral(" dB"));
            limiter_ceiling_spin_->setValue(static_cast<double>(audio_ui_state_.limiter_ceiling_db));
            limiter_ceiling_spin_->setVisible(audio_ui_state_.limiter_enabled);
            limiter_ceiling_spin_->setFixedWidth(160);
            limiter_ceiling_spin_->setProperty("settingsRowInput", true);
            hl->addWidget(limiter_ceiling_spin_, 0, Qt::AlignVCenter);

            limiter_check_ = new ui::widgets::ExoCheckBox(QString(), row);
            limiter_check_->setObjectName(QStringLiteral("limiterCheck"));
            limiter_check_->setChecked(audio_ui_state_.limiter_enabled);
            hl->addWidget(limiter_check_, 0, Qt::AlignVCenter);

            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);
        }

        // A/V clock slaving (H-3) — expert toggle, default on. A single
        // enable/disable control (no sub-parameters: the controller constants are
        // fixed, there is no meaningful user choice between thresholds). Slice 3
        // (cogwheels -> inline): plain inline row, no popover.
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->addWidget(rule);

            auto* row = new QWidget(audio_expert_section_);
            auto* hl = new QHBoxLayout(row);
            hl->setContentsMargins(0, 12, 0, 12);
            hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* lbl = new QLabel(QStringLiteral("Audio clock slaving"), row);
            lbl->setProperty("labelRole", "settingsRowLabel");
            hl->addWidget(lbl, 0);
            hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kClockSlaving, row), 0, Qt::AlignVCenter);
            hl->addStretch(1);

            clock_slaving_check_ = new ui::widgets::ExoCheckBox(QString(), row);
            clock_slaving_check_->setObjectName(QStringLiteral("clockSlavingCheck"));
            clock_slaving_check_->setChecked(audio_ui_state_.clock_slaving_enabled);
            hl->addWidget(clock_slaving_check_, 0, Qt::AlignVCenter);

            row->setProperty("settingsRow", true);
            aes_layout->addWidget(row);
        }

        // Microphone post-processing (Audio v2 — 0.6.0) — Slice 3 (cogwheels -> inline):
        // the 4 stage rows sit behind an inline disclosure (chevron) instead of a
        // cogwheel popover. HPF -> Gate -> AGC -> RNNoise order preserved.
        // Constructed here but inserted at mic_post_insert_index so it appears visually
        // immediately after the Mic channel mode row (mic source/tuning/processing grouped).
        {
            auto* rule = new QFrame(audio_expert_section_);
            rule->setFrameShape(QFrame::HLine);
            rule->setProperty("frameRole", "sectionRuleLine");
            aes_layout->insertWidget(mic_post_insert_index, rule);

            // --- Create all sub-controls (objectNames + values preserved as before). ---
            mic_hpf_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("High-pass filter"), audio_expert_section_);
            mic_hpf_check_->setObjectName(QStringLiteral("micHpfCheck"));
            mic_hpf_check_->setChecked(audio_ui_state_.mic_hpf_enabled);

            mic_hpf_cutoff_spin_ = new QDoubleSpinBox(audio_expert_section_);
            mic_hpf_cutoff_spin_->setObjectName(QStringLiteral("micHpfCutoffSpin"));
            mic_hpf_cutoff_spin_->setRange(20.0, 1000.0);
            mic_hpf_cutoff_spin_->setSingleStep(5.0);
            mic_hpf_cutoff_spin_->setDecimals(0);
            mic_hpf_cutoff_spin_->setSuffix(QStringLiteral(" Hz"));
            mic_hpf_cutoff_spin_->setValue(static_cast<double>(audio_ui_state_.mic_hpf_cutoff_hz));
            mic_hpf_cutoff_spin_->setEnabled(audio_ui_state_.mic_hpf_enabled);
            mic_hpf_cutoff_spin_->setFixedWidth(160);
            mic_hpf_cutoff_spin_->setProperty("settingsRowInput", true);

            mic_gate_check_ = new ui::widgets::ExoCheckBox(QStringLiteral("Noise gate"), audio_expert_section_);
            mic_gate_check_->setObjectName(QStringLiteral("micGateCheck"));
            mic_gate_check_->setChecked(audio_ui_state_.mic_gate_enabled);

            mic_gate_threshold_spin_ = new QDoubleSpinBox(audio_expert_section_);
            mic_gate_threshold_spin_->setObjectName(QStringLiteral("micGateThresholdSpin"));
            mic_gate_threshold_spin_->setRange(-80.0, 0.0);
            mic_gate_threshold_spin_->setSingleStep(1.0);
            mic_gate_threshold_spin_->setDecimals(0);
            mic_gate_threshold_spin_->setSuffix(QStringLiteral(" dB"));
            mic_gate_threshold_spin_->setValue(static_cast<double>(audio_ui_state_.mic_gate_threshold_db));
            mic_gate_threshold_spin_->setEnabled(audio_ui_state_.mic_gate_enabled);
            mic_gate_threshold_spin_->setFixedWidth(160);
            mic_gate_threshold_spin_->setProperty("settingsRowInput", true);

            mic_agc_check_ =
                new ui::widgets::ExoCheckBox(QStringLiteral("Automatic gain control"), audio_expert_section_);
            mic_agc_check_->setObjectName(QStringLiteral("micAgcCheck"));
            mic_agc_check_->setChecked(audio_ui_state_.mic_agc_enabled);

            mic_agc_target_spin_ = new QDoubleSpinBox(audio_expert_section_);
            mic_agc_target_spin_->setObjectName(QStringLiteral("micAgcTargetSpin"));
            mic_agc_target_spin_->setRange(-40.0, 0.0);
            mic_agc_target_spin_->setSingleStep(1.0);
            mic_agc_target_spin_->setDecimals(0);
            mic_agc_target_spin_->setSuffix(QStringLiteral(" dB"));
            mic_agc_target_spin_->setValue(static_cast<double>(audio_ui_state_.mic_agc_target_db));
            mic_agc_target_spin_->setEnabled(audio_ui_state_.mic_agc_enabled);
            mic_agc_target_spin_->setFixedWidth(160);
            mic_agc_target_spin_->setProperty("settingsRowInput", true);

            mic_rnnoise_check_ =
                new ui::widgets::ExoCheckBox(QStringLiteral("Noise suppression (RNNoise)"), audio_expert_section_);
            mic_rnnoise_check_->setObjectName(QStringLiteral("micRnnoiseCheck"));
            mic_rnnoise_check_->setChecked(audio_ui_state_.mic_rnnoise_enabled);

            // --- Disclosure header row: label + info-i + live status + chevron. ---
            mic_post_header_ = new QWidget(audio_expert_section_);
            mic_post_header_->setObjectName(QStringLiteral("micPostProcessingHeader"));
            auto* header_hl = new QHBoxLayout(mic_post_header_);
            header_hl->setContentsMargins(0, 12, 0, 12);
            header_hl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* header_lbl = new QLabel(QStringLiteral("Microphone post-processing"), mic_post_header_);
            header_lbl->setProperty("labelRole", "settingsRowLabel");
            header_hl->addWidget(header_lbl, 0);
            header_hl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kMicPostProcessing, mic_post_header_), 0,
                                 Qt::AlignVCenter);
            header_hl->addStretch(1);

            mic_post_status_label_ = new QLabel(mic_post_header_);
            mic_post_status_label_->setObjectName(QStringLiteral("micPostProcessingStatus"));
            mic_post_status_label_->setProperty("labelRole", "muted");
            header_hl->addWidget(mic_post_status_label_, 0, Qt::AlignVCenter);

            mic_post_disclosure_btn_ = new QToolButton(mic_post_header_);
            mic_post_disclosure_btn_->setObjectName(QStringLiteral("micPostProcessingDisclosure"));
            mic_post_disclosure_btn_->setAutoRaise(true);
            mic_post_disclosure_btn_->setCheckable(true);
            mic_post_disclosure_btn_->setChecked(false);
            mic_post_disclosure_btn_->setCursor(Qt::PointingHandCursor);
            mic_post_disclosure_btn_->setFixedSize(28, 28);
            mic_post_disclosure_btn_->setIconSize(QSize(16, 16));
            mic_post_disclosure_btn_->setToolTip(QStringLiteral("Expand microphone post-processing stages"));
            mic_post_disclosure_btn_->setIcon(ui::theme::lucideIcon(QStringLiteral("chevron-down"),
                                                                    QString::fromUtf8(ui::theme::ActiveTheme().mut), 16,
                                                                    mic_post_disclosure_btn_->devicePixelRatioF()));
            header_hl->addWidget(mic_post_disclosure_btn_, 0, Qt::AlignVCenter);

            mic_post_header_->setProperty("settingsRow", true);
            aes_layout->insertWidget(mic_post_insert_index + 1, mic_post_header_);

            // --- Disclosure content: the four stage rows, hidden until expanded. ---
            mic_post_content_ = new QWidget(audio_expert_section_);
            mic_post_content_->setObjectName(QStringLiteral("micPostProcessingContent"));
            auto* content_layout = new QVBoxLayout(mic_post_content_);
            content_layout->setContentsMargins(20, 0, 0, 12); // indent under the header label
            content_layout->setSpacing(10);
            mic_post_content_->setVisible(false);
            aes_layout->insertWidget(mic_post_insert_index + 2, mic_post_content_);

            // Each sub-stage is a mini row: toggle (+ info-i) and, where relevant, an
            // indented parameter row.
            auto makeStageRow = [](ui::widgets::ExoCheckBox* toggle, const QString& hint, QDoubleSpinBox* param_spin,
                                   const QString& param_label, QWidget* stage_parent) -> QWidget* {
                auto* container = new QWidget(stage_parent);
                auto* vl = new QVBoxLayout(container);
                vl->setContentsMargins(0, 0, 0, 0);
                vl->setSpacing(4);

                auto* toggle_row = new QWidget(container);
                auto* thl = new QHBoxLayout(toggle_row);
                thl->setContentsMargins(0, 0, 0, 0);
                thl->setSpacing(4);
                thl->addWidget(toggle, 0);
                if (!hint.isEmpty())
                    thl->addWidget(new ui::widgets::InfoHintIcon(hint, toggle_row), 0, Qt::AlignVCenter);
                thl->addStretch(1);
                vl->addWidget(toggle_row);

                if (param_spin) {
                    auto* param_row = new QWidget(container);
                    auto* hl = new QHBoxLayout(param_row);
                    hl->setContentsMargins(20, 0, 0, 0); // indent under toggle
                    hl->setSpacing(14);
                    auto* lbl = new QLabel(param_label, param_row);
                    lbl->setProperty("labelRole", "settingsRowLabel");
                    hl->addWidget(lbl, 1);
                    hl->addWidget(param_spin, 0, Qt::AlignVCenter);
                    vl->addWidget(param_row);
                }
                return container;
            };

            content_layout->addWidget(makeStageRow(mic_hpf_check_, ui::hints::kHighPassFilter, mic_hpf_cutoff_spin_,
                                                   QStringLiteral("HPF cutoff"), mic_post_content_));
            content_layout->addWidget(makeStageRow(mic_gate_check_, ui::hints::kNoiseGate, mic_gate_threshold_spin_,
                                                   QStringLiteral("Gate threshold"), mic_post_content_));
            content_layout->addWidget(makeStageRow(mic_agc_check_, ui::hints::kAgc, mic_agc_target_spin_,
                                                   QStringLiteral("AGC target level"), mic_post_content_));
            content_layout->addWidget(
                makeStageRow(mic_rnnoise_check_, ui::hints::kRnnoise, nullptr, QString(), mic_post_content_));

            // Expand/collapse: flip the chevron and show/hide the content container.
            connect(mic_post_disclosure_btn_, &QToolButton::toggled, this, [this](bool expanded) {
                if (mic_post_content_)
                    mic_post_content_->setVisible(expanded);
                if (mic_post_disclosure_btn_) {
                    mic_post_disclosure_btn_->setIcon(
                        ui::theme::lucideIcon(expanded ? QStringLiteral("chevron-up") : QStringLiteral("chevron-down"),
                                              QString::fromUtf8(ui::theme::ActiveTheme().mut), 16,
                                              mic_post_disclosure_btn_->devicePixelRatioF()));
                    mic_post_disclosure_btn_->setToolTip(
                        expanded ? QStringLiteral("Collapse microphone post-processing stages")
                                 : QStringLiteral("Expand microphone post-processing stages"));
                }
            });

            // Status text: list the active stages, updated whenever any stage toggles.
            auto updateMicPostStatus = [this]() {
                QStringList active;
                if (mic_hpf_check_->isChecked())
                    active << QStringLiteral("High-pass");
                if (mic_gate_check_->isChecked())
                    active << QStringLiteral("Gate");
                if (mic_agc_check_->isChecked())
                    active << QStringLiteral("AGC");
                if (mic_rnnoise_check_->isChecked())
                    active << QStringLiteral("RNNoise");
                if (mic_post_status_label_)
                    mic_post_status_label_->setText(active.isEmpty() ? QStringLiteral("Off")
                                                                     : active.join(QStringLiteral(" \xC2\xB7 ")));
            };
            // Wire to the stage toggles so the status stays live.
            connect(mic_hpf_check_, &ui::widgets::ExoCheckBox::toggled, this, updateMicPostStatus);
            connect(mic_gate_check_, &ui::widgets::ExoCheckBox::toggled, this, updateMicPostStatus);
            connect(mic_agc_check_, &ui::widgets::ExoCheckBox::toggled, this, updateMicPostStatus);
            connect(mic_rnnoise_check_, &ui::widgets::ExoCheckBox::toggled, this, updateMicPostStatus);
            // Set initial status text.
            updateMicPostStatus();
        }

        // (0.6.0: the former "PCM / FLAC codecs" roadmap placeholder was removed —
        // PCM and FLAC are now real, selectable audio codecs in the codec dropdown,
        // so advertising them as upcoming here would contradict the live control.)
    }
    if (auto* audio_panel_layout = qobject_cast<QVBoxLayout*>(audio_panel_->layout())) {
        audio_panel_layout->insertWidget(audio_expert_insert_index_, audio_expert_section_);
    }

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
    // Channel / sample-format model (ADR 0030 — 0.6.0).
    connect(audio_sample_rate_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx < 0 || !audio_sample_rate_combo_)
            return;
        audio_ui_state_.audio_sample_rate = static_cast<uint32_t>(audio_sample_rate_combo_->itemData(idx).toInt());
        emitCurrentAudioSettings();
    });
    connect(audio_channels_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx < 0 || !audio_channels_combo_)
            return;
        audio_ui_state_.audio_channels = static_cast<uint32_t>(audio_channels_combo_->itemData(idx).toInt());
        emitCurrentAudioSettings();
    });
    connect(audio_bit_depth_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx < 0 || !audio_bit_depth_combo_)
            return;
        // Float-PCM uses the negative kFloatBitDepthItemData sentinel (see
        // EncodeBitDepthComboData) so it cannot be confused with the plain
        // int "32-bit" entry.
        const int raw = audio_bit_depth_combo_->itemData(idx).toInt();
        if (raw < 0) {
            audio_ui_state_.audio_bit_depth = 32u;
            audio_ui_state_.audio_pcm_float = true;
        } else {
            audio_ui_state_.audio_bit_depth = static_cast<uint32_t>(raw);
            audio_ui_state_.audio_pcm_float = false;
        }
        emitCurrentAudioSettings();
    });
    connect(flac_compression_spin_, &QSpinBox::valueChanged, this, [this](int val) {
        audio_ui_state_.flac_compression_level = val;
        emitCurrentAudioSettings();
    });
    connect(limiter_check_, &ui::widgets::ExoCheckBox::toggled, this, [this](bool on) {
        audio_ui_state_.limiter_enabled = on;
        if (limiter_ceiling_spin_)
            limiter_ceiling_spin_->setVisible(on);
        emitCurrentAudioSettings();
    });
    connect(limiter_ceiling_spin_, &QDoubleSpinBox::valueChanged, this, [this](double db) {
        audio_ui_state_.limiter_ceiling_db = static_cast<float>(db);
        emitCurrentAudioSettings();
    });
    connect(clock_slaving_check_, &ui::widgets::ExoCheckBox::toggled, this, [this](bool on) {
        audio_ui_state_.clock_slaving_enabled = on;
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
    connect(mic_rnnoise_check_, &ui::widgets::ExoCheckBox::toggled, this, [this](bool on) {
        audio_ui_state_.mic_rnnoise_enabled = on;
        emitCurrentAudioSettings();
    });
}

// Startup-perf: the split-recording expert subtree is built on first expert-enable.
void ConfigPage::buildSplitExpertSection() {
    if (split_expert_built_)
        return;
    split_expert_built_ = true;
    QWidget* out_panel = out_panel_; // alias: moved construction references it
    split_expert_section_ = new QWidget(out_panel);
    split_expert_section_->setObjectName(QStringLiteral("splitExpertSection"));
    split_expert_section_->setVisible(false); // hidden until expert mode is on
    {
        auto* split_expert_layout = new QVBoxLayout(split_expert_section_);
        split_expert_layout->setContentsMargins(0, 0, 0, 0);
        split_expert_layout->setSpacing(8);

        // --- Create all sub-controls first (objectNames + values preserved as before). ---
        split_mode_combo_ = new QComboBox(split_expert_section_);
        split_mode_combo_->setObjectName(QStringLiteral("splitModeCombo"));
        split_mode_combo_->addItem(QStringLiteral("Off"), static_cast<int>(SplitRecordingMode::Off));
        split_mode_combo_->addItem(QStringLiteral("Every 15 min"), static_cast<int>(SplitRecordingMode::Every15Min));
        split_mode_combo_->addItem(QStringLiteral("Every 30 min"), static_cast<int>(SplitRecordingMode::Every30Min));
        split_mode_combo_->addItem(QStringLiteral("Every 60 min"), static_cast<int>(SplitRecordingMode::Every60Min));
        split_mode_combo_->addItem(QStringLiteral("Custom"), static_cast<int>(SplitRecordingMode::Custom));
        split_mode_combo_->setToolTip(
            QStringLiteral("Automatically start a new file at the chosen interval (manual splits always work)."));

        split_custom_widget_ = new QWidget(split_expert_section_);
        split_custom_widget_->setObjectName(QStringLiteral("splitCustomWidget"));
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

        split_summary_label_ = makeHint(QString(), split_expert_section_);
        split_summary_label_->setObjectName(QStringLiteral("splitSummaryLabel"));

        split_size_mode_combo_ = new QComboBox(split_expert_section_);
        split_size_mode_combo_->setObjectName(QStringLiteral("splitSizeModeCombo"));
        split_size_mode_combo_->addItem(QStringLiteral("Off"), static_cast<int>(SplitSizeMode::Off));
        split_size_mode_combo_->addItem(QStringLiteral("Custom"), static_cast<int>(SplitSizeMode::Custom));
        split_size_mode_combo_->setToolTip(
            QStringLiteral("Automatically start a new file when the segment reaches the chosen size."));

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

        // --- Lay the split controls out inline inside the Output card so they
        // fill the card instead of hiding behind a popover row. ---
        split_expert_layout->addWidget(makeHRule(split_expert_section_));
        split_expert_layout->addWidget(makeOutputSubLabelWithHint(QStringLiteral("Automatic split"),
                                                                  ui::hints::kSplitRecording, split_expert_section_));

        // "Split recording" (by time) sub-section.
        split_expert_layout->addWidget(makeOutputSubLabel(QStringLiteral("Split recording"), split_expert_section_));

        auto* split_row = new QWidget(split_expert_section_);
        auto* split_row_hl = new QHBoxLayout(split_row);
        split_row_hl->setContentsMargins(0, 4, 0, 4);
        split_row_hl->setSpacing(8);
        split_row_hl->addWidget(split_mode_combo_, 0);
        split_row_hl->addWidget(split_custom_widget_, 0);
        split_row_hl->addStretch();
        split_expert_layout->addWidget(split_row);
        split_expert_layout->addWidget(split_summary_label_);

        // "Split by size" sub-section.
        split_expert_layout->addWidget(makeOutputSubLabel(QStringLiteral("Split by size"), split_expert_section_));

        auto* split_size_row = new QWidget(split_expert_section_);
        auto* split_size_row_hl = new QHBoxLayout(split_size_row);
        split_size_row_hl->setContentsMargins(0, 4, 0, 4);
        split_size_row_hl->setSpacing(8);
        split_size_row_hl->addWidget(split_size_mode_combo_, 0);
        split_size_row_hl->addWidget(split_size_custom_widget_, 0);
        split_size_row_hl->addStretch();
        split_expert_layout->addWidget(split_size_row);
    }
    if (auto* out_panel_layout = qobject_cast<QVBoxLayout*>(out_panel_->layout())) {
        out_panel_layout->insertWidget(split_expert_insert_index_, split_expert_section_);
    }

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

    // Seed the freshly built split controls from the current settings.
    updateSplitSelection();
}

// Startup-perf: the Developer card (expert-gated, UI-only stubs) is built on
// first expert-enable instead of eagerly-then-hidden.
void ConfigPage::buildDeveloperCard() {
    if (developer_card_built_)
        return;
    developer_card_built_ = true;
    QWidget* left_col = left_col_; // alias: moved construction references it
    {
        developer_card_ = makePanel(left_col);
        developer_card_->setObjectName(QStringLiteral("settingsDeveloperCard"));
        auto* dev_layout = new QVBoxLayout(developer_card_);
        dev_layout->setContentsMargins(18, 14, 18, 14);
        dev_layout->setSpacing(M::kSpaceSm);
        dev_layout->addWidget(makeCardTitle(QStringLiteral("Developer"), developer_card_, QStringLiteral("bug")));
        // SETTINGS-HONESTY-R1 (review F2): the old hint ("not persisted between
        // sessions") became false once the log level was genuinely wired + persisted.
        dev_layout->addWidget(
            makeHint(QStringLiteral("Expert debug controls. The logging level is persisted across sessions; "
                                    "profiling markers are planned."),
                     developer_card_));

        // SETTINGS-HONESTY-R1: log level, genuinely wired to AppLog::setMinSeverity
        // (via MainWindow) and persisted (AppSettingsStore::developer_log_level).
        // Controls which severities land in the in-app Logs page + session log file.
        // "Trace" was dropped from the original 6-item stub: AppLog only has four
        // severities (Debug/Info/Warning/Error); inventing a fifth would mean adding a
        // LogSeverity nothing in the app actually emits, so the combo is honest about
        // what levels exist instead.
        {
            auto* row = new QFrame(developer_card_);
            row->setProperty("panelRole", "compactRow");
            auto* rl = new QVBoxLayout(row);
            rl->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
            rl->setSpacing(M::kSpaceXs);
            rl->addWidget(makeFieldLabel(QStringLiteral("Developer logging level"), row));
            auto* log_level_combo = new QComboBox(row);
            log_level_combo->setObjectName(QStringLiteral("developerLogLevelCombo"));
            log_level_combo->setMinimumWidth(220);
            log_level_combo->setMaximumWidth(320);
            log_level_combo->addItem(QStringLiteral("Off"), QStringLiteral("Off"));
            log_level_combo->addItem(QStringLiteral("Error"), QStringLiteral("Error"));
            log_level_combo->addItem(QStringLiteral("Warning"), QStringLiteral("Warning"));
            log_level_combo->addItem(QStringLiteral("Info"), QStringLiteral("Info"));
            log_level_combo->addItem(QStringLiteral("Debug"), QStringLiteral("Debug"));
            // Review F3: name the consequence — raising the level drops lines from
            // support diagnostics, which is exactly the surprise a user should see coming.
            log_level_combo->setToolTip(
                QStringLiteral("Raising this hides lower-severity lines from the in-app log and session log "
                               "file — support diagnostics may be incomplete."));
            developer_log_level_combo_ = log_level_combo;
            {
                const int idx = log_level_combo->findData(developer_log_level_);
                log_level_combo->setCurrentIndex(idx >= 0 ? idx : 4); // fallback: Debug (record everything)
            }
            connect(log_level_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
                if (!developer_log_level_combo_)
                    return;
                const QString level = developer_log_level_combo_->itemData(index).toString();
                if (level.isEmpty())
                    return;
                developer_log_level_ = level;
                emit developerLogLevelChanged(level);
            });
            rl->addWidget(log_level_combo);
            dev_layout->addWidget(row);
        }

        // NVTX profiling markers: no NVTX infrastructure exists anywhere in the app
        // (no headers, no instrumentation calls) — honestly disabled + tooltip rather
        // than building speculative NVTX plumbing for a control nothing else uses yet.
        {
            auto* row = new QFrame(developer_card_);
            row->setProperty("panelRole", "compactRow");
            auto* rl = new QVBoxLayout(row);
            rl->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
            rl->setSpacing(M::kSpaceXs);
            rl->addWidget(makeFieldLabel(QStringLiteral("Profiling"), row));
            auto* nvtx_check = new ui::widgets::ExoCheckBox(QStringLiteral("Enable NVTX / profiling markers"), row);
            nvtx_check->setObjectName(QStringLiteral("nvtxProfilingCheck"));
            nvtx_check->setEnabled(false);
            nvtx_check->setToolTip(QStringLiteral("Profiling markers are planned for a future build."));
            rl->addWidget(nvtx_check);
            dev_layout->addWidget(row);
        }

        developer_card_->setVisible(expert_mode_enabled_);
    }
    if (auto* left_layout = qobject_cast<QVBoxLayout*>(left_col_->layout())) {
        left_layout->insertWidget(developer_insert_index_, developer_card_);
    }
}

// Startup-perf: the interleaved Expert rate/format subtree spread across the Quality
// and Container cards (CQ precision row, Rate control + Bitrate + Frame pacing, Bit
// depth, Colour range, Encoder preset, Keyframe interval, HDR and Chroma — ~430 LOC /
// ~40 widgets, half of them hidden until a rate/codec toggle) is built on first
// expert-enable instead of eagerly-then-hidden, so the default non-expert ConfigPage
// build never pays for it. All external access to these widgets is null-guarded; the
// tail here re-seeds them from the current models before the section becomes visible.
void ConfigPage::buildFormatQualityExpertSections() {
    if (fmt_quality_expert_built_)
        return;
    fmt_quality_expert_built_ = true;
    // Aliases so the moved construction below reads exactly as it did in the ctor.
    QWidget* fmt_panel = fmt_panel_;
    QWidget* quality_panel = quality_panel_;
    auto* fmt_layout = qobject_cast<QVBoxLayout*>(fmt_panel_->layout());
    auto* quality_layout = qobject_cast<QVBoxLayout*>(quality_panel_->layout());

    // Wave 2 Part B: CQ precision spinbox row — shown in expert mode, hidden otherwise.
    {
        quality_expert_widget_ = new QWidget(quality_panel);
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
        qehl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
        auto* qelbl = new QLabel(QStringLiteral("Quality (CQ)"), qecontent);
        qelbl->setProperty("labelRole", "settingsRowLabel");
        qehl->addWidget(qelbl, 0);
        auto* qeinfo = new ui::widgets::InfoHintIcon(ui::hints::kConstantQuality, qecontent);
        qeinfo->setObjectName(QStringLiteral("qualityCqInfoHint"));
        qehl->addWidget(qeinfo, 0, Qt::AlignVCenter);
        qehl->addStretch(1);
        quality_cq_spin_ = new QSpinBox(qecontent);
        quality_cq_spin_->setObjectName(QStringLiteral("qualityCqSpin"));
        quality_cq_spin_->setRange(static_cast<int>(recorder_core::kNvencCqMin),
                                   static_cast<int>(recorder_core::kNvencCqMax));
        quality_cq_spin_->setFixedWidth(160); // same column width as every other row input
        // Scrolling the settings page must not silently retune quality: the wheel
        // only steps the value once the box has been focused deliberately.
        quality_cq_spin_->setFocusPolicy(Qt::StrongFocus);
        quality_cq_spin_->installEventFilter(this);
        quality_cq_spin_->setProperty("settingsRowInput", true);
        qehl->addWidget(quality_cq_spin_, 0, Qt::AlignVCenter);
        qevl->addWidget(qecontent);
        quality_expert_widget_->setProperty("settingsRow", true);
        quality_expert_widget_->setVisible(false); // hidden until expert mode is on
    }

    // --- PS-PHASE-C / v10: Expert sections (split across two cards) ---
    // v10 places Rate control + Bitrate in the Quality & timing card, and Bit depth +
    // Colour range (+ roadmap dummies) in the Container & codecs card. Two expert
    // containers carry these; both are gated on expert_mode_enabled_.
    // Shown only when expert_mode_enabled_ == true. rate_control is the active row;
    // quality_expert_widget_ (existing CQ spinbox) stays visible when rate=CQ and hidden
    // when rate=VBR/CBR (reusing existing logic). bitrate_row is shown for VBR/CBR.
    {
        // Quality & timing expert section (Rate control + Bitrate).
        quality_rate_section_ = new QWidget(quality_panel);
        quality_rate_section_->setObjectName(QStringLiteral("qualityRateSection"));
        quality_rate_section_->setVisible(false);
        auto* qrs_layout = new QVBoxLayout(quality_rate_section_);
        qrs_layout->setContentsMargins(0, 0, 0, 0);
        qrs_layout->setSpacing(0);

        // Container & codecs expert section (Bit depth + Colour range + roadmap).
        fmt_expert_section_ = new QWidget(fmt_panel);
        fmt_expert_section_->setObjectName(QStringLiteral("fmtExpertSection"));
        fmt_expert_section_->setVisible(false); // hidden until expert mode on
        auto* fes_layout = new QVBoxLayout(fmt_expert_section_);
        fes_layout->setContentsMargins(0, 0, 0, 0);
        fes_layout->setSpacing(0);

        // --- Rate control dropdown (CQ / VBR / CBR) — Quality card ---
        // v10/Canon (SSelect): a compact dropdown, not a full-width segmented group.
        // itemData carries the recorder_core::RateControlMode enum.
        rate_control_combo_ = new QComboBox(quality_rate_section_);
        rate_control_combo_->setObjectName(QStringLiteral("rateControlCombo"));
        rate_control_combo_->addItem(QStringLiteral("CQ"),
                                     static_cast<int>(recorder_core::RateControlMode::ConstantQuality));
        rate_control_combo_->addItem(QStringLiteral("VBR"),
                                     static_cast<int>(recorder_core::RateControlMode::VariableBitrate));
        rate_control_combo_->addItem(QStringLiteral("CBR"),
                                     static_cast<int>(recorder_core::RateControlMode::ConstantBitrate));
        rate_control_combo_->setFixedWidth(160);
        rate_control_combo_->setProperty("settingsRowInput", true);

        rate_control_row_widget_ = new QWidget(quality_rate_section_);
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
            rhl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* rlbl = new QLabel(QStringLiteral("Rate control"), rate_control_row_widget_);
            rlbl->setProperty("labelRole", "settingsRowLabel");
            rhl->addWidget(rlbl, 0);
            rhl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kRateControlMode, rate_control_row_widget_), 0,
                           Qt::AlignVCenter);
            rhl->addStretch(1);
            rhl->addWidget(rate_control_combo_, 0, Qt::AlignVCenter);
            rvl->addLayout(rhl);
            rate_control_row_widget_->setProperty("settingsRow", true);
        }
        qrs_layout->addWidget(rate_control_row_widget_);

        // --- Bitrate spinbox (VBR / CBR only) — Quality card ---
        bitrate_row_widget_ = new QWidget(quality_rate_section_);
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
            bhl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* blbl = new QLabel(QStringLiteral("Bitrate"), bitrate_row_widget_);
            blbl->setProperty("labelRole", "settingsRowLabel");
            bhl->addWidget(blbl, 0);
            bhl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kVideoBitrate, bitrate_row_widget_), 0,
                           Qt::AlignVCenter);
            bhl->addStretch(1);
            bitrate_kbps_spin_ = new QSpinBox(bitrate_row_widget_);
            bitrate_kbps_spin_->setObjectName(QStringLiteral("bitrateKbpsSpin"));
            bitrate_kbps_spin_->setRange(1000, 100000);
            bitrate_kbps_spin_->setSuffix(QStringLiteral(" kbps"));
            bitrate_kbps_spin_->setValue(static_cast<int>(video_settings_.bitrate_kbps));
            bitrate_kbps_spin_->setFixedWidth(160);
            bitrate_kbps_spin_->setProperty("settingsRowInput", true);
            bhl->addWidget(bitrate_kbps_spin_, 0, Qt::AlignVCenter);
            bvl->addLayout(bhl);
            bitrate_row_widget_->setProperty("settingsRow", true);
        }
        qrs_layout->addWidget(bitrate_row_widget_);

        // --- Video bit depth (0.7.0 — S7) ---
        // Real, capability-gated control. 8-bit is universal; 10-bit (HEVC Main10 /
        // AV1 10-bit P010, SDR BT.709 — ADR 0032) is selectable only when the video
        // codec is HEVC or AV1. For H.264 the 10-bit item is disabled with a tooltip;
        // selectability is driven by capability::QueryCombo (single source of truth)
        // in updateVideoBitDepthControl().
        {
            video_bit_depth_row_ = new QWidget(fmt_expert_section_);
            video_bit_depth_row_->setObjectName(QStringLiteral("videoBitDepthRow"));
            auto* dvl = new QVBoxLayout(video_bit_depth_row_);
            dvl->setContentsMargins(0, 0, 0, 0);
            dvl->setSpacing(0);
            auto* drule = new QFrame(video_bit_depth_row_);
            drule->setFrameShape(QFrame::HLine);
            drule->setProperty("frameRole", "sectionRuleLine");
            dvl->addWidget(drule);
            auto* dhl = new QHBoxLayout();
            dhl->setContentsMargins(0, 12, 0, 12);
            dhl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* dlbl = new QLabel(QStringLiteral("Bit depth"), video_bit_depth_row_);
            dlbl->setProperty("labelRole", "settingsRowLabel");
            dhl->addWidget(dlbl, 0);
            dhl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kVideoBitDepth, video_bit_depth_row_), 0,
                           Qt::AlignVCenter);
            dhl->addStretch(1);
            video_bit_depth_combo_ = new QComboBox(video_bit_depth_row_);
            video_bit_depth_combo_->setObjectName(QStringLiteral("videoBitDepthCombo"));
            video_bit_depth_combo_->addItem(QStringLiteral("8-bit"), static_cast<int>(capability::BitDepth::Bit8));
            video_bit_depth_combo_->addItem(QStringLiteral("10-bit"), static_cast<int>(capability::BitDepth::Bit10));
            video_bit_depth_combo_->setFixedWidth(160);
            video_bit_depth_combo_->setProperty("settingsRowInput", true);
            dhl->addWidget(video_bit_depth_combo_, 0, Qt::AlignVCenter);
            dvl->addLayout(dhl);
            video_bit_depth_row_->setProperty("settingsRow", true);
            fes_layout->addWidget(video_bit_depth_row_);
        }

        // --- Colour range (0.7.0) ---
        // Full (0-255) is the native precision of PC/screen content and the default;
        // Limited (16-235) is the broadcast standard, safest for editors/players that
        // ignore the range flag. Both are ALWAYS valid for every codec/container, so
        // this control is never capability-gated — only the recording lock disables it
        // (see updateVideoColorRangeControl()).
        {
            video_color_range_row_ = new QWidget(fmt_expert_section_);
            auto* rvl = new QVBoxLayout(video_color_range_row_);
            rvl->setContentsMargins(0, 0, 0, 0);
            rvl->setSpacing(0);
            auto* rrule = new QFrame(video_color_range_row_);
            rrule->setFrameShape(QFrame::HLine);
            rrule->setProperty("frameRole", "sectionRuleLine");
            rvl->addWidget(rrule);
            auto* rhl = new QHBoxLayout();
            rhl->setContentsMargins(0, 12, 0, 12);
            rhl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* rlbl = new QLabel(QStringLiteral("Colour range"), video_color_range_row_);
            rlbl->setProperty("labelRole", "settingsRowLabel");
            rhl->addWidget(rlbl, 0);
            rhl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kVideoColorRange, video_color_range_row_), 0,
                           Qt::AlignVCenter);
            rhl->addStretch(1);
            video_color_range_combo_ = new QComboBox(video_color_range_row_);
            video_color_range_combo_->setObjectName(QStringLiteral("videoColorRangeCombo"));
            video_color_range_combo_->addItem(QStringLiteral("Full (PC)"),
                                              static_cast<int>(capability::ColorRange::Full));
            video_color_range_combo_->addItem(QStringLiteral("Limited (TV)"),
                                              static_cast<int>(capability::ColorRange::Limited));
            video_color_range_combo_->setFixedWidth(160);
            video_color_range_combo_->setProperty("settingsRowInput", true);
            rhl->addWidget(video_color_range_combo_, 0, Qt::AlignVCenter);
            rvl->addLayout(rhl);
            video_color_range_row_->setProperty("settingsRow", true);
            fes_layout->addWidget(video_color_range_row_);
        }

        // --- Encoder preset (NVENC-PRESET-R1) ---
        // NVENC SDK speed/quality preset P1 (fastest, lowest quality) .. P7
        // (slowest, best quality). Independent of the quality-tier CQP values
        // and rate-control mode above — this selects NVENC's internal
        // encoding-pipeline tradeoff. Always valid for every codec (H.264/HEVC/
        // AV1) and container, so never capability-gated — only the recording
        // lock disables it (see updateVideoEncoderPresetControl()).
        {
            video_encoder_preset_row_ = new QWidget(fmt_expert_section_);
            auto* pvl = new QVBoxLayout(video_encoder_preset_row_);
            pvl->setContentsMargins(0, 0, 0, 0);
            pvl->setSpacing(0);
            auto* prule = new QFrame(video_encoder_preset_row_);
            prule->setFrameShape(QFrame::HLine);
            prule->setProperty("frameRole", "sectionRuleLine");
            pvl->addWidget(prule);
            auto* phl = new QHBoxLayout();
            phl->setContentsMargins(0, 12, 0, 12);
            phl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* plbl = new QLabel(QStringLiteral("Encoder preset (NVENC)"), video_encoder_preset_row_);
            plbl->setProperty("labelRole", "settingsRowLabel");
            phl->addWidget(plbl, 0);
            phl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kEncoderPreset, video_encoder_preset_row_), 0,
                           Qt::AlignVCenter);
            phl->addStretch(1);
            video_encoder_preset_combo_ = new QComboBox(video_encoder_preset_row_);
            video_encoder_preset_combo_->setObjectName(QStringLiteral("videoEncoderPresetCombo"));
            video_encoder_preset_combo_->addItem(QStringLiteral("P1 \xe2\x80\x94 Fastest"),
                                                 static_cast<int>(recorder_core::NvencPreset::P1));
            video_encoder_preset_combo_->addItem(QStringLiteral("P2"),
                                                 static_cast<int>(recorder_core::NvencPreset::P2));
            video_encoder_preset_combo_->addItem(QStringLiteral("P3"),
                                                 static_cast<int>(recorder_core::NvencPreset::P3));
            // Shorter label than "P4 — Balanced (default)": that clipped hard at the
            // row's fixed combo width. The "default" callout still lives in the
            // info-i tooltip (kEncoderPreset), so dropping it from the visible
            // label loses no information.
            video_encoder_preset_combo_->addItem(QStringLiteral("P4 \xC2\xB7 Balanced"),
                                                 static_cast<int>(recorder_core::NvencPreset::P4));
            video_encoder_preset_combo_->addItem(QStringLiteral("P5"),
                                                 static_cast<int>(recorder_core::NvencPreset::P5));
            video_encoder_preset_combo_->addItem(QStringLiteral("P6"),
                                                 static_cast<int>(recorder_core::NvencPreset::P6));
            video_encoder_preset_combo_->addItem(QStringLiteral("P7 \xe2\x80\x94 Slowest"),
                                                 static_cast<int>(recorder_core::NvencPreset::P7));
            video_encoder_preset_combo_->setFixedWidth(200);
            video_encoder_preset_combo_->setProperty("settingsRowInput", true);
            phl->addWidget(video_encoder_preset_combo_, 0, Qt::AlignVCenter);
            pvl->addLayout(phl);
            video_encoder_preset_row_->setProperty("settingsRow", true);
            fes_layout->addWidget(video_encoder_preset_row_);
        }

        // --- Frame pacing (ADR 0035 Slice 2) ---
        // Smooth = phase-correct present-time-nearest selection (default, the recording
        // use case). Newest = lowest-latency newest-at-tick (WGC fallback behaviour).
        // Both are always valid — no codec/container gating. Only the recording lock
        // disables it (see updateFramePacingControl()).
        // v0.9 polish (regroup): frame pacing is a timing control, so the row lives
        // in the Quality & timing card — inserted right below Frame timing (before
        // Capture cursor) in the attach block further down — not with the
        // format-identity rows in Container & codecs. It is a standalone
        // expert-gated row (updateExpertModeVisibility), hidden until then.
        {
            frame_pacing_row_ = new QWidget(quality_panel);
            frame_pacing_row_->setObjectName(QStringLiteral("framePacingRow"));
            auto* pvl = new QVBoxLayout(frame_pacing_row_);
            pvl->setContentsMargins(0, 0, 0, 0);
            pvl->setSpacing(0);
            auto* prule = new QFrame(frame_pacing_row_);
            prule->setFrameShape(QFrame::HLine);
            prule->setProperty("frameRole", "sectionRuleLine");
            pvl->addWidget(prule);
            auto* phl = new QHBoxLayout();
            phl->setContentsMargins(0, 12, 0, 12);
            phl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* plbl = new QLabel(QStringLiteral("Frame pacing"), frame_pacing_row_);
            plbl->setProperty("labelRole", "settingsRowLabel");
            phl->addWidget(plbl, 0);
            phl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kFramePacing, frame_pacing_row_), 0,
                           Qt::AlignVCenter);
            phl->addStretch(1);
            frame_pacing_combo_ = new QComboBox(frame_pacing_row_);
            frame_pacing_combo_->setObjectName(QStringLiteral("framePacingSelect"));
            frame_pacing_combo_->addItem(QStringLiteral("Phase-correct"),
                                         static_cast<int>(recorder_core::FramePacingMode::Smooth));
            frame_pacing_combo_->addItem(QStringLiteral("Lowest latency"),
                                         static_cast<int>(recorder_core::FramePacingMode::Newest));
            frame_pacing_combo_->setFixedWidth(160);
            frame_pacing_combo_->setProperty("settingsRowInput", true);
            phl->addWidget(frame_pacing_combo_, 0, Qt::AlignVCenter);
            pvl->addLayout(phl);
            frame_pacing_row_->setProperty("settingsRow", true);
            frame_pacing_row_->setVisible(false); // expert-gated; shown by updateExpertModeVisibility
        }

        // --- Keyframe interval (0.9.0 S1 Quick Trim) ---
        {
            auto* ki_row = new QWidget(fmt_expert_section_);
            auto* kivl = new QVBoxLayout(ki_row);
            kivl->setContentsMargins(0, 0, 0, 0);
            kivl->setSpacing(0);
            auto* kirule = new QFrame(ki_row);
            kirule->setFrameShape(QFrame::HLine);
            // "settingsDivider" has no stylesheet rule, so Qt drew its default
            // (white) frame here instead of the hairline every other row uses.
            kirule->setProperty("frameRole", "sectionRuleLine");
            kivl->addWidget(kirule);
            auto* kihl = new QHBoxLayout();
            kihl->setContentsMargins(0, 12, 0, 12); // flush with every other settings row
            kihl->setSpacing(4);                    // label <-> info-i, matches makeSettingsRow
            auto* kilbl = new QLabel(QStringLiteral("Keyframe interval"), ki_row);
            kilbl->setProperty("labelRole", "settingsRowLabel");
            kihl->addWidget(kilbl, 0);
            kihl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kKeyframeInterval, ki_row), 0, Qt::AlignVCenter);
            kihl->addStretch(1);
            keyframe_interval_combo_ = new QComboBox(ki_row);
            keyframe_interval_combo_->setObjectName(QStringLiteral("keyframeIntervalSelect"));
            keyframe_interval_combo_->addItem(QStringLiteral("2 s (default)"),
                                              static_cast<int>(KeyframeIntervalMode::Seconds2));
            keyframe_interval_combo_->addItem(QStringLiteral("1 s"), static_cast<int>(KeyframeIntervalMode::Seconds1));
            keyframe_interval_combo_->addItem(QStringLiteral("0.5 s"),
                                              static_cast<int>(KeyframeIntervalMode::Seconds0_5));
            keyframe_interval_combo_->setFixedWidth(160);
            keyframe_interval_combo_->setProperty("settingsRowInput", true);
            kihl->addWidget(keyframe_interval_combo_, 0, Qt::AlignVCenter);
            kivl->addLayout(kihl);
            ki_row->setProperty("settingsRow", true);
            fes_layout->addWidget(ki_row);
        }

        // --- HDR handling (expert-only) ---
        // HDR-capable displays are auto-detected elsewhere; this control only selects
        // what the pipeline does once one is found — and the whole row is
        // relevance-gated on that detection (hidden while no probed display is
        // HDR-active; see updateVideoHdrModeControl). Off is intentionally not offered —
        // it has no user-facing value ("break my recording") and stays enum/config-
        // internal. Hdr10 is selectable only when the chosen codec carries a native
        // HDR10 signal (capability::QueryHdr10Native — HEVC/AV1; never H.264); the
        // gating mirrors the bit-depth row but does NOT snap the stored value back to
        // TonemapSdr when the codec changes — the live pre-flight blocker (rec.hdr.h264)
        // owns that conflict at recording time (see updateVideoHdrModeControl()).
        {
            video_hdr_mode_row_ = new QWidget(fmt_expert_section_);
            video_hdr_mode_row_->setObjectName(QStringLiteral("videoHdrModeRow"));
            auto* hvl = new QVBoxLayout(video_hdr_mode_row_);
            hvl->setContentsMargins(0, 0, 0, 0);
            hvl->setSpacing(0);
            auto* hrule = new QFrame(video_hdr_mode_row_);
            hrule->setFrameShape(QFrame::HLine);
            hrule->setProperty("frameRole", "sectionRuleLine");
            hvl->addWidget(hrule);
            auto* hhl = new QHBoxLayout();
            hhl->setContentsMargins(0, 12, 0, 12);
            hhl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* hlbl = new QLabel(QStringLiteral("HDR handling"), video_hdr_mode_row_);
            hlbl->setProperty("labelRole", "settingsRowLabel");
            hhl->addWidget(hlbl, 0);
            hhl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kVideoHdrMode, video_hdr_mode_row_), 0,
                           Qt::AlignVCenter);
            hhl->addStretch(1);
            video_hdr_mode_combo_ = new QComboBox(video_hdr_mode_row_);
            video_hdr_mode_combo_->setObjectName(QStringLiteral("videoHdrModeCombo"));
            video_hdr_mode_combo_->addItem(QStringLiteral("Tone-map to SDR"),
                                           static_cast<int>(recorder_core::HdrMode::TonemapSdr));
            video_hdr_mode_combo_->addItem(QStringLiteral("Record native HDR10"),
                                           static_cast<int>(recorder_core::HdrMode::Hdr10));
            video_hdr_mode_combo_->setFixedWidth(200);
            video_hdr_mode_combo_->setProperty("settingsRowInput", true);
            hhl->addWidget(video_hdr_mode_combo_, 0, Qt::AlignVCenter);
            hvl->addLayout(hhl);
            video_hdr_mode_row_->setProperty("settingsRow", true);
            fes_layout->addWidget(video_hdr_mode_row_);

            // Calm inline hint (never a warning colour) shown only while H.264 disables
            // the Hdr10 item — mirrors the muted validation-hint idiom used elsewhere.
            video_hdr_mode_hint_ = makeHint(
                QStringLiteral("Not available with H.264 \xe2\x80\x94 switch to AV1 or HEVC."), fmt_expert_section_);
            video_hdr_mode_hint_->setVisible(false);
            fes_layout->addWidget(video_hdr_mode_hint_);
        }

        // --- Chroma subsampling (expert): 4:2:0 default / 4:4:4 gated ---
        // Real control: 4:4:4 is an 8-bit H.264/HEVC-only path (AYUV, NVENC High
        // 4:4:4 / HEVC FREXT). The 4:4:4 item is capability-gated per selected
        // codec/bit-depth; 4:2:2 is not offered (Ada NVENC has no 4:2:2).
        {
            video_chroma_row_ = new QWidget(fmt_expert_section_);
            video_chroma_row_->setObjectName(QStringLiteral("videoChromaRow"));
            auto* cvl = new QVBoxLayout(video_chroma_row_);
            cvl->setContentsMargins(0, 0, 0, 0);
            cvl->setSpacing(0);
            auto* crule = new QFrame(video_chroma_row_);
            crule->setFrameShape(QFrame::HLine);
            crule->setProperty("frameRole", "sectionRuleLine");
            cvl->addWidget(crule);
            auto* chl = new QHBoxLayout();
            chl->setContentsMargins(0, 12, 0, 12);
            chl->setSpacing(4); // label <-> info-i, matches makeSettingsRow
            auto* clbl = new QLabel(QStringLiteral("Chroma subsampling"), video_chroma_row_);
            clbl->setProperty("labelRole", "settingsRowLabel");
            chl->addWidget(clbl, 0);
            chl->addWidget(new ui::widgets::InfoHintIcon(ui::hints::kChromaSubsampling, video_chroma_row_), 0,
                           Qt::AlignVCenter);
            chl->addStretch(1);
            video_chroma_combo_ = new QComboBox(video_chroma_row_);
            video_chroma_combo_->setObjectName(QStringLiteral("videoChromaCombo"));
            video_chroma_combo_->addItem(QStringLiteral("4:2:0"),
                                         static_cast<int>(capability::ChromaSubsampling::Cs420));
            video_chroma_combo_->addItem(QStringLiteral("4:4:4"),
                                         static_cast<int>(capability::ChromaSubsampling::Cs444));
            video_chroma_combo_->setFixedWidth(160);
            video_chroma_combo_->setProperty("settingsRowInput", true);
            chl->addWidget(video_chroma_combo_, 0, Qt::AlignVCenter);
            cvl->addLayout(chl);
            video_chroma_row_->setProperty("settingsRow", true);
            fes_layout->addWidget(video_chroma_row_);

            // With the row relevance-gated on codec+GPU, the only conflict this
            // hint ever narrates is the (user-fixable) 10-bit selection.
            video_chroma_hint_ =
                makeHint(QStringLiteral("4:4:4 needs 8-bit \xE2\x80\x94 switch Bit depth to 8-bit to enable it."),
                         fmt_expert_section_);
            video_chroma_hint_->setVisible(false);
            fes_layout->addWidget(video_chroma_hint_);
        }
    }

    // Attach both Expert containers at the slots recorded during construction:
    // fmt_expert_section_ goes before the compat callout in the Container card; the
    // rate section and the CQ row (in that order) go right after the Default dropdown
    // in the Quality card, so the expert order reads Rate control → CQ → Frame rate.
    if (fmt_layout && fmt_expert_insert_index_ >= 0)
        fmt_layout->insertWidget(fmt_expert_insert_index_, fmt_expert_section_);
    if (quality_layout && quality_expert_insert_index_ >= 0) {
        quality_layout->insertWidget(quality_expert_insert_index_, quality_rate_section_);
        quality_layout->insertWidget(quality_expert_insert_index_ + 1, quality_expert_widget_);
    }
    // v0.9 polish (regroup): Frame pacing slots into the Quality & timing card
    // between the Frame timing and Capture cursor rows. Resolved via indexOf so
    // the two insertions above cannot skew the slot.
    if (quality_layout && frame_pacing_row_) {
        const int cursor_idx = capture_cursor_row_ ? quality_layout->indexOf(capture_cursor_row_) : -1;
        if (cursor_idx >= 0)
            quality_layout->insertWidget(cursor_idx, frame_pacing_row_);
        else
            quality_layout->addWidget(frame_pacing_row_);
    }

    // ---- Connects (Container card expert rows) ----
    connect(video_bit_depth_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onVideoBitDepthChanged);
    connect(video_chroma_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onVideoChromaChanged);
    connect(video_color_range_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onVideoColorRangeChanged);
    connect(video_hdr_mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onVideoHdrModeChanged);
    connect(video_encoder_preset_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConfigPage::onVideoEncoderPresetChanged);
    connect(frame_pacing_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx < 0 || !frame_pacing_combo_)
            return;
        video_settings_.frame_pacing =
            static_cast<recorder_core::FramePacingMode>(frame_pacing_combo_->itemData(idx).toInt());
        emitCurrentVideoSettings();
    });
    connect(keyframe_interval_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (idx < 0 || !keyframe_interval_combo_)
            return;
        video_settings_.keyframe_interval =
            static_cast<KeyframeIntervalMode>(keyframe_interval_combo_->itemData(idx).toInt());
        emitCurrentVideoSettings();
    });

    // ---- Connects (Quality card expert rows) ----
    // The CQ value IS the model: the named presets are derived from it, never the
    // other way round. (Deriving a preset and then re-seeding the spinbox from that
    // preset is what previously snapped every keystroke back to 19/24/30.)
    connect(quality_cq_spin_, &QSpinBox::valueChanged, this, [this](int cq) {
        video_settings_.cq = static_cast<uint32_t>(cq);
        // Sync the hidden combo so onQualityChanged path stays consistent.
        if (quality_combo_) {
            const QSignalBlocker qb(quality_combo_);
            const int idx =
                quality_combo_->findData(static_cast<int>(recorder_core::NearestQualityPreset(video_settings_.cq)));
            if (idx >= 0)
                quality_combo_->setCurrentIndex(idx);
        }
        updateQualitySegmentSelection();
        emitCurrentVideoSettings();
    });
    // PS-PHASE-C: Rate control dropdown — updates video_settings_.rate_control.
    connect(rate_control_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || !rate_control_combo_)
            return;
        video_settings_.rate_control =
            static_cast<recorder_core::RateControlMode>(rate_control_combo_->itemData(index).toInt());
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

    // ---- Re-sync the freshly built controls from the current models ----
    // The gated combos own their own seeding + item enable/disable (all signal-
    // blocked, none emit). updateExpertModeVisibility() re-runs several of these
    // right after this build returns; the duplicate calls are idempotent.
    updateVideoBitDepthControl();
    updateVideoChromaControl();
    updateVideoColorRangeControl();
    updateVideoEncoderPresetControl();
    updateVideoHdrModeControl();
    updateFramePacingControl();
    // Keyframe interval, CQ and rate control are not touched by the update*Control
    // helpers above — seed them straight from the model (blocked so nothing emits).
    if (keyframe_interval_combo_) {
        const QSignalBlocker b(keyframe_interval_combo_);
        const int idx = keyframe_interval_combo_->findData(static_cast<int>(video_settings_.keyframe_interval));
        keyframe_interval_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* 2 s */);
        keyframe_interval_combo_->setEnabled(!controls_locked_);
    }
    if (quality_cq_spin_) {
        const QSignalBlocker b(quality_cq_spin_);
        quality_cq_spin_->setValue(static_cast<int>(video_settings_.cq));
    }
    if (rate_control_combo_) {
        const QSignalBlocker b(rate_control_combo_);
        const int idx = rate_control_combo_->findData(static_cast<int>(video_settings_.rate_control));
        if (idx >= 0)
            rate_control_combo_->setCurrentIndex(idx);
    }
    if (bitrate_kbps_spin_) {
        const QSignalBlocker b(bitrate_kbps_spin_);
        bitrate_kbps_spin_->setValue(static_cast<int>(video_settings_.bitrate_kbps));
    }
    // CQ row + bitrate row visibility follow the rate-control mode (mirrors the
    // rate-control connect above and updateExpertModeVisibility()).
    {
        const bool rate_is_cq = (video_settings_.rate_control == recorder_core::RateControlMode::ConstantQuality);
        if (quality_expert_widget_)
            quality_expert_widget_->setVisible(expert_mode_enabled_ && rate_is_cq);
        if (bitrate_row_widget_)
            bitrate_row_widget_->setVisible(expert_mode_enabled_ && !rate_is_cq);
    }
}

void ConfigPage::updateExpertModeVisibility() {
    if (expert_mode_enabled_ && !fmt_quality_expert_built_) {
        buildFormatQualityExpertSections();
    }
    if (expert_mode_enabled_ && !developer_card_built_) {
        buildDeveloperCard();
    }
    if (expert_mode_enabled_ && !split_expert_built_) {
        buildSplitExpertSection();
    }
    // startup-perf: lazily build the heavy audio expert subtree the first time
    // expert mode turns on, before the re-seed block below repopulates it.
    if (expert_mode_enabled_ && !audio_expert_built_) {
        buildAudioExpertSection();
    }
    // P2: amber warning banner above the grid follows the expert gate.
    if (expert_warn_banner_)
        expert_warn_banner_->setVisible(expert_mode_enabled_);
    // P3: "Expert mode" label tints to accent when on (QSS repolish on property change).
    if (expert_mode_label_) {
        expert_mode_label_->setProperty("expertOn", expert_mode_enabled_);
        expert_mode_label_->style()->unpolish(expert_mode_label_);
        expert_mode_label_->style()->polish(expert_mode_label_);
    }
    // SETTINGS-TIERS-P3: show/hide the expert-gated Developer card.
    if (developer_card_)
        developer_card_->setVisible(expert_mode_enabled_);
    // Wave 2 Part A: split recording section is now expert-gated (was behind expander).
    if (split_expert_section_)
        split_expert_section_->setVisible(expert_mode_enabled_);
    // v10: Default shows the "Balanced · CQ 24" dropdown; Expert hides it and reveals
    // Rate control + CQ spinbox.
    if (quality_preset_row_widget_)
        quality_preset_row_widget_->setVisible(!expert_mode_enabled_);
    if (quality_rate_section_)
        quality_rate_section_->setVisible(expert_mode_enabled_);
    if (quality_expert_widget_) {
        // In expert mode: CQ spinbox visible when rate=CQ, hidden when VBR/CBR.
        // Default: same as before (visible when expert on).
        const bool rate_is_cq = (video_settings_.rate_control == recorder_core::RateControlMode::ConstantQuality);
        quality_expert_widget_->setVisible(expert_mode_enabled_ && rate_is_cq);
        if (expert_mode_enabled_ && rate_is_cq && quality_cq_spin_) {
            // Seed the spinbox from the model's CQ on first show.
            const QSignalBlocker b(quality_cq_spin_);
            quality_cq_spin_->setValue(static_cast<int>(video_settings_.cq));
        }
    }
    // PS-PHASE-C: fmt_expert_section (rate control, bitrate, format placeholders).
    if (fmt_expert_section_)
        fmt_expert_section_->setVisible(expert_mode_enabled_);
    // v0.9 polish (regroup): Frame pacing is a standalone expert row in the
    // Quality & timing card (not inside quality_rate_section_), so it carries
    // its own expert gate.
    if (frame_pacing_row_)
        frame_pacing_row_->setVisible(expert_mode_enabled_);
    // 0.7.0 — S7: sync the video bit-depth combo + 10-bit gating when the section shows.
    if (expert_mode_enabled_)
        updateVideoBitDepthControl();
    // Sync the chroma combo (4:2:0 / gated 4:4:4) when the section shows.
    if (expert_mode_enabled_)
        updateVideoChromaControl();
    // 0.7.0: sync the colour-range combo (Full/Limited) when the section shows.
    if (expert_mode_enabled_)
        updateVideoColorRangeControl();
    // NVENC-PRESET-R1: sync the encoder-preset combo (P1..P7) when the section shows.
    if (expert_mode_enabled_)
        updateVideoEncoderPresetControl();
    if (expert_mode_enabled_ && rate_control_combo_) {
        // Seed rate control selection from model.
        {
            const QSignalBlocker b(rate_control_combo_);
            const int idx = rate_control_combo_->findData(static_cast<int>(video_settings_.rate_control));
            if (idx >= 0)
                rate_control_combo_->setCurrentIndex(idx);
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
        if (clock_slaving_check_) {
            const QSignalBlocker b(clock_slaving_check_);
            clock_slaving_check_->setChecked(audio_ui_state_.clock_slaving_enabled);
        }
        if (limiter_ceiling_spin_) {
            const QSignalBlocker b(limiter_ceiling_spin_);
            limiter_ceiling_spin_->setValue(static_cast<double>(audio_ui_state_.limiter_ceiling_db));
            limiter_ceiling_spin_->setVisible(audio_ui_state_.limiter_enabled);
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
        if (mic_rnnoise_check_) {
            const QSignalBlocker b(mic_rnnoise_check_);
            mic_rnnoise_check_->setChecked(audio_ui_state_.mic_rnnoise_enabled);
        }
        // Channel / sample-format model (ADR 0030 — 0.6.0).
        if (audio_sample_rate_combo_) {
            const QSignalBlocker b(audio_sample_rate_combo_);
            const int idx = audio_sample_rate_combo_->findData(static_cast<int>(audio_ui_state_.audio_sample_rate));
            if (idx >= 0)
                audio_sample_rate_combo_->setCurrentIndex(idx);
        }
        if (audio_channels_combo_) {
            const QSignalBlocker b(audio_channels_combo_);
            const int idx = audio_channels_combo_->findData(static_cast<int>(audio_ui_state_.audio_channels));
            if (idx >= 0)
                audio_channels_combo_->setCurrentIndex(idx);
        }
        if (audio_bit_depth_combo_) {
            const QSignalBlocker b(audio_bit_depth_combo_);
            const int idx = audio_bit_depth_combo_->findData(
                EncodeBitDepthComboData(audio_ui_state_.audio_bit_depth, audio_ui_state_.audio_pcm_float));
            if (idx >= 0)
                audio_bit_depth_combo_->setCurrentIndex(idx);
        }
        if (flac_compression_spin_) {
            const QSignalBlocker b(flac_compression_spin_);
            flac_compression_spin_->setValue(audio_ui_state_.flac_compression_level);
        }
        updateAudioFormatControlVisibility();
    }
    // PS-PHASE-C: Presence v0.6 placeholder section.
    if (auto* pres_ph = presence_panel_
                            ? presence_panel_->findChild<QWidget*>(QStringLiteral("presenceV1PlaceholderSection"))
                            : nullptr)
        pres_ph->setVisible(expert_mode_enabled_);
}

// SETTINGS-TIERS-P3: presence + appearance setters (moved from AdvancedPage).

void ConfigPage::setShowOverlay(bool show) {
    if (overlay_check_) {
        const QSignalBlocker blocker(overlay_check_);
        overlay_check_->setOn(show);
    }
}

void ConfigPage::setShowDiagnosticsOverlay(bool show) {
    if (diagnostics_overlay_check_) {
        const QSignalBlocker blocker(diagnostics_overlay_check_);
        diagnostics_overlay_check_->setOn(show);
    }
}

void ConfigPage::setShowNotifications(bool show) {
    if (notifications_check_) {
        const QSignalBlocker blocker(notifications_check_);
        notifications_check_->setOn(show);
    }
}

void ConfigPage::setOpenEditorWhenFinished(bool open) {
    if (open_editor_when_finished_check_) {
        const QSignalBlocker blocker(open_editor_when_finished_check_);
        open_editor_when_finished_check_->setOn(open);
    }
}

void ConfigPage::setKeepRunningInTray(bool keep) {
    if (keep_in_tray_check_) {
        const QSignalBlocker blocker(keep_in_tray_check_);
        keep_in_tray_check_->setOn(keep);
    }
}

void ConfigPage::setShowQuickControls(bool show) {
    if (quick_controls_check_) {
        const QSignalBlocker blocker(quick_controls_check_);
        quick_controls_check_->setOn(show);
    }
}

void ConfigPage::setPresentDiagnosticsOptIn(bool on) {
    if (present_diag_check_) {
        const QSignalBlocker blocker(present_diag_check_);
        present_diag_check_->setOn(on);
    }
}

void ConfigPage::setDeveloperLogLevel(const QString& level) {
    developer_log_level_ = level;
    if (!developer_log_level_combo_)
        return; // Developer card not built yet (lazy) -- applied on buildDeveloperCard().
    const int idx = developer_log_level_combo_->findData(level);
    if (idx < 0)
        return;
    const QSignalBlocker blocker(developer_log_level_combo_);
    developer_log_level_combo_->setCurrentIndex(idx);
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

// The "Merge with above" toggle is on when the source folds into the track above,
// so its checked state maps directly to merge_with_above.
void ConfigPage::onAudioAppSeparateToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::App)
            row.merge_with_above = app_separate_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioMicSeparateToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Mic)
            row.merge_with_above = mic_separate_check_->isChecked();
    }
    emitCurrentAudioSettings();
}

void ConfigPage::onAudioSysSeparateToggled() {
    for (auto& row : audio_ui_state_.source_rows) {
        if (row.kind == recorder_core::AudioSourceKind::Sys || row.kind == recorder_core::AudioSourceKind::SystemOutput)
            row.merge_with_above = sys_separate_check_->isChecked();
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

void ConfigPage::setWebcamPreviewFrame(const QImage& frame) {
    if (webcam_setup_panel_)
        webcam_setup_panel_->setPreviewFrame(frame);
}

#if defined(EXOSNAP_ENABLE_VISUAL_TEST_HARNESS)
void ConfigPage::applyVisualWebcamState(bool available, bool mirror) {
    if (webcam_setup_panel_)
        webcam_setup_panel_->applyVisualState(available, mirror);
}

void ConfigPage::applyVisualPresetSaveError(bool show) {
    if (show && !visual_preset_error_label_) {
        // The warning must land on its own row *below* the slim toolbar, not
        // inside the toolbar's QHBoxLayout — sharing that row let the warning
        // text eat the preset combo's stretch space and squeeze it down to a
        // sliver (e.g. "Gaming ("). profile_overflow_btn_'s parent is the
        // toolbar row itself; the toolbar row's parent is the header zone that
        // stacks the toolbar vertically, which is where the warning belongs.
        QWidget* toolbar_row = profile_overflow_btn_ ? profile_overflow_btn_->parentWidget() : nullptr;
        QWidget* header_zone = toolbar_row ? toolbar_row->parentWidget() : nullptr;
        auto* header_vl = header_zone ? qobject_cast<QVBoxLayout*>(header_zone->layout()) : nullptr;
        if (!header_vl)
            return;
        visual_preset_error_label_ = new QLabel(header_zone);
        visual_preset_error_label_->setObjectName(QStringLiteral("presetVisualErrorLabel"));
        visual_preset_error_label_->setProperty("labelRole", "validationError");
        visual_preset_error_label_->setWordWrap(true);
        visual_preset_error_label_->setText(
            QStringLiteral("⚠ Name already exists. Choose a different name before saving."));
        const int toolbar_index = header_vl->indexOf(toolbar_row);
        header_vl->insertWidget(toolbar_index >= 0 ? toolbar_index + 1 : header_vl->count(),
                                visual_preset_error_label_);
    }
    if (visual_preset_error_label_)
        visual_preset_error_label_->setVisible(show);
}

void ConfigPage::applyVisualHdrDisplayPresent(bool present) {
    // Sticky pin for the HDR-row relevance gate: visual scenarios must render the
    // same pixels on every machine, but the real gate reads the probed display
    // facts. Once pinned, updateVideoHdrModeControl() prefers the pin even if the
    // real probe delivers setRuntimeCapabilities() afterwards.
    visual_hdr_display_override_set_ = true;
    visual_hdr_display_override_ = present;
    updateVideoHdrModeControl();
}

void ConfigPage::applyVisualMicPostProcessingExpanded(bool expanded) {
    if (mic_post_disclosure_btn_)
        mic_post_disclosure_btn_->setChecked(expanded);
}

void ConfigPage::applyVisualCustomResolutionInvalid(int width, int height) {
    // Pin an honest invalid Custom-resolution state. setOutputSettings() ran
    // SanitizeOutputResolution(), which resets any unusable size back to Native;
    // this seam re-asserts Custom with the raw (out-of-range) dimensions so the
    // custom fields AND the invalid indicator both render.
    format_settings_.resolution.mode = OutputResolutionMode::Custom;
    format_settings_.resolution.custom_width = static_cast<uint32_t>((std::max)(0, width));
    format_settings_.resolution.custom_height = static_cast<uint32_t>((std::max)(0, height));

    if (output_res_combo_) {
        const QSignalBlocker b(output_res_combo_);
        const int idx = output_res_combo_->findData(static_cast<int>(OutputResolutionMode::Custom));
        if (idx >= 0)
            output_res_combo_->setCurrentIndex(idx);
    }
    if (resolution_compare_hint_)
        resolution_compare_hint_->setCurrentValue(QStringLiteral("Custom"));

    if (custom_resolution_widget_)
        custom_resolution_widget_->setVisible(true);

    // Widen the spin ranges (real min is 320/180) so the intentionally invalid
    // value displays verbatim instead of being clamped. Harness-only.
    if (custom_width_spin_) {
        const QSignalBlocker b(custom_width_spin_);
        custom_width_spin_->setRange(1, 7680);
        custom_width_spin_->setValue(static_cast<int>(format_settings_.resolution.custom_width));
    }
    if (custom_height_spin_) {
        const QSignalBlocker b(custom_height_spin_);
        custom_height_spin_->setRange(1, 7680);
        custom_height_spin_->setValue(static_cast<int>(format_settings_.resolution.custom_height));
    }

    // The invalid indicator reads format_settings_.resolution directly.
    updateCustomResolutionValidation();
}
#endif

void ConfigPage::setRuntimeCapabilities(const capability::CapabilitySet& caps) {
    runtime_caps_ = caps;
    runtime_caps_set_ = true;
    // Re-evaluate the 4:4:4 gate now that the active GPU's real YUV444 support is
    // known (may hide the row + snap back a selection the static rule had allowed).
    updateVideoChromaControl();
    // Re-evaluate the HDR-row relevance gate now that the probed display facts
    // (hdr_active per display) are known.
    updateVideoHdrModeControl();
}

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
    updatePresetActionState(); // re-derives Save as new / Reset / Delete / menu from controls_locked_
    profile_overflow_btn_->setEnabled(enabled);
    if (container_combo_)
        container_combo_->setEnabled(enabled);
    video_codec_combo_->setEnabled(enabled);
    audio_codec_combo_->setEnabled(enabled);
    // 0.7.0 — S7: bit-depth combo honours both the recording lock and codec gating.
    updateVideoBitDepthControl();
    // Chroma combo honours both the recording lock and codec/bit-depth gating.
    updateVideoChromaControl();
    // 0.7.0: colour-range combo honours the recording lock (never codec-gated).
    updateVideoColorRangeControl();
    // NVENC-PRESET-R1: encoder-preset combo honours the recording lock (never codec-gated).
    updateVideoEncoderPresetControl();
    // HDR-handling combo honours both the recording lock and codec gating.
    updateVideoHdrModeControl();
    // ADR 0035 Slice 2: frame-pacing combo honours the recording lock (never codec-gated).
    updateFramePacingControl();
    // 0.9.0 S1: keyframe interval combo honours the recording lock.
    if (keyframe_interval_combo_) {
        {
            const QSignalBlocker b(keyframe_interval_combo_);
            const int idx = keyframe_interval_combo_->findData(static_cast<int>(video_settings_.keyframe_interval));
            keyframe_interval_combo_->setCurrentIndex(idx >= 0 ? idx : 0 /* 2 s */);
        }
        keyframe_interval_combo_->setEnabled(enabled);
        keyframe_interval_combo_->setToolTip(!enabled ? QStringLiteral("Cannot change during recording") : QString());
        if (!enabled)
            keyframe_interval_combo_->setCursor(Qt::ForbiddenCursor);
        else
            keyframe_interval_combo_->unsetCursor();
    }

    quality_combo_->setEnabled(enabled);
    if (quality_preset_combo_)
        quality_preset_combo_->setEnabled(enabled);
    frame_rate_combo_->setEnabled(enabled);
    quality_segment_small_->setEnabled(enabled);
    quality_segment_balanced_->setEnabled(enabled);
    quality_segment_high_->setEnabled(enabled);
    updateTimingSelection();
    cursor_check_->setEnabled(enabled);
    if (output_res_combo_)
        output_res_combo_->setEnabled(enabled);

    if (webcam_setup_panel_)
        webcam_setup_panel_->setControlsLocked(locked);

    destination_edit_->setEnabled(enabled);
    browse_btn_->setEnabled(enabled);
    naming_edit_->setEnabled(enabled);

    // The updates action (Check / Update to vX.Y / swap-launch) must not fire while a
    // recording or MP4 finalize is in flight. On unlock, restore the state-derived
    // enabled value so a "pending"/"checking" state stays correctly disabled.
    if (updates_action_btn_)
        updates_action_btn_->setEnabled(updates_action_intrinsically_enabled_ && enabled);

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

void ConfigPage::setAutoUpdateCheck(bool on) {
    if (updates_auto_toggle_) {
        QSignalBlocker block(updates_auto_toggle_);
        updates_auto_toggle_->setOn(on);
    }
}

// ADR 0034 Phase A: render the live Updates-card state. Driven by MainWindow
// from UpdateService results.
void ConfigPage::setUpdateStatus(const QString& state, const QString& available_version, const QString& last_checked,
                                 const QString& detail) {
    if (!updates_status_label_ || !updates_action_btn_)
        return;
    // The button's primary action (open releases vs. launch the swap updater) is
    // decided by MainWindow when updatePrimaryActionRequested fires; here it only
    // needs a non-empty version to route to that signal instead of a re-check.
    updates_available_version_ =
        (state == QStringLiteral("available") || state == QStringLiteral("scoop")) ? available_version : QString();

    if (state == QStringLiteral("checking")) {
        updates_status_label_->setText(QStringLiteral("Checking for updates\xe2\x80\xa6"));
        updates_action_btn_->setText(QStringLiteral("Check for updates"));
        updates_action_intrinsically_enabled_ = false;
    } else if (state == QStringLiteral("available")) {
        updates_status_label_->setText(QStringLiteral("Update available \xe2\x80\x94 %1").arg(available_version));
        updates_action_btn_->setText(QStringLiteral("Update to %1").arg(available_version));
        updates_action_intrinsically_enabled_ = true;
    } else if (state == QStringLiteral("scoop")) {
        // Notify-only: Scoop owns the update; we never run the staged swap here.
        updates_status_label_->setText(
            QStringLiteral("Managed by Scoop \xe2\x80\x94 update with 'scoop update exosnap'"));
        updates_action_btn_->setText(QStringLiteral("Open releases page"));
        updates_action_intrinsically_enabled_ = true;
    } else if (state == QStringLiteral("pending")) {
        // Loop guard: the updater is staged/launched for this version. Restart is
        // pending; don't re-offer the Update CTA.
        updates_status_label_->setText(QStringLiteral("Restart pending\xe2\x80\xa6 finishing the update"));
        updates_action_btn_->setText(QStringLiteral("Restart pending"));
        updates_action_intrinsically_enabled_ = false;
    } else if (state == QStringLiteral("error")) {
        updates_status_label_->setText(detail.isEmpty() ? QStringLiteral("Couldn't check for updates") : detail);
        updates_action_btn_->setText(QStringLiteral("Retry"));
        updates_action_intrinsically_enabled_ = true;
    } else { // "uptodate"
        const QString suffix = last_checked.isEmpty() ? QString() : QStringLiteral(" \xc2\xb7 %1").arg(last_checked);
        updates_status_label_->setText(QStringLiteral("\xe2\x9c\x93 Up to date%1").arg(suffix));
        updates_action_btn_->setText(QStringLiteral("Check for updates"));
        updates_action_intrinsically_enabled_ = true;
    }

    // A recording/finalizing lock always wins: never allow the swap/check action to
    // fire while a recording is in flight (belt to the MainWindow handler guard).
    updates_action_btn_->setEnabled(updates_action_intrinsically_enabled_ && !controls_locked_);

    // WHATS-NEW: the "What's new in vX.Y" link appears only in the available state.
    // The suppress setting never hides this link (it only gates the post-update
    // auto-show).
    if (updates_whats_new_link_) {
        const bool show_link = (state == QStringLiteral("available")) && !available_version.isEmpty();
        updates_whats_new_link_->setVisible(show_link);
        if (show_link)
            updates_whats_new_link_->setText(QStringLiteral("What's new in %1").arg(available_version));
    }

    // Accent CTA styling only in the available state (QSS [updatesCta="true"]).
    updates_action_btn_->setProperty("updatesCta", state == QStringLiteral("available"));
    updates_action_btn_->style()->unpolish(updates_action_btn_);
    updates_action_btn_->style()->polish(updates_action_btn_);
}

// Deep-link landing cue: briefly pulse the accent border of the jumped-to section and
// move focus into it, so a deep-link (e.g. the hotkey "Rebind" notification action) gives
// visible feedback even when the target card is already within the viewport —
// ensureWidgetVisible is then a no-op and, without this, the click looks like it did
// nothing. Cleared after ~1.2s. The single-shot timer uses the panel as its context
// object, so it auto-cancels if the panel is destroyed before it fires.
static void pulseSectionLanding(QWidget* panel) {
    if (!panel)
        return;
    panel->setProperty("landing", true);
    panel->style()->unpolish(panel);
    panel->style()->polish(panel);
    const auto controls = panel->findChildren<QWidget*>();
    for (QWidget* control : controls) {
        if (control->focusPolicy() != Qt::NoFocus && control->isEnabled() && control->isVisibleTo(panel)) {
            control->setFocus(Qt::OtherFocusReason);
            break;
        }
    }
    QTimer::singleShot(1200, panel, [panel]() {
        panel->setProperty("landing", QVariant());
        panel->style()->unpolish(panel);
        panel->style()->polish(panel);
    });
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
    else if (section_target == QStringLiteral("settings/updates"))
        target_widget = updates_panel_;

    if (target_widget) {
        scroll_area_->ensureWidgetVisible(target_widget);
        pulseSectionLanding(target_widget);
    }
}

} // namespace exosnap

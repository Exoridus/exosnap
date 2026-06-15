// NotificationToastWindow.cpp — NOTIFY-SKIN-R1
// Skinned toast renderer matching the Mappe Wave 0.3 anatomy exactly.
// Visual tokens from HT (hybrid-shared.jsx) + Toast (mappe-kit.jsx).

#include "NotificationToastWindow.h"

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QShowEvent>

#include "notifications/NotificationManager.h"

#if defined(Q_OS_WIN)
#include <windows.h>
// WDA_EXCLUDEFROMCAPTURE was introduced in Windows 10 2004 (build 19041).
#if !defined(WDA_EXCLUDEFROMCAPTURE)
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace exosnap::ui::overlay {

// ---------------------------------------------------------------------------
// Design tokens — exact match to Mappe HT + Toast spec (NOTIFY-SKIN-R1)
// ---------------------------------------------------------------------------
namespace {

// ── Surfaces ──────────────────────────────────────────────────────────────
// raise = #242428 (HT.raise) — the toast card background
constexpr QColor kCardBg{0x24, 0x24, 0x28, 255};
// line2 = rgba(255,255,255,0.12) — card border
constexpr QColor kCardBorder{255, 255, 255, 31}; // 0.12 * 255 ≈ 31

// ── Text ──────────────────────────────────────────────────────────────────
constexpr QColor kInk{0xF1, 0xF1, 0xEF, 255}; // title
constexpr QColor kMut{0x9C, 0x9C, 0x9A, 255}; // body
constexpr QColor kDim{0x65, 0x65, 0x6A, 255}; // dismiss / mono labels

// ── Accent (Studio Mint) ──────────────────────────────────────────────────
constexpr QColor kAccent{0x9B, 0xD9, 0xD2, 255};    // #9BD9D2
constexpr QColor kAccentInk{0x08, 0x13, 0x0F, 255}; // #08130F — text on primary action

// ── Status palettes ───────────────────────────────────────────────────────
// success: color #84CBA2, dim rgba(132,203,162,0.13), border rgba(132,203,162,0.45)
constexpr QColor kSuccessC{0x84, 0xCB, 0xA2, 255};
constexpr QColor kSuccessDim{132, 203, 162, 33}; // 0.13 * 255 ≈ 33
constexpr QColor kSuccessB{132, 203, 162, 115};  // 0.45 * 255 ≈ 115

// caution: color #E6C57C, dim rgba(230,197,124,0.12), border rgba(230,197,124,0.42)
constexpr QColor kCautionC{0xE6, 0xC5, 0x7C, 255};
constexpr QColor kCautionDim{230, 197, 124, 31}; // 0.12 * 255 ≈ 31
constexpr QColor kCautionB{230, 197, 124, 107};  // 0.42 * 255 ≈ 107

// error: color #E0786C, dim rgba(224,120,108,0.13), border rgba(224,120,108,0.42)
constexpr QColor kErrorC{0xE0, 0x78, 0x6C, 255};
constexpr QColor kErrorDim{224, 120, 108, 33}; // 0.13 * 255 ≈ 33
constexpr QColor kErrorB{224, 120, 108, 107};  // 0.42 * 255 ≈ 107

// info = accent
constexpr QColor kInfoC{0x9B, 0xD9, 0xD2, 255};
constexpr QColor kInfoDim{155, 217, 210, 36}; // 0.14 * 255 ≈ 36
constexpr QColor kInfoB{155, 217, 210, 153};  // 0.60 * 255 ≈ 153

// ── Geometry ──────────────────────────────────────────────────────────────
// Toast card width = 372px (ToastAnatomy spec).
constexpr int kToastWidth = 372;
constexpr int kToastRadius = 14;  // borderRadius: 14 in mappe-kit Toast
constexpr int kStackGap = 12;     // gap between stacked toasts
constexpr int kMarginRight = 20;  // anchor from screen right edge
constexpr int kMarginBottom = 20; // anchor from screen bottom edge

// ── Card interior ─────────────────────────────────────────────────────────
// Padding: "14px 15px 14px 16px" (top, right, bottom, left)
constexpr int kPadTop = 14;
constexpr int kPadRight = 15;
constexpr int kPadBottom = 14;
constexpr int kPadLeft = 16;

// Glyph chip: 30px, borderRadius 9
constexpr int kChipSize = 30;
constexpr int kChipRadius = 9;
constexpr int kChipGlyphSize = 16; // icon inside chip

// Gap between chip and text column
constexpr int kChipTextGap = 12;

// Dismiss button area (top-right ✕)
constexpr int kDismissSize = 18;

// Action pill geometry
constexpr int kPillH = 28;       // approximate: "6px 12px" padding = ~28px tall
constexpr int kPillRadius = 999; // borderRadius: 999 → fully rounded
constexpr int kPillGapX = 8;     // gap between pills

// Progress bar (auto-dismiss countdown)
constexpr int kBarH = 3; // "height: 3" — bottom hairline

// Vertical content heights for layout calculation
constexpr int kTitleH = 18;         // single line title
constexpr int kBodyH = 18;          // height per body line
constexpr int kBodyMaxLines = 2;    // word-wrap up to 2 lines; elide beyond
constexpr int kBodyGapTop = 3;      // marginTop: 3 between title and body
constexpr int kActionsGapTop = 11;  // marginTop: 11 before action row
constexpr int kPillHTotal = kPillH; // pill row height

// ── Font sizes ────────────────────────────────────────────────────────────
// Title: 13.5px / weight 600 (spec), Body: 12.5px, Action: 12.5px / 600
// Qt pixel sizes for "px" values: use setPixelSize.
constexpr int kTitlePx = 14;  // ~13.5px → round to 14 for QPainter
constexpr int kBodyPx = 13;   // ~12.5px → round to 13
constexpr int kActionPx = 13; // same as body

// ── Glyph rendering ───────────────────────────────────────────────────────
// Glyphs: checkmark (success), triangle (caution), circle-x (error), circle-i (info)
// Drawn as paths in the chip center.

struct StatusTokens {
    QColor c;   // text / icon color
    QColor dim; // chip background
    QColor b;   // chip border
};

StatusTokens tokensForType(notifications::NotificationType type) noexcept {
    switch (type) {
    case notifications::NotificationType::Saved:
        return {kSuccessC, kSuccessDim, kSuccessB};
    case notifications::NotificationType::LowStorage:
        return {kCautionC, kCautionDim, kCautionB};
    case notifications::NotificationType::UnexpectedStop:
        return {kErrorC, kErrorDim, kErrorB};
    case notifications::NotificationType::RecoveryAvailable:
        return {kInfoC, kInfoDim, kInfoB};
    case notifications::NotificationType::UpdateAvailable:
        return {kInfoC, kInfoDim, kInfoB}; // azure/info tone
    }
    return {kInfoC, kInfoDim, kInfoB};
}

bool isSticky(notifications::NotificationType type) noexcept {
    // Mirror the NotificationManager per-type dwell constants without calling
    // the private DismissIntervalMs(). Values match kDismissMs_* constants.
    switch (type) {
    case notifications::NotificationType::Saved:
        return notifications::NotificationManager::kDismissMs_Saved == 0;
    case notifications::NotificationType::LowStorage:
        return notifications::NotificationManager::kDismissMs_LowStorage == 0;
    case notifications::NotificationType::UnexpectedStop:
        return notifications::NotificationManager::kDismissMs_UnexpectedStop == 0;
    case notifications::NotificationType::RecoveryAvailable:
        return notifications::NotificationManager::kDismissMs_RecoveryAvailable == 0;
    case notifications::NotificationType::UpdateAvailable:
        return notifications::NotificationManager::kDismissMs_UpdateAvailable == 0;
    }
    return true;
}

// Draw a status glyph centered at (cx, cy) with the given size and color.
// Glyphs match the Mappe STATUS map entries:
//   success → checkCircle, caution → alertTriangle,
//   error   → error (circle-x), info → info (circle-i)
void drawStatusGlyph(QPainter& p, notifications::NotificationType type, int cx, int cy, int sz, const QColor& color) {
    p.save();
    p.setPen(QPen(color, 1.5f));
    p.setBrush(Qt::NoBrush);
    const float r = sz * 0.5f;
    const QRectF circle(cx - r, cy - r, sz, sz);

    switch (type) {
    case notifications::NotificationType::Saved: {
        // checkCircle: circle + check mark
        p.drawEllipse(circle);
        // check: from ~(cx-r*0.45, cy) to (cx-r*0.12, cy+r*0.38) to (cx+r*0.45, cy-r*0.3)
        QPointF p1(cx - r * 0.4f, cy + r * 0.05f);
        QPointF p2(cx - r * 0.1f, cy + r * 0.38f);
        QPointF p3(cx + r * 0.42f, cy - r * 0.28f);
        p.drawLine(p1, p2);
        p.drawLine(p2, p3);
        break;
    }
    case notifications::NotificationType::LowStorage: {
        // alertTriangle: equilateral triangle + ! inside
        const float h = sz * 0.86f;
        const float base = sz * 0.88f;
        QPolygonF tri;
        tri << QPointF(cx, cy - h * 0.5f) << QPointF(cx - base * 0.5f, cy + h * 0.5f)
            << QPointF(cx + base * 0.5f, cy + h * 0.5f);
        p.drawPolygon(tri);
        // exclamation mark
        p.drawLine(QPointF(cx, cy - h * 0.22f), QPointF(cx, cy + h * 0.12f));
        p.setPen(QPen(color, 2.0f, Qt::SolidLine, Qt::RoundCap));
        p.drawPoint(QPointF(cx, cy + h * 0.27f));
        break;
    }
    case notifications::NotificationType::UnexpectedStop: {
        // error / circle-x
        p.drawEllipse(circle);
        const float d = r * 0.38f;
        p.drawLine(QPointF(cx - d, cy - d), QPointF(cx + d, cy + d));
        p.drawLine(QPointF(cx + d, cy - d), QPointF(cx - d, cy + d));
        break;
    }
    case notifications::NotificationType::RecoveryAvailable: {
        // info: circle + "i"
        p.drawEllipse(circle);
        p.setPen(QPen(color, 1.8f, Qt::SolidLine, Qt::RoundCap));
        p.drawPoint(QPointF(cx, cy - r * 0.28f));
        p.setPen(QPen(color, 1.5f));
        p.drawLine(QPointF(cx, cy - r * 0.05f), QPointF(cx, cy + r * 0.38f));
        break;
    }
    case notifications::NotificationType::UpdateAvailable: {
        // download: down-arrow into a tray (Lucide "download").
        p.setPen(QPen(color, 1.6f, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        // Vertical shaft.
        p.drawLine(QPointF(cx, cy - r * 0.62f), QPointF(cx, cy + r * 0.22f));
        // Arrowhead.
        p.drawLine(QPointF(cx - r * 0.34f, cy - r * 0.12f), QPointF(cx, cy + r * 0.22f));
        p.drawLine(QPointF(cx + r * 0.34f, cy - r * 0.12f), QPointF(cx, cy + r * 0.22f));
        // Tray base.
        p.drawLine(QPointF(cx - r * 0.5f, cy + r * 0.5f), QPointF(cx + r * 0.5f, cy + r * 0.5f));
        break;
    }
    }
    p.restore();
}

// Draw a compact ✕ at the given top-right corner rect.
void drawDismissX(QPainter& p, const QRectF& rect, const QColor& color) {
    p.save();
    p.setPen(QPen(color, 1.3f, Qt::SolidLine, Qt::RoundCap));
    const float m = rect.width() * 0.28f;
    p.drawLine(rect.left() + m, rect.top() + m, rect.right() - m, rect.bottom() - m);
    p.drawLine(rect.right() - m, rect.top() + m, rect.left() + m, rect.bottom() - m);
    p.restore();
}

// Compute the number of body lines needed (1 or 2) for a given body text
// within the available text column width.
// text_w must be the same width used during paint (kToastWidth - text_x - kPadRight - kDismissSize - 6).
int bodyLineCount(const QString& body, const QFontMetrics& body_fm, int text_w) {
    if (body.isEmpty())
        return 1;
    // If the body fits on a single line, use 1 line.
    if (body_fm.horizontalAdvance(body) <= text_w)
        return 1;
    // Otherwise: use 2 lines (the painter will word-wrap within the 2-line rect).
    return kBodyMaxLines;
}

// The text column x-start and width match the paintEvent layout exactly.
// text_x = kPadLeft + kChipSize + kChipTextGap
// text_w = kToastWidth - text_x - kPadRight - kDismissSize - 6
constexpr int kTextX = kPadLeft + kChipSize + kChipTextGap;
constexpr int kTextW = kToastWidth - kTextX - kPadRight - kDismissSize - 6;

// Calculate the total height of a single toast card including all sections.
int toastHeight(const notifications::NotificationEvent& event, const QFontMetrics& title_fm,
                const QFontMetrics& body_fm) {
    Q_UNUSED(title_fm);

    // Body may wrap to up to 2 lines.
    const int body_lines = bodyLineCount(event.body, body_fm, kTextW);
    const int body_total_h = kBodyH * body_lines;

    // Base: padTop + chip(30) but also: title + body + possibly actions + padBottom
    // Layout: top pad, then content row with chip (30px) and text column.
    // Text column: title + gap + body. Below body: optional action row.
    int content_h = kTitleH + kBodyGapTop + body_total_h;
    const bool has_action = event.hasAction();
    if (has_action)
        content_h += kActionsGapTop + kPillHTotal;

    // Card height = padTop + max(chip, content) + padBottom + optional bar
    int card_content = qMax(kChipSize, content_h);
    int h = kPadTop + card_content + kPadBottom;

    // Countdown bar at the bottom of non-sticky toasts
    if (!isSticky(event.type))
        h += kBarH;

    return h;
}

} // namespace

// ---------------------------------------------------------------------------
// NotificationToastWindow implementation
// ---------------------------------------------------------------------------

NotificationToastWindow::NotificationToastWindow(notifications::NotificationManager* manager, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                          Qt::NoDropShadowWindowHint),
      manager_(manager) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_AlwaysStackOnTop, true);

    if (manager_) {
        connect(manager_, &notifications::NotificationManager::visibleSetChanged, this,
                &NotificationToastWindow::onVisibleSetChanged);
    }
}

bool NotificationToastWindow::isExcluded() const noexcept {
    return excluded_;
}

QSize NotificationToastWindow::sizeHint() const {
    const auto* events_ptr = manager_ ? &manager_->VisibleEvents() : nullptr;
    const int n = events_ptr ? events_ptr->size() : 0;
    if (n == 0)
        return QSize(kToastWidth, 0);

    // Build title/body fonts to measure heights.
    QFont title_f;
    title_f.setFamily(QStringLiteral("Hanken Grotesk"));
    title_f.setPixelSize(kTitlePx);
    title_f.setWeight(QFont::DemiBold);

    QFont body_f;
    body_f.setFamily(QStringLiteral("Hanken Grotesk"));
    body_f.setPixelSize(kBodyPx);
    body_f.setWeight(QFont::Normal);

    const QFontMetrics title_fm(title_f);
    const QFontMetrics body_fm(body_f);

    int total_h = 0;
    for (int i = 0; i < n; ++i) {
        total_h += toastHeight((*events_ptr)[i], title_fm, body_fm);
        if (i < n - 1)
            total_h += kStackGap;
    }
    return QSize(kToastWidth, total_h);
}

QSize NotificationToastWindow::minimumSizeHint() const {
    return sizeHint();
}

void NotificationToastWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!exclusion_attempted_) {
        applyExclusion();
        if (!excluded_) {
            hide();
        }
    }
}

void NotificationToastWindow::paintEvent(QPaintEvent* /*event*/) {
    if (!manager_)
        return;

    const auto& events = manager_->VisibleEvents();
    if (events.isEmpty())
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // ── Fonts ──────────────────────────────────────────────────────────────
    QFont title_font;
    title_font.setFamily(QStringLiteral("Hanken Grotesk"));
    title_font.setPixelSize(kTitlePx);
    title_font.setWeight(QFont::DemiBold); // 600
    const QFontMetrics title_fm(title_font);

    QFont body_font;
    body_font.setFamily(QStringLiteral("Hanken Grotesk"));
    body_font.setPixelSize(kBodyPx);
    body_font.setWeight(QFont::Normal);
    const QFontMetrics body_fm(body_font);

    QFont action_font;
    action_font.setFamily(QStringLiteral("Hanken Grotesk"));
    action_font.setPixelSize(kActionPx);
    action_font.setWeight(QFont::DemiBold); // 600

    int y_offset = 0;
    for (int idx = 0; idx < events.size(); ++idx) {
        const auto& event = events[idx];
        const StatusTokens tok = tokensForType(event.type);
        const bool sticky = isSticky(event.type);
        const int card_h = toastHeight(event, title_fm, body_fm);

        // ── Card background ───────────────────────────────────────────────
        const QRectF card_rect(0.0, y_offset, kToastWidth, card_h - (sticky ? 0 : kBarH));
        QPainterPath card_path;
        card_path.addRoundedRect(card_rect, kToastRadius, kToastRadius);

        // Drop shadow — approximate with a slightly offset semi-transparent fill
        // (Qt's QWidget has no native shadow; we paint a simple blur-offset approximation)
        // box-shadow: 0 18px 48px rgba(0,0,0,0.5) — handled by WA_TranslucentBackground
        // For a crisp shadow we use an expanded rect behind the card.
        const QRectF shadow_rect = card_rect.adjusted(-2, 4, 2, 18);
        QPainterPath shadow_path;
        shadow_path.addRoundedRect(shadow_rect, kToastRadius + 2, kToastRadius + 2);
        p.fillPath(shadow_path, QColor(0, 0, 0, 64));

        // Card fill
        p.fillPath(card_path, kCardBg);

        // Card border: line2 = rgba(255,255,255,0.12)
        p.save();
        p.setClipPath(card_path);
        QPen border_pen(kCardBorder);
        border_pen.setWidthF(1.0);
        p.setPen(border_pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(card_path);
        p.restore();

        // ── Glyph chip (30px, borderRadius 9) ────────────────────────────
        const int chip_x = kPadLeft;
        // Vertically center chip in content area (excluding bar)
        const int content_area_h = card_h - (sticky ? 0 : kBarH) - kPadTop - kPadBottom;
        const int chip_y = y_offset + kPadTop + (content_area_h - kChipSize) / 2;
        const QRectF chip_rect(chip_x, chip_y, kChipSize, kChipSize);

        QPainterPath chip_path;
        chip_path.addRoundedRect(chip_rect, kChipRadius, kChipRadius);
        p.fillPath(chip_path, tok.dim);
        QPen chip_border(tok.b);
        chip_border.setWidthF(1.0);
        p.strokePath(chip_path, chip_border);

        // Status glyph centered in chip
        const int glyph_cx = chip_x + kChipSize / 2;
        const int glyph_cy = chip_y + kChipSize / 2;
        drawStatusGlyph(p, event.type, glyph_cx, glyph_cy, kChipGlyphSize, tok.c);

        // ── Text column ───────────────────────────────────────────────────
        const int text_x = kPadLeft + kChipSize + kChipTextGap;
        const int text_w = kToastWidth - text_x - kPadRight - kDismissSize - 6;

        // Title row: title + optional dismiss ✕
        int text_y_top = y_offset + kPadTop;
        // Vertically align title to the top of the content area (matches spec's flex column)
        const int title_y = text_y_top + (kChipSize / 2 - (kTitleH + kBodyGapTop + kBodyH) / 2);

        // Clamp to padTop to avoid painting above the card
        const int effective_title_y = qMax(text_y_top, title_y);

        // Title
        p.setFont(title_font);
        p.setPen(kInk);
        const QRect title_rect(text_x, effective_title_y, text_w, kTitleH);
        p.drawText(title_rect, Qt::AlignVCenter | Qt::AlignLeft,
                   title_fm.elidedText(event.title, Qt::ElideRight, text_w));

        // Body: word-wrap up to 2 lines; elide only if it still overflows after 2 lines.
        p.setFont(body_font);
        p.setPen(kMut);
        {
            const int body_lines = bodyLineCount(event.body, body_fm, text_w);
            const int body_total_h = kBodyH * body_lines;
            const QRect body_rect(text_x, effective_title_y + kTitleH + kBodyGapTop, text_w, body_total_h);
            if (body_lines > 1) {
                // Word-wrap: let Qt break lines naturally within the rect.
                // If the text still doesn't fit after wrapping, elide the last line.
                p.drawText(body_rect, Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, event.body);
            } else {
                // Single line: elide if it overflows.
                p.drawText(body_rect, Qt::AlignVCenter | Qt::AlignLeft,
                           body_fm.elidedText(event.body, Qt::ElideRight, text_w));
            }
        }

        // ── Dismiss ✕ ─────────────────────────────────────────────────────
        const QRectF dismiss_rect(kToastWidth - kPadRight - kDismissSize, y_offset + kPadTop, kDismissSize,
                                  kDismissSize);
        drawDismissX(p, dismiss_rect, kDim);

        // ── Action pills ──────────────────────────────────────────────────
        if (event.hasAction()) {
            const int body_lines = bodyLineCount(event.body, body_fm, text_w);
            const int body_total_h = kBodyH * body_lines;
            const int actions_y = effective_title_y + kTitleH + kBodyGapTop + body_total_h + kActionsGapTop;

            // Determine primary / secondary button labels from the action enum
            struct ButtonSpec {
                QString label;
                bool primary;
            };
            QVector<ButtonSpec> buttons;

            switch (event.action) {
            case notifications::NotificationAction::OpenFolder:
                buttons.push_back({QStringLiteral("Open folder"), false});
                break;
            case notifications::NotificationAction::OpenRecovery:
                buttons.push_back({QStringLiteral("Recover"), true});
                if (event.secondary_action == notifications::NotificationAction::Discard)
                    buttons.push_back({QStringLiteral("Discard"), false});
                break;
            case notifications::NotificationAction::ChangeFolder:
                buttons.push_back({QStringLiteral("Change folder"), true});
                if (event.secondary_action == notifications::NotificationAction::None ||
                    event.secondary_action == notifications::NotificationAction::Discard)
                    buttons.push_back({QStringLiteral("Dismiss"), false});
                break;
            case notifications::NotificationAction::ShowFile:
                buttons.push_back({QStringLiteral("Show file"), true});
                break;
            case notifications::NotificationAction::OpenUpdate:
                buttons.push_back({QStringLiteral("View update"), true});
                break;
            case notifications::NotificationAction::Discard:
                buttons.push_back({QStringLiteral("Discard"), false});
                break;
            case notifications::NotificationAction::None:
            default:
                break;
            }

            int pill_x = text_x;
            p.setFont(action_font);
            for (const auto& btn : buttons) {
                const int label_w = QFontMetrics(action_font).horizontalAdvance(btn.label);
                const int pill_w = label_w + 24; // 12px each side
                const QRectF pill_rect(pill_x, actions_y, pill_w, kPillH);
                QPainterPath pill_path;
                pill_path.addRoundedRect(pill_rect, kPillRadius, kPillRadius);

                if (btn.primary) {
                    // Primary: accent fill, accent-ink text
                    p.fillPath(pill_path, kAccent);
                    p.setPen(kAccentInk);
                } else {
                    // Ghost: transparent fill, status-color border
                    p.fillPath(pill_path, Qt::transparent);
                    QPen ghost_border(tok.b);
                    ghost_border.setWidthF(1.0);
                    p.strokePath(pill_path, ghost_border);
                    p.setPen(tok.c);
                }
                p.drawText(pill_rect.toRect(), Qt::AlignCenter, btn.label);
                pill_x += pill_w + kPillGapX;
            }
        }

        // ── Auto-dismiss countdown bar (non-sticky only) ──────────────────
        if (!sticky) {
            // The bar sits below the card rounded rect; we render it in the full
            // card_h slot (bottom kBarH pixels) as a track + fill.
            const int bar_y = y_offset + card_h - kBarH;
            // Track (dim background)
            const QRectF track_rect(0, bar_y, kToastWidth, kBarH);
            QPainterPath track_path;
            track_path.addRoundedRect(track_rect, 0, 0);
            p.fillPath(track_path, QColor(255, 255, 255, 15));

            // Fill starts at 100% (full bar) — the manager drives actual progress
            // via dismiss; we render a static full bar for the placeholder.
            // In the future the window will receive a progress [0..1] per toast.
            // For now: paint the status-color bar at the full width to show the widget.
            const QRectF fill_rect(0, bar_y, kToastWidth, kBarH);
            p.fillPath(
                [&]() {
                    QPainterPath fp;
                    fp.addRoundedRect(fill_rect, 0, 0);
                    return fp;
                }(),
                tok.c);
        }

        y_offset += card_h + kStackGap;
    }
}

void NotificationToastWindow::applyExclusion() {
    exclusion_attempted_ = true;
    excluded_ = false;

#if defined(Q_OS_WIN)
    // Ensure the HWND exists. winId() forces creation.
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;

    // WDA_EXCLUDEFROMCAPTURE (0x11) requires Windows 10 2004+.
    // If this fails the window hides and stays hidden for the session — an
    // overlay that cannot guarantee capture exclusion must not be visible.
    const BOOL ok = ::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    excluded_ = (ok != FALSE);
#else
    excluded_ = false;
#endif
}

void NotificationToastWindow::updatePosition() {
    const QSize hint = sizeHint();
    if (hint.height() <= 0) {
        hide();
        return;
    }

    resize(hint);

    // Anchor to the PRIMARY display (not the recorded monitor; notifications
    // are app-level events per ADR 0016). Bottom-right corner, newest on top
    // (stack grows upward from bottom-right per ToastStack spec).
    QRect screen_rect;
    const QScreen* primary = QGuiApplication::primaryScreen();
    if (primary)
        screen_rect = primary->availableGeometry(); // respects taskbar

    if (screen_rect.isNull() || screen_rect.isEmpty()) {
        // Fallback: top-left origin
        move(kMarginRight, kMarginBottom);
        return;
    }

    const int x = screen_rect.right() - hint.width() - kMarginRight;
    const int y = screen_rect.bottom() - hint.height() - kMarginBottom;
    move(x, y);
}

void NotificationToastWindow::onVisibleSetChanged() {
    if (!excluded_ && exclusion_attempted_) {
        // Exclusion failed at startup; refuse to show.
        return;
    }

    const bool has_events = manager_ && !manager_->VisibleEvents().isEmpty();

    if (!has_events) {
        hide();
        return;
    }

    if (!exclusion_attempted_) {
        applyExclusion();
        if (!excluded_) {
            return; // hide and stay hidden
        }
    }

    updatePosition();
    update();
    show();
    raise();
}

} // namespace exosnap::ui::overlay

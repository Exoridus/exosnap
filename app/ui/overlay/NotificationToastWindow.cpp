// NotificationToastWindow.cpp — NOTIFY-SKIN-R1
// Skinned toast renderer matching the Mappe Wave 0.3 anatomy exactly.
// Visual tokens from HT (hybrid-shared.jsx) + Toast (mappe-kit.jsx).

#include "NotificationToastWindow.h"

#include <QApplication>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QWindow>

#include <algorithm>

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
constexpr QColor kAccentInk{0x08, 0x13, 0x0F, 255}; // #08130F — text on mint accent
// Dark ink for tonal primary buttons (v10: button fill = tone color, not always mint)
constexpr QColor kButtonInk{0x0E, 0x0E, 0x10, 255}; // #0E0E10 — legible on all light tones

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
// The WINDOW is kToastWidth + 2 * kShadowMargin wide; the card sits at
// x = kShadowMargin inside the window. (kShadowMargin == public constant.)
constexpr int kToastWidth = NotificationToastWindow::kCardWidth;      // 372
constexpr int kShadowMargin = NotificationToastWindow::kShadowMargin; // 20
constexpr int kToastRadius = 14;                                      // borderRadius: 14 in mappe-kit Toast
constexpr int kStackGap = 12;                                         // gap between stacked toasts
constexpr int kMarginRight = 20;                                      // anchor from screen right edge
constexpr int kMarginBottom = 20;                                     // anchor from screen bottom edge

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
constexpr int kPillH = 28;      // approximate: "6px 12px" padding = ~28px tall
constexpr int kPillRadius = 10; // v10: borderRadius 10 (was 999 fully-round)
constexpr int kPillGapX = 8;    // gap between pills

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
    case notifications::NotificationType::FramesDropped: // caution tone, shared with LowStorage
    case notifications::NotificationType::OverlayOmitted:
    case notifications::NotificationType::HotkeyConflict: // caution tone, shared with FramesDropped
    case notifications::NotificationType::LowStorage:
        return {kCautionC, kCautionDim, kCautionB};
    case notifications::NotificationType::UnexpectedStop:
    case notifications::NotificationType::SettingsSaveFailed: // error tone, shared with UnexpectedStop
        return {kErrorC, kErrorDim, kErrorB};
    case notifications::NotificationType::RecoveryAvailable:
        return {kInfoC, kInfoDim, kInfoB};
    case notifications::NotificationType::UpdateAvailable:
    case notifications::NotificationType::SettingsRepaired: // info tone, shared with UpdateAvailable
    case notifications::NotificationType::PresetSwitched:   // info tone, shared with UpdateAvailable
        return {kInfoC, kInfoDim, kInfoB};                  // azure/info tone
    }
    return {kInfoC, kInfoDim, kInfoB};
}

// A standing notification reports a condition that still holds: no countdown
// bar (nothing is counting down). A timed one leaves on its own, so the bar
// appears exactly when the toast will auto-dismiss.
bool isStanding(notifications::NotificationType type) noexcept {
    return notifications::NotificationManager::IsStanding(type);
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
    case notifications::NotificationType::FramesDropped: // caution glyph, shared with LowStorage
    case notifications::NotificationType::OverlayOmitted:
    case notifications::NotificationType::HotkeyConflict: // caution glyph, shared with FramesDropped
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
    case notifications::NotificationType::UnexpectedStop:
    case notifications::NotificationType::SettingsSaveFailed: { // error glyph, shared with UnexpectedStop
        // error / circle-x
        p.drawEllipse(circle);
        const float d = r * 0.38f;
        p.drawLine(QPointF(cx - d, cy - d), QPointF(cx + d, cy + d));
        p.drawLine(QPointF(cx + d, cy - d), QPointF(cx - d, cy + d));
        break;
    }
    case notifications::NotificationType::RecoveryAvailable:
    case notifications::NotificationType::PresetSwitched: { // info glyph, shared with RecoveryAvailable
        // info: circle + "i"
        p.drawEllipse(circle);
        p.setPen(QPen(color, 1.8f, Qt::SolidLine, Qt::RoundCap));
        p.drawPoint(QPointF(cx, cy - r * 0.28f));
        p.setPen(QPen(color, 1.5f));
        p.drawLine(QPointF(cx, cy - r * 0.05f), QPointF(cx, cy + r * 0.38f));
        break;
    }
    case notifications::NotificationType::UpdateAvailable:
    case notifications::NotificationType::SettingsRepaired: { // shares UpdateAvailable's glyph
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

// Compute the number of body lines needed (0, 1 or 2) for a given body text
// within the available text column width. An absent body occupies no space.
// text_w must be the same width used during paint (kToastWidth - text_x - kPadRight - kDismissSize - 6).
int bodyLineCount(const QString& body, const QFontMetrics& body_fm, int text_w) {
    if (body.isEmpty())
        return 0;
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

// One action button's resolved label, tone, and action tag.
struct ButtonSpec {
    QString label;
    bool primary = false;
    notifications::NotificationAction action = notifications::NotificationAction::None;
};

// Resolve the action-pill list for an event — the SINGLE source of truth shared
// by paintEvent (rendering) and computeHitTargets (hit-testing) so the two can
// never disagree on which pills exist or what they do.
QVector<ButtonSpec> buttonSpecsFor(const notifications::NotificationEvent& event) {
    using notifications::NotificationAction;
    QVector<ButtonSpec> buttons;
    switch (event.action) {
    case NotificationAction::OpenFolder:
        buttons.push_back({QStringLiteral("Open folder"), true, NotificationAction::OpenFolder});
        break;
    case NotificationAction::Edit:
        buttons.push_back({QStringLiteral("Edit"), true, NotificationAction::Edit});
        if (event.secondary_action == NotificationAction::OpenFolder)
            buttons.push_back({QStringLiteral("Show in folder"), false, NotificationAction::OpenFolder});
        break;
    case NotificationAction::OpenRecovery:
        buttons.push_back({QStringLiteral("Recover"), true, NotificationAction::OpenRecovery});
        if (event.secondary_action == NotificationAction::Discard)
            buttons.push_back({QStringLiteral("Discard"), false, NotificationAction::Discard});
        break;
    case NotificationAction::ChangeFolder:
        buttons.push_back({QStringLiteral("Change folder"), true, NotificationAction::ChangeFolder});
        if (event.secondary_action == NotificationAction::None || event.secondary_action == NotificationAction::Discard)
            buttons.push_back({QStringLiteral("Dismiss"), false, NotificationAction::None});
        break;
    case NotificationAction::ShowFile:
        buttons.push_back({QStringLiteral("Show file"), true, NotificationAction::ShowFile});
        break;
    case NotificationAction::OpenUpdate:
        buttons.push_back({QStringLiteral("View update"), true, NotificationAction::OpenUpdate});
        break;
    case NotificationAction::OpenDiagnostics:
        buttons.push_back({QStringLiteral("View diagnostics"), true, NotificationAction::OpenDiagnostics});
        break;
    case NotificationAction::RelaunchElevated:
        // ELEVATION-FOUNDATION-R1 (ADR 0033): "Restart as admin" unlocks the
        // elevation-gated diagnostics bundle. Optional "Not now" dismiss.
        buttons.push_back({QStringLiteral("Restart as admin"), true, NotificationAction::RelaunchElevated});
        if (event.secondary_action == NotificationAction::None)
            buttons.push_back({QStringLiteral("Not now"), false, NotificationAction::None});
        break;
    case NotificationAction::Discard:
        buttons.push_back({QStringLiteral("Discard"), false, NotificationAction::Discard});
        break;
    case NotificationAction::UndoPresetSwitch:
        // PresetSwitched is record-only (never a toast); nothing to render.
        break;
    case NotificationAction::None:
    default:
        break;
    }
    return buttons;
}

// Calculate the total height of a single toast card. The card grows to fit its
// content: no reserved space for an absent body, and a button strip only when
// there are two actions (a one-action card IS the action — no strip).
int toastHeight(const notifications::NotificationEvent& event, const QFontMetrics& body_fm) {
    const int body_lines = bodyLineCount(event.body, body_fm, kTextW);

    int content_h = kTitleH;
    if (body_lines > 0)
        content_h += kBodyGapTop + kBodyH * body_lines;
    if (buttonSpecsFor(event).size() >= 2)
        content_h += kActionsGapTop + kPillHTotal;

    // Card height = padTop + max(chip, content) + padBottom + optional bar
    int h = kPadTop + qMax(kChipSize, content_h) + kPadBottom;

    // Countdown bar at the bottom of timed toasts — the bar appears exactly
    // when the notification leaves on its own.
    if (!isStanding(event.type))
        h += kBarH;

    return h;
}

// Fully resolved window-space geometry for one stacked toast at `y_offset`.
// Both paint and hit-test derive every rect from this struct so they stay in lockstep.
struct ToastLayout {
    int y_offset = 0;
    int card_h = 0;
    bool standing = false;
    QRectF card_rect;    // rounded card body (FULL height incl. bar strip)
    QRectF dismiss_rect; // top-right ✕ hit/paint rect
    int text_x = 0;
    int title_y = 0; // window-space y of the title line (block centered to the chip)
    int body_lines = 0;
    int actions_y = 0;
    // One action: the card IS the action — no pills, a › marks it.
    bool card_is_action = false;
    notifications::NotificationAction card_action = notifications::NotificationAction::None;
    QRectF chevron_rect;          // › marker (paint only; the hit target is the card)
    QVector<ButtonSpec> buttons;  // resolved pills, only when there are >= 2
    QVector<QRectF> button_rects; // 1:1 with `buttons`
};

// Build the three toast fonts identically to paintEvent so any caller that needs
// only the metrics (e.g. hit-testing) measures with the exact same glyph widths.
struct ToastFonts {
    QFont title;
    QFont body;
    QFont action;
};

ToastFonts makeToastFonts() {
    ToastFonts f;
    f.title.setFamily(QStringLiteral("Hanken Grotesk"));
    f.title.setPixelSize(kTitlePx);
    f.title.setWeight(QFont::DemiBold);
    f.body.setFamily(QStringLiteral("Hanken Grotesk"));
    f.body.setPixelSize(kBodyPx);
    f.body.setWeight(QFont::Normal);
    f.action.setFamily(QStringLiteral("Hanken Grotesk"));
    f.action.setPixelSize(kActionPx);
    f.action.setWeight(QFont::DemiBold);
    return f;
}

ToastLayout layoutFor(const notifications::NotificationEvent& event, int y_offset, const QFontMetrics& title_fm,
                      const QFontMetrics& body_fm, const QFontMetrics& action_fm) {
    Q_UNUSED(title_fm);
    ToastLayout L;
    L.y_offset = y_offset;
    L.standing = isStanding(event.type);
    L.card_h = toastHeight(event, body_fm);

    // Card sits at x = kShadowMargin inside the window (the window is wider by
    // 2 * kShadowMargin to accommodate the soft shadow penumbra on all sides).
    // y_offset is already in window-space (callers start at kShadowMargin and
    // increment by card_h + kStackGap between cards — no extra margin between cards).
    L.card_rect = QRectF(kShadowMargin, y_offset, kToastWidth, L.card_h);

    // card-local offsets (unchanged from spec):
    const int card_text_x = kPadLeft + kChipSize + kChipTextGap; // = kTextX, card-relative
    // window-space text x:
    L.text_x = kShadowMargin + card_text_x;

    // Center the title+body block on the glyph chip; a title-only card centers
    // the single line, a wrapped body simply starts at the top pad.
    L.body_lines = bodyLineCount(event.body, body_fm, kTextW);
    const int text_block_h = kTitleH + (L.body_lines > 0 ? kBodyGapTop + kBodyH * L.body_lines : 0);
    const int text_y_top = y_offset + kPadTop;
    L.title_y = text_y_top + qMax(0, (kChipSize - text_block_h) / 2);

    L.actions_y = L.title_y + text_block_h + kActionsGapTop;

    // Dismiss ✕ — window-space rect (card right edge = kShadowMargin + kToastWidth)
    L.dismiss_rect =
        QRectF(kShadowMargin + kToastWidth - kPadRight - kDismissSize, y_offset + kPadTop, kDismissSize, kDismissSize);

    const QVector<ButtonSpec> specs = buttonSpecsFor(event);
    if (specs.size() >= 2) {
        // Two actions: named buttons in their own row.
        L.buttons = specs;
        int pill_x = L.text_x; // window-space pill start
        for (const auto& btn : L.buttons) {
            const int label_w = action_fm.horizontalAdvance(btn.label);
            const int pill_w = label_w + 24; // 12px each side
            L.button_rects.push_back(QRectF(pill_x, L.actions_y, pill_w, kPillH));
            pill_x += pill_w + kPillGapX;
        }
    } else if (specs.size() == 1) {
        // One action: the card is the action; a › marks it, left of the ✕
        // column so the two never collide on a compact card.
        L.card_is_action = true;
        L.card_action = specs[0].action;
        const int content_h = L.card_h - (L.standing ? 0 : kBarH);
        const QRectF x_col(L.dismiss_rect.left() - 6 - kDismissSize, y_offset, kDismissSize, content_h);
        L.chevron_rect = QRectF(x_col.left(), y_offset + (content_h - kDismissSize) / 2.0, kDismissSize, kDismissSize);
    }
    return L;
}

} // namespace

// ---------------------------------------------------------------------------
// NotificationToastWindow implementation
// ---------------------------------------------------------------------------

NotificationToastWindow::NotificationToastWindow(notifications::NotificationManager* manager, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint),
      manager_(manager) {
    // NOTE: Qt::WindowTransparentForInput is intentionally NOT set — the toast is
    // now interactive (✕ dismiss + action pills). Capture exclusion is provided
    // independently by SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) in
    // applyExclusion(); the two concerns are orthogonal. Click-through in the
    // transparent gaps between stacked cards is achieved via setMask() (updateMask).
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_AlwaysStackOnTop, true);
    setMouseTracking(true);

    // ~30 fps repaint so the auto-dismiss countdown bar animates. Only runs while
    // a non-sticky toast is visible (started in onVisibleSetChanged()).
    repaint_timer_ = new QTimer(this);
    repaint_timer_->setInterval(33);
    connect(repaint_timer_, &QTimer::timeout, this, [this]() { update(); });

    if (manager_) {
        connect(manager_, &notifications::NotificationManager::visibleSetChanged, this,
                &NotificationToastWindow::onVisibleSetChanged);
    }
}

void NotificationToastWindow::setAnchorWidget(QWidget* anchor) {
    anchor_widget_ = anchor;
    anchor_screen_connected_ = false;
    if (isVisible())
        updatePosition();
}

QScreen* NotificationToastWindow::anchorScreen() const {
    if (anchor_widget_ != nullptr) {
        if (QScreen* s = anchor_widget_->screen())
            return s;
    }
    return QGuiApplication::primaryScreen();
}

bool NotificationToastWindow::isExcluded() const noexcept {
    return excluded_;
}

QSize NotificationToastWindow::sizeHint() const {
    const auto* events_ptr = manager_ ? &manager_->VisibleEvents() : nullptr;
    const int n = events_ptr ? events_ptr->size() : 0;
    if (n == 0)
        return QSize(kToastWidth, 0);

    const QFontMetrics body_fm(makeToastFonts().body);

    int total_h = 0;
    for (int i = 0; i < n; ++i) {
        total_h += toastHeight((*events_ptr)[i], body_fm);
        if (i < n - 1)
            total_h += kStackGap;
    }
    // Add shadow margin on all sides so the soft penumbra is visible and masked.
    return QSize(kToastWidth + 2 * kShadowMargin, total_h + 2 * kShadowMargin);
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
    const QFontMetrics action_fm(action_font);

    // Pre-compute all hit targets once so paintEvent and hover-check share the
    // same index ordering (dismiss first, then pills, for each toast in order).
    const QVector<ToastHit> all_hits = computeHitTargets();
    int hit_idx = 0; // running cursor into all_hits during paint

    // y_offset starts at kShadowMargin so the card rect has room for the soft
    // shadow penumbra above and to the left of the first card.
    int y_offset = kShadowMargin;
    for (int idx = 0; idx < events.size(); ++idx) {
        const auto& event = events[idx];
        const StatusTokens tok = tokensForType(event.type);

        // Single source of truth for this toast's geometry (shared with hit-test).
        const ToastLayout L = layoutFor(event, y_offset, title_fm, body_fm, action_fm);
        const bool standing = L.standing;
        const int card_h = L.card_h;

        // Hit-slot bookkeeping mirrors computeHitTargets(): the dismiss ✕ first,
        // then either the pills or the card-wide action target.
        const int dismiss_hit = hit_idx;
        const int action_hit_base = hit_idx + 1;
        hit_idx += 1 + (L.buttons.isEmpty() ? (L.card_is_action ? 1 : 0) : L.buttons.size());

        // ── Soft drop shadow ─────────────────────────────────────────────
        // Approximates box-shadow: 0 18px 48px rgba(0,0,0,0.5) with 9 concentric
        // rounded-rects. Each layer is ~2.5 px larger outward with alpha that
        // decays from 0.09 (innermost) to 0.01 (outermost). Painted outside-in so
        // inner layers accumulate on top — giving a ~0.45 cumulative alpha at the
        // card edge and near-zero at the penumbra boundary ~20 px out.
        // The window is sized with kShadowMargin padding so none is clipped.
        {
            static const float kLayerAlpha[] = {0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f, 0.07f, 0.08f, 0.09f};
            constexpr int kNLayers = 9;
            constexpr float kMaxExpand = 20.0f; // total penumbra radius
            constexpr float kOffsetY = 10.0f;   // downward shadow bias
            const QRectF& card_rect_ref = L.card_rect;
            for (int s = 0; s < kNLayers; ++s) {
                // Outermost (s=0) → large + faint; innermost (s=8) → small + darker.
                const float t = float(kNLayers - 1 - s) / float(kNLayers - 1); // 1 at s=0, 0 at s=8
                const float expand = kMaxExpand * t;
                const float shift_y = kOffsetY * t;
                const QRectF sr = card_rect_ref.adjusted(-expand, -expand + shift_y, expand, expand + shift_y);
                QPainterPath sp;
                sp.addRoundedRect(sr, kToastRadius + expand, kToastRadius + expand);
                p.fillPath(sp, QColor(0, 0, 0, static_cast<int>(kLayerAlpha[s] * 255)));
            }
        }

        // ── Card background ───────────────────────────────────────────────
        const QRectF card_rect = L.card_rect;
        QPainterPath card_path;
        card_path.addRoundedRect(card_rect, kToastRadius, kToastRadius);

        p.fillPath(card_path, kCardBg);

        // One-action card: the card is the action — lighten it on hover.
        if (L.card_is_action && hovered_target_ == action_hit_base)
            p.fillPath(card_path, QColor(255, 255, 255, 10));

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
        const int chip_x = kShadowMargin + kPadLeft;
        const int content_area_h = card_h - (standing ? 0 : kBarH) - kPadTop - kPadBottom;
        const int chip_y = y_offset + kPadTop + (content_area_h - kChipSize) / 2;
        const QRectF chip_rect(chip_x, chip_y, kChipSize, kChipSize);

        QPainterPath chip_path;
        chip_path.addRoundedRect(chip_rect, kChipRadius, kChipRadius);
        p.fillPath(chip_path, tok.dim);
        QPen chip_border(tok.b);
        chip_border.setWidthF(1.0);
        p.strokePath(chip_path, chip_border);

        const int glyph_cx = chip_x + kChipSize / 2;
        const int glyph_cy = chip_y + kChipSize / 2;
        drawStatusGlyph(p, event.type, glyph_cx, glyph_cy, kChipGlyphSize, tok.c);

        // ── Text column ───────────────────────────────────────────────────
        const int text_x = L.text_x; // window-space
        // Text width is card-relative (card is always kToastWidth wide).
        const int text_w = kTextW;

        p.setFont(title_font);
        p.setPen(kInk);
        const QRect title_rect(text_x, L.title_y, text_w, kTitleH);
        p.drawText(title_rect, Qt::AlignVCenter | Qt::AlignLeft,
                   title_fm.elidedText(event.title, Qt::ElideRight, text_w));

        if (L.body_lines > 0) {
            p.setFont(body_font);
            p.setPen(kMut);
            const QRect body_rect(text_x, L.title_y + kTitleH + kBodyGapTop, text_w, kBodyH * L.body_lines);
            if (L.body_lines > 1) {
                p.drawText(body_rect, Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, event.body);
            } else {
                p.drawText(body_rect, Qt::AlignVCenter | Qt::AlignLeft,
                           body_fm.elidedText(event.body, Qt::ElideRight, text_w));
            }
        }

        // ── One-action marker › ────────────────────────────────────────────
        // The card is the action; the chevron says so without a button strip.
        if (L.card_is_action) {
            p.save();
            const QColor chevron_color = (hovered_target_ == action_hit_base) ? tok.c : kMut;
            p.setPen(QPen(chevron_color, 1.6f, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QRectF& r = L.chevron_rect;
            const float cx = static_cast<float>(r.center().x());
            const float cy = static_cast<float>(r.center().y());
            const float a = 3.5f;
            p.drawLine(QPointF(cx - a * 0.5f, cy - a), QPointF(cx + a * 0.5f, cy));
            p.drawLine(QPointF(cx + a * 0.5f, cy), QPointF(cx - a * 0.5f, cy + a));
            p.restore();
        }

        // ── Dismiss ✕ ─────────────────────────────────────────────────────
        // v10: dismiss uses kMut (more legible than kDim).
        // On hover: subtle circular background + brighter glyph.
        {
            const bool x_hovered = (hovered_target_ == dismiss_hit);
            if (x_hovered) {
                // Subtle hover circle behind the ✕
                const QRectF hover_circle = L.dismiss_rect.adjusted(-5, -5, 5, 5);
                QPainterPath cp;
                cp.addEllipse(hover_circle);
                p.fillPath(cp, QColor(255, 255, 255, 20));
                drawDismissX(p, L.dismiss_rect, kMut.lighter(150));
            } else {
                drawDismissX(p, L.dismiss_rect, kMut);
            }
        }

        // ── Action pills (two-action cards only; rects from the shared layout) ──
        if (!L.buttons.isEmpty()) {
            p.setFont(action_font);
            for (int b = 0; b < L.buttons.size(); ++b) {
                const ButtonSpec& btn = L.buttons[b];
                const QRectF pill_rect = L.button_rects[b];
                QPainterPath pill_path;
                pill_path.addRoundedRect(pill_rect, kPillRadius, kPillRadius);

                const bool pill_hovered = (hovered_target_ == action_hit_base + b);

                if (btn.primary) {
                    // v10: primary fill = tone color (not always mint), dark ink for contrast.
                    QColor fill = tok.c;
                    if (pill_hovered)
                        fill = fill.lighter(112); // subtle brighten on hover
                    p.fillPath(pill_path, fill);
                    p.setPen(kButtonInk);
                } else {
                    // Ghost: transparent fill, tone-color border.
                    // On hover: border brightens from dim tok.b → full tok.c.
                    p.fillPath(pill_path, Qt::transparent);
                    QPen ghost_border(pill_hovered ? tok.c : tok.b);
                    ghost_border.setWidthF(1.0);
                    p.strokePath(pill_path, ghost_border);
                    p.setPen(tok.c);
                }
                p.drawText(pill_rect.toRect(), Qt::AlignCenter, btn.label);
            }
        }

        // ── Auto-dismiss countdown bar (timed toasts only) ────────────────
        if (!standing) {
            p.save();
            p.setClipPath(card_path);

            const int bar_y = y_offset + card_h - kBarH;

            // Track background — card-width, starting at card left edge.
            const QRectF track_rect(kShadowMargin, bar_y, kToastWidth, kBarH);
            p.fillRect(track_rect, QColor(255, 255, 255, 15));

            double remaining = 1.0;
            const int interval = notifications::NotificationManager::DismissIntervalMs(event.type);
            if (interval > 0) {
                const qint64 shown_at = manager_->ShownAtMs(event.sequence);
                if (shown_at >= 0) {
                    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - shown_at;
                    remaining = std::clamp(1.0 - static_cast<double>(elapsed) / interval, 0.0, 1.0);
                }
            }

            const double fill_w = kToastWidth * remaining;
            if (fill_w > 0.0) {
                const QRectF fill_rect(kShadowMargin, bar_y, fill_w, kBarH);
                p.fillRect(fill_rect, tok.c);
            }
            p.restore();
        }

        y_offset += card_h + kStackGap;
    }
}

QVector<NotificationToastWindow::ToastHit> NotificationToastWindow::computeHitTargets() const {
    QVector<ToastHit> hits;
    if (!manager_)
        return hits;

    const auto& events = manager_->VisibleEvents();
    if (events.isEmpty())
        return hits;

    const ToastFonts fonts = makeToastFonts();
    const QFontMetrics title_fm(fonts.title);
    const QFontMetrics body_fm(fonts.body);
    const QFontMetrics action_fm(fonts.action);

    // Must start at kShadowMargin to stay in lockstep with paintEvent and updateMask.
    int y_offset = kShadowMargin;
    for (const auto& event : events) {
        const ToastLayout L = layoutFor(event, y_offset, title_fm, body_fm, action_fm);

        // Dismiss ✕ — slightly enlarge the hit rect for forgiving clicks.
        // Pushed first so it wins over a card-wide action target underneath.
        ToastHit x_hit;
        x_hit.rect = L.dismiss_rect.adjusted(-4, -4, 4, 4);
        x_hit.sequence = event.sequence;
        x_hit.is_dismiss = true;
        hits.push_back(x_hit);

        // Action pills (two-action cards).
        for (int b = 0; b < L.buttons.size(); ++b) {
            ToastHit pill_hit;
            pill_hit.rect = L.button_rects[b];
            pill_hit.sequence = event.sequence;
            pill_hit.is_dismiss = false;
            pill_hit.action = L.buttons[b].action;
            hits.push_back(pill_hit);
        }

        // One-action card: the whole card is the action.
        if (L.card_is_action) {
            ToastHit card_hit;
            card_hit.rect = L.card_rect;
            card_hit.sequence = event.sequence;
            card_hit.is_dismiss = false;
            card_hit.action = L.card_action;
            hits.push_back(card_hit);
        }

        y_offset += L.card_h + kStackGap;
    }
    return hits;
}

void NotificationToastWindow::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !manager_) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPointF pos = event->position();
    // Iterate the resolved targets; first hit wins (dismiss ✕ is pushed before
    // the pills for each toast, so it takes priority in its overlapping corner).
    for (const ToastHit& hit : computeHitTargets()) {
        if (!hit.rect.contains(pos))
            continue;

        if (hit.is_dismiss) {
            manager_->Dismiss(hit.sequence);
            event->accept();
            return;
        }

        // Action pill: emit the dispatch signal with a copy of the owning event,
        // then dismiss the toast. MainWindow runs the actual handler (Open folder /
        // Show file / Recover / …) — the toast never duplicates that logic.
        // A "Dismiss" ghost pill resolves to NotificationAction::None → just close.
        const auto& events = manager_->VisibleEvents();
        for (const auto& ev : events) {
            if (ev.sequence != hit.sequence)
                continue;
            if (hit.action != notifications::NotificationAction::None)
                emit actionTriggered(ev, hit.action);
            break;
        }
        manager_->Dismiss(hit.sequence);
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void NotificationToastWindow::mouseMoveEvent(QMouseEvent* event) {
    // Track which hit target is under the cursor and repaint when it changes so
    // hover feedback (pill lighten / × circle / secondary border brighten) is live.
    const QPointF pos = event->position();
    const auto hits = computeHitTargets();
    int new_hovered = -1;
    for (int i = 0; i < hits.size(); ++i) {
        if (hits[i].rect.contains(pos)) {
            new_hovered = i;
            break;
        }
    }
    if (new_hovered != hovered_target_) {
        hovered_target_ = new_hovered;
        update();
    }
    setCursor(new_hovered >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(event);
}

void NotificationToastWindow::leaveEvent(QEvent* event) {
    if (hovered_target_ != -1) {
        hovered_target_ = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

void NotificationToastWindow::updateMask() {
    // The window is one translucent top-level spanning the bounding box of the
    // whole toast stack. Mask it to the UNION of the card rectangles so clicks in
    // the transparent GAPS between stacked toasts fall through to the app behind —
    // only the actual cards capture input.
    if (!manager_) {
        clearMask();
        return;
    }
    const auto& events = manager_->VisibleEvents();
    if (events.isEmpty()) {
        clearMask();
        return;
    }

    const ToastFonts fonts = makeToastFonts();
    const QFontMetrics title_fm(fonts.title);
    const QFontMetrics body_fm(fonts.body);
    const QFontMetrics action_fm(fonts.action);

    QRegion region;
    // Start at kShadowMargin — same as paintEvent and computeHitTargets.
    int y_offset = kShadowMargin;
    for (const auto& event : events) {
        const ToastLayout L = layoutFor(event, y_offset, title_fm, body_fm, action_fm);
        // Expand the mask to cover the soft shadow penumbra so those semi-transparent
        // pixels are composited (not hidden). The shadow extends kShadowMargin to the
        // left/right, ~(kShadowMargin - 10) above, and ~(kShadowMargin + 18) below.
        region += L.card_rect.toAlignedRect().adjusted(-kShadowMargin, -(kShadowMargin - 10), kShadowMargin,
                                                       kShadowMargin + 18);
        y_offset += L.card_h + kStackGap;
    }
    setMask(region);
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

    // Anchor to the bottom-right of the screen that hosts the ExoSnap window —
    // notifications should appear where the user is working, not on whatever
    // display happens to be primary. Standing toasts stack above the single
    // timed toast, which sits closest to the corner.
    // Re-anchor live when the app window migrates to another screen. The
    // window handle exists only after the anchor is shown, so connect lazily.
    if (anchor_widget_ != nullptr && !anchor_screen_connected_ && anchor_widget_->window() != nullptr &&
        anchor_widget_->window()->windowHandle() != nullptr) {
        connect(anchor_widget_->window()->windowHandle(), &QWindow::screenChanged, this,
                [this](QScreen*) { updatePosition(); });
        anchor_screen_connected_ = true;
    }
    const QScreen* host = anchorScreen();

    QRect screen_rect;
    if (host)
        screen_rect = host->availableGeometry(); // respects taskbar

    if (screen_rect.isNull() || screen_rect.isEmpty()) {
        // Fallback: top-left origin
        move(kMarginRight, kMarginBottom);
        updateMask();
        return;
    }

    const int x = screen_rect.right() - hint.width() - kMarginRight;
    const int y = screen_rect.bottom() - hint.height() - kMarginBottom;
    move(x, y);

    // Keep the input mask in lockstep with the rendered card stack so clicks in
    // the gaps between toasts pass through to the app behind.
    updateMask();
}

void NotificationToastWindow::onVisibleSetChanged() {
    if (!excluded_ && exclusion_attempted_) {
        // Exclusion failed at startup; refuse to show.
        return;
    }

    const bool has_events = manager_ && !manager_->VisibleEvents().isEmpty();

    if (!has_events) {
        if (repaint_timer_)
            repaint_timer_->stop();
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

    // Animate the countdown bar only while a timed toast is visible; otherwise
    // stop the ~30 fps repaint to stay idle.
    bool has_countdown = false;
    for (const auto& event : manager_->VisibleEvents()) {
        if (!isStanding(event.type)) {
            has_countdown = true;
            break;
        }
    }
    if (repaint_timer_) {
        if (has_countdown && !repaint_timer_->isActive())
            repaint_timer_->start();
        else if (!has_countdown && repaint_timer_->isActive())
            repaint_timer_->stop();
    }
}

} // namespace exosnap::ui::overlay

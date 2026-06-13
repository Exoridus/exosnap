#include "DiagnosticsOverlayWindow.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QShowEvent>

#if defined(Q_OS_WIN)
#include <windows.h>
// WDA_EXCLUDEFROMCAPTURE was introduced in Windows 10 2004 (build 19041).
// The constant may not be defined in older SDKs.
#if !defined(WDA_EXCLUDEFROMCAPTURE)
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace exosnap::ui::overlay {

namespace {

// Design-system tokens — match RecordingOverlayWindow
constexpr QColor kBgColor{0x16, 0x16, 0x18, 230};       // ~90% opaque near-black
constexpr QColor kBorderColor{255, 255, 255, 28};       // subtle white hairline
constexpr QColor kTextPrimary{0xF1, 0xF1, 0xEF, 255};   // text-primary
constexpr QColor kTextSecondary{0xAA, 0xAA, 0xA8, 255}; // dimmed label
constexpr QColor kGlyphMuted{0xE0, 0x78, 0x6C, 255};    // coral — muted indicator
constexpr QColor kGlyphActive{0x4A, 0xC2, 0x8A, 255};   // green — active (not used for glyph dot)

constexpr int kPaddingH = 14; // horizontal inner padding
constexpr int kPaddingV = 8;  // vertical inner padding
constexpr int kRadius = 10;   // pill corner radius
constexpr int kRowGap = 4;    // gap between metric rows
constexpr int kGlyphSize = 7; // mute glyph dot diameter
constexpr int kGlyphGap = 5;  // gap between glyph dot and label

constexpr int kMarginRight = 20;  // distance from right edge of monitor
constexpr int kMarginBottom = 20; // distance from bottom edge of monitor

} // namespace

DiagnosticsOverlayWindow::DiagnosticsOverlayWindow(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                          Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_AlwaysStackOnTop, true);
}

void DiagnosticsOverlayWindow::showOverlay() {
    if (!exclusion_attempted_) {
        applyExclusion();
    }

    if (!excluded_) {
        // Exclusion failed — refuse to show; overlay must not contaminate recording.
        return;
    }

    updatePosition();
    update();
    show();
    raise();
}

void DiagnosticsOverlayWindow::hideOverlay() {
    hide();
}

void DiagnosticsOverlayWindow::updateMetrics(const QString& fps_bitrate_text, const QString& av_drift_text,
                                             const QString& dropped_frames_text, const QString& output_size_text,
                                             bool mic_muted, bool sys_muted) {
    fps_bitrate_text_ = fps_bitrate_text;
    av_drift_text_ = av_drift_text;
    dropped_frames_text_ = dropped_frames_text;
    output_size_text_ = output_size_text;
    mic_muted_ = mic_muted;
    sys_muted_ = sys_muted;

    if (isVisible())
        update();
}

void DiagnosticsOverlayWindow::setMonitorGeometry(const QRect& monitor_rect) {
    monitor_rect_ = monitor_rect;
    if (isVisible())
        updatePosition();
}

bool DiagnosticsOverlayWindow::isExcluded() const noexcept {
    return excluded_;
}

const QString& DiagnosticsOverlayWindow::fpsBitrateText() const noexcept {
    return fps_bitrate_text_;
}

const QString& DiagnosticsOverlayWindow::avDriftText() const noexcept {
    return av_drift_text_;
}

const QString& DiagnosticsOverlayWindow::droppedFramesText() const noexcept {
    return dropped_frames_text_;
}

const QString& DiagnosticsOverlayWindow::outputSizeText() const noexcept {
    return output_size_text_;
}

bool DiagnosticsOverlayWindow::isMicMuted() const noexcept {
    return mic_muted_;
}

bool DiagnosticsOverlayWindow::isSysMuted() const noexcept {
    return sys_muted_;
}

void DiagnosticsOverlayWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    // Reapply exclusion if the window handle was just created (first show).
    if (!exclusion_attempted_) {
        applyExclusion();
        if (!excluded_) {
            hide();
        }
    }
}

void DiagnosticsOverlayWindow::paintEvent(QPaintEvent* /*event*/) {
    const QRect rect = this->rect();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Background pill
    QPainterPath path;
    path.addRoundedRect(rect, kRadius, kRadius);
    p.fillPath(path, kBgColor);

    // Border hairline
    QPen border_pen(kBorderColor);
    border_pen.setWidthF(1.0);
    p.setPen(border_pen);
    p.drawPath(path);

    // Font: same family as RecordingOverlayWindow
    QFont font = p.font();
    font.setFamily(QStringLiteral("JetBrains Mono"));
    font.setPointSizeF(9.0);
    font.setWeight(QFont::Normal);
    p.setFont(font);

    const QFontMetrics fm(font);
    const int row_h = fm.height();
    const int content_width = rect.width() - kPaddingH * 2;

    int y = kPaddingV;

    // Helper: draw a label+value row
    auto draw_row = [&](const QString& label, const QString& value) {
        const QRect label_rect(kPaddingH, y, content_width, row_h);
        p.setPen(kTextSecondary);
        p.drawText(label_rect, Qt::AlignVCenter | Qt::AlignLeft, label);
        p.setPen(kTextPrimary);
        p.drawText(label_rect, Qt::AlignVCenter | Qt::AlignRight, value);
        y += row_h + kRowGap;
    };

    // Row 1: FPS / bitrate
    draw_row(QStringLiteral("FPS"), fps_bitrate_text_.isEmpty() ? QStringLiteral("—") : fps_bitrate_text_);
    // Row 2: A/V drift
    draw_row(QStringLiteral("DRIFT"), av_drift_text_.isEmpty() ? QStringLiteral("—") : av_drift_text_);
    // Row 3: dropped frames
    draw_row(QStringLiteral("DROPS"), dropped_frames_text_.isEmpty() ? QStringLiteral("0") : dropped_frames_text_);
    // Row 4: output size
    draw_row(QStringLiteral("SIZE"), output_size_text_.isEmpty() ? QStringLiteral("—") : output_size_text_);

    // Glyph row: muted-source indicators (only shown when muted)
    if (mic_muted_ || sys_muted_) {
        // Small separator gap before glyphs
        y += 2;
        p.setPen(Qt::NoPen);

        int glyph_x = kPaddingH;

        auto draw_glyph = [&](const QString& label) {
            p.setBrush(kGlyphMuted);
            p.drawEllipse(QPoint(glyph_x + kGlyphSize / 2, y + row_h / 2), kGlyphSize / 2, kGlyphSize / 2);
            glyph_x += kGlyphSize + kGlyphGap;

            p.setPen(kGlyphMuted);
            const QRect g_label_rect(glyph_x, y, content_width - glyph_x + kPaddingH, row_h);
            p.drawText(g_label_rect, Qt::AlignVCenter | Qt::AlignLeft, label);
            p.setPen(Qt::NoPen);

            const QFontMetrics gfm(font);
            glyph_x += gfm.horizontalAdvance(label) + kGlyphGap * 3;
        };

        if (mic_muted_)
            draw_glyph(QStringLiteral("MIC"));
        if (sys_muted_)
            draw_glyph(QStringLiteral("SYS"));
    }
}

QSize DiagnosticsOverlayWindow::minimumSizeHint() const {
    return sizeHint();
}

QSize DiagnosticsOverlayWindow::sizeHint() const {
    QFont font;
    font.setFamily(QStringLiteral("JetBrains Mono"));
    font.setPointSizeF(9.0);
    font.setWeight(QFont::Normal);

    const QFontMetrics fm(font);
    const int row_h = fm.height();

    // 4 metric rows + row gap between each
    const int num_rows = 4;
    int content_h = num_rows * row_h + (num_rows - 1) * kRowGap;

    // Glyph row: include its height when any source is muted so the pill grows
    // to accommodate the row instead of clipping the painted glyphs.
    if (mic_muted_ || sys_muted_) {
        content_h += 2 + row_h; // 2 px separator gap + one text row height
    }

    // Width: accommodate "DROPS  00000" comfortably
    const int label_w = fm.horizontalAdvance(QStringLiteral("DROPS"));
    const int value_w = fm.horizontalAdvance(QStringLiteral("99999 MB"));
    const int w = kPaddingH + label_w + 20 + value_w + kPaddingH;

    const int h = content_h + kPaddingV * 2;
    return QSize(w, h);
}

void DiagnosticsOverlayWindow::applyExclusion() {
    exclusion_attempted_ = true;
    excluded_ = false;

#if defined(Q_OS_WIN)
    // Ensure the window handle exists. On first call this may not be created yet —
    // winId() forces creation.
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd == nullptr)
        return;

    // WDA_EXCLUDEFROMCAPTURE (0x11) requires Windows 10 2004+.
    const BOOL ok = ::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    excluded_ = (ok != FALSE);
#else
    // Non-Windows: no capture exclusion supported; keep hidden.
    excluded_ = false;
#endif
}

void DiagnosticsOverlayWindow::updatePosition() {
    const QSize hint = sizeHint();
    resize(hint);

    QRect target_rect = monitor_rect_;
    if (target_rect.isNull() || target_rect.isEmpty()) {
        // Fall back to the primary screen.
        const QScreen* primary = QGuiApplication::primaryScreen();
        if (primary)
            target_rect = primary->geometry();
    }

    if (target_rect.isNull() || target_rect.isEmpty()) {
        // Last resort: use bottom-right with margin from (0,0).
        move(kMarginRight, kMarginBottom);
        return;
    }

    // Bottom-right corner — does not overlap RecordingOverlayWindow (top-right).
    const int x = target_rect.right() - hint.width() - kMarginRight;
    const int y = target_rect.bottom() - hint.height() - kMarginBottom;
    move(x, y);
}

} // namespace exosnap::ui::overlay

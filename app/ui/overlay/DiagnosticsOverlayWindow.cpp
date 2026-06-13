// DiagnosticsOverlayWindow.cpp — OVERLAY-SKIN-AND-PROOF-R1
// Re-skinned to match Mappe Wave 0.3 OverlayPill spec exactly:
//   Single horizontal row: fps 60.0 · drop 0 · drift +12ms · [micOff] 612 MB
//   Pill: rgba(12,12,14,0.74) bg, border rgba(255,255,255,0.16), radius 999,
//         padding 8px 14px, box-shadow (outer ring + drop), IBM Plex Mono 13px.
//   Labels in rgba(255,255,255,0.60), values in #fff, drop=0 success green #84CBA2.
//   Separator · in rgba(255,255,255,0.22). Muted-source glyph in rgba(255,255,255,0.85).

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

// ── Mappe OverlayPill tokens ─────────────────────────────────────────────────
// background: rgba(12,12,14,0.74)
constexpr QColor kPillBg{12, 12, 14, 189}; // 0.74 × 255 ≈ 189
// border: rgba(255,255,255,0.16)
constexpr QColor kBorderColor{255, 255, 255, 41}; // 0.16 × 255 ≈ 41
// outer ring: rgba(0,0,0,0.5)
constexpr QColor kOuterRing{0, 0, 0, 128};
// drop shadow: rgba(0,0,0,0.45)
constexpr QColor kShadow{0, 0, 0, 115}; // 0.45 × 255 ≈ 115

// ── Text tokens (Mappe OverlayReadouts / HT) ─────────────────────────────────
// label rgba(255,255,255,0.60): "fps", "drop", "drift"
constexpr QColor kLabel{255, 255, 255, 153}; // 0.60 × 255 ≈ 153
// value #fff
constexpr QColor kValue{255, 255, 255, 255};
// separator ·: rgba(255,255,255,0.22)
constexpr QColor kSeparator{255, 255, 255, 56}; // 0.22 × 255 ≈ 56
// drop=0 success green: #84CBA2
constexpr QColor kSuccess{0x84, 0xCB, 0xA2, 255};
// muted-source glyph: rgba(255,255,255,0.85)
constexpr QColor kGlyphMuted{255, 255, 255, 217}; // 0.85 × 255 ≈ 217

// ── Pill geometry (OverlayPill: padding '8px 14px', borderRadius 999) ────────
constexpr int kPaddingH = 14;
constexpr int kPaddingV = 8;
constexpr int kRadius = 999; // fully rounded pill

// ── Font: IBM Plex Mono, fontSize 12.5px (spec), pixel size 13 ───────────────
constexpr int kFontPx = 13;

// ── Gap between pill items (OverlayPill gap: 9) ───────────────────────────────
constexpr int kItemGap = 9;

// ── Positioning: top-left of recorded monitor, stacked below REC pill ─────────
// Mappe OverlayReadouts: both pills at top:14, left:14 with gap:8 between them.
constexpr int kMarginLeft = 14;
constexpr int kMarginTop = 14;
// Approximate REC pill height (8+16+8 px): we place diag below it.
constexpr int kRecPillApproxH = 32;
constexpr int kPillGap = 8; // vertical gap between REC and diag pills

} // namespace

// ---------------------------------------------------------------------------
// Helper: build the segment list for a row.
// Each segment is a { text, color } pair. The font for all segments is the
// mono font. The paintEvent and sizeHint share this builder for consistency.
// ---------------------------------------------------------------------------
struct DiagSegment {
    QString text;
    QColor color;
};

static QVector<DiagSegment> buildSegments(const QString& fps_text, const QString& drop_text, const QString& drift_text,
                                          const QString& size_text, bool mic_muted, bool sys_muted) {
    QVector<DiagSegment> segs;

    const QString fps_disp = fps_text.isEmpty() ? QStringLiteral("—") : fps_text;
    const QString drop_disp = drop_text.isEmpty() ? QStringLiteral("0") : drop_text;
    const bool drop_zero = (drop_disp == QStringLiteral("0"));
    const QString drift_disp = drift_text.isEmpty() ? QStringLiteral("—") : drift_text;
    const QString size_disp = size_text.isEmpty() ? QStringLiteral("—") : size_text;

    // fps <label> <value>
    segs.push_back({QStringLiteral("fps"), kLabel});
    segs.push_back({fps_disp, kValue});

    // · drop <label> <value>
    segs.push_back({QStringLiteral("\xB7"), kSeparator}); // U+00B7 middle dot
    segs.push_back({QStringLiteral("drop"), kLabel});
    segs.push_back({drop_disp, drop_zero ? kSuccess : kValue});

    // · drift <label> <value>
    segs.push_back({QStringLiteral("\xB7"), kSeparator});
    segs.push_back({QStringLiteral("drift"), kLabel});
    segs.push_back({drift_disp, kValue});

    // · [micOff glyph] [sysOff glyph] <size>
    segs.push_back({QStringLiteral("\xB7"), kSeparator});

    // Muted-source glyphs (compact text labels in muted white per Mappe)
    if (mic_muted)
        segs.push_back({QStringLiteral("mic\xE2\x83\x9E"), kGlyphMuted}); // mic + combining enclosing circle
    if (sys_muted)
        segs.push_back({QStringLiteral("sys\xE2\x83\x9E"), kGlyphMuted});

    // Size value
    segs.push_back({size_disp, kValue});

    return segs;
}

static int measureSegmentsWidth(const QVector<DiagSegment>& segs, const QFontMetrics& fm) {
    int w = kPaddingH * 2;
    for (int i = 0; i < segs.size(); ++i) {
        w += fm.horizontalAdvance(segs[i].text);
        if (i < segs.size() - 1)
            w += kItemGap;
    }
    return w;
}

// ---------------------------------------------------------------------------
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

    if (isVisible()) {
        // Content width may change with new metrics; recalculate.
        updatePosition();
        update();
    }
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
    const QRect r = this->rect();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // ── Shadow (box-shadow: 0 8px 26px rgba(0,0,0,0.45)) ─────────────────────
    {
        const qreal spread = 8.0;
        const QRectF sh = QRectF(r).adjusted(-spread, spread, spread, spread + spread);
        QPainterPath sh_path;
        sh_path.addRoundedRect(sh, kRadius + spread, kRadius + spread);
        p.fillPath(sh_path, kShadow);
    }

    // ── Outer ring (box-shadow: 0 0 0 1px rgba(0,0,0,0.5)) ──────────────────
    {
        QPainterPath ring_path;
        ring_path.addRoundedRect(QRectF(r).adjusted(-1, -1, 1, 1), kRadius + 1, kRadius + 1);
        p.fillPath(ring_path, kOuterRing);
    }

    // ── Pill background ───────────────────────────────────────────────────────
    QPainterPath pill_path;
    pill_path.addRoundedRect(QRectF(r), kRadius, kRadius);
    p.fillPath(pill_path, kPillBg);

    // ── Border 1px rgba(255,255,255,0.16) ────────────────────────────────────
    {
        QPen border_pen(kBorderColor);
        border_pen.setWidthF(1.0);
        p.setPen(border_pen);
        p.setBrush(Qt::NoBrush);
        QPainterPath inner_path;
        inner_path.addRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
        p.drawPath(inner_path);
    }

    // ── Content: horizontal segment row ──────────────────────────────────────
    QFont mono_font;
    mono_font.setFamily(QStringLiteral("IBM Plex Mono"));
    mono_font.setPixelSize(kFontPx);
    mono_font.setWeight(QFont::Normal);
    p.setFont(mono_font);

    const auto segs = buildSegments(fps_bitrate_text_, dropped_frames_text_, av_drift_text_, output_size_text_,
                                    mic_muted_, sys_muted_);
    const QFontMetrics fm(mono_font);

    int x = kPaddingH;
    for (int i = 0; i < segs.size(); ++i) {
        const auto& seg = segs[i];
        const int seg_w = fm.horizontalAdvance(seg.text);
        const QRect seg_rect(x, 0, seg_w, r.height());
        p.setPen(seg.color);
        p.drawText(seg_rect, Qt::AlignVCenter | Qt::AlignLeft, seg.text);
        x += seg_w;
        if (i < segs.size() - 1)
            x += kItemGap;
    }
}

QSize DiagnosticsOverlayWindow::minimumSizeHint() const {
    return sizeHint();
}

QSize DiagnosticsOverlayWindow::sizeHint() const {
    QFont mono_font;
    mono_font.setFamily(QStringLiteral("IBM Plex Mono"));
    mono_font.setPixelSize(kFontPx);
    mono_font.setWeight(QFont::Normal);
    const QFontMetrics fm(mono_font);

    const auto segs = buildSegments(fps_bitrate_text_, dropped_frames_text_, av_drift_text_, output_size_text_,
                                    mic_muted_, sys_muted_);
    const int w = measureSegmentsWidth(segs, fm);
    const int h = fm.height() + kPaddingV * 2;
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
        // Last resort: use margin from (0,0).
        move(kMarginLeft, kMarginTop + kRecPillApproxH + kPillGap);
        return;
    }

    // Top-left corner per Mappe OverlayReadouts: stacked below REC pill.
    const int x = target_rect.left() + kMarginLeft;
    const int y = target_rect.top() + kMarginTop + kRecPillApproxH + kPillGap;
    move(x, y);
}

} // namespace exosnap::ui::overlay

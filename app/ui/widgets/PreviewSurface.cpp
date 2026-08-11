#include "PreviewSurface.h"

#include "../../diagnostics/AppLog.h"
#include "../../services/DxgiPreviewRenderer.h"
#include "../theme/ExoSnapTheme.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QCursor>
#include <QEasingCurve>
#include <QFont>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QSvgRenderer>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <utility>

namespace exosnap::ui::widgets {
namespace {

constexpr double kMinOverlayNorm = 0.05;

double clampOrdered(double value, double low, double high) {
    if (low > high)
        std::swap(low, high);
    return std::clamp(value, low, high);
}

double finiteOrDefault(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
}

QRectF sanitizeOverlayRect(QRectF rect_norm) {
    double x = finiteOrDefault(rect_norm.x(), 0.0);
    double y = finiteOrDefault(rect_norm.y(), 0.0);
    double w = finiteOrDefault(rect_norm.width(), 0.25);
    double h = finiteOrDefault(rect_norm.height(), 0.25);

    w = clampOrdered(w, kMinOverlayNorm, 1.0);
    h = clampOrdered(h, kMinOverlayNorm, 1.0);
    x = clampOrdered(x, 0.0, 1.0 - kMinOverlayNorm);
    y = clampOrdered(y, 0.0, 1.0 - kMinOverlayNorm);

    if (x + w > 1.0)
        w = std::max(kMinOverlayNorm, 1.0 - x);
    if (y + h > 1.0)
        h = std::max(kMinOverlayNorm, 1.0 - y);
    if (x + w > 1.0)
        x = std::max(0.0, 1.0 - w);
    if (y + h > 1.0)
        y = std::max(0.0, 1.0 - h);

    return {x, y, w, h};
}

QRectF snapRectToAspectInside(QRectF rect_norm, double aspect_w_over_h) {
    rect_norm = sanitizeOverlayRect(rect_norm);
    if (aspect_w_over_h <= 0.0)
        return rect_norm;

    const double cx = rect_norm.center().x();
    const double cy = rect_norm.center().y();
    double w = rect_norm.width();
    double h = rect_norm.height();
    const double current_ar = h > 0.0 ? (w / h) : aspect_w_over_h;
    if (current_ar > aspect_w_over_h) {
        w = h * aspect_w_over_h;
    } else {
        h = w / aspect_w_over_h;
    }

    QRectF snapped(cx - (w * 0.5), cy - (h * 0.5), w, h);
    if (snapped.left() < 0.0)
        snapped.moveLeft(0.0);
    if (snapped.top() < 0.0)
        snapped.moveTop(0.0);
    if (snapped.right() > 1.0)
        snapped.moveRight(1.0);
    if (snapped.bottom() > 1.0)
        snapped.moveBottom(1.0);

    return sanitizeOverlayRect(snapped);
}

QRectF fitAspectIntoRect(const QRectF& bounds, double aspect_w_over_h) {
    if (bounds.width() <= 0.0 || bounds.height() <= 0.0 || aspect_w_over_h <= 0.0)
        return bounds;

    double w = bounds.width();
    double h = w / aspect_w_over_h;
    if (h > bounds.height()) {
        h = bounds.height();
        w = h * aspect_w_over_h;
    }

    const double x = bounds.left() + (bounds.width() - w) * 0.5;
    const double y = bounds.top() + (bounds.height() - h) * 0.5;
    return {x, y, w, h};
}

std::pair<double, double> fitAspectSize(double desired_w, double desired_h, double max_w, double max_h, double min_w,
                                        double min_h, double aspect_w_over_h) {
    max_w = std::max(max_w, min_w);
    max_h = std::max(max_h, min_h);
    desired_w = std::clamp(desired_w, min_w, max_w);
    desired_h = std::clamp(desired_h, min_h, max_h);

    double w_from_h = desired_h * aspect_w_over_h;
    double h_from_w = desired_w / aspect_w_over_h;
    double w = desired_w;
    double h = desired_h;
    if (std::abs(desired_w - w_from_h) < std::abs(desired_h - h_from_w)) {
        w = w_from_h;
    } else {
        h = h_from_w;
    }

    if (w > max_w) {
        w = max_w;
        h = w / aspect_w_over_h;
    }
    if (h > max_h) {
        h = max_h;
        w = h * aspect_w_over_h;
    }
    if (w < min_w) {
        w = min_w;
        h = w / aspect_w_over_h;
    }
    if (h < min_h) {
        h = min_h;
        w = h * aspect_w_over_h;
    }
    if (w > max_w) {
        w = max_w;
        h = w / aspect_w_over_h;
    }
    if (h > max_h) {
        h = max_h;
        w = h * aspect_w_over_h;
    }
    return {w, h};
}

} // namespace

PreviewSurface::PreviewSurface(QWidget* parent) : QWidget(parent) {
    setObjectName("previewSurface");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setFocusPolicy(Qt::StrongFocus);

    top_row_ = new QWidget(this);
    top_row_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* top_layout = new QHBoxLayout(top_row_);
    top_layout->setContentsMargins(0, 0, 0, 0);
    top_layout->setSpacing(8);

    // DESIGN-FIDELITY: the live recording status lives in the title bar + meta strip,
    // NOT over the preview (suite-record.jsx:198 "live status moved OFF the preview
    // surface"), so the preview carries no status pill.
    top_meta_label_ = new QLabel("NO TARGET", top_row_);
    top_meta_label_->setObjectName("previewTopMetaLabel");
    top_meta_label_->setProperty("labelRole", "previewMeta");

    top_layout->addWidget(top_meta_label_, 0, Qt::AlignVCenter);
    top_layout->addStretch(1);

    bottom_row_ = new QWidget(this);
    bottom_row_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* bottom_layout = new QHBoxLayout(bottom_row_);
    bottom_layout->setContentsMargins(0, 0, 0, 0);
    bottom_layout->setSpacing(8);

    bottom_left_label_ = new QLabel(QString(), bottom_row_);
    bottom_left_label_->setObjectName("previewBottomLeftLabel");
    bottom_left_label_->setProperty("labelRole", "previewBottomText");
    bottom_left_label_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    bottom_left_label_->setVisible(false);

    bottom_right_label_ = new QLabel("AV1 · CQ 24", bottom_row_);
    bottom_right_label_->setObjectName("previewBottomRightLabel");
    bottom_right_label_->setProperty("labelRole", "previewBottomAccent");
    bottom_right_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    bottom_layout->addWidget(bottom_left_label_, 1);
    bottom_layout->addWidget(bottom_right_label_, 0);

    top_meta_full_ = top_meta_label_->text();
    bottom_right_full_ = bottom_right_label_->text();

    // Live DXGI source dimensions change on the render thread (captured window
    // resized, display mode change) with no notification to the UI; poll them at a
    // low cadence while the DXGI preview runs so the content-fit box follows.
    aspect_poll_timer_ = new QTimer(this);
    aspect_poll_timer_->setInterval(500);
    connect(aspect_poll_timer_, &QTimer::timeout, this, [this]() { notifyAspectRatioMaybeChanged(); });
}

PreviewSurface::~PreviewSurface() {
    // Inline teardown instead of stopDxgiPreview(): that helper emits
    // contentAspectRatioChanged, and no signal may fire from a destructor.
    dxgi_active_ = false;
    if (dxgi_renderer_) {
        dxgi_renderer_->StopCapture();
        dxgi_renderer_->Shutdown();
        dxgi_renderer_.reset();
    }
}

bool PreviewSurface::hasHeightForWidth() const {
    // PREVIEW-PANEL: the surface fills the host (no 16:9 widget aspect lock); live
    // content letterboxes inside it. heightForWidth() is retained but unused.
    return false;
}

int PreviewSurface::heightForWidth(int width) const {
    return static_cast<int>(static_cast<double>(width) * 9.0 / 16.0);
}

QSize PreviewSurface::sizeHint() const {
    return {720, 405};
}

QSize PreviewSurface::minimumSizeHint() const {
    return {320, 200};
}

void PreviewSurface::setRecording(bool recording) {
    if (recording_ == recording)
        return;
    recording_ = recording;
    update();
}

void PreviewSurface::setLiveFrame(QImage frame) {
    current_frame_ = std::move(frame);
    notifyAspectRatioMaybeChanged();
    update();
}

bool PreviewSurface::isRecording() const noexcept {
    return recording_;
}

bool PreviewSurface::tryStartDxgiPreview(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num,
                                         uint32_t frame_rate_den, std::optional<exosnap::PreviewCropBox> crop_box) {
    stopDxgiPreview();

    setAttribute(Qt::WA_NativeWindow);
    winId();
    HWND hwnd = reinterpret_cast<HWND>(effectiveWinId());
    if (!hwnd) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("no native window handle for PreviewSurface"));
        return false;
    }

    SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_CLIPCHILDREN);

    dxgi_renderer_ = std::make_unique<exosnap::DxgiPreviewRenderer>();

    // Win32 CreateWindowEx expects physical pixels for DPI-aware apps; Qt width()/height() are logical.
    const qreal dpr = devicePixelRatioF();
    const uint32_t hwndW = static_cast<uint32_t>(std::max(1.0, width() * dpr));
    const uint32_t hwndH = static_cast<uint32_t>(std::max(1.0, height() * dpr));
    const uint32_t swapW = hwndW;
    const uint32_t swapH = hwndH;
    if (!dxgi_renderer_->Initialize(hwnd, hwndW, hwndH, swapW, swapH)) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("DxgiPreviewRenderer init failed, falling back to QImage"));
        dxgi_renderer_.reset();
        return false;
    }

    // Fires on the render thread; hop to the GUI thread bound to `this` so a
    // callback racing surface teardown is dropped by Qt instead of delivered.
    dxgi_renderer_->SetFirstFramePresentedCallback([this]() {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                // The source dimensions are known once the first frame rendered.
                notifyAspectRatioMaybeChanged();
                emit dxgiFirstFrameRendered();
            },
            Qt::QueuedConnection);
    });

    // Pass the crop box (if any) so the renderer crops the monitor frame to the
    // selected region.  For Display and Window targets this is std::nullopt.
    if (!dxgi_renderer_->StartCapture(target, frame_rate_num, frame_rate_den, std::move(crop_box))) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("DxgiPreviewRenderer StartCapture failed, falling back to QImage"));
        dxgi_renderer_->Shutdown();
        dxgi_renderer_.reset();
        return false;
    }

    applyDxgiPreviewResize();
    dxgi_active_ = true;
    current_frame_ = QImage{};
    // The native child HWND occludes Qt painting, so push the current overlay
    // (video + placement + chrome) to the renderer to composite it WYSIWYG.
    syncWebcamOverlayToDxgi();
    // The meta/stats rows are occluded too — push them as OSD sprites.
    syncOsdToDxgi();
    // Ensures the magnifier dim scrim starts definitely cleared on a fresh
    // preview (no-op unless a stale mid-animation state somehow survived).
    syncEnlargedWebcamToDxgi();
    aspect_poll_timer_->start();
    notifyAspectRatioMaybeChanged();
    update();
    return true;
}

bool PreviewSurface::tryStartDxgiPushedPreview(const recorder_core::CaptureTarget& target, uint32_t frame_rate_num,
                                               uint32_t frame_rate_den) {
    stopDxgiPreview();

    setAttribute(Qt::WA_NativeWindow);
    winId();
    HWND hwnd = reinterpret_cast<HWND>(effectiveWinId());
    if (!hwnd) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("no native window handle for PreviewSurface"));
        return false;
    }

    SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_CLIPCHILDREN);

    dxgi_renderer_ = std::make_unique<exosnap::DxgiPreviewRenderer>();

    const qreal dpr = devicePixelRatioF();
    const uint32_t hwndW = static_cast<uint32_t>(std::max(1.0, width() * dpr));
    const uint32_t hwndH = static_cast<uint32_t>(std::max(1.0, height() * dpr));
    if (!dxgi_renderer_->Initialize(hwnd, hwndW, hwndH, hwndW, hwndH)) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("DxgiPreviewRenderer init failed (pushed-only)"));
        dxgi_renderer_.reset();
        return false;
    }

    // Fires on the render thread; hop to the GUI thread bound to `this` so a
    // callback racing surface teardown is dropped by Qt instead of delivered.
    dxgi_renderer_->SetFirstFramePresentedCallback([this]() {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                // The source dimensions are known once the first frame rendered.
                notifyAspectRatioMaybeChanged();
                emit dxgiFirstFrameRendered();
            },
            Qt::QueuedConnection);
    });

    if (!dxgi_renderer_->StartPushedOnly(target, frame_rate_num, frame_rate_den)) {
        diagnostics::AppLog::warning(QStringLiteral("dxgi-preview"),
                                     QStringLiteral("DxgiPreviewRenderer StartPushedOnly failed"));
        dxgi_renderer_->Shutdown();
        dxgi_renderer_.reset();
        return false;
    }

    applyDxgiPreviewResize();
    dxgi_active_ = true;
    current_frame_ = QImage{};
    syncWebcamOverlayToDxgi();
    syncOsdToDxgi();
    // Ensures the magnifier dim scrim starts definitely cleared on a fresh
    // preview (no-op unless a stale mid-animation state somehow survived).
    syncEnlargedWebcamToDxgi();
    aspect_poll_timer_->start();
    notifyAspectRatioMaybeChanged();
    update();
    return true;
}

void PreviewSurface::stopDxgiPreview() {
    dxgi_active_ = false;
    if (aspect_poll_timer_)
        aspect_poll_timer_->stop();
    if (dxgi_renderer_) {
        dxgi_renderer_->StopCapture();
        dxgi_renderer_->Shutdown();
        dxgi_renderer_.reset();
    }
    notifyAspectRatioMaybeChanged();
}

bool PreviewSurface::isDxgiPreviewActive() const noexcept {
    return dxgi_active_ && dxgi_renderer_ && dxgi_renderer_->IsActive();
}

bool PreviewSurface::isDxgiSnapshotReady() const noexcept {
    return isDxgiPreviewActive() && dxgi_renderer_->HasPresentedFrame();
}

void PreviewSurface::resetDxgiPerformanceMetrics() noexcept {
    if (dxgi_renderer_)
        dxgi_renderer_->ResetPerformanceMetrics();
}

exosnap::DxgiPreviewPerformanceSnapshot PreviewSurface::dxgiPerformanceMetrics() const {
    return dxgi_renderer_ ? dxgi_renderer_->PerformanceMetrics() : exosnap::DxgiPreviewPerformanceSnapshot{};
}

void PreviewSurface::beginPushedSource(void* nt_handle, uint32_t width, uint32_t height,
                                       recorder_core::PreviewTapDesc tap, bool raw_source_frames) {
    if (dxgi_renderer_)
        dxgi_renderer_->BeginPushedSource(nt_handle, width, height, tap, raw_source_frames);
}

void PreviewSurface::endPushedSource() {
    if (dxgi_renderer_)
        dxgi_renderer_->EndPushedSource();
}

void PreviewSurface::requestDxgiSnapshot(
    std::function<void(bool, uint32_t, uint32_t, std::vector<uint8_t>, std::string)> callback) {
    if (!isDxgiPreviewActive()) {
        if (callback)
            callback(false, 0, 0, {}, "No live DXGI preview");
        return;
    }
    dxgi_renderer_->RequestSnapshot(std::move(callback));
}

void PreviewSurface::repositionDxgiPreview() {
    applyDxgiPreviewResize();
}

void PreviewSurface::applyDxgiPreviewResize() {
    if (!dxgi_renderer_ || !dxgi_active_)
        return;

    // Win32 SetWindowPos expects physical pixels for DPI-aware apps; Qt width()/height() are logical.
    const qreal dpr = devicePixelRatioF();
    const uint32_t hwndW = static_cast<uint32_t>(std::max(1.0, width() * dpr));
    const uint32_t hwndH = static_cast<uint32_t>(std::max(1.0, height() * dpr));
    const uint32_t swapW = hwndW;
    const uint32_t swapH = hwndH;
    dxgi_renderer_->Resize(hwndW, hwndH, swapW, swapH);
}

QRectF PreviewSurface::displayedFrameRectForSource(int srcW, int srcH) const {
    if (width() <= 0 || height() <= 0 || srcW <= 0 || srcH <= 0)
        return QRectF(0, 0, width(), height());

    const double sx = static_cast<double>(width()) / static_cast<double>(srcW);
    const double sy = static_cast<double>(height()) / static_cast<double>(srcH);
    const double s = std::min(sx, sy);
    const double dw = static_cast<double>(srcW) * s;
    const double dh = static_cast<double>(srcH) * s;
    const double dx = (static_cast<double>(width()) - dw) * 0.5;
    const double dy = (static_cast<double>(height()) - dh) * 0.5;
    return {dx, dy, dw, dh};
}

double PreviewSurface::contentAspectRatio() const {
    if (dxgi_active_ && dxgi_renderer_) {
        uint32_t sw = 0;
        uint32_t sh = 0;
        dxgi_renderer_->GetSourceSize(sw, sh);
        if (sw > 0 && sh > 0)
            return static_cast<double>(sw) / static_cast<double>(sh);
        return 0.0; // DXGI running but no frame yet — dimensions unknown.
    }
    if (!current_frame_.isNull() && current_frame_.width() > 0 && current_frame_.height() > 0)
        return static_cast<double>(current_frame_.width()) / static_cast<double>(current_frame_.height());
    return 0.0;
}

void PreviewSurface::notifyAspectRatioMaybeChanged() {
    const double ar = contentAspectRatio();
    if (std::abs(ar - last_notified_aspect_) < 1e-6)
        return;
    last_notified_aspect_ = ar;
    emit contentAspectRatioChanged(ar);
}

void PreviewSurface::setTopMetaText(const QString& text) {
    top_meta_full_ = text;
    applyOverlayTextElision();
}

void PreviewSurface::setPlaceholderHint(const QString& text) {
    if (placeholder_hint_ == text)
        return;
    placeholder_hint_ = text;
    update();
}

void PreviewSurface::setBottomLeftText(const QString& text) {
    bottom_left_full_ = text;
    applyOverlayTextElision();
}

void PreviewSurface::setBottomRightText(const QString& text) {
    bottom_right_full_ = text;
    applyOverlayTextElision();
}

// VR-009: the surface can shrink well below the overlay text width (compact
// windows, letterboxed completed previews). Texts elide instead of painting
// half-cut glyphs across the frame border, and the meta rows hide entirely
// when there is no usable room.
void PreviewSurface::applyOverlayTextElision() {
    constexpr int kPadX = 16;
    constexpr int kSpacing = 8;
    constexpr int kMinOverlayTextWidth = 220;

    const int row_width = width() - (kPadX * 2);
    const bool rows_usable = width() >= kMinOverlayTextWidth;

    const QString right =
        bottom_right_label_->fontMetrics().elidedText(bottom_right_full_, Qt::ElideRight, std::max(0, row_width));
    bottom_right_label_->setText(right);
    bottom_right_label_->setVisible(rows_usable && !right.isEmpty());

    const int left_avail =
        row_width - (right.isEmpty() ? 0 : bottom_right_label_->fontMetrics().horizontalAdvance(right) + kSpacing);
    const QString left =
        bottom_left_label_->fontMetrics().elidedText(bottom_left_full_, Qt::ElideRight, std::max(0, left_avail));
    bottom_left_label_->setText(left);
    bottom_left_label_->setVisible(rows_usable && !left.trimmed().isEmpty());

    const int meta_avail = row_width - kSpacing;
    const QString meta =
        top_meta_label_->fontMetrics().elidedText(top_meta_full_, Qt::ElideRight, std::max(0, meta_avail));
    top_meta_label_->setText(meta);
    top_meta_label_->setVisible(rows_usable && !meta.isEmpty());

    // Text or visibility changed — refresh the OSD sprites the live DXGI preview
    // composites in place of the (occluded) Qt-painted rows.
    syncOsdToDxgi();
}

void PreviewSurface::setFrameTone(FrameTone tone) {
    if (frame_tone_ == tone) {
        return;
    }
    frame_tone_ = tone;
    update();
}

void PreviewSurface::setCountdownState(bool active, int remaining_seconds, int duration_seconds) {
    if (countdown_active_ == active && countdown_remaining_ == remaining_seconds &&
        countdown_duration_ == duration_seconds) {
        return;
    }
    countdown_active_ = active;
    countdown_remaining_ = remaining_seconds;
    countdown_duration_ = duration_seconds > 0 ? duration_seconds : 1;
    update();
}

// ---------------------------------------------------------------------------
// Webcam overlay helpers
// ---------------------------------------------------------------------------

void PreviewSurface::setWebcamFrame(QImage frame) {
    // Normalize to ARGB32 (BGRA byte order on little-endian) so both the Qt paint
    // path and the DXGI overlay upload see a consistent pixel layout.
    if (!frame.isNull() && frame.format() != QImage::Format_ARGB32 && frame.format() != QImage::Format_RGB32) {
        frame = frame.convertToFormat(QImage::Format_ARGB32);
    }
    if (!frame.isNull() && frame.width() > 0 && frame.height() > 0) {
        const double new_ar = static_cast<double>(frame.width()) / static_cast<double>(frame.height());
        // The default PiP rect is a guess derived from the *requested* resolution, which
        // often differs from what the camera actually delivers (e.g. a 4:3 sensor crop
        // served when 16:9 was asked for). Once the real frame aspect is known, re-fit
        // the box to it so the feed is shown at its true shape instead of stretched.
        const bool aspect_changed = std::abs(new_ar - webcam_aspect_ratio_) > 0.001;
        webcam_aspect_ratio_ = new_ar;
        if (aspect_changed && aspect_ratio_locked_)
            snapOverlayRectToCurrentAspect();
    }
    webcam_frame_ = std::move(frame);
    syncWebcamOverlayToDxgi();
    if (webcam_enabled_)
        update();
}

void PreviewSurface::setWebcamOverlayEnabled(bool enabled) {
    webcam_enabled_ = enabled;
    if (!enabled) {
        if (drag_mode_ != DragMode::None) {
            drag_mode_ = DragMode::None;
            if (QWidget::keyboardGrabber() == this) {
                releaseKeyboard();
            }
        }
        drag_modifier_toggle_held_ = false;
        if (webcam_selected_) {
            webcam_selected_ = false;
            emit webcamSelectionChanged(false);
        }
        // Hard reset (no animation): the PiP itself is gone, so a floating view of
        // it makes no sense to keep animating down.
        webcam_hovered_ = false;
        webcam_enlarged_ = false;
        magnify_progress_ = 0.0;
        if (magnify_animation_ != nullptr)
            magnify_animation_->stop();
        syncEnlargedWebcamToDxgi(); // clears the DXGI-side dim scrim too
    }
    // Mouse tracking drives BOTH the drag/resize hover cursor (editing-gated) and the
    // magnifier hover affordance (available whenever the PiP is visible at all, even
    // while edit-locked -- peeking at a bigger view is not an editing action).
    setMouseTracking(webcam_enabled_);
    syncWebcamOverlayToDxgi();
    applyHoverCursor();
    update();
}

void PreviewSurface::setWebcamOverlayRect(QRectF rect_norm) {
    webcam_rect_norm_ = sanitizeOverlayRect(rect_norm);
    syncWebcamOverlayToDxgi();
    applyHoverCursor();
    update();
}

void PreviewSurface::setAspectRatioLocked(bool locked) {
    if (aspect_ratio_locked_ == locked)
        return;
    aspect_ratio_locked_ = locked;
    if (aspect_ratio_locked_)
        snapOverlayRectToCurrentAspect();
    syncWebcamOverlayToDxgi();
    update();
}

void PreviewSurface::setWebcamMirror(bool mirror) {
    if (webcam_mirror_ == mirror)
        return;
    webcam_mirror_ = mirror;
    syncWebcamOverlayToDxgi();
    if (webcam_enabled_)
        update();
}

void PreviewSurface::setWebcamChromaKey(const WebcamChromaKeySettings& chroma) {
    if (webcam_chroma_ == chroma)
        return;
    webcam_chroma_ = chroma;
    syncWebcamOverlayToDxgi();
}

void PreviewSurface::setWebcamOpacity(float opacity) {
    const float clamped = std::isfinite(static_cast<double>(opacity)) ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
    if (qFuzzyCompare(webcam_opacity_, clamped))
        return;
    webcam_opacity_ = clamped;
    syncWebcamOverlayToDxgi();
    if (webcam_enabled_)
        update();
}

bool PreviewSurface::webcamEditingAllowed() const noexcept {
    return webcam_enabled_ && !webcam_edit_locked_;
}

void PreviewSurface::setWebcamEditLocked(bool locked) {
    if (webcam_edit_locked_ == locked)
        return;
    webcam_edit_locked_ = locked;
    if (locked) {
        // Locking ends any interaction and drops the selection so no edit chrome
        // lingers; the PiP video itself stays visible.
        if (drag_mode_ != DragMode::None) {
            drag_mode_ = DragMode::None;
            if (QWidget::keyboardGrabber() == this) {
                releaseKeyboard();
            }
        }
        drag_modifier_toggle_held_ = false;
        if (webcam_selected_) {
            webcam_selected_ = false;
            emit webcamSelectionChanged(false);
        }
    }
    // See setWebcamOverlayEnabled(): mouse tracking must stay on whenever the PiP is
    // visible so the magnifier hover affordance still works while edit-locked.
    setMouseTracking(webcam_enabled_);
    syncWebcamOverlayToDxgi();
    applyHoverCursor();
    update();
}

void PreviewSurface::setWebcamSelected(bool selected) {
    // Selection only takes effect while editing is allowed.
    const bool effective = selected && webcamEditingAllowed();
    if (webcam_selected_ != effective) {
        webcam_selected_ = effective;
        emit webcamSelectionChanged(webcam_selected_);
    }
    syncWebcamOverlayToDxgi();
    // Selection toggles the corner handles' hit zones — refresh the cursor for a
    // pointer already resting on one.
    applyHoverCursor();
    update();
}

void PreviewSurface::cancelWebcamInteraction() {
    if (drag_mode_ != DragMode::None) {
        drag_mode_ = DragMode::None;
        drag_modifier_toggle_held_ = false;
        if (QWidget::keyboardGrabber() == this) {
            releaseKeyboard();
        }
    }
    // Hard reset (no animation): called on page/target changes, where an instant
    // snap-back is correct -- there is no "current placement" left to animate from.
    webcam_hovered_ = false;
    if (webcam_enlarged_ || magnify_progress_ != 0.0) {
        webcam_enlarged_ = false;
        magnify_progress_ = 0.0;
        if (magnify_animation_ != nullptr)
            magnify_animation_->stop();
        syncEnlargedWebcamToDxgi(); // clears the DXGI-side dim scrim too
    }
    syncWebcamOverlayToDxgi();
    applyHoverCursor();
    update();
}

QRect PreviewSurface::webcamMappedPreviewRect() const {
    return webcamPixelRect().toRect();
}

QString PreviewSurface::webcamActiveHandle() const {
    switch (drag_mode_) {
    case DragMode::Move:
        return QStringLiteral("move");
    case DragMode::ResizeTL:
        return QStringLiteral("tl");
    case DragMode::ResizeTR:
        return QStringLiteral("tr");
    case DragMode::ResizeBL:
        return QStringLiteral("bl");
    case DragMode::ResizeBR:
        return QStringLiteral("br");
    case DragMode::None:
    default:
        return QStringLiteral("none");
    }
}

void PreviewSurface::syncWebcamOverlayToDxgi() {
    if (!dxgi_active_ || !dxgi_renderer_)
        return;
    const bool show = webcam_enabled_;
    const bool selected = webcam_selected_ && webcamEditingAllowed();

    const WebcamChromaKeySettings::ActiveRgb key = webcam_chroma_.active_color();
    recorder_core::ChromaKeyParams chroma;
    chroma.enabled = webcam_chroma_.enabled;
    chroma.r = key.r;
    chroma.g = key.g;
    chroma.b = key.b;
    chroma.tolerance = webcam_chroma_.tolerance;
    chroma.softness = webcam_chroma_.softness;
    chroma.spill_reduction = webcam_chroma_.spill_reduction;

    // True WYSIWYG: the renderer composites the SAME normalised placement the
    // recording compositor consumes — never trimmed, squished or clipped. The
    // meta/stats rows are OSD sprites drawn above the PiP (syncOsdToDxgi), so no
    // preview-only geometry adjustment is needed to keep them readable.
    dxgi_renderer_->SetWebcamOverlayState(
        show, selected, static_cast<float>(webcam_rect_norm_.x()), static_cast<float>(webcam_rect_norm_.y()),
        static_cast<float>(webcam_rect_norm_.width()), static_cast<float>(webcam_rect_norm_.height()), webcam_mirror_,
        webcam_opacity_, chroma);
    if (show && !webcam_frame_.isNull()) {
        const QImage& img = webcam_frame_;
        dxgi_renderer_->SetWebcamOverlayFrame(img.constBits(), img.width(), img.height(),
                                              static_cast<int>(img.bytesPerLine()));
    } else {
        dxgi_renderer_->SetWebcamOverlayFrame(nullptr, 0, 0, 0);
    }
}

// Drives the DXGI-composited counterpart of paintEvent's "floating enlarged
// view" block: same dim scrim, same lerped rect. Never writes
// webcam_rect_norm_ -- only what's pushed to the renderer for THIS preview
// frame changes.
void PreviewSurface::syncEnlargedWebcamToDxgi() {
    if (!dxgi_active_ || !dxgi_renderer_)
        return;

    if (magnify_progress_ <= 0.0) {
        dxgi_renderer_->SetWebcamDimScrim(nullptr, 0, 0, 0, 0, 0);
        syncWebcamOverlayToDxgi(); // restore the normal (confirmed) placement
        return;
    }

    const QRectF frame_rect = displayedFrameRect();
    if (frame_rect.width() < 1.0 || frame_rect.height() < 1.0)
        return;

    // Scrim: a solid, panel-sized, alpha-animated rect, rasterized the same way
    // syncOsdToDxgi() rasterizes the meta/stats rows, but pushed through the
    // dedicated dim-scrim channel -- which composites BELOW the webcam PiP (not
    // above everything like OSD sprites), so it dims the background without
    // darkening the enlarged PiP, matching paintEvent's scrim-then-PiP z-order.
    const qreal dpr = devicePixelRatioF();
    const QSize scrim_size = (frame_rect.size() * dpr).toSize().expandedTo(QSize(1, 1));
    QImage scrim(scrim_size, QImage::Format_ARGB32);
    scrim.fill(QColor(6, 6, 8, qRound(150 * magnify_progress_)));
    dxgi_renderer_->SetWebcamDimScrim(scrim.constBits(), scrim.width(), scrim.height(),
                                      static_cast<int>(scrim.bytesPerLine()), qRound(frame_rect.x() * dpr),
                                      qRound(frame_rect.y() * dpr));

    // Interpolated placement, normalized the same way webcam_rect_norm_ is
    // (fraction of frame_rect), matching paintEvent's lerp exactly.
    const QRectF normal = webcamPixelRect();
    const QRectF target = webcamEnlargedTargetRect();
    const QRectF draw_rect(normal.x() + (target.x() - normal.x()) * magnify_progress_,
                           normal.y() + (target.y() - normal.y()) * magnify_progress_,
                           normal.width() + (target.width() - normal.width()) * magnify_progress_,
                           normal.height() + (target.height() - normal.height()) * magnify_progress_);

    // The enlarged view always shows the camera's true aspect ratio, even if the
    // normal (non-enlarged) PiP is itself stretched off-aspect (aspect lock can be
    // disabled for that placement) -- matching paintEvent's enlarged branch, which
    // unconditionally fitAspectIntoRect()s the image into draw_rect rather than
    // stretching it. Unlike RenderWebcamOverlay(), which always stretches its frame
    // to fill whatever rect it's given, so the fit must happen here before the rect
    // is normalized and handed to it.
    const double frame_ar =
        webcam_frame_.height() > 0 ? static_cast<double>(webcam_frame_.width()) / webcam_frame_.height() : 0.0;
    const QRectF img_rect = fitAspectIntoRect(draw_rect, frame_ar);

    const float nx = static_cast<float>((img_rect.x() - frame_rect.x()) / frame_rect.width());
    const float ny = static_cast<float>((img_rect.y() - frame_rect.y()) / frame_rect.height());
    const float nw = static_cast<float>(img_rect.width() / frame_rect.width());
    const float nh = static_cast<float>(img_rect.height() / frame_rect.height());

    const WebcamChromaKeySettings::ActiveRgb key = webcam_chroma_.active_color();
    recorder_core::ChromaKeyParams chroma;
    chroma.enabled = webcam_chroma_.enabled;
    chroma.r = key.r;
    chroma.g = key.g;
    chroma.b = key.b;
    chroma.tolerance = webcam_chroma_.tolerance;
    chroma.softness = webcam_chroma_.softness;
    chroma.spill_reduction = webcam_chroma_.spill_reduction;
    dxgi_renderer_->SetWebcamOverlayState(/*show=*/true, /*selected=*/false, nx, ny, nw, nh, webcam_mirror_,
                                          webcam_opacity_, chroma);
    if (!webcam_frame_.isNull()) {
        const QImage& img = webcam_frame_;
        dxgi_renderer_->SetWebcamOverlayFrame(img.constBits(), img.width(), img.height(),
                                              static_cast<int>(img.bytesPerLine()));
    } else {
        dxgi_renderer_->SetWebcamOverlayFrame(nullptr, 0, 0, 0);
    }
}

QRectF PreviewSurface::defaultWebcamOverlayRect(double camera_aspect_w_over_h) const {
    const QRectF frame_rect = displayedFrameRect();
    if (frame_rect.width() < 1.0 || frame_rect.height() < 1.0) {
        return sanitizeOverlayRect(QRectF(0.75, 0.75, 0.25, 0.25));
    }

    const double W = frame_rect.width();
    const double H = frame_rect.height();
    const double requested_ar = camera_aspect_w_over_h > 0.0 ? camera_aspect_w_over_h : webcam_aspect_ratio_;
    const double cam_ar = requested_ar > 0.0 ? requested_ar : (16.0 / 9.0);
    constexpr double kMaxSide = 0.25;

    double w_px = 0.0;
    double h_px = 0.0;
    // Startup rule: landscape webcams are width-limited to 25%, square/portrait are height-limited to 25%.
    if (cam_ar > 1.0) {
        w_px = kMaxSide * W;
        h_px = w_px / cam_ar;
    } else {
        h_px = kMaxSide * H;
        w_px = h_px * cam_ar;
    }

    w_px = std::clamp(w_px, kMinOverlayNorm * W, W);
    h_px = std::clamp(h_px, kMinOverlayNorm * H, H);
    const double w_norm = std::clamp(w_px / W, kMinOverlayNorm, 1.0);
    const double h_norm = std::clamp(h_px / H, kMinOverlayNorm, 1.0);
    // Leave a comfortable margin from the content edges instead of sitting flush in
    // the corner. 12 logical px, expressed in this frame's normalised space so
    // it scales with the content rect. The inset lives ONLY in the DEFAULT rect; the
    // recording compositor consumes the same normalised placement, so the file gets
    // the identical inset placement (WYSIWYG) — only the flush-corner start moves.
    const double inset_x = 12.0 / W;
    const double inset_y = 12.0 / H;
    const double x_norm = std::max(0.0, 1.0 - w_norm - inset_x);
    const double y_norm = std::max(0.0, 1.0 - h_norm - inset_y);
    return sanitizeOverlayRect(QRectF(x_norm, y_norm, w_norm, h_norm));
}

// Rasterize one meta row into a transparent image and hand it to the renderer.
// slot 0 = top meta row, slot 1 = bottom stats row. Rows whose labels are all
// hidden (elision below the minimum width) clear their sprite.
void PreviewSurface::syncOsdToDxgi() {
    if (!dxgi_active_ || !dxgi_renderer_)
        return;

    const qreal dpr = devicePixelRatioF();
    const auto push_row = [&](int slot, QWidget* row, bool has_visible_text) {
        if (row == nullptr || !has_visible_text || row->width() <= 0 || row->height() <= 0) {
            dxgi_renderer_->SetOsdSprite(slot, nullptr, 0, 0, 0, 0, 0);
            return;
        }
        QImage img(row->size() * dpr, QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::transparent);
        row->render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
        // The renderer blends with straight source alpha (same state as the PiP),
        // so convert out of premultiplied before upload.
        img = img.convertToFormat(QImage::Format_ARGB32);
        dxgi_renderer_->SetOsdSprite(slot, img.constBits(), img.width(), img.height(),
                                     static_cast<int>(img.bytesPerLine()), qRound(row->x() * dpr),
                                     qRound(row->y() * dpr));
    };

    const bool top_visible = top_meta_label_ != nullptr && top_meta_label_->isVisibleTo(top_row_);
    const bool bottom_visible = (bottom_left_label_ != nullptr && bottom_left_label_->isVisibleTo(bottom_row_)) ||
                                (bottom_right_label_ != nullptr && bottom_right_label_->isVisibleTo(bottom_row_));
    push_row(0, top_row_, top_visible);
    push_row(1, bottom_row_, bottom_visible);
}

// Returns the webcam overlay rect in widget pixel coordinates.
QRectF PreviewSurface::webcamPixelRect() const {
    const QRectF fr = displayedFrameRect();
    if (fr.width() <= 0.0 || fr.height() <= 0.0)
        return {};

    return QRectF(fr.x() + webcam_rect_norm_.x() * fr.width(), fr.y() + webcam_rect_norm_.y() * fr.height(),
                  webcam_rect_norm_.width() * fr.width(), webcam_rect_norm_.height() * fr.height());
}

QRectF PreviewSurface::displayedFrameRect() const {
    if (width() <= 0 || height() <= 0)
        return {};

    // While the DXGI live preview runs, the captured source dimensions define the
    // content rectangle: the renderer contain-fits the same way, so PiP hit-testing
    // and the rendered overlay agree (no offset into letterbox margins).
    if (dxgi_active_ && dxgi_renderer_) {
        uint32_t sw = 0;
        uint32_t sh = 0;
        dxgi_renderer_->GetSourceSize(sw, sh);
        if (sw > 0 && sh > 0)
            return displayedFrameRectForSource(static_cast<int>(sw), static_cast<int>(sh));
    }

    if (current_frame_.isNull() || current_frame_.width() <= 0 || current_frame_.height() <= 0)
        return rect();

    return displayedFrameRectForSource(current_frame_.width(), current_frame_.height());
}

// Root-cause fix for the intermittently missing resize cursor: the cursor used to
// be recomputed ONLY in mouseMoveEvent, so any state change that alters the hit
// map under a stationary pointer (selection gained/lost — corner handles only hit
// while selected —, lock toggles, programmatic placement updates, a drag ending)
// left a stale cursor until the pointer left and re-entered the surface. Every
// such transition now re-evaluates the cursor from the last known pointer
// position.
void PreviewSurface::applyHoverCursor() {
    if (drag_mode_ != DragMode::None)
        return; // the in-flight interaction keeps the cursor it started with
    // The magnifier hotspot is clickable regardless of the edit-lock gate below --
    // peeking at a bigger view still works while drag/resize editing is locked
    // (e.g. during a live recording) -- so it takes priority over the resize/move
    // cursors hitTestWebcam() would otherwise pick.
    if (hover_pos_valid_) {
        const QRect icon = webcamMagnifierIconMappedRect();
        if (!icon.isEmpty() && icon.contains(hover_pos_.toPoint())) {
            setCursor(Qt::PointingHandCursor);
            return;
        }
    }
    if (!webcamEditingAllowed() || !hover_pos_valid_) {
        unsetCursor();
        return;
    }
    switch (hitTestWebcam(hover_pos_)) {
    case DragMode::ResizeTL:
    case DragMode::ResizeBR:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case DragMode::ResizeTR:
    case DragMode::ResizeBL:
        setCursor(Qt::SizeBDiagCursor);
        break;
    case DragMode::Move:
        setCursor(Qt::SizeAllCursor);
        break;
    default:
        unsetCursor();
        break;
    }
}

PreviewSurface::DragMode PreviewSurface::hitTestWebcam(QPointF pos) const {
    const QRectF r = webcamPixelRect();
    // Corner resize handles are only active once the PiP is selected; an unselected
    // PiP responds to a click anywhere inside it by selecting + moving.
    if (webcam_selected_) {
        constexpr double kHandle = 10.0;
        const bool nearL = std::abs(pos.x() - r.left()) < kHandle;
        const bool nearR = std::abs(pos.x() - r.right()) < kHandle;
        const bool nearT = std::abs(pos.y() - r.top()) < kHandle;
        const bool nearB = std::abs(pos.y() - r.bottom()) < kHandle;
        if (nearT && nearL)
            return DragMode::ResizeTL;
        if (nearT && nearR)
            return DragMode::ResizeTR;
        if (nearB && nearL)
            return DragMode::ResizeBL;
        if (nearB && nearR)
            return DragMode::ResizeBR;
    }
    if (r.contains(pos))
        return DragMode::Move;
    return DragMode::None;
}

// ---------------------------------------------------------------------------
// Webcam magnifier (hover-to-enlarge)
// ---------------------------------------------------------------------------

QRectF PreviewSurface::webcamEnlargedTargetRect() const {
    const QRectF bounds = QRectF(rect()).adjusted(2.0, 2.0, -2.0, -2.0); // same frame inset as previewContentRect()
    if (bounds.width() < 1.0 || bounds.height() < 1.0)
        return {};

    constexpr double kFillFraction = 0.72; // occupies ~72% of the shorter preview dimension
    constexpr double kMargin = 16.0;
    const double aspect = webcam_frame_.height() > 0
                              ? static_cast<double>(webcam_frame_.width()) / webcam_frame_.height()
                              : (webcam_aspect_ratio_ > 0.0 ? webcam_aspect_ratio_ : (16.0 / 9.0));

    double w = std::min(bounds.width(), bounds.height()) * kFillFraction;
    double h = w / aspect;
    const double max_w = std::max(1.0, bounds.width() - kMargin * 2.0);
    const double max_h = std::max(1.0, bounds.height() - kMargin * 2.0);
    if (w > max_w) {
        w = max_w;
        h = w / aspect;
    }
    if (h > max_h) {
        h = max_h;
        w = h * aspect;
    }

    const double x = bounds.center().x() - w / 2.0;
    const double y = bounds.center().y() - h / 2.0;
    return {x, y, w, h};
}

QRectF PreviewSurface::magnifierIconRect(const QRectF& pip_rect) const {
    constexpr double kDiameter = 22.0;
    constexpr double kMargin = 6.0;
    // A badge this size would dominate a very small PiP -- skip it there rather
    // than shown-and-clipped.
    if (pip_rect.width() < kDiameter * 1.6 || pip_rect.height() < kDiameter * 1.6)
        return {};
    return QRectF(pip_rect.right() - kMargin - kDiameter, pip_rect.top() + kMargin, kDiameter, kDiameter);
}

void PreviewSurface::ensureMagnifyAnimation() {
    if (magnify_animation_ != nullptr)
        return;
    magnify_animation_ = new QVariantAnimation(this);
    magnify_animation_->setDuration(180); // 150-200ms range, matching the task's animated-transition ask
    magnify_animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(magnify_animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        magnify_progress_ = value.toDouble();
        update();
        syncEnlargedWebcamToDxgi();
    });
}

void PreviewSurface::setWebcamEnlarged(bool enlarged) {
    if (webcam_enlarged_ == enlarged)
        return;
    ensureMagnifyAnimation();
    if (enlarged && drag_mode_ != DragMode::None) {
        // Entering the floating view ends any in-progress drag so nothing stale
        // survives underneath it. Inlined rather than calling
        // cancelWebcamInteraction(), which also hard-resets magnify state -- not
        // what we want while starting the very animation it would clobber.
        drag_mode_ = DragMode::None;
        drag_modifier_toggle_held_ = false;
        if (QWidget::keyboardGrabber() == this)
            releaseKeyboard();
    }
    webcam_enlarged_ = enlarged;
    magnify_animation_->stop();
    magnify_animation_->setStartValue(magnify_progress_);
    magnify_animation_->setEndValue(enlarged ? 1.0 : 0.0);
    magnify_animation_->start();
}

QRect PreviewSurface::webcamMagnifierIconMappedRect() const {
    if (!webcam_enabled_)
        return {};
    if (magnify_progress_ > 0.0) {
        // Restore hotspot: active throughout the enlarge/collapse animation and
        // while settled-enlarged, tracking the same lerped rect paintEvent draws
        // the PiP content at.
        const QRectF normal = webcamPixelRect();
        const QRectF target = webcamEnlargedTargetRect();
        const QRectF draw_rect(normal.x() + (target.x() - normal.x()) * magnify_progress_,
                               normal.y() + (target.y() - normal.y()) * magnify_progress_,
                               normal.width() + (target.width() - normal.width()) * magnify_progress_,
                               normal.height() + (target.height() - normal.height()) * magnify_progress_);
        return magnifierIconRect(draw_rect).toRect();
    }
    // Normal placement: hover-gated, and hidden while the resize-handle chrome (or
    // an active drag) already occupies the same corners -- mirrors paintEvent.
    const bool show_chrome = webcam_selected_ && webcamEditingAllowed();
    if (!webcam_hovered_ || show_chrome || drag_mode_ != DragMode::None)
        return {};
    return magnifierIconRect(webcamPixelRect()).toRect();
}

void PreviewSurface::snapOverlayRectToCurrentAspect() {
    const QRectF frame_rect = displayedFrameRect();
    const double W = frame_rect.width();
    const double H = frame_rect.height();
    if (W < 1.0 || H < 1.0)
        return;
    const double normalized_ar = webcam_aspect_ratio_ > 0.0 ? (webcam_aspect_ratio_ * H / W) : 0.0;
    if (normalized_ar <= 0.0)
        return;
    webcam_rect_norm_ = snapRectToAspectInside(webcam_rect_norm_, normalized_ar);
}

void PreviewSurface::applyDragFromPointer(QPointF pos, Qt::KeyboardModifiers modifiers) {
    if (drag_mode_ == DragMode::None)
        return;

    const QRectF frame_rect = displayedFrameRect();
    const double W = frame_rect.width();
    const double H = frame_rect.height();
    if (W < 1.0 || H < 1.0)
        return;

    const double dx = (pos.x() - drag_origin_.x()) / W;
    const double dy = (pos.y() - drag_origin_.y()) / H;
    QRectF r = sanitizeOverlayRect(drag_start_rect_);

    constexpr double kMinNorm = kMinOverlayNorm;
    const bool modifier_toggle_held = (modifiers & (Qt::ShiftModifier | Qt::ControlModifier)) != 0;
    drag_modifier_toggle_held_ = modifier_toggle_held;
    const bool effective_aspect_lock = aspect_ratio_locked_ != modifier_toggle_held;
    const double normalized_ar = webcam_aspect_ratio_ > 0.0 ? (webcam_aspect_ratio_ * H / W) : 0.0;
    const bool use_ar = effective_aspect_lock && normalized_ar > 0.0;

    if (use_ar && drag_mode_ != DragMode::Move) {
        const double min_w = kMinNorm;
        const double min_h = kMinNorm;
        switch (drag_mode_) {
        case DragMode::ResizeBR: {
            const double max_w = std::max(min_w, 1.0 - r.left());
            const double max_h = std::max(min_h, 1.0 - r.top());
            const double desired_w = drag_start_rect_.width() + dx;
            const double desired_h = drag_start_rect_.height() + dy;
            const auto [new_w, new_h] = fitAspectSize(desired_w, desired_h, max_w, max_h, min_w, min_h, normalized_ar);
            r.setRight(r.left() + new_w);
            r.setBottom(r.top() + new_h);
            webcam_rect_norm_ = sanitizeOverlayRect(r);
            update();
            return;
        }
        case DragMode::ResizeTL: {
            const double max_w = std::max(min_w, r.right());
            const double max_h = std::max(min_h, r.bottom());
            const double desired_w = r.right() - (drag_start_rect_.left() + dx);
            const double desired_h = r.bottom() - (drag_start_rect_.top() + dy);
            const auto [new_w, new_h] = fitAspectSize(desired_w, desired_h, max_w, max_h, min_w, min_h, normalized_ar);
            r.setLeft(r.right() - new_w);
            r.setTop(r.bottom() - new_h);
            webcam_rect_norm_ = sanitizeOverlayRect(r);
            update();
            return;
        }
        case DragMode::ResizeTR: {
            const double max_w = std::max(min_w, 1.0 - r.left());
            const double max_h = std::max(min_h, r.bottom());
            const double desired_w = (drag_start_rect_.right() + dx) - r.left();
            const double desired_h = r.bottom() - (drag_start_rect_.top() + dy);
            const auto [new_w, new_h] = fitAspectSize(desired_w, desired_h, max_w, max_h, min_w, min_h, normalized_ar);
            r.setRight(r.left() + new_w);
            r.setTop(r.bottom() - new_h);
            webcam_rect_norm_ = sanitizeOverlayRect(r);
            update();
            return;
        }
        case DragMode::ResizeBL: {
            const double max_w = std::max(min_w, r.right());
            const double max_h = std::max(min_h, 1.0 - r.top());
            const double desired_w = r.right() - (drag_start_rect_.left() + dx);
            const double desired_h = (drag_start_rect_.bottom() + dy) - r.top();
            const auto [new_w, new_h] = fitAspectSize(desired_w, desired_h, max_w, max_h, min_w, min_h, normalized_ar);
            r.setLeft(r.right() - new_w);
            r.setBottom(r.top() + new_h);
            webcam_rect_norm_ = sanitizeOverlayRect(r);
            update();
            return;
        }
        default:
            break;
        }
    }

    switch (drag_mode_) {
    case DragMode::Move:
        r.translate(dx, dy);
        r.moveLeft(clampOrdered(r.left(), 0.0, 1.0 - r.width()));
        r.moveTop(clampOrdered(r.top(), 0.0, 1.0 - r.height()));
        break;
    case DragMode::ResizeBR: {
        const double max_w = std::max(kMinNorm, 1.0 - r.left());
        const double max_h = std::max(kMinNorm, 1.0 - r.top());
        double new_w = std::clamp(drag_start_rect_.width() + dx, kMinNorm, max_w);
        double new_h = std::clamp(drag_start_rect_.height() + dy, kMinNorm, max_h);
        if (use_ar) {
            if (std::abs(dx) >= std::abs(dy))
                new_h = std::clamp(new_w / normalized_ar, kMinNorm, max_h);
            else
                new_w = std::clamp(new_h * normalized_ar, kMinNorm, max_w);
        }
        r.setRight(r.left() + new_w);
        r.setBottom(r.top() + new_h);
        break;
    }
    case DragMode::ResizeTL: {
        const double max_left = std::max(0.0, r.right() - kMinNorm);
        const double max_top = std::max(0.0, r.bottom() - kMinNorm);
        double new_left = std::clamp(drag_start_rect_.left() + dx, 0.0, max_left);
        double new_top = std::clamp(drag_start_rect_.top() + dy, 0.0, max_top);
        if (use_ar) {
            if (std::abs(dx) >= std::abs(dy))
                new_top = std::clamp(r.bottom() - (r.right() - new_left) / normalized_ar, 0.0, max_top);
            else
                new_left = std::clamp(r.right() - (r.bottom() - new_top) * normalized_ar, 0.0, max_left);
        }
        r.setLeft(new_left);
        r.setTop(new_top);
        break;
    }
    case DragMode::ResizeTR: {
        const double min_right = std::min(1.0, r.left() + kMinNorm);
        const double max_top = std::max(0.0, r.bottom() - kMinNorm);
        double new_right = std::clamp(drag_start_rect_.right() + dx, min_right, 1.0);
        double new_top = std::clamp(drag_start_rect_.top() + dy, 0.0, max_top);
        if (use_ar) {
            if (std::abs(dx) >= std::abs(dy))
                new_top = std::clamp(r.bottom() - (new_right - r.left()) / normalized_ar, 0.0, max_top);
            else
                new_right = std::clamp(r.left() + (r.bottom() - new_top) * normalized_ar, min_right, 1.0);
        }
        r.setTop(new_top);
        r.setRight(new_right);
        break;
    }
    case DragMode::ResizeBL: {
        const double max_left = std::max(0.0, r.right() - kMinNorm);
        const double min_bottom = std::min(1.0, r.top() + kMinNorm);
        double new_left = std::clamp(drag_start_rect_.left() + dx, 0.0, max_left);
        double new_bottom = std::clamp(drag_start_rect_.bottom() + dy, min_bottom, 1.0);
        if (use_ar) {
            if (std::abs(dx) >= std::abs(dy))
                new_bottom = std::clamp(r.top() + (r.right() - new_left) / normalized_ar, min_bottom, 1.0);
            else
                new_left = std::clamp(r.right() - (new_bottom - r.top()) * normalized_ar, 0.0, max_left);
        }
        r.setLeft(new_left);
        r.setBottom(new_bottom);
        break;
    }
    default:
        break;
    }

    webcam_rect_norm_ = sanitizeOverlayRect(r);
    update();
}

void PreviewSurface::mousePressEvent(QMouseEvent* event) {
    hover_pos_ = event->position();
    hover_pos_valid_ = true;
    const QPointF pos = event->position();

    // Magnifier click handling: hover/click hit-testing is backend-independent (a
    // cursor/hotspot check has no "which preview backend is active" concept). The
    // *painting* of the enlarged view's content differs by backend only in WHERE
    // it happens: paintEvent draws it directly in the QImage fallback, while the
    // animation tick's syncEnlargedWebcamToDxgi() composites the same scrim +
    // lerped rect into the live DXGI preview.
    if (webcam_enabled_ &&
        (magnify_animation_ == nullptr || magnify_animation_->state() != QAbstractAnimation::Running)) {
        if (webcam_enlarged_) {
            // Settled + enlarged (the "Running" guard above rules out mid-animation):
            // the PiP is drawn exactly at its enlarged target rect. Clicking anywhere
            // OUTSIDE that rect collapses back down; clicking inside it is absorbed
            // (no drag/resize while enlarged).
            const QRectF target = webcamEnlargedTargetRect();
            if (!target.contains(pos))
                setWebcamEnlarged(false);
            event->accept();
            return;
        }
        if (webcam_hovered_ && !webcam_selected_) {
            const QRectF icon = magnifierIconRect(webcamPixelRect());
            if (!icon.isEmpty() && icon.contains(pos)) {
                setWebcamEnlarged(true);
                event->accept();
                return;
            }
        }
    }

    if (!webcamEditingAllowed() || magnify_progress_ > 0.0) {
        QWidget::mousePressEvent(event);
        return;
    }
    const DragMode hit = hitTestWebcam(pos);
    if (hit != DragMode::None) {
        // Clicking the PiP selects it (showing edit chrome) and begins the interaction.
        if (!webcam_selected_) {
            webcam_selected_ = true;
            emit webcamSelectionChanged(true);
        }
        drag_mode_ = hit;
        drag_origin_ = pos;
        drag_start_rect_ = sanitizeOverlayRect(webcam_rect_norm_);
        // Captured so Escape can roll back the in-progress drag/resize.
        pre_interaction_rect_ = drag_start_rect_;
        drag_modifier_toggle_held_ = false;
        setFocus(Qt::MouseFocusReason);
        grabKeyboard();
        syncWebcamOverlayToDxgi();
        update();
        event->accept();
    } else {
        // Clicking outside the PiP deselects (keeps the confirmed placement).
        if (webcam_selected_) {
            webcam_selected_ = false;
            emit webcamSelectionChanged(false);
            syncWebcamOverlayToDxgi();
            applyHoverCursor();
            update();
        }
        QWidget::mousePressEvent(event);
    }
}

void PreviewSurface::mouseMoveEvent(QMouseEvent* event) {
    hover_pos_ = event->position();
    hover_pos_valid_ = true;
    const QPointF pos = event->position();

    // Magnifier hover tracking: backend-independent (see mousePressEvent).
    // Independent of the edit-lock gate below -- peeking at a bigger view still
    // works while drag/resize editing is locked (e.g. during a live recording).
    // Uses webcamPixelRect() alone (not intersected with previewContentRect()),
    // matching hitTestWebcam()'s own hit area exactly -- the magnifier hotspot is a
    // click target and must agree with what a click there actually hits.
    if (webcam_enabled_ && magnify_progress_ <= 0.0) {
        const bool now_hovered = webcamPixelRect().contains(pos);
        if (now_hovered != webcam_hovered_) {
            webcam_hovered_ = now_hovered;
            update();
        }
    }

    if (webcam_enlarged_ || magnify_progress_ > 0.0) {
        // Enlarged / mid-transition: the normal placement geometry does not apply,
        // but the enlarged view's own restore hotspot still needs its cursor hint.
        applyHoverCursor();
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (!webcamEditingAllowed()) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    if (drag_mode_ == DragMode::None) {
        if (drag_modifier_toggle_held_) {
            drag_modifier_toggle_held_ = false;
            update();
        }
        applyHoverCursor();
        QWidget::mouseMoveEvent(event);
        return;
    }

    applyDragFromPointer(pos, event->modifiers());
    syncWebcamOverlayToDxgi();
    event->accept();
}

void PreviewSurface::mouseReleaseEvent(QMouseEvent* event) {
    hover_pos_ = event->position();
    hover_pos_valid_ = true;
    if (drag_mode_ != DragMode::None) {
        drag_mode_ = DragMode::None;
        if (drag_modifier_toggle_held_) {
            drag_modifier_toggle_held_ = false;
            update();
        }
        if (QWidget::keyboardGrabber() == this) {
            releaseKeyboard();
        }
        // Confirm the new placement (RecordPage persists it).
        emit webcamOverlayMoved(webcam_rect_norm_);
        syncWebcamOverlayToDxgi();
        // The drag is over — the cursor must reflect what is now under the
        // stationary pointer (e.g. a corner handle after a move-select).
        applyHoverCursor();
        event->accept();
    } else {
        QWidget::mouseReleaseEvent(event);
    }
}

void PreviewSurface::keyPressEvent(QKeyEvent* event) {
    if (webcam_enlarged_ && event->key() == Qt::Key_Escape) {
        setWebcamEnlarged(false);
        event->accept();
        return;
    }
    if (webcam_enabled_ && event->key() == Qt::Key_Escape) {
        if (drag_mode_ != DragMode::None) {
            // Cancel the active drag/resize and restore the pre-interaction geometry.
            drag_mode_ = DragMode::None;
            drag_modifier_toggle_held_ = false;
            webcam_rect_norm_ = sanitizeOverlayRect(pre_interaction_rect_);
            if (QWidget::keyboardGrabber() == this) {
                releaseKeyboard();
            }
            syncWebcamOverlayToDxgi();
            applyHoverCursor();
            update();
            event->accept();
            return;
        }
        if (webcam_selected_) {
            // No active drag: Escape simply deselects (placement unchanged).
            webcam_selected_ = false;
            emit webcamSelectionChanged(false);
            syncWebcamOverlayToDxgi();
            applyHoverCursor();
            update();
            event->accept();
            return;
        }
    }

    if (webcam_enabled_ && drag_mode_ != DragMode::None &&
        (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Control)) {
        if (event->isAutoRepeat()) {
            event->accept();
            return;
        }
        Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
        if (event->key() == Qt::Key_Shift)
            mods |= Qt::ShiftModifier;
        if (event->key() == Qt::Key_Control)
            mods |= Qt::ControlModifier;
        applyDragFromPointer(mapFromGlobal(QCursor::pos()), mods);
        syncWebcamOverlayToDxgi();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void PreviewSurface::keyReleaseEvent(QKeyEvent* event) {
    if (webcam_enabled_ && drag_mode_ != DragMode::None &&
        (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Control)) {
        if (event->isAutoRepeat()) {
            event->accept();
            return;
        }
        Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
        if (event->key() == Qt::Key_Shift)
            mods &= ~Qt::ShiftModifier;
        if (event->key() == Qt::Key_Control)
            mods &= ~Qt::ControlModifier;
        applyDragFromPointer(mapFromGlobal(QCursor::pos()), mods);
        syncWebcamOverlayToDxgi();
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void PreviewSurface::enterEvent(QEnterEvent* event) {
    hover_pos_ = event->position();
    hover_pos_valid_ = true;
    applyHoverCursor();
    QWidget::enterEvent(event);
}

void PreviewSurface::leaveEvent(QEvent* event) {
    hover_pos_valid_ = false;
    applyHoverCursor();
    if (webcam_hovered_) {
        webcam_hovered_ = false;
        update();
    }
    QWidget::leaveEvent(event);
}

void PreviewSurface::hideEvent(QHideEvent* event) {
    // BUG-C: the DXGI child HWND is a manually created WS_CHILD window (see
    // DxgiPreviewRenderer::Initialize), not a QWidget — Qt's own hide cascade for
    // WA_NativeWindow descendants is what would normally take it off-screen when
    // this surface (and its ancestor Record page) is hidden, but this app's
    // frameless/custom-chrome window is not the ordinary case that cascade was
    // designed for, and the render thread keeps calling Present() regardless of
    // OS-level visibility. Hide the child window explicitly and synchronously,
    // as the very first thing that happens on the way out, so it cannot still be
    // composited for even one frame while the next page (e.g. Settings) does its
    // first paint underneath — the reported "brief flicker on navigating away
    // from Record". This does not stop capture or the render thread (that stays
    // fast and stateful for an instant resume in showEvent below); it only
    // toggles OS-level window visibility, which is cheap and never blocks.
    if (dxgi_renderer_)
        dxgi_renderer_->SetChildWindowVisible(false);
    QWidget::hideEvent(event);
}

void PreviewSurface::showEvent(QShowEvent* event) {
    // Inverse of hideEvent: restore the child window's OS-level visibility as
    // soon as this surface is shown again. No-op if no DXGI preview is running
    // (dxgi_renderer_ is null) or if it never went through hideEvent (e.g. first
    // show on app startup) — ShowWindow(SW_SHOWNA) on an already-visible window
    // is harmless.
    if (dxgi_renderer_)
        dxgi_renderer_->SetChildWindowVisible(true);
    QWidget::showEvent(event);
}

// ---------------------------------------------------------------------------
// paintEvent
// ---------------------------------------------------------------------------

void PreviewSurface::paintBrandPlaceholder(QPainter& painter, const QRectF& frame_rect) {
    const auto& t = theme::ActiveTheme();
    const QColor dim(QString::fromUtf8(t.dim)); // hint

    // Lockup scales with the panel: the canonical aperture mark + two-tone
    // "exosnap" wordmark, drawn from the brand SVG (single source of truth — no
    // hand-rendered text), centred, with a quiet hint below.
    const qreal A = std::clamp(frame_rect.height() * 0.12, 38.0, 60.0);
    const qreal cx = frame_rect.center().x();
    const qreal cy = frame_rect.center().y();

    static QSvgRenderer brand_renderer(QStringLiteral(":/brand/exosnap-logo-wordmark.svg"));
    if (brand_renderer.isValid()) {
        const QSizeF def = brand_renderer.defaultSize();
        const qreal aspect = (def.height() > 0.0) ? def.width() / def.height() : 4.375;
        const qreal lock_w = A * aspect;
        const QRectF lock_rect(cx - lock_w / 2.0, cy - A / 2.0, lock_w, A);
        brand_renderer.render(&painter, lock_rect);
    }

    // Quiet help text below the lockup (replaces the retired center text box).
    // Customisable via setPlaceholderHint(); defaults to a no-source prompt.
    const QString hint = placeholder_hint_.isEmpty()
                             ? QStringLiteral("No source selected \xE2\x80\x94 choose one to preview")
                             : placeholder_hint_;
    QFont hf = font();
    hf.setPixelSize(qRound(std::clamp(A * 0.30, 12.0, 16.0)));
    painter.setFont(hf);
    const QFontMetricsF hfm(hf);
    painter.setPen(dim);
    painter.drawText(QPointF(cx - hfm.horizontalAdvance(hint) / 2.0, cy + A / 2.0 + A * 0.70), hint);
}

void PreviewSurface::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF frame_rect = rect();
    // PREVIEW-PANEL: the host (recordPreviewSurfaceHost) is a borderless SQUARE box
    // sized to the source aspect ratio — the box edge IS the record edge, so the
    // surface fills it corner to corner with no rounding and no inner clipping. The
    // recording/warn/blocked status tone is the host's own square 1px border (QSS
    // recordState variants), which also stays visible while the DXGI child HWND
    // covers this widget; no glow is drawn here.

    if (dxgi_active_) {
        painter.fillRect(frame_rect, QColor(0, 0, 0));
    } else {
        // DESIGN-FIDELITY: suite-record.jsx:23 PreviewEmpty — a quiet near-black radial
        // vignette (NOT a flat fill, and no warm/amber cast):
        //   radial-gradient(120% 130% at 50% 40%, #131316 0%, #0C0C0E 58%, #08080A 100%).
        QRadialGradient empty_bg(QPointF(0.5, 0.4), 1.2, QPointF(0.5, 0.4));
        empty_bg.setCoordinateMode(QGradient::ObjectBoundingMode);
        empty_bg.setColorAt(0.0, QColor(0x13, 0x13, 0x16));
        empty_bg.setColorAt(0.58, QColor(0x0C, 0x0C, 0x0E));
        empty_bg.setColorAt(1.0, QColor(0x08, 0x08, 0x0A));
        painter.fillRect(frame_rect, QBrush(empty_bg));

        if (!current_frame_.isNull()) {
            // Draw through the same contain-fit rect used for PiP hit-testing and
            // placement (displayedFrameRect) so the main image and the overlay share
            // exactly one content rectangle (no sub-pixel divergence).
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(displayedFrameRectForSource(current_frame_.width(), current_frame_.height()),
                              current_frame_);
            painter.restore();
        } else {
            // No live frame and no DXGI preview (no source / source unavailable):
            // show the branded capture-safe placeholder instead of a blank panel.
            paintBrandPlaceholder(painter, frame_rect);
        }
    }

    const bool tone_recording = (frame_tone_ == FrameTone::Recording);

    if (tone_recording) {
        painter.save();
        painter.setClipRect(rect().adjusted(1, 1, -1, -1));
        painter.setPen(QPen(QColor(240, 91, 84, 14), 1.0));
        for (int y = 0; y < height(); y += 4)
            painter.drawLine(1, y, width() - 1, y);
        painter.restore();
    }

    // Webcam overlay (Qt paint path). When the DXGI live preview is active the native
    // child HWND occludes this, and the renderer composites the PiP itself
    // (syncWebcamOverlayToDxgi). This path drives the QImage preview and all
    // deterministic visual-test scenarios (which run with DXGI stopped). The
    // magnifier's hover/click handling is backend-independent (see
    // mousePressEvent/mouseMoveEvent), and so is the enlarged view itself --
    // syncEnlargedWebcamToDxgi() composites the same scrim + lerped rect into the
    // live DXGI preview (driven by the animation tick), since the block painted
    // below is Qt-paint-layer-only and occluded the same way as the rest of this
    // function while DXGI is active.
    if (webcam_enabled_ && magnify_progress_ > 0.0) {
        // Floating enlarged view: dim scrim over the whole panel, and the PiP lerped
        // from its normal placement to the centred enlarged target.
        // Preview-only and fully transient: never touches webcam_rect_norm_ (the
        // confirmed WYSIWYG placement) or anything persisted.
        painter.save();
        // Matches ExoSnapMetrics::kRadiusLg (the panel corner radius the record-page
        // QSS frame around this surface already draws), so the scrim's clipped
        // corners agree visually with the surrounding chrome.
        constexpr qreal kPanelRadius = 14.0;
        QPainterPath clip;
        clip.addRoundedRect(frame_rect, kPanelRadius, kPanelRadius);
        painter.setClipPath(clip);

        painter.fillRect(rect(), QColor(6, 6, 8, qRound(150 * magnify_progress_)));

        const QRectF normal = webcamPixelRect();
        const QRectF target = webcamEnlargedTargetRect();
        const QRectF draw_rect(normal.x() + (target.x() - normal.x()) * magnify_progress_,
                               normal.y() + (target.y() - normal.y()) * magnify_progress_,
                               normal.width() + (target.width() - normal.width()) * magnify_progress_,
                               normal.height() + (target.height() - normal.height()) * magnify_progress_);

        if (!webcam_frame_.isNull()) {
            painter.save();
            painter.setOpacity(webcam_opacity_);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const double frame_ar =
                webcam_frame_.height() > 0 ? static_cast<double>(webcam_frame_.width()) / webcam_frame_.height() : 0.0;
            const QRectF img_rect = fitAspectIntoRect(draw_rect, frame_ar);
            if (webcam_mirror_) {
                painter.translate(img_rect.center());
                painter.scale(-1.0, 1.0);
                painter.translate(-img_rect.center());
            }
            painter.drawImage(img_rect, webcam_frame_);
            painter.restore();
        } else {
            painter.save();
            painter.setOpacity(webcam_opacity_);
            painter.setBrush(QColor(0, 0, 0, 160));
            painter.setPen(Qt::NoPen);
            painter.drawRect(draw_rect);
            painter.restore();
        }

        painter.restore();
    } else if (webcam_enabled_) {
        // Clip the PiP to the content rect so it never overpaints the stats footer or
        // the frame border in the preview. Preview-only: the recording compositor
        // draws the PiP un-clipped from the same normalised placement, so the file is
        // unchanged. The whole block (image, placeholder, edit chrome) is clipped.
        painter.save();
        const QRectF cam_rect = webcamPixelRect();
        const bool show_chrome = webcam_selected_ && webcamEditingAllowed();
        if (!webcam_frame_.isNull()) {
            painter.save();
            painter.setOpacity(webcam_opacity_);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const bool effective_aspect_lock = aspect_ratio_locked_ != drag_modifier_toggle_held_;
            QRectF draw_rect = cam_rect;
            if (effective_aspect_lock) {
                const double frame_ar = webcam_frame_.height() > 0
                                            ? static_cast<double>(webcam_frame_.width()) / webcam_frame_.height()
                                            : 0.0;
                draw_rect = fitAspectIntoRect(cam_rect, frame_ar);
                if (draw_rect != cam_rect) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(0, 0, 0, 140));
                    painter.drawRect(cam_rect);
                }
            }
            // Real horizontal mirror about the draw rect's vertical centre (no vertical flip).
            if (webcam_mirror_) {
                painter.translate(draw_rect.center());
                painter.scale(-1.0, 1.0);
                painter.translate(-draw_rect.center());
            }
            painter.drawImage(draw_rect, webcam_frame_);
            painter.restore();
        } else {
            painter.save();
            painter.setOpacity(webcam_opacity_);
            painter.setBrush(QColor(0, 0, 0, 160));
            painter.setPen(Qt::NoPen);
            painter.drawRect(cam_rect);
            painter.setPen(QPen(QColor(255, 255, 255, 100), 1.0));
            painter.setFont(QFont("Arial", 8));
            painter.drawText(cam_rect, Qt::AlignCenter, "CAM");
            painter.restore();
        }

        // Edit chrome (selection border + corner handles) only while selected and
        // editable. Never drawn into the recording (separate compositor path).
        if (show_chrome) {
            painter.save();
            painter.setPen(QPen(QColor("#d7a744"), 1.5, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(cam_rect);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor("#d7a744"));
            constexpr double hs = 6.0;
            for (const QPointF& corner :
                 {cam_rect.topLeft(), cam_rect.topRight(), cam_rect.bottomLeft(), cam_rect.bottomRight()}) {
                painter.drawRect(QRectF(corner.x() - hs / 2, corner.y() - hs / 2, hs, hs));
            }
            painter.restore();
        }

        painter.restore(); // release the preview content-rect clip
    }

    // SUITE-PHASE-F: In-window countdown ring.
    // Drawn only when DXGI is not active (QImage / Ready-state preview) because
    // the DXGI native HWND occludes Qt painting — the on-screen
    // CountdownOverlayWindow handles the live-capture case instead.
    if (countdown_active_ && countdown_remaining_ > 0 && !dxgi_active_) {
        painter.save();

        // Dim scrim over the whole preview
        painter.fillRect(rect(), QColor(8, 8, 10, 89)); // 35% opacity

        // Ring geometry (120 px circle, centred)
        constexpr int kDiameter = 120;
        constexpr int kRadius = kDiameter / 2;
        const int cx = width() / 2;
        const int cy = height() / 2;
        const QRectF ring_rect(cx - kRadius, cy - kRadius, kDiameter, kDiameter);

        // Blurred glass background (approximated by a filled, partially-opaque circle)
        painter.setBrush(QColor(14, 14, 16, 153)); // rgba(14,14,16,0.6)
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(ring_rect);

        // Ring border (caution amber)
        constexpr QColor kCautionBorder(0xE6, 0xC5, 0x7C, 120); // ~50% amber border
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(kCautionBorder, 2.0));
        painter.drawEllipse(ring_rect.adjusted(1.0, 1.0, -1.0, -1.0));

        // Digit
        painter.setPen(QColor(0xE6, 0xC5, 0x7C)); // amber text
        QFont digit_font(QStringLiteral("IBM Plex Mono"));
        if (digit_font.family().isEmpty())
            digit_font.setFamily(QStringLiteral("Consolas"));
        digit_font.setPixelSize(56);
        digit_font.setWeight(QFont::Medium);
        painter.setFont(digit_font);
        painter.drawText(ring_rect, Qt::AlignCenter, QString::number(countdown_remaining_));

        painter.restore();
    }

    // The recording/warn/blocked tone is carried by the host's square 1px border
    // (QSS recordState variants) so it shows identically over the DXGI child HWND
    // and in this Qt paint path; the surface itself draws no frame chrome.
}

void PreviewSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    const int pad_x = 16;
    const int pad_top = 12;
    const int pad_bottom = 12;
    const int top_height = 24;
    const int bottom_height = 24;

    top_row_->setGeometry(pad_x, pad_top, width() - (pad_x * 2), top_height);
    bottom_row_->setGeometry(pad_x, height() - pad_bottom - bottom_height, width() - (pad_x * 2), bottom_height);

    applyOverlayTextElision();
    applyDxgiPreviewResize();
}

} // namespace exosnap::ui::widgets

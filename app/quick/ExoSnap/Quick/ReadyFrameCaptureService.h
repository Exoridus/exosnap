#pragma once

#include "models/VideoSettingsModel.h"
#include "models/WebcamSettings.h"

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QThreadPool>

#include <recorder_core/preview_tap.h>
#include <recorder_core/recorder_session.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace exosnap::quick {

struct ReadyFrameSource {
    // Ownership transfers to Capture(). This is a duplicate of the same NT
    // shared handle currently feeding ExoPreviewItem, never a second capture.
    void* shared_handle = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    recorder_core::PreviewTapDesc tap;
    recorder_core::CaptureTarget target;
    bool cursor_already_composited = false;
};

struct ReadyFrameComposition {
    QRectF normalized_source_rect{0.0, 0.0, 1.0, 1.0};
    VideoSettingsModel video;
    WebcamSettings webcam;
    QImage webcam_frame;
};

class ReadyFrameCaptureService final {
  public:
    using Callback =
        std::function<void(bool ok, uint32_t width, uint32_t height, std::vector<uint8_t> bgra, QString error)>;

    // Runs the capture on `pool`, which the caller must own and outlive-join.
    // The worker opens its own D3D11 device, tone-maps, composites and reads
    // back, and only then invokes `callback` — a callback that reaches into
    // application-owned state. Detaching the worker instead left nobody to wait
    // for it and no cancel path, so process teardown could free that state (and
    // Qt's own statics) while the worker was still touching COM/DXGI.
    static void Capture(QThreadPool& pool, const ReadyFrameSource& source, ReadyFrameComposition composition,
                        Callback callback);

    [[nodiscard]] static QRect SourceCropPixels(uint32_t width, uint32_t height, const QRectF& normalized_rect);
};

} // namespace exosnap::quick

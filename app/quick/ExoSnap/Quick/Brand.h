#pragma once

// The application's own surfaces, addressing the same rendered mark the shell
// does.
//
// ExoBrandMark.qml used to draw the aperture with Qt Quick Shapes, which meant
// the coordinates existed in QML as well as in the asset. They do not any more:
// the mark is `app/assets/brand/marks/brand.svg`, recoloured for the running
// theme and rasterized by ui/brand/ShellIconRenderer, and this singleton is only
// how QML names the one it wants.
//
// Stateless on purpose. The URL carries the appearance and the accent, so a
// theme change re-evaluates the binding and Qt Quick's pixmap cache keys on the
// result -- there is nothing here to invalidate.

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/RecordingPulse.h"
#include "ui/brand/ShellIconRenderer.h"

namespace exosnap::quick {

class Brand : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // One full recording beat, in milliseconds. The shell plays the beat as four
    // whole-icon swaps and stops; an in-application surface is a scene graph and
    // breathes for as long as the recording runs. Two policies, and deliberately
    // ONE period: a HUD on the recorded screen and the tray icon beside it
    // pulsing at different rates is the same recording told two ways.
    Q_PROPERTY(int recordingBeatMs READ recordingBeatMs CONSTANT FINAL)

  public:
    explicit Brand(QObject* parent = nullptr) : QObject(parent) {
    }

    [[nodiscard]] static int recordingBeatMs() noexcept {
        return kRecordingPulseFrameCount * kRecordingPulseIntervalMs;
    }

    // The brand mark at `px` device pixels, for a caller that has its own margin
    // -- an in-application mark is placed by a layout, and the margin a tray icon
    // reserves would read as the mark being too small for its box.
    [[nodiscard]] Q_INVOKABLE static QString source(int px, const QString& appearance_id, const QString& accent_id) {
        ui::brand::ShellMarkRequest request;
        request.kind = ui::brand::BrandMarkKind::Brand;
        request.px = px;
        request.standalone = false;
        request.appearance_id = appearance_id;
        request.accent_id = accent_id;
        return ui::brand::ShellIconImageUrl(ui::brand::MarkImageId(request));
    }
};

} // namespace exosnap::quick

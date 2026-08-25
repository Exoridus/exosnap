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
#include <QSizeF>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/RecordingPulse.h"
#include "ui/brand/BrandMark.h"
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

    // The wordmark's box, as the asset itself declares it: how wide it is per
    // unit of height, and how tall its box is per em of the type it was cut at.
    // Both come from the SVG's viewBox, so a re-exported wordmark moves the
    // layout with it and nothing in QML restates a coordinate.
    Q_PROPERTY(double wordmarkAspect READ wordmarkAspect CONSTANT FINAL)
    Q_PROPERTY(double wordmarkEmHeight READ wordmarkEmHeight CONSTANT FINAL)

  public:
    explicit Brand(QObject* parent = nullptr) : QObject(parent) {
    }

    [[nodiscard]] static int recordingBeatMs() noexcept {
        return kRecordingPulseFrameCount * kRecordingPulseIntervalMs;
    }

    [[nodiscard]] static double wordmarkAspect() {
        return wordmarkBox().width() / wordmarkBox().height();
    }

    [[nodiscard]] static double wordmarkEmHeight() {
        return wordmarkBox().height() / ui::brand::kWordmarkEmUnits;
    }

    // The mark for a shell state, at `px` device pixels high, for a caller that
    // has its own margin -- an in-application mark is placed by a layout, and the
    // margin a tray icon reserves would read as the mark being too small for its
    // box.
    //
    // `icon_state` and `frame` are ShellPresenceAdapter's, unmodified. QML is the
    // wire between the projection and the drawing and decides neither.
    [[nodiscard]] Q_INVOKABLE static QString source(int px, const QString& appearance_id, const QString& accent_id,
                                                    int icon_state = 0, int frame = 0) {
        return url(ui::brand::BrandMarkKindForStateValue(icon_state), px, appearance_id, accent_id, frame);
    }

    // The product name, at `px` device pixels high. Wider than it is tall, and
    // the caller is expected to have asked `wordmarkAspect` how much wider.
    [[nodiscard]] Q_INVOKABLE static QString wordmarkSource(int px, const QString& appearance_id,
                                                            const QString& accent_id) {
        return url(ui::brand::BrandMarkKind::Wordmark, px, appearance_id, accent_id, 0);
    }

  private:
    [[nodiscard]] static QString url(ui::brand::BrandMarkKind kind, int px, const QString& appearance_id,
                                     const QString& accent_id, int frame) {
        ui::brand::ShellMarkRequest request;
        request.kind = kind;
        request.px = px;
        request.frame = frame;
        request.standalone = false;
        request.appearance_id = appearance_id;
        request.accent_id = accent_id;
        return ui::brand::ShellIconImageUrl(ui::brand::MarkImageId(request));
    }

    // Read once: the asset is a compiled-in resource and cannot change under a
    // running process, and the two properties above are evaluated on bindings.
    [[nodiscard]] static const QSizeF& wordmarkBox() {
        static const QSizeF box = ui::brand::BrandMarkViewBox(ui::brand::BrandMarkKind::Wordmark);
        return box;
    }
};

} // namespace exosnap::quick

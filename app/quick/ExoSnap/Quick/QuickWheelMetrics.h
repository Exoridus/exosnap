#pragma once

#include <QGuiApplication>
#include <QObject>
#include <QStyleHints>
#include <QtQmlIntegration/qqmlintegration.h>

namespace exosnap::quick {

// How many lines one mouse wheel notch scrolls, as the platform defines it --
// on Windows the Mouse Properties "Roll the wheel to scroll" setting
// (SPI_GETWHEELSCROLLLINES), default 3. QStyleHints already resolves this per
// platform and keeps it live across a system settings change; this singleton
// exists only to forward it, because QML has no route to
// QGuiApplication::styleHints().
class QuickWheelMetrics : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(int scrollLines READ scrollLines NOTIFY scrollLinesChanged FINAL)

  public:
    explicit QuickWheelMetrics(QObject* parent = nullptr) : QObject(parent) {
        connect(QGuiApplication::styleHints(), &QStyleHints::wheelScrollLinesChanged, this,
                &QuickWheelMetrics::scrollLinesChanged);
    }

    [[nodiscard]] static int scrollLines() noexcept {
        return QGuiApplication::styleHints()->wheelScrollLines();
    }

  signals:
    void scrollLinesChanged();
};

} // namespace exosnap::quick

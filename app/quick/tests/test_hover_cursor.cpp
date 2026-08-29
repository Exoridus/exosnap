// Does a HoverHandler's cursorShape actually reach the window?
//
// The transport's controls all declare `HoverHandler { cursorShape: ... }`, and
// on the shipping build no pointing hand appears over any of them. A QML test
// that reads the handler's own property only proves the declaration is there,
// which was never in doubt. What decides the question is the cursor the WINDOW
// ends up carrying after a hover, so that is what these assert.
//
// Two shapes are covered because they resolve differently: the handler on the
// item the pointer is over, and the handler on a PARENT whose child fills it --
// which is what every control in this product looks like, a FocusScope carrying
// the handler with a Control filling it.
//
// The scene is driven by posted events, never by moving the machine's pointer:
// a QMouseEvent sent to the window goes through its own delivery path and
// leaves the desktop cursor alone.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QUrl>

#include <memory>

namespace {

QGuiApplication* ensureApplication() {
    if (auto* existing = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "hover_cursor_tests";
    static char* argv[] = {app_name, nullptr};
    static QGuiApplication app(argc, argv);
    return &app;
}

void pump(int ms = 200) {
    QElapsedTimer timer;
    timer.start();
    while (!timer.hasExpired(ms))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

Qt::CursorShape ShapeAfterHover(const char* qml) {
    ensureApplication();

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(QByteArray(qml), QUrl(QStringLiteral("qrc:/inline.qml")));
    EXPECT_TRUE(component.isReady()) << component.errorString().toStdString();
    if (!component.isReady())
        return Qt::ArrowCursor;

    std::unique_ptr<QObject> root(component.create());
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    EXPECT_NE(window, nullptr);
    if (window == nullptr)
        return Qt::ArrowCursor;

    window->show();
    pump();

    const QPointF centre(window->width() / 2.0, window->height() / 2.0);
    QMouseEvent move(QEvent::MouseMove, centre, window->mapToGlobal(centre.toPoint()), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(window, &move);
    pump();

    return window->cursor().shape();
}

constexpr const char* kHandlerOnTheHoveredItem = R"(
import QtQuick
Window {
    width: 200; height: 120; visible: true
    Rectangle {
        anchors.fill: parent
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }
}
)";

// The product's shape: the handler sits on the outer scope and an inner item
// fills it. Nothing about the inner item asks for a cursor.
constexpr const char* kHandlerOnAParentCoveredByAChild = R"(
import QtQuick
Window {
    width: 200; height: 120; visible: true
    FocusScope {
        anchors.fill: parent
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        Rectangle { anchors.fill: parent; color: "transparent" }
    }
}
)";

TEST(HoverCursor, HandlerOnTheHoveredItemSetsTheWindowCursor) {
    EXPECT_EQ(ShapeAfterHover(kHandlerOnTheHoveredItem), Qt::PointingHandCursor);
}

TEST(HoverCursor, HandlerOnAParentStillWinsWhenTheChildAsksForNothing) {
    EXPECT_EQ(ShapeAfterHover(kHandlerOnAParentCoveredByAChild), Qt::PointingHandCursor);
}

} // namespace

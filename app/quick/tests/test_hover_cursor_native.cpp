// Does the OS cursor actually change, on a real HWND?
//
// The sibling suite proves that a HoverHandler sets QWindow::cursor(). That is
// only half the path. Qt's Windows plugin turns a QWindow cursor into a visible
// one through QWindowsWindow::setCursor(), which calls SetCursor() ONLY while
// the platform layer believes the pointer is inside that window -- and the
// platform layer learns that from real mouse messages, never from a
// QCoreApplication::sendEvent(). A test that posts a QMouseEvent therefore
// cannot fail the way the shipping build fails.
//
// These drive a real top-level HWND with WM_MOUSEMOVE and read GetCursor(), so
// the assertion is the cursor a user would see. WM_MOUSEMOVE is SENT to our own
// window; it moves no pointer, takes no focus and synthesizes no input.
//
// The configurations are the shipping window's two deviations from a default
// Qt Quick window, crossed: WS_EX_NOREDIRECTIONBITMAP (the flag that removed
// the near-white startup block) and Qt::FramelessWindowHint (the custom
// chrome). If the cursor survives all four, neither of them is the cause.

#include <gtest/gtest.h>

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QUrl>

#include <memory>

#include <windows.h>

namespace {

QGuiApplication* ensureApplication() {
    if (auto* existing = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        return existing;

    static int argc = 1;
    static char app_name[] = "hover_cursor_native_tests";
    static char* argv[] = {app_name, nullptr};
    static QGuiApplication app(argc, argv);
    return &app;
}

void pump(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (!timer.hasExpired(ms))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

constexpr const char* kFramed = R"(
import QtQuick
Window {
    width: 320; height: 200; visible: true
    Rectangle {
        anchors.fill: parent
        color: "#202020"
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }
}
)";

constexpr const char* kFrameless = R"(
import QtQuick
Window {
    width: 320; height: 200; visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    Rectangle {
        anchors.fill: parent
        color: "#202020"
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }
}
)";

// The two things QuickWindowChrome does to the message stream that could plausibly
// reach the cursor: it ANSWERS WM_NCHITTEST itself (so Qt's window procedure never
// sees the message) and it makes the client area cover the whole window. Replicated
// rather than linked so this stays a cursor test; the full class carries the taskbar,
// the DWM border and the Snap Layouts block, none of which touch a cursor.
class BorderlessChromeFilter : public QAbstractNativeEventFilter {
  public:
    explicit BorderlessChromeFilter(HWND hwnd) : hwnd_(hwnd) {
    }

    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) override {
        if (message == nullptr)
            return false;
        if (event_type != QByteArrayLiteral("windows_generic_MSG") &&
            event_type != QByteArrayLiteral("windows_dispatcher_MSG")) {
            return false;
        }
        auto* msg = static_cast<MSG*>(message);
        if (msg->hwnd != hwnd_)
            return false;

        switch (msg->message) {
        case WM_NCHITTEST:
            // The content area's answer. The title band's HTCAPTION and the resize
            // grips are deliberately left out: no control lives there.
            if (result != nullptr)
                *result = HTCLIENT;
            return true;
        case WM_NCCALCSIZE:
            if (msg->wParam != static_cast<WPARAM>(TRUE))
                return false;
            if (result != nullptr)
                *result = 0;
            return true;
        default:
            return false;
        }
    }

  private:
    HWND hwnd_ = nullptr;
};

// The product's transport controls: the handler on a FocusScope, and a Controls
// Button filling it that accepts hover events of its own. Everything else about
// them is the same as the shapes above, and they are the ones with no pointing
// hand on the shipping build.
constexpr const char* kHoverAcceptingChildOverTheHandler = R"(
import QtQuick
import QtQuick.Controls.Basic
Window {
    width: 320; height: 200; visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    FocusScope {
        anchors.fill: parent
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        Button { anchors.fill: parent; hoverEnabled: true }
    }
}
)";

// The same tree with the child's own hover switched off. Its visuals never read
// the Button's `hovered` in this product -- they read the scope's handler.
constexpr const char* kNonHoverAcceptingChildOverTheHandler = R"(
import QtQuick
import QtQuick.Controls.Basic
Window {
    width: 320; height: 200; visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    FocusScope {
        anchors.fill: parent
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        Button { anchors.fill: parent; hoverEnabled: false }
    }
}
)";

// Same scope, same fill, but the child is inert. Separates "a FocusScope cannot
// carry a HoverHandler" from "a Controls Button on top of one swallows hover".
constexpr const char* kInertChildOverTheHandler = R"(
import QtQuick
Window {
    width: 320; height: 200; visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    FocusScope {
        anchors.fill: parent
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        Rectangle { anchors.fill: parent; color: "#202020" }
    }
}
)";

// Fix candidate 1: the handler on the control itself.
constexpr const char* kHandlerOnTheControl = R"(
import QtQuick
import QtQuick.Controls.Basic
Window {
    width: 320; height: 200; visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    FocusScope {
        anchors.fill: parent
        Button {
            anchors.fill: parent
            hoverEnabled: true
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }
}
)";

// Fix candidate 1 under the condition the product actually needs: the control is
// DISABLED, which is the state whose cursor the transport has to keep saying
// something about. The handler's own documentation says it keeps reacting when
// its parent is disabled; this is whether that survives all the way to the OS.
constexpr const char* kHandlerOnADisabledControl = R"(
import QtQuick
import QtQuick.Controls.Basic
Window {
    width: 320; height: 200; visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    FocusScope {
        anchors.fill: parent
        Button {
            anchors.fill: parent
            enabled: false
            hoverEnabled: true
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }
}
)";

// Fix candidate 2: the handler on a transparent item declared AFTER the control,
// so it is in front of it. An Item accepts no mouse buttons, so the control
// underneath still gets every press.
constexpr const char* kHandlerOnATopmostTransparentItem = R"(
import QtQuick
import QtQuick.Controls.Basic
Window {
    width: 320; height: 200; visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    FocusScope {
        anchors.fill: parent
        Button { anchors.fill: parent; enabled: false; hoverEnabled: true }
        Item {
            anchors.fill: parent
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }
}
)";

// The negative control. Without it a green suite proves only that the desktop
// cursor happened to be a hand already, or that GetCursor() reports something
// this harness never influenced.
constexpr const char* kNoHandlerAtAll = R"(
import QtQuick
Window {
    width: 320; height: 200; visible: true
    Rectangle { anchors.fill: parent; color: "#202020" }
}
)";

// Leaves the desktop cursor as it was found: SetCursor() is process-visible
// state, and a test that walks away from a pointing hand hands it to whatever
// runs next.
class CursorGuard {
  public:
    CursorGuard() : previous_(GetCursor()) {
    }
    ~CursorGuard() {
        if (previous_ != nullptr)
            SetCursor(previous_);
    }
    CursorGuard(const CursorGuard&) = delete;
    CursorGuard& operator=(const CursorGuard&) = delete;

  private:
    HCURSOR previous_ = nullptr;
};

// Returns the cursor the OS carries after the pointer is reported over the
// window's centre, or nullptr if the scene could not be brought up.
HCURSOR CursorAfterNativeHover(const char* qml, bool disable_redirection_surface, bool borderless_chrome = false) {
    ensureApplication();
    if (QGuiApplication::platformName() != QLatin1String("windows"))
        return nullptr;

    // Read per window creation by the platform plugin, so it can be flipped
    // between the cases in one process.
    qputenv("QT_QPA_DISABLE_REDIRECTION_SURFACE", disable_redirection_surface ? "1" : "0");

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(QByteArray(qml), QUrl(QStringLiteral("qrc:/inline.qml")));
    EXPECT_TRUE(component.isReady()) << component.errorString().toStdString();
    if (!component.isReady())
        return nullptr;

    std::unique_ptr<QObject> root(component.create());
    auto* window = qobject_cast<QQuickWindow*>(root.get());
    EXPECT_NE(window, nullptr);
    if (window == nullptr)
        return nullptr;

    // The developer may be working on this machine while the suite runs. A test
    // window that activates would take their keyboard focus, and nothing here
    // needs focus: WM_MOUSEMOVE reaches an inactive window unchanged. Applied to
    // every case so the four differ only in what they are meant to differ in.
    window->setFlag(Qt::WindowDoesNotAcceptFocus, true);
    window->show();
    pump(400);

    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    EXPECT_NE(hwnd, nullptr);
    if (hwnd == nullptr)
        return nullptr;

    // Installed after the window exists, so the frame has to be recalculated for
    // WM_NCCALCSIZE to be asked again.
    std::unique_ptr<BorderlessChromeFilter> chrome;
    if (borderless_chrome) {
        chrome = std::make_unique<BorderlessChromeFilter>(hwnd);
        QCoreApplication::instance()->installNativeEventFilter(chrome.get());
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        pump(200);
    }
    const struct ChromeRemover {
        QAbstractNativeEventFilter* filter;
        ~ChromeRemover() {
            if (filter != nullptr)
                QCoreApplication::instance()->removeNativeEventFilter(filter);
        }
    } chrome_remover{chrome.get()};

    RECT client{};
    if (GetClientRect(hwnd, &client) == FALSE)
        return nullptr;
    const auto x = static_cast<WORD>((client.right - client.left) / 2);
    const auto y = static_cast<WORD>((client.bottom - client.top) / 2);

    CursorGuard guard;
    // Sent, not posted: the handler runs before this returns, so the pump below
    // only has to flush the queued Qt-side delivery.
    SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    pump(200);
    const HCURSOR shown = GetCursor();

    window->hide();
    pump(50);
    return shown;
}

const char* CursorName(HCURSOR cursor) {
    if (cursor == nullptr)
        return "none";
    if (cursor == LoadCursorW(nullptr, IDC_HAND))
        return "IDC_HAND";
    if (cursor == LoadCursorW(nullptr, IDC_ARROW))
        return "IDC_ARROW";
    return "other";
}

void ExpectArrow(HCURSOR shown) {
    if (shown == nullptr)
        GTEST_SKIP() << "no windows platform plugin; nothing native to assert";
    EXPECT_NE(shown, LoadCursorW(nullptr, IDC_HAND)) << "OS cursor was " << CursorName(shown);
}

void ExpectPointingHand(HCURSOR shown) {
    if (shown == nullptr)
        GTEST_SKIP() << "no windows platform plugin; nothing native to assert";
    EXPECT_EQ(shown, LoadCursorW(nullptr, IDC_HAND)) << "OS cursor was " << CursorName(shown);
}

TEST(HoverCursorNative, AWindowThatAsksForNothingKeepsTheArrow) {
    const HCURSOR shown = CursorAfterNativeHover(kNoHandlerAtAll, false);
    if (shown == nullptr)
        GTEST_SKIP() << "no windows platform plugin; nothing native to assert";
    EXPECT_NE(shown, LoadCursorW(nullptr, IDC_HAND)) << "OS cursor was " << CursorName(shown);
}

TEST(HoverCursorNative, FramedWindowWithARedirectionSurface) {
    ExpectPointingHand(CursorAfterNativeHover(kFramed, false));
}

TEST(HoverCursorNative, FramedWindowWithoutARedirectionSurface) {
    ExpectPointingHand(CursorAfterNativeHover(kFramed, true));
}

TEST(HoverCursorNative, FramelessWindowWithARedirectionSurface) {
    ExpectPointingHand(CursorAfterNativeHover(kFrameless, false));
}

TEST(HoverCursorNative, FramelessWindowWithoutARedirectionSurface) {
    ExpectPointingHand(CursorAfterNativeHover(kFrameless, true));
}

// The shipping combination: borderless frame plus a hit test Qt never sees.
TEST(HoverCursorNative, FramelessWindowWithBorderlessChromeAnsweringTheHitTest) {
    ExpectPointingHand(CursorAfterNativeHover(kFrameless, true, true));
}

TEST(HoverCursorNative, AHandlerOnTheControlItselfReachesTheDesktop) {
    ExpectPointingHand(CursorAfterNativeHover(kHandlerOnTheControl, true, true));
}

// Not a wish: the measured behaviour, and the reason the transport cannot simply
// move its handler onto the control. Qt delivers no hover to a disabled item, so
// a handler declared inside one asks for a cursor nobody ever sets. If a future
// Qt changes this, this case goes red and the extra hover surface can go away.
TEST(HoverCursorNative, AHandlerOnADisabledControlNeverReachesTheDesktop) {
    ExpectArrow(CursorAfterNativeHover(kHandlerOnADisabledControl, true, true));
}

TEST(HoverCursorNative, AHandlerOnATopmostTransparentItemReachesTheDesktop) {
    ExpectPointingHand(CursorAfterNativeHover(kHandlerOnATopmostTransparentItem, true, true));
}

TEST(HoverCursorNative, AnInertChildLeavesTheParentHandlerAlone) {
    ExpectPointingHand(CursorAfterNativeHover(kInertChildOverTheHandler, true, true));
}

// The defect this suite was written for. A Controls item that fills its parent
// swallows the parent's HoverHandler, so the pointing hand every transport
// control declared never appeared. Pinned in both directions because the obvious
// suspect is innocent: switching the child's own hoverEnabled off changes
// nothing, so `hoverEnabled` is not the lever and never was.
TEST(HoverCursorNative, AControlFillingItsParentSwallowsTheParentHandler) {
    ExpectArrow(CursorAfterNativeHover(kHoverAcceptingChildOverTheHandler, true, true));
}

TEST(HoverCursorNative, ItSwallowsItEvenWithItsOwnHoverDisabled) {
    ExpectArrow(CursorAfterNativeHover(kNonHoverAcceptingChildOverTheHandler, true, true));
}

} // namespace

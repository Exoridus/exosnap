#pragma once

#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QIcon>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRectF>
// Full include rather than a forward declaration: QQuickWindow* appears as a
// Q_PROPERTY type and a Q_INVOKABLE parameter, and the meta-type system rejects
// pointers to incomplete types.
#include <QQuickWindow>
#include <QtQmlIntegration/qqmlintegration.h>

#include "MainWindowAffinity.h"

#include <functional>

namespace exosnap::quick {

// Win32 non-client chrome for the borderless Quick shell.
//
// WHY THIS TYPE EXISTS AT ALL
// ---------------------------
// The borderless 40 px title bar had to be reverted in the Widgets shell because
// `WM_NCHITTEST` is only ever sent to the window that OWNS the pixel under the
// cursor. `PreviewSurface` set `Qt::WA_NativeWindow`, so a child HWND owned the
// band the title bar drew into and the top-level was never asked — no drag, no
// resize, no Snap Layouts. A `QQuickWindow` is a single top-level HWND with no
// native children, so the whole client area (including the title band) belongs to
// the one window that answers the hit test. That removes the blocker; this class
// supplies the answer.
//
// WHY QObject AND QAbstractNativeEventFilter IN ONE CLASS
// ------------------------------------------------------
// `QuickHotkeyEventFilter` (QuickApplication.cpp) is a separate class because it
// only forwards `WM_HOTKEY` into a `std::function` and owns no state of its own.
// Here every branch of the filter reads this object's QML-driven properties
// (`titleBarHeight`, `interactiveRects`, `maximizeButtonRect`, ...) and writes
// back into notifying properties. A split class would need a back-pointer plus a
// lifetime rule to keep it valid, so the two responsibilities are fused: QObject
// first (moc requires it), the interface second. The destructor removes the
// filter, which makes the lifetime rule trivially correct.
//
// COORDINATE SPACES
// -----------------
// Everything Windows hands us (`MSG::lParam`, `GetClientRect`, `MINMAXINFO`) is in
// PHYSICAL device pixels. Everything QML pushes in (`titleBarHeight`,
// `interactiveRects`, `maximizeButtonRect`) is in LOGICAL Qt pixels. The single
// conversion point is `QWindow::devicePixelRatio()`, applied inside the hit test:
// physical client coordinates are divided down into logical ones and compared
// against the QML rects there. Nothing else in this class mixes the two.
class QuickWindowChrome : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QQuickWindow* target READ target WRITE setTarget NOTIFY targetChanged FINAL)
    Q_PROPERTY(int titleBarHeight READ titleBarHeight WRITE setTitleBarHeight NOTIFY titleBarHeightChanged FINAL)
    Q_PROPERTY(int resizeBorderThickness READ resizeBorderThickness WRITE setResizeBorderThickness NOTIFY
                   resizeBorderThicknessChanged FINAL)
    Q_PROPERTY(QList<QRectF> interactiveRects READ interactiveRects WRITE setInteractiveRects NOTIFY
                   interactiveRectsChanged FINAL)
    Q_PROPERTY(QRectF maximizeButtonRect READ maximizeButtonRect WRITE setMaximizeButtonRect NOTIFY
                   maximizeButtonRectChanged FINAL)
    // MEASURED, and the reason this is a Win32 fact rather than Window.visibility:
    // setting `visibility = Window.Maximized` on this frameless window produces a
    // single SetWindowPos onto the work area and nothing else -- no WM_SHOWWINDOW,
    // no WS_MAXIMIZE, no SC_MAXIMIZE. The window is then work-area sized while
    // Windows still has it in SW_SHOWNORMAL, so it stays resizable and Windows
    // records the work-area rect as the rect to un-maximize to. IsZoomed() is the
    // only value every path agrees on, including the ones Windows performs on its
    // own (Snap, double-click, Win+arrow, drag-to-restore).
    Q_PROPERTY(bool windowMaximized READ windowMaximized NOTIFY windowMaximizedChanged FINAL)
    Q_PROPERTY(bool maximizeButtonHovered READ maximizeButtonHovered NOTIFY maximizeButtonHoveredChanged FINAL)
    Q_PROPERTY(bool maximizeButtonPressed READ maximizeButtonPressed NOTIFY maximizeButtonPressedChanged FINAL)
    Q_PROPERTY(QColor borderColor READ borderColor WRITE setBorderColor NOTIFY borderColorChanged FINAL)
    // Both of the following gate behaviour that is NOT verified on this codebase
    // (see the notes on the corresponding handlers). They are runtime-togglable
    // on purpose so a measurement session can A/B them without a rebuild.
    Q_PROPERTY(bool snapLayoutsEnabled READ snapLayoutsEnabled WRITE setSnapLayoutsEnabled NOTIFY
                   snapLayoutsEnabledChanged FINAL)
    Q_PROPERTY(bool nonClientActivationWorkaround READ nonClientActivationWorkaround WRITE
                   setNonClientActivationWorkaround NOTIFY nonClientActivationWorkaroundChanged FINAL)

  public:
    // The default title band height matches ui::theme::ExoSnapMetrics::kTitlebarHeight (40).
    static constexpr int kDefaultTitleBarHeight = 40;
    // The Widgets shell used an 8 px grab band (resizeZoneFromLocalPoint).
    static constexpr int kDefaultResizeBorderThickness = 8;

    explicit QuickWindowChrome(QObject* parent = nullptr);
    ~QuickWindowChrome() override;

    QuickWindowChrome(const QuickWindowChrome&) = delete;
    QuickWindowChrome& operator=(const QuickWindowChrome&) = delete;

    // Binds to `window`, installs the native event filter, re-adds WS_THICKFRAME
    // and paints the DWM border. Passing nullptr is equivalent to detach().
    Q_INVOKABLE void attach(QQuickWindow* window);
    Q_INVOKABLE void detach();
    // Qt can recreate the platform window (flag changes, some DPI transitions),
    // which invalidates the cached HWND. Re-reads it. Exposed to QML for a caller
    // that changes such a property; today the only callers are C++.
    Q_INVOKABLE void refreshHandle();

    // Re-asserts the window styles the native gestures are gated on, once Qt has
    // written the window style for the last time, and must be called then rather
    // than at attach.
    //
    // MEASURED: attach happens while the HWND still carries the framed style Qt
    // creates it with (WS_CAPTION|WS_THICKFRAME|WS_SYSMENU|...), so the check in
    // ensureNativeFrameStyle finds the bits already set and returns. Qt then
    // applies Qt::FramelessWindowHint, which rewrites the whole style to WS_POPUP
    // and takes all of them with it -- and with them the native resize drag, Aero
    // Snap, Win+Arrow, double-click-to-maximize and the window menu, none of
    // which look wrong in a screenshot.
    Q_INVOKABLE void applyNativeWindowStyle();

    // Incremental builders so QML can assemble the exclusion list from repeaters
    // without materialising a JS array first.
    Q_INVOKABLE void clearInteractiveRects();
    Q_INVOKABLE void addInteractiveRect(const QRectF& rect);

    // Maximizes or restores through ShowWindow, which is what keeps Windows'
    // own WINDOWPLACEMENT bookkeeping intact: the rect the window un-maximizes to
    // is the one it last stood on, maintained by Windows rather than tracked
    // alongside it.
    Q_INVOKABLE void setWindowMaximized(bool maximized);
    Q_INVOKABLE void toggleMaximized();

    // Minimizes through ShowWindow for the same reason as the two above: Windows
    // records WPF_RESTORETOMAXIMIZED in the placement when a MAXIMIZED window is
    // minimized, and that flag is the only thing that brings it back maximized
    // from the taskbar.
    //
    // Consults the minimize-to-tray provider first, so the title bar's own button
    // resolves exactly as Win+Down and the window menu do.
    Q_INVOKABLE void minimizeWindow();

    // Answers "should a minimize hide the window to the tray instead?". Set once
    // by the application, which owns the persisted preference and knows whether a
    // tray exists at all (models/WindowPresencePolicy). Absent means the ordinary
    // taskbar minimize -- the safe answer, because hiding a window with no
    // restore path strands the user.
    //
    // Deliberately NOT a QML-side check on the button: SC_MINIMIZE reaches this
    // object from the window menu, from Win+Down and from a click on the taskbar
    // button of the active window, none of which QML ever sees. One provider, one
    // outcome, whatever asked.
    void setMinimizeToTrayProvider(std::function<bool()> provider);

    // Answers a WM_COMMAND. Returns true when the command was one of ours and
    // Windows must not perform it. The handler is what the taskbar's thumbnail
    // buttons arrive through: THBN_CLICKED is delivered as a WM_COMMAND, and this
    // window has no menus and no accelerators of its own, so everything else in
    // that message is somebody else's.
    //
    // A provider rather than a signal for the same reason as the minimize one
    // below: the filter needs an ANSWER, and a signal has none.
    void setNativeCommandHandler(std::function<bool(quint64)> handler);
    [[nodiscard]] bool handleNativeCommand(quint64 wparam);

    // The shell HWND as an opaque handle, for the per-HWND shell integrations
    // that live outside this class. Null before attach and after detach.
    [[nodiscard]] void* nativeHandle() const noexcept;

    // Answers a WM_SYSCOMMAND. Returns true when the command was taken over and
    // Windows must not perform it -- today only SC_MINIMIZE, and only while the
    // provider says the minimize belongs in the tray.
    //
    // Public rather than buried in the filter so the native route is reachable
    // without a live message pump: what has to be provable is that it and
    // minimizeWindow() consult the SAME provider, which is exactly the thing a
    // QML-side check on the visible button would break.
    [[nodiscard]] bool handleSysCommand(quint64 wparam);

    // WDA_EXCLUDEFROMCAPTURE for the shell window. Applied immediately, and
    // re-applied whenever the native handle changes identity -- display affinity
    // is per-HWND and does not survive a recreate.
    //
    // Fail-OPEN, unlike the overlays' CaptureExclusion: a refused platform call
    // leaves the window visible and usable and is logged (MainWindowAffinity).
    void setCaptureExcluded(bool excluded);
    [[nodiscard]] bool captureExcluded() const noexcept;
    // What actually happened, which is not the same question: a window that is
    // present in a capture because the call was refused looks exactly like one
    // whose setting is off.
    [[nodiscard]] bool captureExclusionApplied() const noexcept;
    void setAffinityFunctionForTest(MainWindowAffinity::AffinityFunction fn);

    // Un-minimizes without deciding what to un-minimize INTO. SW_RESTORE is the
    // gesture the taskbar button performs, so a window that was maximized comes
    // back maximized; Qt's showNormal() forces WindowNoState and does not.
    // A window that is not iconic is left alone.
    Q_INVOKABLE void restoreWindow();

    // What this window will occupy the screen with once it is shown again --
    // the value to bank before hiding it to the tray.
    //
    // Two sources, and which one applies is documented, not guessed:
    // IsZoomed answers for a window that is not minimized. For one that IS,
    // WPF_RESTORETOMAXIMIZED is the authority ("the restored window will be
    // maximized, regardless of whether it was maximized before it was
    // minimized"), and it is valid ONLY while showCmd is SW_SHOWMINIMIZED --
    // which is exactly the case being asked about here.
    [[nodiscard]] Q_INVOKABLE bool willOccupyScreenMaximized() const;

    // Sets the LIVE WINDOW's icon, which is what its frame and its taskbar button
    // show. `icon` should carry a pixmap at each of the two Windows icon metrics,
    // because Qt picks the nearest one per metric rather than scaling.
    //
    // The executable's own icon is a different thing and is not touched here:
    // Explorer, the desktop and Start read the PE resource table, which WM_SETICON
    // does not reach.
    void applyWindowIcon(const QIcon& icon);

    [[nodiscard]] QQuickWindow* target() const noexcept;
    void setTarget(QQuickWindow* window);

    [[nodiscard]] int titleBarHeight() const noexcept;
    void setTitleBarHeight(int height);

    [[nodiscard]] int resizeBorderThickness() const noexcept;
    void setResizeBorderThickness(int thickness);

    [[nodiscard]] const QList<QRectF>& interactiveRects() const noexcept;
    void setInteractiveRects(const QList<QRectF>& rects);

    [[nodiscard]] QRectF maximizeButtonRect() const noexcept;
    void setMaximizeButtonRect(const QRectF& rect);

    [[nodiscard]] bool windowMaximized() const noexcept;

    [[nodiscard]] bool maximizeButtonHovered() const noexcept;
    [[nodiscard]] bool maximizeButtonPressed() const noexcept;

    [[nodiscard]] QColor borderColor() const noexcept;
    void setBorderColor(const QColor& color);

    [[nodiscard]] bool snapLayoutsEnabled() const noexcept;
    void setSnapLayoutsEnabled(bool enabled);

    [[nodiscard]] bool nonClientActivationWorkaround() const noexcept;
    void setNonClientActivationWorkaround(bool enabled);

    bool nativeEventFilter(const QByteArray& event_type, void* message, qintptr* result) override;

  signals:
    void targetChanged();
    void titleBarHeightChanged();
    void resizeBorderThicknessChanged();
    void interactiveRectsChanged();
    void maximizeButtonRectChanged();
    void windowMaximizedChanged();
    void maximizeButtonHoveredChanged();
    void maximizeButtonPressedChanged();
    void borderColorChanged();
    void snapLayoutsEnabledChanged();
    void nonClientActivationWorkaroundChanged();

    // The Windows shell theme changed under a running process. Carried here
    // because WM_SETTINGCHANGE is a broadcast to top-level windows and this class
    // already owns the only one we have; the surfaces that care are the tray icon
    // and the taskbar button, neither of which is part of the scene.
    void shellColorsChanged();

    // Emitted on a completed NC click on maximizeButtonRect. The shell routes it
    // back into toggleMaximized() rather than acting on it here, so the button and
    // the title-bar control share one path.
    void maximizeButtonClicked();

    // A minimize request that the provider resolved to "hide to the tray". The
    // hide itself belongs to the application: it banks the window geometry and
    // the maximized state first, and it owns the tray icon that brings the window
    // back.
    void minimizeToTrayRequested();

    // Explorer created (or re-created, after its own restart) the taskbar button
    // for this window. ITaskbarList3 is not usable before this: calls made
    // earlier are accepted by COM and dropped, which looks exactly like a silent
    // bug. Carried as a signal rather than acted on here -- what the taskbar
    // button shows is product state, and this class owns none.
    void taskbarButtonCreated();

    // The native handle changed identity. Everything applied per-HWND -- display
    // affinity, the taskbar button's registrations -- has to be re-asserted, and
    // this is the one place a real recreation is observed.
    void nativeHandleChanged();

  private:
    // `hwnd_` is the raw HWND but is typed as void* so this header stays free of
    // <windows.h> — it is included by moc-generated TUs and by any QML consumer.
    void* hwnd_ = nullptr;
    QPointer<QQuickWindow> target_;

    int title_bar_height_ = kDefaultTitleBarHeight;
    int resize_border_thickness_ = kDefaultResizeBorderThickness;
    QList<QRectF> interactive_rects_;
    QRectF maximize_button_rect_;
    bool window_maximized_ = false;
    bool maximize_button_hovered_ = false;
    bool maximize_button_pressed_ = false;
    QColor border_color_;
    bool snap_layouts_enabled_ = true;
    bool non_client_activation_workaround_ = true;
    // TrackMouseEvent is one-shot; this stops us re-arming it on every single
    // WM_NCMOUSEMOVE.
    bool non_client_leave_tracked_ = false;

    std::function<bool()> minimize_to_tray_provider_;
    std::function<bool(quint64)> native_command_handler_;
    MainWindowAffinity affinity_;

    // What the DWM border attribute was last set to, and on which handle. Mutable
    // because applyBorderColor is const: it changes the window, never this object's
    // observable state. `quint32` rather than COLORREF to keep <windows.h> out of a
    // header moc-generated translation units include.
    mutable quint32 applied_border_color_ = 0;
    mutable bool applied_border_valid_ = false;
    mutable void* applied_border_hwnd_ = nullptr;

    // Re-reads IsZoomed and emits on a real change. Called from the message
    // stream rather than from a state setter: the state has writers this process
    // never sees -- Snap, the window menu, Win+arrow, the drag that pulls a
    // maximized window loose.
    void refreshWindowMaximized();

    void setMaximizeButtonHovered(bool hovered);
    void setMaximizeButtonPressed(bool pressed);

    // Reads the live client rect and scale factor, then resolves the HT* code.
    // A window that is gone falls back to HTCLIENT: Windows already resolved the
    // point to this HWND, so claiming HTNOWHERE for it would be a lie.
    [[nodiscard]] qintptr resolveHitTest(qintptr lparam) const;

    void applyBorderColor(const char* reason) const;
    void ensureNativeFrameStyle() const;

    // Consults the provider and, when it says so, emits minimizeToTrayRequested.
    // Returns whether the minimize was taken over -- the caller must then NOT
    // perform the ordinary one.
    [[nodiscard]] bool requestMinimizeToTray();
};

} // namespace exosnap::quick

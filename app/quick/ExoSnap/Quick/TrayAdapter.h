#pragma once

// Everything the notification-area icon shows, and everything a click on it can
// mean.
//
// The tray itself is QML (`ShellTray.qml`, on Qt.labs.platform). This is the
// model behind it, and the split is deliberate: what the menu OFFERS is product
// policy and stays in C++, so QML never re-derives whether Pause is legal. It
// binds to the rows below and calls back with the row it was given.
//
// It decides nothing about the session either. The rows come from
// `ShellButtonFor()` -- the same appearance table the taskbar's thumbnail strip
// reads -- so a tray entry and the thumbnail button beside it cannot offer two
// different answers.

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/ShellPresence.h"

namespace exosnap::quick {

class TrayAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("TrayAdapter is provided by the application")

    // False when the platform has no notification area, or when a harness mode
    // suppressed the icon. The QML tray binds its visibility to this rather than
    // being conditionally instantiated: a tray that exists and is hidden keeps
    // one code path.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged FINAL)

    // An image-provider URL carrying the state, the size, the heartbeat frame and
    // the resolved palette. It changes only when one of those does, which is what
    // keeps Qt Quick's pixmap cache doing the work instead of the painter.
    Q_PROPERTY(QString iconSource READ iconSource NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString tooltip READ tooltip NOTIFY appearanceChanged FINAL)

    // "Show window" / "Hide window" -- one entry, and the label decides which of
    // the two signals it raises. It used to raise the window under both.
    Q_PROPERTY(QString showHideText READ showHideText NOTIFY appearanceChanged FINAL)

    // One transport row each: `{ visible, enabled, text, icon }`. Assembled from
    // the appearance table, not from the recording state.
    Q_PROPERTY(QVariantMap recordItem READ recordItem NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QVariantMap pauseResumeItem READ pauseResumeItem NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QVariantMap stopItem READ stopItem NOTIFY appearanceChanged FINAL)

    // The unread mirror for toasts raised while the window was not on screen.
    Q_PROPERTY(int unreadCount READ unreadCount NOTIFY unreadCountChanged FINAL)
    Q_PROPERTY(bool notificationsVisible READ notificationsVisible NOTIFY unreadCountChanged FINAL)
    Q_PROPERTY(QString notificationsText READ notificationsText NOTIFY unreadCountChanged FINAL)

  public:
    // Which of our three transport rows a QML callback is talking about. Mirrors
    // ShellButton; exposed separately because QML needs an enum it can name.
    enum TransportRow : int {
        RecordRow,
        PauseResumeRow,
        StopRow,
    };
    Q_ENUM(TransportRow)

    // QPlatformSystemTrayIcon::ActivationReason, which is what
    // Qt.labs.platform's SystemTrayIcon reports. Repeated here rather than
    // included: the enum is a QtGui platform-interface detail, and this maps a
    // GESTURE onto a product intent, which is a decision worth having in one
    // testable place.
    enum ActivationReason : int {
        UnknownActivation = 0,
        ContextActivation = 1,
        DoubleClickActivation = 2,
        TriggerActivation = 3,
        MiddleClickActivation = 4,
    };
    Q_ENUM(ActivationReason)

    explicit TrayAdapter(QObject* parent = nullptr);

    // ---- inputs, from the application -----------------------------------
    void setActive(bool active);
    // `pulse_frame` indexes the recording heartbeat and is ignored in every state
    // but Recording.
    void setPresence(const ShellPresenceState& state, const QString& elapsed_text, int pulse_frame);
    // The elapsed clock moves on the metrics cadence without the state changing,
    // and the tooltip is the surface that shows it.
    void setElapsedText(const QString& elapsed_text);
    // Ids from ui/theme/ExoSnapThemes.h. The mark follows the application's
    // palette, so changing the accent repaints the tray with no restart.
    void setAppearance(const QString& appearance_id, const QString& accent_id);
    // The raster the notification area will actually use, in device pixels.
    // Rendering at any other size means the shell rescales, which is what the
    // optical profiles exist to avoid.
    void setIconPixelSize(int px);
    void setWindowVisible(bool visible);

    void incrementUnreadCount();
    void clearUnreadCount();

    // ---- what QML binds to ----------------------------------------------
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] QString iconSource() const;
    [[nodiscard]] QString tooltip() const;
    [[nodiscard]] QString showHideText() const;
    [[nodiscard]] QVariantMap recordItem() const;
    [[nodiscard]] QVariantMap pauseResumeItem() const;
    [[nodiscard]] QVariantMap stopItem() const;
    [[nodiscard]] int unreadCount() const noexcept;
    [[nodiscard]] bool notificationsVisible() const noexcept;
    [[nodiscard]] QString notificationsText() const;

    // ---- what QML calls back ---------------------------------------------
    // Re-checked against the appearance table before anything is emitted: a menu
    // can be triggered by an accelerator between the state change and the
    // repaint, and the row that drew itself is not necessarily the row that is
    // true now.
    Q_INVOKABLE void triggerTransport(TransportRow row);
    Q_INVOKABLE void triggerShowHide();
    Q_INVOKABLE void triggerNotifications();
    Q_INVOKABLE void triggerOpenOutputFolder();
    Q_INVOKABLE void triggerQuit();
    Q_INVOKABLE void handleActivation(int reason);

    // Read-only introspection for tests.
    [[nodiscard]] ShellIconState currentIconState() const noexcept;
    [[nodiscard]] int currentPulseFrame() const noexcept;

  signals:
    void activeChanged();
    void appearanceChanged();
    void unreadCountChanged();

    // The window is wanted on screen -- the menu entry while it reads "Show
    // window", a left click on the icon, or the notifications entry.
    void activateWindowRequested();
    // The same menu entry under its other label. Its own signal because that
    // entry used to offer to hide a window and then show it.
    void hideWindowRequested();
    // A transport entry was chosen, carrying the intent the appearance table
    // resolved. The same signal the thumbnail buttons raise.
    void shellActionRequested(ShellAction action);
    // A double click, which is "toggle recording" rather than a specific
    // transport action -- the gesture has no state to read.
    void recordToggleRequested();
    void openOutputFolderRequested();
    void quitRequested();

  private:
    [[nodiscard]] QVariantMap rowFor(ShellButton button, bool keep_visible_when_disabled) const;

    ShellPresenceState state_;
    QString elapsed_text_;
    QString appearance_id_;
    QString accent_id_;
    int icon_px_ = 16;
    int pulse_frame_ = 0;
    bool active_ = false;
    bool window_visible_ = true;
    int unread_count_ = 0;
};

} // namespace exosnap::quick

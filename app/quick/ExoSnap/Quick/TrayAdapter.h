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
//
// There is deliberately no "Open last recording" row: the Saved toast and the
// Record page already offer Edit for the finished recording, and a menu row fed
// from session state would be empty after every restart.

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include "models/ShellPresence.h"
#include "ui/brand/ShellIconRenderer.h"

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

    // An image-provider URL carrying the mark, the size, the animation frame and
    // the resolved palette. It changes only when one of those does, which is what
    // keeps Qt Quick's pixmap cache doing the work instead of the painter.
    Q_PROPERTY(QString iconSource READ iconSource NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString tooltip READ tooltip NOTIFY appearanceChanged FINAL)

    // Why a start is refused, when something knows. Offered only in the blocked
    // phase and only with a reason to give: a caption row that appears empty
    // reads as a broken menu. A native popup menu has no header item, so there
    // is deliberately no status caption above it -- the icon and its tooltip
    // already carry the session's state.
    Q_PROPERTY(bool blockedReasonVisible READ blockedReasonVisible NOTIFY appearanceChanged FINAL)
    // "Cannot record: <reason>", composed once here rather than in QML so the
    // capitalisation rule has one place to live. Drawn disabled, with the
    // caution glyph below, because it is information rather than something to
    // click.
    Q_PROPERTY(QString blockedReason READ blockedReason NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString blockedReasonIcon READ blockedReasonIcon NOTIFY appearanceChanged FINAL)

    // The non-transport entries' glyphs. Constant shapes, but not constant URLs:
    // they carry the palette, so a theme change repaints them with everything
    // else. A menu where three rows have an icon and four do not reads as three
    // unfinished rows.
    Q_PROPERTY(QString showWindowIcon READ showWindowIcon NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString outputFolderIcon READ outputFolderIcon NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString notificationsIcon READ notificationsIcon NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QString quitIcon READ quitIcon NOTIFY appearanceChanged FINAL)

    // One transport row each: `{ visible, enabled, text, icon }`. Assembled from
    // the appearance table, not from the recording state. `visible` is always
    // true -- see rowFor() for why the menu keeps a fixed shape where the
    // thumbnail strip does not.
    Q_PROPERTY(QVariantMap recordItem READ recordItem NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QVariantMap pauseResumeItem READ pauseResumeItem NOTIFY appearanceChanged FINAL)
    Q_PROPERTY(QVariantMap stopItem READ stopItem NOTIFY appearanceChanged FINAL)

    // The unread mirror for toasts raised while the window was not on screen.
    Q_PROPERTY(int unreadCount READ unreadCount NOTIFY unreadCountChanged FINAL)
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
    // `mark_frame` indexes whichever animated mark the state shows, and is
    // ignored by a static one.
    void setPresence(const ShellPresenceState& state, const QString& elapsed_text, int mark_frame);
    // The elapsed clock moves on the metrics cadence without the state changing,
    // and the tooltip is the surface that shows it.
    void setElapsedText(const QString& elapsed_text);
    // Empty when nothing is known, which is not the same as not being blocked:
    // the phase decides whether the row exists at all.
    void setBlockedReason(const QString& reason);
    // Ids from ui/theme/ExoSnapThemes.h. The mark follows the application's
    // palette, so changing the accent repaints the tray with no restart.
    void setAppearance(const QString& appearance_id, const QString& accent_id);
    // The raster the notification area will actually use, in device pixels.
    // Rendering at any other size means the shell rescales, which is what the
    // optical profiles exist to avoid.
    void setIconPixelSize(int px);

    void incrementUnreadCount();
    void clearUnreadCount();

    // ---- what QML binds to ----------------------------------------------
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] QString iconSource() const;
    [[nodiscard]] QString tooltip() const;
    [[nodiscard]] bool blockedReasonVisible() const;
    [[nodiscard]] QString blockedReason() const;
    [[nodiscard]] QString blockedReasonIcon() const;
    [[nodiscard]] QString showWindowIcon() const;
    [[nodiscard]] QString outputFolderIcon() const;
    [[nodiscard]] QString notificationsIcon() const;
    [[nodiscard]] QString quitIcon() const;
    [[nodiscard]] QVariantMap recordItem() const;
    [[nodiscard]] QVariantMap pauseResumeItem() const;
    [[nodiscard]] QVariantMap stopItem() const;
    [[nodiscard]] int unreadCount() const noexcept;
    [[nodiscard]] QString notificationsText() const;

    // ---- what QML calls back ---------------------------------------------
    // Re-checked against the appearance table before anything is emitted: a menu
    // can be triggered by an accelerator between the state change and the
    // repaint, and the row that drew itself is not necessarily the row that is
    // true now.
    Q_INVOKABLE void triggerTransport(TransportRow row);
    Q_INVOKABLE void triggerShowWindow();
    Q_INVOKABLE void triggerNotifications();
    Q_INVOKABLE void triggerOpenOutputFolder();
    Q_INVOKABLE void triggerQuit();
    Q_INVOKABLE void handleActivation(int reason);

    // Read-only introspection for tests.
    [[nodiscard]] ShellIconState currentIconState() const noexcept;
    [[nodiscard]] int currentMarkFrame() const noexcept;

    // "Cannot record: <reason>", with the reason's first letter lowercased into
    // the sentence unless the reason already reads as its own sentence about
    // recording -- lowercasing that one would stutter ("record: recording is
    // blocked..."). A trailing period is stripped either way, since the result
    // always ends the row's own sentence. Static and pure so QML never has to
    // reimplement the rule, and so it is testable with no TrayAdapter instance.
    [[nodiscard]] static QString ComposeBlockedReasonSentence(const QString& reason);

  signals:
    void activeChanged();
    void appearanceChanged();
    void unreadCountChanged();

    // The window is wanted on screen -- the "Show window" entry, a click or
    // double click on the icon, or the notifications entry. The handler raises
    // and activates an already visible window, so the entry means the same thing
    // in every state. Toggling a recording stays with the hotkey and the menu: a
    // double click is too easy to produce while reaching for the window to be
    // allowed to start or stop one.
    void activateWindowRequested();
    // A transport entry was chosen, carrying the intent the appearance table
    // resolved. The same signal the thumbnail buttons raise.
    void shellActionRequested(ShellAction action);
    void openOutputFolderRequested();
    void quitRequested();

  private:
    // The tooltip's second half: one phrase naming the session's state. No
    // longer a property -- the menu no longer has a caption row to share it
    // with -- but the tooltip still composes itself from exactly this, so the
    // two cannot drift apart.
    [[nodiscard]] QString statusText() const;

    // `fallback_action` names the row when the appearance table has none to give
    // -- a row the table hides still has to say what it is.
    [[nodiscard]] QVariantMap rowFor(ShellButton button, ShellAction fallback_action) const;
    [[nodiscard]] QString glyphUrl(ui::brand::ShellGlyph glyph) const;
    // A native menu has no padding property: Windows sizes a row to fit the
    // larger of its text and its item bitmap plus margins, so a taller glyph is
    // the only lever that gives a row more air. Menu glyphs render a few pixels
    // taller than the notification-area mark for exactly that reason -- the mark
    // itself stays at icon_px_, the shell's own metric, or the shell rescales it.
    [[nodiscard]] int menuGlyphPixelSize() const noexcept;

    ShellPresenceState state_;
    QString elapsed_text_;
    QString blocked_reason_;
    QString appearance_id_;
    QString accent_id_;
    int icon_px_ = 16;
    int mark_frame_ = 0;
    bool active_ = false;
    int unread_count_ = 0;
};

} // namespace exosnap::quick

#pragma once

#include "NotificationEntryModel.h"
#include "NotificationToastModel.h"
#include "notifications/NotificationManager.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QRect>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

namespace exosnap::quick {

// Narrow QML boundary for the notification bell + hub (product-spec §9).
//
// ── Ownership ────────────────────────────────────────────────────────────
// This adapter OWNS a notifications::NotificationManager instance — built
// here with std::make_unique and given NO Qt parent, exactly like
// DiagnosticsAdapter owns its SupportBundleService (see
// DiagnosticsAdapter.cpp), so lifetime is the unique_ptr's alone and never
// contested with QObject parent/child deletion. Three reasons this is
// ownership rather than a borrowed reference:
//
//  1. Every event source that needs to reach a manager (recording result,
//     drops, audio degraded, hotkey conflict, recovery available, update
//     available, settings repaired/save-failed, capture-action-failed —
//     the same list MainWindow::initNotificationToasts() wires) needs
//     exactly one instance to Enqueue() into. This adapter is the natural,
//     already-mandated construction site for the Quick frontend's copy of
//     that instance, rather than inventing a second one in the composition
//     root and wiring this adapter to it after the fact.
//  2. NotificationManager itself declares "No Win32 / window code here —
//     fully unit-testable" — owning it inside the QML boundary layer does
//     not leak any platform dependency into that layer, it only adds signal
//     plumbing, which is exactly what an adapter is for.
//  3. Whatever renders the actual toast WINDOW (OverlayNotificationToast.qml,
//     which carries the spec rules for a card in its header) needs the
//     SAME manager instance to read VisibleEvents() / ShownAtMs() and call
//     Dismiss() on a click. manager() below hands out that one instance by
//     reference so a future toast-window owner and this adapter's hub model
//     can never observe two different notion of "what's currently visible".
//
// The hub model is filled ONLY by this adapter's own connection to
// manager().eventRecorded() — nothing else may push entries into it — so the
// hub history and the manager's own bookkeeping can never drift apart.
//
// Action dispatch is explicitly NOT this adapter's job: triggerAction() looks
// up the row's stored NotificationAction/payload and re-emits it as
// actionTriggered(), the same "*Requested signal, composition root performs
// it" contract every other adapter in this directory uses (see
// RecordViewModelAdapter::startRequested() and its siblings). Keeping
// navigation, Explorer/QDesktopServices calls, and the elevated-relaunch path
// out of this file keeps the QML boundary layer free of platform code — the
// Quick-side equivalent of MainWindow::dispatchNotificationAction() belongs
// in the composition root, not here.
class NotificationsAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("NotificationsAdapter is provided by the application")

    // Declared as the Qt base type: qmltyperegistrar records the concrete
    // subclass under its namespaced C++ name while moc writes it unqualified,
    // so a concrete spelling here is unresolvable for qmllint (same note as
    // LogsAdapter::model / DiagnosticsAdapter::issues).
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT FINAL)
    // The transient glance: exactly what the manager currently shows. Feeds the
    // out-of-window toast stack, which is a separate top-level window so a toast
    // about a finished recording stays visible over a fullscreen game.
    Q_PROPERTY(QAbstractItemModel* toastModel READ toastModel CONSTANT FINAL)
    // Available geometry (taskbar excluded) of the screen hosting the app
    // window. Resolved in C++ and re-published when the window changes screen:
    // QML's Screen attached object exposes no available-area ORIGIN, so a
    // top- or left-docked taskbar would push the stack off the work area.
    Q_PROPERTY(QRect toastAnchorGeometry READ toastAnchorGeometry NOTIFY toastAnchorChanged FINAL)

    Q_PROPERTY(int unreadCount READ unreadCount NOTIFY unreadChanged FINAL)
    // "" when nothing is unread; otherwise "success" | "caution" | "error" |
    // "info" — the worst rung per notifications::AdvisoryStatusRank. Per
    // product-spec §9 the bell paints a severity DOT from this, never the
    // unreadCount number itself.
    Q_PROPERTY(QString worstUnreadTone READ worstUnreadTone NOTIFY unreadChanged FINAL)
    Q_PROPERTY(bool hubOpen READ hubOpen NOTIFY hubOpenChanged FINAL)
    Q_PROPERTY(bool hasEntries READ hasEntries NOTIFY entriesChanged FINAL)

  public:
    explicit NotificationsAdapter(QObject* parent = nullptr);

    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] QAbstractItemModel* toastModel() noexcept;
    [[nodiscard]] const QRect& toastAnchorGeometry() const noexcept;
    [[nodiscard]] int unreadCount() const noexcept;
    [[nodiscard]] QString worstUnreadTone() const;
    [[nodiscard]] bool hubOpen() const noexcept;
    [[nodiscard]] bool hasEntries() const noexcept;

    Q_INVOKABLE void openHub();
    Q_INVOKABLE void closeHub();
    Q_INVOKABLE void toggleHub();
    // "Mark all read" (widget precedent: the header label in
    // NotificationHubPanel — visually present there but never actually wired
    // to a slot; this is the first working implementation of it).
    Q_INVOKABLE void markAllRead();
    Q_INVOKABLE void dismissEntry(int index);
    Q_INVOKABLE void dismissAll();
    // Re-emits row `index`'s NotificationAction as actionTriggered(), then
    // marks that row read. `action` must equal either the row's
    // primaryAction or its secondary entry's action value (see the `actions`
    // role on NotificationEntryModel) — anything else, including None, is
    // silently ignored, so a stale QML delegate can never replay an action
    // its row no longer offers.
    Q_INVOKABLE void triggerAction(int index, int action);

    // ── Toast stack ────────────────────────────────────────────────────────
    // Re-emits a visible toast's action, addressed by the toast's sequence
    // rather than a row index: the manager can retire a card between the click
    // and this call, and a row index would then point at whatever slid into its
    // place. Ignored when the sequence is no longer visible, or when `action`
    // is not one the event actually offers.
    Q_INVOKABLE void triggerToastAction(qint64 sequence, int action);
    Q_INVOKABLE void dismissToast(qint64 sequence);

    // Set by the composition root from the app window's screen.
    void setToastAnchorGeometry(const QRect& geometry);

    // ── Host-side (C++-only) wiring surface — never exposed to QML ─────────

    // The manager every event source enqueues into. See the class doc
    // comment for why this adapter owns it and hands it out by reference.
    [[nodiscard]] notifications::NotificationManager& manager() noexcept;

    // UpdateAvailable's hub entry cannot clear itself on a later "up to date"
    // result — no NotificationEvent is raised for that outcome, only for
    // finding an update. The composition root calls this directly from that
    // result path, mirroring MainWindow::onUpdateCheckComplete()'s
    // removeAdvisoryById(QStringLiteral("update-available")). Returns false
    // if no entry with that key is currently recorded.
    bool removeEntryByKey(const QString& key);

  signals:
    void unreadChanged();
    void hubOpenChanged();
    void entriesChanged();
    void toastAnchorChanged();

    // The routed action, dispatched nowhere inside this adapter (see class
    // doc comment). `payload` is the triggering event's action_payload
    // (e.g. an output file path for Edit/OpenFolder/ShowFile) — the same
    // single field MainWindow::dispatchNotificationAction() reads for every
    // case except UndoPresetSwitch, which needs no payload because the
    // composition root tracks its own pending-undo state exactly as
    // MainWindow does today.
    void actionTriggered(exosnap::notifications::NotificationAction action, const QString& payload);

  private:
    void onEventRecorded(const notifications::NotificationEvent& event);

    std::unique_ptr<notifications::NotificationManager> manager_;
    NotificationEntryModel model_;
    NotificationToastModel toast_model_;
    QRect toast_anchor_geometry_;
    bool hub_open_ = false;
};

} // namespace exosnap::quick

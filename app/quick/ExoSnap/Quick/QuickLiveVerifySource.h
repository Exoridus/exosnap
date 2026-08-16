#pragma once

// QuickLiveVerifySource.h -- binds the Live Verify control channel to the real
// running application.
//
// Every intent below routes through the Q_INVOKABLE request methods the QML
// surfaces call -- RecordViewModelAdapter's transport requests, the shell's one
// navigation edge, EditSessionAdapter's clamping seek/trim, the notification
// adapter's hub. That is the point: an acceptance check that reached
// RecordingCoordinator directly would prove the engine works while saying
// nothing about the product, and would keep passing after the button stopped
// being wired to it. The control channel observes and drives product semantics;
// it never owns any.
//
// It owns no preconditions either. State() answers what the product state IS,
// LiveVerifyCommandPolicy decides from that what may run, and the same answer
// feeds availableActions -- so the channel cannot report an action as available
// and then refuse it.
//
// The one exception, and it is a narrow one, is window.moveToScreen: placing a
// window on a screen is not an intent the product exposes to users at all. It
// exists because the cross-monitor Preview defect needs a boundary crossing, and
// synthesising a mouse drag is ruled out (CLAUDE.md). The human drag stays a
// manual gate precisely because this is not the same thing.
//
// A QObject because the revision counter has to observe: it recomputes the
// automation state whenever one of the signals behind it fires and advances only
// when the state actually differs. Not "increment on every notify" -- the record
// adapter's aggregated `changed` fires on every elapsed-time tick, and a
// revision that moved with the clock would be a number a runner could never wait
// on.

#include "live_verify/LiveVerifyAutomationState.h"
#include "live_verify/LiveVerifySource.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>

class QQuickWindow;

namespace exosnap::quick {

class QuickApplication;

class QuickLiveVerifySource final : public QObject, public live_verify::LiveVerifySource {
    Q_OBJECT

  public:
    QuickLiveVerifySource(QuickApplication& application, QQuickWindow* root_window, QObject* parent = nullptr);

    [[nodiscard]] QJsonObject Identity() const override;
    [[nodiscard]] live_verify::AutomationState State() const override;
    [[nodiscard]] std::uint64_t StateRevision() const override;
    [[nodiscard]] QJsonObject SystemSnapshot() const override;
    [[nodiscard]] QJsonObject AppSnapshot() const override;
    [[nodiscard]] QJsonObject WindowSnapshot() const override;
    [[nodiscard]] QJsonObject PreviewSnapshot() const override;
    [[nodiscard]] QJsonObject RecordSnapshot() const override;
    [[nodiscard]] QJsonObject RecordResult() const override;
    [[nodiscard]] QJsonObject OverlaySnapshot() const override;
    [[nodiscard]] QJsonObject EditorSnapshot() const override;
    [[nodiscard]] QJsonObject DiagnosticsSnapshot() const override;

    bool MoveWindowToScreen(const QString& screen_name, QString* error) override;
    bool SelectRecordTarget(const QString& kind, const QString& title_filter, QString* error) override;
    bool RecordStart(QString* error) override;
    bool RecordPause(QString* error) override;
    bool RecordResume(QString* error) override;
    bool RecordStop(QString* error) override;
    bool RecordSplit(QString* error) override;
    bool RecordCaptureFrame(QString* error) override;

    bool Navigate(const QString& page, QString* error) override;
    [[nodiscard]] RevealOutcome Reveal(const QString& surface, const QString& target, QString* error) override;
    bool ScrollHome(const QString& surface, QString* error) override;
    bool ScrollEnd(const QString& surface, QString* error) override;
    bool SetSourcePickerOpen(bool open, QString* error) override;
    bool SetNotificationHubOpen(bool open, QString* error) override;
    bool ClearNotifications(QString* error) override;

    bool EditOpen(QString* error) override;
    bool EditPlayPause(QString* error) override;
    bool EditSeek(qint64 position_ms, QString* error) override;
    bool EditSetTrimIn(qint64 position_ms, QString* error) override;
    bool EditSetTrimOut(qint64 position_ms, QString* error) override;
    bool EditTimelineHome(QString* error) override;
    bool EditTimelineEnd(QString* error) override;
    bool EditClose(QString* error) override;

  signals:
    // The observable state changed, and the revision has already been advanced.
    // main.cpp turns this into the ui.stateChanged event -- the protocol-2
    // settle signal for everything that has no domain event of its own, the
    // blocking surfaces above all.
    void observableStateChanged();

  private:
    // Recomputes State() and advances the revision only if it differs. Connected
    // to every signal that can move one of the fields; extra connections are
    // therefore harmless, and a missing one is the only real failure mode.
    void refreshObservableState();
    // The page-scoped QML object a reveal/scroll command addresses, or null when
    // that page is not loaded. Constant object names only -- a client-supplied
    // string never reaches findChild().
    [[nodiscard]] QObject* pageObjectFor(const QString& surface) const;

    QuickApplication& application_;
    QPointer<QQuickWindow> root_window_;
    live_verify::AutomationState observed_;
    std::uint64_t revision_ = 0;
    // The executable hash is the artifact binding, and hashing a ~40 MB image on
    // every hello would put a visible cost into a channel that must not perturb
    // what it measures. Computed once, on first use.
    mutable QString executable_sha256_;
};

} // namespace exosnap::quick

#pragma once

#include "services/RecoveryService.h"
#include "settings/RecoveryManifestStore.h"

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVector>
#include <QtQmlIntegration/qqmlintegration.h>

#include <atomic>

namespace exosnap::quick {

// Rows of the recovery surface. Presentation-ready strings only: the size, the
// date and the container are formatted here so QML never has to reason about
// bytes or ISO-8601 — and never has to reach into a manifest entry.
class RecoveryCandidateModel : public QAbstractListModel {
    Q_OBJECT
  public:
    enum Roles {
        DisplayNameRole = Qt::UserRole + 1,
        MetaRole,        // "412 MB · 2026-08-10 · MKV"
        CanContinueRole, // false for finalized entries — see below
        StatusRole,      // inline status/error text, empty when idle
        StatusIsErrorRole,
        BusyRole,     // this row's Finish is running
        ProgressRole, // 0.0 .. 1.0 while busy
        ArmedDiscardRole,
    };

    explicit RecoveryCandidateModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void setCandidates(QVector<RecoveryCandidate> candidates);
    void removeAt(int index);
    [[nodiscard]] const QVector<RecoveryCandidate>& candidates() const noexcept;
    [[nodiscard]] bool contains(int index) const noexcept;

    void setStatus(int index, const QString& text, bool is_error);
    void setBusy(int index, bool busy);
    void setProgress(int index, double progress);
    void setArmedDiscard(int index, bool armed);
    void clearArmedDiscard();

  private:
    struct RowState {
        QString status;
        bool status_is_error = false;
        bool busy = false;
        double progress = 0.0;
        bool armed_discard = false;
    };

    void emitRowChanged(int index, const QList<int>& roles);

    QVector<RecoveryCandidate> candidates_;
    QVector<RowState> states_;
};

// Narrow QML boundary for startup recovery (ADR-0014/ADR-0015).
//
// Everything that decides anything stays behind this class: the scan, what
// counts as a live candidate, which action a candidate may offer, whether a
// destructive action has been confirmed, and what the artefact becomes. QML gets
// rows and four verbs.
//
// The one action this adapter deliberately does NOT perform is "Continue".
// Resuming into an armed session is the recording coordinator's business, and
// this adapter holds no service references beyond RecoveryService — so it
// re-emits the request and the composition root arms the coordinator, the same
// "*Requested signal, composition root performs it" contract every other adapter
// in this directory follows.
class RecoveryAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("RecoveryAdapter is provided by the application")

    // Declared as the Qt base type for the same reason LogsAdapter::model is:
    // qmltyperegistrar records the concrete subclass under its namespaced C++
    // name while moc writes it unqualified.
    Q_PROPERTY(QAbstractItemModel* model READ model CONSTANT FINAL)
    Q_PROPERTY(bool hasCandidates READ hasCandidates NOTIFY candidatesChanged FINAL)
    Q_PROPERTY(int candidateCount READ candidateCount NOTIFY candidatesChanged FINAL)
    // Whether the surface is currently up. "Decide later" lowers it without
    // resolving anything; the notification action raises it again.
    Q_PROPERTY(bool surfaceOpen READ surfaceOpen NOTIFY surfaceOpenChanged FINAL)
    // An action is running. One at a time, application-wide: a repair remux is
    // I/O-bound and two of them racing over the same output folder is not a
    // state the user asked for.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged FINAL)

  public:
    explicit RecoveryAdapter(QObject* parent = nullptr);
    ~RecoveryAdapter() override;

    // `service` must outlive this adapter. Absent (the default) makes every verb
    // a no-op, which is what keeps a frontend without recovery wiring inert
    // rather than crashing.
    void setService(RecoveryService* service);

    [[nodiscard]] QAbstractItemModel* model() noexcept;
    [[nodiscard]] bool hasCandidates() const noexcept;
    [[nodiscard]] int candidateCount() const noexcept;
    [[nodiscard]] bool surfaceOpen() const noexcept;
    [[nodiscard]] bool busy() const noexcept;

    // Re-reads the manifest through the service, dropping entries whose artefact
    // is gone. Returns the surviving count. Called once at startup by the
    // composition root; safe to call again.
    int scan();

    // Harness-only (--overlay-visual-state). Seeds rows without a manifest so a
    // deterministic capture never depends on this machine having crashed. The
    // actions stay wired to the real service, which is absent in that mode, so
    // nothing here can delete or rewrite a file.
    void seedCandidatesForVisualHarness(QVector<RecoveryCandidate> candidates);

    Q_INVOKABLE void openSurface();
    // "Decide later": lowers the surface, resolves nothing. Entries stay in the
    // manifest and are offered again on the next launch.
    Q_INVOKABLE void dismiss();

    // Saves the recording as originally configured (rename or repair-remux,
    // decided by the manifest snapshot, not here). Runs off the GUI thread;
    // progress and the result arrive back as row state.
    Q_INVOKABLE void finish(int index);
    // Arms the inline confirm for a destructive delete. A second call to
    // discard() within the armed state performs it; anything else disarms.
    Q_INVOKABLE void armDiscard(int index);
    Q_INVOKABLE void disarmDiscard();
    // Deletes the artefact and the manifest entry. Refuses unless armDiscard()
    // armed this exact row — the confirmation is policy, so it is enforced here
    // rather than trusted to the delegate that drew the button.
    Q_INVOKABLE void discard(int index);
    // Re-emits continueRequested for the composition root. Only offered for
    // non-finalized entries: a finalized one is a deliberate stop whose remux
    // failed, and there is nothing left to record into it.
    Q_INVOKABLE void continueSession(int index);
    // Cancels a running Finish. The artefact and the entry are preserved.
    Q_INVOKABLE void cancelAction();

  signals:
    void candidatesChanged();
    void surfaceOpenChanged();
    void busyChanged();
    void continueRequested(const exosnap::RecoveryManifestEntry& entry);

  private:
    void setSurfaceOpen(bool open);
    void setBusyIndex(int index);
    void onFinishComplete(int index, bool success, const QString& message);

    RecoveryService* service_ = nullptr;
    RecoveryCandidateModel model_;
    bool surface_open_ = false;
    int busy_index_ = -1;
    int armed_discard_index_ = -1;
    // Read from the worker thread, written from the GUI thread.
    std::atomic<bool> cancel_requested_{false};

    // Declared LAST so it is destroyed FIRST: its destructor waits for an
    // in-flight Finish, whose lambda captures `this` and touches the members
    // above. A pool that outlived them would be a use-after-free on shutdown
    // during a repair remux.
    QThreadPool finish_pool_;
};

} // namespace exosnap::quick

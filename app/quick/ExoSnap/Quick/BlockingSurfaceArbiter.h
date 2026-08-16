#pragma once

#include <QObject>

#include <optional>
#include <vector>

namespace exosnap::quick {

class CrashReportAdapter;
class RecordingErrorAdapter;
class RecoveryAdapter;

// Which of the three blocking surfaces may be up right now.
//
// Recovery, the crash-consent prompt and the recording-error surface are all
// full-window modal surfaces, and any of them can be asked for while another is
// already on screen:
//
//   - startup finds recovery candidates AND a crash sidecar;
//   - the crash prompt is up and the user triggers the standing
//     "Recover last session?" notification (a desktop toast is a separate
//     always-on-top window, so it stays clickable behind a modal scrim);
//   - recovery is up and the deferred crash prompt comes due;
//   - either startup surface is up and the global start hotkey — which is
//     deliberately desktop-wide — starts a recording that then fails.
//
// Their QML loaders are independent and z-ordered, which decides what is drawn
// on top but not who owns the keyboard: two active modal loaders means the
// hidden one still holds focusable controls. So the decision is made here,
// once, instead of by each caller checking the others' `visible`.
//
// The contract:
//   - at most one of the three is raised at any time;
//   - the surface that is ALREADY up keeps precedence — nothing supersedes it,
//     because a modal that vanishes mid-read took its question with it;
//   - the request that loses is QUEUED, not dropped — a crash prompt, a recovery
//     offer or a failure report that silently disappears is information the user
//     cannot get back without restarting;
//   - the queue is FIFO and each entry is single-shot: a surface is raised when
//     the ones ahead of it have come down, and never again after that.
//
// The crash and recording-error surfaces are emitted as requests rather than
// raised directly: only the composition root can build the previous session's
// crash context, and only it holds the failure report the error surface shows.
class BlockingSurfaceArbiter : public QObject {
    Q_OBJECT

  public:
    explicit BlockingSurfaceArbiter(QObject* parent = nullptr);

    enum class Surface { Recovery, Crash, RecordingError };

    // All must outlive this object. A null adapter makes its own surface inert
    // (never raised, never queued), which keeps a frontend that wires only some
    // of them working.
    void setSurfaces(RecoveryAdapter* recovery, CrashReportAdapter* crash, RecordingErrorAdapter* recording_error);

    // The recovery surface wants to be up: the startup scan found candidates,
    // or the standing notification's action asked for it again.
    void requestRecovery();
    // The crash-consent prompt wants to be up. Answered by crashSurfaceRequested()
    // when it is this surface's turn.
    void requestCrash();
    // A recording just failed. Answered by recordingErrorSurfaceRequested() when
    // it is this surface's turn; the caller keeps the report until then.
    void requestRecordingError();

    [[nodiscard]] bool recoveryQueued() const noexcept;
    [[nodiscard]] bool crashQueued() const noexcept;
    [[nodiscard]] bool recordingErrorQueued() const noexcept;

    // Whether any blocking surface is on screen right now. Read by the one
    // admission edge outside this class: a recording must not START under a
    // modal that covers the transport the user would need to control it, for the
    // same reason the navigation shortcuts are inactive there. Stopping, pausing
    // and resuming keep working — those are desktop-wide by contract, and a
    // running recording the user cannot stop is the worse state.
    [[nodiscard]] bool anySurfaceUp() const;

    // Which one is up, for the automation state snapshot. Answered from the same
    // isUp() the admission edge above reads, so the control channel can never
    // report a surface the product does not consider raised — reconstructing it
    // from the three adapters separately is exactly the second source of truth
    // this class exists to prevent.
    [[nodiscard]] std::optional<Surface> activeSurface() const;

  signals:
    // Raise the crash surface now — build the context and call
    // CrashReportAdapter::present().
    void crashSurfaceRequested();
    // Raise the recording-error surface now — call
    // RecordingErrorAdapter::present() with the report that was held back.
    void recordingErrorSurfaceRequested();

  private:
    void request(Surface surface);
    void raise(Surface surface);
    void onSurfaceStateChanged();
    [[nodiscard]] bool isUp(Surface surface) const;
    [[nodiscard]] bool available(Surface surface) const;
    [[nodiscard]] bool anyUp() const;
    [[nodiscard]] bool queued(Surface surface) const noexcept;

    RecoveryAdapter* recovery_ = nullptr;
    CrashReportAdapter* crash_ = nullptr;
    RecordingErrorAdapter* recording_error_ = nullptr;
    // FIFO, deduplicated. Small and fixed by construction — there are three
    // surfaces — so a vector is the whole data structure.
    std::vector<Surface> queue_;
    // Guards the re-entry a raise causes: raising one surface emits the very
    // signal this class listens to.
    bool dispatching_ = false;
};

} // namespace exosnap::quick

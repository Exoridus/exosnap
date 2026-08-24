#pragma once

// Who owns the taskbar progress bar, and for how long.
//
// Three long operations publish a fraction -- the MP4 remux on stop, the
// recovery finish, and the edit export -- and there is exactly one bar. A
// first-producer-wins rule with no way back leaves the bar stuck on whichever
// operation happened to run first in a session, so ownership is a LEASE: taken,
// held, and given back on every way an operation can end, including the ones
// that end badly.
//
// The generation is what makes a stale callback identifiable. A remux that
// reports 80% after its own session was torn down and a new export acquired the
// bar must not move that export's bar, and the pointer-shaped question ("is this
// still the owner?") cannot tell the two apart when the same operation kind runs
// twice.
//
// Pure: no COM, no window, no timers. The adapter above it turns a publication
// into ITaskbarList3 calls.

#include <QtGlobal>

namespace exosnap {

// Mirrors the TBPF_* set ITaskbarList3::SetProgressState takes.
enum class TaskbarProgressState {
    NoProgress,
    Indeterminate,
    Normal,
    Error,
};

// The fixed set of producers. An enum rather than an opaque token because the
// log line has to name which operation owns the bar, and "0x7ffd..." does not.
enum class TaskbarProgressOwner {
    None,
    RecordingSave,
    RecoveryFinish,
    EditExport,
};

// A lease handle. Worthless on its own -- the ledger checks both halves.
struct TaskbarProgressLease {
    TaskbarProgressOwner owner = TaskbarProgressOwner::None;
    quint64 generation = 0;

    [[nodiscard]] bool valid() const noexcept {
        return owner != TaskbarProgressOwner::None && generation != 0;
    }
};

[[nodiscard]] bool operator==(const TaskbarProgressLease& lhs, const TaskbarProgressLease& rhs) noexcept;
[[nodiscard]] bool operator!=(const TaskbarProgressLease& lhs, const TaskbarProgressLease& rhs) noexcept;

class TaskbarProgressLedger {
  public:
    // Takes the bar. Returns an invalid lease when another producer already
    // holds it -- the caller then publishes nothing, rather than interleaving
    // fractions into a bar that would jump backwards. The refusal is the
    // caller's to log; the ledger has no opinion about it.
    [[nodiscard]] TaskbarProgressLease acquire(TaskbarProgressOwner owner) noexcept;

    // Publishes a fraction in [0, 1]. Returns whether the published state
    // actually changed: producers report far more often than a taskbar bar can
    // show, so the gate is a whole percent. A no-op from a stale lease also
    // returns false, which is the same answer for a different reason -- nothing
    // downstream needs to tell them apart.
    bool update(const TaskbarProgressLease& lease, double fraction) noexcept;

    // For a phase that runs without a meaningful fraction.
    bool setIndeterminate(const TaskbarProgressLease& lease) noexcept;

    // The three ways an operation ends. All of them release the lease: an owner
    // that keeps it past its own end is the stuck-bar defect in slow motion.
    // `fail` leaves the bar in TBPF_ERROR, which stays until the next acquire --
    // an error the user never sees is not a report.
    bool fail(const TaskbarProgressLease& lease) noexcept;
    bool finish(const TaskbarProgressLease& lease) noexcept;
    bool cancel(const TaskbarProgressLease& lease) noexcept;

    // Teardown. Same as cancel for the bar, separate for the log: a producer
    // that disappeared is not a user who pressed Cancel.
    bool release(const TaskbarProgressLease& lease) noexcept;

    [[nodiscard]] TaskbarProgressState state() const noexcept;
    // The last published fraction, in [0, 1]. Meaningless unless state() is
    // Normal.
    [[nodiscard]] double fraction() const noexcept;
    [[nodiscard]] TaskbarProgressOwner owner() const noexcept;
    [[nodiscard]] bool held() const noexcept;

  private:
    [[nodiscard]] bool holds(const TaskbarProgressLease& lease) const noexcept;
    bool endLease(const TaskbarProgressLease& lease, TaskbarProgressState final_state) noexcept;

    TaskbarProgressOwner owner_ = TaskbarProgressOwner::None;
    quint64 generation_ = 0;
    quint64 next_generation_ = 1;
    TaskbarProgressState state_ = TaskbarProgressState::NoProgress;
    double fraction_ = 0.0;
};

} // namespace exosnap

#include "models/TaskbarProgressLease.h"

#include <algorithm>
#include <cmath>

namespace exosnap {

namespace {

// The bar is a few hundred pixels wide and Explorer repaints the whole taskbar
// button for each SetProgressValue, while a remux reports every muxed packet.
// A whole percent is the finest step the bar can actually show.
[[nodiscard]] int WholePercent(double fraction) noexcept {
    return static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0));
}

} // namespace

bool operator==(const TaskbarProgressLease& lhs, const TaskbarProgressLease& rhs) noexcept {
    return lhs.owner == rhs.owner && lhs.generation == rhs.generation;
}

bool operator!=(const TaskbarProgressLease& lhs, const TaskbarProgressLease& rhs) noexcept {
    return !(lhs == rhs);
}

TaskbarProgressLease TaskbarProgressLedger::acquire(TaskbarProgressOwner owner) noexcept {
    if (owner == TaskbarProgressOwner::None)
        return {};
    // Refused rather than pre-empted. Two producers sharing one bar make it jump
    // backwards, and the second operation is still perfectly visible in its own
    // surface -- the bar is a convenience, not the report.
    if (held())
        return {};

    owner_ = owner;
    generation_ = next_generation_++;
    // A producer that has taken the bar is running, whether or not it can say
    // how far along it is. Leaving the previous operation's error on screen
    // under a new owner would attribute it to the wrong one.
    state_ = TaskbarProgressState::Indeterminate;
    fraction_ = 0.0;
    return TaskbarProgressLease{owner_, generation_};
}

bool TaskbarProgressLedger::update(const TaskbarProgressLease& lease, double fraction) noexcept {
    if (!holds(lease))
        return false;

    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const bool same_step = state_ == TaskbarProgressState::Normal && WholePercent(clamped) == WholePercent(fraction_);
    fraction_ = clamped;
    if (same_step)
        return false;

    state_ = TaskbarProgressState::Normal;
    return true;
}

bool TaskbarProgressLedger::setIndeterminate(const TaskbarProgressLease& lease) noexcept {
    if (!holds(lease) || state_ == TaskbarProgressState::Indeterminate)
        return false;
    state_ = TaskbarProgressState::Indeterminate;
    return true;
}

bool TaskbarProgressLedger::fail(const TaskbarProgressLease& lease) noexcept {
    // The error state outlives the lease on purpose: an operation that failed
    // has nothing left to publish, and a bar cleared in the same instant is a
    // failure report nobody sees. The next acquire clears it.
    return endLease(lease, TaskbarProgressState::Error);
}

bool TaskbarProgressLedger::finish(const TaskbarProgressLease& lease) noexcept {
    return endLease(lease, TaskbarProgressState::NoProgress);
}

bool TaskbarProgressLedger::cancel(const TaskbarProgressLease& lease) noexcept {
    return endLease(lease, TaskbarProgressState::NoProgress);
}

bool TaskbarProgressLedger::release(const TaskbarProgressLease& lease) noexcept {
    return endLease(lease, TaskbarProgressState::NoProgress);
}

TaskbarProgressState TaskbarProgressLedger::state() const noexcept {
    return state_;
}

double TaskbarProgressLedger::fraction() const noexcept {
    return fraction_;
}

TaskbarProgressOwner TaskbarProgressLedger::owner() const noexcept {
    return owner_;
}

bool TaskbarProgressLedger::held() const noexcept {
    return owner_ != TaskbarProgressOwner::None;
}

bool TaskbarProgressLedger::holds(const TaskbarProgressLease& lease) const noexcept {
    // Both halves. The owner alone cannot separate a session's second remux from
    // its first one's late callback, which is exactly the case that moves the
    // wrong bar.
    return lease.valid() && lease.owner == owner_ && lease.generation == generation_;
}

bool TaskbarProgressLedger::endLease(const TaskbarProgressLease& lease, TaskbarProgressState final_state) noexcept {
    if (!holds(lease))
        return false;
    owner_ = TaskbarProgressOwner::None;
    generation_ = 0;
    state_ = final_state;
    fraction_ = 0.0;
    return true;
}

} // namespace exosnap

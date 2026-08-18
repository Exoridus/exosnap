#pragma once
// UpdateCheckGate.h -- the pure resolver behind UpdateService's asynchronous
// update check (QCR-202).
//
// An update check runs on a worker thread while the GUI thread stays live: the
// user can change the channel, and can ask for another check. The engine's
// UpdateCheckResult carries no channel and no request identity, so on its own it
// cannot say which question it answers. Everything an answer needs in order to
// be attributed correctly is therefore snapshotted into an immutable
// UpdateCheckOperation before the worker starts, and the completion is judged
// against that snapshot rather than against whatever the service's mutable state
// happens to be when the answer arrives.
//
// Pure and Qt-free apart from the engine types, in the same shape as
// capture_hub_policy.h / CaptureHubGate: the service is a thin driver, so the
// interleavings can be tested exhaustively without a network or a thread.

#include <update/update_types.h>

#include <cstdint>

namespace exosnap {

// The immutable context of one asynchronous check. Filled in once, under the
// service mutex, before the worker is started; never re-read from mutable state.
struct UpdateCheckOperation {
    // Monotonic, assigned at start. 0 is "no operation".
    std::uint64_t id = 0;
    // The feed this check actually queried.
    exosnap::update::UpdateChannel channel = exosnap::update::UpdateChannel::Stable;
    // ADR 0055: whether this check was allowed to offer the running version.
    // Part of the context because the answer means something different with and
    // without it, and the mode can be toggled while a check is in flight.
    bool verify_reinstall = false;
};

// Why a completion was not adopted. Reported so the driver can log the actual
// reason rather than guessing, and so the tests name what they assert.
enum class UpdateCompletionVerdict : std::uint8_t {
    // The answer belongs to the current question: adopt it.
    Adopt,
    // A newer operation has since started. The older answer must not overwrite
    // the newer operation's state — including its `checking` flag, which the
    // newer operation still owns.
    SupersededByNewerCheck,
    // The user left the channel this check ran on. The answer describes a feed
    // they are no longer on, so it must not be presented as this channel's
    // availability. This operation is still the active one, so it is also the
    // one that has to clear `checking`.
    ChannelChanged,
};

struct UpdateCheckCompletion {
    // The state to store and publish. Equal to the prior state when the verdict
    // is SupersededByNewerCheck.
    exosnap::update::UpdateState state{};
    UpdateCompletionVerdict verdict = UpdateCompletionVerdict::Adopt;
    // True when the state above must replace the stored state. False only for
    // SupersededByNewerCheck, where the newer operation owns the state.
    bool publish = false;
    // True when the release notes carried by the completed operation may be
    // stored. False on both superseded paths — notes for a channel the user has
    // left would otherwise end up in the post-update "What's new" payload.
    bool adopt_notes = false;
};

// Fold a completed check into the service state.
//
//   prior              the state as currently stored
//   completed          the immutable context of the check that just finished
//   active_operation   the id of the operation the service currently considers
//                      in flight (0 when none)
//   current_channel    the channel selected right now
//   result             what the engine returned
[[nodiscard]] UpdateCheckCompletion ResolveUpdateCheckCompletion(exosnap::update::UpdateState prior,
                                                                 const UpdateCheckOperation& completed,
                                                                 std::uint64_t active_operation,
                                                                 exosnap::update::UpdateChannel current_channel,
                                                                 const exosnap::update::UpdateCheckResult& result);

// Selecting a channel invalidates whatever the previous channel answered:
// "Update available — 1.2.0" is a statement about a feed, not about the app. The
// availability fields are cleared and the channel is set; `checking` is left
// alone, because a check that is in flight still has to report itself finished.
[[nodiscard]] exosnap::update::UpdateState ApplyUpdateChannelSelection(exosnap::update::UpdateState prior,
                                                                       exosnap::update::UpdateChannel channel);

} // namespace exosnap

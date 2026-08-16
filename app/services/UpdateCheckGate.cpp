// UpdateCheckGate.cpp -- see UpdateCheckGate.h.

#include "UpdateCheckGate.h"

namespace exosnap {

UpdateCheckCompletion ResolveUpdateCheckCompletion(exosnap::update::UpdateState prior,
                                                   const UpdateCheckOperation& completed,
                                                   std::uint64_t active_operation,
                                                   exosnap::update::UpdateChannel current_channel,
                                                   const exosnap::update::UpdateCheckResult& result) {
    UpdateCheckCompletion out;

    // A newer check has started since this one was launched. Its own completion
    // will publish, and it owns `checking` until it does — so this answer is
    // dropped whole rather than half-applied.
    if (completed.id != active_operation) {
        out.state = std::move(prior);
        out.verdict = UpdateCompletionVerdict::SupersededByNewerCheck;
        return out;
    }

    out.state = std::move(prior);
    // This operation is the active one, so it is the one that has to say the
    // check is over, on both the adopt and the channel-changed path.
    out.state.checking = false;
    out.publish = true;

    if (completed.channel != current_channel) {
        // The answer is about a feed the user has left. Publishing it would put
        // the other channel's availability under the current channel's label,
        // which is precisely the misattribution this gate exists to prevent.
        out.state.channel = current_channel;
        out.state.update_available = false;
        out.state.available_version.reset();
        out.state.last_error.clear();
        out.verdict = UpdateCompletionVerdict::ChannelChanged;
        return out;
    }

    out.state.channel = completed.channel;
    out.state.update_available = result.update_available;
    out.state.available_version = result.available_version;
    // Cleared on success rather than left standing: last_error describes the
    // most recent check, and a check that succeeded had no error. Keeping the
    // previous one made a recovered feed keep reporting the outage.
    out.state.last_error = result.error_message.value_or(std::string{});
    out.verdict = UpdateCompletionVerdict::Adopt;
    out.adopt_notes = true;
    return out;
}

exosnap::update::UpdateState ApplyUpdateChannelSelection(exosnap::update::UpdateState prior,
                                                         exosnap::update::UpdateChannel channel) {
    exosnap::update::UpdateState next = std::move(prior);
    if (next.channel == channel)
        return next;
    next.channel = channel;
    next.update_available = false;
    next.available_version.reset();
    next.last_error.clear();
    return next;
}

} // namespace exosnap

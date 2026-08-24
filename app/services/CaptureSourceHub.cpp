#include "services/CaptureSourceHub.h"

#include <utility>

#include <exosnap/engine/logging/logging.h>

namespace exosnap {

using exosnap::engine::CaptureHubDecision;
using exosnap::engine::CaptureHubEvent;
using exosnap::engine::HubFrameKind;
using exosnap::engine::ResolveHubFrame;
using exosnap::engine::StepCaptureHub;

CaptureSourceHub::CaptureSourceHub(std::unique_ptr<HubSourceProducer> producer) : producer_(std::move(producer)) {
}

CaptureSourceHub::~CaptureSourceHub() {
    CloseProducer();
}

CaptureHubDecision CaptureSourceHub::Apply(CaptureHubEvent event) {
    const CaptureHubDecision d = StepCaptureHub(state_, event);
    state_ = d.next;

    // Close before the lease is granted: the engine may not open a second
    // duplication of an output this process still holds.
    if (d.close_capture)
        CloseProducer();
    if (d.open_capture)
        OpenProducer();

    // schedule_reopen needs no action here: Pump() reopens whenever it owns the
    // capture but has no live producer, which is exactly the holding state.

    if (!state_.has_frame)
        held_ = {};

    return d;
}

void CaptureSourceHub::OpenProducer() {
    if (producer_open_)
        return;
    std::string err;
    if (producer_->Open(err)) {
        producer_open_ = true;
        last_open_error_.clear();
        return;
    }
    // A source that will not open leaves the hub owning a capture with no live
    // producer -- the holding state. Pump() retries it, without a deadline, so
    // without this line the reason is nowhere: consumers just keep seeing the
    // held frame and the log stays silent for as long as the retry runs.
    LogOpenFailure(err);
    Apply(CaptureHubEvent::SourceLost);
}

void CaptureSourceHub::LogOpenFailure(const std::string& err) {
    if (err == last_open_error_)
        return;
    last_open_error_ = err;
    exosnap::engine::logging::log(
        exosnap::engine::logging::LogLevel::Warn, "capture_hub",
        "capture source failed to open: " + (err.empty() ? std::string("no reason given") : err), {});
}

void CaptureSourceHub::CloseProducer() {
    if (!producer_open_)
        return;
    producer_->Close();
    producer_open_ = false;
}

void CaptureSourceHub::Deliver(const HubFrame& frame) {
    std::lock_guard lk(subscribers_mutex_);
    const HubFrameKind kind = ResolveHubFrame(state_);
    for (const Subscriber& s : subscribers_)
        s.cb(frame, kind);
}

uint64_t CaptureSourceHub::Subscribe(FrameCallback cb) {
    uint64_t token = 0;
    {
        std::lock_guard lk(subscribers_mutex_);
        token = next_token_++;
        subscribers_.push_back({token, std::move(cb)});
    }
    Apply(CaptureHubEvent::ConsumerAdded);
    return token;
}

void CaptureSourceHub::Unsubscribe(uint64_t token) {
    bool removed = false;
    {
        std::lock_guard lk(subscribers_mutex_);
        for (auto it = subscribers_.begin(); it != subscribers_.end(); ++it) {
            if (it->token == token) {
                subscribers_.erase(it);
                removed = true;
                break;
            }
        }
    }
    // An unknown token is not a consumer leaving. Reporting it as one would
    // close a capture somebody else is still watching.
    if (removed)
        Apply(CaptureHubEvent::ConsumerRemoved);
}

void CaptureSourceHub::Pump() {
    // Nothing to own, nothing to pump. This covers both the idle hub (no
    // consumers) and a leased one: granting the lease clears capture_open,
    // because the engine's duplication and ours may not coexist.
    if (!state_.capture_open)
        return;

    if (!producer_open_) {
        std::string err;
        if (producer_->Open(err)) {
            producer_open_ = true;
            last_open_error_.clear();
            // Reopened, but not producing yet: consumers keep the held frame
            // until a frame actually arrives.
            Apply(CaptureHubEvent::ReopenSucceeded);
        } else {
            LogOpenFailure(err);
            Apply(CaptureHubEvent::ReopenFailed);
        }
        return;
    }

    HubFrame frame;
    switch (producer_->PollFrame(frame)) {
    case ProducerPoll::Frame:
        held_ = frame;
        Apply(CaptureHubEvent::FrameReceived);
        Deliver(frame);
        break;

    case ProducerPoll::NoFrame:
        // A quiet source is not a lost one. The desktop simply did not change.
        break;

    case ProducerPoll::Lost:
        CloseProducer();
        Apply(CaptureHubEvent::SourceLost);
        break;

    case ProducerPoll::Fatal:
        CloseProducer();
        Apply(CaptureHubEvent::SourceUnrecoverable);
        break;
    }
}

HubFrameKind CaptureSourceHub::Frame() const {
    return ResolveHubFrame(state_);
}

HubFrame CaptureSourceHub::HeldFrame() const {
    return held_;
}

bool CaptureSourceHub::RequestLease() {
    return Apply(CaptureHubEvent::LeaseRequested).grant_lease;
}

void CaptureSourceHub::ReturnLease() {
    Apply(CaptureHubEvent::LeaseReturned);
}

void CaptureSourceHub::ForwardFrame(const HubFrame& frame) {
    held_ = frame;
    Apply(CaptureHubEvent::FrameReceived);
    Deliver(frame);
}

} // namespace exosnap

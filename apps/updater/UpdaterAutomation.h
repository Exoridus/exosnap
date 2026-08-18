#pragma once

// UpdaterAutomation.h -- the concrete state source behind the updater's
// automation endpoint.
//
// Two jobs, and the second one is the interesting one:
//
//  1. It routes the six intents to the same functions the window's buttons call.
//     The wiring is handed in as callbacks by main.cpp, so this class stays
//     Qt-Core-only and testable without a QApplication, a window or a worker.
//
//  2. It owns the revision counter -- and the RULE for advancing it.
//
// The rule: `stateRevision` advances when the state a runner can act on
// changed, and at no other time. Download progress is deliberately excluded.
// A download emits progress at roughly 12 Hz; letting each tick bump the
// revision would turn "wait until the revision advances" -- the whole reason
// the counter exists -- into "wait up to 80 ms", which is a sleep with extra
// steps. The byte counters are still published in every state payload, at full
// frequency; they simply are not what the counter measures.
//
// Everything else in UpdateFlowState IS revision-bearing, including the failure
// case, the retry entry step and the install state: those are exactly the
// facts an automated recovery check waits for.

#include "UpdaterControlSource.h"

#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <functional>

#include <update/update_flow_state.h>

class UpdaterAutomation : public UpdaterControlSource {
  public:
    // The six product actions, wired by main.cpp to the same slots the footer
    // buttons drive. An unset callback answers false with a filled error rather
    // than silently succeeding -- an intent that cannot be routed must never
    // look accepted.
    struct Intents {
        std::function<bool(QString*)> check;
        std::function<bool(QString*)> download;
        std::function<bool(QString*)> apply;
        std::function<bool(QString*)> retry;
        std::function<bool(QString*)> cancel;
        std::function<bool(QString*)> close;
    };

    UpdaterAutomation(QJsonObject identity, Intents intents);

    // Called after every controller event. Returns true when the revision
    // advanced, which is the signal main.cpp turns into an updater.stateChanged
    // event -- so the event and the counter can never disagree.
    bool Publish(const exosnap::update::UpdateFlowState& state);

    [[nodiscard]] QJsonObject Identity() const override;
    [[nodiscard]] exosnap::update::UpdateFlowState State() const override;
    [[nodiscard]] std::uint64_t StateRevision() const override;

    bool Check(QString* error) override;
    bool Download(QString* error) override;
    bool Apply(QString* error) override;
    bool Retry(QString* error) override;
    bool Cancel(QString* error) override;
    bool Close(QString* error) override;

    // The comparison the revision counter uses: the full state minus the byte
    // counters. Exposed so the exclusion is pinned by a test rather than by
    // reading Publish().
    [[nodiscard]] static bool RevisionBearingEqual(const exosnap::update::UpdateFlowState& a,
                                                   const exosnap::update::UpdateFlowState& b);

  private:
    bool Invoke(const std::function<bool(QString*)>& intent, const char* name, QString* error);

    QJsonObject identity_;
    Intents intents_;
    exosnap::update::UpdateFlowState state_;
    std::uint64_t revision_ = 0;
};

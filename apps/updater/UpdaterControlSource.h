#pragma once

// UpdaterControlSource.h -- everything the updater's automation endpoint is
// allowed to see or do, expressed as one narrow interface.
//
// This is the security boundary, not a convenience abstraction. There is no
// "invoke method by name", no property path, no object lookup: a command that is
// not a member function here cannot be reached over the pipe, and adding one is
// a code change with a review, not a runtime capability.
//
// The intents bind to the SAME actions the window's buttons call. An automated
// check that drove a private path would prove something users never execute --
// and for an updater that is the whole point: the thing being asserted is that
// the real install flow behaves, not that a test harness can install.
//
// Preconditions are NOT here. They live in UpdaterCommandPolicy, evaluated
// against State() before any of these run, so the same predicates answer "may
// this run" and "what is available right now".
//
// Every call runs on the updater's GUI thread; implementations do not need to be
// thread-safe.

#include <QJsonObject>
#include <QString>

#include <cstdint>

#include <update/update_flow_state.h>

class UpdaterControlSource {
  public:
    virtual ~UpdaterControlSource() = default;

    // --- Identity -----------------------------------------------------------
    // Answered by system.hello. Must carry enough for the runner to refuse a
    // process it did not mean to talk to: product version, executable path, pid,
    // install mode, install directory and channel.
    [[nodiscard]] virtual QJsonObject Identity() const = 0;

    // --- Product state ------------------------------------------------------
    // The observable state, in product vocabulary. Feeds updater.getState, the
    // precondition policy and availableActions -- one read, three consumers, so
    // a client can never be told an action is available by one of them and
    // refused by another.
    [[nodiscard]] virtual exosnap::update::UpdateFlowState State() const = 0;
    // Monotonic. Advances exactly when the state a runner can act on changed --
    // NOT on a download progress tick; see UpdaterAutomation.h for why that
    // distinction is what keeps the counter useful.
    [[nodiscard]] virtual std::uint64_t StateRevision() const = 0;

    // --- Intents ------------------------------------------------------------
    // All return false with a filled `error` rather than throwing; a refused
    // intent is a normal, reportable outcome. Accepting one is NOT completing
    // it: every intent here is asynchronous, and the response says so.
    virtual bool Check(QString* error) = 0;
    virtual bool Download(QString* error) = 0;
    virtual bool Apply(QString* error) = 0;
    virtual bool Retry(QString* error) = 0;
    // Requests cancellation of the operation in flight. Only meaningful where
    // the engine actually observes it (a download, the bounded msiexec wait);
    // the policy refuses it everywhere else rather than accepting it and
    // doing nothing.
    virtual bool Cancel(QString* error) = 0;
    // Ends the process. The endpoint disappears with it, so a client sees the
    // response and then the connection closing -- that IS the completion.
    virtual bool Close(QString* error) = 0;
};

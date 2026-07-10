#include "PageHydrationController.h"

#include <QTimer>

#include "diagnostics/AppLog.h"
#include "diagnostics/StartupClock.h"

namespace exosnap {

PageHydrationController::PageHydrationController(std::vector<Step> steps, QObject* parent)
    : QObject(parent), steps_(std::move(steps)) {
}

void PageHydrationController::start() {
    if (started_)
        return;
    started_ = true;
    if (steps_.empty())
        return;
    runStep(0);
}

void PageHydrationController::runStep(std::size_t index) {
    const Step& step = steps_[index];

    // The first step keeps the historical "hydrate-<name>-start/-end" milestone
    // shape; every later step uses "hydrate-page-start/-end:<name>" (see the
    // class comment — startup baseline comparisons depend on these exact lines).
    const bool is_first = (index == 0);
    if (is_first)
        logMilestone(QStringLiteral("hydrate-%1-start").arg(step.name));
    else
        logMilestone(QStringLiteral("hydrate-page-start:%1").arg(step.name));

    if (step.build)
        step.build();

    if (is_first)
        logMilestone(QStringLiteral("hydrate-%1-end").arg(step.name));
    else
        logMilestone(QStringLiteral("hydrate-page-end:%1").arg(step.name));

    const std::size_t next = index + 1;
    if (next < steps_.size()) {
        QTimer::singleShot(0, this, [this, next]() { runStep(next); });
    } else {
        logMilestone(QStringLiteral("hydrate-all-end"));
    }
}

void PageHydrationController::logMilestone(const QString& name) {
    diagnostics::AppLog::info(QStringLiteral("perf"),
                              QStringLiteral("%1 %2 ms").arg(name).arg(diagnostics::StartupClock().elapsed()));
}

} // namespace exosnap

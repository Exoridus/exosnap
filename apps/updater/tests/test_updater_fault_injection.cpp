// The verification-only failure seam. What is pinned here is the BOUNDARY, not
// the convenience: the seam has to be unarmed by default, has to name exactly one
// fault, and has to refuse anything it does not recognise instead of falling
// through to a different one.

#include <gtest/gtest.h>

#include <QString>
#include <QtGlobal>

#include "../UpdaterFaultInjection.h"

using exosnap::updater::CurrentInjectedFault;
using exosnap::updater::InjectedFault;
using exosnap::updater::InjectedFaultVariableName;
using exosnap::updater::ParseInjectedFault;

namespace {

// Restores the variable the test found, so a suite that runs these cases in any
// order cannot arm a fault for an unrelated test in the same process.
class ScopedFaultVariable {
  public:
    explicit ScopedFaultVariable(const char* value) {
        had_previous_ = qEnvironmentVariableIsSet(InjectedFaultVariableName());
        if (had_previous_)
            previous_ = qEnvironmentVariable(InjectedFaultVariableName());
        if (value == nullptr)
            qunsetenv(InjectedFaultVariableName());
        else
            qputenv(InjectedFaultVariableName(), value);
    }
    ~ScopedFaultVariable() {
        if (had_previous_)
            qputenv(InjectedFaultVariableName(), previous_.toUtf8());
        else
            qunsetenv(InjectedFaultVariableName());
    }
    ScopedFaultVariable(const ScopedFaultVariable&) = delete;
    ScopedFaultVariable& operator=(const ScopedFaultVariable&) = delete;

  private:
    bool had_previous_ = false;
    QString previous_;
};

} // namespace

TEST(UpdaterFaultInjection, UnsetMeansNoFault) {
    const ScopedFaultVariable unset(nullptr);
    EXPECT_EQ(CurrentInjectedFault(), InjectedFault::None);
}

TEST(UpdaterFaultInjection, EmptyMeansNoFault) {
    const ScopedFaultVariable empty("");
    EXPECT_EQ(CurrentInjectedFault(), InjectedFault::None);
}

TEST(UpdaterFaultInjection, UacDeclinedIsTheOnlyNamedFault) {
    EXPECT_EQ(ParseInjectedFault(QStringLiteral("uacDeclined")), InjectedFault::UacDeclined);
    EXPECT_EQ(ParseInjectedFault(QStringLiteral("UACDECLINED")), InjectedFault::UacDeclined);
}

TEST(UpdaterFaultInjection, AnUnknownNameArmsNothing) {
    // A typo must not silently arm a DIFFERENT fault: the run that meant to
    // inject one then fails its gate on the missing state, which is a true
    // report, instead of passing on a state nobody asked for.
    for (const char* name : {"uac", "declined", "installFailed", "true", "1"})
        EXPECT_EQ(ParseInjectedFault(QString::fromLatin1(name)), InjectedFault::None) << name;
}

TEST(UpdaterFaultInjection, TheVariableIsReadOnEveryCall) {
    // A launcher arms the seam for exactly one child process. A cached read would
    // outlive that run and inject a decline into the accept gate that follows it,
    // which is the failure the campaign's own ordering exists to avoid.
    {
        const ScopedFaultVariable armed("uacDeclined");
        EXPECT_EQ(CurrentInjectedFault(), InjectedFault::UacDeclined);
    }
    EXPECT_EQ(CurrentInjectedFault(), InjectedFault::None);
}

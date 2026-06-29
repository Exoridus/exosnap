// ELEVATION-FOUNDATION-R1 (ADR 0033): unit tests for the elevation provider
// interface, the elevated-relaunch handoff arg builder/parser, and the
// RelaunchResult contract. The Win32 ShellExecuteEx/token calls themselves are
// NOT exercised here — only the pure logic.

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

#include "diagnostics/ElevationProvider.h"
#include "services/ElevatedRelaunch.h"

namespace {

using exosnap::diagnostics::IElevationProvider;
using exosnap::services::BuildRelaunchArgs;
using exosnap::services::ParseRelaunchArgs;
using exosnap::services::RelaunchHandoff;
using exosnap::services::RelaunchResult;

// Stub provider returning a fixed elevation state (the injectable seam tests and
// future providers rely on, mirroring the DiskSpaceProvider stub pattern).
class StubElevationProvider final : public IElevationProvider {
  public:
    explicit StubElevationProvider(bool elevated) : elevated_(elevated) {
    }
    [[nodiscard]] bool IsElevated() const override {
        return elevated_;
    }

  private:
    bool elevated_;
};

TEST(ElevationProviderTest, StubReportsElevated) {
    const StubElevationProvider provider(true);
    EXPECT_TRUE(provider.IsElevated());
}

TEST(ElevationProviderTest, StubReportsNonElevated) {
    const StubElevationProvider provider(false);
    EXPECT_FALSE(provider.IsElevated());
}

TEST(ElevationProviderTest, ConsumedViaInterfacePointer) {
    const StubElevationProvider concrete(true);
    const IElevationProvider& as_iface = concrete;
    EXPECT_TRUE(as_iface.IsElevated());
}

// ---- Handoff arg builder / parser ----------------------------------------

TEST(RelaunchArgsTest, EmptyHandoffProducesNoArgs) {
    const RelaunchHandoff handoff;
    EXPECT_TRUE(BuildRelaunchArgs(handoff).isEmpty());
}

TEST(RelaunchArgsTest, PageAndFlagRoundtrip) {
    RelaunchHandoff handoff;
    handoff.page_name = QStringLiteral("Diagnostics");
    handoff.reenable_present_diag = true;

    const QStringList args = BuildRelaunchArgs(handoff);
    const RelaunchHandoff parsed = ParseRelaunchArgs(args);

    EXPECT_EQ(parsed.page_name, QStringLiteral("Diagnostics"));
    EXPECT_TRUE(parsed.reenable_present_diag);
}

TEST(RelaunchArgsTest, PageOnlyRoundtrip) {
    RelaunchHandoff handoff;
    handoff.page_name = QStringLiteral("Settings");

    const QStringList args = BuildRelaunchArgs(handoff);
    const RelaunchHandoff parsed = ParseRelaunchArgs(args);

    EXPECT_EQ(parsed.page_name, QStringLiteral("Settings"));
    EXPECT_FALSE(parsed.reenable_present_diag);
}

TEST(RelaunchArgsTest, FlagOnlyRoundtrip) {
    RelaunchHandoff handoff;
    handoff.reenable_present_diag = true;

    const QStringList args = BuildRelaunchArgs(handoff);
    const RelaunchHandoff parsed = ParseRelaunchArgs(args);

    EXPECT_TRUE(parsed.page_name.isEmpty());
    EXPECT_TRUE(parsed.reenable_present_diag);
}

TEST(RelaunchArgsTest, ParserIgnoresUnknownArgsAndArgv0) {
    // Simulates the relaunched process's full argv (argv[0] is the exe path).
    const QStringList args{QStringLiteral("C:/path/exosnap.exe"), QStringLiteral("--some-other-flag"),
                           QStringLiteral("--relaunch-page"), QStringLiteral("Diagnostics"),
                           QStringLiteral("--reenable-present-diag")};
    const RelaunchHandoff parsed = ParseRelaunchArgs(args);

    EXPECT_EQ(parsed.page_name, QStringLiteral("Diagnostics"));
    EXPECT_TRUE(parsed.reenable_present_diag);
}

TEST(RelaunchArgsTest, ParserToleratesPageFlagWithoutValue) {
    // A trailing --relaunch-page with no following token must not crash or
    // mis-consume; the page simply stays empty.
    const QStringList args{QStringLiteral("--relaunch-page")};
    const RelaunchHandoff parsed = ParseRelaunchArgs(args);

    EXPECT_TRUE(parsed.page_name.isEmpty());
    EXPECT_FALSE(parsed.reenable_present_diag);
}

TEST(RelaunchArgsTest, PageNameIsTrimmed) {
    RelaunchHandoff handoff;
    handoff.page_name = QStringLiteral("  Logs  ");
    const QStringList args = BuildRelaunchArgs(handoff);
    const RelaunchHandoff parsed = ParseRelaunchArgs(args);
    EXPECT_EQ(parsed.page_name, QStringLiteral("Logs"));
}

// ---- RelaunchResult contract (no Win32 invocation) ------------------------

TEST(RelaunchResultTest, DistinctOutcomes) {
    EXPECT_NE(RelaunchResult::Launched, RelaunchResult::UserDeclined);
    EXPECT_NE(RelaunchResult::Launched, RelaunchResult::Failed);
    EXPECT_NE(RelaunchResult::UserDeclined, RelaunchResult::Failed);
}

} // namespace

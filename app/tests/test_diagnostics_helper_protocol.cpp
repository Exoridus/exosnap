#include "../../apps/diagnostics_helper/Protocol.h"

#include <gtest/gtest.h>

namespace {
using namespace exosnap::diagnostics::ipc;

TEST(DiagnosticsHelperProtocol, BeginHasNoParameters) {
    Record r{};
    r.kind = Kind::Begin;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    r.value = 1;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.value = 0;
    r.related.back() = 1;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

TEST(DiagnosticsHelperProtocol, RejectsUnknownVersionsKindsAndLengths) {
    Record r{};
    r.kind = Kind::Lost;
    EXPECT_FALSE(Valid(r, sizeof(r) - 1));
    EXPECT_FALSE(Valid(r, sizeof(r) + 1));
    r.version++;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.version = kVersion;
    r.kind = static_cast<Kind>(99);
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

TEST(DiagnosticsHelperProtocol, BoundsPresentFieldsBeforeUse) {
    Record r{};
    r.kind = Kind::Present;
    r.value = 1;
    r.process = 12;
    r.related_count = 16;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    ++r.related_count;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.related_count = 0;
    r.flags = 8;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}
TEST(DiagnosticsHelperProtocol, KernelMetricsRejectPathsStaleSamplesAndOverlongNames) {
    Record r{};
    r.kind = Kind::Dpc;
    r.value = 10;
    r.window = 5;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    r.driver[0] = '/';
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.driver.fill('a');
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.driver.fill(0);
    r.process = 15001;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.process = 0;
    r.window = 11;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

TEST(DiagnosticsHelperProtocol, DpcOnlyBeginCannotSpecifyProviderOrParameters) {
    Record r{};
    r.kind = Kind::BeginDpcOnly;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    r.driver[0] = 'x';
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.driver[0] = 0;
    r.window = 3;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

} // namespace

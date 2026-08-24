#include "exosnap/engine/wgc_acquire_classify.h"

#include <gtest/gtest.h>

using exosnap::engine::ClassifyWgcAcquireFailure;
using exosnap::engine::WgcAcquireFailure;

namespace {

// The three device-loss codes are the only ones for which retrying the same
// pool is pointless: every consumer must stop rather than hold.
TEST(WgcAcquireClassify, DeviceRemovedIsDeviceLost) {
    EXPECT_EQ(ClassifyWgcAcquireFailure(DXGI_ERROR_DEVICE_REMOVED), WgcAcquireFailure::DeviceLost);
}

TEST(WgcAcquireClassify, DeviceResetIsDeviceLost) {
    EXPECT_EQ(ClassifyWgcAcquireFailure(DXGI_ERROR_DEVICE_RESET), WgcAcquireFailure::DeviceLost);
}

TEST(WgcAcquireClassify, DeviceHungIsDeviceLost) {
    EXPECT_EQ(ClassifyWgcAcquireFailure(DXGI_ERROR_DEVICE_HUNG), WgcAcquireFailure::DeviceLost);
}

// The capture went away while the device stayed usable.
TEST(WgcAcquireClassify, AccessLostIsSourceLost) {
    EXPECT_EQ(ClassifyWgcAcquireFailure(DXGI_ERROR_ACCESS_LOST), WgcAcquireFailure::SourceLost);
}

TEST(WgcAcquireClassify, AccessDeniedIsSourceLost) {
    EXPECT_EQ(ClassifyWgcAcquireFailure(DXGI_ERROR_ACCESS_DENIED), WgcAcquireFailure::SourceLost);
}

TEST(WgcAcquireClassify, DisconnectedObjectIsSourceLost) {
    EXPECT_EQ(ClassifyWgcAcquireFailure(RPC_E_DISCONNECTED), WgcAcquireFailure::SourceLost);
}

// The contract that matters most: an unlisted code must NOT be folded into a
// known class. Silently treating it as a recoverable loss is what lets a real
// defect keep a dead capture looking alive.
TEST(WgcAcquireClassify, UnlistedHresultIsUnexpected) {
    EXPECT_EQ(ClassifyWgcAcquireFailure(E_INVALIDARG), WgcAcquireFailure::Unexpected);
    EXPECT_EQ(ClassifyWgcAcquireFailure(E_OUTOFMEMORY), WgcAcquireFailure::Unexpected);
    EXPECT_EQ(ClassifyWgcAcquireFailure(static_cast<int32_t>(0x887A0099)), WgcAcquireFailure::Unexpected);
}

// A success code reaching the classifier means the caller called it off the
// failure path; it must not be mistaken for a known loss either.
TEST(WgcAcquireClassify, SuccessCodeIsUnexpected) {
    EXPECT_EQ(ClassifyWgcAcquireFailure(S_OK), WgcAcquireFailure::Unexpected);
}

// Usable in a constant expression, so a caller can switch on it without any
// runtime cost on the hot path.
TEST(WgcAcquireClassify, IsConstexpr) {
    static_assert(ClassifyWgcAcquireFailure(DXGI_ERROR_DEVICE_HUNG) == WgcAcquireFailure::DeviceLost);
    static_assert(ClassifyWgcAcquireFailure(E_FAIL) == WgcAcquireFailure::Unexpected);
    SUCCEED();
}

} // namespace

// CV-BUG-001 / CV-BUG-005: ComApartment RAII guard tests.
//
// The pure classifiers (ComApartmentOwnsReference / ComApartmentUsable) are
// exercised directly against representative HRESULTs, including a genuine
// failure code, without touching real per-thread COM state -- a true
// CoInitializeEx failure is not reliably reproducible in a test process
// without corrupting that state for later tests. The ComApartment class
// itself is then exercised against the two realistic multi-init scenarios
// (same-model re-entry -> S_FALSE, different-model re-entry -> RPC_E_CHANGED_MODE)
// to prove it wraps CoInitializeEx/CoUninitialize correctly and only balances
// the reference it actually owns.

#include <recorder_core/util/com_apartment.h>

#include <gtest/gtest.h>

namespace {

using recorder_core::ComApartment;
using recorder_core::ComApartmentOwnsReference;
using recorder_core::ComApartmentUsable;

// --- Pure classification -----------------------------------------------

TEST(ComApartmentClassifyTest, SOkOwnsReferenceAndIsUsable) {
    EXPECT_TRUE(ComApartmentOwnsReference(S_OK));
    EXPECT_TRUE(ComApartmentUsable(S_OK));
}

TEST(ComApartmentClassifyTest, SFalseOwnsReferenceAndIsUsable) {
    EXPECT_TRUE(ComApartmentOwnsReference(S_FALSE));
    EXPECT_TRUE(ComApartmentUsable(S_FALSE));
}

TEST(ComApartmentClassifyTest, ChangedModeIsUsableButDoesNotOwnReference) {
    EXPECT_FALSE(ComApartmentOwnsReference(RPC_E_CHANGED_MODE));
    EXPECT_TRUE(ComApartmentUsable(RPC_E_CHANGED_MODE));
}

TEST(ComApartmentClassifyTest, RealFailureIsNotUsableAndDoesNotOwnReference) {
    // A genuine CoInitializeEx failure (e.g. E_OUTOFMEMORY / CO_E_INIT_TLS) is
    // not something that can be reliably reproduced on demand without
    // corrupting real COM state for other tests in this process. Exercise the
    // pure classifier with a representative failing HRESULT instead -- this
    // is exactly the code path a real failure would hit.
    constexpr HRESULT kRepresentativeFailure = E_OUTOFMEMORY;
    EXPECT_FALSE(ComApartmentOwnsReference(kRepresentativeFailure));
    EXPECT_FALSE(ComApartmentUsable(kRepresentativeFailure));
}

// --- Real ComApartment: proves the wrapper matches the classifiers -----

TEST(ComApartmentTest, FirstInitOnThreadReturnsSOkAndOwnsReference) {
    ComApartment apartment(COINIT_APARTMENTTHREADED);
    EXPECT_EQ(apartment.result(), S_OK);
    EXPECT_TRUE(apartment.usable());
    EXPECT_TRUE(apartment.ownsReference());
    // Destructor balances with CoUninitialize(), restoring this thread to
    // uninitialized so later tests in this binary see a clean slate.
}

TEST(ComApartmentTest, SameModelReentryReturnsSFalseAndOwnsReference) {
    ComApartment outer(COINIT_APARTMENTTHREADED);
    ASSERT_EQ(outer.result(), S_OK);

    ComApartment inner(COINIT_APARTMENTTHREADED);
    EXPECT_EQ(inner.result(), S_FALSE);
    EXPECT_TRUE(inner.usable());
    EXPECT_TRUE(inner.ownsReference());
    // inner destructs first (reverse declaration order), balancing its own
    // S_FALSE reference; outer then balances its S_OK reference.
}

TEST(ComApartmentTest, ChangedModeDoesNotUnbalanceOuterApartment) {
    ComApartment outer(COINIT_APARTMENTTHREADED);
    ASSERT_EQ(outer.result(), S_OK);

    {
        ComApartment inner(COINIT_MULTITHREADED); // different model, same thread
        EXPECT_EQ(inner.result(), RPC_E_CHANGED_MODE);
        EXPECT_TRUE(inner.usable());
        EXPECT_FALSE(inner.ownsReference());
        // inner's destructor must NOT call CoUninitialize here -- if it did,
        // it would tear down the apartment `outer` still owns.
    }

    // outer's apartment must still be alive: a fresh CoInitializeEx in the
    // SAME model as outer should observe S_FALSE (already initialized), never
    // S_OK (which would mean inner's destructor wrongly unbalanced outer).
    ComApartment probe(COINIT_APARTMENTTHREADED);
    EXPECT_EQ(probe.result(), S_FALSE);
}

} // namespace

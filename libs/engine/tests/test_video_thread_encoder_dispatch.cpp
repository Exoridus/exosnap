// test_video_thread_encoder_dispatch.cpp — IVideoEncoder-refactor design spec,
// section "4. Teststrategie".
//
// Coverage this file provides, and a note on what it deliberately does NOT
// cover (see the design-decision comment block below the includes):
//
//   * FakeVideoEncoder's own contract: slot acquire/exhaustion/release,
//     EncodeFrame producing one structurally-correct packet per call with a
//     pass-through PTS and correct keyframe flagging, and every force-fail
//     hook (Open/Configure/EncodeFrame/Flush) setting out_error and
//     returning false.
//   * VideoEncoderFactory's dispatch mechanism: a test subclass can return a
//     FakeVideoEncoder for any AdapterVendor value, and the real production
//     default (video_encoder_factory.cpp) returns nullptr for every
//     non-Nvidia vendor today.
//   * The SessionState::video_encoder_factory injection seam itself: the
//     field defaults to a real VideoEncoderFactory, is replaceable, and a
//     replaced factory is reachable and dispatches correctly through the
//     SessionState.
//
// ---------------------------------------------------------------------------
// Design decision: why these tests stop short of driving VideoThread::Run()
// ---------------------------------------------------------------------------
// The parallel-split base commit (ab46665) added the SessionState::
// video_encoder_factory seam, but video_thread.cpp itself is Agent A's file
// in a separate worktree and, as of this worktree's base commit, still
// constructs its own concrete `NvencVideoEncoder nvenc;` by value and never
// reads m_state.video_encoder_factory at all (verified by inspection: every
// nvenc.* call site in video_thread.cpp targets that local value, not the
// factory-produced encoder). Swapping in a SessionState with a
// FakeVideoEncoderFactory today would therefore have no observable effect on
// VideoThread -- it would still open a real NVENC session against real GPU
// hardware, exactly as it does now, because the seam it would need to read
// isn't wired up yet in this worktree.
//
// Driving a full SessionState/VideoThread::Run() here would consequently
// test nothing new about dispatch (it would just re-run the real hardware
// path, which the existing NVENC-hardware tests already cover), while adding
// a real-capture dependency to this file. So this file scopes down to what
// is genuinely reachable without Agent A's wiring: the fake's own contract,
// the factory's dispatch/injection mechanics, and the SessionState seam
// itself. See "Remaining limitations" in this task's final report for the
// specific slot-exhaustion/mid-recording-failure coverage this leaves for a
// follow-up once video_thread.cpp is wired to consume the factory.
//
// One deliberate scoping choice inside "factory dispatch": this file never
// asserts what VideoEncoderFactory::Create(Nvidia) returns on the real,
// unmodified factory. video_encoder_factory.cpp is itself a placeholder on
// this base commit (returns nullptr unconditionally, comment: "Agent A wires
// the Nvidia -> NvencVideoEncoder branch") that Agent A is actively changing
// in a sibling worktree. Asserting today's placeholder Nvidia behavior would
// make this test fragile against a merge that is expected and desired. The
// non-Nvidia -> nullptr behavior, by contrast, is stable across that change
// (only the Nvidia branch is being added), so that's what's asserted here.

#include <gtest/gtest.h>

#include "fakes/fake_video_encoder.h"
#include "session_internal.h"

#include <capability/adapter_enum.h>
#include <exosnap/engine/interfaces/VideoEncoderFactory.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using exosnap::capability::AdapterVendor;
using exosnap::engine::EncodedVideoPacket;
using exosnap::engine::IVideoEncoder;
using exosnap::engine::SessionState;
using exosnap::engine::VideoEncoderFactory;
using exosnap::engine::testutil::FakeVideoEncoder;

// Test factory subclass: returns a FakeVideoEncoder for ANY vendor value,
// exactly the pattern the design spec describes tests using to inject the
// fake without touching the real Nvidia branch.
class FakeVideoEncoderFactory : public VideoEncoderFactory {
  public:
    explicit FakeVideoEncoderFactory(int32_t slot_count = 4) : slot_count_(slot_count) {
    }

    [[nodiscard]] std::unique_ptr<IVideoEncoder> Create(AdapterVendor vendor) const override {
        (void)vendor;
        return std::make_unique<FakeVideoEncoder>(slot_count_);
    }

  private:
    int32_t slot_count_;
};

// Test factory subclass: always returns nullptr, standing in for "no encoder
// wired for this vendor" so tests can assert on the nullptr contract itself
// without depending on the real factory's in-flight Nvidia branch.
class NullVideoEncoderFactory : public VideoEncoderFactory {
  public:
    [[nodiscard]] std::unique_ptr<IVideoEncoder> Create(AdapterVendor vendor) const override {
        (void)vendor;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// FakeVideoEncoder: Open / Configure / Flush default-success + force-fail
// ---------------------------------------------------------------------------

TEST(FakeVideoEncoderTest, OpenConfigureFlushSucceedByDefault) {
    FakeVideoEncoder enc;
    std::string err;

    EXPECT_TRUE(enc.Open(nullptr, err));
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(enc.WasOpened());

    EXPECT_TRUE(enc.Configure(1920, 1080, 60, 1, err));
    EXPECT_TRUE(err.empty());
    EXPECT_TRUE(enc.WasConfigured());
    EXPECT_TRUE(enc.GetInitInfo().valid);

    std::vector<EncodedVideoPacket> flushed;
    EXPECT_TRUE(enc.Flush(flushed, err));
    EXPECT_TRUE(err.empty());
}

TEST(FakeVideoEncoderTest, OpenFailsWhenForced) {
    FakeVideoEncoder enc;
    enc.force_open_fail = true;
    std::string err;

    EXPECT_FALSE(enc.Open(nullptr, err));
    EXPECT_FALSE(err.empty());
}

TEST(FakeVideoEncoderTest, ConfigureFailsWhenForced) {
    FakeVideoEncoder enc;
    enc.force_configure_fail = true;
    std::string err;

    EXPECT_FALSE(enc.Configure(1920, 1080, 60, 1, err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(enc.WasConfigured());
    EXPECT_FALSE(enc.GetInitInfo().valid) << "Configure() failure must not mark init info valid";
}

TEST(FakeVideoEncoderTest, FlushFailsWhenForced) {
    FakeVideoEncoder enc;
    enc.force_flush_fail = true;
    std::string err;
    std::vector<EncodedVideoPacket> flushed;

    EXPECT_FALSE(enc.Flush(flushed, err));
    EXPECT_FALSE(err.empty());
}

// ---------------------------------------------------------------------------
// FakeVideoEncoder: EncodeFrame packet synthesis
// ---------------------------------------------------------------------------

TEST(FakeVideoEncoderTest, EncodeFrameProducesOneStructurallyCorrectPacket) {
    FakeVideoEncoder enc;
    std::string err;
    std::string open_err, cfg_err;
    ASSERT_TRUE(enc.Open(nullptr, open_err));
    ASSERT_TRUE(enc.Configure(1280, 720, 60, 1, cfg_err));

    const int32_t slot = enc.AcquireFreeSlot();
    ASSERT_GE(slot, 0);

    std::vector<EncodedVideoPacket> pkts;
    ASSERT_TRUE(enc.EncodeFrame(slot, /*pts_ns=*/1'000'000, 1280, 720, pkts, err));
    ASSERT_EQ(pkts.size(), 1u);
    EXPECT_GT(pkts[0].bytes.size(), 0u);
    EXPECT_EQ(pkts[0].pts_ns, 1'000'000u);
    EXPECT_EQ(enc.LastPtsNs(), 1'000'000u);
}

TEST(FakeVideoEncoderTest, EncodeFramePtsIsPassThroughAndIncrementsAcrossCalls) {
    FakeVideoEncoder enc;
    std::string open_err, cfg_err;
    ASSERT_TRUE(enc.Open(nullptr, open_err));
    ASSERT_TRUE(enc.Configure(1280, 720, 60, 1, cfg_err));

    uint64_t last_pts = 0;
    for (int i = 0; i < 5; ++i) {
        const int32_t slot = enc.AcquireFreeSlot();
        ASSERT_GE(slot, 0);
        const uint64_t pts = static_cast<uint64_t>(i) * 16'666'667ull;
        std::vector<EncodedVideoPacket> pkts;
        std::string err;
        ASSERT_TRUE(enc.EncodeFrame(slot, pts, 1280, 720, pkts, err));
        ASSERT_EQ(pkts.size(), 1u);
        EXPECT_GE(pkts[0].pts_ns, last_pts);
        last_pts = pkts[0].pts_ns;
    }
    EXPECT_EQ(enc.EncodeFrameCallCount(), 5);
}

TEST(FakeVideoEncoderTest, FirstEncodeFrameCallIsAlwaysKeyframe) {
    FakeVideoEncoder enc;
    const int32_t slot = enc.AcquireFreeSlot();
    ASSERT_GE(slot, 0);

    std::vector<EncodedVideoPacket> pkts;
    std::string err;
    ASSERT_TRUE(enc.EncodeFrame(slot, 0, 1280, 720, pkts, err));
    ASSERT_EQ(pkts.size(), 1u);
    EXPECT_TRUE(pkts[0].keyframe);
}

TEST(FakeVideoEncoderTest, RequestKeyframeArmsExactlyNextEncodeFrame) {
    FakeVideoEncoder enc;

    // Consume the automatic first-call keyframe first.
    {
        const int32_t slot = enc.AcquireFreeSlot();
        std::vector<EncodedVideoPacket> pkts;
        std::string err;
        ASSERT_TRUE(enc.EncodeFrame(slot, 0, 1280, 720, pkts, err));
    }

    // Second call, no RequestKeyframe(): not a keyframe.
    {
        const int32_t slot = enc.AcquireFreeSlot();
        std::vector<EncodedVideoPacket> pkts;
        std::string err;
        ASSERT_TRUE(enc.EncodeFrame(slot, 1, 1280, 720, pkts, err));
        ASSERT_EQ(pkts.size(), 1u);
        EXPECT_FALSE(pkts[0].keyframe);
    }

    enc.RequestKeyframe();

    // Third call: arms one keyframe.
    {
        const int32_t slot = enc.AcquireFreeSlot();
        std::vector<EncodedVideoPacket> pkts;
        std::string err;
        ASSERT_TRUE(enc.EncodeFrame(slot, 2, 1280, 720, pkts, err));
        ASSERT_EQ(pkts.size(), 1u);
        EXPECT_TRUE(pkts[0].keyframe);
    }

    // Fourth call: keyframe request was one-shot, so this one is not.
    {
        const int32_t slot = enc.AcquireFreeSlot();
        std::vector<EncodedVideoPacket> pkts;
        std::string err;
        ASSERT_TRUE(enc.EncodeFrame(slot, 3, 1280, 720, pkts, err));
        ASSERT_EQ(pkts.size(), 1u);
        EXPECT_FALSE(pkts[0].keyframe);
    }
}

// ---------------------------------------------------------------------------
// FakeVideoEncoder: mid-recording EncodeFrame failure (fatal escalation
// precondition -- see the design-decision comment above for why the actual
// escalation into video_thread.cpp isn't reachable from this file).
// ---------------------------------------------------------------------------

TEST(FakeVideoEncoderTest, EncodeFrameFailsWhenForced_SetsOutErrorAndReturnsFalse) {
    FakeVideoEncoder enc;
    const int32_t slot = enc.AcquireFreeSlot();
    ASSERT_GE(slot, 0);

    enc.force_encode_fail = true;
    std::vector<EncodedVideoPacket> pkts;
    std::string err;
    EXPECT_FALSE(enc.EncodeFrame(slot, 0, 1280, 720, pkts, err));
    EXPECT_FALSE(err.empty());
    EXPECT_TRUE(pkts.empty()) << "a failed EncodeFrame must not append a packet";
}

TEST(FakeVideoEncoderTest, EncodeFrameFailureIsMidRecording_EarlierFramesUnaffected) {
    // Models the spec's "mid-recording EncodeFrame failure" scenario: several
    // successful frames, then one forced failure, proving the fake can
    // reproduce that shape for a future VideoThread-level test once
    // video_thread.cpp consumes the factory seam.
    FakeVideoEncoder enc;
    std::string err;

    for (int i = 0; i < 3; ++i) {
        const int32_t slot = enc.AcquireFreeSlot();
        ASSERT_GE(slot, 0);
        std::vector<EncodedVideoPacket> pkts;
        ASSERT_TRUE(enc.EncodeFrame(slot, static_cast<uint64_t>(i), 1280, 720, pkts, err));
        ASSERT_EQ(pkts.size(), 1u);
    }

    enc.force_encode_fail = true;
    const int32_t slot = enc.AcquireFreeSlot();
    ASSERT_GE(slot, 0);
    std::vector<EncodedVideoPacket> pkts;
    EXPECT_FALSE(enc.EncodeFrame(slot, 3, 1280, 720, pkts, err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(enc.EncodeFrameCallCount(), 4);
}

// ---------------------------------------------------------------------------
// FakeVideoEncoder: slot lifecycle (acquire/exhaustion/release)
// ---------------------------------------------------------------------------

TEST(FakeVideoEncoderTest, AcquireFreeSlot_ExhaustsAtConfiguredSlotCount) {
    constexpr int32_t kSlots = 3;
    FakeVideoEncoder enc(kSlots);
    ASSERT_EQ(enc.SlotCount(), kSlots);
    EXPECT_EQ(enc.FreeSlotCount(), kSlots);

    std::vector<int32_t> acquired;
    for (int32_t i = 0; i < kSlots; ++i) {
        const int32_t slot = enc.AcquireFreeSlot();
        ASSERT_GE(slot, 0);
        acquired.push_back(slot);
    }
    EXPECT_EQ(enc.FreeSlotCount(), 0);

    // One more acquire beyond capacity must fail (the slot-exhaustion path
    // VideoThread's backpressure logic depends on).
    EXPECT_EQ(enc.AcquireFreeSlot(), -1);
}

TEST(FakeVideoEncoderTest, ReleaseSlot_FreesAnAcquiredButUnsubmittedSlotForReuse) {
    constexpr int32_t kSlots = 2;
    FakeVideoEncoder enc(kSlots);

    const int32_t slot_a = enc.AcquireFreeSlot();
    const int32_t slot_b = enc.AcquireFreeSlot();
    ASSERT_GE(slot_a, 0);
    ASSERT_GE(slot_b, 0);
    ASSERT_EQ(enc.AcquireFreeSlot(), -1) << "precondition: pool exhausted";

    // Error-path release: slot_a was acquired but never submitted via
    // EncodeFrame (e.g. an upstream texture-copy failure).
    enc.ReleaseSlot(slot_a);
    EXPECT_TRUE(enc.FreeSlotCount() >= 1);
    EXPECT_FALSE(enc.SlotInUse(slot_a));

    const int32_t reacquired = enc.AcquireFreeSlot();
    EXPECT_EQ(reacquired, slot_a) << "released slot should be reusable";
}

TEST(FakeVideoEncoderTest, EncodeFrame_ImplicitlyFreesItsSlotOnSuccess_NoExplicitReleaseNeeded) {
    constexpr int32_t kSlots = 1;
    FakeVideoEncoder enc(kSlots);

    const int32_t slot = enc.AcquireFreeSlot();
    ASSERT_GE(slot, 0);
    EXPECT_EQ(enc.AcquireFreeSlot(), -1) << "precondition: sole slot in use";

    std::vector<EncodedVideoPacket> pkts;
    std::string err;
    ASSERT_TRUE(enc.EncodeFrame(slot, 0, 1280, 720, pkts, err));

    // EncodeFrame itself owns/frees the slot on success -- the caller never
    // calls ReleaseSlot after a submitted EncodeFrame (IVideoEncoder's
    // documented contract).
    EXPECT_FALSE(enc.SlotInUse(slot));
    EXPECT_EQ(enc.AcquireFreeSlot(), slot);
}

TEST(FakeVideoEncoderTest, EncodeFrame_ImplicitlyFreesItsSlotOnFailureToo) {
    // Slot exhaustion recovery after a mid-recording encode failure: the
    // failed frame's slot must come back so the caller doesn't leak the pool
    // down to zero on every encode error.
    constexpr int32_t kSlots = 1;
    FakeVideoEncoder enc(kSlots);
    enc.force_encode_fail = true;

    const int32_t slot = enc.AcquireFreeSlot();
    ASSERT_GE(slot, 0);

    std::vector<EncodedVideoPacket> pkts;
    std::string err;
    EXPECT_FALSE(enc.EncodeFrame(slot, 0, 1280, 720, pkts, err));
    EXPECT_FALSE(enc.SlotInUse(slot));
    EXPECT_EQ(enc.AcquireFreeSlot(), slot);
}

// ---------------------------------------------------------------------------
// VideoEncoderFactory: dispatch mechanism
// ---------------------------------------------------------------------------

TEST(VideoEncoderFactoryDispatchTest, DefaultProductionFactory_NonNvidiaVendorsReturnNull) {
    // Stable across Agent A's in-flight change to the Nvidia branch: only the
    // Nvidia case is being wired up, every other vendor stays nullptr both
    // before and after that merge (see the design-decision comment above for
    // why this file does not assert on the Nvidia case itself).
    const VideoEncoderFactory factory;
    EXPECT_EQ(factory.Create(AdapterVendor::Amd), nullptr);
    EXPECT_EQ(factory.Create(AdapterVendor::Intel), nullptr);
    EXPECT_EQ(factory.Create(AdapterVendor::Other), nullptr);
}

TEST(VideoEncoderFactoryDispatchTest, SubclassFactory_ReturnsFakeEncoderRegardlessOfVendor) {
    const FakeVideoEncoderFactory factory;
    for (const AdapterVendor vendor :
         {AdapterVendor::Nvidia, AdapterVendor::Amd, AdapterVendor::Intel, AdapterVendor::Other}) {
        std::unique_ptr<IVideoEncoder> encoder = factory.Create(vendor);
        ASSERT_NE(encoder, nullptr) << "vendor index " << static_cast<int>(vendor);
        EXPECT_NE(dynamic_cast<FakeVideoEncoder*>(encoder.get()), nullptr)
            << "factory subclass must dispatch to FakeVideoEncoder for every vendor";
    }
}

TEST(VideoEncoderFactoryDispatchTest, NullFactory_CreateReturnsNullptr_FatalInitErrorPrecondition) {
    // Per the design spec: video_thread.cpp is defined to treat a nullptr
    // Create() result as the same fatal init error as a failed Open()/
    // Configure() (existing out_error/blocker path, no new error class). This
    // test pins the nullptr contract a caller must handle; the actual
    // escalation into video_thread.cpp is Agent A's wiring and isn't present
    // in this worktree (see the design-decision comment above).
    const NullVideoEncoderFactory factory;
    EXPECT_EQ(factory.Create(AdapterVendor::Nvidia), nullptr);
}

// ---------------------------------------------------------------------------
// SessionState::video_encoder_factory injection seam
// ---------------------------------------------------------------------------

TEST(SessionStateEncoderSeamTest, DefaultsToARealNonNullFactory) {
    SessionState state;
    ASSERT_NE(state.video_encoder_factory, nullptr);
    // The default factory is the base VideoEncoderFactory (production
    // behavior); confirm it's reachable and callable through the field.
    EXPECT_EQ(state.video_encoder_factory->Create(AdapterVendor::Amd), nullptr);
}

TEST(SessionStateEncoderSeamTest, FactoryIsReplaceable_AndDispatchesThroughTheSeam) {
    SessionState state;
    state.video_encoder_factory = std::make_shared<FakeVideoEncoderFactory>(/*slot_count=*/6);

    std::unique_ptr<IVideoEncoder> encoder = state.video_encoder_factory->Create(AdapterVendor::Nvidia);
    ASSERT_NE(encoder, nullptr);
    auto* fake = dynamic_cast<FakeVideoEncoder*>(encoder.get());
    ASSERT_NE(fake, nullptr) << "SessionState must hand back exactly the injected factory's product";
    EXPECT_EQ(fake->SlotCount(), 6);
}

} // namespace

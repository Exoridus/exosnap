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

    [[nodiscard]] std::unique_ptr<IVideoEncoder> Create(AdapterVendor vendor,
                                                        const exosnap::engine::RecorderConfig& = {}) const override {
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
    [[nodiscard]] std::unique_ptr<IVideoEncoder> Create(AdapterVendor vendor,
                                                        const exosnap::engine::RecorderConfig& = {}) const override {
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

template <typename T>
concept HasVendorPreset = requires(T& encoder) { encoder.SetPreset(exosnap::engine::NvencPreset::P4); };

TEST(VideoEncoderFactoryDispatchTest, ActualAdapterDispatchNeverFallsBackToNvidia) {
    const VideoEncoderFactory factory;
    const exosnap::engine::RecorderConfig config;
    for (uint32_t vendor : {0x1002u, 0x1022u, 0x8086u, 0x1414u, 0u}) {
        std::string error;
        EXPECT_EQ(factory.CreateForAdapter(vendor, config, error), nullptr);
        EXPECT_FALSE(error.empty());
    }
    std::string error = "old failure";
    EXPECT_NE(factory.CreateForAdapter(0x10DEu, config, error), nullptr);
    EXPECT_TRUE(error.empty());
}

TEST(VideoEncoderFactoryDispatchTest, ActualAdapterDispatchUsesInjectedFactoryForEveryVendor) {
    class TrackingFactory final : public VideoEncoderFactory {
      public:
        mutable AdapterVendor selected = AdapterVendor::Other;
        std::unique_ptr<IVideoEncoder> Create(AdapterVendor vendor,
                                              const exosnap::engine::RecorderConfig&) const override {
            selected = vendor;
            return std::make_unique<FakeVideoEncoder>(3);
        }
    } factory;
    std::string error;
    const auto encoder = factory.CreateForAdapter(0x8086u, {}, error);
    ASSERT_NE(encoder, nullptr);
    EXPECT_EQ(factory.selected, AdapterVendor::Intel);
    EXPECT_EQ(encoder->SlotCount(), 3);
    EXPECT_TRUE(error.empty());
}

TEST(VideoEncoderFactoryDispatchTest, GenericContractHasNoVendorPreset) {
    EXPECT_FALSE(HasVendorPreset<IVideoEncoder>);
}

TEST(VideoEncoderFactoryDispatchTest, NvidiaConstructionDoesNotRequireOpeningHardware) {
    const VideoEncoderFactory factory;
    EXPECT_NE(factory.Create(AdapterVendor::Nvidia), nullptr);
}

TEST(VideoEncoderFactoryDispatchTest, DefaultProductionFactory_NonNvidiaVendorsReturnNull) {
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

TEST(BackendTuning, NvencPresetIsAppliedOnlyByTheConcreteFactory) {
    exosnap::engine::VideoEncoderFactory factory;
    for (int i = 0; i < 7; ++i) {
        exosnap::engine::RecorderConfig config;
        config.backend_tuning = exosnap::engine::NvencTuning{static_cast<exosnap::engine::NvencPreset>(i)};
        const auto encoder = factory.Create(exosnap::capability::AdapterVendor::Nvidia, config);
        ASSERT_NE(encoder, nullptr);
        EXPECT_EQ(encoder->GetInitInfo().backend_id, "nvenc");
        EXPECT_EQ(encoder->GetInitInfo().backend_preset, "P" + std::to_string(i + 1));
    }
}

TEST(BackendTuning, OnlyTheNvencAlternativeCarriesAPreset) {
    exosnap::engine::BackendTuning none;
    EXPECT_EQ(exosnap::engine::GetNvencTuning(none), nullptr);
    exosnap::engine::BackendTuning nvenc{exosnap::engine::NvencTuning{exosnap::engine::NvencPreset::P6}};
    ASSERT_NE(exosnap::engine::GetNvencTuning(nvenc), nullptr);
    EXPECT_EQ(exosnap::engine::GetNvencTuning(nvenc)->preset, exosnap::engine::NvencPreset::P6);
}

TEST(EncoderDeviceValidation, AutoWithoutResolutionUsesTheCaptureAdapter) {
    exosnap::engine::EncoderDevicePreference preference; // Auto
    const auto validation = exosnap::engine::ValidateEncoderDeviceForCapture(preference, {}, 0x1234);
    EXPECT_TRUE(validation.ok);
}

TEST(EncoderDeviceValidation, ExplicitUnresolvedFailsClosed) {
    exosnap::engine::EncoderDevicePreference preference;
    preference.mode = exosnap::engine::EncoderDevicePreference::Mode::Explicit;
    exosnap::engine::ResolvedEncoderDevice resolved;
    resolved.reason = "device not present";
    const auto validation = exosnap::engine::ValidateEncoderDeviceForCapture(preference, resolved, 0x1234);
    EXPECT_FALSE(validation.ok);
    EXPECT_EQ(validation.reason, "device not present");
}

TEST(EncoderDeviceValidation, CrossAdapterExplicitIsRefused) {
    exosnap::engine::EncoderDevicePreference preference;
    preference.mode = exosnap::engine::EncoderDevicePreference::Mode::Explicit;
    exosnap::engine::ResolvedEncoderDevice resolved;
    resolved.valid = true;
    resolved.adapter_luid = 2;
    const auto validation = exosnap::engine::ValidateEncoderDeviceForCapture(preference, resolved, 1);
    EXPECT_FALSE(validation.ok);
    EXPECT_NE(validation.reason.find("cross-adapter"), std::string::npos);
}

TEST(EncoderDeviceValidation, ExplicitOnTheCaptureAdapterPasses) {
    exosnap::engine::EncoderDevicePreference preference;
    preference.mode = exosnap::engine::EncoderDevicePreference::Mode::Explicit;
    exosnap::engine::ResolvedEncoderDevice resolved;
    resolved.valid = true;
    resolved.adapter_luid = 1;
    EXPECT_TRUE(exosnap::engine::ValidateEncoderDeviceForCapture(preference, resolved, 1).ok);
}

#include "encoder_input_lifetime.h"
#include "fakes/fake_video_encoder.h"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace exosnap::engine;

struct InputState {
    bool alive = false;
    bool registered = false;
};

struct LifetimeState {
    std::array<InputState, 2> inputs;
    unsigned destroy_calls = 0;
    unsigned flush_calls = 0;
    bool destroyed_after_input_release = false;
};

struct HostInput {
    explicit HostInput(InputState& input) : state(input) {
        state.alive = true;
    }
    ~HostInput() {
        state.alive = false;
    }
    InputState& state;
};

class BorrowingEncoder : public testutil::FakeVideoEncoder {
  public:
    explicit BorrowingEncoder(LifetimeState& state) : FakeVideoEncoder(2), state_(state) {
    }
    ~BorrowingEncoder() override {
        if (!WasDestroyed())
            Destroy();
    }

    bool fail_second_registration = false;

    bool RegisterSlotTexture(int32_t slot, GpuTextureHandle texture, std::string& error) override {
        if (fail_second_registration && slot == 1) {
            error = "input registration failed";
            return false;
        }
        if (!FakeVideoEncoder::RegisterSlotTexture(slot, texture, error))
            return false;
        static_cast<InputState*>(texture)->registered = true;
        return true;
    }

    bool Flush(std::vector<EncodedVideoPacket>& packets, std::string& error) override {
        ++state_.flush_calls;
        return FakeVideoEncoder::Flush(packets, error);
    }

    void Destroy() override {
        ++state_.destroy_calls;
        for (auto& input : state_.inputs) {
            if (input.registered && !input.alive)
                state_.destroyed_after_input_release = true;
            input.registered = false;
        }
        FakeVideoEncoder::Destroy();
    }

  private:
    LifetimeState& state_;
};

TEST(EncoderInputLifetime, EarlyReturnUnregistersBeforeThePoolForBothEncoderOwners) {
    for (const bool external_owner : {false, true}) {
        for (const bool registration_failure : {false, true}) {
            SCOPED_TRACE(external_owner ? "FileOutput ownership" : "standalone ownership");
            SCOPED_TRACE(registration_failure ? "later registration fails" : "later allocation/view fails");
            LifetimeState state;
            std::unique_ptr<BorrowingEncoder> output_encoder;
            const auto initialize = [&]() {
                auto standalone = std::make_unique<BorrowingEncoder>(state);
                auto* encoder = standalone.get();
                if (external_owner)
                    output_encoder = std::move(standalone);
                std::array<std::optional<HostInput>, 2> inputs;
                EncoderInputLifetime lifetime(*encoder);
                inputs[0].emplace(state.inputs[0]);
                std::string error;
                if (!encoder->RegisterSlotTexture(0, &state.inputs[0], error))
                    return false;
                if (!registration_failure)
                    return false;
                inputs[1].emplace(state.inputs[1]);
                encoder->fail_second_registration = true;
                return encoder->RegisterSlotTexture(1, &state.inputs[1], error);
            };
            EXPECT_FALSE(initialize());
            output_encoder.reset();
            EXPECT_FALSE(state.destroyed_after_input_release);
            EXPECT_EQ(state.destroy_calls, 1u);
            EXPECT_EQ(state.flush_calls, 0u);
            EXPECT_FALSE(state.inputs[0].registered);
            EXPECT_FALSE(state.inputs[0].alive);
        }
    }
}

TEST(EncoderInputLifetime, ExplicitCleanupDisarmsBeforeTheEncoderOwnerIsReleased) {
    for (const bool flush_fails : {false, true}) {
        SCOPED_TRACE(flush_fails);
        LifetimeState state;
        {
            auto encoder = std::make_unique<BorrowingEncoder>(state);
            std::array<std::optional<HostInput>, 2> inputs;
            EncoderInputLifetime lifetime(*encoder);
            inputs[0].emplace(state.inputs[0]);
            std::string error;
            ASSERT_TRUE(encoder->RegisterSlotTexture(0, &state.inputs[0], error));
            std::vector<EncodedVideoPacket> packets;
            encoder->force_flush_fail = flush_fails;
            EXPECT_EQ(encoder->Flush(packets, error), !flush_fails);
            lifetime.Destroy();
            EXPECT_TRUE(state.inputs[0].alive);
            EXPECT_FALSE(state.inputs[0].registered);
            encoder.reset();
            // The guard remains alive after its encoder owner has released it.
        }
        EXPECT_FALSE(state.destroyed_after_input_release);
        EXPECT_EQ(state.destroy_calls, 1u);
        EXPECT_EQ(state.flush_calls, 1u);
        EXPECT_FALSE(state.inputs[0].alive);
    }
}

} // namespace

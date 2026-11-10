#pragma once
// FakeVideoEncoder — in-memory IVideoEncoder test double
// (IVideoEncoder-refactor design spec, section "4. Teststrategie").
//
// Exists so tests can exercise VideoThread's encoder-dispatch logic (slot
// lifecycle, error escalation) without real NVENC hardware. Configurable slot
// count; EncodeFrame() produces one structurally-correct EncodedVideoPacket
// per call (fixed size, pass-through PTS from the caller) instead of a real
// bitstream. Open()/Configure()/Flush() succeed by default; each has a
// force-fail hook that sets out_error and returns false, for error-path
// tests. Real image/encode correctness stays the domain of the real-hardware
// E2E test (test_session_e2e_real_file.cpp) — this fake never produces a
// valid bitstream.

#include <exosnap/engine/interfaces/IVideoEncoder.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace exosnap::engine::testutil {

class FakeVideoEncoder : public IVideoEncoder {
  public:
    explicit FakeVideoEncoder(int32_t slot_count = 4)
        : slot_count_(slot_count), slot_in_use_(static_cast<size_t>(slot_count < 0 ? 0 : slot_count), false) {
    }

    // --- Error-injection hooks. Set before the call they should affect;
    // force_encode_fail is checked on every EncodeFrame() call until cleared
    // by the test, so a test can fail exactly one mid-recording frame by
    // toggling it back off inside a loop. ---
    bool force_open_fail = false;
    bool force_configure_fail = false;
    bool force_encode_fail = false;
    bool force_reap_fail = false;
    bool force_flush_fail = false;
    std::string forced_error_message = "FakeVideoEncoder: forced failure";

    // Completions from earlier submissions can accompany a later failure.
    // Each batch is transferred once, before the corresponding status returns.
    std::vector<EncodedVideoPacket> encode_completions;
    std::vector<EncodedVideoPacket> reap_completions;
    std::vector<EncodedVideoPacket> flush_completions;
    bool retain_input_after_encode = false;

    void CompleteInput(int32_t slot_idx) {
        ReleaseSlot(slot_idx);
    }

    // --- Configuration setters: recorded only, no validation. The fake has
    // no real codec limits to enforce. ---
    void SetCodec(VideoCodec codec) noexcept override {
        codec_ = codec;
    }
    void SetBitDepth(BitDepth depth) noexcept override {
        bit_depth_ = depth;
    }
    void SetChroma(ChromaSubsampling chroma) noexcept override {
        chroma_ = chroma;
    }
    void SetCq(uint32_t cq) noexcept override {
        cq_ = cq;
    }
    void SetRateControl(RateControlMode mode, uint32_t bitrate_kbps) noexcept override {
        rc_mode_ = mode;
        bitrate_kbps_ = bitrate_kbps;
    }
    void SetColor(const ColorMetadata& color) noexcept override {
        color_ = color;
    }
    void SetKeyframeIntervalSecs(float secs) noexcept override {
        keyframe_interval_secs_ = secs;
    }
    void SetConstantFrameRate(bool cfr) noexcept override {
        cfr_ = cfr;
    }

    [[nodiscard]] EncoderInitInfo GetInitInfo() const noexcept override {
        return init_info_;
    }

    [[nodiscard]] EncoderInputRequirements GetInputRequirements() const noexcept override {
        return input_requirements_;
    }

    bool Open(void* gpu_context, std::string& out_error) override {
        open_called_ = true;
        if (force_open_fail) {
            out_error = forced_error_message;
            return false;
        }
        device_ = gpu_context;
        return true;
    }

    bool Configure(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                   std::string& out_error) override {
        input_requirements_ = {};
        if (force_configure_fail) {
            out_error = forced_error_message;
            return false;
        }
        width_ = width;
        height_ = height;
        fps_num_ = fps_num;
        fps_den_ = fps_den;
        configured_ = true;
        input_requirements_.storage = EncoderInputStorage::D3D11Texture2D;
        input_requirements_.format = chroma_ == ChromaSubsampling::Cs444 ? EncoderInputFormat::Ayuv
                                     : bit_depth_ == BitDepth::Bit10     ? EncoderInputFormat::P010
                                                                         : EncoderInputFormat::Nv12;
        input_requirements_.codec = codec_;
        input_requirements_.bit_depth = bit_depth_;
        input_requirements_.chroma = chroma_;
        input_requirements_.allocation = {width, height};
        input_requirements_.visible = {0, 0, width, height};
        input_requirements_.alignment = {2, 2};
        input_requirements_.required_bind =
            chroma_ == ChromaSubsampling::Cs444 ? EncoderInputBind::UnorderedAccess : EncoderInputBind::RenderTarget;
        input_requirements_.allowed_bind = input_requirements_.required_bind | EncoderInputBind::ShaderResource;
        const auto slots = static_cast<uint32_t>(slot_count_ < 0 ? 0 : slot_count_);
        input_requirements_.pool = {slots, slots, slots};
        input_requirements_.device = device_;

        init_info_.valid = true;
        init_info_.codec = codec_;
        init_info_.rc_mode = rc_mode_;
        init_info_.target_bitrate_kbps = bitrate_kbps_;
        init_info_.max_bitrate_kbps = bitrate_kbps_;
        init_info_.cq = cq_;
        init_info_.bit_depth = bit_depth_;
        init_info_.chroma = chroma_;
        init_info_.color_full_range = (color_.range == ColorRange::Full);
        return true;
    }

    bool RegisterSlotTexture(int32_t slot_idx, GpuTextureHandle texture, std::string& out_error) override {
        (void)texture;
        if (slot_idx < 0 || slot_idx >= slot_count_) {
            out_error = "FakeVideoEncoder: slot index out of range";
            return false;
        }
        return true;
    }

    [[nodiscard]] int32_t SlotCount() const override {
        return slot_count_;
    }

    int32_t AcquireFreeSlot() override {
        for (int32_t i = 0; i < slot_count_; ++i) {
            if (!slot_in_use_[static_cast<size_t>(i)]) {
                slot_in_use_[static_cast<size_t>(i)] = true;
                return i;
            }
        }
        return -1;
    }

    // Error-path release: caller acquired a slot but never submitted it via
    // EncodeFrame (e.g. a texture-copy failure between AcquireFreeSlot and
    // EncodeFrame). Submitted inputs belong to the encoder until the fake's
    // completion hook or its immediate-completion default frees them.
    void ReleaseSlot(int32_t slot_idx) noexcept override {
        if (slot_idx >= 0 && slot_idx < slot_count_) {
            slot_in_use_[static_cast<size_t>(slot_idx)] = false;
        }
    }

    bool EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height,
                     std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) override {
        (void)width;
        (void)height;
        ++encode_frame_calls_;
        // Output can complete while input reuse remains blocked. A forced
        // failure in this fake rejects the new input and frees that slot.
        if (slot_idx >= 0 && slot_idx < slot_count_ && (!retain_input_after_encode || force_encode_fail)) {
            slot_in_use_[static_cast<size_t>(slot_idx)] = false;
        }

        AppendCompletions(encode_completions, out_packets);
        if (force_encode_fail) {
            out_error = forced_error_message;
            return false;
        }

        EncodedVideoPacket pkt;
        pkt.bytes.assign(kPacketSize, static_cast<uint8_t>(0xAB));
        pkt.pts_ns = pts_ns;
        pkt.keyframe = keyframe_requested_ || (encode_frame_calls_ == 1);
        keyframe_requested_ = false;
        out_packets.push_back(std::move(pkt));
        last_pts_ns_ = pts_ns;
        return true;
    }

    bool ReapCompleted(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error,
                       uint32_t wait_head_ms = 0) override {
        (void)wait_head_ms;
        AppendCompletions(reap_completions, out_packets);
        if (force_reap_fail) {
            out_error = forced_error_message;
            return false;
        }
        return true;
    }

    bool Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) override {
        AppendCompletions(flush_completions, out_packets);
        if (force_flush_fail) {
            out_error = forced_error_message;
            return false;
        }
        return true;
    }

    void RequestKeyframe() override {
        keyframe_requested_ = true;
    }

    void Destroy() override {
        destroyed_ = true;
        input_requirements_ = {};
    }

    // --- Test introspection ---
    [[nodiscard]] int32_t EncodeFrameCallCount() const noexcept {
        return encode_frame_calls_;
    }
    [[nodiscard]] bool WasOpened() const noexcept {
        return open_called_;
    }
    [[nodiscard]] bool WasConfigured() const noexcept {
        return configured_;
    }
    [[nodiscard]] bool WasDestroyed() const noexcept {
        return destroyed_;
    }
    [[nodiscard]] uint64_t LastPtsNs() const noexcept {
        return last_pts_ns_;
    }
    [[nodiscard]] int32_t FreeSlotCount() const noexcept {
        int32_t n = 0;
        for (bool in_use : slot_in_use_) {
            if (!in_use)
                ++n;
        }
        return n;
    }
    [[nodiscard]] bool SlotInUse(int32_t slot_idx) const noexcept {
        if (slot_idx < 0 || slot_idx >= slot_count_)
            return false;
        return slot_in_use_[static_cast<size_t>(slot_idx)];
    }

  private:
    static void AppendCompletions(std::vector<EncodedVideoPacket>& completed,
                                  std::vector<EncodedVideoPacket>& out_packets) {
        for (auto& packet : completed)
            out_packets.push_back(std::move(packet));
        completed.clear();
    }

    static constexpr size_t kPacketSize = 256;

    int32_t slot_count_;
    std::vector<bool> slot_in_use_;

    VideoCodec codec_ = VideoCodec::Av1;
    BitDepth bit_depth_ = BitDepth::Bit8;
    ChromaSubsampling chroma_ = ChromaSubsampling::Cs420;
    uint32_t cq_ = 0;
    RateControlMode rc_mode_ = RateControlMode::ConstantQuality;
    uint32_t bitrate_kbps_ = 0;
    ColorMetadata color_{};
    float keyframe_interval_secs_ = 0.0f;
    bool cfr_ = true;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_num_ = 0;
    uint32_t fps_den_ = 0;
    bool configured_ = false;
    bool open_called_ = false;
    bool destroyed_ = false;
    bool keyframe_requested_ = false;
    int32_t encode_frame_calls_ = 0;
    uint64_t last_pts_ns_ = 0;

    EncoderInitInfo init_info_{};
    EncoderInputRequirements input_requirements_;
    void* device_ = nullptr;
};

} // namespace exosnap::engine::testutil

#pragma once

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/packet_types.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace exosnap::engine {

struct AudioCodecPrivateSlot {
    std::vector<uint8_t> bytes;
    uint32_t codec_delay_samples = 0; // IAudioEncoder::CodecDelaySamples
};

struct CodecPrivateData {
    // AV1: AV1CodecConfigurationRecord (4 fixed bytes + the Sequence Header OBU
    // as configOBUs), as Matroska's V_AV1 mapping defines CodecPrivate.
    std::vector<uint8_t> av1_codec_private;
    bool av1_ready = false;

    // H264: SPS+PPS in Annex-B (for MF_MT_MPEG_SEQUENCE_HEADER in IMFSinkWriter)
    std::vector<uint8_t> h264_sps_pps;
    bool h264_ready = false;

    // HEVC: VPS+SPS+PPS in Annex-B (for hvcC construction in MuxThread)
    std::vector<uint8_t> hevc_vps_sps_pps;
    bool hevc_ready = false;

    static constexpr uint32_t kMaxAudioTracks = 3;

    std::array<AudioCodecPrivateSlot, kMaxAudioTracks> audio_codec_private{};
    std::array<bool, kMaxAudioTracks> audio_track_ready{};

    [[nodiscard]] bool VideoReady(VideoCodec codec) const noexcept {
        if (codec == VideoCodec::H264)
            return h264_ready;
        if (codec == VideoCodec::Hevc)
            return hevc_ready;
        return av1_ready;
    }

    [[nodiscard]] bool AudioAllReady(uint32_t track_count) const {
        if (track_count == 0) {
            return true;
        }
        if (track_count > kMaxAudioTracks) {
            return false;
        }

        for (uint32_t i = 0; i < track_count; ++i) {
            if (!audio_track_ready[i]) {
                return false;
            }
        }
        return true;
    }
};

enum class PremuxRoute { Buffered, Mux, Full };

class PremuxState {
  public:
    static constexpr size_t kVideoLimit = 120;
    static constexpr size_t kAudioLimit = 600;

    struct Pending {
        std::deque<EncodedVideoPacket> video;
        std::deque<EncodedAudioPacket> audio;
    };

    void PublishVideo(VideoCodec codec, std::vector<uint8_t> bytes) {
        std::lock_guard lock(mutex_);
        switch (codec) {
        case VideoCodec::Av1:
            codec_private_.av1_codec_private = std::move(bytes);
            codec_private_.av1_ready = true;
            break;
        case VideoCodec::H264:
            codec_private_.h264_sps_pps = std::move(bytes);
            codec_private_.h264_ready = true;
            break;
        case VideoCodec::Hevc:
            codec_private_.hevc_vps_sps_pps = std::move(bytes);
            codec_private_.hevc_ready = true;
            break;
        }
        cv_.notify_all();
    }

    void PublishAudio(uint32_t track, AudioCodecPrivateSlot slot) {
        std::lock_guard lock(mutex_);
        codec_private_.audio_codec_private.at(track) = std::move(slot);
        codec_private_.audio_track_ready.at(track) = true;
        cv_.notify_all();
    }

    // A packet is moved only when buffered. Once headers are ready the caller
    // keeps ownership and routes to the bounded mux queue outside this lock.
    [[nodiscard]] PremuxRoute Route(EncodedVideoPacket& packet, VideoCodec codec, uint32_t tracks) {
        std::lock_guard lock(mutex_);
        if (Ready(codec, tracks)) {
            return PremuxRoute::Mux;
        }
        if (pending_.video.size() >= kVideoLimit) {
            return PremuxRoute::Full;
        }
        pending_.video.push_back(std::move(packet));
        return PremuxRoute::Buffered;
    }

    [[nodiscard]] PremuxRoute Route(EncodedAudioPacket& packet, VideoCodec codec, uint32_t tracks,
                                    uint32_t& buffered_depth) {
        std::lock_guard lock(mutex_);
        if (Ready(codec, tracks)) {
            return PremuxRoute::Mux;
        }
        if (pending_.audio.size() >= kAudioLimit) {
            return PremuxRoute::Full;
        }
        pending_.audio.push_back(std::move(packet));
        buffered_depth = static_cast<uint32_t>(pending_.audio.size());
        return PremuxRoute::Buffered;
    }

    template <typename Stopping> bool WaitReady(VideoCodec codec, uint32_t tracks, Stopping stopping) {
        std::unique_lock lock(mutex_);
        ++waiting_;
        waiting_cv_.notify_all();
        cv_.wait(lock, [&] { return Ready(codec, tracks) || stopping(); });
        --waiting_;
        return Ready(codec, tracks);
    }

    template <typename Stopping>
    bool WaitAudioReady(uint32_t tracks, std::chrono::milliseconds timeout, Stopping stopping) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return codec_private_.AudioAllReady(tracks) || stopping(); });
    }

    [[nodiscard]] CodecPrivateData CodecSnapshot() const {
        std::lock_guard lock(mutex_);
        return codec_private_;
    }

    [[nodiscard]] Pending TakePending() {
        std::lock_guard lock(mutex_);
        return std::exchange(pending_, {});
    }

    [[nodiscard]] Pending PendingSnapshot() const {
        std::lock_guard lock(mutex_);
        return pending_;
    }

    void NotifyStop() noexcept {
        std::lock_guard lock(mutex_);
        cv_.notify_all();
    }

    void Reset() {
        std::lock_guard lock(mutex_);
        codec_private_ = {};
        pending_ = {};
    }

    [[nodiscard]] bool WaitForWaiter(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return waiting_cv_.wait_for(lock, timeout, [&] { return waiting_ != 0; });
    }

  private:
    [[nodiscard]] bool Ready(VideoCodec codec, uint32_t tracks) const {
        return codec_private_.VideoReady(codec) && codec_private_.AudioAllReady(tracks);
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable waiting_cv_;
    CodecPrivateData codec_private_;
    Pending pending_;
    size_t waiting_ = 0;
};

} // namespace exosnap::engine

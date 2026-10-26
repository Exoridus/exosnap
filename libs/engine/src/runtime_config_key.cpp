#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/backend_tuning.h>
#include <exosnap/engine/encoder_device.h>
#include <exosnap/engine/mic_dsp_config.h>
#include <exosnap/engine/overlay_sprites.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/recording_media_types.h>
#include <optional>
#include <string>
#include <vector>

#include <exosnap/engine/recorder_session.h>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <bit>
#include <new>
#include <type_traits>

namespace exosnap::engine {
namespace runtime_config_detail {

class Digest {
  public:
    Digest() : context_(av_sha_alloc()) {
        if (!context_)
            throw std::bad_alloc();
        av_sha_init(context_, 256);
    }
    ~Digest() {
        av_free(context_);
    }
    Digest(const Digest&) = delete;
    Digest& operator=(const Digest&) = delete;

    template <typename T>
    void Add(T value)
        requires(std::is_arithmetic_v<T> || std::is_enum_v<T>)
    {
        if constexpr (std::is_floating_point_v<T>) {
            Add(std::bit_cast<std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>>(value));
        } else if constexpr (std::is_enum_v<T>) {
            Add(static_cast<std::underlying_type_t<T>>(value));
        } else {
            uint64_t bits = static_cast<uint64_t>(value);
            uint8_t bytes[8];
            for (size_t i = 0; i < 8; ++i) {
                bytes[i] = static_cast<uint8_t>(bits & 255);
                bits >>= 8;
            }
            av_sha_update(context_, bytes, 8);
        }
    }
    void Add(const std::string& value) {
        Add(value.size());
        av_sha_update(context_, reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
    template <typename Tag> void Add(const OpaqueId<Tag>& value) {
        av_sha_update(context_, value.bytes.data(), value.bytes.size());
    }
    template <typename Tag> void Add(const ScalarValue<Tag>& value) {
        Add(value.value);
    }
    template <typename T> void Add(const std::optional<T>& value) {
        Add(value.has_value());
        if (value)
            Add(*value);
    }
    template <typename T> void Add(const std::vector<T>& value) {
        Add(value.size());
        for (const auto& item : value)
            Add(item);
    }
    template <typename... T> void Fields(const T&... values) {
        (Add(values), ...);
    }
    void Add(const MicDspConfig& v) {
        Fields(v.hpf_enabled, v.hpf_cutoff_hz, v.gate_enabled, v.gate_threshold_db, v.agc_enabled, v.agc_target_db,
               v.rnnoise_enabled);
    }
    void Add(const ProcessInstanceIdentity& v) {
        Fields(v.process_id, v.creation_time, v.identity_hint);
    }
    void Add(const AudioSourceBinding& v) {
        Fields(v.kind, v.follow_default, v.endpoint_role, v.endpoint_id, v.process, v.process_selection_confirmed);
    }
    void Add(const AudioSourceSpec& v) {
        Fields(v.id, v.legacy_row_id, v.name, v.binding, v.channel_mode, v.dsp, v.post_dsp_gain_linear, v.participation,
               v.muted);
    }
    void Add(const PhysicalCaptureKey& v) {
        Fields(v.kind, v.endpoint_id, v.process, v.format_generation, v.capture_flags);
    }
    void Add(const ResolvedAudioSource& v) {
        Fields(v.spec, v.capture_key, v.availability, v.silence_for_this_start);
    }
    void Add(const TrackLimiterConfig& v) {
        Fields(v.enabled, v.ceiling_db);
    }
    void Add(const ResolvedAudioRoute& v) {
        Fields(v.id, v.source_id, v.gain_linear);
    }
    void Add(const ResolvedRoutingTrack& v) {
        Fields(v.id, v.mux_index.value, v.visible_position, v.name, v.role, v.routes, v.fixed_average_divisor,
               v.limiter);
    }
    void Add(const ResolvedAudioPlan& v) {
        Fields(v.revision, v.sources, v.tracks, v.default_monitor_track_ids, v.monitor_selection_origin,
               v.monitor_confirmation_required, v.warnings);
    }
    void Add(const ResolvedAudioTrack& v) {
        Fields(v.sources, v.source_gain_linear, v.track_index);
    }
    void Add(const EncoderDeviceFingerprint& v) {
        Fields(v.vendor_id, v.device_id, v.subsystem_id, v.name);
    }
    void Add(const CaptureRegion& v) {
        Fields(v.x, v.y, v.width, v.height);
    }
    std::string Finish() {
        uint8_t digest[32];
        av_sha_final(context_, digest);
        static constexpr char digits[] = "0123456789abcdef";
        std::string result(64, '0');
        for (size_t i = 0; i < 32; ++i) {
            result[i * 2] = digits[digest[i] >> 4];
            result[i * 2 + 1] = digits[digest[i] & 15];
        }
        return result;
    }

  private:
    AVSHA* context_;
};

} // namespace runtime_config_detail

// Hash explicit values rather than object bytes, which contain padding and
// allocator addresses. This identity belongs to the in-process command cache.
std::string RuntimeConfigContentKey(const RecorderConfig& v) {
    runtime_config_detail::Digest d;
    const auto path = v.output_path.generic_u8string();
    d.Add(std::string(reinterpret_cast<const char*>(path.data()), path.size()));
    d.Fields(v.output_path_pre_reserved, v.target.kind, v.target.native_id, v.target.description, v.capture_backend,
             v.container, v.video_codec, v.audio_codec, v.chroma, v.bit_depth, v.hdr_mode, v.cq, v.rate_control_mode,
             v.encoder_device.mode, v.encoder_device.device, v.resolved_encoder_device.valid,
             v.resolved_encoder_device.adapter_luid, v.resolved_encoder_device.vendor_id,
             v.resolved_encoder_device.backend, v.resolved_encoder_device.device, v.resolved_encoder_device.reason,
             v.backend_tuning.index());
    if (const auto* t = GetNvencTuning(v.backend_tuning))
        d.Fields(t->preset, t->bframes, t->b_ref_mode, t->lookahead, t->lookahead_depth, t->spatial_aq, t->temporal_aq,
                 t->multipass);
    d.Fields(v.target_bitrate_kbps, v.audio_bitrate_kbps, v.opus_frame_duration, v.opus_complexity, v.audio_sample_rate,
             v.audio_channels, v.audio_bit_depth, v.audio_pcm_float, v.flac_compression_level, v.frame_rate_num,
             v.frame_rate_den, v.cfr, v.cfr_pacing_mode, v.keyframe_interval_secs, v.output_width, v.output_height,
             v.output_fit, v.audio_track_plan.tracks, v.resolved_audio_plan, v.record_audio, v.mic_channel_mode,
             v.mic_device_id, v.audio_target_process_id, v.mic_gain_linear, v.audio_limiter_enabled,
             v.audio_limiter_ceiling_db, v.audio_clock_slaving_enabled, v.mic_hpf_enabled, v.mic_hpf_cutoff_hz,
             v.mic_gate_enabled, v.mic_gate_threshold_db, v.mic_agc_enabled, v.mic_agc_target_db, v.mic_rnnoise_enabled,
             v.capture_cursor, v.crop_region, v.split.duration_ms, v.split.size_bytes);
    const auto& c = v.color;
    d.Fields(c.primaries, c.transfer, c.matrix, c.range, c.bits_per_channel, c.hdr, c.max_content_light_level,
             c.max_frame_average_light_level, c.has_mastering_display, c.mastering_display_primary_r_x,
             c.mastering_display_primary_r_y, c.mastering_display_primary_g_x, c.mastering_display_primary_g_y,
             c.mastering_display_primary_b_x, c.mastering_display_primary_b_y, c.mastering_display_white_point_x,
             c.mastering_display_white_point_y, c.mastering_display_max_luminance, c.mastering_display_min_luminance);
    const auto& w = v.webcam;
    d.Fields(w.enabled, reinterpret_cast<uintptr_t>(w.frame_provider), w.overlay_x_norm, w.overlay_y_norm,
             w.overlay_w_norm, w.overlay_h_norm, w.mirror, w.opacity, w.chroma_key_enabled, w.chroma_r, w.chroma_g,
             w.chroma_b, w.chroma_tolerance, w.chroma_softness, w.chroma_spill_reduction);
    d.Fields(v.sprite_sources.size());
    for (const SpriteSourceProvider* source : v.sprite_sources)
        d.Fields(reinterpret_cast<uintptr_t>(source));
    return d.Finish();
}

} // namespace exosnap::engine

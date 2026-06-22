#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <recorder_core/audio_track_model.h>
#include <recorder_core/recorder_session.h>

namespace exosnap::capability {

enum class CaptureTargetKind {
    Window,
    Display,
};

struct AudioUiState {
    CaptureTargetKind target_kind = CaptureTargetKind::Display;

    // Ordered list of audio sources. The first enabled row always starts a new
    // track; subsequent rows merge into the previous track when merge_with_above=true.
    std::vector<recorder_core::AudioSourceRow> source_rows;

    // Mic-specific settings (not part of the row model).
    recorder_core::MicChannelMode mic_channel_mode = recorder_core::MicChannelMode::Auto;
    std::optional<std::string> selected_mic_device_id;
    float mic_gain_linear = 1.0f;

    // Set for Window target. Null for Display or unresolved Window PID.
    std::optional<uint32_t> selected_window_pid;

    // ---------------------------------------------------------------------------
    // Audio encoding parameters (ADR 0019)
    // ---------------------------------------------------------------------------

    // Target audio bitrate in kbps. 0 = use the engine default for the active codec.
    // Opus: [32, 510] kbps; default 160 kbps (VBR).
    // AAC:  [64, 320] kbps; default 192 kbps.
    uint32_t audio_bitrate_kbps = 160;

    // Opus frame duration. Controls the latency vs CPU tradeoff.
    // Ignored when audio codec is not Opus.
    recorder_core::OpusFrameDuration opus_frame_duration = recorder_core::OpusFrameDuration::Ms20;

    // Opus encoder complexity 0-10 (10 = best quality/highest CPU). Default 10.
    // Ignored when audio codec is not Opus.
    int opus_complexity = 10;

    // ---------------------------------------------------------------------------
    // Brickwall limiter (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // Peak-limit mixed/gained audio to limiter_ceiling_db instead of hard-clipping.
    // Default true (strictly better than the previous hard clip at the ceiling).
    bool limiter_enabled = true;

    // Limiter ceiling in dBFS (<= 0). Default 0.0 keeps the previous clamp ceiling.
    float limiter_ceiling_db = 0.0f;

    // ---------------------------------------------------------------------------
    // Microphone high-pass filter (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // Run the mic input through a high-pass filter to remove low-frequency
    // rumble. Default false (mic DSP alters captured audio, so it is opt-in).
    bool mic_hpf_enabled = false;

    // High-pass cutoff (−3 dB) frequency in Hz. Default 80 Hz.
    float mic_hpf_cutoff_hz = 80.0f;

    // ---------------------------------------------------------------------------
    // Microphone noise gate (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // Run the mic input through a downward noise gate to silence low-level noise
    // between speech. Default false (mic DSP alters captured audio, opt-in).
    bool mic_gate_enabled = false;

    // Gate threshold in dBFS. Below this the gate closes. Default -45 dB.
    float mic_gate_threshold_db = -45.0f;

    // ---------------------------------------------------------------------------
    // Microphone automatic gain control (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // Run the mic input through an automatic gain control that tracks the level
    // and drives it toward a target loudness. Default false (mic DSP alters
    // captured audio, opt-in).
    bool mic_agc_enabled = false;

    // AGC target loudness in dBFS. Default -18 dB.
    float mic_agc_target_db = -18.0f;

    // ---------------------------------------------------------------------------
    // Microphone RNNoise neural noise suppression (Audio v2 — 0.6.0)
    // ---------------------------------------------------------------------------

    // Run the mic input through RNNoise neural noise suppression (the fourth
    // stage of the mic-DSP chain) to attenuate background noise while preserving
    // speech. Default false (mic DSP alters captured audio, opt-in). No numeric
    // parameter — RNNoise is a fixed trained model.
    bool mic_rnnoise_enabled = false;

    // ---------------------------------------------------------------------------
    // Channel / sample-format model (ADR 0030 — 0.6.0)
    // ---------------------------------------------------------------------------

    // Output sample rate in Hz. Vetted set: 44100, 48000, 96000. Default 48000.
    // Opus locks this to 48000 (libopus native rate).
    uint32_t audio_sample_rate = 48000;

    // Output channel count. 1 = mono, 2 = stereo (default). 5.1+ deferred.
    uint32_t audio_channels = 2;

    // Output bit depth for lossless codecs (PCM: 16/24/32; FLAC: 16/24).
    // Ignored for lossy codecs (Opus/AAC). Default 16.
    uint32_t audio_bit_depth = 16;

    // FLAC compression level [0, 8]. Lossless at every level; only trades CPU
    // vs. file size. Default 5.
    int flac_compression_level = 5;

    // Convenience predicates.
    [[nodiscard]] bool IsAppEnabled() const noexcept;
    [[nodiscard]] bool IsSysEnabled() const noexcept;
    [[nodiscard]] bool IsMicEnabled() const noexcept;
};

struct AudioPlanResult {
    bool record_audio = false;
    recorder_core::AudioTrackPlan plan;
    std::optional<uint32_t> audio_target_process_id;
    recorder_core::MicChannelMode mic_channel_mode = recorder_core::MicChannelMode::Auto;
    std::optional<std::string> mic_device_id;
    float mic_gain_linear = 1.0f;

    // Audio encoding parameters (ADR 0019) — passed through from AudioUiState.
    uint32_t audio_bitrate_kbps = 160;
    recorder_core::OpusFrameDuration opus_frame_duration = recorder_core::OpusFrameDuration::Ms20;
    int opus_complexity = 10;

    // Brickwall limiter (Audio v2 — 0.6.0) — passed through from AudioUiState.
    bool limiter_enabled = true;
    float limiter_ceiling_db = 0.0f;

    // Microphone high-pass filter (Audio v2 — 0.6.0) — passed through from AudioUiState.
    bool mic_hpf_enabled = false;
    float mic_hpf_cutoff_hz = 80.0f;

    // Microphone noise gate (Audio v2 — 0.6.0) — passed through from AudioUiState.
    bool mic_gate_enabled = false;
    float mic_gate_threshold_db = -45.0f;

    // Microphone automatic gain control (Audio v2 — 0.6.0) — passed through from AudioUiState.
    bool mic_agc_enabled = false;
    float mic_agc_target_db = -18.0f;

    // Microphone RNNoise neural noise suppression (Audio v2 — 0.6.0) — passed through.
    bool mic_rnnoise_enabled = false;

    // Channel / sample-format model (ADR 0030 — 0.6.0) — passed through from AudioUiState.
    uint32_t audio_sample_rate = 48000;
    uint32_t audio_channels = 2;
    uint32_t audio_bit_depth = 16;
    int flac_compression_level = 5;
};

[[nodiscard]] AudioPlanResult BuildAudioPlan(const AudioUiState& state);

} // namespace exosnap::capability

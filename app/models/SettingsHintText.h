#pragma once

#include <QString>

// SETTINGS-TIERS-R2 Phase 2: per-setting InfoHint strings.
//
// Verbatim from .workspace/design/info-hints-content.md (the authoritative source).
// Only strings whose setting control exists as a UI widget today are included here.
// Future-wave entries (0.6/0.7) are omitted until the controls exist.
//
// Style: terse (~2-4 words or one short clause). "·" separates effect/tradeoff.
namespace exosnap::ui::hints {

// ---- Video / Format & encoding ----
inline const QString kContainer = QStringLiteral("MKV safest \xC2\xB7 MP4 most compatible");
inline const QString kVideoCodecAv1 = QStringLiteral("Best compression \xC2\xB7 newer players");
inline const QString kVideoCodecH264 = QStringLiteral("Universal compatibility \xC2\xB7 larger files");
inline const QString kQualityPreset =
    QStringLiteral("Constant-quality presets: Small \xe2\x89\x88 CQ 30 (smaller files), "
                   "Balanced \xe2\x89\x88 CQ 24, High \xe2\x89\x88 CQ 19 (sharper, larger files). "
                   "Lower CQ = higher quality.");
inline const QString kFrameRate = QStringLiteral("Constant rate \xC2\xB7 editor-friendly");
inline const QString kCaptureCursor = QStringLiteral("Show the mouse pointer");
inline const QString kOutputResolution = QStringLiteral("Downscale to save size \xC2\xB7 re-encodes");

// ---- Audio ----
inline const QString kAudioSourceEnable = QStringLiteral("Include this source");
inline const QString kSeparateTrack = QStringLiteral("Combine into one track");
inline const QString kAudioCodecOpus = QStringLiteral("Best quality-per-bit \xC2\xB7 MKV/WebM");
inline const QString kAudioCodecAac = QStringLiteral("Wide compatibility \xC2\xB7 MP4");
inline const QString kMicDevice = QStringLiteral("How stereo mics are mapped");

// ---- Output / files ----
inline const QString kOutputFolder = QStringLiteral("Where recordings are saved");
inline const QString kFilenamePattern = QStringLiteral("Tokens for auto-naming");
inline const QString kSplitRecording = QStringLiteral("New file every N min / ~N GB");

// ---- Expert mode toggle ----
inline const QString kExpertMode = QStringLiteral("Expert mode reveals lower-level controls that can produce "
                                                  "incompatible files. Enable only if you know why.");

// ---- Presence / Appearance (moved from AdvancedPage in SETTINGS-TIERS-P3) ----
inline const QString kRecordingOverlay = QStringLiteral("On-screen REC badge \xC2\xB7 excluded from capture");
inline const QString kDiagnosticsOverlay = QStringLiteral("Live fps/drops on screen \xC2\xB7 excluded");
inline const QString kQuickControlPill = QStringLiteral("Floating controls while recording");
inline const QString kCloseToTray = QStringLiteral("Keep running when window closed");
inline const QString kNotifications = QStringLiteral("Toasts for saved / low disk / stops");
inline const QString kAccent = QStringLiteral("App highlight color");

// ---- Audio expert (0.6.0) ----
inline const QString kRateControlMode = QStringLiteral("CQ = constant quality \xC2\xB7 VBR/CBR needs bitrate target");
inline const QString kVideoBitrate = QStringLiteral("Target bitrate for VBR/CBR \xC2\xB7 ignored in CQ mode");
inline const QString kMicGain = QStringLiteral("Boost or cut the microphone level before encoding");
inline const QString kMicChannelMode = QStringLiteral("How stereo mic inputs are mapped to the recorded channel");
inline const QString kAudioBitrate =
    QStringLiteral("Codec bitrate for Opus/AAC \xC2\xB7 ignored for PCM/FLAC (lossless)");
inline const QString kOpusFrameDuration =
    QStringLiteral("Opus packet size \xC2\xB7 20 ms default \xC2\xB7 shorter = lower latency");
inline const QString kOpusComplexity =
    QStringLiteral("Encoder search depth (0\xe2\x80\x93 10) \xC2\xB7 higher = better quality");
inline const QString kAudioSampleRate = QStringLiteral("PCM sampling rate \xC2\xB7 Opus is fixed at 48\xC2\xa0kHz");
inline const QString kAudioChannels = QStringLiteral("Stereo preserves L/R \xC2\xB7 Mono mixes both channels");
inline const QString kAudioBitDepth =
    QStringLiteral("PCM/FLAC word size \xC2\xB7 16-bit sufficient for most recordings");
inline const QString kFlacCompression = QStringLiteral("FLAC compression level (0 = fastest, 8 = smallest file)");
inline const QString kBrickwallLimiter =
    QStringLiteral("Hard clip ceiling applied after all DSP stages \xC2\xB7 prevents digital over");
inline const QString kHighPassFilter = QStringLiteral("HPF removes low-frequency rumble below the cutoff frequency");
inline const QString kNoiseGate = QStringLiteral("Silences the mic when input falls below the threshold");
inline const QString kAgc = QStringLiteral("Automatic gain control normalises mic loudness to the target level");
inline const QString kRnnoise = QStringLiteral("Neural-network noise suppression \xC2\xB7 removes background noise");
inline const QString kVideoCodec = QStringLiteral("Video compression codec for this recording");
inline const QString kVideoBitDepth =
    QStringLiteral("8-bit is universal \xC2\xB7 10-bit needs HEVC or AV1 (smoother gradients, larger files)");
inline const QString kVideoColorRange =
    QStringLiteral("Full = native screen precision, best for direct playback \xC2\xB7 Limited = broadcast "
                   "standard, safest for editors/players that ignore the range flag");
inline const QString kFrameTiming =
    QStringLiteral("CFR = constant frame rate for editor compatibility \xC2\xB7 VFR = variable");
inline const QString kFramePacing =
    QStringLiteral("Smooth removes judder from high-refresh/VRR sources; Newest minimises latency");
inline const QString kKeyframeInterval = QStringLiteral(
    "Keyframe interval controls trim accuracy: "
    "2\xC2\xA0s\xC2\xA0=\xC2\xA0"
    "default (lower file size, 2-second trim grid) \xC2\xB7 "
    "1\xC2\xA0s\xC2\xA0=\xC2\xA0"
    "1-second trim grid \xC2\xB7 "
    "0.5\xC2\xA0s\xC2\xA0=\xC2\xA0"
    "finest trim accuracy (slightly larger files). "
    "Shorter intervals produce more frequent keyframes \xe2\x80\x94 required for precise Quick Trim cuts.");

// ---- Skipped (control does not exist in current UI) ----
// kEncoderPreset         — NVENC P1–P7 (0.5 wave — no UI control yet)
// kHdr10                 — HDR10 (0.7 wave)
// kPerTrackGain          — per-track gain (0.6 wave)
// kMute                  — per-track mute (0.6 wave)
// kAudioCodecPcm         — PCM (0.6 wave)
// kAudioCodecFlac        — FLAC (0.6 wave)
// kAutoOpenOutput        — auto-open Output page (Output-editor wave)
// kCountdownOverlay      — countdown overlay (future wave)
// kUpdateChannel         — update channel (0.4 wave, inside UpdateSettingsPanel, not in scope)
// kCrashReporting        — crash reporting (0.4 wave, inside CrashReportPanel, not in scope)

} // namespace exosnap::ui::hints

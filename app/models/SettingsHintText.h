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
inline const QString kQualityPreset = QStringLiteral("Higher = better \xC2\xB7 larger files");
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

// ---- Skipped (control does not exist in v0.5.0 UI) ----
// kVideoCodecHevc        — HEVC codec (0.7 wave)
// kRateControlCQ/VBR/CBR — rate-control mode selector (not yet wired in Settings)
// kBitrate               — per-preset bitrate field (not yet wired)
// kEncoderPreset         — NVENC P1–P7 (0.5 wave — no UI control yet)
// kBitDepth              — 8/10-bit (0.7 wave)
// kHdr10                 — HDR10 (0.7 wave)
// kPerTrackGain          — per-track gain (0.6 wave)
// kMute                  — per-track mute (0.6 wave)
// kAudioCodecPcm         — PCM (0.6 wave)
// kAudioCodecFlac        — FLAC (0.6 wave)
// kAudioBitrate          — global audio bitrate (not yet wired)
// kOpusFrameSize         — Opus frame size (0.5 wave)
// kOpusComplexity        — Opus complexity (0.5 wave)
// kMicChannelMode        — mic channel mode (not yet wired)
// kMicGain               — mic gain (not yet wired)
// kBrickwallLimiter      — brickwall limiter (0.6 wave)
// kHighPassFilter        — high-pass filter (0.6 wave)
// kAutoOpenOutput        — auto-open Output page (Output-editor wave)
// kAccent                — accent color (0.5 wave — advanced page, not in scope)
// kRecordingOverlay      — recording overlay (AdvancedPage, not in scope)
// kDiagnosticsOverlay    — diagnostics overlay (AdvancedPage, not in scope)
// kCountdownOverlay      — countdown overlay (AdvancedPage, not in scope)
// kQuickControlPill      — quick-control pill (AdvancedPage, not in scope)
// kCloseToTray           — close to tray (AdvancedPage, not in scope)
// kNotifications         — notifications (AdvancedPage, not in scope)
// kUpdateChannel         — update channel (0.4 wave, inside UpdateSettingsPanel, not in scope)
// kCrashReporting        — crash reporting (0.4 wave, inside CrashReportPanel, not in scope)

} // namespace exosnap::ui::hints

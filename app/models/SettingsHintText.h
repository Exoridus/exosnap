#pragma once

#include <QString>

// SETTINGS-TIERS-R2 Phase 2: per-setting InfoHint strings.
//
// Two styles coexist, both rendered by InfoHintIcon's QToolTip:
//   - Terse one-liners for rows whose tradeoff is a single short clause. "·"
//     (U+00B7 MIDDLE DOT, "\xC2\xB7") separates an effect from its tradeoff.
//   - Rich-text cards for rows that need real explanation. QToolTip renders HTML,
//     so these carry a short **bold lead line** followed by deliberate <br> breaks.
//     detail::card() wraps the body in a fixed-width block so the tooltip word-wraps
//     to a readable column instead of one very wide line (QTextDocument honours a
//     table cell's width where a bare block's width is not guaranteed). Rich-text
//     bodies must avoid a literal '<' (use the ≤ glyph, not "<=") and a raw '&'.
//
// Wording stays calm and factual, consistent with docs/product-spec.md. InfoHintIcon
// flattens any markup for the screen-reader accessible name.
//
// Style: terse rows ~2-4 words or one short clause; cards a bold lead + a few lines.
namespace exosnap::ui::hints {

namespace detail {
// Fixed-width rich-text block: gives QToolTip a bounded (~250px) column to wrap the
// body into. The body is authored with a bold lead line and deliberate <br> breaks.
inline QString card(const QString& body) {
    return QStringLiteral("<qt><table><tr><td width=\"250\">") + body + QStringLiteral("</td></tr></table></qt>");
}
} // namespace detail

// ---- Video / Format & encoding ----
inline const QString kContainer = QStringLiteral("MKV safest \xC2\xB7 MP4 most compatible");
inline const QString kVideoCodecAv1 = QStringLiteral("Best compression \xC2\xB7 newer players");
inline const QString kVideoCodecH264 = QStringLiteral("Universal compatibility \xC2\xB7 larger files");
inline const QString kQualityPreset =
    detail::card(QStringLiteral("<b>Constant-quality presets.</b><br>"
                                "Draft \xe2\x89\x88 CQ 35 \xe2\x80\xa6 "
                                "Ultra \xe2\x89\x88 CQ 16 (sharpest, largest files).<br>"
                                "Lower CQ = higher quality."));
inline const QString kConstantQuality =
    detail::card(QStringLiteral("<b>Quantizer target (1\xe2\x80\x93"
                                "51).</b><br>"
                                "Lower = better quality, larger files.<br>"
                                "16 = Ultra \xC2\xB7 19 = High \xC2\xB7 24 = Balanced \xC2\xB7 30 = Efficient \xC2\xB7 "
                                "35 = Draft."));
inline const QString kFrameRate = QStringLiteral("Constant rate \xC2\xB7 editor-friendly");
inline const QString kCaptureCursor = QStringLiteral("Show the mouse pointer");
inline const QString kOutputResolution = QStringLiteral("Downscale to save size \xC2\xB7 re-encodes");

// ---- Audio ----
inline const QString kAudioSourceEnable = QStringLiteral("Include this source");
inline const QString kSeparateTrack =
    detail::card(QStringLiteral("<b>Track routing.</b><br>"
                                "On: combines into the track above.<br>"
                                "Off: separate track \xE2\x80\x94 each source keeps its own channel for editing."));
inline const QString kAudioCodecOpus = QStringLiteral("Best quality-per-bit \xC2\xB7 MKV/WebM");
inline const QString kAudioCodecAac = QStringLiteral("Wide compatibility \xC2\xB7 MP4");
inline const QString kMicDevice = QStringLiteral("How stereo mics are mapped");

// ---- Webcam ----
// v0.9 polish: absorbed the former in-card placement note into the card's info-i.
inline const QString kWebcamPlacement = QStringLiteral("Position and size are configured in the Record preview.");

// ---- Output / files ----
inline const QString kOutputFolder = QStringLiteral("Where recordings are saved");
inline const QString kFilenamePattern = QStringLiteral("Tokens for auto-naming");
inline const QString kSplitRecording = QStringLiteral("New file every N min / ~N GB");

// ---- Expert mode toggle ----
inline const QString kExpertMode =
    detail::card(QStringLiteral("<b>Expert mode.</b><br>"
                                "Reveals lower-level controls that can produce incompatible files.<br>"
                                "Enable only if you know why."));

// ---- Presence / Appearance (moved from AdvancedPage in SETTINGS-TIERS-P3) ----
inline const QString kRecordingOverlay = QStringLiteral("On-screen REC badge \xC2\xB7 excluded from capture");
inline const QString kDiagnosticsOverlay = QStringLiteral("Live fps/drops on screen \xC2\xB7 excluded");
inline const QString kQuickControlPill = QStringLiteral("Floating controls while recording");
inline const QString kCloseToTray = QStringLiteral("Keep running when window closed");
inline const QString kNotifications = QStringLiteral("Toasts for saved / low disk / stops");
inline const QString kAccent = QStringLiteral("App highlight color");

// ---- Audio expert (0.6.0) ----
inline const QString kRateControlMode = detail::card(QStringLiteral("<b>Rate control.</b><br>"
                                                                    "CQ = constant quality.<br>"
                                                                    "VBR/CBR needs a bitrate target."));
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
    detail::card(QStringLiteral("<b>Bit depth.</b><br>"
                                "PCM/FLAC word size.<br>"
                                "16-bit is sufficient for most recordings.<br>"
                                "32-bit float is the mix bus's native format \xE2\x80\x94 no conversion and no "
                                "clipping, so no headroom is needed."));
inline const QString kFlacCompression = QStringLiteral("FLAC compression level (0 = fastest, 8 = smallest file)");
inline const QString kBrickwallLimiter =
    detail::card(QStringLiteral("<b>Brickwall limiter.</b><br>"
                                "Hard ceiling applied after all DSP stages \xE2\x80\x94 prevents samples from "
                                "exceeding 0\xC2\xA0"
                                "dBFS (digital clipping)."));
inline const QString kMicPostProcessing = QStringLiteral("DSP stages applied to the microphone signal before encoding");
inline const QString kClockSlaving =
    detail::card(QStringLiteral("<b>A/V clock slaving.</b><br>"
                                "Gently resamples audio (\xe2\x89\xa4 0.05 %, inaudible) onto the video clock when the "
                                "device clock drifts \xE2\x80\x94 keeps long recordings in sync.<br>"
                                "Off = byte-exact capture."));
inline const QString kHighPassFilter = QStringLiteral("HPF removes low-frequency rumble below the cutoff frequency");
inline const QString kNoiseGate = QStringLiteral("Silences the mic when input falls below the threshold");
inline const QString kAgc = QStringLiteral("Automatic gain control normalizes mic loudness to the target level");
inline const QString kRnnoise = QStringLiteral("Neural-network noise suppression \xC2\xB7 removes background noise");
inline const QString kVideoCodec = QStringLiteral("Video compression codec for this recording");
inline const QString kVideoBitDepth =
    detail::card(QStringLiteral("<b>Bit depth.</b><br>"
                                "8-bit is universal.<br>"
                                "10-bit needs HEVC or AV1 \xE2\x80\x94 smoother gradients, larger files."));
inline const QString kChromaSubsampling =
    detail::card(QStringLiteral("<b>Chroma subsampling.</b><br>"
                                "4:2:0 is universal and the right choice for almost everything.<br>"
                                "4:4:4 keeps full color resolution (sharper text and UI, larger files) but needs "
                                "8-bit H.264 or HEVC \xE2\x80\x94 not available with AV1 or 10-bit."));
inline const QString kVideoColorRange =
    detail::card(QStringLiteral("<b>Color range.</b><br>"
                                "Limited is compatible with every player.<br>"
                                "Full keeps marginally more tonal detail, but some players (notably VLC) ignore the "
                                "range flag and expand as Limited, so Full-range recordings look too dark there.<br>"
                                "Only choose Full if your whole playback chain honors it."));
inline const QString kFrameTiming = detail::card(QStringLiteral("<b>Frame timing.</b><br>"
                                                                "CFR = constant frame rate, best for editors.<br>"
                                                                "VFR = variable frame rate."));
inline const QString kFramePacing =
    detail::card(QStringLiteral("<b>Frame pacing.</b><br>"
                                "Phase-correct removes judder from high-refresh / VRR sources.<br>"
                                "Lowest latency shows the newest frame."));
inline const QString kKeyframeInterval =
    detail::card(QStringLiteral("<b>Keyframe interval.</b><br>"
                                "Controls trim accuracy:<br>"
                                "2\xC2\xA0s = default (smaller files, 2-second trim grid).<br>"
                                "1\xC2\xA0s = 1-second trim grid.<br>"
                                "0.5\xC2\xA0s = finest accuracy (slightly larger files).<br>"
                                "Shorter intervals give more frequent keyframes \xE2\x80\x94 needed for precise Quick "
                                "Trim cuts."));
inline const QString kEncoderPreset =
    detail::card(QStringLiteral("<b>NVENC preset.</b><br>"
                                "Speed/quality tradeoff: P1 = fastest, lowest quality \xC2\xB7 P7 = slowest, best "
                                "quality.<br>"
                                "Default P4 (balanced). Applies from the next recording."));
inline const QString kVideoHdrMode =
    detail::card(QStringLiteral("<b>HDR handling.</b><br>"
                                "HDR-capable displays are detected automatically.<br>"
                                "Tone-map to SDR (default) is safest everywhere.<br>"
                                "Record native HDR10 keeps the original PQ/BT.2020 signal but needs HEVC or AV1.<br>"
                                "No effect when the display is not HDR."));

// ---- Crash reporting (Developer/Advanced card) ----
// Mirrors the crash dialog's "Send reports automatically next time" opt-in
// (CrashReportPanel) so the choice can be revisited later without waiting for
// another crash.
inline const QString kCrashReporting =
    QStringLiteral("Off \xC2\xB7 the next crash asks again. On \xC2\xB7 future crashes are sent automatically, "
                   "no dialog.");

// ---- Skipped (control does not exist in current UI) ----
// kPerTrackGain          — per-track gain (0.6 wave)
// kMute                  — per-track mute (0.6 wave)
// kAudioCodecPcm         — PCM (0.6 wave)
// kAudioCodecFlac        — FLAC (0.6 wave)
// kAutoOpenOutput        — auto-open Output page (Output-editor wave)
// kCountdownOverlay      — countdown overlay (future wave)
// kUpdateChannel         — update channel (0.4 wave, Settings updates card, not in scope)

} // namespace exosnap::ui::hints

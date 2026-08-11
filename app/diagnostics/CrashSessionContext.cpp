#include "diagnostics/CrashSessionContext.h"

#include "ExoSnapBuildInfo.h" // exosnap::build::kVersion

namespace exosnap::diagnostics {

std::string CrashContainerToken(capability::Container container) {
    switch (container) {
    case capability::Container::Matroska:
        return "MKV";
    case capability::Container::Mp4:
        return "MP4";
    case capability::Container::WebM:
        return "WebM";
    }
    return "MKV";
}

std::string CrashVideoCodecToken(capability::VideoCodec codec) {
    switch (codec) {
    case capability::VideoCodec::Av1:
        return "AV1";
    case capability::VideoCodec::Hevc:
        return "HEVC";
    case capability::VideoCodec::H264:
        return "H.264";
    }
    return "AV1";
}

std::string CrashAudioCodecToken(capability::AudioCodec codec) {
    switch (codec) {
    case capability::AudioCodec::Opus:
        return "Opus";
    case capability::AudioCodec::Aac:
        return "AAC";
    case capability::AudioCodec::Pcm:
        return "PCM";
    case capability::AudioCodec::Flac:
        return "FLAC";
    }
    return "Opus";
}

crash_capture::SessionContext MakeCrashSessionContext(capability::Container container,
                                                      capability::VideoCodec video_codec,
                                                      capability::AudioCodec audio_codec) {
    crash_capture::SessionContext ctx;
    ctx.app_version = build::kVersion;
    // All NVENC video codecs ship today; the encoder backend baseline is nvenc.
    ctx.encoder_backend = "nvenc";
    ctx.container = CrashContainerToken(container);
    ctx.video_codec = CrashVideoCodecToken(video_codec);
    ctx.audio_codec = CrashAudioCodecToken(audio_codec);
    return ctx;
}

} // namespace exosnap::diagnostics

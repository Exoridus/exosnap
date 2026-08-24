#pragma once

#include "config_types.h"

#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/frame_pacing.h>

#include <cstdint>

namespace exosnap::capability {

struct UserRecorderConfig {
    Container container = Container::Matroska;
    VideoCodec video_codec = VideoCodec::Av1;
    AudioCodec audio_codec = AudioCodec::Opus;
    ChromaSubsampling chroma = ChromaSubsampling::Cs420;
    BitDepth bit_depth = BitDepth::Bit8;
    // Y'CbCr quantization range. Limited (16-235, broadcast) is the default as of
    // fix/color-range-signaling: a controlled A/B comparison showed common
    // consumer players (VLC) ignore the bitstream/container range flag entirely
    // and always apply limited->full expansion, so a Full-range recording is
    // permanently crushed/dark there regardless of correct tagging — the same
    // reason OBS and the rest of the consumer-video ecosystem encode limited by
    // default. Full (0-255, native screen precision) remains available as an
    // opt-in for pipelines that are known to honour the range flag. Never gated.
    ColorRange color_range = ColorRange::Limited;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint32_t frame_rate_num = 60;
    uint32_t frame_rate_den = 1;
    // CFR frame pacing mode (ADR 0035). Smooth = phase-correct (default); Newest = lowest-latency.
    exosnap::engine::FramePacingMode frame_pacing = exosnap::engine::FramePacingMode::Smooth;
    // HDR handling mode (config plumbing only for now). Default TonemapSdr —
    // see exosnap::engine::HdrMode for full semantics. Passed straight through
    // to RecorderConfig by ToRecorderCoreConfig; does not yet derive
    // BT.2020/PQ ColorMetadata from it (needs runtime display facts, still to
    // be wired up).
    exosnap::engine::HdrMode hdr_mode = exosnap::engine::HdrMode::TonemapSdr;
};

} // namespace exosnap::capability

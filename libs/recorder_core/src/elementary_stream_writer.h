#pragma once

// Pure IVF header builders for AV1 elementary-stream output. IVF is the
// simple framed container aomenc/libaom's own tools use for a raw AV1
// bitstream (32-byte file header + one 12-byte header per frame) — needed
// because, unlike NVENC's H.264/HEVC output, raw AV1 OBUs from NVENC are not
// self-delimited the way Annex-B start codes make H.264/HEVC self-delimited.
// H.264/HEVC need no equivalent function here: see elementary_stream_writer's
// caller for why.

#include <cstdint>
#include <vector>

namespace recorder_core {

// Builds the 32-byte IVF file header: "DKIF" signature, u16 version (0), u16
// header length (32), 4-byte FourCC ("AV01"), u16 width, u16 height, u32
// frame-rate numerator ("rate"), u32 frame-rate denominator ("scale"), u32
// frame count, u32 reserved (0). All multi-byte fields little-endian.
std::vector<uint8_t> BuildIvfFileHeader(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                                        uint32_t frame_count);

// Builds the 12-byte IVF per-frame header that precedes each frame's raw
// bitstream bytes: u32 frame size in bytes, then u64 presentation timestamp.
// `frame_index` is used directly as the PTS (the file header already
// declares the frame rate, so a simple 0, 1, 2, ... sequence is enough for
// ffmpeg/ffprobe to reconstruct correct timing).
std::vector<uint8_t> BuildIvfFrameHeader(uint32_t frame_size_bytes, uint64_t frame_index);

} // namespace recorder_core

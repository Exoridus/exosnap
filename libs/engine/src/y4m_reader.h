#pragma once

// Pure YUV4MPEG2 (.y4m) reader: parses the header line and reads raw I420
// frame data from an in-memory buffer. No file I/O here so the parsing logic
// is unit-testable without a real file — the probe (dev-only, GPU-driving)
// owns reading the file into memory and calling these.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace exosnap::engine {

struct Y4mHeader {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 0;
    uint32_t fps_den = 0;
    // Byte length of the header line including its trailing '\n' — frame
    // data starts at this offset in the source buffer.
    size_t header_bytes = 0;
};

// Parses the first line of a .y4m file, e.g.
// "YUV4MPEG2 W1920 H1080 F30:1 Ip A1:1 C420jpeg\n". Only 8-bit 4:2:0 chroma
// (C420, C420jpeg, C420mpeg2) is accepted — anything else, or a missing C
// tag, is an error naming what was rejected. `data` only needs to contain at
// least the header line; trailing frame data (if present) is ignored here.
std::optional<Y4mHeader> ParseY4mHeader(std::string_view data, std::string& out_error);

// Number of raw bytes one 8-bit 4:2:0 (I420) frame occupies: a full-resolution
// Y plane plus two quarter-resolution chroma planes.
constexpr size_t I420FrameSize(uint32_t width, uint32_t height) noexcept {
    return static_cast<size_t>(width) * height +
           2 * ((static_cast<size_t>(width) / 2) * (static_cast<size_t>(height) / 2));
}

struct Y4mFrame {
    // Offset into the source buffer where this frame's raw I420 pixel data
    // begins (immediately after its "FRAME" marker line).
    size_t data_offset = 0;
    size_t data_size = 0;
    // Offset where the next frame's "FRAME" marker (or end of stream) begins.
    size_t next_offset = 0;
};

// Reads one frame's "FRAME" marker line plus its raw I420 payload, starting
// at `offset` in `data` (which must point at the start of a marker line).
// Returns std::nullopt with `out_error` set on a malformed marker or
// truncated payload. Returns std::nullopt with an EMPTY `out_error` at a
// clean end of stream (offset == data.size()) — that is not an error, it is
// how the caller's read loop knows to stop.
std::optional<Y4mFrame> ReadY4mFrame(std::string_view data, size_t offset, uint32_t width, uint32_t height,
                                     std::string& out_error);

} // namespace exosnap::engine

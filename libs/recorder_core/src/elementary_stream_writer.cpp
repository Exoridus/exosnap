#include "elementary_stream_writer.h"

namespace recorder_core {

namespace {

void AppendU16Le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void AppendU32Le(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void AppendU64Le(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

} // namespace

std::vector<uint8_t> BuildIvfFileHeader(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                                        uint32_t frame_count) {
    std::vector<uint8_t> out;
    out.reserve(32);
    out.push_back('D');
    out.push_back('K');
    out.push_back('I');
    out.push_back('F');
    AppendU16Le(out, 0);  // version
    AppendU16Le(out, 32); // header length
    out.push_back('A');
    out.push_back('V');
    out.push_back('0');
    out.push_back('1');
    AppendU16Le(out, static_cast<uint16_t>(width));
    AppendU16Le(out, static_cast<uint16_t>(height));
    AppendU32Le(out, fps_num); // rate
    AppendU32Le(out, fps_den); // scale
    AppendU32Le(out, frame_count);
    AppendU32Le(out, 0); // reserved
    return out;
}

std::vector<uint8_t> BuildIvfFrameHeader(uint32_t frame_size_bytes, uint64_t frame_index) {
    std::vector<uint8_t> out;
    out.reserve(12);
    AppendU32Le(out, frame_size_bytes);
    AppendU64Le(out, frame_index);
    return out;
}

} // namespace recorder_core

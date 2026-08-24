#include "y4m_reader.h"

#include <charconv>

namespace exosnap::engine {

namespace {

bool ParseUint(std::string_view s, uint32_t& out) {
    const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc{} && res.ptr == s.data() + s.size();
}

} // namespace

std::optional<Y4mHeader> ParseY4mHeader(std::string_view data, std::string& out_error) {
    const size_t nl = data.find('\n');
    if (nl == std::string_view::npos) {
        out_error = "y4m header: no newline terminator found";
        return std::nullopt;
    }
    const std::string_view line = data.substr(0, nl);

    size_t pos = 0;
    const size_t firstSpace = line.find(' ');
    const std::string_view magic = line.substr(0, firstSpace == std::string_view::npos ? line.size() : firstSpace);
    if (magic != "YUV4MPEG2") {
        out_error = "y4m header: expected YUV4MPEG2 magic, got '" + std::string(magic) + "'";
        return std::nullopt;
    }
    pos = (firstSpace == std::string_view::npos) ? line.size() : firstSpace + 1;

    Y4mHeader header;
    bool haveWidth = false, haveHeight = false, haveFps = false, haveChroma = false;

    while (pos < line.size()) {
        size_t tagEnd = line.find(' ', pos);
        if (tagEnd == std::string_view::npos)
            tagEnd = line.size();
        const std::string_view tag = line.substr(pos, tagEnd - pos);
        pos = tagEnd + 1;
        if (tag.empty())
            continue;

        switch (tag[0]) {
        case 'W':
            if (!ParseUint(tag.substr(1), header.width)) {
                out_error = "y4m header: bad width tag '" + std::string(tag) + "'";
                return std::nullopt;
            }
            haveWidth = true;
            break;
        case 'H':
            if (!ParseUint(tag.substr(1), header.height)) {
                out_error = "y4m header: bad height tag '" + std::string(tag) + "'";
                return std::nullopt;
            }
            haveHeight = true;
            break;
        case 'F': {
            const size_t colon = tag.find(':');
            if (colon == std::string_view::npos || !ParseUint(tag.substr(1, colon - 1), header.fps_num) ||
                !ParseUint(tag.substr(colon + 1), header.fps_den)) {
                out_error = "y4m header: bad framerate tag '" + std::string(tag) + "'";
                return std::nullopt;
            }
            haveFps = true;
            break;
        }
        case 'C': {
            const std::string_view chroma = tag.substr(1);
            if (chroma != "420" && chroma != "420jpeg" && chroma != "420mpeg2") {
                out_error = "y4m header: unsupported chroma format '" + std::string(chroma) +
                            "' (only 8-bit 4:2:0 is supported)";
                return std::nullopt;
            }
            haveChroma = true;
            break;
        }
        default:
            // I (interlace), A (aspect), X (comment/extension): accepted, ignored.
            break;
        }
    }

    if (!haveWidth || !haveHeight || !haveFps || !haveChroma) {
        out_error = "y4m header: missing required tag (need W, H, F, and C)";
        return std::nullopt;
    }

    header.header_bytes = nl + 1;
    return header;
}

std::optional<Y4mFrame> ReadY4mFrame(std::string_view data, size_t offset, uint32_t width, uint32_t height,
                                     std::string& out_error) {
    if (offset == data.size())
        return std::nullopt; // clean EOF: out_error stays empty

    if (offset > data.size()) {
        out_error = "y4m frame: offset past end of buffer";
        return std::nullopt;
    }

    const size_t nl = data.find('\n', offset);
    if (nl == std::string_view::npos) {
        out_error = "y4m frame: no newline terminator on FRAME marker";
        return std::nullopt;
    }
    const std::string_view markerLine = data.substr(offset, nl - offset);
    // The marker is "FRAME" optionally followed by per-frame parameters
    // ("FRAME Ip ..."); only the literal prefix matters here.
    if (markerLine.substr(0, 5) != "FRAME") {
        out_error = "y4m frame: expected FRAME marker, got '" + std::string(markerLine) + "'";
        return std::nullopt;
    }

    Y4mFrame frame;
    frame.data_offset = nl + 1;
    frame.data_size = I420FrameSize(width, height);
    if (frame.data_offset + frame.data_size > data.size()) {
        out_error = "y4m frame: truncated frame data (need " + std::to_string(frame.data_size) + " bytes, have " +
                    std::to_string(data.size() - frame.data_offset) + ")";
        return std::nullopt;
    }
    frame.next_offset = frame.data_offset + frame.data_size;
    return frame;
}

} // namespace exosnap::engine

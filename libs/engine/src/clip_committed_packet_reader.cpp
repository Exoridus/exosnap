#include "clip_committed_packet_reader.h"
#include "committed_matroska_view.h"
#include "matroska_packet_timestamps.h"
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <utility>

namespace exosnap::engine {
namespace {

constexpr uint32_t kCluster = 0x1f43b675;
constexpr uint32_t kBlockGroup = 0xa0;
constexpr uint32_t kBlock = 0xa1;
constexpr uint32_t kSimpleBlock = 0xa3;
constexpr uint32_t kBlockAdditions = 0x75a1;
constexpr uint32_t kBlockMore = 0xa6;
constexpr uint32_t kBlockAddId = 0xee;
constexpr uint32_t kBlockAdditional = 0xa5;
constexpr uint32_t kReferenceBlock = 0xfb;

EngineError ReaderError(EngineErrorCode code, const char* context) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::Clip;
    error.context = context;
    return error;
}

struct Element {
    uint32_t id = 0;
    uint64_t payload = 0;
    uint64_t end = 0;
};

unsigned VintWidth(uint8_t first, unsigned maximum) {
    for (unsigned width = 1; width <= maximum; ++width)
        if (first & (0x80u >> (width - 1)))
            return width;
    return 0;
}

Outcome<Element> ReadElement(CommittedMatroskaView& view, uint64_t offset, uint64_t parent_end) {
    if (offset >= parent_end || parent_end > view.size())
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Truncated committed Matroska element");
    std::array<uint8_t, 12> header{};
    const auto count = static_cast<size_t>((std::min)(uint64_t{header.size()}, parent_end - offset));
    const auto read = view.ReadAt(offset, std::span<uint8_t>(header.data(), count));
    if (!read.succeeded())
        return *read.error();
    if (*read.value() != count)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Short committed Matroska element header");
    const unsigned id_width = VintWidth(header[0], 4);
    if (!id_width || id_width >= count)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Invalid committed Matroska element ID");
    const unsigned size_width = VintWidth(header[id_width], 8);
    if (!size_width || id_width + size_width > count)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Invalid committed Matroska element size");
    uint32_t id = 0;
    for (unsigned index = 0; index < id_width; ++index)
        id = (id << 8) | header[index];
    uint64_t size = header[id_width] & ((1u << (8 - size_width)) - 1);
    for (unsigned index = 1; index < size_width; ++index)
        size = (size << 8) | header[id_width + index];
    if (size == (uint64_t{1} << (7 * size_width)) - 1)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Unknown-size committed cluster child");
    const uint64_t payload = offset + id_width + size_width;
    if (size > parent_end - payload)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Committed Matroska element crosses its parent");
    return Element{id, payload, payload + size};
}

Outcome<uint64_t> ReadUnsigned(CommittedMatroskaView& view, const Element& element) {
    const uint64_t size = element.end - element.payload;
    if (size == 0 || size > 8)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Invalid committed Matroska integer");
    std::array<uint8_t, 8> bytes{};
    const auto read = view.ReadAt(element.payload, std::span<uint8_t>(bytes.data(), static_cast<size_t>(size)));
    if (!read.succeeded())
        return *read.error();
    if (*read.value() != size)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Short committed Matroska integer");
    uint64_t value = 0;
    for (size_t index = 0; index < size; ++index)
        value = (value << 8) | bytes[index];
    return value;
}

Outcome<std::optional<MatroskaPacketTimestamps>> ReadMore(CommittedMatroskaView& view, const Element& more) {
    uint64_t id = 1;
    std::optional<Element> data;
    for (uint64_t position = more.payload; position < more.end;) {
        const auto child = ReadElement(view, position, more.end);
        if (!child.succeeded())
            return *child.error();
        position = child.value()->end;
        if (child.value()->id == kBlockAddId) {
            const auto value = ReadUnsigned(view, *child.value());
            if (!value.succeeded())
                return *value.error();
            id = *value.value();
        } else if (child.value()->id == kBlockAdditional) {
            if (data)
                return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Duplicate packet addition payload");
            data = *child.value();
        }
    }
    if (id != kPacketTimestampBlockId || !data)
        return std::optional<MatroskaPacketTimestamps>{};
    if (data->end - data->payload != 24)
        return ReaderError(EngineErrorCode::MetadataIncomplete, "Invalid exact packet timestamp length");
    std::array<uint8_t, 24> bytes{};
    const auto read = view.ReadAt(data->payload, bytes);
    if (!read.succeeded())
        return *read.error();
    if (*read.value() != bytes.size())
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Short exact packet timestamps");
    auto decoded = DecodeMatroskaPacketTimestamps(bytes);
    if (!decoded)
        return ReaderError(EngineErrorCode::MetadataIncomplete, "Invalid exact packet timestamps");
    return decoded;
}

Outcome<std::optional<MatroskaPacketTimestamps>> ReadAdditions(CommittedMatroskaView& view, const Element& additions) {
    std::optional<MatroskaPacketTimestamps> found;
    for (uint64_t position = additions.payload; position < additions.end;) {
        const auto child = ReadElement(view, position, additions.end);
        if (!child.succeeded())
            return *child.error();
        position = child.value()->end;
        if (child.value()->id != kBlockMore)
            continue;
        const auto exact = ReadMore(view, *child.value());
        if (!exact.succeeded())
            return *exact.error();
        if (exact.value()->has_value()) {
            if (found)
                return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Duplicate exact packet timestamps");
            found = **exact.value();
        }
    }
    return found;
}

Outcome<ClipCommittedPacket> ReadGroup(CommittedMatroskaView& view, const Element& group, size_t max_packet_bytes) {
    std::optional<Element> block;
    std::optional<MatroskaPacketTimestamps> exact;
    bool referenced = false;
    for (uint64_t position = group.payload; position < group.end;) {
        const auto child = ReadElement(view, position, group.end);
        if (!child.succeeded())
            return *child.error();
        position = child.value()->end;
        if (child.value()->id == kBlock) {
            if (block)
                return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Multiple blocks in committed group");
            if (child.value()->end - child.value()->payload > max_packet_bytes + 11ULL)
                return ReaderError(EngineErrorCode::ResourceBudgetExceeded,
                                   "Encoded clip packet exceeds the worker memory budget");
            block = *child.value();
        } else if (child.value()->id == kReferenceBlock) {
            referenced = true;
        } else if (child.value()->id == kBlockAdditions) {
            if (exact)
                return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Duplicate committed block additions");
            const auto decoded = ReadAdditions(view, *child.value());
            if (!decoded.succeeded())
                return *decoded.error();
            exact = *decoded.value();
        }
    }
    if (!block)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Committed group has no block");
    if (!exact)
        return ReaderError(EngineErrorCode::MetadataIncomplete, "Exact packet timestamps are missing");
    const uint64_t block_size = block->end - block->payload;
    std::array<uint8_t, 11> header{};
    const auto header_size = static_cast<size_t>((std::min)(block_size, uint64_t{header.size()}));
    const auto read_header = view.ReadAt(block->payload, std::span<uint8_t>(header.data(), header_size));
    if (!read_header.succeeded())
        return *read_header.error();
    if (*read_header.value() != header_size)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Short committed block header");
    const unsigned track_width = VintWidth(header[0], 8);
    if (!track_width || block_size <= track_width + 3 || header_size < track_width + 3)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Invalid committed block header");
    uint64_t track_number = header[0] & ((1u << (8 - track_width)) - 1);
    for (unsigned index = 1; index < track_width; ++index)
        track_number = (track_number << 8) | header[index];
    if (!track_number || track_number > 9 || (header[track_width + 2] & 0x06) != 0)
        return ReaderError(EngineErrorCode::UnsupportedFormat, "Unsupported committed block track or lacing");
    const uint64_t payload_size = block_size - track_width - 3;
    if (payload_size > max_packet_bytes)
        return ReaderError(EngineErrorCode::ResourceBudgetExceeded,
                           "Encoded clip packet exceeds the worker memory budget");
    ClipCommittedPacket packet;
    packet.track_number = track_number;
    packet.is_key = !referenced;
    packet.timestamps = *exact;
    try {
        packet.bytes.resize(static_cast<size_t>(payload_size));
    } catch (const std::bad_alloc&) {
        return ReaderError(EngineErrorCode::ResourceBudgetExceeded, "Cannot allocate committed clip packet");
    }
    const auto read_packet = view.ReadAt(block->payload + track_width + 3, packet.bytes);
    if (!read_packet.succeeded())
        return *read_packet.error();
    if (*read_packet.value() != payload_size)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Short committed clip packet");
    return std::move(packet);
}

} // namespace

ClipCommittedPacketReader::ClipCommittedPacketReader(CommittedMatroskaView& view, uint64_t first_cluster_offset,
                                                     size_t max_packet_bytes)
    : view_(view), position_(first_cluster_offset), cluster_end_(first_cluster_offset),
      max_packet_bytes_(max_packet_bytes) {
}

Outcome<std::optional<ClipCommittedPacket>> ClipCommittedPacketReader::Next() {
    if (position_ > view_.size())
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Committed cluster starts beyond the view");
    while (position_ < view_.size()) {
        if (position_ == cluster_end_) {
            const auto cluster = ReadElement(view_, position_, view_.size());
            if (!cluster.succeeded())
                return *cluster.error();
            if (cluster.value()->id != kCluster)
                return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Committed extent is not a cluster");
            position_ = cluster.value()->payload;
            cluster_end_ = cluster.value()->end;
            continue;
        }
        if (position_ > cluster_end_)
            return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Committed cluster boundary changed");
        const auto element = ReadElement(view_, position_, cluster_end_);
        if (!element.succeeded())
            return *element.error();
        position_ = element.value()->end;
        if (element.value()->id == kSimpleBlock &&
            element.value()->end - element.value()->payload > max_packet_bytes_ + 11ULL)
            return ReaderError(EngineErrorCode::ResourceBudgetExceeded,
                               "Encoded clip packet exceeds the worker memory budget");
        if (element.value()->id == kSimpleBlock)
            return ReaderError(EngineErrorCode::MetadataIncomplete, "Exact packet timestamps are missing");
        if (element.value()->id == kBlockGroup) {
            auto packet = ReadGroup(view_, *element.value(), max_packet_bytes_);
            if (!packet.succeeded())
                return *packet.error();
            return std::optional<ClipCommittedPacket>{std::move(*packet.value())};
        }
    }
    if (position_ != cluster_end_)
        return ReaderError(EngineErrorCode::SourceIntegrityFailure, "Committed cluster is incomplete");
    return std::optional<ClipCommittedPacket>{};
}

} // namespace exosnap::engine

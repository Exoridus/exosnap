#include "recording_media_index.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <windows.h>

namespace exosnap::engine {
namespace {

using Json = nlohmann::json;
constexpr size_t kQueueSlots = 1024;
constexpr size_t kQueueBytes = 4ULL * 1024 * 1024;
constexpr size_t kCacheBytes = 16ULL * 1024 * 1024;
constexpr size_t kRecordHeader = 28;
constexpr uint32_t kMagic = 0x58494d45;

EngineError IndexError(const char* context, EngineErrorCode code = EngineErrorCode::IndexNotReady) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::MediaIndex;
    error.context = context;
    return error;
}

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

bool Identity(HANDLE file, FileIdentity& result) {
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(file, FileIdInfo, &info, sizeof(info)))
        return false;
    result.volume_serial = info.VolumeSerialNumber;
    std::copy(std::begin(info.FileId.Identifier), std::end(info.FileId.Identifier), result.file_id.begin());
    return true;
}

uint32_t Crc(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void Put(std::span<uint8_t> bytes, size_t offset, uint64_t value, size_t count) {
    for (size_t index = 0; index < count; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
}
uint64_t Get(std::span<const uint8_t> bytes, size_t offset, size_t count) {
    uint64_t result = 0;
    for (size_t index = 0; index < count; ++index)
        result |= static_cast<uint64_t>(bytes[offset + index]) << (index * 8);
    return result;
}

template <typename T> T Checked(const Json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<uint64_t>();
        if (number > static_cast<uint64_t>((std::numeric_limits<T>::max)()))
            throw std::overflow_error("Index integer exceeds its range");
        return static_cast<T>(number);
    }
    if (!value.is_number_integer())
        throw std::runtime_error("Index counter is not an integer");
    const auto number = value.get<int64_t>();
    if constexpr (std::is_unsigned_v<T>) {
        if (number < 0 || static_cast<uint64_t>(number) > (std::numeric_limits<T>::max)())
            throw std::overflow_error("Index counter exceeds its range");
    } else if (number < (std::numeric_limits<T>::min)() || number > (std::numeric_limits<T>::max)()) {
        throw std::overflow_error("Index timestamp exceeds its range");
    }
    return static_cast<T>(number);
}

std::vector<uint8_t> Bytes(const Json& value) {
    if (!value.is_binary() || value.size() > kQueueBytes)
        throw std::runtime_error("Invalid index byte string");
    const auto& bytes = value.get_binary();
    return {bytes.begin(), bytes.end()};
}

bool CheckCborItem(std::span<const uint8_t> bytes, size_t& cursor, unsigned depth) {
    if (cursor >= bytes.size() || depth > 32)
        return false;
    const uint8_t initial = bytes[cursor++];
    const unsigned major = initial >> 5;
    const unsigned additional = initial & 31;
    uint64_t argument = additional;
    if (additional >= 24) {
        if (additional > 27)
            return false;
        const size_t count = size_t{1} << (additional - 24);
        if (count > bytes.size() - cursor)
            return false;
        argument = 0;
        for (size_t byte = 0; byte < count; ++byte)
            argument = (argument << 8) | bytes[cursor++];
    }
    if (major < 2)
        return major != 1 || argument <= static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    if (major == 2 || major == 3) {
        if (argument > bytes.size() - cursor)
            return false;
        cursor += static_cast<size_t>(argument);
        return true;
    }
    if (major == 4 || major == 5) {
        const size_t factor = major == 5 ? 2 : 1;
        if (argument > (bytes.size() - cursor) / factor)
            return false;
        for (uint64_t index = 0; index < argument * factor; ++index)
            if (!CheckCborItem(bytes, cursor, depth + 1))
                return false;
        return true;
    }
    return major == 7 && (additional == 20 || additional == 21 || additional == 22 || additional == 25 ||
                          additional == 26 || additional == 27);
}

Json ParsePayload(std::span<const uint8_t> payload) {
    size_t cursor = 0;
    if (!CheckCborItem(payload, cursor, 0) || cursor != payload.size())
        throw std::runtime_error("Index CBOR bounds or depth exceeded");
    return Json::from_cbor(payload.begin(), payload.end(), true, true);
}

Json Codec(const MediaCodecDescriptor& codec) {
    return Json::array({codec.codec_id, Json::binary(codec.codec_private), codec.sample_rate, codec.channels,
                        codec.bit_depth, codec.codec_delay_samples});
}
MediaCodecDescriptor ReadCodec(const Json& value) {
    if (!value.is_array() || value.size() != 6 || value[1].size() > kQueueBytes)
        throw std::runtime_error("Invalid codec descriptor");
    return {value[0].get<std::string>(), Bytes(value[1]),
            Checked<uint32_t>(value[2]), Checked<uint32_t>(value[3]),
            Checked<uint32_t>(value[4]), Checked<uint32_t>(value[5])};
}

Json Descriptor(const RecordingMediaDescriptor& value) {
    Json tracks = Json::array();
    for (const auto& track : value.audio_tracks) {
        Json sources = Json::array();
        for (const auto& source : track.source_ids)
            sources.push_back(ToUuid(source));
        tracks.push_back(Json::array({ToUuid(track.track_id), track.stream_index.value, track.track_uid, track.name,
                                      track.role, Codec(track.codec), sources}));
    }
    Json monitor = Json::array();
    for (const auto& id : value.default_monitor_track_ids)
        monitor.push_back(ToUuid(id));
    const auto& c = value.color;
    Json color = Json::array(
        {static_cast<uint8_t>(c.primaries), static_cast<uint8_t>(c.transfer), static_cast<uint8_t>(c.matrix),
         static_cast<uint8_t>(c.range), c.bits_per_channel, c.hdr, c.max_content_light_level,
         c.max_frame_average_light_level, c.has_mastering_display, c.mastering_display_primary_r_x,
         c.mastering_display_primary_r_y, c.mastering_display_primary_g_x, c.mastering_display_primary_g_y,
         c.mastering_display_primary_b_x, c.mastering_display_primary_b_y, c.mastering_display_white_point_x,
         c.mastering_display_white_point_y, c.mastering_display_max_luminance, c.mastering_display_min_luminance});
    return Json::array(
        {ToUuid(value.recording_id), ToUuid(value.media_id), ToUuid(value.segment_id), value.source_timeline_revision,
         value.writer_generation, value.descriptor_revision, value.file_identity.volume_serial,
         Json::binary(std::vector<uint8_t>(value.file_identity.file_id.begin(), value.file_identity.file_id.end())),
         value.path, Codec(value.video), tracks, monitor, color, value.segment_origin_r_ns, value.continuity_group,
         Json::binary(value.immutable_header)});
}

template <typename Id> Id ReadId(const Json& value) {
    const auto result = FromUuid<Id>(value.get<std::string>());
    if (!result || *result == Id{})
        throw std::runtime_error("Invalid media identity");
    return *result;
}

RecordingMediaDescriptor ReadDescriptor(const Json& value) {
    if (!value.is_array() || value.size() != 16 || value[10].size() > 8 || value[11].size() > 8 ||
        value[12].size() != 19 || value[15].size() > kQueueBytes)
        throw std::runtime_error("Invalid media descriptor");
    RecordingMediaDescriptor result;
    result.recording_id = ReadId<RecordingId>(value[0]);
    result.media_id = ReadId<MediaId>(value[1]);
    result.segment_id = ReadId<SegmentId>(value[2]);
    result.source_timeline_revision = Checked<uint64_t>(value[3]);
    result.writer_generation = Checked<uint64_t>(value[4]);
    result.descriptor_revision = Checked<uint64_t>(value[5]);
    result.file_identity.volume_serial = Checked<uint64_t>(value[6]);
    const auto file_id = Bytes(value[7]);
    if (file_id.size() != result.file_identity.file_id.size())
        throw std::runtime_error("Invalid file identity");
    std::copy(file_id.begin(), file_id.end(), result.file_identity.file_id.begin());
    result.path = value[8].get<std::string>();
    result.video = ReadCodec(value[9]);
    for (const auto& item : value[10]) {
        if (item.size() != 7 || item[6].size() > 1024)
            throw std::runtime_error("Invalid audio track descriptor");
        ContainerAudioTrack track;
        track.track_id = ReadId<AudioTrackId>(item[0]);
        track.stream_index.value = Checked<uint32_t>(item[1]);
        track.track_uid = Checked<uint64_t>(item[2]);
        track.name = item[3].get<std::string>();
        track.role = item[4].get<std::string>();
        track.codec = ReadCodec(item[5]);
        for (const auto& id : item[6])
            track.source_ids.push_back(ReadId<AudioSourceId>(id));
        result.audio_tracks.push_back(std::move(track));
    }
    for (const auto& id : value[11])
        result.default_monitor_track_ids.push_back(ReadId<AudioTrackId>(id));
    const auto& c = value[12];
    result.color.primaries = static_cast<ColorPrimaries>(Checked<uint8_t>(c[0]));
    result.color.transfer = static_cast<TransferCharacteristics>(Checked<uint8_t>(c[1]));
    result.color.matrix = static_cast<MatrixCoefficients>(Checked<uint8_t>(c[2]));
    result.color.range = static_cast<ColorRange>(Checked<uint8_t>(c[3]));
    result.color.bits_per_channel = Checked<uint32_t>(c[4]);
    result.color.hdr = c[5].get<bool>();
    result.color.max_content_light_level = Checked<uint32_t>(c[6]);
    result.color.max_frame_average_light_level = Checked<uint32_t>(c[7]);
    result.color.has_mastering_display = c[8].get<bool>();
    float* values[] = {&result.color.mastering_display_primary_r_x,   &result.color.mastering_display_primary_r_y,
                       &result.color.mastering_display_primary_g_x,   &result.color.mastering_display_primary_g_y,
                       &result.color.mastering_display_primary_b_x,   &result.color.mastering_display_primary_b_y,
                       &result.color.mastering_display_white_point_x, &result.color.mastering_display_white_point_y,
                       &result.color.mastering_display_max_luminance, &result.color.mastering_display_min_luminance};
    for (size_t index = 0; index < std::size(values); ++index)
        *values[index] = c[9 + index].get<float>();
    result.segment_origin_r_ns = Checked<int64_t>(value[13]);
    result.continuity_group = Checked<uint64_t>(value[14]);
    result.immutable_header = Bytes(value[15]);
    if (result.writer_generation == 0 || result.descriptor_revision == 0 || result.file_identity == FileIdentity{} ||
        result.immutable_header.empty())
        throw std::runtime_error("Incomplete media descriptor");
    for (size_t index = 0; index < result.audio_tracks.size(); ++index) {
        const auto& track = result.audio_tracks[index];
        if (track.stream_index.value != index || track.track_uid != index + 2)
            throw std::runtime_error("Inconsistent audio track mapping");
        for (size_t prior = 0; prior < index; ++prior)
            if (track.track_id == result.audio_tracks[prior].track_id)
                throw std::runtime_error("Duplicate audio track identity");
    }
    return result;
}

Json Receipt(const CommittedMediaReceipt& receipt) {
    Json clusters = Json::array();
    for (const auto& extent : receipt.clusters) {
        Json coverage = Json::array(), raps = Json::array();
        for (const auto& c : extent.coverage)
            coverage.push_back(Json::array({c.track_number, c.begin_pts_ns, c.end_pts_ns, c.min_dts_ns, c.max_dts_ns}));
        for (const auto& r : extent.random_access_points)
            raps.push_back(Json::array(
                {r.track_number, r.cluster_offset, r.block_offset, r.pts_ns, r.dts_ns, static_cast<unsigned>(r.kind)}));
        clusters.push_back(Json::array({extent.file_begin, extent.file_end, coverage, raps}));
    }
    return Json::array({ToUuid(receipt.recording_id), ToUuid(receipt.segment_id), receipt.writer_generation,
                        receipt.commit_sequence, receipt.descriptor_revision, receipt.readable_end_offset,
                        receipt.durable_end_offset, clusters});
}

CommittedMediaReceipt ReadReceipt(const Json& value) {
    if (!value.is_array() || value.size() != 8 || value[7].size() > 1024)
        throw std::runtime_error("Invalid media receipt");
    CommittedMediaReceipt result{ReadId<RecordingId>(value[0]), ReadId<SegmentId>(value[1]),
                                 Checked<uint64_t>(value[2]),   Checked<uint64_t>(value[3]),
                                 Checked<uint64_t>(value[4]),   Checked<uint64_t>(value[5]),
                                 Checked<uint64_t>(value[6]),   {}};
    if (result.durable_end_offset > result.readable_end_offset || result.commit_sequence == 0 ||
        result.writer_generation == 0 || result.descriptor_revision == 0 || value[7].empty())
        throw std::runtime_error("Invalid durable boundary");
    uint64_t previous = 0;
    for (const auto& item : value[7]) {
        if (item.size() != 4 || item[2].size() > 9 || item[3].size() > 65536)
            throw std::runtime_error("Invalid cluster record");
        ClusterExtent extent{Checked<uint64_t>(item[0]), Checked<uint64_t>(item[1]), {}, {}};
        if (extent.file_begin < previous || extent.file_end <= extent.file_begin ||
            extent.file_end > result.readable_end_offset)
            throw std::runtime_error("Invalid cluster boundary");
        for (const auto& c : item[2]) {
            if (c.size() != 5)
                throw std::runtime_error("Invalid coverage record");
            MediaTrackCoverage coverage{Checked<uint64_t>(c[0]), Checked<int64_t>(c[1]), Checked<int64_t>(c[2]),
                                        Checked<int64_t>(c[3]), Checked<int64_t>(c[4])};
            if (coverage.track_number < 1 || coverage.track_number > 9 || coverage.end_pts_ns < coverage.begin_pts_ns ||
                coverage.max_dts_ns < coverage.min_dts_ns)
                throw std::runtime_error("Invalid track coverage");
            extent.coverage.push_back(coverage);
        }
        for (const auto& r : item[3]) {
            if (r.size() != 6)
                throw std::runtime_error("Invalid random-access record");
            MediaRandomAccessPoint rap{Checked<uint64_t>(r[0]), Checked<uint64_t>(r[1]),
                                       Checked<uint64_t>(r[2]), Checked<int64_t>(r[3]),
                                       Checked<int64_t>(r[4]),  static_cast<RandomAccessKind>(Checked<unsigned>(r[5]))};
            if (rap.track_number < 1 || rap.track_number > 9 || rap.cluster_offset != extent.file_begin ||
                rap.block_offset < extent.file_begin || rap.block_offset >= extent.file_end ||
                static_cast<unsigned>(rap.kind) > static_cast<unsigned>(RandomAccessKind::CodecQualified))
                throw std::runtime_error("Invalid random-access offset");
            extent.random_access_points.push_back(rap);
        }
        previous = extent.file_end;
        result.clusters.push_back(std::move(extent));
    }
    return result;
}

void ValidateReceipt(const CommittedMediaReceipt& receipt, const RecordingMediaDescriptor& descriptor,
                     uint64_t last_commit, uint64_t last_readable) {
    const uint64_t expected_begin = last_readable == 0 ? descriptor.immutable_header.size() : last_readable;
    if (receipt.recording_id != descriptor.recording_id || receipt.segment_id != descriptor.segment_id ||
        receipt.writer_generation != descriptor.writer_generation ||
        receipt.descriptor_revision != descriptor.descriptor_revision || receipt.commit_sequence <= last_commit ||
        receipt.readable_end_offset < last_readable || receipt.clusters.empty() ||
        receipt.clusters.front().file_begin != expected_begin)
        throw std::runtime_error("Receipt generation, sequence or prefix mismatch");
}

size_t ReceiptBytes(const CommittedMediaReceipt& receipt) {
    size_t bytes = sizeof(receipt) + receipt.clusters.capacity() * sizeof(ClusterExtent);
    for (const auto& extent : receipt.clusters) {
        if (extent.coverage.capacity() > kQueueBytes / sizeof(MediaTrackCoverage) ||
            extent.random_access_points.capacity() > kQueueBytes / sizeof(MediaRandomAccessPoint))
            return kQueueBytes + 1;
        bytes += extent.coverage.capacity() * sizeof(MediaTrackCoverage) +
                 extent.random_access_points.capacity() * sizeof(MediaRandomAccessPoint);
        if (bytes > kQueueBytes)
            return bytes;
    }
    return bytes;
}

Outcome<size_t> ReadJournal(const std::filesystem::path& path, uint64_t end, bool recover, const FileIdentity* identity,
                            const std::function<bool(const RecordingMediaDescriptor&)>& descriptor_visitor,
                            const std::function<bool(const CommittedMediaReceipt&)>& receipt_visitor) {
    Handle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    FileIdentity actual;
    if (file.value == INVALID_HANDLE_VALUE || !Identity(file.value, actual) || (identity && *identity != actual))
        return IndexError("Index journal identity unavailable or changed", EngineErrorCode::SourceIntegrityFailure);
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.value, &length) || length.QuadPart < 0)
        return IndexError("Cannot read journal size");
    const uint64_t available = static_cast<uint64_t>(length.QuadPart);
    if (!recover && end > available)
        return IndexError("Snapshot journal was truncated", EngineErrorCode::SourceIntegrityFailure);
    end = recover ? available : end;
    uint64_t cursor = 0, sequence = 0;
    size_t count = 0;
    uint64_t last_commit = 0, last_readable = 0;
    std::optional<RecordingMediaDescriptor> descriptor;
    try {
        while (cursor < end) {
            if (end - cursor < kRecordHeader)
                return recover ? Outcome<size_t>{count} : Outcome<size_t>{IndexError("Truncated journal header")};
            std::array<uint8_t, kRecordHeader> header{};
            DWORD read = 0;
            if (!ReadFile(file.value, header.data(), static_cast<DWORD>(header.size()), &read, nullptr) ||
                read != header.size())
                return IndexError("Cannot read journal header");
            const uint64_t payload_size = Get(header, 12, 4);
            const uint64_t next_sequence = Get(header, 16, 8);
            const uint64_t type = Get(header, 8, 4);
            if (Get(header, 0, 4) != kMagic || Get(header, 4, 4) != 1 || (type != 1 && type != 2) ||
                payload_size > kQueueBytes || next_sequence != sequence + 1)
                return IndexError("Invalid journal record header", EngineErrorCode::SourceIntegrityFailure);
            if (payload_size > end - cursor - kRecordHeader)
                return recover ? Outcome<size_t>{count} : Outcome<size_t>{IndexError("Truncated journal payload")};
            std::vector<uint8_t> payload(static_cast<size_t>(payload_size));
            if (!ReadFile(file.value, payload.data(), static_cast<DWORD>(payload.size()), &read, nullptr) ||
                read != payload.size())
                return IndexError("Cannot read journal payload");
            if (Crc(payload) != Get(header, 24, 4))
                return IndexError("Index journal CRC mismatch", EngineErrorCode::SourceIntegrityFailure);
            auto value = ParsePayload(payload);
            bool keep = true;
            if (type == 1) {
                auto next = ReadDescriptor(value);
                if (descriptor && descriptor->recording_id != next.recording_id)
                    throw std::runtime_error("Journal recording identity changed");
                descriptor = std::move(next);
                last_readable = 0;
                if (descriptor_visitor)
                    keep = descriptor_visitor(*descriptor);
            } else {
                auto receipt = ReadReceipt(value);
                if (!descriptor)
                    throw std::runtime_error("Missing journal media descriptor");
                ValidateReceipt(receipt, *descriptor, last_commit, last_readable);
                last_commit = receipt.commit_sequence;
                last_readable = receipt.readable_end_offset;
                if (receipt_visitor)
                    keep = receipt_visitor(receipt);
            }
            ++count;
            if (!keep)
                break;
            sequence = next_sequence;
            cursor += kRecordHeader + payload_size;
        }
    } catch (...) {
        return IndexError("Invalid journal payload", EngineErrorCode::SourceIntegrityFailure);
    }
    return count;
}

} // namespace

struct RecordingMediaIndex::State {
    struct Entry {
        std::shared_ptr<const RecordingMediaDescriptor> descriptor;
        CommittedMediaReceipt receipt;
        size_t bytes = 0;
    };
    std::array<std::optional<Entry>, kQueueSlots> queue;
    std::atomic<uint64_t> written{0}, consumed{0}, committed{0};
    std::atomic<size_t> payload_bytes{0}, cached_bytes{0};
    std::atomic<bool> rebuild{false}, stopping{false};
    std::condition_variable wake;
    std::mutex wait_mutex, snapshot_mutex;
    std::filesystem::path path;
    std::optional<MediaIndexSnapshot> snapshot;

    ReceiptAcceptance Enqueue(Entry entry) noexcept {
        const uint64_t write = written.load(std::memory_order_relaxed);
        if (rebuild.load(std::memory_order_acquire) || stopping.load(std::memory_order_acquire) ||
            entry.bytes > kQueueBytes || entry.bytes > kQueueBytes - payload_bytes.load(std::memory_order_acquire) ||
            write - consumed.load(std::memory_order_acquire) >= kQueueSlots) {
            rebuild.store(true, std::memory_order_release);
            return ReceiptAcceptance::IndexDegraded;
        }
        payload_bytes.fetch_add(entry.bytes, std::memory_order_relaxed);
        queue[write % kQueueSlots].emplace(std::move(entry));
        written.store(write + 1, std::memory_order_release);
        wake.notify_one();
        return ReceiptAcceptance::Accepted;
    }

    void Run() noexcept {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        Handle file;
        file.value = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        FileIdentity identity;
        if (file.value == INVALID_HANDLE_VALUE || !Identity(file.value, identity)) {
            rebuild.store(true, std::memory_order_release);
            return;
        }
        uint64_t sequence = 0, end = 0, last_commit = 0, last_readable = 0;
        std::shared_ptr<const RecordingMediaDescriptor> descriptor;
        std::deque<std::vector<uint8_t>> cache;
        try {
            for (;;) {
                const uint64_t read = consumed.load(std::memory_order_relaxed);
                if (read == written.load(std::memory_order_acquire)) {
                    if (stopping.load(std::memory_order_acquire))
                        break;
                    std::unique_lock lock(wait_mutex);
                    wake.wait_for(lock, std::chrono::milliseconds(50));
                    continue;
                }
                auto entry = std::move(*queue[read % kQueueSlots]);
                queue[read % kQueueSlots].reset();
                consumed.store(read + 1, std::memory_order_release);
                payload_bytes.fetch_sub(entry.bytes, std::memory_order_release);
                if (rebuild.load(std::memory_order_acquire))
                    continue;
                const uint32_t type = entry.descriptor ? 1 : 2;
                if (entry.descriptor) {
                    if (descriptor && descriptor->recording_id != entry.descriptor->recording_id)
                        throw std::runtime_error("Index recording identity changed");
                    descriptor = std::move(entry.descriptor);
                    last_readable = 0;
                } else {
                    const auto& receipt = entry.receipt;
                    if (!descriptor)
                        throw std::runtime_error("Missing index media descriptor");
                    ValidateReceipt(receipt, *descriptor, last_commit, last_readable);
                    last_commit = receipt.commit_sequence;
                    last_readable = receipt.readable_end_offset;
                }
                auto payload = Json::to_cbor(type == 1 ? Descriptor(*descriptor) : Receipt(entry.receipt));
                if (payload.size() > kQueueBytes)
                    throw std::length_error("Index record budget exceeded");
                // Round-trip validation applies the same limits to producer and persisted records.
                if (type == 1)
                    (void)ReadDescriptor(Json::from_cbor(payload));
                else
                    (void)ReadReceipt(Json::from_cbor(payload));
                std::array<uint8_t, kRecordHeader> header{};
                Put(header, 0, kMagic, 4);
                Put(header, 4, 1, 4);
                Put(header, 8, type, 4);
                Put(header, 12, payload.size(), 4);
                Put(header, 16, ++sequence, 8);
                Put(header, 24, Crc(payload), 4);
                DWORD count = 0;
                if (!WriteFile(file.value, header.data(), static_cast<DWORD>(header.size()), &count, nullptr) ||
                    count != header.size() ||
                    !WriteFile(file.value, payload.data(), static_cast<DWORD>(payload.size()), &count, nullptr) ||
                    count != payload.size())
                    throw std::runtime_error("Index journal write failed");
                end += header.size() + payload.size();
                while (!cache.empty() && cached_bytes.load() + payload.capacity() + sizeof(payload) > kCacheBytes) {
                    cached_bytes.fetch_sub(cache.front().capacity() + sizeof(payload));
                    cache.pop_front();
                }
                cached_bytes.fetch_add(payload.capacity() + sizeof(payload));
                cache.push_back(std::move(payload));
                if (type == 2) {
                    MediaIndexSnapshot next;
                    next.token = {
                        descriptor->recording_id, last_commit, descriptor->descriptor_revision, sequence,
                        Nanoseconds{static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                              std::chrono::steady_clock::now().time_since_epoch())
                                                              .count())}};
                    next.journal_path = path;
                    next.journal_identity = identity;
                    next.journal_end = end;
                    next.descriptor = descriptor;
                    {
                        std::lock_guard lock(snapshot_mutex);
                        snapshot = std::move(next);
                    }
                    committed.store(last_commit, std::memory_order_release);
                }
            }
        } catch (...) {
            rebuild.store(true, std::memory_order_release);
        }
    }
};

RecordingMediaIndex::RecordingMediaIndex(std::filesystem::path journal_path) : state_(std::make_shared<State>()) {
    state_->path = std::move(journal_path);
    std::thread([state = state_] { state->Run(); }).detach();
}
RecordingMediaIndex::~RecordingMediaIndex() {
    state_->stopping.store(true, std::memory_order_release);
    state_->wake.notify_one();
}
ReceiptAcceptance
RecordingMediaIndex::RegisterDescriptor(std::shared_ptr<const RecordingMediaDescriptor> descriptor) noexcept {
    if (!descriptor || descriptor->audio_tracks.size() > 8 || descriptor->immutable_header.capacity() > kQueueBytes ||
        descriptor->video.codec_private.capacity() > kQueueBytes) {
        state_->rebuild.store(true, std::memory_order_release);
        return ReceiptAcceptance::IndexDegraded;
    }
    size_t bytes = sizeof(RecordingMediaDescriptor) + descriptor->immutable_header.capacity() +
                   descriptor->video.codec_private.capacity() + descriptor->path.capacity();
    for (const auto& track : descriptor->audio_tracks) {
        if (track.source_ids.size() > 1024 || track.codec.codec_private.capacity() > kQueueBytes) {
            state_->rebuild.store(true, std::memory_order_release);
            return ReceiptAcceptance::IndexDegraded;
        }
        bytes += sizeof(track) + track.codec.codec_private.capacity() + track.name.capacity() + track.role.capacity() +
                 track.source_ids.capacity() * sizeof(AudioSourceId);
    }
    return state_->Enqueue({std::move(descriptor), {}, bytes});
}
ReceiptAcceptance RecordingMediaIndex::PublishCommitted(CommittedMediaReceipt receipt) noexcept {
    const size_t bytes = ReceiptBytes(receipt);
    return state_->Enqueue({{}, std::move(receipt), bytes});
}
Outcome<MediaIndexSnapshot> RecordingMediaIndex::Freeze() const {
    std::lock_guard lock(state_->snapshot_mutex);
    return state_->snapshot ? Outcome<MediaIndexSnapshot>{*state_->snapshot}
                            : Outcome<MediaIndexSnapshot>{IndexError("No confirmed media index snapshot")};
}
bool RecordingMediaIndex::needs_rebuild() const noexcept {
    return state_->rebuild.load(std::memory_order_acquire);
}
uint64_t RecordingMediaIndex::journal_committed_through() const noexcept {
    return state_->committed.load(std::memory_order_acquire);
}
size_t RecordingMediaIndex::queued_bytes() const noexcept {
    return state_->payload_bytes.load(std::memory_order_acquire);
}
size_t RecordingMediaIndex::cache_bytes() const noexcept {
    return state_->cached_bytes.load(std::memory_order_acquire);
}
Outcome<size_t>
RecordingMediaIndex::Visit(const MediaIndexSnapshot& snapshot,
                           const std::function<bool(const RecordingMediaDescriptor&)>& descriptor_visitor,
                           const std::function<bool(const CommittedMediaReceipt&)>& receipt_visitor) {
    return ReadJournal(snapshot.journal_path, snapshot.journal_end, false, &snapshot.journal_identity,
                       descriptor_visitor, receipt_visitor);
}
Outcome<size_t> RecordingMediaIndex::InspectJournal(const std::filesystem::path& path) {
    return ReadJournal(path, 0, true, nullptr, {}, {});
}

} // namespace exosnap::engine

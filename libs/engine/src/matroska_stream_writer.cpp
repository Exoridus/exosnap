#include "matroska_stream_writer.h"
#include "audio_media_manifest_adapter.h"
#include "codec_random_access.h"
#include "matroska_packet_timestamps.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/recording_media_types.h>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ratio>
#include <utility>
#include <vector>

// libebml / libmatroska headers — suppress MSVC warnings from third-party code
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267) // size_t to smaller integer conversion
#pragma warning(disable : 4244) // possible data loss in conversion
#pragma warning(disable : 4245) // signed/unsigned mismatch in conversion
#pragma warning(disable : 4100) // unreferenced formal parameter
#endif
#include <ebml/EbmlFloat.h>
#include <ebml/EbmlHead.h>
#include <ebml/EbmlMaster.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/EbmlVoid.h>
#include <ebml/IOCallback.h>
#include <ebml/c/libebml_t.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxBlockData.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxCues.h>
#include <matroska/KaxCuesData.h>
#include <matroska/KaxSeekHead.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxSemantic.h>
#include <matroska/KaxTracks.h>
#include <matroska/KaxTypes.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <exosnap/engine/logging/logging.h>
#include <exosnap/engine/portable_file_io.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>

namespace exosnap::engine {

namespace {

// A half-millisecond tick represents Opus's 6.5 ms initial pre-skip exactly.
constexpr uint64_t kTimecodeScaleNs = 500000ULL;
constexpr uint64_t kClusterBoundaryTicks = 2'000'000'000ULL / kTimecodeScaleNs;
constexpr int64_t kMaxClusterRelativeTicks = 32767LL;
// Space reserved for the SeekHead placeholder at the start of the Segment.
constexpr uint64_t kSeekHeadReservedBytes = 150ULL;
constexpr size_t kReceiptPayloadLimit = 4ULL * 1024 * 1024;
constexpr size_t kPendingClusterLimit = 1024;
constexpr size_t kClipClusterPacketLimit = 512;
constexpr size_t kClipGroupObjectBytes =
    sizeof(libmatroska::KaxBlockGroup) + sizeof(libmatroska::KaxBlock) + sizeof(libmatroska::KaxReferencePriority) +
    sizeof(libmatroska::KaxReferenceBlock) + sizeof(libmatroska::KaxBlockDuration) +
    sizeof(libmatroska::KaxBlockAdditions) + sizeof(libmatroska::KaxBlockMore) + sizeof(libmatroska::KaxBlockAddID) +
    sizeof(libmatroska::KaxBlockAdditional) + sizeof(libmatroska::DataBuffer);
constexpr size_t kClipSimpleObjectBytes = sizeof(libmatroska::KaxSimpleBlock) + sizeof(libmatroska::DataBuffer);
constexpr size_t kClipBlockGraphBytes = (std::max)(kClipGroupObjectBytes + 10 * 64 + 16 * sizeof(void*) + 24,
                                                   kClipSimpleObjectBytes + 2 * 64 + 4 * sizeof(void*));
constexpr size_t kClipClusterGraphBytes =
    kClipClusterPacketLimit * (kClipBlockGraphBytes + 3 * sizeof(libebml::EbmlElement*) +
                               3 * sizeof(std::vector<uint8_t>) + 3 * sizeof(uint64_t)) +
    sizeof(libmatroska::KaxCluster) + 64;
static_assert(kClipClusterGraphBytes < 2 * 1024 * 1024);

class StreamedCues final : public libmatroska::KaxCues {
  public:
    uint64_t payload_bytes = 0;

    void RenderHeader(libebml::IOCallback& output, uint64_t bytes) {
        payload_bytes = bytes;
        RenderHead(output, true);
    }

    uint64_t UpdateSize(bool = false, bool = false) override {
        SetSize_(payload_bytes);
        return payload_bytes;
    }
};

// Width of each reserved MaxCLL / MaxFALL leaf. CTA-861.3 codes both as u(16),
// so two bytes hold every representable level and the back-patch at Finalize()
// can never need more room than the header reserved.
constexpr uint64_t kContentLightLevelReservedBytes = 2ULL;

// A level that does not fit the reservation saturates rather than overflowing
// into a wider element the back-patch has no room for. The ceiling is also the
// largest value CTA-861.3 can express, so nothing representable is lost.
constexpr uint64_t kContentLightLevelCeiling = 0xFFFFULL;

uint64_t ClampContentLightLevel(uint32_t nits) noexcept {
    const uint64_t v = static_cast<uint64_t>(nits);
    return v > kContentLightLevelCeiling ? kContentLightLevelCeiling : v;
}

constexpr const char* kLogComponent = "matroska_stream_writer";

void LogDurabilityFlushFailure(const char* message, const std::string& reason) {
    logging::LogField fields[] = {{"error", reason}};
    logging::log(logging::LogLevel::Warn, kLogComponent, message, std::span<const logging::LogField>(fields, 1));
}

} // namespace

// Owns the output FILE* directly, rather than via libebml::StdIOCallback: that
// class's FILE* member is private with no accessor, so there is no way to reach
// the CRT stdio buffer (for fflush) or the underlying OS HANDLE (for
// FlushFileBuffers) through it. Read/write/seek semantics below mirror
// StdIOCallback exactly -- this is plumbing for durability, not a behavior
// change.
//
// The initial open is exclusive-create (_O_CREAT | _O_EXCL), NOT the
// truncate-on-exists "wb+" fopen mode StdIOCallback would use. Opening in
// truncate mode would silently overwrite a file some other writer (or a
// leftover from a previous run) created in the gap between the caller's probe
// and this open (the TOCTOU window). Exclusive creation makes that race fail
// loudly (EEXIST) through the normal Open()-failure path instead.
//
// `pre_reserved` inverts only who performed that exclusive create. The
// application layer reserves the .partial artifact with CREATE_NEW before the
// engine starts, so segment 0 is handed a path whose exclusivity is already
// established and whose file therefore exists. Demanding _O_EXCL again would
// fail against the caller's own reservation. The open then requires the file
// to be there (no _O_CREAT): a reservation that is claimed but absent means the
// two sides disagree about the path, which must surface rather than be papered
// over by creating a second file.
class DurableFileIo final : public libebml::IOCallback {
  public:
    DurableFileIo(const std::string& path, bool pre_reserved, const std::function<bool(OutputIoOperation)>& fail_io,
                  const std::function<bool(OutputIoOperation, uint64_t)>& before_write,
                  const std::function<void(OutputIoOperation, uint64_t)>& after_write, OutputIoTimings& timings)
        : m_fail_io(fail_io), m_before_write(before_write), m_after_write(after_write), m_timings(timings) {
        const int flags = pre_reserved ? (_O_TRUNC | _O_RDWR | _O_BINARY) : (_O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY);
        int fd = -1;
        const errno_t open_err = _sopen_s(&fd, path.c_str(), flags, _SH_DENYNO, _S_IREAD | _S_IWRITE);
        if (open_err != 0 || fd < 0) {
            const char* what = pre_reserved ? "Can't open the reserved file \"" : "Can't exclusively create file \"";
            throw std::runtime_error(what + path + "\" in mode \"wb+\": " + std::strerror(open_err));
        }
        m_file = _fdopen(fd, "wb+");
        if (m_file == nullptr) {
            _close(fd);
            throw std::runtime_error("Can't associate a stream with file \"" + path + "\"");
        }
    }

    ~DurableFileIo() override {
        // Defensive: CloseIo() normally calls close() explicitly first; this
        // only fires on an unexpected teardown path. Never throw from a
        // destructor.
        if (m_file != nullptr) {
            std::fclose(m_file);
            m_file = nullptr;
        }
    }

    DurableFileIo(const DurableFileIo&) = delete;
    DurableFileIo& operator=(const DurableFileIo&) = delete;

    // NOTE: uint32/int64/uint64 below are libebml's own global typedefs
    // (ebml/c/libebml_t.h) — the signatures must match IOCallback exactly.
    uint32 read(void* buffer, size_t size) override {
        return static_cast<uint32>(std::fread(buffer, 1, size, m_file));
    }

    void setFilePointer(int64 offset, libebml::seek_mode mode = libebml::seek_beginning) override {
        // Fseek64 (_fseeki64), not std::fseek: fseek's offset is a 32-bit `long`
        // on Windows and silently truncates an offset >= 2 GiB instead of
        // failing loudly. A long recording routinely crosses that boundary.
        if (Fseek64(m_file, offset, static_cast<int>(mode)) != 0) {
            throw std::runtime_error("Failed to seek matroska output file");
        }
    }

    size_t write(const void* buffer, size_t size) override {
        if (Fails(OutputIoOperation::PayloadWrite) || (m_phase != OutputIoOperation::PayloadWrite && Fails(m_phase)))
            throw std::runtime_error("Injected output write failure");
        constexpr size_t kThrottledWriteChunk = 4ULL * 1024 * 1024;
        const size_t limit = m_before_write ? kThrottledWriteChunk : size;
        const auto* bytes = static_cast<const uint8_t*>(buffer);
        size_t total_written = 0;
        do {
            const size_t chunk = (std::min)(size - total_written, limit);
            if (m_before_write && !m_before_write(m_phase, chunk))
                throw std::runtime_error("Clip output I/O was cancelled");
            const void* current = chunk ? bytes + total_written : buffer;
            const auto start = std::chrono::steady_clock::now();
            const size_t written = std::fwrite(current, 1, chunk, m_file);
            m_timings.write_ms += ElapsedMs(start);
            if (m_after_write)
                m_after_write(m_phase, written);
            if (m_capture_header) {
                try {
                    const uint64_t end = getFilePointer();
                    if (end > kReceiptPayloadLimit)
                        throw std::length_error("Immutable media header budget exceeded");
                    m_header.resize((std::max)(m_header.size(), static_cast<size_t>(end)));
                    std::memcpy(m_header.data() + static_cast<size_t>(end) - written, current, written);
                } catch (...) {
                    m_header.clear();
                    m_capture_header = false;
                }
            }
            total_written += written;
            if (written != chunk)
                break;
        } while (total_written < size);
        return total_written;
    }

    uint64 getFilePointer() override {
        // Ftell64 (_ftelli64), not std::ftell: ftell returns a 32-bit `long`
        // on Windows and fails once the file position reaches 2 GiB, aborting
        // the in-progress Matroska trailer write for every recording past
        // that size.
        const int64_t pos = Ftell64(m_file);
        if (pos < 0) {
            throw std::runtime_error("Can't tell the current matroska output file position");
        }
        return static_cast<uint64>(pos);
    }

    void close() override {
        if (m_file == nullptr)
            return;
        FILE* file = m_file;
        m_file = nullptr;
        const int result = std::fclose(file);
        if (result != 0 || Fails(OutputIoOperation::Close)) {
            throw std::runtime_error("Can't close matroska output file");
        }
    }

    enum class FlushResult { Success, StreamFailure, DurabilityFailure };
    std::vector<uint8_t> TakeHeader() {
        m_capture_header = false;
        return std::move(m_header);
    }
    bool GetIdentity(FileIdentity& identity) const noexcept {
        FILE_ID_INFO info{};
        const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<intptr_t>(_get_osfhandle(_fileno(m_file))));
        if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info)))
            return false;
        identity.volume_serial = info.VolumeSerialNumber;
        std::copy(std::begin(info.FileId.Identifier), std::end(info.FileId.Identifier), identity.file_id.begin());
        return true;
    }
    void SetPhase(OutputIoOperation phase) {
        m_phase = phase;
    }

    FlushResult FlushToDisk(std::string* out_reason) {
        if (m_file == nullptr)
            return FlushResult::Success;
        auto start = std::chrono::steady_clock::now();
        const int result = std::fflush(m_file);
        m_timings.crt_flush_ms += ElapsedMs(start);
        if (result != 0 || Fails(OutputIoOperation::CrtFlush)) {
            if (out_reason != nullptr)
                *out_reason = std::string("fflush failed: ") + std::strerror(errno);
            return FlushResult::StreamFailure;
        }
        start = std::chrono::steady_clock::now();
        const auto durabilityResult = [&]() {
            const int fd = _fileno(m_file);
            if (fd < 0) {
                if (out_reason != nullptr)
                    *out_reason = "_fileno returned an invalid descriptor";
                return false;
            }
            const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<intptr_t>(_get_osfhandle(fd)));
            if (handle == INVALID_HANDLE_VALUE) {
                if (out_reason != nullptr)
                    *out_reason = "_get_osfhandle returned an invalid handle";
                return false;
            }
            if (Fails(OutputIoOperation::DurabilityFlush) || !FlushFileBuffers(handle)) {
                if (out_reason != nullptr)
                    *out_reason = "FlushFileBuffers failed (GetLastError=" + std::to_string(GetLastError()) + ")";
                return false;
            }
            return true;
        };
        const bool durable = durabilityResult();
        m_timings.durability_flush_ms += ElapsedMs(start);
        if (!durable)
            ++m_timings.durability_failures;
        return durable ? FlushResult::Success : FlushResult::DurabilityFailure;
    }

  private:
    static double ElapsedMs(std::chrono::steady_clock::time_point start) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    bool Fails(OutputIoOperation operation) const {
        return m_fail_io && m_fail_io(operation);
    }
    const std::function<bool(OutputIoOperation)>& m_fail_io;
    const std::function<bool(OutputIoOperation, uint64_t)>& m_before_write;
    const std::function<void(OutputIoOperation, uint64_t)>& m_after_write;
    OutputIoTimings& m_timings;
    OutputIoOperation m_phase = OutputIoOperation::PayloadWrite;
    FILE* m_file = nullptr;
    bool m_capture_header = true;
    std::vector<uint8_t> m_header;
};

class DiskCueSpool final : public libebml::IOCallback {
  public:
    explicit DiskCueSpool(const MatroskaStreamConfig& config) : config_(config) {
        int descriptor = -1;
        const auto path = config.output_path + ".cues";
        if (_sopen_s(&descriptor, path.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY | _O_TEMPORARY, _SH_DENYRW,
                     _S_IREAD | _S_IWRITE) != 0)
            throw std::runtime_error("Cannot create clip cue spool");
        file_ = _fdopen(descriptor, "wb+");
        if (!file_) {
            _close(descriptor);
            throw std::runtime_error("Cannot open clip cue spool stream");
        }
    }

    ~DiskCueSpool() override {
        close();
    }

    uint32 read(void* buffer, size_t size) override {
        return static_cast<uint32>(std::fread(buffer, 1, size, file_));
    }

    void setFilePointer(int64 offset, libebml::seek_mode mode = libebml::seek_beginning) override {
        if (Fseek64(file_, offset, static_cast<int>(mode)) != 0)
            throw std::runtime_error("Cannot seek clip cue spool");
    }

    size_t write(const void* buffer, size_t size) override {
        if (config_.fail_io && config_.fail_io(OutputIoOperation::ClusterWrite))
            throw std::runtime_error("Injected clip cue spool write failure");
        if (config_.before_write && !config_.before_write(OutputIoOperation::ClusterWrite, size))
            throw std::runtime_error("Clip output I/O was cancelled");
        const auto written = std::fwrite(buffer, 1, size, file_);
        if (config_.after_write)
            config_.after_write(OutputIoOperation::ClusterWrite, written);
        if (written != size)
            throw std::runtime_error("Cannot write clip cue spool");
        return written;
    }

    uint64 getFilePointer() override {
        const auto position = Ftell64(file_);
        if (position < 0)
            throw std::runtime_error("Cannot measure clip cue spool");
        return static_cast<uint64>(position);
    }

    void close() override {
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    void CopyTo(libebml::IOCallback& output, uint64_t bytes) {
        setFilePointer(0);
        std::array<uint8_t, 32768> buffer{};
        while (bytes) {
            const auto chunk = static_cast<size_t>((std::min)(bytes, static_cast<uint64_t>(buffer.size())));
            if (read(buffer.data(), chunk) != chunk)
                throw std::runtime_error("Cannot read clip cue spool");
            output.writeFully(buffer.data(), chunk);
            bytes -= chunk;
        }
    }

  private:
    const MatroskaStreamConfig& config_;
    FILE* file_ = nullptr;
};

bool MatroskaStreamWriter::InitializeMediaDescriptor() {
    if (!m_config.collect_media_index) {
        (void)m_io->TakeHeader();
        return true;
    }
    m_commit_sequence = m_config.initial_commit_sequence;
    try {
        auto descriptor =
            std::make_shared<RecordingMediaDescriptor>(m_config.media_descriptor.value_or(RecordingMediaDescriptor{}));
        if (m_config.audio_manifest) {
            descriptor->recording_id = m_config.audio_manifest->recording_id;
            descriptor->segment_id = m_config.audio_manifest->segment_id;
            descriptor->media_id = m_config.audio_manifest->media_id;
            descriptor->segment_origin_r_ns = m_config.audio_manifest->segment_origin_r_ns;
            descriptor->continuity_group = m_config.audio_manifest->continuity_group;
        }
        if (descriptor->recording_id == RecordingId{})
            descriptor->recording_id = NewId<RecordingId>();
        if (descriptor->segment_id == SegmentId{})
            descriptor->segment_id = NewId<SegmentId>();
        if (descriptor->media_id == MediaId{})
            descriptor->media_id = NewId<MediaId>();
        if (descriptor->writer_generation == 0)
            descriptor->writer_generation = 1;
        if (descriptor->descriptor_revision == 0)
            descriptor->descriptor_revision = 1;
        descriptor->path = m_config.output_path;
        descriptor->video.codec_id = m_config.video_codec_id;
        descriptor->video.codec_private = m_config.video_codec_private;
        descriptor->video_width = m_config.encode_width;
        descriptor->video_height = m_config.encode_height;
        descriptor->frame_rate_num = m_config.frame_rate_num;
        descriptor->frame_rate_den = m_config.frame_rate_den;
        descriptor->color = m_config.color;
        if (m_config.reserve_content_light_level) {
            descriptor->color.max_content_light_level = 0;
            descriptor->color.max_frame_average_light_level = 0;
        }
        descriptor->audio_tracks.resize(m_config.audio_track_count);
        for (uint32_t index = 0; index < m_config.audio_track_count; ++index) {
            auto& track = descriptor->audio_tracks[index];
            if (track.track_id == AudioTrackId{})
                track.track_id = NewId<AudioTrackId>();
            track.stream_index.value = index;
            track.track_uid = 2 + index;
            track.name = m_config.audio_tracks[index].name;
            const char* codec = m_config.audio_codec == StreamAudioCodec::Opus   ? "A_OPUS"
                                : m_config.audio_codec == StreamAudioCodec::Aac  ? "A_AAC"
                                : m_config.audio_codec == StreamAudioCodec::Flac ? "A_FLAC"
                                : m_config.audio_float                           ? "A_PCM/FLOAT/IEEE"
                                                                                 : "A_PCM/INT/LIT";
            track.codec = {codec,
                           m_config.audio_tracks[index].codec_private,
                           m_config.audio_sample_rate,
                           m_config.audio_channels,
                           m_config.audio_bit_depth,
                           m_config.audio_tracks[index].codec_delay_samples,
                           DefaultAudioChannelLayout(m_config.audio_channels)};
            if (m_config.audio_codec == StreamAudioCodec::Aac || m_config.audio_codec == StreamAudioCodec::Opus)
                track.codec.bit_depth = 0;
            if (m_config.audio_codec == StreamAudioCodec::Opus && track.codec.codec_private.size() >= 12)
                track.codec.codec_delay_samples = static_cast<uint32_t>(track.codec.codec_private[10]) |
                                                  (static_cast<uint32_t>(track.codec.codec_private[11]) << 8);
        }
        if (m_config.audio_manifest) {
            for (const auto& track : m_config.audio_manifest->tracks)
                descriptor->audio_tracks[track.container.stream_index.value] = track.container;
            descriptor->default_monitor_track_ids = m_config.audio_manifest->default_monitor_track_ids;
            descriptor->audio_manifest = std::make_shared<const AudioManifest1>(*m_config.audio_manifest);
        }
        descriptor->immutable_header = m_io->TakeHeader();
        if (descriptor->immutable_header.empty() || !m_io->GetIdentity(descriptor->file_identity)) {
            m_media_index_degraded = true;
            return false;
        }
        m_media_descriptor = std::move(descriptor);
        return true;
    } catch (...) {
        m_media_index_degraded = true;
        return false;
    }
}

void MatroskaStreamWriter::PublishCommitted(bool durable) noexcept {
    if (m_media_index_degraded || !m_media_descriptor || m_pending_closed_extents.empty())
        return;
    try {
        if (m_commit_sequence == (std::numeric_limits<uint64_t>::max)())
            throw std::overflow_error("Media commit sequence exhausted");
        m_readable_end = m_pending_closed_extents.back().file_end;
        if (durable)
            m_durable_end = m_readable_end;
        CommittedMediaReceipt receipt{m_media_descriptor->recording_id,
                                      m_media_descriptor->segment_id,
                                      m_media_descriptor->writer_generation,
                                      ++m_commit_sequence,
                                      m_media_descriptor->descriptor_revision,
                                      m_readable_end,
                                      m_durable_end,
                                      std::move(m_pending_closed_extents)};
        m_pending_closed_extents.clear();
        m_pending_receipt_bytes = 0;
        if (m_config.publish_committed && m_config.publish_committed(std::move(receipt)) != ReceiptAcceptance::Accepted)
            m_media_index_degraded = true;
    } catch (...) {
        m_media_index_degraded = true;
        m_pending_closed_extents.clear();
        m_pending_receipt_bytes = 0;
    }
}

MatroskaStreamWriter::MatroskaStreamWriter() = default;

std::optional<RecordingSegmentSeal> MatroskaStreamWriter::finalized_segment_seal() const noexcept {
    if (!m_finalize_succeeded || m_io != nullptr || m_media_index_degraded || !m_config.publish_committed ||
        !m_media_descriptor || !m_has_video_covered_end || m_readable_end == 0 ||
        m_commit_sequence <= m_config.initial_commit_sequence ||
        m_max_video_covered_end_ns > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
        return std::nullopt;
    const auto local_end = static_cast<int64_t>(m_max_video_covered_end_ns);
    const auto origin = m_media_descriptor->segment_origin_r_ns;
    if (origin > 0 && local_end > (std::numeric_limits<int64_t>::max)() - origin)
        return std::nullopt;
    return RecordingSegmentSeal{m_media_descriptor->recording_id,
                                m_media_descriptor->segment_id,
                                m_media_descriptor->writer_generation,
                                m_media_descriptor->descriptor_revision,
                                m_commit_sequence,
                                m_readable_end,
                                origin + local_end};
}

MatroskaStreamWriter::~MatroskaStreamWriter() {
    // Defensive: if Finalize() was never called, release the cluster and close
    // the handle so we never leak the file descriptor. We do NOT attempt a real
    // finalize here — an un-finalized file is intentionally left as-is for the
    // failure path; the owner decides whether to quarantine it.
    delete m_cluster;
    m_cluster = nullptr;
    CloseIo();
}

void MatroskaStreamWriter::Fail(const std::string& reason) {
    if (!m_failed) {
        m_failed = true;
        m_error = reason;
    }
}

void MatroskaStreamWriter::CloseIo() {
    m_cue_spool.reset();
    if (m_io != nullptr) {
        try {
            m_io->close();
        } catch (const std::exception& ex) {
            Fail(ex.what());
        } catch (...) {
            Fail("Unknown output close failure");
        }
        delete m_io;
        m_io = nullptr;
    }
}

bool MatroskaStreamWriter::Open(const MatroskaStreamConfig& config) {
    if (m_opened) {
        Fail("MatroskaStreamWriter::Open called twice");
        return false;
    }
    m_config = config;
    m_emit_exact_audio_timestamps = config.audio_manifest.has_value() && !config.webm;
    m_opened = true;

    if (config.audio_track_count > kMaxAudioTracks || config.reorder_window_max_packets == 0 ||
        config.reorder_window_max_bytes == 0) {
        Fail("Invalid audio track count or reorder budget");
        return false;
    }
    if (config.audio_manifest) {
        const auto valid = ValidateAudioManifest1(*config.audio_manifest);
        if (!valid.succeeded() || config.audio_manifest->tracks.size() != config.audio_track_count) {
            Fail("Invalid audio manifest or track count");
            return false;
        }
        const auto& manifest = *config.audio_manifest;
        if (config.media_descriptor && ((config.media_descriptor->recording_id != RecordingId{} &&
                                         config.media_descriptor->recording_id != manifest.recording_id) ||
                                        (config.media_descriptor->segment_id != SegmentId{} &&
                                         config.media_descriptor->segment_id != manifest.segment_id) ||
                                        (config.media_descriptor->media_id != MediaId{} &&
                                         config.media_descriptor->media_id != manifest.media_id))) {
            Fail("Audio manifest contradicts the media descriptor identity");
            return false;
        }
        for (const auto& track : manifest.tracks) {
            const auto index = track.container.stream_index.value;
            const char* codec = config.audio_codec == StreamAudioCodec::Opus   ? "A_OPUS"
                                : config.audio_codec == StreamAudioCodec::Aac  ? "A_AAC"
                                : config.audio_codec == StreamAudioCodec::Flac ? "A_FLAC"
                                : config.audio_float                           ? "A_PCM/FLOAT/IEEE"
                                                                               : "A_PCM/INT/LIT";
            uint32_t delay = config.audio_tracks[index].codec_delay_samples;
            if (config.audio_codec == StreamAudioCodec::Opus && config.audio_tracks[index].codec_private.size() >= 12)
                delay = config.audio_tracks[index].codec_private[10] |
                        (static_cast<uint32_t>(config.audio_tracks[index].codec_private[11]) << 8);
            if (track.container.track_uid != index + 2 || track.container.name != config.audio_tracks[index].name ||
                track.container.codec.codec_id != codec ||
                track.container.codec.codec_private != config.audio_tracks[index].codec_private ||
                track.container.codec.sample_rate != config.audio_sample_rate ||
                track.container.codec.channels != config.audio_channels ||
                track.container.codec.bit_depth !=
                    ((config.audio_codec == StreamAudioCodec::Aac || config.audio_codec == StreamAudioCodec::Opus)
                         ? 0
                         : config.audio_bit_depth) ||
                track.container.codec.channel_layout != DefaultAudioChannelLayout(config.audio_channels) ||
                track.container.codec.codec_delay_samples != delay) {
                Fail("Audio manifest contradicts the configured container tracks");
                return false;
            }
        }
    }

    // --- Open output file ---
    try {
        m_io = new DurableFileIo(m_config.output_path, m_config.path_pre_reserved, m_config.fail_io,
                                 m_config.before_write, m_config.after_write, m_io_timings);
    } catch (const std::exception& ex) {
        m_io = nullptr;
        Fail(std::string("DurableFileIo::open failed: ") + ex.what());
        return false;
    }

    try {
        // --- EBML header ---
        {
            libebml::EbmlHead ebml_head;
            if (m_config.webm) {
                // WebM: DocType "webm", AV1 or VP8/VP9 video with
                // Opus or Vorbis audio only. Anything else would be a Matroska
                // file wearing the wrong name.
                if (m_config.video_codec_id != "V_AV1" || m_config.audio_codec == StreamAudioCodec::Aac ||
                    m_config.audio_codec == StreamAudioCodec::Pcm || m_config.audio_codec == StreamAudioCodec::Flac) {
                    Fail("WebM permits only AV1 video with Opus audio in this recorder");
                    CloseIo();
                    return false;
                }
                libebml::GetChild<libebml::EDocType>(ebml_head).SetValue("webm");
                libebml::GetChild<libebml::EDocTypeVersion>(ebml_head).SetValue(4);
                libebml::GetChild<libebml::EDocTypeReadVersion>(ebml_head).SetValue(2);
            } else {
                libebml::GetChild<libebml::EDocType>(ebml_head).SetValue("matroska");
                libebml::GetChild<libebml::EDocTypeVersion>(ebml_head).SetValue(4);
                libebml::GetChild<libebml::EDocTypeReadVersion>(ebml_head).SetValue(2);
            }
            ebml_head.Render(*m_io, true);
        }

        // --- Segment head (unknown size, back-patched at finalize) ---
        m_segment = std::make_unique<libmatroska::KaxSegment>();
        m_segment->SetSizeInfinite(true);
        m_segment->WriteHead(*m_io, 8);
        // This libebml version resets the numeric size in WriteHead even for
        // an infinite master. Write the unknown-size VINT explicitly so an
        // interrupted recording exposes its clusters before the final backpatch.
        const uint8_t unknown_size[8] = {0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        m_io->setFilePointer(-8, libebml::seek_current);
        m_io->writeFully(unknown_size, sizeof(unknown_size));
        m_segment_data_start = static_cast<uint64_t>(m_io->getFilePointer());

        // --- SeekHead placeholder void (replaced at finalize) ---
        m_seekhead_void = std::make_unique<libebml::EbmlVoid>();
        m_seekhead_void->SetSize(kSeekHeadReservedBytes);
        m_seekhead_void->Render(*m_io);

        // --- Segment Info. Duration is written as 0.0 now and back-patched at
        //     finalize. We reserve a fixed 8-byte float so the patched value is
        //     guaranteed to fit in place. ---
        m_info = std::make_unique<libmatroska::KaxInfo>();
        libebml::GetChild<libmatroska::KaxTimecodeScale>(*m_info).SetValue(kTimecodeScaleNs);
        libebml::GetChild<libmatroska::KaxMuxingApp>(*m_info).SetValueUTF8("exosnap");
        libebml::GetChild<libmatroska::KaxWritingApp>(*m_info).SetValueUTF8("exosnap");
        {
            auto& dur = libebml::GetChild<libmatroska::KaxDuration>(*m_info);
            // Force 8-byte (double) precision so the value can be back-patched at
            // finalize without changing the element's serialized size (a 4-byte
            // float would risk a size mismatch in OverwriteData).
            dur.SetPrecision(libebml::EbmlFloat::FLOAT_64);
            dur.SetValue(0.0);
        }
        m_info->Render(*m_io, /*bSaveDefault=*/true);

        // --- Tracks ---
        m_tracks = std::make_unique<libmatroska::KaxTracks>();
        m_track_entries.clear();

        // Video track (track_num=1)
        {
            auto& vid = libebml::AddNewChild<libmatroska::KaxTrackEntry>(*m_tracks);
            libebml::GetChild<libmatroska::KaxTrackNumber>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackUID>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackType>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackFlagEnabled>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackFlagDefault>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackFlagLacing>(vid).SetValue(0);
            libebml::GetChild<libmatroska::KaxCodecID>(vid).SetValue(m_config.video_codec_id);
            libebml::GetChild<libmatroska::KaxMaxBlockAdditionID>(vid).SetValue(kPacketTimestampBlockId);
            auto& timestamps = libebml::GetChild<libmatroska::KaxBlockAdditionMapping>(vid);
            libebml::GetChild<libmatroska::KaxBlockAddIDValue>(timestamps).SetValue(kPacketTimestampBlockId);
            libebml::GetChild<libmatroska::KaxBlockAddIDType>(timestamps).SetValue(kPacketTimestampMappingType);
            libebml::GetChild<libmatroska::KaxBlockAddIDName>(timestamps).SetValue("ExoSnap packet timestamps v1");
            if (!m_config.video_codec_private.empty()) {
                libebml::GetChild<libmatroska::KaxCodecPrivate>(vid).CopyBuffer(
                    m_config.video_codec_private.data(), static_cast<uint32_t>(m_config.video_codec_private.size()));
            }
            auto& vs = libebml::GetChild<libmatroska::KaxTrackVideo>(vid);
            libebml::GetChild<libmatroska::KaxVideoPixelWidth>(vs).SetValue(m_config.encode_width);
            libebml::GetChild<libmatroska::KaxVideoPixelHeight>(vs).SetValue(m_config.encode_height);
            if (m_config.frame_rate_num > 0 && m_config.frame_rate_den > 0) {
                const uint64_t frame_dur_ns = static_cast<uint64_t>(m_config.frame_rate_den) * 1000000000ULL /
                                              static_cast<uint64_t>(m_config.frame_rate_num);
                libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(vid).SetValue(frame_dur_ns);
            }

            // Colour description. Written for every recording so the
            // file is no longer color-ambiguous; the values match the BT.709
            // limited-range conversion the VideoProcessor performs in
            // video_thread.cpp. HDR sub-elements are emitted only when set.
            {
                auto& colour = libebml::GetChild<libmatroska::KaxVideoColour>(vs);
                libebml::GetChild<libmatroska::KaxVideoColourPrimaries>(colour).SetValue(
                    static_cast<uint64_t>(m_config.color.primaries));
                libebml::GetChild<libmatroska::KaxVideoColourTransferCharacter>(colour).SetValue(
                    static_cast<uint64_t>(m_config.color.transfer));
                libebml::GetChild<libmatroska::KaxVideoColourMatrix>(colour).SetValue(
                    static_cast<uint64_t>(m_config.color.matrix));
                libebml::GetChild<libmatroska::KaxVideoColourRange>(colour).SetValue(
                    static_cast<uint64_t>(m_config.color.range));
                libebml::GetChild<libmatroska::KaxVideoBitsPerChannel>(colour).SetValue(
                    static_cast<uint64_t>(m_config.color.bits_per_channel));
                if (m_config.chroma_420) {
                    // 1 = left collocated, 2 = half (centred): the siting every
                    // 4:2:0 conversion path here produces.
                    libebml::GetChild<libmatroska::KaxVideoChromaSitHorz>(colour).SetValue(1);
                    libebml::GetChild<libmatroska::KaxVideoChromaSitVert>(colour).SetValue(2);
                }
                if (m_config.color.hdr) {
                    if (m_config.reserve_content_light_level) {
                        // Reserved, not written: MaxCLL and MaxFALL are maxima over
                        // the whole stream and the last frame has not been encoded
                        // yet. Both leaves are rendered now at a fixed two bytes --
                        // the exact width CTA-861.3's u(16) coding needs, so the
                        // patched value can never outgrow the reservation -- and
                        // Finalize() overwrites those bytes in place. A recording
                        // that never reaches Finalize() keeps the zeros, which is
                        // what CTA-861.3 reads as an unknown level.
                        auto& cll = libebml::GetChild<libmatroska::KaxVideoColourMaxCLL>(colour);
                        cll.SetDefaultSize(kContentLightLevelReservedBytes);
                        cll.SetValue(0);
                        m_max_cll_element = &cll;
                        auto& fall = libebml::GetChild<libmatroska::KaxVideoColourMaxFALL>(colour);
                        fall.SetDefaultSize(kContentLightLevelReservedBytes);
                        fall.SetValue(0);
                        m_max_fall_element = &fall;
                    } else {
                        if (m_config.color.max_content_light_level > 0) {
                            libebml::GetChild<libmatroska::KaxVideoColourMaxCLL>(colour).SetValue(
                                static_cast<uint64_t>(m_config.color.max_content_light_level));
                        }
                        if (m_config.color.max_frame_average_light_level > 0) {
                            libebml::GetChild<libmatroska::KaxVideoColourMaxFALL>(colour).SetValue(
                                static_cast<uint64_t>(m_config.color.max_frame_average_light_level));
                        }
                    }
                }
                // Mastering display metadata (SMPTE ST 2086). Independent of the
                // `hdr`/MaxCLL/MaxFALL gate above — written whenever
                // has_mastering_display is set, omitted entirely otherwise (no
                // empty/zeroed KaxVideoColourMasterMeta element).
                if (m_config.color.has_mastering_display) {
                    auto& mdcv = libebml::GetChild<libmatroska::KaxVideoColourMasterMeta>(colour);
                    libebml::GetChild<libmatroska::KaxVideoRChromaX>(mdcv).SetValue(
                        m_config.color.mastering_display_primary_r_x);
                    libebml::GetChild<libmatroska::KaxVideoRChromaY>(mdcv).SetValue(
                        m_config.color.mastering_display_primary_r_y);
                    libebml::GetChild<libmatroska::KaxVideoGChromaX>(mdcv).SetValue(
                        m_config.color.mastering_display_primary_g_x);
                    libebml::GetChild<libmatroska::KaxVideoGChromaY>(mdcv).SetValue(
                        m_config.color.mastering_display_primary_g_y);
                    libebml::GetChild<libmatroska::KaxVideoBChromaX>(mdcv).SetValue(
                        m_config.color.mastering_display_primary_b_x);
                    libebml::GetChild<libmatroska::KaxVideoBChromaY>(mdcv).SetValue(
                        m_config.color.mastering_display_primary_b_y);
                    libebml::GetChild<libmatroska::KaxVideoWhitePointChromaX>(mdcv).SetValue(
                        m_config.color.mastering_display_white_point_x);
                    libebml::GetChild<libmatroska::KaxVideoWhitePointChromaY>(mdcv).SetValue(
                        m_config.color.mastering_display_white_point_y);
                    libebml::GetChild<libmatroska::KaxVideoLuminanceMax>(mdcv).SetValue(
                        m_config.color.mastering_display_max_luminance);
                    libebml::GetChild<libmatroska::KaxVideoLuminanceMin>(mdcv).SetValue(
                        m_config.color.mastering_display_min_luminance);
                }
            }

            vid.SetGlobalTimecodeScale(kTimecodeScaleNs);
            m_track_entries.push_back(&vid);
        }

        // Audio tracks (track_num=2+i)
        for (uint32_t i = 0; i < m_config.audio_track_count; ++i) {
            const uint32_t track_number = 2 + i;
            auto& aud = libebml::AddNewChild<libmatroska::KaxTrackEntry>(*m_tracks);
            libebml::GetChild<libmatroska::KaxTrackNumber>(aud).SetValue(track_number);
            libebml::GetChild<libmatroska::KaxTrackUID>(aud).SetValue(track_number);
            libebml::GetChild<libmatroska::KaxTrackType>(aud).SetValue(2);
            libebml::GetChild<libmatroska::KaxTrackFlagLacing>(aud).SetValue(0);
            // Only the first audio track is the default one; the element's own
            // default is 1, so the others must say 0 explicitly.
            libebml::GetChild<libmatroska::KaxTrackFlagDefault>(aud).SetValue(i == 0 ? 1 : 0);

            // Track name (see StreamAudioTrack::name): omitted entirely when the
            // caller did not resolve one, rather than writing an empty element --
            // an older reader falling back to positional labels needs "absent",
            // not "named the empty string".
            if (const std::string& name = m_config.audio_tracks[i].name; !name.empty()) {
                libebml::GetChild<libmatroska::KaxTrackName>(aud).SetValueUTF8(name);
            }

            const auto& slot = m_config.audio_tracks[i].codec_private;
            if (m_config.audio_codec == StreamAudioCodec::Opus) {
                if (slot.size() < 19) {
                    Fail("Opus CodecPrivate is incomplete; expected full 19-byte OpusHead");
                    CloseIo();
                    return false;
                }
                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue("A_OPUS");
                libebml::GetChild<libmatroska::KaxCodecPrivate>(aud).CopyBuffer(slot.data(),
                                                                                static_cast<uint32_t>(slot.size()));
                const uint16_t pre_skip = static_cast<uint16_t>(slot[10]) | (static_cast<uint16_t>(slot[11]) << 8u);
                const uint64_t codec_delay_ns = static_cast<uint64_t>(pre_skip) * 1000000000ULL / 48000u;
                libebml::GetChild<libmatroska::KaxCodecDelay>(aud).SetValue(codec_delay_ns);
                libebml::GetChild<libmatroska::KaxSeekPreRoll>(aud).SetValue(80000000ULL);
                if (m_config.opus_frame_samples > 0)
                    libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(aud).SetValue(
                        static_cast<uint64_t>(m_config.opus_frame_samples) * 1000000000ULL / 48000u);
            } else if (m_config.audio_codec == StreamAudioCodec::Pcm) {
                // Uncompressed PCM. No CodecPrivate; the bit depth is carried
                // in the track audio header below. audio_float selects the
                // 32-bit IEEE754 float CodecID instead of signed int PCM.
                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue(m_config.audio_float ? "A_PCM/FLOAT/IEEE"
                                                                                              : "A_PCM/INT/LIT");
            } else if (m_config.audio_codec == StreamAudioCodec::Flac) {
                // Lossless FLAC. A_FLAC mandates the CodecPrivate be the native
                // FLAC stream header (the "fLaC" marker + STREAMINFO and any
                // leading metadata blocks) — players require it to decode.
                if (slot.size() < 4 || slot[0] != 0x66 || slot[1] != 0x4C || slot[2] != 0x61 || slot[3] != 0x43) {
                    Fail("FLAC CodecPrivate is missing or malformed; expected native 'fLaC' header");
                    CloseIo();
                    return false;
                }
                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue("A_FLAC");
                libebml::GetChild<libmatroska::KaxCodecPrivate>(aud).CopyBuffer(slot.data(),
                                                                                static_cast<uint32_t>(slot.size()));
            } else {
                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue("A_AAC");
                if (!slot.empty()) {
                    libebml::GetChild<libmatroska::KaxCodecPrivate>(aud).CopyBuffer(slot.data(),
                                                                                    static_cast<uint32_t>(slot.size()));
                }
                // 1024-sample AAC frames at the configured rate, not a 48 kHz constant.
                libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(aud).SetValue(
                    1024ULL * 1000000000ULL / std::max<uint32_t>(1u, m_config.audio_sample_rate));
                if (m_config.audio_tracks[i].codec_delay_samples > 0) {
                    libebml::GetChild<libmatroska::KaxCodecDelay>(aud).SetValue(
                        static_cast<uint64_t>(m_config.audio_tracks[i].codec_delay_samples) * 1000000000ULL /
                        std::max<uint32_t>(1u, m_config.audio_sample_rate));
                }
            }

            auto& as = libebml::GetChild<libmatroska::KaxTrackAudio>(aud);
            libebml::GetChild<libmatroska::KaxAudioSamplingFreq>(as).SetValue(
                static_cast<double>(m_config.audio_sample_rate));
            libebml::GetChild<libmatroska::KaxAudioChannels>(as).SetValue(m_config.audio_channels);
            if (m_config.audio_codec == StreamAudioCodec::Pcm || m_config.audio_codec == StreamAudioCodec::Flac) {
                libebml::GetChild<libmatroska::KaxAudioBitDepth>(as).SetValue(m_config.audio_bit_depth);
            }
            if (m_config.audio_manifest && !m_config.webm) {
                libebml::GetChild<libmatroska::KaxMaxBlockAdditionID>(aud).SetValue(kPacketTimestampBlockId);
                auto& timestamps = libebml::GetChild<libmatroska::KaxBlockAdditionMapping>(aud);
                libebml::GetChild<libmatroska::KaxBlockAddIDValue>(timestamps).SetValue(kPacketTimestampBlockId);
                libebml::GetChild<libmatroska::KaxBlockAddIDType>(timestamps).SetValue(kPacketTimestampMappingType);
                libebml::GetChild<libmatroska::KaxBlockAddIDName>(timestamps).SetValue("ExoSnap packet timestamps v1");
            }
            aud.SetGlobalTimecodeScale(kTimecodeScaleNs);
            m_track_entries.push_back(&aud);
        }

        // bSaveDefault=false avoids rendering deprecated KaxTrackTimecodeScale.
        m_tracks->Render(*m_io);
        if (m_config.audio_manifest && !m_config.webm) {
            const auto serialized = SerializeAudioManifest1(*m_config.audio_manifest);
            if (!serialized.succeeded())
                throw std::runtime_error("Cannot serialize audio manifest");
            libmatroska::KaxTags tags;
            auto& global = libebml::AddNewChild<libmatroska::KaxTag>(tags);
            libebml::GetChild<libmatroska::KaxTagTargets>(global);
            const auto add_tag = [](libmatroska::KaxTag& tag, const char* name, const std::string& value) {
                auto& simple = libebml::AddNewChild<libmatroska::KaxTagSimple>(tag);
                libebml::GetChild<libmatroska::KaxTagName>(simple).SetValueUTF8(name);
                libebml::GetChild<libmatroska::KaxTagString>(simple).SetValueUTF8(value);
            };
            add_tag(global, "EXOSNAP_AUDIO_MANIFEST", *serialized.value());
            for (const auto& track : m_config.audio_manifest->tracks) {
                auto& tag = libebml::AddNewChild<libmatroska::KaxTag>(tags);
                auto& target = libebml::GetChild<libmatroska::KaxTagTargets>(tag);
                libebml::GetChild<libmatroska::KaxTagTrackUID>(target).SetValue(track.container.track_uid);
                add_tag(tag, "EXOSNAP_TRACK_ID", ToUuid(track.container.track_id));
                add_tag(tag, "EXOSNAP_ROLE", track.container.role);
                const auto provenance =
                    SerializeAudioTrackProvenance(*m_config.audio_manifest, track.container.track_id);
                if (!provenance.succeeded())
                    throw std::runtime_error("Cannot serialize audio track provenance");
                add_tag(tag, "EXOSNAP_PROVENANCE", *provenance.value());
            }
            tags.Render(*m_io);
        }

        // --- Cues accumulator (entries added incrementally per keyframe). ---
        if (m_config.stream_cues_to_disk) {
            m_cues = std::make_unique<StreamedCues>();
            m_cue_spool = std::make_unique<DiskCueSpool>(m_config);
        } else {
            m_cues = std::make_unique<libmatroska::KaxCues>();
        }
        m_cues->SetGlobalTimecodeScale(kTimecodeScaleNs);
    } catch (const std::exception& ex) {
        Fail(std::string("MatroskaStreamWriter::Open render failed: ") + ex.what());
        CloseIo();
        return false;
    }

    InitializeMediaDescriptor();
    if (!m_config.collect_media_index) {
        // Track initialization and tags already own the bytes they need. Without
        // an index, retaining a second configuration copy only increases the
        // memory held alongside packet payloads.
        std::vector<uint8_t>().swap(m_config.video_codec_private);
        for (auto& track : m_config.audio_tracks)
            std::vector<uint8_t>().swap(track.codec_private);
        m_config.audio_manifest.reset();
    }
    return true;
}

size_t MatroskaStreamWriter::retained_cue_count() const noexcept {
    return m_cues ? m_cues->ListSize() : 0;
}

bool MatroskaStreamWriter::Push(MuxPacket packet) {
    static_assert((2 * sizeof(WindowEntry) + 4 * sizeof(void*) + 64) * 4097 < 3 * 1024 * 1024 / 2);
    if (m_failed || !m_opened || m_finalized)
        return false;
    if (packet.bytes.empty())
        return true; // skip empties silently (matches old muxer)

    if (packet.bytes.capacity() > m_config.reorder_window_max_bytes - m_cur_window_bytes) {
        Fail("Reorder payload budget exceeded before a safe interleave boundary");
        return false;
    }

    WindowEntry e;
    e.pts_ns = packet.pts_ns;
    e.ordering_ns = packet.dts_ns.value_or(static_cast<int64_t>(packet.pts_ns));
    e.dts_ns = packet.dts_ns;
    e.track_num = packet.track_num;
    e.is_key = packet.is_key;
    e.bytes = std::move(packet.bytes);

    m_max_pushed_ordering_ns = (std::max)(e.ordering_ns, m_max_pushed_ordering_ns);

    // Interleave by decode time while retaining presentation timestamps. The window is small
    // (a few seconds) so a back-to-front insertion scan is cheap; equal PTS
    // preserves push order (stable) which matches the old stable_sort.
    auto it = m_window.end();
    while (it != m_window.begin()) {
        auto prev = std::prev(it);
        if (prev->ordering_ns <= e.ordering_ns)
            break;
        it = prev;
    }
    m_cur_window_bytes += e.bytes.capacity();
    m_window.insert(it, std::move(e));

    if (m_window.size() > m_peak_window_packets)
        m_peak_window_packets = m_window.size();
    if (m_cur_window_bytes > m_peak_window_bytes)
        m_peak_window_bytes = m_cur_window_bytes;

    return DrainWindow(/*force=*/false);
}

bool MatroskaStreamWriter::DrainWindow(bool force) {
    // Emit from the front (oldest decode time) while either forcing, or the front packet
    // is far enough behind the newest pushed PTS that no earlier packet can still
    // arrive within the reorder horizon. Also enforce the hard count ceiling so a
    // stalled track can never blow RAM.
    while (!m_window.empty()) {
        WindowEntry& front = m_window.front();
        const bool behind_horizon =
            m_max_pushed_ordering_ns >= front.ordering_ns &&
            static_cast<uint64_t>(m_max_pushed_ordering_ns - front.ordering_ns) >= m_config.reorder_window_ns;
        const bool over_capacity = m_window.size() > m_config.reorder_window_max_packets;
        if (!force && !behind_horizon && over_capacity) {
            // Capacity alone cannot prove that a stalled producer has no earlier packet.
            Fail("Reorder packet budget exceeded before a safe interleave boundary");
            return false;
        }
        if (!force && !behind_horizon)
            break;

        const size_t retained_bytes = front.bytes.capacity();
        if (!EmitPacket(front))
            return false;
        m_cur_window_bytes -= retained_bytes;
        m_window.pop_front();
    }
    return true;
}

bool MatroskaStreamWriter::EmitPacket(WindowEntry& e) {
    if (e.track_num == 0)
        return true;

    // Resolve KaxTrackEntry*: track_num 1 -> [0] video; 2+n -> [1+n] audio.
    libmatroska::KaxTrackEntry* track_entry = nullptr;
    if (e.track_num == 1) {
        track_entry = m_track_entries[0];
    } else {
        const uint32_t entry_idx = static_cast<uint32_t>(e.track_num - 2) + 1;
        if (entry_idx >= static_cast<uint32_t>(m_track_entries.size()))
            return true; // out-of-range track: skip (matches old muxer)
        track_entry = m_track_entries[entry_idx];
    }

    const uint64_t pkt_tick = e.pts_ns / kTimecodeScaleNs;

    if (m_config.max_cluster_payload_bytes && e.bytes.capacity() > m_config.max_cluster_payload_bytes) {
        Fail("Encoded packet exceeds clip cluster payload budget");
        return false;
    }

    bool need_new_cluster = m_first_cluster;
    if (!m_first_cluster && m_cluster != nullptr) {
        if (m_config.stream_cues_to_disk && m_cluster_bytes.size() >= kClipClusterPacketLimit)
            need_new_cluster = true;
        if (m_config.max_cluster_payload_bytes && m_cluster_payload_bytes &&
            e.bytes.capacity() > m_config.max_cluster_payload_bytes - m_cluster_payload_bytes)
            need_new_cluster = true;
        const int64_t rel_ticks = static_cast<int64_t>(pkt_tick) - static_cast<int64_t>(m_cluster_start_ticks);
        if (rel_ticks > kMaxClusterRelativeTicks || rel_ticks < -kMaxClusterRelativeTicks) {
            need_new_cluster = true;
        } else if (e.is_key && e.track_num == 1 && pkt_tick >= m_cluster_start_ticks &&
                   (pkt_tick - m_cluster_start_ticks) >= kClusterBoundaryTicks) {
            need_new_cluster = true;
        }
    }

    try {
        if (need_new_cluster) {
            if (!FlushCluster())
                return false;
            m_first_cluster = false;
            m_cluster_start_ticks = pkt_tick;
            m_cluster = new libmatroska::KaxCluster();
            m_cluster->SetParent(*m_segment);
            m_cluster->SetGlobalTimecodeScale(kTimecodeScaleNs);
            m_cluster->InitTimecode(m_cluster_start_ticks, static_cast<int64_t>(kTimecodeScaleNs));
        }

        // Backing bytes must outlive FlushCluster()/Render(). Keep them in a
        // per-cluster store that is cleared only after the cluster is rendered.
        if (m_config.max_cluster_payload_bytes)
            m_cluster_bytes.push_back(std::move(e.bytes));
        else
            // Time-based recorder clusters must not accumulate unused packet capacity.
            m_cluster_bytes.push_back(e.bytes);
        m_cluster_payload_bytes += m_cluster_bytes.back().capacity();
        m_peak_cluster_payload_bytes = (std::max)(m_peak_cluster_payload_bytes, m_cluster_payload_bytes);
        std::vector<uint8_t>& stored = m_cluster_bytes.back();

        auto* data_buf = new libmatroska::DataBuffer(stored.data(), static_cast<uint32_t>(stored.size()));
        libebml::EbmlElement* rendered_block = nullptr;
        const bool exact_audio = e.track_num > 1 && m_emit_exact_audio_timestamps;
        if ((e.track_num == 1 || exact_audio) &&
            e.pts_ns > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
            throw std::overflow_error("Packet timestamp exceeds the signed timestamp domain");
        if ((e.track_num == 1 && (e.dts_ns || !m_config.webm)) || exact_audio) {
            auto& group = libebml::AddNewChild<libmatroska::KaxBlockGroup>(*m_cluster);
            rendered_block = &group;
            group.SetParent(*m_cluster);
            group.AddFrame(*track_entry, e.pts_ns, *data_buf, libmatroska::LACING_NONE);
            if (!e.is_key) {
                // Zero denotes a dependency whose exact reference picture is
                // unknown. Omitting ReferenceBlock would mark this as a keyframe.
                libebml::GetChild<libmatroska::KaxReferenceBlock>(group).SetReferencedTimecode(0);
            }
            if (e.track_num == 1 && m_config.frame_rate_num > 0 && m_config.frame_rate_den > 0)
                group.SetBlockDuration(static_cast<uint64_t>(m_config.frame_rate_den) * 1000000000ULL /
                                       m_config.frame_rate_num);
            auto& additions = libebml::GetChild<libmatroska::KaxBlockAdditions>(group);
            auto& more = libebml::GetChild<libmatroska::KaxBlockMore>(additions);
            libebml::GetChild<libmatroska::KaxBlockAddID>(more).SetValue(kPacketTimestampBlockId);
            const auto timestamps =
                EncodeMatroskaPacketTimestamps(e.pts_ns, e.dts_ns.value_or(static_cast<int64_t>(e.pts_ns)));
            libebml::GetChild<libmatroska::KaxBlockAdditional>(more).CopyBuffer(
                timestamps.data(), static_cast<uint32_t>(timestamps.size()));
        } else {
            auto& sb = libebml::AddNewChild<libmatroska::KaxSimpleBlock>(*m_cluster);
            rendered_block = &sb;
            sb.SetParent(*m_cluster);
            sb.AddFrame(*track_entry, e.pts_ns, *data_buf, libmatroska::LACING_NONE);
            sb.SetKeyframe(e.is_key);
        }

        if (m_config.collect_media_index && !m_media_index_degraded) {
            try {
                uint64_t duration = 0;
                if (e.track_num == 1 && m_config.frame_rate_num != 0)
                    duration = static_cast<uint64_t>(m_config.frame_rate_den) * 1000000000ULL / m_config.frame_rate_num;
                else if (m_config.audio_sample_rate != 0) {
                    uint64_t samples = 0;
                    if (m_config.audio_codec == StreamAudioCodec::Pcm && m_config.audio_channels &&
                        m_config.audio_bit_depth >= 8)
                        samples = stored.size() /
                                  (static_cast<size_t>(m_config.audio_channels) * (m_config.audio_bit_depth / 8));
                    else if (m_config.audio_codec == StreamAudioCodec::Opus)
                        samples = m_config.opus_frame_samples;
                    else if (m_config.audio_codec == StreamAudioCodec::Aac)
                        samples = 1024;
                    duration = samples * 1000000000ULL / m_config.audio_sample_rate;
                }
                if (e.pts_ns > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
                    throw std::overflow_error("Media coverage timestamp overflow");
                auto pts = static_cast<int64_t>(e.pts_ns);
                bool audible = true;
                if (e.track_num > 1 && m_config.audio_codec == StreamAudioCodec::Opus &&
                    m_config.opus_frame_samples != 0) {
                    const auto index = static_cast<size_t>(e.track_num - 2);
                    const auto& codec_private = m_config.audio_tracks[index].codec_private;
                    const uint64_t delay_samples =
                        static_cast<uint64_t>(codec_private[10]) | (static_cast<uint64_t>(codec_private[11]) << 8);
                    const uint64_t samples = m_config.opus_frame_samples;
                    const uint64_t emitted = m_opus_emitted_samples[index];
                    if (emitted > (std::numeric_limits<uint64_t>::max)() - samples)
                        throw std::overflow_error("Opus sample count overflow");
                    const uint64_t remaining_delay = emitted < delay_samples ? delay_samples - emitted : 0;
                    const uint64_t skipped = (std::min)(samples, remaining_delay);
                    m_opus_emitted_samples[index] = emitted + samples;
                    audible = skipped < samples;
                    pts -= static_cast<int64_t>(delay_samples * 1000000000ULL / 48000);
                    duration = (samples - skipped) * 1000000000ULL / 48000;
                }
                if (pts > (std::numeric_limits<int64_t>::max)() - static_cast<int64_t>(duration))
                    throw std::overflow_error("Media coverage timestamp overflow");
                const auto end = pts + static_cast<int64_t>(duration);
                if (audible) {
                    if (end >= 0) {
                        m_max_covered_end_ns = (std::max)(m_max_covered_end_ns, static_cast<uint64_t>(end));
                        m_has_covered_end = true;
                        if (e.track_num == 1) {
                            m_max_video_covered_end_ns =
                                (std::max)(m_max_video_covered_end_ns, static_cast<uint64_t>(end));
                            m_has_video_covered_end = true;
                        }
                    }
                    auto& coverage = m_active_cluster_extent.coverage;
                    auto found = std::find_if(coverage.begin(), coverage.end(),
                                              [&](const auto& entry) { return entry.track_number == e.track_num; });
                    if (found == coverage.end())
                        coverage.push_back({e.track_num, pts, end, e.ordering_ns, e.ordering_ns});
                    else {
                        found->begin_pts_ns = (std::min)(found->begin_pts_ns, pts);
                        found->end_pts_ns = (std::max)(found->end_pts_ns, end);
                        found->min_dts_ns = (std::min)(found->min_dts_ns, e.ordering_ns);
                        found->max_dts_ns = (std::max)(found->max_dts_ns, e.ordering_ns);
                    }
                }
                if (e.is_key && e.track_num == 1) {
                    if (m_active_cluster_extent.random_access_points.size() >=
                        kReceiptPayloadLimit / sizeof(MediaRandomAccessPoint))
                        throw std::length_error("Media random-access metadata budget exceeded");
                    const size_t index = m_active_cluster_extent.random_access_points.size();
                    m_active_cluster_extent.random_access_points.push_back(
                        {1, 0, 0, pts, e.ordering_ns,
                         QualifyClosedRandomAccess(m_config.video_codec_id, m_config.video_codec_private, stored,
                                                   e.is_key)});
                    m_pending_rap_blocks.emplace_back(rendered_block, index);
                }
            } catch (...) {
                m_media_index_degraded = true;
                m_active_cluster_extent = {};
                m_pending_rap_blocks.clear();
                m_pending_closed_extents.clear();
            }
        }

        if (e.is_key && e.track_num == 1)
            m_pending_cue_ticks.push_back(pkt_tick);

        if (e.pts_ns > m_max_emitted_pts_ns)
            m_max_emitted_pts_ns = e.pts_ns;
    } catch (const std::exception& ex) {
        Fail(std::string("MatroskaStreamWriter::EmitPacket failed: ") + ex.what());
        return false;
    }
    return true;
}

bool MatroskaStreamWriter::FlushCluster() {
    if (m_cluster == nullptr)
        return true;
    try {
        const uint64_t pos_before = static_cast<uint64_t>(m_io->getFilePointer());
        const auto flush_t0 = std::chrono::steady_clock::now();
        m_io->SetPhase(OutputIoOperation::ClusterWrite);
        m_cluster->Render(*m_io, *m_cues);
        m_io->SetPhase(OutputIoOperation::PayloadWrite);
        const auto flush_t1 = std::chrono::steady_clock::now();
        m_last_flush_ms = std::chrono::duration<double, std::milli>(flush_t1 - flush_t0).count();
        ++m_flush_count;
        const uint64_t pos_after = static_cast<uint64_t>(m_io->getFilePointer());
        if (m_config.collect_media_index && !m_media_index_degraded) {
            try {
                m_active_cluster_extent.file_begin = pos_before;
                m_active_cluster_extent.file_end = pos_after;
                for (const auto& [block, index] : m_pending_rap_blocks) {
                    auto& rap = m_active_cluster_extent.random_access_points[index];
                    rap.cluster_offset = pos_before;
                    rap.block_offset = block->GetElementPosition();
                }
                const size_t bytes =
                    sizeof(ClusterExtent) + m_active_cluster_extent.coverage.capacity() * sizeof(MediaTrackCoverage) +
                    m_active_cluster_extent.random_access_points.capacity() * sizeof(MediaRandomAccessPoint);
                if (bytes > kReceiptPayloadLimit - m_pending_receipt_bytes ||
                    m_pending_closed_extents.size() == kPendingClusterLimit)
                    throw std::length_error("Pending media receipt budget exceeded");
                m_pending_receipt_bytes += bytes;
                m_pending_closed_extents.push_back(std::move(m_active_cluster_extent));
            } catch (...) {
                m_media_index_degraded = true;
                m_pending_closed_extents.clear();
            }
        }
        m_active_cluster_extent = {};
        m_pending_rap_blocks.clear();
        if (pos_after > pos_before) {
            m_bytes_written += pos_after - pos_before;
        }
        // Publish progress for the out-of-thread shutdown observer. This runs on
        // the mux thread's streaming loop AND inside the blocking Finalize() drain,
        // so a long finalize keeps advancing this value while it writes.
        PublishProgress();
        // CueClusterPosition must be relative to the Segment data start (Matroska spec
        // §8.1.6.1): relative = absolute - Segment_element_start - Segment_head_bytes.
        // KaxCluster::GetPosition() ALREADY returns that -- it is defined as
        // ParentSegment->GetRelativePosition(*this). Running it through
        // GetRelativePosition() again would subtract the Segment header twice and
        // point every cue that many bytes short of its cluster.
        const uint64_t cluster_rel = m_cluster->GetPosition();
        for (const uint64_t cue_tick : m_pending_cue_ticks) {
            libmatroska::KaxCuePoint streamed_point;
            auto& point = m_cue_spool ? streamed_point : libebml::AddNewChild<libmatroska::KaxCuePoint>(*m_cues);
            libebml::GetChild<libmatroska::KaxCueTime>(point).SetValue(cue_tick);
            auto& tp = libebml::AddNewChild<libmatroska::KaxCueTrackPositions>(point);
            libebml::GetChild<libmatroska::KaxCueTrack>(tp).SetValue(1);
            libebml::GetChild<libmatroska::KaxCueClusterPosition>(tp).SetValue(cluster_rel);
            if (m_cue_spool)
                point.Render(*m_cue_spool);
        }
        m_pending_cue_ticks.clear();
    } catch (const std::exception& ex) {
        Fail(std::string("MatroskaStreamWriter::FlushCluster failed: ") + ex.what());
        delete m_cluster;
        m_cluster = nullptr;
        m_cluster_bytes.clear();
        m_cluster_payload_bytes = 0;
        return false;
    }
    delete m_cluster;
    m_cluster = nullptr;
    // Block bytes are now serialized to disk; free them.
    m_cluster_bytes.clear();
    m_cluster_payload_bytes = 0;

    // Flush the stream before requesting best-effort OS durability.
    // The cluster Render() above only reaches the CRT stdio buffer / OS
    // write-back cache — a process kill loses at most the in-RAM reorder
    // window plus the not-yet-rendered cluster (a few seconds), but a power
    // loss would additionally lose everything still sitting in the OS cache,
    // which without this flush is unbounded (grows for the whole recording).
    // Gate by wall-clock time (not "every cluster") so this cannot fire faster
    // than the interval even if clusters ever land much more often than the
    // steady-state ~2 s cadence (kClusterBoundaryTicks) — e.g. Finalize()'s forced
    // window drain, which can flush several small clusters back-to-back.
    if (m_io != nullptr) {
        const auto now = std::chrono::steady_clock::now();
        if (m_durability_flush_scheduler.IsDue(now)) {
            std::string reason;
            const auto result = m_io->FlushToDisk(&reason);
            if (result == DurableFileIo::FlushResult::StreamFailure) {
                Fail(reason);
                return false;
            }
            if (result == DurableFileIo::FlushResult::DurabilityFailure)
                LogDurabilityFlushFailure("durability flush failed", reason);
            PublishCommitted(result == DurableFileIo::FlushResult::Success);
            ++m_durability_flush_count;
            m_durability_flush_scheduler.MarkFlushed(now);
        }
    }

    return true;
}

void MatroskaStreamWriter::PublishProgress() noexcept {
    if (m_io == nullptr)
        return;
    // Progress publication is strictly best-effort: getFilePointer() can throw
    // (ftell failure), and this runs from noexcept contexts (including after the
    // final flush, outside Finalize()'s try/catch), so swallow any failure — a
    // stale/unchanged sink value is harmless, it just means one fewer progress
    // sample for the out-of-thread finalize watcher.
    try {
        // Clamp to a running max: some back-patch steps (SeekHead/Segment-size
        // OverwriteHead) seek backward to rewrite an earlier region of the file
        // and may or may not restore the position afterward. Never let a
        // transient backward position regress the published value — the
        // out-of-thread finalize watcher (FinalizeProgressTracker) must see
        // monotonic-or-flat progress, never a spurious drop.
        const uint64_t pos = static_cast<uint64_t>(m_io->getFilePointer());
        if (pos > m_bytes_written) {
            m_bytes_written = pos;
        }
    } catch (...) {
        // keep the last known value
    }
    if (m_progress_sink != nullptr) {
        m_progress_sink->store(m_bytes_written, std::memory_order_relaxed);
    }
}

bool MatroskaStreamWriter::Finalize() {
    if (!m_opened || m_finalized)
        return !m_failed;
    m_finalized = true;

    // Even on prior failure, attempt an honest finalize so the file is closed
    // and as valid as possible. If we already failed, we still close the handle.
    bool ok = !m_failed;

    if (ok) {
        // Drain remaining window into clusters, then flush the last cluster.
        if (!DrainWindow(/*force=*/true))
            ok = false;
    }
    if (ok) {
        if (!FlushCluster())
            ok = false;
    }

    if (ok) {
        try {
            m_io->SetPhase(OutputIoOperation::TrailerWrite);
            // --- Cues ---
            // O(number of keyframes across the whole recording) and disk-bound
            // on a slow/networked target — exactly the phase the finalize
            // shutdown wait (FinalizeProgressTracker) must see advancing, so
            // publish a checkpoint immediately after.
            if (m_cue_spool) {
                const auto bytes = m_cue_spool->getFilePointer();
                static_cast<StreamedCues&>(*m_cues).RenderHeader(*m_io, bytes);
                m_cue_spool->CopyTo(*m_io, bytes);
            } else {
                m_cues->Render(*m_io);
            }
            PublishProgress();

            // --- SeekHead into the reserved placeholder ---
            {
                libmatroska::KaxSeekHead seekhead;
                seekhead.IndexThis(*m_info, *m_segment);
                seekhead.IndexThis(*m_tracks, *m_segment);
                seekhead.IndexThis(*m_cues, *m_segment);
                m_seekhead_void->ReplaceWith(seekhead, *m_io, /*ComeBackAfterward=*/true);
            }
            PublishProgress();

            // --- Back-patch Duration in the already-written Info element ---
            {
                uint64_t last_frame_ns = 0;
                if (m_config.frame_rate_num > 0 && m_config.frame_rate_den > 0) {
                    last_frame_ns = static_cast<uint64_t>(m_config.frame_rate_den) * 1000000000ULL /
                                    static_cast<uint64_t>(m_config.frame_rate_num);
                }
                const uint64_t duration_ns = m_config.timeline_duration_ns.value_or(
                    m_media_index_degraded ? m_max_emitted_pts_ns + last_frame_ns : m_max_covered_end_ns);
                const uint64_t duration_ticks = duration_ns / kTimecodeScaleNs;
                auto& dur = libebml::GetChild<libmatroska::KaxDuration>(*m_info);
                dur.SetValue(static_cast<double>(duration_ticks));
                // Patch ONLY the Duration leaf in place. It was rendered as a
                // fixed 8-byte float at Open() (FLOAT_64), and its ElementPosition
                // was recorded when the parent Info was rendered, so OverwriteData
                // rewrites exactly those 8 data bytes without resizing anything.
                // (Patching the whole Info master would risk a size-mismatch assert
                // if any child's encoded size changed.)
                dur.OverwriteData(*m_io);
            }
            PublishProgress();

            // --- Back-patch the measured content light levels ---
            //
            // Same mechanism as Duration above and for the same reason: the value
            // describes the finished stream, and the element it belongs in sits in
            // the track header at the start of the file. Each leaf was rendered at
            // a fixed width inside the already-written Tracks master, so
            // OverwriteData rewrites exactly its own data bytes; patching the
            // master would risk a size mismatch against any sibling.
            if (m_max_cll_element != nullptr && m_max_fall_element != nullptr) {
                m_max_cll_element->SetValue(ClampContentLightLevel(m_measured_max_cll));
                m_max_cll_element->OverwriteData(*m_io);
                m_max_fall_element->SetValue(ClampContentLightLevel(m_measured_max_fall));
                m_max_fall_element->OverwriteData(*m_io);
            }
            PublishProgress();

            // --- Back-patch the Segment size ---
            {
                const uint64_t eof_pos = static_cast<uint64_t>(m_io->getFilePointer());
                const uint64_t segment_size = eof_pos - m_segment_data_start;
                if (m_segment->ForceSize(segment_size)) {
                    m_segment->OverwriteHead(*m_io);
                }
            }
            PublishProgress();
        } catch (const std::exception& ex) {
            Fail(std::string("MatroskaStreamWriter::Finalize failed: ") + ex.what());
            ok = false;
        }
    }

    // CRT flush is part of successful output. OS durability is separately best-effort.
    if (m_io != nullptr) {
        std::string reason;
        const auto result = m_io->FlushToDisk(&reason);
        if (result == DurableFileIo::FlushResult::StreamFailure)
            Fail(reason);
        if (result == DurableFileIo::FlushResult::DurabilityFailure)
            LogDurabilityFlushFailure("final durability flush failed", reason);
        if (result != DurableFileIo::FlushResult::StreamFailure)
            PublishCommitted(result == DurableFileIo::FlushResult::Success);
        ++m_durability_flush_count;
    }
    // Checkpoint around the final flush too: even though FlushFileBuffers
    // itself does not move the file position, publishing here ensures the
    // watcher's last-progress mark reflects the moment Finalize() actually
    // finished writing, not the last cluster flushed minutes earlier.
    PublishProgress();

    CloseIo();
    m_finalize_succeeded = ok && !m_failed;
    return m_finalize_succeeded;
}

} // namespace exosnap::engine

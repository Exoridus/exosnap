#include "media_identity_sidecar.h"
#include "bounded_metadata_json.h"
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/background_io.h>
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>
#include <windows.h>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

namespace exosnap::engine {
namespace {
using Json = MetadataJson;
constexpr size_t kSidecarBytes = 2 * 1024 * 1024;
std::mutex hash_measurements_mutex;
MediaHashMeasurementSnapshot hash_measurements;
struct HashMeasurement {
    MediaHashCause cause;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    uint64_t bytes = 0;
    bool completed = false;
    bool cancelled = false;
    explicit HashMeasurement(MediaHashCause value) : cause(value) {
        std::lock_guard lock(hash_measurements_mutex);
        ++hash_measurements.total.starts;
        ++hash_measurements.by_cause[static_cast<size_t>(cause)].starts;
    }
    ~HashMeasurement() {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
        std::lock_guard lock(hash_measurements_mutex);
        for (auto* value : {&hash_measurements.total, &hash_measurements.by_cause[static_cast<size_t>(cause)]}) {
            value->bytes += bytes;
            value->completions += completed ? 1 : 0;
            value->cancellations += cancelled ? 1 : 0;
            value->elapsed_ns += elapsed;
        }
    }
};
bool ValidMarkerReference(const std::string& name) {
    if (name.empty() || name.size() > 4096 || name == "." || name == ".." ||
        name.find_first_of("/\\:<>\"|?*") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char value) { return value < 32 || value == 127; }))
        return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0) >
           0;
}
Json ReferenceRanges(const MediaIdentitySidecarReferences& references) {
    MetadataJsonQuotaScope json_quota;
    if (references.marker_refs.size() > 4096 || references.source_ranges_json.size() > kSidecarBytes)
        throw std::invalid_argument("Reference budget exceeded");
    MetadataJsonNameSet names;
    for (const auto& name : references.marker_refs)
        if (!ValidMarkerReference(name) || !names.insert(std::string_view(name)).second)
            throw std::invalid_argument("Invalid marker reference");
    MetadataJsonKeyStack keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 18)
            throw std::invalid_argument("Reference nesting budget exceeded");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<MetadataJsonString>()).second)
            throw std::invalid_argument("Duplicate reference key");
        return true;
    };
    auto ranges = Json::parse(references.source_ranges_json, callback);
    if (!ranges.is_array() || ranges.size() > 4096 ||
        !std::all_of(ranges.begin(), ranges.end(), [](const auto& range) { return range.is_object(); }))
        throw std::invalid_argument("Invalid source range references");
    return ranges;
}
EngineError IdentityError(const char* context, EngineErrorCode code = EngineErrorCode::MetadataIncomplete) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::FileOutput;
    error.context = context;
    return error;
}
struct FileLease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit FileLease(const std::filesystem::path& path, bool explicit_offsets = false)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                                 (explicit_offsets ? FILE_FLAG_OVERLAPPED : 0),
                             nullptr)) {
    }
    ~FileLease() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};
bool Snapshot(HANDLE handle, MediaIdentitySnapshot& snapshot) {
    FILE_ID_INFO identity{};
    FILE_BASIC_INFO basic{};
    LARGE_INTEGER length{};
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        !GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileSizeEx(handle, &length) || length.QuadPart < 0)
        return false;
    snapshot.file_identity.volume_serial = identity.VolumeSerialNumber;
    std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier),
              snapshot.file_identity.file_id.begin());
    snapshot.file_size = static_cast<uint64_t>(length.QuadPart);
    snapshot.last_write_time = static_cast<uint64_t>(basic.LastWriteTime.QuadPart);
    return true;
}
bool SupportedLocalCapability(HANDLE handle) {
    FILE_REMOTE_PROTOCOL_INFO protocol{};
    if (GetFileInformationByHandleEx(handle, FileRemoteProtocolInfo, &protocol, sizeof(protocol)) &&
        protocol.Protocol != 0)
        return false;
    const auto length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length > 32768)
        return false;
    std::vector<wchar_t> path(static_cast<size_t>(length) + 1);
    const auto written = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written >= path.size())
        return false;
    std::vector<wchar_t> volume(32768);
    if (!GetVolumePathNameW(path.data(), volume.data(), static_cast<DWORD>(volume.size())))
        return false;
    const auto kind = GetDriveTypeW(volume.data());
    return kind == DRIVE_FIXED || kind == DRIVE_REMOVABLE || kind == DRIVE_RAMDISK;
}
std::string Hex(std::span<const uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (auto byte : bytes) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 15]);
    }
    return text;
}
AudioMetadataReadResult Status(AudioMetadataStatus status, const char* warning) {
    AudioMetadataReadResult result;
    result.status = status;
    result.warnings.push_back(warning);
    return result;
}
uint64_t Length(const Json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0))
        throw std::invalid_argument("Invalid sidecar media length");
    return value.get<uint64_t>();
}
} // namespace

MediaHashMeasurementSnapshot GlobalMediaHashMeasurements() {
    std::lock_guard lock(hash_measurements_mutex);
    return hash_measurements;
}

struct MediaReadCapability::Impl {
    std::filesystem::path path;
    std::shared_ptr<FileLease> lease;
    MediaIdentitySnapshot identity;
    uint64_t generation = 0;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool hashing = false;
    std::optional<MediaIdentitySnapshot> observed;
    MediaVerificationMeasurements measurements;
    std::shared_ptr<const AudioMetadataReadResult> provisional_audio;
    std::shared_ptr<const AudioMetadataReadResult> verified_audio;
    std::shared_ptr<const AudioMetadataReadResult> adopted_audio;
    std::shared_ptr<const EditMediaMetadata> metadata;
};

MediaReadCapability::MediaReadCapability(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {
}
MediaReadCapability::~MediaReadCapability() = default;
const std::filesystem::path& MediaReadCapability::Path() const {
    return impl_->path;
}
const MediaIdentitySnapshot& MediaReadCapability::Identity() const {
    return impl_->identity;
}
uint64_t MediaReadCapability::Generation() const {
    return impl_->generation;
}
MediaVerificationMeasurements MediaReadCapability::Measurements() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->measurements;
}
Outcome<size_t> MediaReadCapability::ReadAt(uint64_t offset, std::span<uint8_t> bytes) const {
    if (offset >= impl_->identity.file_size || bytes.empty())
        return size_t{0};
    const auto count = static_cast<DWORD>(
        (std::min)({uint64_t{bytes.size()}, impl_->identity.file_size - offset, uint64_t{4 * 1024 * 1024}}));
    OVERLAPPED operation{};
    operation.Offset = static_cast<DWORD>(offset);
    operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent)
        return IdentityError("Cannot allocate media read event", EngineErrorCode::ResourceBudgetExceeded);
    std::unique_ptr<void, decltype(&CloseHandle)> event(operation.hEvent, CloseHandle);
    DWORD read = 0;
    if (!ReadFile(impl_->lease->handle, bytes.data(), count, &read, &operation)) {
        if (GetLastError() != ERROR_IO_PENDING || !GetOverlappedResult(impl_->lease->handle, &operation, &read, TRUE))
            return IdentityError("Admitted media read failed", EngineErrorCode::SourceIntegrityFailure);
    }
    return static_cast<size_t>(read);
}

Outcome<std::shared_ptr<const MediaReadCapability>> AcquireMediaReadCapability(const std::filesystem::path& path) {
    auto lease = std::make_shared<FileLease>(path, true);
    MediaIdentitySnapshot identity;
    if (lease->handle == INVALID_HANDLE_VALUE || !Snapshot(lease->handle, identity)) {
        auto error = IdentityError("Source busy or unavailable", EngineErrorCode::UnavailableSource);
        error.win32_code = GetLastError();
        error.retryability = Retryability::AfterUserAction;
        return error;
    }
    if (!SupportedLocalCapability(lease->handle)) {
        auto error = IdentityError("Typed source access requires a characterized local filesystem",
                                   EngineErrorCode::UnsupportedPlatform);
        error.retryability = Retryability::AfterUserAction;
        return error;
    }
    static std::mutex registry_mutex;
    using SourceKey = std::pair<FileIdentity, std::wstring>;
    static std::map<SourceKey, std::weak_ptr<const MediaReadCapability>> registry;
    static uint64_t next_generation = 0;
    std::error_code path_error;
    auto absolute_path = std::filesystem::absolute(path, path_error);
    auto path_key = (path_error ? path : absolute_path).lexically_normal().wstring();
    std::transform(path_key.begin(), path_key.end(), path_key.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    const SourceKey key{identity.file_identity, std::move(path_key)};
    std::lock_guard lock(registry_mutex);
    std::erase_if(registry, [](const auto& entry) { return entry.second.expired(); });
    if (const auto found = registry.find(key); found != registry.end())
        if (auto current = found->second.lock())
            return current;
    auto impl = std::make_shared<MediaReadCapability::Impl>();
    impl->path = path;
    impl->lease = std::move(lease);
    impl->identity = identity;
    impl->generation = ++next_generation;
    std::shared_ptr<const MediaReadCapability> source(new MediaReadCapability(std::move(impl)));
    registry[key] = source;
    return source;
}

VerifiedMediaIdentity::VerifiedMediaIdentity(std::shared_ptr<const MediaReadCapability> source,
                                             MediaIdentitySnapshot identity)
    : source_(std::move(source)), identity_(std::move(identity)) {
}
const MediaIdentitySnapshot& VerifiedMediaIdentity::Identity() const {
    return identity_;
}
const std::shared_ptr<const MediaReadCapability>& VerifiedMediaIdentity::Source() const {
    return source_;
}

Outcome<std::shared_ptr<const VerifiedMediaIdentity>>
VerifyMediaIdentity(const std::shared_ptr<const MediaReadCapability>& source,
                    std::optional<std::array<uint8_t, 32>> expected, const std::function<bool()>& continue_io,
                    const BackgroundIoAdmission& before_io, const std::atomic<bool>* cancelled) {
    if (!source)
        return IdentityError("No admitted media source", EngineErrorCode::UnavailableSource);
    const auto stopped = [&] {
        return (cancelled && cancelled->load(std::memory_order_relaxed)) || (continue_io && !continue_io());
    };
    auto& state = *source->impl_;
    std::unique_lock lock(state.mutex);
    while (state.hashing) {
        if (stopped())
            return IdentityError("Media identity verification cancelled", EngineErrorCode::Cancelled);
        state.changed.wait_for(lock, std::chrono::milliseconds(10));
    }
    if (stopped())
        return IdentityError("Media identity verification cancelled", EngineErrorCode::Cancelled);
    if (!state.observed) {
        state.hashing = true;
        ++state.measurements.hash_starts;
        lock.unlock();
        const auto started = std::chrono::steady_clock::now();
        HashMeasurement measurement(MediaHashCause::LiveLease);
        uint64_t total = 0;
        auto observed = state.identity;
        const auto release_failed_job = [&] {
            if (lock.owns_lock())
                return;
            lock.lock();
            measurement.bytes = total;
            state.measurements.hash_bytes += total;
            state.measurements.hash_ns +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started)
                    .count();
            state.hashing = false;
            state.changed.notify_all();
        };
        // A callback may throw while the job owns the shared verification slot.
        // Release that slot before unwinding so another consumer can retry.
        struct FailedJobCleanup {
            const decltype(release_failed_job)& release;
            ~FailedJobCleanup() {
                release();
            }
        } cleanup{release_failed_job};
        auto hash = [&]() -> Outcome<MediaIdentitySnapshot> {
            std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), av_free);
            if (!sha || av_sha_init(sha.get(), 256) < 0)
                return IdentityError("Cannot initialize media hash", EngineErrorCode::ResourceBudgetExceeded);
            std::array<uint8_t, 64 * 1024> bytes{};
            while (total < observed.file_size) {
                const auto count = (std::min)(observed.file_size - total, uint64_t{bytes.size()});
                if (stopped() || (before_io && !before_io(count, cancelled)))
                    return IdentityError("Media identity verification cancelled", EngineErrorCode::Cancelled);
                const auto read = source->ReadAt(total, std::span(bytes).first(static_cast<size_t>(count)));
                if (!read.succeeded())
                    return *read.error();
                if (*read.value() == 0)
                    return IdentityError("Admitted media ended during hash", EngineErrorCode::SourceIntegrityFailure);
                av_sha_update(sha.get(), bytes.data(), *read.value());
                total += *read.value();
            }
            MediaIdentitySnapshot after;
            if (!Snapshot(state.lease->handle, after) || after.file_identity != observed.file_identity ||
                after.file_size != observed.file_size || after.last_write_time != observed.last_write_time)
                return IdentityError("Admitted media changed during hash", EngineErrorCode::SourceIntegrityFailure);
            av_sha_final(sha.get(), observed.sha256.data());
            return observed;
        }();
        lock.lock();
        measurement.bytes = total;
        measurement.completed = hash.succeeded();
        measurement.cancelled = !hash.succeeded() && hash.error()->code == EngineErrorCode::Cancelled;
        state.measurements.hash_bytes += total;
        state.measurements.hash_ns +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
        state.hashing = false;
        if (hash.succeeded()) {
            state.observed = *hash.value();
            ++state.measurements.hash_completions;
        } else if (hash.error()->code == EngineErrorCode::Cancelled) {
            ++state.measurements.hash_cancellations;
        }
        state.changed.notify_all();
        if (!hash.succeeded())
            return *hash.error();
    }
    if (expected && *expected != state.observed->sha256)
        return IdentityError("Media content differs from expected identity", EngineErrorCode::SourceIntegrityFailure);
    return std::shared_ptr<const VerifiedMediaIdentity>(new VerifiedMediaIdentity(source, *state.observed));
}

std::shared_ptr<const AudioMetadataReadResult>
CachedMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>& source, bool verified) {
    if (!source)
        return {};
    std::lock_guard lock(source->impl_->mutex);
    return verified ? source->impl_->verified_audio : source->impl_->provisional_audio;
}
void RetainMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>& source,
                              std::shared_ptr<const AudioMetadataReadResult> metadata, bool verified) {
    if (!source)
        return;
    std::lock_guard lock(source->impl_->mutex);
    auto& slot = verified ? source->impl_->verified_audio : source->impl_->provisional_audio;
    if (!slot)
        slot = std::move(metadata);
}
std::shared_ptr<const AudioMetadataReadResult>
CachedAdoptedMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>& source) {
    if (!source)
        return {};
    std::lock_guard lock(source->impl_->mutex);
    return source->impl_->adopted_audio;
}
void RetainAdoptedMediaAudioMetadata(const std::shared_ptr<const MediaReadCapability>& source,
                                     std::shared_ptr<const AudioMetadataReadResult> metadata) {
    if (!source)
        return;
    std::lock_guard lock(source->impl_->mutex);
    if (!source->impl_->adopted_audio)
        source->impl_->adopted_audio = std::move(metadata);
}
std::shared_ptr<const EditMediaMetadata>
CachedEditMediaMetadata(const std::shared_ptr<const MediaReadCapability>& source) {
    if (!source)
        return {};
    std::lock_guard lock(source->impl_->mutex);
    return source->impl_->metadata;
}
void RetainEditMediaMetadata(const std::shared_ptr<const MediaReadCapability>& source,
                             std::shared_ptr<const EditMediaMetadata> metadata) {
    if (!source)
        return;
    std::lock_guard lock(source->impl_->mutex);
    if (!source->impl_->metadata)
        source->impl_->metadata = std::move(metadata);
}

std::filesystem::path MediaIdentitySidecarPath(const std::filesystem::path& media) {
    auto sidecar = media;
    sidecar += L".exosnap.json";
    return sidecar;
}
Outcome<std::shared_ptr<void>> AcquireMediaReadLease(const std::filesystem::path& media) {
    auto lease = std::make_shared<FileLease>(media);
    MediaIdentitySnapshot identity;
    if (lease->handle == INVALID_HANDLE_VALUE || !Snapshot(lease->handle, identity))
        return IdentityError("Cannot acquire immutable media read identity", EngineErrorCode::SourceIntegrityFailure);
    return std::shared_ptr<void>(std::move(lease));
}
Outcome<MediaIdentitySnapshot> HashFinalMedia(const std::filesystem::path& media,
                                              const std::function<bool()>& continue_io,
                                              const BackgroundIoAdmission& before_io,
                                              const std::atomic<bool>* cancelled, MediaHashCause cause) {
    HashMeasurement measurement(cause);
    FileLease file(media);
    MediaIdentitySnapshot before, after;
    if (file.handle == INVALID_HANDLE_VALUE || !Snapshot(file.handle, before))
        return IdentityError("Cannot open finalized media identity", EngineErrorCode::SourceIntegrityFailure);
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), av_free);
    if (!sha || av_sha_init(sha.get(), 256) < 0)
        return IdentityError("Cannot initialize final media hash");
    std::array<uint8_t, 64 * 1024> bytes{};
    uint64_t total = 0;
    if ((continue_io && !continue_io()) || (cancelled && cancelled->load(std::memory_order_relaxed))) {
        measurement.cancelled = true;
        return IdentityError("Media identity verification cancelled", EngineErrorCode::Cancelled);
    }
    while (total < before.file_size) {
        const auto chunk = static_cast<DWORD>((std::min)(before.file_size - total, uint64_t{bytes.size()}));
        if ((continue_io && !continue_io()) || (cancelled && cancelled->load(std::memory_order_relaxed)) ||
            (before_io && !before_io(chunk, cancelled))) {
            measurement.cancelled = true;
            return IdentityError("Media identity verification cancelled", EngineErrorCode::Cancelled);
        }
        DWORD read = 0;
        if (!ReadFile(file.handle, bytes.data(), chunk, &read, nullptr))
            return IdentityError("Final media hash read failed", EngineErrorCode::SourceIntegrityFailure);
        if (read == 0)
            return IdentityError("Media changed during final hash", EngineErrorCode::SourceIntegrityFailure);
        av_sha_update(sha.get(), bytes.data(), read);
        total += read;
        measurement.bytes += read;
    }
    if (!Snapshot(file.handle, after) || before.file_identity != after.file_identity ||
        before.file_size != after.file_size || before.last_write_time != after.last_write_time ||
        total != before.file_size)
        return IdentityError("Media identity changed during final hash", EngineErrorCode::SourceIntegrityFailure);
    av_sha_final(sha.get(), before.sha256.data());
    measurement.completed = true;
    return before;
}
Outcome<std::monostate> WriteMediaIdentitySidecar(const std::filesystem::path& media, const AudioManifest1& manifest,
                                                  const std::function<bool()>& continue_io,
                                                  const std::optional<MediaIdentitySidecarReferences>& references,
                                                  const BackgroundIoAdmission& before_io,
                                                  const std::atomic<bool>* cancelled) {
    auto serialized = SerializeAudioManifest1(manifest);
    if (!serialized.succeeded())
        return *serialized.error();
    const auto lease = AcquireMediaReadLease(media);
    if (!lease.succeeded())
        return *lease.error();
    auto hash = HashFinalMedia(media, continue_io, before_io, cancelled, MediaHashCause::RecordingFinalization);
    if (!hash.succeeded())
        return *hash.error();
    const auto final_path = MediaIdentitySidecarPath(media);
    auto staging = final_path;
    staging += L"." + std::filesystem::path(ToUuid(NewId<MediaId>())).wstring() + L".tmp";
    try {
        MetadataJsonQuotaScope json_quota;
        MediaIdentitySidecarReferences retained;
        if (references) {
            retained = *references;
        } else {
            std::error_code error;
            if (std::filesystem::exists(final_path, error)) {
                auto existing = ReadMediaIdentitySidecar(media, continue_io, before_io, cancelled);
                if (existing.status != AudioMetadataStatus::Verified || !existing.manifest ||
                    existing.manifest->media_id != manifest.media_id)
                    return IdentityError("Cannot preserve unverified identity references");
                retained = std::move(existing.references);
            } else if (error) {
                return IdentityError("Cannot inspect existing identity references");
            }
        }
        std::string ranges_text;
        {
            const auto ranges = ReferenceRanges(retained);
            const auto compact = ranges.dump();
            ranges_text.assign(compact.data(), compact.size());
        }
        std::string payload;
        payload.reserve(kSidecarBytes);
        const auto append = [&](std::string_view part) {
            if (part.size() > kSidecarBytes - payload.size())
                throw std::length_error("Identity sidecar exceeds byte budget");
            payload.append(part);
        };
        append("{\"audio_manifest\":");
        append(*serialized.value());
        append(",\"file_size\":");
        append(std::to_string(hash.value()->file_size));
        append(",\"manifest_version\":1,\"marker_refs\":[");
        for (size_t index = 0; index < retained.marker_refs.size(); ++index) {
            if (index != 0)
                append(",");
            const auto quoted = Json(retained.marker_refs[index]).dump();
            append(std::string_view(quoted.data(), quoted.size()));
        }
        append("],\"media_id\":");
        const auto quoted_id = Json(ToUuid(manifest.media_id)).dump();
        append(std::string_view(quoted_id.data(), quoted_id.size()));
        append(",\"sha256\":");
        const auto quoted_hash = Json(Hex(hash.value()->sha256)).dump();
        append(std::string_view(quoted_hash.data(), quoted_hash.size()));
        append(",\"source_ranges\":");
        append(ranges_text);
        append("}");
        HANDLE file =
            CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return IdentityError("Cannot create identity sidecar staging file");
        DWORD written = 0;
        const bool admitted = (!cancelled || !cancelled->load(std::memory_order_relaxed)) &&
                              (!before_io || before_io(payload.size(), cancelled));
        const bool complete = admitted &&
                              WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
                              written == payload.size() && FlushFileBuffers(file);
        const bool closed = CloseHandle(file) != FALSE;
        if (!complete || !closed ||
            !MoveFileExW(staging.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(staging.c_str());
            return IdentityError("Cannot publish complete identity sidecar");
        }
        return std::monostate{};
    } catch (const MetadataJsonBudgetExceeded&) {
        DeleteFileW(staging.c_str());
        return IdentityError("Identity sidecar JSON memory budget exceeded", EngineErrorCode::ResourceBudgetExceeded);
    } catch (const std::length_error&) {
        DeleteFileW(staging.c_str());
        return IdentityError("Identity sidecar exceeds byte budget");
    } catch (...) {
        DeleteFileW(staging.c_str());
        return IdentityError("Cannot serialize identity sidecar");
    }
}
AudioMetadataReadResult
ReadMediaIdentitySidecar(const std::filesystem::path& media, const std::function<bool()>& continue_io,
                         const BackgroundIoAdmission& before_io, const std::atomic<bool>* cancelled,
                         const std::shared_ptr<const MediaReadCapability>& source, bool verify_content) {
    const auto path = MediaIdentitySidecarPath(media);
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return Status(error == std::errc::no_such_file_or_directory ? AudioMetadataStatus::Missing
                                                                    : AudioMetadataStatus::Invalid,
                      "Audio identity sidecar is missing or unavailable");
    if (size == 0 || size > kSidecarBytes)
        return Status(AudioMetadataStatus::Invalid, "Audio identity sidecar exceeds byte budget");
    try {
        MetadataJsonQuotaScope json_quota;
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return Status(AudioMetadataStatus::Invalid, "Audio identity sidecar is unavailable");
        std::unique_ptr<void, decltype(&CloseHandle)> input(handle, CloseHandle);
        std::string bytes(static_cast<size_t>(size), '\0');
        DWORD read = 0;
        LARGE_INTEGER final_size{};
        if ((cancelled && cancelled->load(std::memory_order_relaxed)) || (before_io && !before_io(size, cancelled)) ||
            !ReadFile(handle, bytes.data(), static_cast<DWORD>(size), &read, nullptr) || read != size ||
            !GetFileSizeEx(handle, &final_size) || final_size.QuadPart != static_cast<int64_t>(size))
            return Status(AudioMetadataStatus::Invalid, "Audio identity sidecar changed during read");
        MetadataJsonKeyStack keys;
        const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 18)
                throw std::invalid_argument("Sidecar nesting budget exceeded");
            if (event == Json::parse_event_t::object_start)
                keys.emplace_back();
            else if (event == Json::parse_event_t::object_end)
                keys.pop_back();
            else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<MetadataJsonString>()).second)
                throw std::invalid_argument("Duplicate sidecar key");
            return true;
        };
        const auto json = Json::parse(bytes, callback);
        if (!json.is_object() || json.size() != 7 || Length(json.at("manifest_version")) != 1 ||
            !json.at("marker_refs").is_array() || !json.at("source_ranges").is_array() ||
            json.at("marker_refs").size() > 4096 || json.at("source_ranges").size() > 4096)
            return Status(AudioMetadataStatus::Invalid, "Invalid identity sidecar structure");
        const auto serialized_manifest = json.at("audio_manifest").dump();
        auto manifest = ParseAudioManifest1(std::string_view(serialized_manifest.data(), serialized_manifest.size()));
        if (!manifest.succeeded() && manifest.error()->code == EngineErrorCode::ResourceBudgetExceeded)
            return Status(AudioMetadataStatus::Invalid, "Identity sidecar JSON memory budget exceeded");
        if (!manifest.succeeded() || json.at("media_id") != ToUuid(manifest.value()->media_id))
            return Status(AudioMetadataStatus::Invalid, "Identity sidecar manifest is invalid");
        const auto& hash_text = json.at("sha256").get_ref<const Json::string_t&>();
        const std::string expected_hash(hash_text.data(), hash_text.size());
        if (expected_hash.size() != 64 || expected_hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            return Status(AudioMetadataStatus::Invalid, "Identity sidecar hash is invalid");
        AudioMetadataReadResult result;
        std::array<uint8_t, 32> expected{};
        const auto nibble = [](char value) {
            return static_cast<uint8_t>(value <= '9' ? value - '0' : value - 'a' + 10);
        };
        for (size_t index = 0; index < expected.size(); ++index)
            expected[index] =
                static_cast<uint8_t>((nibble(expected_hash[index * 2]) << 4) | nibble(expected_hash[index * 2 + 1]));
        result.expected_content_sha256 = expected;
        if (verify_content) {
            const auto hash = source ? [&]() -> Outcome<MediaIdentitySnapshot> {
                auto proof = VerifyMediaIdentity(source, expected, continue_io, before_io, cancelled);
                return proof.succeeded() ? Outcome<MediaIdentitySnapshot>{(*proof.value())->Identity()}
                                         : Outcome<MediaIdentitySnapshot>{*proof.error()};
            }()
                : HashFinalMedia(media, continue_io, before_io, cancelled, MediaHashCause::Sidecar);
            if (!hash.succeeded() || Length(json.at("file_size")) != hash.value()->file_size ||
                expected_hash != Hex(hash.value()->sha256))
                return Status(AudioMetadataStatus::Stale, "Audio identity sidecar does not match final media bytes");
        } else if (!source || Length(json.at("file_size")) != source->Identity().file_size) {
            return Status(AudioMetadataStatus::Stale, "Audio identity sidecar does not match admitted media size");
        }
        for (const auto& marker : json.at("marker_refs")) {
            const auto& name = marker.get_ref<const Json::string_t&>();
            result.references.marker_refs.emplace_back(name.data(), name.size());
        }
        const auto serialized_ranges = json.at("source_ranges").dump();
        result.references.source_ranges_json.assign(serialized_ranges.data(), serialized_ranges.size());
        (void)ReferenceRanges(result.references);
        result.status = verify_content ? AudioMetadataStatus::Verified : AudioMetadataStatus::Pending;
        result.selection_media_id = manifest.value()->media_id;
        result.manifest = std::move(*manifest.value());
        for (const auto& track : result.manifest->tracks)
            result.actual_tracks.push_back(track.container);
        return result;
    } catch (const MetadataJsonBudgetExceeded&) {
        return Status(AudioMetadataStatus::Invalid, "Identity sidecar JSON memory budget exceeded");
    } catch (...) {
        return Status(AudioMetadataStatus::Invalid, "Invalid bounded identity sidecar");
    }
}

Outcome<std::monostate> UpdateMediaIdentityMarkerReference(const std::filesystem::path& media,
                                                           const std::optional<std::string>& marker_reference,
                                                           const std::function<bool()>& continue_io,
                                                           const std::optional<std::string>& owned_reference) {
    if (continue_io && !continue_io())
        return IdentityError("Marker reference update cancelled", EngineErrorCode::Cancelled);
    std::error_code error;
    if (!std::filesystem::exists(MediaIdentitySidecarPath(media), error)) {
        if (error)
            return IdentityError("Cannot inspect staged identity sidecar");
        return std::monostate{};
    }
    auto metadata = ReadMediaIdentitySidecar(media, continue_io);
    if (continue_io && !continue_io())
        return IdentityError("Marker reference update cancelled", EngineErrorCode::Cancelled);
    if (metadata.status != AudioMetadataStatus::Verified || !metadata.manifest)
        return IdentityError("Cannot update unverified identity sidecar references");
    if (owned_reference)
        std::erase(metadata.references.marker_refs, *owned_reference);
    if (marker_reference && std::find(metadata.references.marker_refs.begin(), metadata.references.marker_refs.end(),
                                      *marker_reference) == metadata.references.marker_refs.end())
        metadata.references.marker_refs.push_back(*marker_reference);
    return WriteMediaIdentitySidecar(media, *metadata.manifest, continue_io, metadata.references);
}

MediaIdentityReferenceCopyGuard::~MediaIdentityReferenceCopyGuard() {
    if (!committed)
        for (const auto& path : created_paths)
            DeleteFileW(path.c_str());
}

std::vector<std::filesystem::path> MediaIdentityReferenceCopyGuard::Commit() {
    committed = true;
    return std::move(created_paths);
}

Outcome<std::monostate> CopyMediaIdentityReferenceFiles(const std::filesystem::path& source,
                                                        const std::filesystem::path& output,
                                                        const MediaIdentitySidecarReferences& references,
                                                        MediaIdentityReferenceCopyGuard* guard,
                                                        const BackgroundIoAdmission& before_io,
                                                        const std::atomic<bool>* cancelled) {
    MediaIdentityReferenceCopyGuard local_guard;
    auto* owner = guard ? guard : &local_guard;
    const auto admit = [&](uint64_t bytes) {
        return (!cancelled || !cancelled->load(std::memory_order_relaxed)) &&
               (!before_io || before_io(bytes, cancelled));
    };
    try {
        MetadataJsonQuotaScope json_quota;
        (void)ReferenceRanges(references);
        owner->created_paths.reserve(owner->created_paths.size() + references.marker_refs.size());
        uint64_t total_bytes = 0;
        for (const auto& name : references.marker_refs) {
            const auto sibling = std::filesystem::path(std::u8string(name.begin(), name.end()));
            const auto source_path = source.parent_path() / sibling;
            auto output_path = output.parent_path() / sibling;
            const auto lease = AcquireMediaReadLease(source_path);
            if (!lease.succeeded())
                return IdentityError("A referenced marker companion is unavailable");
            auto source_guard = std::static_pointer_cast<FileLease>(*lease.value());
            std::error_code error;
            const auto size = std::filesystem::file_size(source_path, error);
            if (error || size > kSidecarBytes || total_bytes > 32 * 1024 * 1024 - size)
                return IdentityError("Marker companion byte budget exceeded");
            total_bytes += size;
            const bool exists = std::filesystem::exists(output_path, error);
            if (error)
                return IdentityError("Cannot inspect marker companion destination");
            if (exists) {
                if (std::filesystem::equivalent(source_path, output_path, error) && !error)
                    continue;
                const auto output_lease = AcquireMediaReadLease(output_path);
                if (!output_lease.succeeded() || std::filesystem::file_size(output_path, error) != size || error)
                    return IdentityError("Conflicting marker companion destination");
                auto existing_guard = std::static_pointer_cast<FileLease>(*output_lease.value());
                std::array<char, 64 * 1024> left{}, right{};
                uint64_t remaining = size;
                while (remaining) {
                    const auto chunk = static_cast<DWORD>((std::min)(remaining, static_cast<uint64_t>(left.size())));
                    DWORD left_read = 0, right_read = 0;
                    if (!admit(chunk) || !ReadFile(source_guard->handle, left.data(), chunk, &left_read, nullptr) ||
                        left_read != chunk || !admit(chunk) ||
                        !ReadFile(existing_guard->handle, right.data(), chunk, &right_read, nullptr) ||
                        right_read != chunk || !std::equal(left.begin(), left.begin() + chunk, right.begin()))
                        return IdentityError("Conflicting marker companion bytes");
                    remaining -= chunk;
                }
            } else {
                HANDLE destination = CreateFileW(output_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                                 FILE_ATTRIBUTE_NORMAL, nullptr);
                if (destination == INVALID_HANDLE_VALUE)
                    return IdentityError("Cannot transport marker companion");
                std::unique_ptr<void, decltype(&CloseHandle)> destination_guard(destination, CloseHandle);
                // Claim ownership before I/O so a partial copy is removed without
                // ever claiming an existing or concurrently created destination.
                owner->created_paths.push_back(std::move(output_path));
                std::array<char, 64 * 1024> bytes{};
                uint64_t remaining = size;
                while (remaining) {
                    const auto chunk = static_cast<DWORD>((std::min)(remaining, static_cast<uint64_t>(bytes.size())));
                    DWORD read = 0, written = 0;
                    if (!admit(chunk) || !ReadFile(source_guard->handle, bytes.data(), chunk, &read, nullptr) ||
                        read != chunk || !admit(chunk) ||
                        !WriteFile(destination, bytes.data(), chunk, &written, nullptr) || written != chunk)
                        return IdentityError("Cannot complete marker companion copy");
                    remaining -= chunk;
                }
                LARGE_INTEGER final_source_size{};
                if (!GetFileSizeEx(source_guard->handle, &final_source_size) ||
                    final_source_size.QuadPart != static_cast<int64_t>(size) || !FlushFileBuffers(destination))
                    return IdentityError("Cannot finalize marker companion copy");
                if (!CloseHandle(destination_guard.release()))
                    return IdentityError("Cannot close marker companion copy");
            }
        }
        if (!guard)
            (void)local_guard.Commit();
        return std::monostate{};
    } catch (const MetadataJsonBudgetExceeded&) {
        return IdentityError("Marker association JSON memory budget exceeded", EngineErrorCode::ResourceBudgetExceeded);
    } catch (...) {
        return IdentityError("Invalid marker companion association");
    }
}

} // namespace exosnap::engine

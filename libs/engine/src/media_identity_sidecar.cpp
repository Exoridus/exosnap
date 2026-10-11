#include "media_identity_sidecar.h"
#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
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
using Json = nlohmann::json;
constexpr size_t kSidecarBytes = 2 * 1024 * 1024;
bool ValidMarkerReference(const std::string& name) {
    if (name.empty() || name.size() > 4096 || name == "." || name == ".." ||
        name.find_first_of("/\\:<>\"|?*") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char value) { return value < 32 || value == 127; }))
        return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0) >
           0;
}
Json ReferenceRanges(const MediaIdentitySidecarReferences& references) {
    if (references.marker_refs.size() > 4096 || references.source_ranges_json.size() > kSidecarBytes)
        throw std::invalid_argument("Reference budget exceeded");
    std::set<std::string> names;
    for (const auto& name : references.marker_refs)
        if (!ValidMarkerReference(name) || !names.insert(name).second)
            throw std::invalid_argument("Invalid marker reference");
    std::vector<std::set<std::string>> keys;
    const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > 18)
            throw std::invalid_argument("Reference nesting budget exceeded");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
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
    explicit FileLease(const std::filesystem::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {
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
                                              const std::function<bool()>& continue_io) {
    FileLease file(media);
    MediaIdentitySnapshot before, after;
    if (file.handle == INVALID_HANDLE_VALUE || !Snapshot(file.handle, before))
        return IdentityError("Cannot open finalized media identity", EngineErrorCode::SourceIntegrityFailure);
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), av_free);
    if (!sha || av_sha_init(sha.get(), 256) < 0)
        return IdentityError("Cannot initialize final media hash");
    std::array<uint8_t, 64 * 1024> bytes{};
    uint64_t total = 0;
    for (;;) {
        if (continue_io && !continue_io())
            return IdentityError("Media identity verification cancelled", EngineErrorCode::Cancelled);
        DWORD read = 0;
        if (!ReadFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
            return IdentityError("Final media hash read failed", EngineErrorCode::SourceIntegrityFailure);
        if (read == 0)
            break;
        if (read > before.file_size - (std::min)(total, before.file_size))
            return IdentityError("Media changed during final hash", EngineErrorCode::SourceIntegrityFailure);
        av_sha_update(sha.get(), bytes.data(), read);
        total += read;
    }
    if (!Snapshot(file.handle, after) || before.file_identity != after.file_identity ||
        before.file_size != after.file_size || before.last_write_time != after.last_write_time ||
        total != before.file_size)
        return IdentityError("Media identity changed during final hash", EngineErrorCode::SourceIntegrityFailure);
    av_sha_final(sha.get(), before.sha256.data());
    return before;
}
Outcome<std::monostate> WriteMediaIdentitySidecar(const std::filesystem::path& media, const AudioManifest1& manifest,
                                                  const std::function<bool()>& continue_io,
                                                  const std::optional<MediaIdentitySidecarReferences>& references) {
    auto serialized = SerializeAudioManifest1(manifest);
    if (!serialized.succeeded())
        return *serialized.error();
    const auto lease = AcquireMediaReadLease(media);
    if (!lease.succeeded())
        return *lease.error();
    auto hash = HashFinalMedia(media, continue_io);
    if (!hash.succeeded())
        return *hash.error();
    const auto final_path = MediaIdentitySidecarPath(media);
    auto staging = final_path;
    staging += L"." + std::filesystem::path(ToUuid(NewId<MediaId>())).wstring() + L".tmp";
    try {
        MediaIdentitySidecarReferences retained;
        if (references) {
            retained = *references;
        } else {
            std::error_code error;
            if (std::filesystem::exists(final_path, error)) {
                auto existing = ReadMediaIdentitySidecar(media, continue_io);
                if (existing.status != AudioMetadataStatus::Verified || !existing.manifest ||
                    existing.manifest->media_id != manifest.media_id)
                    return IdentityError("Cannot preserve unverified identity references");
                retained = std::move(existing.references);
            } else if (error) {
                return IdentityError("Cannot inspect existing identity references");
            }
        }
        const auto ranges = ReferenceRanges(retained);
        const auto payload = Json{
            {"manifest_version", 1},
            {"media_id", ToUuid(manifest.media_id)},
            {"file_size", hash.value()->file_size},
            {"sha256", Hex(hash.value()->sha256)},
            {"audio_manifest", Json::parse(*serialized.value())},
            {"marker_refs", retained.marker_refs},
            {"source_ranges",
             ranges}}.dump();
        if (payload.size() > kSidecarBytes)
            return IdentityError("Identity sidecar exceeds byte budget");
        HANDLE file =
            CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return IdentityError("Cannot create identity sidecar staging file");
        DWORD written = 0;
        const bool complete = WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) &&
                              written == payload.size() && FlushFileBuffers(file);
        const bool closed = CloseHandle(file) != FALSE;
        if (!complete || !closed ||
            !MoveFileExW(staging.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(staging.c_str());
            return IdentityError("Cannot publish complete identity sidecar");
        }
        return std::monostate{};
    } catch (...) {
        DeleteFileW(staging.c_str());
        return IdentityError("Cannot serialize identity sidecar");
    }
}
AudioMetadataReadResult ReadMediaIdentitySidecar(const std::filesystem::path& media,
                                                 const std::function<bool()>& continue_io) {
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
        std::ifstream input(path, std::ios::binary);
        std::string bytes(static_cast<size_t>(size), '\0');
        if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
            input.peek() != std::char_traits<char>::eof())
            return Status(AudioMetadataStatus::Invalid, "Audio identity sidecar changed during read");
        std::vector<std::set<std::string>> keys;
        const auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth > 18)
                throw std::invalid_argument("Sidecar nesting budget exceeded");
            if (event == Json::parse_event_t::object_start)
                keys.emplace_back();
            else if (event == Json::parse_event_t::object_end)
                keys.pop_back();
            else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                throw std::invalid_argument("Duplicate sidecar key");
            return true;
        };
        const auto json = Json::parse(bytes, callback);
        if (!json.is_object() || json.size() != 7 || Length(json.at("manifest_version")) != 1 ||
            !json.at("marker_refs").is_array() || !json.at("source_ranges").is_array() ||
            json.at("marker_refs").size() > 4096 || json.at("source_ranges").size() > 4096)
            return Status(AudioMetadataStatus::Invalid, "Invalid identity sidecar structure");
        auto manifest = ParseAudioManifest1(json.at("audio_manifest").dump());
        if (!manifest.succeeded() || json.at("media_id") != ToUuid(manifest.value()->media_id))
            return Status(AudioMetadataStatus::Invalid, "Identity sidecar manifest is invalid");
        const auto expected_hash = json.at("sha256").get<std::string>();
        if (expected_hash.size() != 64 || expected_hash.find_first_not_of("0123456789abcdef") != std::string::npos)
            return Status(AudioMetadataStatus::Invalid, "Identity sidecar hash is invalid");
        const auto hash = HashFinalMedia(media, continue_io);
        if (!hash.succeeded() || Length(json.at("file_size")) != hash.value()->file_size ||
            expected_hash != Hex(hash.value()->sha256))
            return Status(AudioMetadataStatus::Stale, "Audio identity sidecar does not match final media bytes");
        AudioMetadataReadResult result;
        result.references.marker_refs = json.at("marker_refs").get<std::vector<std::string>>();
        result.references.source_ranges_json = json.at("source_ranges").dump();
        (void)ReferenceRanges(result.references);
        result.status = AudioMetadataStatus::Verified;
        result.selection_media_id = manifest.value()->media_id;
        result.manifest = std::move(*manifest.value());
        for (const auto& track : result.manifest->tracks)
            result.actual_tracks.push_back(track.container);
        return result;
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
                                                        MediaIdentityReferenceCopyGuard* guard) {
    MediaIdentityReferenceCopyGuard local_guard;
    auto* owner = guard ? guard : &local_guard;
    try {
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
                std::ifstream input(source_path, std::ios::binary);
                std::ifstream existing(output_path, std::ios::binary);
                std::string left(static_cast<size_t>(size), '\0');
                std::string right(static_cast<size_t>(size), '\0');
                if (!input.read(left.data(), static_cast<std::streamsize>(size)) ||
                    !existing.read(right.data(), static_cast<std::streamsize>(size)) || left != right)
                    return IdentityError("Conflicting marker companion bytes");
            } else {
                std::ifstream input(source_path, std::ios::binary);
                if (!input)
                    return IdentityError("Cannot read marker companion");
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
                    DWORD written = 0;
                    if (!input.read(bytes.data(), chunk) ||
                        !WriteFile(destination, bytes.data(), chunk, &written, nullptr) || written != chunk)
                        return IdentityError("Cannot complete marker companion copy");
                    remaining -= chunk;
                }
                if (input.peek() != std::char_traits<char>::eof() || !FlushFileBuffers(destination))
                    return IdentityError("Cannot finalize marker companion copy");
                if (!CloseHandle(destination_guard.release()))
                    return IdentityError("Cannot close marker companion copy");
            }
        }
        if (!guard)
            (void)local_guard.Commit();
        return std::monostate{};
    } catch (...) {
        return IdentityError("Invalid marker companion association");
    }
}

} // namespace exosnap::engine

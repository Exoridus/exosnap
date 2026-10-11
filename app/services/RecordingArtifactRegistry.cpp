#include "RecordingArtifactRegistry.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace exosnap {
namespace {

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

bool ReadIdentity(HANDLE handle, engine::FileIdentity& identity) {
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info)))
        return false;
    identity.volume_serial = info.VolumeSerialNumber;
    std::copy(std::begin(info.FileId.Identifier), std::end(info.FileId.Identifier), identity.file_id.begin());
    return true;
}

HANDLE OpenSource(const std::filesystem::path& path, DWORD access) {
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool DeleteExact(const std::filesystem::path& path, const engine::FileIdentity& expected) {
    Handle file{OpenSource(path, DELETE | FILE_READ_ATTRIBUTES)};
    engine::FileIdentity actual;
    if (file.value == INVALID_HANDLE_VALUE || !ReadIdentity(file.value, actual) || actual != expected)
        return false;
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
}

bool RenameExact(const std::filesystem::path& source, const std::filesystem::path& destination,
                 const engine::FileIdentity& expected) {
    Handle file{OpenSource(source, DELETE | FILE_READ_ATTRIBUTES)};
    engine::FileIdentity actual;
    if (file.value == INVALID_HANDLE_VALUE || !ReadIdentity(file.value, actual) || actual != expected)
        return false;
    const DWORD attributes = GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return false;
    if (const DWORD reason = GetLastError(); reason != ERROR_FILE_NOT_FOUND && reason != ERROR_PATH_NOT_FOUND)
        return false;
    const auto absolute_destination = std::filesystem::absolute(destination).native();
    const size_t name_bytes = absolute_destination.size() * sizeof(wchar_t);
    if (name_bytes > (std::numeric_limits<DWORD>::max)() - sizeof(FILE_RENAME_INFO) - sizeof(wchar_t))
        return false;
    const size_t buffer_bytes = sizeof(FILE_RENAME_INFO) + name_bytes + sizeof(wchar_t);
    std::vector<uint64_t> storage((buffer_bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->Flags = FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename->FileName, absolute_destination.data(), name_bytes);
    return SetFileInformationByHandle(file.value, FileRenameInfoEx, rename, static_cast<DWORD>(buffer_bytes)) != 0;
}

RecordingArtifactKey KeyOf(const engine::RecordingMediaDescriptor& descriptor) {
    return {descriptor.recording_id, descriptor.segment_id, descriptor.writer_generation,
            descriptor.descriptor_revision};
}

} // namespace

struct RecordingArtifactRegistry::State {
    struct Entry {
        std::filesystem::path path;
        engine::FileIdentity identity;
        size_t leases = 0;
        bool writer_open = false;
        bool busy = false;
        bool deletion_requested = false;
    };

    std::mutex mutex;
    std::condition_variable changed;
    std::map<RecordingArtifactKey, Entry> entries;

    void Release(const RecordingArtifactKey& key) noexcept {
        std::filesystem::path path;
        engine::FileIdentity identity;
        bool delete_now = false;
        try {
            std::lock_guard lock(mutex);
            auto it = entries.find(key);
            if (it == entries.end() || it->second.leases == 0)
                return;
            auto& entry = it->second;
            --entry.leases;
            if (entry.leases == 0 && entry.deletion_requested && !entry.writer_open && !entry.busy) {
                path = entry.path;
                identity = entry.identity;
                entry.busy = true;
                delete_now = true;
            }
        } catch (...) {
            return;
        }
        if (!delete_now)
            return;
        const bool deleted = DeleteExact(path, identity);
        {
            std::lock_guard lock(mutex);
            auto it = entries.find(key);
            if (it != entries.end()) {
                if (deleted)
                    entries.erase(it);
                else
                    it->second.busy = false;
            }
        }
        changed.notify_all();
    }
};

RecordingArtifactRegistry::SourceLease::SourceLease(std::shared_ptr<State> state, RecordingArtifactKey key,
                                                    HANDLE handle, engine::FileIdentity identity,
                                                    std::filesystem::path path,
                                                    std::vector<engine::ClusterExtent> extents,
                                                    uint64_t readable_end_offset) noexcept
    : state_(std::move(state)), key_(key), handle_(handle), identity_(identity), path_(std::move(path)),
      extents_(std::move(extents)), readable_end_offset_(readable_end_offset) {
}

RecordingArtifactRegistry::SourceLease::SourceLease(SourceLease&& other) noexcept
    : state_(std::move(other.state_)), key_(other.key_), handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
      identity_(other.identity_), path_(std::move(other.path_)), extents_(std::move(other.extents_)),
      readable_end_offset_(other.readable_end_offset_) {
}

RecordingArtifactRegistry::SourceLease&
RecordingArtifactRegistry::SourceLease::operator=(SourceLease&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = std::move(other.state_);
        key_ = other.key_;
        handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        identity_ = other.identity_;
        path_ = std::move(other.path_);
        extents_ = std::move(other.extents_);
        readable_end_offset_ = other.readable_end_offset_;
    }
    return *this;
}

RecordingArtifactRegistry::SourceLease::~SourceLease() {
    Release();
}

void RecordingArtifactRegistry::SourceLease::Release() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    if (state_) {
        state_->Release(key_);
        state_.reset();
    }
}

RecordingArtifactRegistry::RecordingArtifactRegistry() : state_(std::make_shared<State>()) {
}

bool RecordingArtifactRegistry::RegisterSource(const engine::RecordingMediaDescriptor& descriptor, bool writer_open) {
    const auto key = KeyOf(descriptor);
    if (key.recording_id == engine::RecordingId{} || key.segment_id == engine::SegmentId{} ||
        key.writer_generation == 0 || key.descriptor_revision == 0 ||
        descriptor.file_identity == engine::FileIdentity{} || descriptor.path.empty())
        return false;
    const std::filesystem::path path(descriptor.path);
    Handle file{OpenSource(path, FILE_READ_ATTRIBUTES)};
    engine::FileIdentity actual;
    if (file.value == INVALID_HANDLE_VALUE || !ReadIdentity(file.value, actual) || actual != descriptor.file_identity)
        return false;
    std::lock_guard lock(state_->mutex);
    auto [it, inserted] = state_->entries.try_emplace(key, State::Entry{path, actual, 0, writer_open});
    return inserted || (it->second.path == path && it->second.identity == actual &&
                        it->second.writer_open == writer_open && !it->second.deletion_requested);
}

std::optional<RecordingArtifactRegistry::SourceLease>
RecordingArtifactRegistry::AcquireSourceLease(const RecordingArtifactKey& key, const engine::FileIdentity& identity,
                                              std::vector<engine::ClusterExtent> extents,
                                              uint64_t readable_end_offset) {
    if (!extents.empty()) {
        uint64_t previous_end = 0;
        for (const auto& extent : extents) {
            if (extent.file_end <= extent.file_begin || extent.file_end > readable_end_offset ||
                (previous_end != 0 && extent.file_begin < previous_end))
                return std::nullopt;
            previous_end = extent.file_end;
        }
    }
    std::filesystem::path path;
    {
        std::unique_lock lock(state_->mutex);
        state_->changed.wait(lock, [&] {
            const auto it = state_->entries.find(key);
            return it == state_->entries.end() || !it->second.busy;
        });
        auto it = state_->entries.find(key);
        if (it == state_->entries.end() || it->second.deletion_requested || it->second.identity != identity)
            return std::nullopt;
        ++it->second.leases;
        path = it->second.path;
    }
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        Handle file{OpenSource(path, GENERIC_READ)};
        engine::FileIdentity actual;
        if (file.value != INVALID_HANDLE_VALUE && ReadIdentity(file.value, actual) && actual == identity) {
            const HANDLE owned = std::exchange(file.value, INVALID_HANDLE_VALUE);
            return SourceLease(state_, key, owned, actual, std::move(path), std::move(extents), readable_end_offset);
        }
        std::unique_lock lock(state_->mutex);
        state_->changed.wait(lock, [&] {
            const auto it = state_->entries.find(key);
            return it == state_->entries.end() || !it->second.busy;
        });
        const auto it = state_->entries.find(key);
        if (it == state_->entries.end() || it->second.path == path)
            break;
        path = it->second.path;
    }
    state_->Release(key);
    return std::nullopt;
}

bool RecordingArtifactRegistry::MarkWriterClosed(const RecordingArtifactKey& key) {
    bool delete_pending = false;
    {
        std::lock_guard lock(state_->mutex);
        const auto it = state_->entries.find(key);
        if (it == state_->entries.end() || it->second.busy)
            return false;
        it->second.writer_open = false;
        delete_pending = it->second.deletion_requested && it->second.leases == 0;
    }
    if (delete_pending)
        (void)RequestArtifactDeletion(key);
    return true;
}

ArtifactDisposition RecordingArtifactRegistry::RenameClosed(const RecordingArtifactKey& key,
                                                            const std::filesystem::path& destination,
                                                            bool defer_while_leased) {
    if (destination.empty())
        return ArtifactDisposition::Failure;
    std::filesystem::path updated_path = destination;
    std::filesystem::path source;
    engine::FileIdentity identity;
    {
        std::lock_guard lock(state_->mutex);
        const auto it = state_->entries.find(key);
        if (it == state_->entries.end())
            return ArtifactDisposition::NotFound;
        if (it->second.writer_open || it->second.busy || it->second.deletion_requested)
            return ArtifactDisposition::Failure;
        if (defer_while_leased && it->second.leases != 0)
            return ArtifactDisposition::DeferredByLease;
        it->second.busy = true;
        source = it->second.path;
        identity = it->second.identity;
    }
    bool renamed = false;
    try {
        renamed = RenameExact(source, destination, identity);
    } catch (...) {
    }
    bool delete_pending = false;
    {
        std::lock_guard lock(state_->mutex);
        auto& entry = state_->entries.at(key);
        if (renamed)
            entry.path.swap(updated_path);
        else
            entry.deletion_requested = false;
        delete_pending = renamed && entry.deletion_requested && entry.leases == 0;
        entry.busy = false;
    }
    state_->changed.notify_all();
    if (delete_pending)
        (void)RequestArtifactDeletion(key);
    return renamed ? ArtifactDisposition::Completed : ArtifactDisposition::Failure;
}

ArtifactDisposition RecordingArtifactRegistry::RebindClosedSource(const RecordingArtifactKey& key,
                                                                  const std::filesystem::path& destination) {
    if (destination.empty())
        return ArtifactDisposition::Failure;
    std::filesystem::path updated_path = destination;
    engine::FileIdentity identity;
    {
        std::lock_guard lock(state_->mutex);
        const auto it = state_->entries.find(key);
        if (it == state_->entries.end())
            return ArtifactDisposition::NotFound;
        if (it->second.writer_open || it->second.busy || it->second.deletion_requested)
            return ArtifactDisposition::Failure;
        it->second.busy = true;
        identity = it->second.identity;
    }
    bool matched = false;
    {
        Handle file{OpenSource(destination, FILE_READ_ATTRIBUTES)};
        engine::FileIdentity actual;
        matched = file.value != INVALID_HANDLE_VALUE && ReadIdentity(file.value, actual) && actual == identity;
    }
    {
        std::lock_guard lock(state_->mutex);
        auto& entry = state_->entries.at(key);
        if (matched)
            entry.path.swap(updated_path);
        entry.busy = false;
    }
    state_->changed.notify_all();
    return matched ? ArtifactDisposition::Completed : ArtifactDisposition::Failure;
}

ArtifactDisposition RecordingArtifactRegistry::RequestArtifactDeletion(const RecordingArtifactKey& key) {
    std::filesystem::path path;
    engine::FileIdentity identity;
    {
        std::lock_guard lock(state_->mutex);
        const auto it = state_->entries.find(key);
        if (it == state_->entries.end())
            return ArtifactDisposition::NotFound;
        auto& entry = it->second;
        entry.deletion_requested = true;
        if (entry.leases != 0 || entry.writer_open || entry.busy)
            return ArtifactDisposition::DeferredByLease;
        entry.busy = true;
        path = entry.path;
        identity = entry.identity;
    }
    const bool deleted = DeleteExact(path, identity);
    {
        std::lock_guard lock(state_->mutex);
        auto it = state_->entries.find(key);
        if (it != state_->entries.end()) {
            if (deleted)
                state_->entries.erase(it);
            else
                it->second.busy = false;
        }
    }
    state_->changed.notify_all();
    return deleted ? ArtifactDisposition::Completed : ArtifactDisposition::Failure;
}

} // namespace exosnap

#include <exosnap/engine/media_cache_store.h>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <io.h>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exosnap::engine {
namespace {

constexpr std::array<char, 8> kMagic{'E', 'X', 'O', 'M', 'C', 'A', 'C', 'H'};
constexpr std::size_t kFixedHeaderBytes = 64;
constexpr std::size_t kHashChunkBytes = 64 * 1024;

class Sha256Stream {
  public:
    Sha256Stream() : sha_(av_sha_alloc(), av_free) {
        if (!sha_ || av_sha_init(sha_.get(), 256) < 0)
            throw std::bad_alloc();
    }
    void Update(const std::uint8_t* data, std::size_t size) {
        av_sha_update(sha_.get(), data, size);
    }
    std::array<std::uint8_t, 32> Final() {
        std::array<std::uint8_t, 32> digest{};
        av_sha_final(sha_.get(), digest.data());
        return digest;
    }

  private:
    std::unique_ptr<AVSHA, decltype(&av_free)> sha_;
};

void PutU32(std::uint8_t* at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        at[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void PutU64(std::uint8_t* at, std::uint64_t value) {
    for (int i = 0; i < 8; ++i)
        at[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint32_t GetU32(const std::uint8_t* at) {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i)
        value = (value << 8) | at[i];
    return value;
}
std::uint64_t GetU64(const std::uint8_t* at) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i)
        value = (value << 8) | at[i];
    return value;
}

struct FileCloser {
    void operator()(std::FILE* file) const noexcept {
        if (file)
            std::fclose(file);
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr OpenRead(const std::filesystem::path& path) {
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0)
        return {};
    return FilePtr(file);
}

bool SeekTo(std::FILE* file, std::uint64_t offset) {
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
}

std::string RandomToken() {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    const std::uint64_t value = generator();
    std::array<std::uint8_t, 8> bytes{};
    PutU64(bytes.data(), value);
    return ToHex(bytes);
}

} // namespace

std::string ToHex(std::span<const std::uint8_t> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        text.push_back(kDigits[byte >> 4]);
        text.push_back(kDigits[byte & 0x0f]);
    }
    return text;
}

std::array<std::uint8_t, 32> Sha256(std::span<const std::uint8_t> bytes) {
    Sha256Stream sha;
    sha.Update(bytes.data(), bytes.size());
    return sha.Final();
}

struct MediaCacheStore::Impl {
    struct Entry {
        std::uint64_t file_bytes = 0;
        std::filesystem::file_time_type last_use{};
        // Payload size once the whole entry has been hashed in this process.
        std::optional<std::uint64_t> verified_payload;
    };

    MediaCacheStoreConfig config;
    std::string suffix; // ".v<schema><extension>"
    mutable std::mutex mutex;
    bool indexed = false;
    std::unordered_map<std::string, Entry> entries;
    std::uint64_t total_bytes = 0;
    MediaCacheStoreStats stats;

    std::filesystem::path PathFor(std::string_view key) const {
        return config.root / std::string(key.substr(0, 2)) / (std::string(key) + suffix);
    }

    void Drop(const std::string& key) {
        const auto found = entries.find(key);
        if (found == entries.end())
            return;
        total_bytes -= found->second.file_bytes;
        entries.erase(found);
    }

    void DeleteEntry(const std::string& key) {
        std::error_code ignored;
        std::filesystem::remove(PathFor(key), ignored);
        Drop(key);
    }

    // Classifies one file below the root. Entries of this store's kind but
    // another schema version share the extension, differ in the version tag.
    void IndexFile(const std::filesystem::directory_entry& file) {
        std::error_code error;
        const auto name = file.path().filename().string();
        const auto now = std::filesystem::file_time_type::clock::now();
        if (name.ends_with(".tmp")) {
            const auto written = file.last_write_time(error);
            if (!error && now - written >= config.stale_temp_age) {
                if (std::filesystem::remove(file.path(), error))
                    ++stats.temp_cleaned;
            }
            return;
        }
        if (!name.ends_with(config.extension))
            return;
        const auto stem = name.substr(0, name.size() - config.extension.size());
        const auto version_at = stem.rfind(".v");
        if (version_at == std::string::npos)
            return;
        const auto key = stem.substr(0, version_at);
        if (!IsValidKey(key) || stem.substr(version_at) != suffix.substr(0, suffix.size() - config.extension.size()) ||
            file.path().parent_path().filename().string() != key.substr(0, 2)) {
            if (std::filesystem::remove(file.path(), error))
                ++stats.stale;
            return;
        }
        const auto size = file.file_size(error);
        if (error)
            return;
        const auto written = file.last_write_time(error);
        if (error)
            return;
        entries[key] = Entry{size, written, std::nullopt};
        total_bytes += size;
    }

    void EnsureIndexed() {
        if (indexed)
            return;
        indexed = true;
        std::error_code error;
        if (!std::filesystem::exists(config.root, error))
            return;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 config.root, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_regular_file(error))
                IndexFile(*it);
        }
        EvictToBudget({});
    }

    void EvictToBudget(const std::string& keep) {
        while (total_bytes > config.budget_bytes && !entries.empty()) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->first == keep)
                    continue;
                if (oldest == entries.end() || it->second.last_use < oldest->second.last_use)
                    oldest = it;
            }
            if (oldest == entries.end())
                return;
            const auto key = oldest->first;
            DeleteEntry(key);
            ++stats.evicted;
        }
    }

    void Touch(const std::string& key, Entry& entry) {
        entry.last_use = std::filesystem::file_time_type::clock::now();
        std::error_code ignored;
        std::filesystem::last_write_time(PathFor(key), entry.last_use, ignored);
    }

    // Streams the whole entry through SHA-256 and checks every header field.
    // Returns the payload size when the entry is intact.
    std::optional<std::uint64_t> Verify(const std::string& key, std::vector<std::uint8_t>* payload_out) {
        auto file = OpenRead(PathFor(key));
        if (!file)
            return std::nullopt;
        std::array<std::uint8_t, kFixedHeaderBytes> header{};
        if (std::fread(header.data(), 1, header.size(), file.get()) != header.size())
            return std::nullopt;
        if (std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0 ||
            std::memcmp(header.data() + 8, config.kind.data(), 4) != 0 ||
            GetU32(header.data() + 12) != config.schema_version || GetU32(header.data() + 16) != key.size())
            return std::nullopt;
        const std::uint64_t payload_size = GetU64(header.data() + 24);
        std::string echoed(key.size(), '\0');
        if (std::fread(echoed.data(), 1, echoed.size(), file.get()) != echoed.size() || echoed != key)
            return std::nullopt;
        const std::uint64_t payload_offset = kFixedHeaderBytes + key.size();
        if (_fseeki64(file.get(), 0, SEEK_END) != 0)
            return std::nullopt;
        const auto end = _ftelli64(file.get());
        if (end < 0 || static_cast<std::uint64_t>(end) != payload_offset + payload_size ||
            !SeekTo(file.get(), payload_offset))
            return std::nullopt;
        Sha256Stream sha;
        if (payload_out)
            payload_out->resize(static_cast<std::size_t>(payload_size));
        std::vector<std::uint8_t> chunk(payload_out ? 0 : kHashChunkBytes);
        std::uint64_t remaining = payload_size;
        std::uint64_t at = 0;
        while (remaining > 0) {
            const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kHashChunkBytes));
            std::uint8_t* destination = payload_out ? payload_out->data() + at : chunk.data();
            if (std::fread(destination, 1, take, file.get()) != take)
                return std::nullopt;
            sha.Update(destination, take);
            remaining -= take;
            at += take;
        }
        const auto digest = sha.Final();
        if (std::memcmp(digest.data(), header.data() + 32, digest.size()) != 0)
            return std::nullopt;
        return payload_size;
    }
};

MediaCacheStore::MediaCacheStore(MediaCacheStoreConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->suffix = ".v" + std::to_string(impl_->config.schema_version) + impl_->config.extension;
}

MediaCacheStore::~MediaCacheStore() = default;

bool MediaCacheStore::IsValidKey(std::string_view key) noexcept {
    if (key.size() < 16 || key.size() > 128)
        return false;
    return std::all_of(key.begin(), key.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool MediaCacheStore::Publish(std::string_view key_view, std::span<const std::uint8_t> payload, std::string& error) {
    if (!IsValidKey(key_view)) {
        error = "Invalid media cache key.";
        return false;
    }
    const std::string key(key_view);
    const std::uint64_t file_bytes = kFixedHeaderBytes + key.size() + payload.size();
    std::lock_guard lock(impl_->mutex);
    impl_->EnsureIndexed();
    if (file_bytes > impl_->config.budget_bytes) {
        error = "The media cache entry exceeds the cache budget.";
        return false;
    }
    const auto final_path = impl_->PathFor(key);
    std::error_code fs_error;
    std::filesystem::create_directories(final_path.parent_path(), fs_error);
    if (fs_error) {
        error = "Cannot create the media cache directory.";
        return false;
    }
    std::array<std::uint8_t, kFixedHeaderBytes> header{};
    std::memcpy(header.data(), kMagic.data(), kMagic.size());
    std::memcpy(header.data() + 8, impl_->config.kind.data(), 4);
    PutU32(header.data() + 12, impl_->config.schema_version);
    PutU32(header.data() + 16, static_cast<std::uint32_t>(key.size()));
    PutU64(header.data() + 24, payload.size());
    const auto digest = Sha256(payload);
    std::memcpy(header.data() + 32, digest.data(), digest.size());

    const auto temp_path = final_path.parent_path() / (key + "." + RandomToken() + ".tmp");
    {
        std::FILE* raw = nullptr;
        if (_wfopen_s(&raw, temp_path.c_str(), L"wbx") != 0 || !raw) {
            error = "Cannot create a media cache staging file.";
            return false;
        }
        FilePtr file(raw);
        const bool written =
            std::fwrite(header.data(), 1, header.size(), file.get()) == header.size() &&
            std::fwrite(key.data(), 1, key.size(), file.get()) == key.size() &&
            (payload.empty() || std::fwrite(payload.data(), 1, payload.size(), file.get()) == payload.size()) &&
            std::fflush(file.get()) == 0 && _commit(_fileno(file.get())) == 0;
        file.reset();
        if (!written) {
            std::filesystem::remove(temp_path, fs_error);
            error = "Cannot write a media cache staging file.";
            return false;
        }
    }
    if (impl_->config.before_rename && !impl_->config.before_rename(temp_path)) {
        error = "Media cache publication was interrupted.";
        return false;
    }
    if (!MoveFileExW(temp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temp_path, fs_error);
        error = "Cannot publish a media cache entry.";
        return false;
    }
    impl_->Drop(key);
    Impl::Entry entry{file_bytes, std::filesystem::file_time_type::clock::now(), payload.size()};
    std::filesystem::last_write_time(final_path, entry.last_use, fs_error);
    impl_->entries[key] = entry;
    impl_->total_bytes += file_bytes;
    ++impl_->stats.published;
    impl_->EvictToBudget(key);
    return true;
}

std::optional<std::vector<std::uint8_t>> MediaCacheStore::Read(std::string_view key_view) {
    if (!IsValidKey(key_view))
        return std::nullopt;
    const std::string key(key_view);
    std::lock_guard lock(impl_->mutex);
    impl_->EnsureIndexed();
    const auto found = impl_->entries.find(key);
    if (found == impl_->entries.end()) {
        ++impl_->stats.misses;
        return std::nullopt;
    }
    std::vector<std::uint8_t> payload;
    const auto size = impl_->Verify(key, &payload);
    if (!size) {
        impl_->DeleteEntry(key);
        ++impl_->stats.rejected;
        ++impl_->stats.misses;
        return std::nullopt;
    }
    auto& entry = impl_->entries.at(key);
    entry.verified_payload = *size;
    impl_->Touch(key, entry);
    ++impl_->stats.hits;
    return payload;
}

std::optional<std::uint64_t> MediaCacheStore::OpenVerified(std::string_view key_view) {
    if (!IsValidKey(key_view))
        return std::nullopt;
    const std::string key(key_view);
    std::lock_guard lock(impl_->mutex);
    impl_->EnsureIndexed();
    const auto found = impl_->entries.find(key);
    if (found == impl_->entries.end()) {
        ++impl_->stats.misses;
        return std::nullopt;
    }
    if (!found->second.verified_payload) {
        const auto size = impl_->Verify(key, nullptr);
        if (!size) {
            impl_->DeleteEntry(key);
            ++impl_->stats.rejected;
            ++impl_->stats.misses;
            return std::nullopt;
        }
        found->second.verified_payload = *size;
    }
    impl_->Touch(key, found->second);
    ++impl_->stats.hits;
    return found->second.verified_payload;
}

bool MediaCacheStore::ReadPayloadRange(std::string_view key_view, std::uint64_t offset, std::span<std::uint8_t> out) {
    if (!IsValidKey(key_view))
        return false;
    const std::string key(key_view);
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->entries.find(key);
    if (found == impl_->entries.end() || !found->second.verified_payload)
        return false;
    const auto payload_size = *found->second.verified_payload;
    if (offset > payload_size || out.size() > payload_size - offset)
        return false;
    auto file = OpenRead(impl_->PathFor(key));
    const std::uint64_t base = kFixedHeaderBytes + key.size();
    bool ok = file != nullptr;
    if (ok) {
        ok = _fseeki64(file.get(), 0, SEEK_END) == 0 &&
             _ftelli64(file.get()) == static_cast<long long>(found->second.file_bytes) &&
             SeekTo(file.get(), base + offset) &&
             (out.empty() || std::fread(out.data(), 1, out.size(), file.get()) == out.size());
    }
    if (!ok) {
        file.reset();
        impl_->DeleteEntry(key);
        ++impl_->stats.rejected;
    }
    return ok;
}

bool MediaCacheStore::Contains(std::string_view key_view) {
    std::lock_guard lock(impl_->mutex);
    impl_->EnsureIndexed();
    return impl_->entries.contains(std::string(key_view));
}

void MediaCacheStore::Remove(std::string_view key_view) {
    if (!IsValidKey(key_view))
        return;
    std::lock_guard lock(impl_->mutex);
    impl_->EnsureIndexed();
    impl_->DeleteEntry(std::string(key_view));
}

std::uint64_t MediaCacheStore::Clear() {
    std::lock_guard lock(impl_->mutex);
    impl_->EnsureIndexed();
    const auto released = impl_->total_bytes;
    std::vector<std::string> keys;
    keys.reserve(impl_->entries.size());
    for (const auto& [key, entry] : impl_->entries)
        keys.push_back(key);
    for (const auto& key : keys)
        impl_->DeleteEntry(key);
    std::error_code error;
    if (std::filesystem::exists(impl_->config.root, error)) {
        std::vector<std::filesystem::path> temps;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 impl_->config.root, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_regular_file(error) && it->path().filename().string().ends_with(".tmp"))
                temps.push_back(it->path());
        }
        for (const auto& temp : temps)
            std::filesystem::remove(temp, error);
    }
    return released;
}

std::uint64_t MediaCacheStore::TotalBytes() {
    std::lock_guard lock(impl_->mutex);
    impl_->EnsureIndexed();
    return impl_->total_bytes;
}

std::size_t MediaCacheStore::EntryCount() {
    std::lock_guard lock(impl_->mutex);
    impl_->EnsureIndexed();
    return impl_->entries.size();
}

std::uint64_t MediaCacheStore::BudgetBytes() const noexcept {
    return impl_->config.budget_bytes;
}

MediaCacheStoreStats MediaCacheStore::Stats() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->stats;
}

std::filesystem::path MediaCacheStore::EntryPath(std::string_view key) const {
    return impl_->PathFor(key);
}

} // namespace exosnap::engine

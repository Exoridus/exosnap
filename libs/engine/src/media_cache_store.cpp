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
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <io.h>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

struct MediaCacheStore::Writer::State {
    std::shared_ptr<MediaCacheStore::Impl> store;
    std::string key;
    std::filesystem::path path;
    FilePtr file;
    std::uint64_t payload_size = 0;
    std::uint64_t file_bytes = 0;
    bool active = true;
};

struct MediaCacheStore::Impl {
    struct Namespace {
        std::filesystem::path root;
        std::mutex mutex;
        std::atomic<std::shared_ptr<const std::string>> selected;
        std::string initial_selected;
        std::string persisted;
        std::function<bool(const std::filesystem::path&)> before_rename;
        bool initialized = false;

        void InitializeLocked() {
            if (initialized)
                return;
            auto file = OpenRead(root / "active-epoch");
            std::array<char, 64> name{};
            const auto size = file ? std::fread(name.data(), 1, name.size(), file.get()) : 0;
            const std::string existing(name.data(), size);
            if (existing.starts_with("epoch-") && existing.size() == 22 &&
                MediaCacheStore::IsValidKey(std::string_view(existing).substr(6)))
                initial_selected = existing;
            auto empty = std::shared_ptr<const std::string>{};
            selected.compare_exchange_strong(empty, std::make_shared<const std::string>(initial_selected));
            persisted = initial_selected;
            initialized = true;
        }

        void Persist() {
            std::lock_guard lock(mutex);
            InitializeLocked();
            const auto desired = selected.load(std::memory_order_acquire);
            if (*desired == persisted)
                return;
            std::error_code error;
            std::filesystem::create_directories(root, error);
            if (error)
                return;
            const auto pointer = root / "active-epoch";
            const auto temporary = root / ("active-epoch." + RandomToken() + ".tmp");
            std::FILE* raw = nullptr;
            if (_wfopen_s(&raw, temporary.c_str(), L"wb") != 0 || !raw)
                return;
            FilePtr file(raw);
            const bool written = std::fwrite(desired->data(), 1, desired->size(), file.get()) == desired->size() &&
                                 std::fflush(file.get()) == 0 && _commit(_fileno(file.get())) == 0;
            file.reset();
            const bool permitted = !before_rename || before_rename(temporary);
            if (written && permitted &&
                MoveFileExW(temporary.c_str(), pointer.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                persisted = *desired;
            else
                std::filesystem::remove(temporary, error);
        }
    };
    struct Entry {
        std::uint64_t file_bytes = 0;
        std::filesystem::file_time_type last_use{};
        // Payload size once the whole entry has been hashed in this process.
        std::optional<std::uint64_t> verified_payload;
    };

    MediaCacheStoreConfig config;
    std::shared_ptr<Namespace> namespace_state;
    bool resolve_namespace = true;
    std::atomic<bool> retired{false};
    std::atomic<std::uint64_t> known_bytes{0};
    std::uint64_t epoch = 0;
    std::string suffix; // ".v<schema><extension>"
    mutable std::mutex mutex;
    bool indexed = false;
    std::uint64_t reserved_bytes = 0;
    std::vector<std::weak_ptr<Writer::State>> writers;
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
        known_bytes.store(total_bytes, std::memory_order_relaxed);
        entries.erase(found);
    }

    bool DeleteEntry(const std::string& key) {
        std::error_code error;
        std::filesystem::remove(PathFor(key), error);
        if (error)
            return false;
        Drop(key);
        return true;
    }

    // Classifies one file below the root. Entries of this store's kind but
    // another schema version share the extension, differ in the version tag.
    void IndexFile(const std::filesystem::directory_entry& file) {
        std::error_code error;
        const auto name = file.path().filename().string();
        if (name.starts_with("active-epoch"))
            return;
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
        if (resolve_namespace) {
            std::lock_guard namespace_lock(namespace_state->mutex);
            namespace_state->InitializeLocked();
            config.root = namespace_state->initial_selected.empty()
                              ? namespace_state->root
                              : namespace_state->root / namespace_state->initial_selected;
            resolve_namespace = false;
        }
        namespace_state->Persist();
        indexed = true;
        std::error_code error;
        if (!std::filesystem::exists(config.root, error))
            return;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 config.root, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_directory(error) && it->path().filename().string().starts_with("epoch-")) {
                it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(error))
                IndexFile(*it);
        }
        EvictToBudget({});
        known_bytes.store(total_bytes, std::memory_order_relaxed);
    }

    std::uint64_t ClearLocked() {
        EnsureIndexed();
        for (const auto& weak : writers) {
            if (const auto writer = weak.lock(); writer && writer->active) {
                writer->active = false;
                writer->file.reset();
                reserved_bytes -= writer->file_bytes;
                std::error_code ignored;
                std::filesystem::remove(writer->path, ignored);
            }
        }
        writers.clear();
        const auto released = total_bytes;
        std::vector<std::string> keys;
        for (const auto& [key, entry] : entries)
            keys.push_back(key);
        for (const auto& key : keys)
            DeleteEntry(key);
        std::error_code error;
        if (config.root != namespace_state->root && entries.empty()) {
            std::filesystem::remove_all(config.root, error);
        } else if (std::filesystem::exists(config.root, error)) {
            for (auto it = std::filesystem::recursive_directory_iterator(
                     config.root, std::filesystem::directory_options::skip_permission_denied, error);
                 !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
                if (it->is_directory(error) && it->path().filename().string().starts_with("epoch-")) {
                    it.disable_recursion_pending();
                } else if (it->is_regular_file(error) && !it->path().filename().string().starts_with("active-epoch") &&
                           it->path().filename().string().ends_with(".tmp")) {
                    std::filesystem::remove(it->path(), error);
                }
            }
        }
        return released - total_bytes;
    }

    // Makes room for `incoming` bytes that will replace the entry `replaced`,
    // which is therefore never evicted. Publication evicts before its rename
    // because a published file cannot be reliably rolled back: another
    // process may open it the moment it becomes visible.
    bool EvictToBudget(const std::string& replaced, std::uint64_t incoming = 0) {
        std::set<std::string> blocked;
        const auto found = entries.find(replaced);
        const std::uint64_t replaced_bytes = found == entries.end() ? 0 : found->second.file_bytes;
        while (total_bytes - replaced_bytes + incoming > config.budget_bytes && !entries.empty()) {
            auto oldest = entries.end();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->first == replaced || blocked.contains(it->first))
                    continue;
                if (oldest == entries.end() || it->second.last_use < oldest->second.last_use)
                    oldest = it;
            }
            if (oldest == entries.end())
                return false;
            const auto key = oldest->first;
            if (DeleteEntry(key))
                ++stats.evicted;
            else
                blocked.insert(key);
        }
        return true;
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

struct MediaCacheStore::Maintenance {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<Impl>> retired;
    bool stopping = false;
    bool working = false;
    std::thread worker;

    Maintenance() : worker([this] { Run(); }) {
    }
    ~Maintenance() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        worker.join();
    }
    void Run() {
        for (;;) {
            std::shared_ptr<Impl> old;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] { return stopping || !retired.empty(); });
                if (retired.empty())
                    return;
                old = std::move(retired.front());
                retired.pop_front();
                working = true;
            }
            {
                std::lock_guard lock(old->mutex);
                old->namespace_state->Persist();
                old->ClearLocked();
            }
            {
                std::lock_guard lock(mutex);
                working = false;
            }
            changed.notify_all();
        }
    }
};

MediaCacheStore::MediaCacheStore(MediaCacheStoreConfig config)
    : config_(std::move(config)), impl_(std::make_shared<Impl>()) {
    impl_->config = config_;
    impl_->suffix = ".v" + std::to_string(impl_->config.schema_version) + impl_->config.extension;
    impl_->namespace_state = std::make_shared<Impl::Namespace>();
    impl_->namespace_state->root = impl_->config.root;
    impl_->namespace_state->before_rename = impl_->config.before_rename;
    maintenance_ = std::make_unique<Maintenance>();
}

MediaCacheStore::~MediaCacheStore() = default;

std::shared_ptr<MediaCacheStore::Impl> MediaCacheStore::current() const {
    std::lock_guard lock(current_mutex_);
    return impl_;
}

std::uint64_t MediaCacheStore::Retire() {
    auto replacement = std::make_shared<Impl>();
    std::shared_ptr<Impl> old;
    {
        std::lock_guard lock(current_mutex_);
        old = impl_;
        replacement->config = config_;
        replacement->suffix = old->suffix;
        replacement->namespace_state = old->namespace_state;
        replacement->epoch = old->epoch + 1;
        const auto selected = "epoch-" + RandomToken();
        replacement->config.root = replacement->namespace_state->root / selected;
        replacement->resolve_namespace = false;
        replacement->namespace_state->selected.store(std::make_shared<const std::string>(selected),
                                                     std::memory_order_release);
        old->retired.store(true, std::memory_order_release);
        impl_ = replacement;
        std::lock_guard maintenance_lock(maintenance_->mutex);
        maintenance_->retired.push_back(old);
    }
    maintenance_->changed.notify_all();
    return old->known_bytes.load(std::memory_order_relaxed);
}

void MediaCacheStore::WaitForRetired() {
    std::unique_lock lock(maintenance_->mutex);
    maintenance_->changed.wait(lock, [this] { return maintenance_->retired.empty() && !maintenance_->working; });
}

std::uint64_t MediaCacheStore::Epoch() const {
    return current()->epoch;
}

bool MediaCacheStore::IsValidKey(std::string_view key) noexcept {
    if (key.size() < 16 || key.size() > 128)
        return false;
    return std::all_of(key.begin(), key.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool MediaCacheStore::Publish(std::string_view key_view, std::span<const std::uint8_t> payload, std::string& error,
                              std::optional<std::uint64_t> expected_epoch) {
    if (!IsValidKey(key_view)) {
        error = "Invalid media cache key.";
        return false;
    }
    const std::string key(key_view);
    const std::uint64_t file_bytes = kFixedHeaderBytes + key.size() + payload.size();
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    if (impl->retired.load(std::memory_order_acquire) || (expected_epoch && *expected_epoch != impl->epoch)) {
        error = "The media cache generation was retired.";
        return false;
    }
    impl->EnsureIndexed();
    if (file_bytes > impl->config.budget_bytes) {
        error = "The media cache entry exceeds the cache budget.";
        return false;
    }
    const auto final_path = impl->PathFor(key);
    std::error_code fs_error;
    std::filesystem::create_directories(final_path.parent_path(), fs_error);
    if (fs_error) {
        error = "Cannot create the media cache directory.";
        return false;
    }
    std::array<std::uint8_t, kFixedHeaderBytes> header{};
    std::memcpy(header.data(), kMagic.data(), kMagic.size());
    std::memcpy(header.data() + 8, impl->config.kind.data(), 4);
    PutU32(header.data() + 12, impl->config.schema_version);
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
    if (impl->config.before_rename && !impl->config.before_rename(temp_path)) {
        error = "Media cache publication was interrupted.";
        return false;
    }
    if (impl->retired.load(std::memory_order_acquire)) {
        std::filesystem::remove(temp_path, fs_error);
        error = "The media cache generation was retired.";
        return false;
    }
    if (!impl->EvictToBudget(key, file_bytes)) {
        std::filesystem::remove(temp_path, fs_error);
        error = "Locked media cache entries prevent publication within the disk budget.";
        return false;
    }
    if (!MoveFileExW(temp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temp_path, fs_error);
        error = "Cannot publish a media cache entry.";
        return false;
    }
    impl->Drop(key);
    Impl::Entry entry{file_bytes, std::filesystem::file_time_type::clock::now(), payload.size()};
    std::filesystem::last_write_time(final_path, entry.last_use, fs_error);
    impl->entries[key] = entry;
    impl->total_bytes += file_bytes;
    impl->known_bytes.store(impl->total_bytes, std::memory_order_relaxed);
    ++impl->stats.published;
    return true;
}

MediaCacheStore::Writer::Writer(std::shared_ptr<State> state) : state_(std::move(state)) {
}

MediaCacheStore::Writer::~Writer() {
    std::lock_guard lock(state_->store->mutex);
    if (!state_->active)
        return;
    state_->active = false;
    state_->file.reset();
    state_->store->reserved_bytes -= state_->file_bytes;
    std::error_code ignored;
    std::filesystem::remove(state_->path, ignored);
}

bool MediaCacheStore::Writer::Write(std::uint64_t offset, std::span<const std::uint8_t> bytes) {
    std::lock_guard lock(state_->store->mutex);
    if (state_->store->retired.load(std::memory_order_acquire) || !state_->active || offset > state_->payload_size ||
        bytes.size() > state_->payload_size - offset)
        return false;
    return SeekTo(state_->file.get(), kFixedHeaderBytes + state_->key.size() + offset) &&
           (bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), state_->file.get()) == bytes.size());
}

bool MediaCacheStore::Writer::Read(std::uint64_t offset, std::span<std::uint8_t> bytes) {
    std::lock_guard lock(state_->store->mutex);
    if (state_->store->retired.load(std::memory_order_acquire) || !state_->active || offset > state_->payload_size ||
        bytes.size() > state_->payload_size - offset)
        return false;
    return SeekTo(state_->file.get(), kFixedHeaderBytes + state_->key.size() + offset) &&
           (bytes.empty() || std::fread(bytes.data(), 1, bytes.size(), state_->file.get()) == bytes.size());
}

bool MediaCacheStore::Writer::Publish(std::string& error, const std::function<bool()>& continue_io) {
    auto& store = *state_->store;
    std::lock_guard lock(store.mutex);
    if (store.retired.load(std::memory_order_acquire) || !state_->active) {
        error = "The media cache writer was invalidated.";
        return false;
    }
    std::array<std::uint8_t, kFixedHeaderBytes> header{};
    std::memcpy(header.data(), kMagic.data(), kMagic.size());
    std::memcpy(header.data() + 8, store.config.kind.data(), 4);
    PutU32(header.data() + 12, store.config.schema_version);
    PutU32(header.data() + 16, static_cast<std::uint32_t>(state_->key.size()));
    PutU64(header.data() + 24, state_->payload_size);
    Sha256Stream sha;
    std::array<std::uint8_t, kHashChunkBytes> chunk{};
    bool ok = SeekTo(state_->file.get(), kFixedHeaderBytes + state_->key.size());
    for (std::uint64_t at = 0; ok && at < state_->payload_size;) {
        if (store.retired.load(std::memory_order_acquire) || (continue_io && !continue_io())) {
            error = "Media cache publication was cancelled.";
            return false;
        }
        const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), state_->payload_size - at));
        ok = std::fread(chunk.data(), 1, take, state_->file.get()) == take;
        if (ok)
            sha.Update(chunk.data(), take);
        at += take;
    }
    const auto digest = sha.Final();
    if (continue_io && !continue_io()) {
        error = "Media cache publication was cancelled.";
        return false;
    }
    std::memcpy(header.data() + 32, digest.data(), digest.size());
    ok = ok && SeekTo(state_->file.get(), 0) &&
         std::fwrite(header.data(), 1, header.size(), state_->file.get()) == header.size() &&
         std::fwrite(state_->key.data(), 1, state_->key.size(), state_->file.get()) == state_->key.size() &&
         std::fflush(state_->file.get()) == 0 && _commit(_fileno(state_->file.get())) == 0;
    if (!ok) {
        error = "Cannot finalize a media cache staging file.";
        return false;
    }
    if (continue_io && !continue_io()) {
        error = "Media cache publication was cancelled.";
        return false;
    }
    state_->file.reset();
    state_->active = false;
    store.reserved_bytes -= state_->file_bytes;
    if (store.config.before_rename && !store.config.before_rename(state_->path)) {
        error = "Media cache publication was interrupted.";
        return false;
    }
    if (store.retired.load(std::memory_order_acquire)) {
        std::error_code ignored;
        std::filesystem::remove(state_->path, ignored);
        error = "The media cache generation was retired.";
        return false;
    }
    std::error_code ignored;
    if (!store.EvictToBudget(state_->key, state_->file_bytes)) {
        std::filesystem::remove(state_->path, ignored);
        error = "Locked media cache entries prevent publication within the disk budget.";
        return false;
    }
    const auto final_path = store.PathFor(state_->key);
    if (!MoveFileExW(state_->path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(state_->path, ignored);
        error = "Cannot publish a media cache entry.";
        return false;
    }
    store.Drop(state_->key);
    Impl::Entry entry{state_->file_bytes, std::filesystem::file_time_type::clock::now(), state_->payload_size};
    std::filesystem::last_write_time(final_path, entry.last_use, ignored);
    store.entries[state_->key] = entry;
    store.total_bytes += state_->file_bytes;
    store.known_bytes.store(store.total_bytes, std::memory_order_relaxed);
    ++store.stats.published;
    return true;
}

std::unique_ptr<MediaCacheStore::Writer> MediaCacheStore::BeginWrite(std::string_view key_view,
                                                                     std::uint64_t payload_size, std::string& error,
                                                                     MediaCacheWriteFailure* failure) {
    const auto reject = [&](MediaCacheWriteFailure value) {
        if (failure)
            *failure = value;
    };
    reject(MediaCacheWriteFailure::None);
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    if (impl->retired.load(std::memory_order_acquire)) {
        error = "The media cache generation was retired.";
        reject(MediaCacheWriteFailure::Retired);
        return {};
    }
    impl->EnsureIndexed();
    const auto overhead = kFixedHeaderBytes + key_view.size();
    if (!IsValidKey(key_view) || overhead > impl->config.budget_bytes ||
        payload_size > impl->config.budget_bytes - overhead) {
        error = "The media cache entry exceeds the cache budget.";
        reject(MediaCacheWriteFailure::InvalidEntry);
        return {};
    }
    if (payload_size + overhead > impl->config.budget_bytes - impl->reserved_bytes) {
        error = "The media cache staging budget is temporarily reserved.";
        reject(MediaCacheWriteFailure::Busy);
        return {};
    }
    auto state = std::make_shared<Writer::State>();
    state->store = impl;
    state->key = key_view;
    state->payload_size = payload_size;
    state->file_bytes = overhead + payload_size;
    const auto final_path = impl->PathFor(key_view);
    state->path = final_path.parent_path() / (state->key + "." + RandomToken() + ".tmp");
    std::error_code fs_error;
    std::filesystem::create_directories(final_path.parent_path(), fs_error);
    std::FILE* raw = nullptr;
    if (fs_error || _wfopen_s(&raw, state->path.c_str(), L"w+bx") != 0 || !raw) {
        error = "Cannot create a media cache staging file.";
        reject(MediaCacheWriteFailure::Io);
        return {};
    }
    state->file.reset(raw);
    if (_chsize_s(_fileno(raw), state->file_bytes) != 0) {
        state->file.reset();
        std::filesystem::remove(state->path, fs_error);
        error = "Cannot size a media cache staging file.";
        reject(MediaCacheWriteFailure::Io);
        return {};
    }
    impl->reserved_bytes += state->file_bytes;
    std::erase_if(impl->writers, [](const auto& writer) { return writer.expired(); });
    impl->writers.push_back(state);
    return std::unique_ptr<Writer>(new Writer(std::move(state)));
}
std::optional<std::vector<std::uint8_t>> MediaCacheStore::Read(std::string_view key_view) {
    if (!IsValidKey(key_view))
        return std::nullopt;
    const std::string key(key_view);
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    impl->EnsureIndexed();
    const auto found = impl->entries.find(key);
    if (found == impl->entries.end()) {
        ++impl->stats.misses;
        return std::nullopt;
    }
    std::vector<std::uint8_t> payload;
    const auto size = impl->Verify(key, &payload);
    if (!size) {
        impl->DeleteEntry(key);
        ++impl->stats.rejected;
        ++impl->stats.misses;
        return std::nullopt;
    }
    auto& entry = impl->entries.at(key);
    entry.verified_payload = *size;
    impl->Touch(key, entry);
    ++impl->stats.hits;
    return payload;
}

std::optional<std::uint64_t> MediaCacheStore::OpenVerified(std::string_view key_view) {
    if (!IsValidKey(key_view))
        return std::nullopt;
    const std::string key(key_view);
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    impl->EnsureIndexed();
    const auto found = impl->entries.find(key);
    if (found == impl->entries.end()) {
        ++impl->stats.misses;
        return std::nullopt;
    }
    if (!found->second.verified_payload) {
        const auto size = impl->Verify(key, nullptr);
        if (!size) {
            impl->DeleteEntry(key);
            ++impl->stats.rejected;
            ++impl->stats.misses;
            return std::nullopt;
        }
        found->second.verified_payload = *size;
    }
    impl->Touch(key, found->second);
    ++impl->stats.hits;
    return found->second.verified_payload;
}

bool MediaCacheStore::ReadPayloadRange(std::string_view key_view, std::uint64_t offset, std::span<std::uint8_t> out) {
    if (!IsValidKey(key_view))
        return false;
    const std::string key(key_view);
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    const auto found = impl->entries.find(key);
    if (found == impl->entries.end() || !found->second.verified_payload)
        return false;
    const auto payload_size = *found->second.verified_payload;
    if (offset > payload_size || out.size() > payload_size - offset)
        return false;
    auto file = OpenRead(impl->PathFor(key));
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
        impl->DeleteEntry(key);
        ++impl->stats.rejected;
    }
    return ok;
}

bool MediaCacheStore::Contains(std::string_view key_view) {
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    impl->EnsureIndexed();
    return impl->entries.contains(std::string(key_view));
}

void MediaCacheStore::Remove(std::string_view key_view) {
    if (!IsValidKey(key_view))
        return;
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    impl->EnsureIndexed();
    impl->DeleteEntry(std::string(key_view));
}

std::uint64_t MediaCacheStore::Clear() {
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    return impl->ClearLocked();
}

std::uint64_t MediaCacheStore::TotalBytes() {
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    impl->EnsureIndexed();
    return impl->total_bytes;
}

std::size_t MediaCacheStore::EntryCount() {
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    impl->EnsureIndexed();
    return impl->entries.size();
}

std::uint64_t MediaCacheStore::BudgetBytes() const noexcept {
    return current()->config.budget_bytes;
}

MediaCacheStoreStats MediaCacheStore::Stats() const {
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    return impl->stats;
}

std::filesystem::path MediaCacheStore::EntryPath(std::string_view key) const {
    const auto impl = current();
    std::lock_guard lock(impl->mutex);
    impl->EnsureIndexed();
    return impl->PathFor(key);
}

} // namespace exosnap::engine

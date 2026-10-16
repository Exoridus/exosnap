#pragma once

// Persistent, budgeted store for derived media data (waveform peaks, timeline
// thumbnails). Entries are immutable blobs addressed by a caller-computed hex
// key that must already encode the validated content identity and every
// parameter the derivation depends on. The store never interprets the payload.
//
// Guarantees:
// - Publication is atomic: a payload is written to a unique temporary file in
//   the entry's own directory, flushed to disk and only then renamed onto its
//   final name. A reader never observes a partially written entry under the
//   final name.
// - Every entry carries magic, kind, schema version, an echo of its key, the
//   payload length and the payload's SHA-256. An entry failing any check is
//   deleted and reported as a miss, never returned.
// - Entries of another schema version are stale. They are deleted when the
//   directory is indexed and are never returned.
// - The total size of accepted entries stays within the byte budget. Eviction
//   is least recently used, where use is publication or a successful read.
//   The last use is persisted in the file's last-write time, so the order
//   survives restarts.
//
// Thread-safe. Indexing the directory is lazy and happens on the first call
// that needs it, so construction performs no I/O and may happen on any thread.

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exosnap::engine {

struct MediaCacheStoreConfig {
    std::filesystem::path root;
    std::uint64_t budget_bytes = 0;
    // Four ASCII characters identifying the payload kind, e.g. "WAVE".
    std::array<char, 4> kind{};
    std::uint32_t schema_version = 1;
    // Entry file extension including the dot, e.g. ".wave".
    std::string extension;
    // Temporary files older than this are leftovers of an interrupted
    // publication and are deleted while indexing. Younger ones may belong to
    // a concurrent writer and are left alone.
    std::chrono::seconds stale_temp_age{600};
    // Test seam. Called after the temporary file is complete and before it is
    // renamed. Returning false abandons publication as a crash would: the
    // temporary file stays on disk and nothing is published.
    std::function<bool(const std::filesystem::path& temp_file)> before_rename;
};

struct MediaCacheStoreStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t rejected = 0; // corrupt, truncated or mismatching entries deleted on read
    std::uint64_t stale = 0;    // other-schema entries deleted while indexing
    std::uint64_t evicted = 0;
    std::uint64_t published = 0;
    std::uint64_t temp_cleaned = 0;
};

class MediaCacheStore {
  public:
    class Writer {
      public:
        ~Writer();
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;
        bool Write(std::uint64_t offset, std::span<const std::uint8_t> bytes);
        bool Read(std::uint64_t offset, std::span<std::uint8_t> bytes);
        // The callback runs synchronously between hash blocks while the store is locked.
        // It must not reenter the store. False aborts before publication.
        bool Publish(std::string& error, const std::function<bool()>& continue_io = {});

      private:
        friend class MediaCacheStore;
        struct State;
        explicit Writer(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
    };

    explicit MediaCacheStore(MediaCacheStoreConfig config);
    ~MediaCacheStore();

    MediaCacheStore(const MediaCacheStore&) = delete;
    MediaCacheStore& operator=(const MediaCacheStore&) = delete;

    // Keys are 16 to 128 lowercase hexadecimal characters.
    [[nodiscard]] static bool IsValidKey(std::string_view key) noexcept;

    // Publishes `payload` under `key`, replacing an existing entry, then evicts
    // least recently used entries until the budget holds again. An entry larger
    // than the whole budget is refused.
    bool Publish(std::string_view key, std::span<const std::uint8_t> payload, std::string& error);

    // Creates a bounded-size, private random-access payload. Destruction aborts
    // unpublished work. Clear invalidates active writers and deletes staging.
    // Publish hashes the completed payload in bounded chunks before rename.
    [[nodiscard]] std::unique_ptr<Writer> BeginWrite(std::string_view key, std::uint64_t payload_size,
                                                     std::string& error);

    // The verified payload, or nullopt for a miss. Counts as a use.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> Read(std::string_view key);

    // Verifies the whole entry once per publication (streaming, constant
    // memory) and returns its payload size. Counts as a use. Subsequent
    // ReadPayloadRange calls read slices without re-hashing the file.
    [[nodiscard]] std::optional<std::uint64_t> OpenVerified(std::string_view key);

    // Reads `out.size()` payload bytes at `offset` of an entry that passed
    // OpenVerified. Fails (and drops the entry from the index) when the file
    // changed size or disappeared since.
    bool ReadPayloadRange(std::string_view key, std::uint64_t offset, std::span<std::uint8_t> out);

    [[nodiscard]] bool Contains(std::string_view key);
    void Remove(std::string_view key);

    // Deletes every entry and leftover temporary file of this store. Returns
    // the number of entry bytes released.
    std::uint64_t Clear();

    [[nodiscard]] std::uint64_t TotalBytes();
    [[nodiscard]] std::size_t EntryCount();
    [[nodiscard]] std::uint64_t BudgetBytes() const noexcept;
    [[nodiscard]] MediaCacheStoreStats Stats() const;
    [[nodiscard]] std::filesystem::path EntryPath(std::string_view key) const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Lowercase hexadecimal spelling of `bytes`.
[[nodiscard]] std::string ToHex(std::span<const std::uint8_t> bytes);

// SHA-256 of `bytes`.
[[nodiscard]] std::array<std::uint8_t, 32> Sha256(std::span<const std::uint8_t> bytes);

} // namespace exosnap::engine

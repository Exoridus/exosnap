#include <exosnap/engine/media_cache_store.h>

#include "test_unique_temp.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace exosnap::engine;

namespace {

struct TempDir {
    std::filesystem::path path = exosnap_test::UniqueTempPath("cache");
    TempDir() {
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

MediaCacheStoreConfig Config(const std::filesystem::path& root, std::uint64_t budget = 1 << 20,
                             std::uint32_t version = 1) {
    MediaCacheStoreConfig config;
    config.root = root;
    config.budget_bytes = budget;
    config.kind = {'T', 'E', 'S', 'T'};
    config.schema_version = version;
    config.extension = ".test";
    config.stale_temp_age = std::chrono::seconds(0);
    return config;
}

std::string Key(int index) {
    std::string key = "00112233445566778899aabbccddeeff";
    const std::string digits = std::to_string(index);
    key.replace(key.size() - digits.size(), digits.size(), digits);
    return key;
}

std::vector<std::uint8_t> Payload(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<std::uint8_t>(seed + i * 7);
    return bytes;
}

std::vector<std::filesystem::path> FilesUnder(const std::filesystem::path& root, const std::string& suffix) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.is_regular_file() && entry.path().filename().string().ends_with(suffix))
            files.push_back(entry.path());
    return files;
}

} // namespace

TEST(MediaCacheStore, PublishesAndReadsAcrossInstances) {
    TempDir dir;
    const auto payload = Payload(5000, 3);
    {
        MediaCacheStore store(Config(dir.path));
        std::string error;
        ASSERT_TRUE(store.Publish(Key(1), payload, error)) << error;
        EXPECT_TRUE(FilesUnder(dir.path, ".tmp").empty());
    }
    MediaCacheStore reopened(Config(dir.path));
    const auto read = reopened.Read(Key(1));
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, payload);
    EXPECT_EQ(reopened.Stats().hits, 1u);

    const auto size = reopened.OpenVerified(Key(1));
    ASSERT_TRUE(size.has_value());
    EXPECT_EQ(*size, payload.size());
    std::vector<std::uint8_t> slice(100);
    ASSERT_TRUE(reopened.ReadPayloadRange(Key(1), 1234, slice));
    EXPECT_TRUE(std::equal(slice.begin(), slice.end(), payload.begin() + 1234));
    EXPECT_FALSE(reopened.ReadPayloadRange(Key(1), payload.size() - 10, slice));
}

TEST(MediaCacheStore, RejectsInvalidKeys) {
    TempDir dir;
    MediaCacheStore store(Config(dir.path));
    std::string error;
    EXPECT_FALSE(store.Publish("short", Payload(4, 1), error));
    EXPECT_FALSE(store.Publish("../../../../escape/0123456789abcdef", Payload(4, 1), error));
    EXPECT_FALSE(store.Publish("0123456789ABCDEF0123", Payload(4, 1), error));
}

TEST(MediaCacheStore, CorruptPayloadIsRejectedAndDeleted) {
    TempDir dir;
    MediaCacheStore store(Config(dir.path));
    std::string error;
    ASSERT_TRUE(store.Publish(Key(2), Payload(4096, 9), error)) << error;
    const auto path = store.EntryPath(Key(2));
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(-100, std::ios::end);
        file.put('\x5a');
    }
    MediaCacheStore reopened(Config(dir.path));
    EXPECT_FALSE(reopened.Read(Key(2)).has_value());
    EXPECT_EQ(reopened.Stats().rejected, 1u);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(reopened.Contains(Key(2)));
}

TEST(MediaCacheStore, TruncatedEntryIsRejected) {
    TempDir dir;
    MediaCacheStore store(Config(dir.path));
    std::string error;
    ASSERT_TRUE(store.Publish(Key(3), Payload(4096, 4), error)) << error;
    const auto path = store.EntryPath(Key(3));
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
    MediaCacheStore reopened(Config(dir.path));
    EXPECT_FALSE(reopened.OpenVerified(Key(3)).has_value());
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(MediaCacheStore, InterruptedPublicationLeavesNoAcceptedEntryAndIsCleaned) {
    TempDir dir;
    auto config = Config(dir.path);
    config.before_rename = [](const std::filesystem::path&) { return false; };
    {
        MediaCacheStore store(config);
        std::string error;
        EXPECT_FALSE(store.Publish(Key(4), Payload(2048, 5), error));
        EXPECT_FALSE(store.Read(Key(4)).has_value());
    }
    ASSERT_EQ(FilesUnder(dir.path, ".tmp").size(), 1u);
    EXPECT_TRUE(FilesUnder(dir.path, ".test").empty());

    MediaCacheStore reopened(Config(dir.path));
    EXPECT_FALSE(reopened.Contains(Key(4)));
    EXPECT_EQ(reopened.Stats().temp_cleaned, 1u);
    EXPECT_TRUE(FilesUnder(dir.path, ".tmp").empty());
}

TEST(MediaCacheStore, YoungTemporaryFilesOfAConcurrentWriterAreKept) {
    TempDir dir;
    auto interrupted = Config(dir.path);
    interrupted.before_rename = [](const std::filesystem::path&) { return false; };
    {
        MediaCacheStore store(interrupted);
        std::string error;
        EXPECT_FALSE(store.Publish(Key(5), Payload(64, 1), error));
    }
    auto patient = Config(dir.path);
    patient.stale_temp_age = std::chrono::hours(1);
    MediaCacheStore reopened(patient);
    EXPECT_EQ(reopened.EntryCount(), 0u);
    EXPECT_EQ(FilesUnder(dir.path, ".tmp").size(), 1u);
}

TEST(MediaCacheStore, OtherSchemaVersionIsStale) {
    TempDir dir;
    {
        MediaCacheStore store(Config(dir.path, 1 << 20, 1));
        std::string error;
        ASSERT_TRUE(store.Publish(Key(6), Payload(100, 2), error)) << error;
    }
    MediaCacheStore bumped(Config(dir.path, 1 << 20, 2));
    EXPECT_FALSE(bumped.Read(Key(6)).has_value());
    EXPECT_EQ(bumped.Stats().stale, 1u);
    EXPECT_TRUE(FilesUnder(dir.path, ".test").empty());
}

TEST(MediaCacheStore, BudgetIsNeverExceededAndEvictionIsLeastRecentlyUsed) {
    TempDir dir;
    const std::size_t payload_bytes = 1000;
    const std::uint64_t entry_bytes = 64 + Key(0).size() + payload_bytes;
    const std::uint64_t budget = entry_bytes * 3;
    MediaCacheStore store(Config(dir.path, budget));
    std::string error;
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(store.Publish(Key(10 + i), Payload(payload_bytes, static_cast<std::uint8_t>(i)), error));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Key 10 is the oldest publication; reading it makes key 11 the oldest use.
    ASSERT_TRUE(store.Read(Key(10)).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (int i = 3; i < 8; ++i) {
        ASSERT_TRUE(store.Publish(Key(10 + i), Payload(payload_bytes, static_cast<std::uint8_t>(i)), error));
        EXPECT_LE(store.TotalBytes(), budget);
        if (i == 3) {
            EXPECT_TRUE(store.Contains(Key(10)));
            EXPECT_FALSE(store.Contains(Key(11)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(store.EntryCount(), 3u);
    EXPECT_GE(store.Stats().evicted, 5u);
    std::uint64_t on_disk = 0;
    for (const auto& file : FilesUnder(dir.path, ".test"))
        on_disk += std::filesystem::file_size(file);
    EXPECT_LE(on_disk, budget);

    EXPECT_FALSE(store.Publish(Key(99), Payload(static_cast<std::size_t>(budget), 1), error));
}

TEST(MediaCacheStore, EvictionOrderSurvivesRestart) {
    TempDir dir;
    const std::size_t payload_bytes = 500;
    const std::uint64_t entry_bytes = 64 + Key(0).size() + payload_bytes;
    std::string error;
    {
        MediaCacheStore store(Config(dir.path, entry_bytes * 2));
        ASSERT_TRUE(store.Publish(Key(20), Payload(payload_bytes, 1), error));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ASSERT_TRUE(store.Publish(Key(21), Payload(payload_bytes, 2), error));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ASSERT_TRUE(store.Read(Key(20)).has_value());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    MediaCacheStore reopened(Config(dir.path, entry_bytes * 2));
    ASSERT_TRUE(reopened.Publish(Key(22), Payload(payload_bytes, 3), error));
    EXPECT_TRUE(reopened.Contains(Key(20)));
    EXPECT_FALSE(reopened.Contains(Key(21)));
}

TEST(MediaCacheStore, ClearRemovesEverything) {
    TempDir dir;
    MediaCacheStore store(Config(dir.path));
    std::string error;
    ASSERT_TRUE(store.Publish(Key(30), Payload(300, 1), error));
    ASSERT_TRUE(store.Publish(Key(31), Payload(300, 2), error));
    EXPECT_GT(store.Clear(), 0u);
    EXPECT_EQ(store.EntryCount(), 0u);
    EXPECT_EQ(store.TotalBytes(), 0u);
    EXPECT_TRUE(FilesUnder(dir.path, ".test").empty());
}

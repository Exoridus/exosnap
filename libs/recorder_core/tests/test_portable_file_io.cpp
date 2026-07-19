#include "recorder_core/portable_file_io.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

using namespace recorder_core;

namespace {

class TempFile {
  public:
    TempFile() {
        path_ = std::filesystem::temp_directory_path() / "exosnap_portable_file_io_test.bin";
        std::error_code ec;
        std::filesystem::remove(path_, ec); // clean up a leftover from a prior crashed run
        fopen_s(&file_, path_.string().c_str(), "wb+");
    }
    ~TempFile() {
        if (file_ != nullptr)
            std::fclose(file_);
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] FILE* get() const noexcept {
        return file_;
    }

  private:
    std::filesystem::path path_;
    FILE* file_ = nullptr;
};

} // namespace

TEST(PortableFileIo, SmallPositionsRoundTrip) {
    TempFile f;
    ASSERT_NE(f.get(), nullptr);
    ASSERT_EQ(Fseek64(f.get(), 0, SEEK_SET), 0);
    const char buf[4] = {'a', 'b', 'c', 'd'};
    ASSERT_EQ(std::fwrite(buf, 1, sizeof(buf), f.get()), sizeof(buf));
    EXPECT_EQ(Ftell64(f.get()), 4);
}

TEST(PortableFileIo, SeekCurAndEndWork) {
    TempFile f;
    ASSERT_NE(f.get(), nullptr);
    const char buf[10] = {};
    ASSERT_EQ(std::fwrite(buf, 1, sizeof(buf), f.get()), sizeof(buf));
    ASSERT_EQ(Fseek64(f.get(), -4, SEEK_CUR), 0);
    EXPECT_EQ(Ftell64(f.get()), 6);
    ASSERT_EQ(Fseek64(f.get(), 0, SEEK_END), 0);
    EXPECT_EQ(Ftell64(f.get()), 10);
}

// Reproduces the bug this fix addresses: a real file position beyond
// LONG_MAX (2^31 - 1), which std::ftell()/std::fseek() cannot represent on
// Windows (LONG_MAX == 2147483647). Seeking past the current end and writing
// one byte creates a sparse file on NTFS -- no 2 GiB of data is physically
// written, so this stays fast and CI-safe.
TEST(PortableFileIo, PositionBeyond2GibIsQueriedCorrectly) {
    TempFile f;
    ASSERT_NE(f.get(), nullptr);
    constexpr int64_t kBeyond2Gib = 2147483648LL + 12345; // 2^31 + 12345
    ASSERT_EQ(Fseek64(f.get(), kBeyond2Gib, SEEK_SET), 0);
    const char one_byte = 'x';
    ASSERT_EQ(std::fwrite(&one_byte, 1, 1, f.get()), 1u);
    EXPECT_EQ(Ftell64(f.get()), kBeyond2Gib + 1);
}

TEST(PortableFileIo, SeekSetBeyond2GibThenReadBackPosition) {
    TempFile f;
    ASSERT_NE(f.get(), nullptr);
    constexpr int64_t kBeyond2Gib = 3000000000LL; // ~2.8 GiB, comfortably past LONG_MAX
    ASSERT_EQ(Fseek64(f.get(), kBeyond2Gib, SEEK_SET), 0);
    EXPECT_EQ(Ftell64(f.get()), kBeyond2Gib);
}

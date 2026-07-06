// ::getenv is the portable way to check the opt-in network-test gate; silence the
// MSVC CRT "unsafe" deprecation (C4996) so it does not trip warnings-as-error.
#define _CRT_SECURE_NO_WARNINGS

#include <filesystem>
#include <gtest/gtest.h>
#include <update/http_download.h>
using namespace exosnap::update;

TEST(HttpDownload, RejectsNonHttpsUrl) {
    std::atomic<bool> cancel{false};
    auto err = DownloadToFile("http://example.com/x.zip", (std::filesystem::temp_directory_path() / "x.zip").wstring(),
                              {}, cancel);
    ASSERT_TRUE(err.has_value());
}

TEST(HttpDownload, RejectsMalformedUrl) {
    std::atomic<bool> cancel{false};
    auto err = DownloadToFile("not a url", L"x.zip", {}, cancel);
    ASSERT_TRUE(err.has_value());
}

TEST(HttpDownload, PreSetCancelReturnsCanceledWithoutFile) {
    std::atomic<bool> cancel{true};
    auto dest = std::filesystem::temp_directory_path() / "exosnap_dl_cancel.bin";
    auto err = DownloadToFile("https://example.com/x.zip", dest.wstring(), {}, cancel);
    ASSERT_TRUE(err.has_value());
    EXPECT_EQ(*err, "canceled");
    EXPECT_FALSE(std::filesystem::exists(dest));
}

// Real-network integration; skipped unless EXOSNAP_NET_TESTS=1 (CI stays hermetic).
TEST(HttpDownload, DownloadsSmallFileOverHttps) {
    if (!::getenv("EXOSNAP_NET_TESTS"))
        GTEST_SKIP() << "set EXOSNAP_NET_TESTS=1";
    std::atomic<bool> cancel{false};
    auto dest = std::filesystem::temp_directory_path() / "exosnap_dl_it.bin";
    DownloadProgress last{};
    auto err = DownloadToFile(
        "https://api.github.com/repos/Exoridus/exosnap", dest.wstring(), [&](const DownloadProgress& p) { last = p; },
        cancel);
    ASSERT_FALSE(err.has_value()) << *err;
    EXPECT_GT(last.bytes_received, 0u);
    std::filesystem::remove(dest);
}

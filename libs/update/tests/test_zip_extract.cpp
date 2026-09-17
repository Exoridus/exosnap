// test_zip_extract.cpp -- vendored-miniz zip extraction + zip-slip guard tests.

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <miniz.h>
#include <update/zip_extract.h>
namespace fs = std::filesystem;
using namespace exosnap::update;

namespace {
fs::path MakeTempDir(const char* tag) {
    auto p = fs::temp_directory_path() / (std::string("exosnap_zip_test_") + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}
// Write a zip with given (name, content) entries using miniz.
void WriteZip(const fs::path& zip, std::initializer_list<std::pair<const char*, const char*>> entries) {
    mz_zip_archive za{};
    ASSERT_TRUE(mz_zip_writer_init_file(&za, zip.string().c_str(), 0));
    for (auto& [name, content] : entries)
        ASSERT_TRUE(mz_zip_writer_add_mem(&za, name, content, strlen(content), MZ_DEFAULT_LEVEL));
    ASSERT_TRUE(mz_zip_writer_finalize_archive(&za));
    mz_zip_writer_end(&za);
}
} // namespace

TEST(ZipExtract, RoundtripPreservesTreeAndContent) {
    auto dir = MakeTempDir("roundtrip");
    auto zip = dir / "pkg.zip";
    WriteZip(zip, {{"a.txt", "alpha"}, {"sub/b.txt", "beta"}});
    auto err = ExtractZip(zip.wstring(), (dir / "out").wstring());
    ASSERT_FALSE(err.has_value()) << *err;
    std::ifstream a(dir / "out" / "a.txt");
    std::string s((std::istreambuf_iterator<char>(a)), {});
    a.close(); // Windows: the open handle would block the remove_all below.
    EXPECT_EQ(s, "alpha");
    EXPECT_TRUE(fs::exists(dir / "out" / "sub" / "b.txt"));
    fs::remove_all(dir);
}

TEST(ZipExtract, ExtractsPastMaxPathIntoADeepDestination) {
    // A portable installation sits wherever the user put it, and the deepest
    // entry in the real package adds about 90 characters to that. A synced cloud
    // folder or a per-project layout reaches 260 without anything unusual, and
    // the CRT open call stays bound to MAX_PATH whatever the system long-path
    // setting says. The destination below is built to cross it on purpose.
    auto dir = MakeTempDir("deep");
    fs::path deep = dir;
    while (deep.wstring().size() < 200)
        deep /= "a_directory_component_of_some_length";

    auto zip = dir / "pkg.zip";
    WriteZip(zip, {{"nested/plugins/networkinformation/qnetworklistmanager.dll", "payload"}});

    const auto out = deep / "out";
    ASSERT_GT((out / "nested/plugins/networkinformation/qnetworklistmanager.dll").wstring().size(), 260u)
        << "the destination must exceed MAX_PATH or this test proves nothing";

    auto err = ExtractZip(zip.wstring(), out.wstring());
    ASSERT_FALSE(err.has_value()) << *err;

    // Reading the result back is bound by MAX_PATH exactly as writing it was, so
    // the check has to use the extended form too. A plain `fs::exists` here
    // reports false on a file that is present, which would read as the extraction
    // having silently done nothing.
    const fs::path extracted = out / "nested" / "plugins" / "networkinformation" / "qnetworklistmanager.dll";
    const fs::path extracted_long(LR"(\\?\)" + fs::absolute(extracted).make_preferred().wstring());
    EXPECT_TRUE(fs::exists(extracted_long)) << "extracted file missing at " << extracted.string();

    // Removal is MAX_PATH bound the same way the write was, so the tree this
    // test just created cannot be deleted by the plain call. Leaving it behind
    // would also fail the NEXT run, whose setup starts by clearing this path.
    std::error_code ec;
    fs::remove_all(fs::path(LR"(\\?\)" + fs::absolute(dir).make_preferred().wstring()), ec);
}

TEST(ZipExtract, RejectsTraversalEntriesWithoutWriting) {
    auto dir = MakeTempDir("slip");
    auto zip = dir / "evil.zip";
    WriteZip(zip, {{"ok.txt", "x"}, {"../evil.txt", "pwn"}});
    auto out = dir / "out";
    auto err = ExtractZip(zip.wstring(), out.wstring());
    EXPECT_TRUE(err.has_value());
    EXPECT_FALSE(fs::exists(dir / "evil.txt"));
    EXPECT_FALSE(fs::exists(out / "ok.txt")); // guard runs BEFORE any write
    fs::remove_all(dir);
}

TEST(ZipExtract, SafeEntryNamePredicate) {
    EXPECT_TRUE(IsSafeZipEntryName("a/b/c.txt"));
    EXPECT_FALSE(IsSafeZipEntryName("../x"));
    EXPECT_FALSE(IsSafeZipEntryName("a/../../x"));
    EXPECT_FALSE(IsSafeZipEntryName("C:/abs.txt"));
    EXPECT_FALSE(IsSafeZipEntryName("/abs.txt"));
    EXPECT_FALSE(IsSafeZipEntryName("a\\..\\x"));
}

// test_url_utils.cpp -- UTF-8 -> UTF-16 conversion used by http_download.cpp
// and zip_extract.cpp. Pure conversion checks, no network/filesystem I/O.
//
// Inputs/expectations use \x / \u escapes instead of literal non-ASCII source
// characters so the test does not depend on this file's saved encoding.

#include <gtest/gtest.h>

#include "../src/url_utils.h"

using namespace exosnap::update;

TEST(UrlUtils, EmptyInputYieldsEmptyOutput) {
    EXPECT_EQ(Utf8ToWide(""), L"");
}

TEST(UrlUtils, AsciiRoundTrips) {
    EXPECT_EQ(Utf8ToWide("https://example.com/file.zip"), L"https://example.com/file.zip");
}

// U+00E9 (e-acute) encodes in UTF-8 as the two-byte sequence 0xC3 0xA9. A
// naive byte-wise widening (std::wstring(s.begin(), s.end())) turns those two
// bytes into two separate garbled wide chars (U+00C3, U+00A9) instead of the
// single correct code unit, and inflates the length by one. A correct
// CP_UTF8 conversion collapses them back to one code unit.
TEST(UrlUtils, NonAsciiPathSegmentConvertsToSingleCodeUnit) {
    const std::string utf8_path = "https://example.com/caf\xC3\xA9/asset.zip";
    const std::wstring wide = Utf8ToWide(utf8_path);

    const std::wstring expected = L"https://example.com/caf\u00E9/asset.zip";
    EXPECT_EQ(wide, expected);

    // The naive byte-wise widening would have been one wchar_t longer (two
    // wide chars for the one non-ASCII code point instead of one).
    EXPECT_EQ(wide.size(), utf8_path.size() - 1);
}

// A multi-byte (3-byte) UTF-8 code point sequence must collapse to exactly
// one wide char per code point as well (U+30C6 U+30B9 U+30C8, Katakana).
TEST(UrlUtils, ThreeByteUtf8SequenceConvertsToSingleCodeUnit) {
    const std::string utf8_segment = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";
    const std::wstring wide = Utf8ToWide(utf8_segment);

    const std::wstring expected = L"\u30C6\u30B9\u30C8";
    EXPECT_EQ(wide, expected);
    EXPECT_EQ(wide.size(), 3u);
}

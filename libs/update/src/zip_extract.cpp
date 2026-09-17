// src/zip_extract.cpp -- vendored-miniz zip extraction with a zip-slip guard.
//
// No Qt. All paths cross the API boundary as UTF-8 (miniz entry names) or
// std::wstring (Windows filesystem paths); conversion goes through
// url_utils.h's Utf8ToWide (proper CP_UTF8 widening, not byte-wise).

#include <update/zip_extract.h>

#include "url_utils.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <miniz.h>

#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace exosnap::update {

namespace {

// Absolute local path in `\\?\` form, which the Win32 layer accepts beyond
// MAX_PATH. A UNC path takes the `\\?\UNC\` spelling instead, and anything
// already prefixed, or that cannot be made absolute, is handed back untouched --
// the caller's failure is then the real one rather than one this produced.
std::wstring ToExtendedPath(const std::wstring& path) {
    if (path.starts_with(LR"(\\?\)") || path.starts_with(LR"(\\.\)"))
        return path;

    std::error_code ec;
    // The prefix disables all normalisation by Win32, so `.` and `..` would
    // survive into the filesystem call and resolve to the wrong place.
    fs::path absolute = fs::weakly_canonical(fs::absolute(fs::path(path), ec), ec);
    if (ec || absolute.empty())
        return path;

    std::wstring native = absolute.make_preferred().wstring();
    if (native.starts_with(LR"(\\)"))
        return LR"(\\?\UNC\)" + native.substr(2);
    return LR"(\\?\)" + native;
}

} // namespace

bool IsSafeZipEntryName(std::string_view entry_name) {
    if (entry_name.empty())
        return false;

    // Reject absolute paths: leading '/' or leading '\'.
    if (entry_name.front() == '/' || entry_name.front() == '\\')
        return false;

    // Reject drive letters / any embedded ':' (e.g. "C:/abs.txt").
    if (entry_name.find(':') != std::string_view::npos)
        return false;

    // Walk path components split on either separator, rejecting any ".."
    // component regardless of which slash style is used.
    size_t start = 0;
    while (start <= entry_name.size()) {
        size_t end = entry_name.find_first_of("/\\", start);
        std::string_view component =
            (end == std::string_view::npos) ? entry_name.substr(start) : entry_name.substr(start, end - start);

        if (component == "..")
            return false;

        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }

    return true;
}

std::optional<std::string> ExtractZip(const std::wstring& zip_path, const std::wstring& dest_dir,
                                      const ZipProgressFn& progress) {
    mz_zip_archive archive{};
    std::string zip_path_utf8;
    {
        int needed = WideCharToMultiByte(CP_UTF8, 0, zip_path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return std::string("ExtractZip: failed to convert zip path to UTF-8");
        zip_path_utf8.resize(static_cast<size_t>(needed) - 1);
        WideCharToMultiByte(CP_UTF8, 0, zip_path.c_str(), -1, zip_path_utf8.data(), needed, nullptr, nullptr);
    }

    if (!mz_zip_reader_init_file(&archive, zip_path_utf8.c_str(), 0)) {
        return std::string("ExtractZip: failed to open zip archive: ") + zip_path_utf8;
    }

    const mz_uint num_entries = mz_zip_reader_get_num_files(&archive);

    // FIRST loop: validate every entry name BEFORE writing anything (zip-slip guard).
    std::vector<std::string> entry_names;
    entry_names.reserve(num_entries);
    for (mz_uint i = 0; i < num_entries; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
            mz_zip_reader_end(&archive);
            return std::string("ExtractZip: failed to stat entry ") + std::to_string(i);
        }
        std::string name(stat.m_filename);
        if (!IsSafeZipEntryName(name)) {
            mz_zip_reader_end(&archive);
            return std::string("ExtractZip: unsafe entry name rejected: ") + name;
        }
        entry_names.push_back(std::move(name));
    }

    // Extended-length form, because a portable installation is wherever the user
    // put it and the deepest entry in the package adds about 90 characters on top
    // of that. `_wfopen_s` below is a CRT entry point and stays bound to MAX_PATH
    // whatever the system long-path opt-in says, so a destination that is merely
    // deep -- a synced cloud folder, a per-project layout -- fails to open with
    // nothing wrong with it. Prefixing the root once is enough: every path built
    // from `dest` inherits it.
    fs::path dest = ToExtendedPath(dest_dir);
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        mz_zip_reader_end(&archive);
        return std::string("ExtractZip: failed to create destination directory: ") + ec.message();
    }

    // SECOND loop: create directories + extract entries.
    ZipProgress zip_progress;
    zip_progress.entries_total = num_entries;
    for (mz_uint i = 0; i < num_entries; ++i) {
        const std::string& name = entry_names[i];
        // Zip entry names use '/', and the `\\?\` prefix on `dest` turns off every
        // normalisation Win32 would otherwise apply -- including the one that maps
        // a forward slash onto a separator. An unconverted name reaches the
        // filesystem as one long invalid component, so the separators are made
        // native here rather than left to a layer that no longer does it.
        fs::path out_path = (dest / fs::path(Utf8ToWide(name))).make_preferred();

        if (mz_zip_reader_is_file_a_directory(&archive, i)) {
            fs::create_directories(out_path, ec);
            if (ec) {
                mz_zip_reader_end(&archive);
                return std::string("ExtractZip: failed to create directory for entry: ") + name;
            }
        } else {
            fs::create_directories(out_path.parent_path(), ec);
            if (ec) {
                mz_zip_reader_end(&archive);
                return std::string("ExtractZip: failed to create parent directory for entry: ") + name;
            }
            // mz_zip_reader_extract_to_file() only accepts a narrow (ANSI) path,
            // which mangles non-ASCII destination paths on Windows. Open the
            // destination via the wide-char CRT entry point ourselves and
            // extract into that FILE* instead so the on-disk path round-trips
            // through UTF-16 rather than the ANSI code page.
            FILE* out_file = nullptr;
            if (_wfopen_s(&out_file, out_path.c_str(), L"wb") != 0 || !out_file) {
                mz_zip_reader_end(&archive);
                return std::string("ExtractZip: failed to open destination file for entry: ") + name;
            }
            mz_bool ok = mz_zip_reader_extract_to_cfile(&archive, i, out_file, 0);
            fclose(out_file);
            if (!ok) {
                mz_zip_reader_end(&archive);
                return std::string("ExtractZip: failed to extract entry: ") + name;
            }
        }

        zip_progress.entries_done = i + 1;
        if (progress)
            progress(zip_progress);
    }

    mz_zip_reader_end(&archive);
    return std::nullopt;
}

} // namespace exosnap::update

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace exosnap::diagnostics {

// Minimal in-memory ZIP writer over the vendored miniz (the same library
// libs/update already uses to *extract*). It links the existing exosnap_miniz
// target rather than introducing a new archive library, and validates every
// entry name with libs/update's IsSafeZipEntryName so a stored path can never
// carry a drive letter, an absolute prefix or a ".." escape.
//
// Entries are compressed into a heap archive; Finalize()/WriteToFile() emit the
// complete .zip. Not thread-safe; used from a single UI action.
class ZipWriter {
  public:
    ZipWriter();
    ~ZipWriter();

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    // Add one in-memory file. Returns false if the writer failed to init, the
    // name is unsafe, or miniz rejected the entry.
    bool AddFileFromMemory(std::string_view name, const void* data, std::size_t size);
    bool AddFileFromMemory(std::string_view name, std::string_view bytes) {
        return AddFileFromMemory(name, bytes.data(), bytes.size());
    }
    bool AddFileFromMemory(std::string_view name, const std::vector<char>& bytes) {
        return AddFileFromMemory(name, bytes.data(), bytes.size());
    }

    // Finalize the archive into an in-memory buffer. Empty on failure. After
    // this the writer must not be reused.
    [[nodiscard]] std::vector<char> Finalize();

    // Finalize and write the archive to a file (UTF-16 path). Returns false on
    // any failure.
    bool WriteToFile(const std::wstring& path);

  private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace exosnap::diagnostics

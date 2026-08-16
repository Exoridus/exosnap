#include "diagnostics/ZipWriter.h"

#include <update/zip_extract.h> // IsSafeZipEntryName — reused, not reimplemented

#include <miniz.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace exosnap::diagnostics {

struct ZipWriter::Impl {
    mz_zip_archive zip{};
    bool ok = false;
    bool finalized = false;
};

ZipWriter::ZipWriter() : impl_(new Impl) {
    mz_zip_zero_struct(&impl_->zip);
    impl_->ok = mz_zip_writer_init_heap(&impl_->zip, 0, 0) != MZ_FALSE;
}

ZipWriter::~ZipWriter() {
    if (impl_) {
        if (impl_->ok && !impl_->finalized) {
            mz_zip_writer_end(&impl_->zip);
        }
        delete impl_;
    }
}

bool ZipWriter::AddFileFromMemory(std::string_view name, const void* data, std::size_t size) {
    if (!impl_ || !impl_->ok || impl_->finalized) {
        return false;
    }
    if (!update::IsSafeZipEntryName(name)) {
        return false;
    }
    const std::string entry(name);
    return mz_zip_writer_add_mem(&impl_->zip, entry.c_str(), data, size, MZ_BEST_COMPRESSION) != MZ_FALSE;
}

std::vector<char> ZipWriter::Finalize() {
    if (!impl_ || !impl_->ok || impl_->finalized) {
        return {};
    }
    void* buf = nullptr;
    std::size_t buf_size = 0;
    // The library's own finalize is authoritative for the archive structure: it
    // is what writes the central directory and the end-of-central-directory
    // record, so a heap buffer that exists before this returns is not an archive.
    if (mz_zip_writer_finalize_heap_archive(&impl_->zip, &buf, &buf_size) == MZ_FALSE) {
        return {};
    }
    std::vector<char> out(static_cast<const char*>(buf), static_cast<const char*>(buf) + buf_size);
    mz_free(buf);
    // Failing here would mean the writer state could not be released; the bytes
    // themselves are already complete, but a caller must not be told the archive
    // is fine while the library says otherwise.
    const bool ended = mz_zip_writer_end(&impl_->zip) != MZ_FALSE;
    impl_->finalized = true;
    if (!ended) {
        return {};
    }
    return out;
}

bool ZipWriter::WriteToFile(const std::wstring& path) {
    const std::vector<char> bytes = Finalize();
    if (bytes.empty()) {
        return false;
    }

    const std::filesystem::path file_path(path);
    bool written = false;
    {
        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            // QCR-204, the same defect QCR-108 found in the output-folder probe:
            // a good() stream after write() only means the bytes reached the
            // buffer. The transfer to the volume happens in flush/close, and
            // ~ofstream discards whatever it reports — so a full disk, a quota
            // or a dropped network share produced a truncated .zip that the
            // support-bundle flow announced as a success. Judged after close().
            out.flush();
            out.close();
            written = static_cast<bool>(out);
        }
    }

    if (!written) {
        // A truncated archive under the final name is worse than no archive:
        // the user would attach it to a support request and only find out then.
        std::error_code ec;
        std::filesystem::remove(file_path, ec);
        return false;
    }
    return true;
}

} // namespace exosnap::diagnostics

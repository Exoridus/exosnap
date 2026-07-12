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
    if (mz_zip_writer_finalize_heap_archive(&impl_->zip, &buf, &buf_size) == MZ_FALSE) {
        return {};
    }
    std::vector<char> out(static_cast<const char*>(buf), static_cast<const char*>(buf) + buf_size);
    mz_free(buf);
    mz_zip_writer_end(&impl_->zip);
    impl_->finalized = true;
    return out;
}

bool ZipWriter::WriteToFile(const std::wstring& path) {
    const std::vector<char> bytes = Finalize();
    if (bytes.empty()) {
        return false;
    }
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

} // namespace exosnap::diagnostics

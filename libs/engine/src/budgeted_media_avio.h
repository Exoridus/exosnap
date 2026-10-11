#pragma once

#include <exosnap/engine/background_io.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#include <memory>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace exosnap::engine {

class BudgetedMediaAvio {
  public:
    static std::unique_ptr<BudgetedMediaAvio> Open(const std::filesystem::path& path, bool write,
                                                   const BackgroundIoAdmission* before_io,
                                                   const std::atomic<bool>* cancelled, bool* denied) {
        auto owner = std::unique_ptr<BudgetedMediaAvio>(new BudgetedMediaAvio(before_io, cancelled, denied));
        const DWORD access = write ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
        const DWORD disposition = write ? CREATE_ALWAYS : OPEN_EXISTING;
        HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return nullptr;
        const int descriptor =
            _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY | (write ? _O_RDWR : _O_RDONLY));
        if (descriptor < 0) {
            CloseHandle(handle);
            return nullptr;
        }
        owner->file_ = _fdopen(descriptor, write ? "wb+" : "rb");
        if (!owner->file_) {
            _close(descriptor);
            return nullptr;
        }
        if (setvbuf(owner->file_, nullptr, _IONBF, 0) != 0)
            return nullptr;
        auto* buffer = static_cast<unsigned char*>(av_malloc(kBufferBytes));
        if (!buffer)
            return nullptr;
        owner->context_ = avio_alloc_context(buffer, kBufferBytes, write ? 1 : 0, owner.get(), write ? nullptr : &Read,
                                             write ? &Write : nullptr, &Seek);
        if (!owner->context_) {
            av_free(buffer);
            return nullptr;
        }
        owner->context_->seekable = AVIO_SEEKABLE_NORMAL;
        return owner;
    }

    ~BudgetedMediaAvio() {
        Close();
    }
    BudgetedMediaAvio(const BudgetedMediaAvio&) = delete;
    BudgetedMediaAvio& operator=(const BudgetedMediaAvio&) = delete;

    AVIOContext* context() const {
        return context_;
    }

    int Close() {
        if (context_) {
            av_freep(&context_->buffer);
            avio_context_free(&context_);
        }
        if (!file_)
            return 0;
        const int result = fclose(file_);
        file_ = nullptr;
        return result == 0 ? 0 : AVERROR(EIO);
    }

  private:
    static constexpr int kBufferBytes = 64 * 1024;
    static constexpr int kMaxTransferBytes = 4 * 1024 * 1024;

    BudgetedMediaAvio(const BackgroundIoAdmission* before_io, const std::atomic<bool>* cancelled, bool* denied)
        : before_io_(before_io), cancelled_(cancelled), denied_(denied) {
    }

    bool Admit(uint64_t bytes) {
        if ((!cancelled_ || !cancelled_->load(std::memory_order_relaxed)) &&
            (!before_io_ || !*before_io_ || (*before_io_)(bytes, cancelled_)))
            return true;
        *denied_ = true;
        return false;
    }

    static int Read(void* opaque, uint8_t* bytes, int size) {
        auto* self = static_cast<BudgetedMediaAvio*>(opaque);
        const auto current = _ftelli64(self->file_);
        if (current < 0 || _fseeki64(self->file_, 0, SEEK_END) != 0)
            return AVERROR(EIO);
        const auto end = _ftelli64(self->file_);
        if (end < 0 || _fseeki64(self->file_, current, SEEK_SET) != 0)
            return AVERROR(EIO);
        if (current >= end)
            return AVERROR_EOF;
        const auto count =
            static_cast<int>((std::min)(static_cast<int64_t>((std::min)(size, kMaxTransferBytes)), end - current));
        if (!self->Admit(static_cast<uint64_t>(count)))
            return AVERROR_EXIT;
        const auto read = fread(bytes, 1, static_cast<size_t>(count), self->file_);
        if (read == 0)
            return ferror(self->file_) ? AVERROR(EIO) : AVERROR_EOF;
        return static_cast<int>(read);
    }

    static int Write(void* opaque, const uint8_t* bytes, int size) {
        auto* self = static_cast<BudgetedMediaAvio*>(opaque);
        int completed = 0;
        while (completed < size) {
            const auto count = (std::min)(size - completed, kMaxTransferBytes);
            if (!self->Admit(static_cast<uint64_t>(count)))
                return AVERROR_EXIT;
            const auto written = fwrite(bytes + completed, 1, static_cast<size_t>(count), self->file_);
            if (written != static_cast<size_t>(count))
                return AVERROR(EIO);
            completed += count;
        }
        return completed;
    }

    static int64_t Seek(void* opaque, int64_t offset, int whence) {
        auto* self = static_cast<BudgetedMediaAvio*>(opaque);
        if (whence == AVSEEK_SIZE) {
            const auto current = _ftelli64(self->file_);
            if (current < 0 || _fseeki64(self->file_, 0, SEEK_END) != 0)
                return AVERROR(EIO);
            const auto end = _ftelli64(self->file_);
            return _fseeki64(self->file_, current, SEEK_SET) == 0 ? end : AVERROR(EIO);
        }
        if (_fseeki64(self->file_, offset, whence & ~AVSEEK_FORCE) != 0)
            return AVERROR(EIO);
        return _ftelli64(self->file_);
    }

    const BackgroundIoAdmission* before_io_;
    const std::atomic<bool>* cancelled_;
    bool* denied_;
    FILE* file_ = nullptr;
    AVIOContext* context_ = nullptr;
};

} // namespace exosnap::engine

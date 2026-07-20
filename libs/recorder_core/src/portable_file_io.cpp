#include "recorder_core/portable_file_io.h"

namespace recorder_core {

int64_t Ftell64(FILE* file) noexcept {
    return _ftelli64(file);
}

int Fseek64(FILE* file, int64_t offset, int origin) noexcept {
    return _fseeki64(file, offset, origin);
}

} // namespace recorder_core

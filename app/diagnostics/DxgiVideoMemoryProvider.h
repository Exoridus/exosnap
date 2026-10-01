#pragma once

#include "VideoMemoryProvider.h"

namespace exosnap::diagnostics {

class DxgiVideoMemoryProvider final : public IVideoMemoryProvider {
  public:
    VideoMemoryReading Read(int64_t adapter_luid) override;
};

} // namespace exosnap::diagnostics

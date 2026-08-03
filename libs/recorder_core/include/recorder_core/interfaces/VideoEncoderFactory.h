#pragma once
// Dispatches an AdapterVendor to a concrete IVideoEncoder implementation.
// Virtual so tests can inject a subclass that returns a fake encoder for any
// vendor value, without touching the real NVENC path.
//
// Only include <capability/adapter_enum.h> here (a header-only enum) — never
// link exosnap_capability from recorder_core: exosnap_capability already links
// recorder_core PUBLICLY (it consumes recorder_core's video/codec types), so a
// reverse link dependency would be a build-graph cycle. See recorder_core's
// CMakeLists.txt for the include-path-only wiring this header relies on.

#include <capability/adapter_enum.h>
#include <recorder_core/interfaces/IVideoEncoder.h>

#include <memory>

namespace recorder_core {

class VideoEncoderFactory {
  public:
    virtual ~VideoEncoderFactory() = default;

    // Nvidia -> NvencVideoEncoder. Every other vendor -> nullptr (not wired
    // yet; callers treat a null result as the same fatal init error as a
    // failed Open()/Configure()).
    virtual std::unique_ptr<IVideoEncoder> Create(exosnap::capability::AdapterVendor vendor) const;
};

} // namespace recorder_core

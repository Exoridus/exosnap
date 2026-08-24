#pragma once
// Dispatches an AdapterVendor to a concrete IVideoEncoder implementation.
// Virtual so tests can inject a subclass that returns a fake encoder for any
// vendor value, without touching the real NVENC path.
//
// Only include <capability/adapter_enum.h> here (a header-only enum) — never
// link exosnap_capability from engine: exosnap_capability already links
// engine PUBLICLY (it consumes the engine's video/codec types), so a
// reverse link dependency would be a build-graph cycle. See the engine's
// CMakeLists.txt for the include-path-only wiring this header relies on.

#include <capability/adapter_enum.h>
#include <exosnap/engine/interfaces/IVideoEncoder.h>

#include <memory>

namespace exosnap::engine {

class VideoEncoderFactory {
  public:
    virtual ~VideoEncoderFactory() = default;

    // Nvidia -> NvencVideoEncoder. Every other vendor -> nullptr (not wired
    // yet; callers treat a null result as the same fatal init error as a
    // failed Open()/Configure()).
    virtual std::unique_ptr<IVideoEncoder> Create(exosnap::capability::AdapterVendor vendor) const;
};

} // namespace exosnap::engine

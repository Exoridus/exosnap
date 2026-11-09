#pragma once
// Dispatches a PCI vendor ID to a concrete IVideoEncoder implementation.
// Virtual so tests can inject a subclass that returns a fake encoder for any
// vendor ID, without touching the real NVENC path.

#include <exosnap/engine/interfaces/IVideoEncoder.h>

#include <exosnap/engine/recorder_session.h>

#include <cstdint>
#include <memory>
#include <string>

namespace exosnap::engine {

class VideoEncoderFactory {
  public:
    virtual ~VideoEncoderFactory() = default;

    // Maps the vendor to the backend this build implements, then dispatches.
    // Nvidia -> NvencVideoEncoder with the NVENC alternative of the config's
    // BackendTuning applied. Every other vendor -> nullptr (not wired yet;
    // callers treat a null result as the same fatal init error as a failed
    // Open()/Configure()).
    virtual std::unique_ptr<IVideoEncoder> Create(uint32_t pci_vendor_id, const RecorderConfig& config = {}) const;

    // Dispatch from the actual recording adapter. An unsupported vendor fails
    // explicitly; backend tuning is consumed only by the selected implementation.
    std::unique_ptr<IVideoEncoder> CreateForAdapter(uint32_t pci_vendor_id, const RecorderConfig& config,
                                                    std::string& error) const;
};

} // namespace exosnap::engine

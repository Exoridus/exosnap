#pragma once
// Platform-agnostic video encoder interface.
// Windows implementation: NvencVideoEncoder (wraps NvencEncoder + D3D11).
// GpuTextureHandle lets the encoder operate zero-copy on GPU memory when
// both capture and encoder share the same GPU context.

#include <recorder_core/packet_types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace recorder_core {

// Opaque GPU texture handle passed from IVideoCaptureSource to IVideoEncoder.
// On Windows/D3D11: ID3D11Texture2D* cast to void*.
// The encoder does not take ownership.
using GpuTextureHandle = void*;

class IVideoEncoder {
  public:
    virtual ~IVideoEncoder() = default;

    // Open an encode session.
    // gpu_context: on Windows, an ID3D11Device* cast to void*. Null for CPU-only encoders.
    virtual bool Open(void* gpu_context, std::string& out_error) = 0;

    // Configure dimensions and frame rate. Call after Open(), before RegisterSlotTexture().
    virtual bool Configure(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den,
                           std::string& out_error) = 0;

    // Register a GPU texture as a reusable input slot.
    // slot_idx in [0, SlotCount()). Texture must remain valid for the encoder lifetime.
    virtual bool RegisterSlotTexture(int32_t slot_idx, GpuTextureHandle texture, std::string& out_error) = 0;

    virtual int32_t SlotCount() const = 0;

    // Acquire the next free input slot. Returns -1 if no slot is free.
    virtual int32_t AcquireFreeSlot() = 0;

    // Submit one frame for encoding.
    // Returns true + packet with bytes -> encoded output available.
    // Returns true + packet with bytes empty -> buffered (need more input).
    // Returns false -> fatal error (out_error set).
    virtual bool EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t width, uint32_t height,
                             EncodedVideoPacket& out_packet, std::string& out_error) = 0;

    // Flush all buffered frames (EOS). Appends remaining packets.
    virtual bool Flush(std::vector<EncodedVideoPacket>& out_packets, std::string& out_error) = 0;

    virtual void Destroy() = 0;
};

} // namespace recorder_core

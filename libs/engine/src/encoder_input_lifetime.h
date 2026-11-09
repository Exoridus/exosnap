#pragma once

#include <exosnap/engine/interfaces/IVideoEncoder.h>

#include <utility>

namespace exosnap::engine {

// Declare after borrowed input resources and before their first registration.
// Call Destroy before releasing a separately held encoder owner early.
class EncoderInputLifetime {
  public:
    explicit EncoderInputLifetime(IVideoEncoder& encoder) noexcept : encoder_(&encoder) {
    }
    EncoderInputLifetime(const EncoderInputLifetime&) = delete;
    EncoderInputLifetime& operator=(const EncoderInputLifetime&) = delete;

    ~EncoderInputLifetime() {
        Destroy();
    }

    void Destroy() {
        if (auto* encoder = std::exchange(encoder_, nullptr))
            encoder->Destroy();
    }

  private:
    IVideoEncoder* encoder_;
};

} // namespace exosnap::engine

#pragma once

#include "ControllerState.h"
#include "NativeRenderTarget.h"

#include <exosnap/engine/gpu_timestamp_profiler.h>
#include <exosnap/engine/visual_diagnostics.h>
#include <exosnap/engine/visual_frame.h>
#include <gamepad/gamepad.h>

namespace exosnap::quick {

enum class NativeGamepadSampleResult { Ready, Busy, UnsupportedModel, Failed };

// The host owns scheduling and destruction on the GUI thread. Each instance is
// one fixed request/size generation and borrows no native input callback owner.
class NativeGamepadProducer final {
  public:
    [[nodiscard]] static bool Supports(const gamepad::DeviceIdentity&) noexcept;
    static std::unique_ptr<NativeGamepadProducer> Create(const MappedGamepadArtwork&, gamepad::HistoryReadView,
                                                         const engine::SourceToken&, ID3D11Device*,
                                                         ID3D11DeviceContext*, QSize, bool ghost, bool light,
                                                         QString& error);
    ~NativeGamepadProducer() = default;
    NativeGamepadProducer(const NativeGamepadProducer&) = delete;
    NativeGamepadProducer& operator=(const NativeGamepadProducer&) = delete;

    NativeGamepadSampleResult RenderSample(engine::CaptureTime100ns, QString& error);
    // Valid only following Ready, before another RenderSample or destruction.
    [[nodiscard]] const engine::VisualFrameStamp& Stamp() const noexcept;
    [[nodiscard]] std::optional<engine::CaptureTime100ns> InputTime() const noexcept;
    [[nodiscard]] ID3D11Texture2D* Texture() const noexcept;
    [[nodiscard]] const engine::NativeGamepadDiagnostics& Diagnostics() const noexcept;

  private:
    NativeGamepadProducer(const MappedGamepadArtwork&, gamepad::HistoryReadView, engine::SourceToken);
    gamepad::HistoryReadView history_;
    engine::SourceToken token_;
    engine::VisualFrameStamp stamp_;
    std::optional<engine::CaptureTime100ns> input_time_;
    uint64_t sequence_ = 0;
    ControllerState state_;
    NativeRenderTarget target_;
    ID3D11DeviceContext* context_ = nullptr;
    engine::GpuStageTimer render_timer_;
    engine::NativeGamepadDiagnostics diagnostics_;
};

} // namespace exosnap::quick

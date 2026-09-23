#pragma once
#include <cstdint>
#include <optional>

namespace exosnap::engine {

struct CaptureContentSize {
    int32_t width = 0;
    int32_t height = 0;

    friend constexpr bool operator==(CaptureContentSize, CaptureContentSize) = default;
};

// What the recording does with a Windows.Graphics.Capture frame, judged by its
// ContentSize against the size the encoder was configured for.
enum class WgcFrameGeometry : uint8_t {
    Encode,
    // The frame shows the window in its minimized (iconic) state, or is a stale
    // frame from that state. The recording keeps its last encoded frame.
    HoldLastFrame,
    // The source really changed size. The encoder is fixed at session start, so
    // the recording ends with an explicit size-changed failure.
    SourceResized,
};

// Distinguishes a minimized window from a resized one for one recording session.
//
// A minimized window still yields WGC frames, sized to its iconic caption. That
// size says nothing about the recorded source, so it must not end the recording.
// A frame rendered while minimized may also be dequeued after the window is
// restored; its size matches the iconic size seen during the minimized episode.
// Any other size mismatch is a resize.
class WgcFrameGeometryTracker {
  public:
    explicit constexpr WgcFrameGeometryTracker(CaptureContentSize session) noexcept : session_(session) {
    }

    // `window_minimized` is IsIconic() for the captured window at the time the
    // frame is examined; always false for a monitor target.
    [[nodiscard]] constexpr WgcFrameGeometry Classify(CaptureContentSize content, bool window_minimized) noexcept {
        if (content == session_) {
            encoded_any_ = true;
            iconic_ = std::nullopt;
            return WgcFrameGeometry::Encode;
        }
        if (!encoded_any_)
            return WgcFrameGeometry::SourceResized;
        if (window_minimized) {
            iconic_ = content;
            return WgcFrameGeometry::HoldLastFrame;
        }
        if (iconic_ == content)
            return WgcFrameGeometry::HoldLastFrame;
        return WgcFrameGeometry::SourceResized;
    }

  private:
    CaptureContentSize session_;
    std::optional<CaptureContentSize> iconic_;
    bool encoded_any_ = false;
};

} // namespace exosnap::engine

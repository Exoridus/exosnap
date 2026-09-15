#pragma once

// A webcam frame provider that never changes, for measuring what the PiP overlay
// does when nothing else moves.
//
// The overlay's geometry, opacity and chroma key are settable while a recording
// runs, and a change has to advance the overlay generation or a still capture
// source keeps re-emitting the composite made with the previous overlay. Proving
// that needs a recording in which the overlay is the only thing that can have
// caused a recomposition -- and a real camera is the opposite of that: every
// delivered sample advances the webcam generation, which recomposites the frame
// on its own and hides a missing overlay generation completely.
//
// Hence both halves of the contract below. Identical pixels are not enough; the
// generation has to stand still too.
//
// This is a measurement aid in the same class as --auto-record and --visual-test:
// selected by argv, never the default, and it replaces only the camera. The
// overlay, compositor and encoder paths under test are the shipping ones.

#include <exosnap/engine/recorder_session.h>

#include <cstdint>
#include <vector>

namespace exosnap {

// The pattern the provider serves. Four quadrants of distinct brightness, a
// border, and one small asymmetric mark, so an analyzer can tell a moved overlay
// from a scaled one, from an absent one, and from a change in the screen source
// behind it. Deliberately not a flat colour, and deliberately not animated: a
// time code would be a visual generation of its own.
[[nodiscard]] std::vector<uint8_t> MakeStaticWebcamPattern(int width, int height);

class StaticWebcamFrameSource final : public exosnap::engine::WebcamFrameProvider {
  public:
    StaticWebcamFrameSource(int width, int height);

    // Contract, and the whole point of the class:
    //   * the first call and every later one return the same pixels,
    //   * out_generation is the same value on every call,
    //   * it never returns false.
    // A provider that bumps the generation per call, or serves different pixels
    // under one generation, breaks the measurement it exists to enable rather
    // than merely being untidy.
    bool TryGetFrame(int& out_width, int& out_height, std::vector<uint8_t>& out_bgra,
                     uint64_t& out_generation) override;

    // For the evidence record: which source produced the pixels, and at which
    // generation they were frozen.
    [[nodiscard]] uint64_t Generation() const noexcept {
        return generation_;
    }
    [[nodiscard]] int Width() const noexcept {
        return width_;
    }
    [[nodiscard]] int Height() const noexcept {
        return height_;
    }

  private:
    int width_ = 0;
    int height_ = 0;
    uint64_t generation_ = 1;
    std::vector<uint8_t> bgra_;
};

} // namespace exosnap

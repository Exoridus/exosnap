#pragma once

// Deterministic synthetic pictures and audio for Edit composition fixtures.
// Thumbnails and waveforms use their production presentation paths. The viewer
// uses a separate still image, independent of native decode and GPU rendering.

#include <exosnap/engine/waveform_cache.h>

#include <QImage>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace exosnap::visual_tests {

// One frame of the synthetic scene at `time_us`, `width` x `height`. `seed`
// picks the scene's palette and layout, so two sources look like two sources.
[[nodiscard]] QImage PaintEditSourceFrame(std::uint32_t seed, int width, int height, std::int64_t time_us);

// A stereo programme with speech-like bursts and pauses, for the waveform
// analysis of one source stream.
[[nodiscard]] std::unique_ptr<engine::WaveformAudioDecoder> MakeEditSourceAudio(std::uint32_t seed);

[[nodiscard]] bool WriteEditVisualAudioFixture(const std::filesystem::path& path, std::uint32_t seed,
                                               std::string& error);

// A stable seed for a source, from the first bytes of its content identity.
[[nodiscard]] std::uint32_t EditSourceSeed(const std::uint8_t* identity, std::size_t size);

} // namespace exosnap::visual_tests

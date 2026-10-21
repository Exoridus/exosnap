#pragma once

#include <exosnap/engine/audio_media_manifest.h>
#include <exosnap/engine/background_io.h>

#include <filesystem>

struct AVFormatContext;

namespace exosnap::engine {

struct MatroskaStreamConfig;

AudioMetadataReadResult ReadAudioMediaMetadata(const std::filesystem::path& path, const AVFormatContext* input,
                                               const std::function<bool()>& continue_io = {},
                                               const BackgroundIoAdmission& before_io = {},
                                               const std::atomic<bool>* cancelled = nullptr,
                                               const std::shared_ptr<const MediaReadCapability>& source = {},
                                               bool verify_content = true);
AudioMetadataReadResult
ReadAudioMediaMetadataForAdoption(const std::filesystem::path& path, const AVFormatContext* input,
                                  const std::shared_ptr<const MediaReadCapability>& source,
                                  const std::shared_ptr<const VerifiedMediaIdentity>& observed_proof,
                                  const std::function<bool()>& continue_io, const BackgroundIoAdmission& before_io,
                                  const std::atomic<bool>* cancelled);
Outcome<std::monostate> ValidateAudioManifestStreams(const AudioManifest1& manifest, const AVFormatContext* input);
Outcome<std::monostate> WriteAudioManifestMetadata(AVFormatContext* output, const AudioManifest1& manifest);
Outcome<std::string> SerializeAudioTrackProvenance(const AudioManifest1& manifest, const AudioTrackId& track_id);
AudioManifest1 DeriveAudioManifest(const AudioManifest1& source);
std::string DefaultAudioChannelLayout(uint32_t channels);
bool ConfigureMatroskaStreamCopy(const AVFormatContext* input, const std::string& output, MatroskaStreamConfig& config,
                                 std::vector<uint64_t>& track_numbers);

} // namespace exosnap::engine

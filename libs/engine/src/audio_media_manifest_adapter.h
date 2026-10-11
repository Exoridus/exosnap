#pragma once

#include <exosnap/engine/audio_media_manifest.h>

#include <filesystem>

struct AVFormatContext;

namespace exosnap::engine {

struct MatroskaStreamConfig;

AudioMetadataReadResult ReadAudioMediaMetadata(const std::filesystem::path& path, const AVFormatContext* input);
Outcome<std::monostate> ValidateAudioManifestStreams(const AudioManifest1& manifest, const AVFormatContext* input);
Outcome<std::monostate> WriteAudioManifestMetadata(AVFormatContext* output, const AudioManifest1& manifest);
Outcome<std::string> SerializeAudioTrackProvenance(const AudioManifest1& manifest, const AudioTrackId& track_id);
AudioManifest1 DeriveAudioManifest(const AudioManifest1& source);
std::string DefaultAudioChannelLayout(uint32_t channels);
bool ConfigureMatroskaStreamCopy(const AVFormatContext* input, const std::string& output, MatroskaStreamConfig& config,
                                 std::vector<uint64_t>& track_numbers);

} // namespace exosnap::engine

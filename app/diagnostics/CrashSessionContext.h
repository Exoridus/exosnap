#pragma once

// CrashSessionContext.h -- the encoder facts written into the crash session
// sidecar and attached to every dump.
//
// Both frontends have to produce byte-identical tokens: a dump is triaged by
// grouping on them, so "MKV" from one entry point and "Matroska" from the other
// would split one bug into two. The rules lived in an anonymous namespace inside
// MainWindow.cpp, where the second frontend could only have copied them.

#include <capability/capability_set.h>
#include <crash_capture/crash_capture.h>

#include <string>

namespace exosnap::diagnostics {

// Compact, allowlisted tokens for the sidecar and the crash report. The
// capability ToString() helpers are verbose ("Matroska", "AV1 NVENC", "AAC");
// the crash facts want the short form. Prefixed Crash* to avoid colliding with
// the std::wstring ContainerToken in RecordingPreset.h used for filename
// building.
[[nodiscard]] std::string CrashContainerToken(capability::Container container);
[[nodiscard]] std::string CrashVideoCodecToken(capability::VideoCodec codec);
[[nodiscard]] std::string CrashAudioCodecToken(capability::AudioCodec codec);

// The full context for the currently configured output. `encoder_backend` is
// the nvenc baseline every shipped video codec runs on today.
[[nodiscard]] crash_capture::SessionContext MakeCrashSessionContext(capability::Container container,
                                                                    capability::VideoCodec video_codec,
                                                                    capability::AudioCodec audio_codec);

} // namespace exosnap::diagnostics

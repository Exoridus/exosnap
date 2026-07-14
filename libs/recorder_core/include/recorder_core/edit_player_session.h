#pragma once

// EditPlayerSession -- top-level orchestrator for the Edit-page video player
// (docs/superpowers/specs/2026-07-14-edit-video-player-design.md). Owns one
// EditPlayerEngine (decode) and, when the file has an audio stream, one
// WasapiAudioRenderer (playback + master clock). UI-agnostic per CLAUDE.md.
//
// Ownership split with the app layer: this class does NOT know about Qt
// timers. When HasAudioStream() is false, the caller (EditExportPage) is
// responsible for driving playback position from its own existing wall-clock
// timer and calling SeekTo() to request frames at that position -- this
// mirrors the design's documented fallback (the existing preview_elapsed_/
// onPreviewTick logic becomes the real fallback path, not a Qt concern this
// class needs to duplicate).
//
// Threading contract: Open/Close/Play/Pause/SeekTo are expected to be driven
// from a single caller thread (the app layer's UI thread), same as the rest
// of recorder_core's session-shaped classes. SetOnFrameReady's callback is
// invoked from an internal thread (never the caller's thread) and may be
// invoked from either the continuous-playback decode thread or a scrub-seek
// worker thread, but never both at once -- this class enforces mutual
// exclusion between EditPlayerEngine's DecodeFrameAt (scrub) and its
// playback decode thread, since the engine itself documents that as the
// caller's responsibility, not something it guards internally.

#include <recorder_core/edit_player_engine.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace recorder_core {

class EditPlayerSession {
  public:
    EditPlayerSession();
    ~EditPlayerSession();

    EditPlayerSession(const EditPlayerSession&) = delete;
    EditPlayerSession& operator=(const EditPlayerSession&) = delete;

    // Opens `path` (the MKV edit master) and, if it has an audio stream,
    // initializes the WASAPI render client. Returns false with a message in
    // out_error if the file cannot be opened (audio-renderer init failure is
    // non-fatal here -- logged and the session falls back to HasAudioStream()
    // == false, matching "no audio stream" behavior, per the design's stance
    // that a broken audio device should degrade to silent video, not break
    // the whole preview).
    bool Open(const std::filesystem::path& path, std::string& out_error);
    void Close();

    [[nodiscard]] bool HasAudioStream() const noexcept;

    // Sets the callback invoked (from an internal thread -- NOT the caller's
    // thread) whenever a new frame is ready to display, during both
    // continuous playback and single-frame scrub/trim-drag seeks.
    void SetOnFrameReady(std::function<void(DecodedVideoFrame)> callback);

    // Starts continuous playback (decode thread + audio renderer, if
    // present) from the current position. No-op if not open.
    void Play();

    // Pauses continuous playback. No-op if not open or not playing.
    void Pause();

    // Requests a single frame at target_us (scrub / trim-handle-drag path).
    // If currently playing, this pauses playback first (matching the
    // existing UI contract: scrubbing pauses, resume-on-release is the
    // caller's job, same as today's onScrubStarted/onScrubFinished). A newer
    // SeekTo() call supersedes an in-flight older one.
    void SeekTo(int64_t target_us);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recorder_core

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
#include <optional>
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
    // present) from start_us. No-op if not open. The caller is responsible
    // for tracking "current position" (this class holds no position state of
    // its own beyond what a live playback/seek thread is doing) -- pass the
    // caller's own last-known position (e.g. after a pause or a scrub) to
    // resume from there; pass 0 to start from the beginning.
    void Play(int64_t start_us = 0);

    // Pauses continuous playback. No-op if not open or not playing.
    void Pause();

    // Requests a single frame at target_us (scrub / trim-handle-drag path).
    // If currently playing, this pauses playback first (matching the
    // existing UI contract: scrubbing pauses, resume-on-release is the
    // caller's job, same as today's onScrubStarted/onScrubFinished). A newer
    // SeekTo() call supersedes an in-flight older one.
    void SeekTo(int64_t target_us);

    // Pulls the next frame to display, paced by the audio master clock.
    // Valid only while HasAudioStream() is true -- returns nullopt
    // unconditionally otherwise (the caller must drive the no-audio
    // fallback by calling SeekTo() itself once per tick instead; see
    // docs/superpowers/specs/2026-07-14-edit-video-player-pacing-design.md).
    // Also returns nullopt if no frames are currently queued, or if the
    // clock hasn't advanced to the next queued frame's timestamp yet.
    // Intended to be polled from the caller's own UI-thread timer.
    [[nodiscard]] std::optional<DecodedVideoFrame> PollFrame();

    // Current playback position derived from the audio master clock (0 if no
    // audio stream). While paused, holds steady at the position playback
    // was paused at -- the underlying audio clock (WasapiAudioRenderer::
    // FramesPlayed()) freezes at the paused position once Stop() is called, so
    // this naturally holds rather than needing a separate "not playing" check.
    // Kept separate from PollFrame() because the caller needs to advance the
    // displayed position on every tick, even the ones where PollFrame()
    // itself returns nullopt (clock hasn't reached the next frame yet).
    [[nodiscard]] int64_t CurrentPositionMs() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recorder_core

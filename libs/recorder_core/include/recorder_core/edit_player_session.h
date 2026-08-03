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
// exclusion between EditPlayerEngine's DecodeFrameAtRaw (scrub) and its
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

    // The opened clip's own frame rate in fps, or 0.0 when unknown -- the
    // caller drives its presentation timer from this rather than from a fixed
    // interval, so a clip recorded at any rate plays at that rate.
    [[nodiscard]] double VideoFrameRate() const noexcept;

    // Sets the callback invoked (from an internal thread -- NOT the caller's
    // thread) with each newly decoded RAW video frame, during both continuous
    // playback and single-frame scrub/trim-drag seeks -- the GPU render path
    // (docs/superpowers/specs/2026-08-03-editor-playback-gpu-render-design.md).
    // Delivered directly, with no intermediate queue: pacing/drop decisions
    // for this path live in EditPlayerRenderer::PresentFrame (present-gated
    // against CurrentPositionMs(), which stays the clock source of truth),
    // not in this class.
    void SetOnFrameReady(std::function<void(RawDecodedVideoFrame)> callback);

    // Starts continuous playback (decode thread + audio renderer, if
    // present) from start_us. No-op if not open. The caller is responsible
    // for tracking "current position" (this class holds no position state of
    // its own beyond what a live playback/seek thread is doing) -- pass the
    // caller's own last-known position (e.g. after a pause or a scrub) to
    // resume from there; pass 0 to start from the beginning.
    //
    // Drives the engine's RAW playback decode (EditPlayerEngine::
    // StartPlaybackDecode) and delivers through SetOnFrameReady's callback --
    // see that method's doc comment.
    void Play(int64_t start_us = 0);

    // Pauses continuous playback. No-op if not open or not playing.
    void Pause();

    // Requests a single frame at target_us (scrub / trim-handle-drag path).
    // If currently playing, this pauses playback first (matching the
    // existing UI contract: scrubbing pauses, resume-on-release is the
    // caller's job, same as today's onScrubStarted/onScrubFinished). A newer
    // SeekTo() call supersedes an in-flight older one.
    //
    // Drives the engine's RAW seek (EditPlayerEngine::DecodeFrameAtRaw) and
    // delivers through SetOnFrameReady's callback -- see that method's doc
    // comment.
    void SeekTo(int64_t target_us);

    // Current playback position derived from the audio master clock (0 if no
    // audio stream). While paused, holds steady at the position playback
    // was paused at -- the underlying audio clock (WasapiAudioRenderer::
    // FramesPlayed()) freezes at the paused position once Stop() is called, so
    // this naturally holds rather than needing a separate "not playing" check.
    // Called once per presentation tick from the caller's UI thread, alongside
    // ClockSnapshotUs() below.
    [[nodiscard]] int64_t CurrentPositionMs() const noexcept;

    // Thread-safe atomic snapshot of the playback clock in absolute media
    // time (microseconds), or a negative value when no clock is available --
    // the SAME snapshot EditPlayerEngine's own decode thread already reads
    // via the current_media_time_us callback Play() passes to
    // StartPlaybackDecode. Refreshed as a side effect of CurrentPositionMs()
    // (called once per presentation tick from the caller's UI thread). Unlike
    // CurrentPositionMs(), which calls through to WasapiAudioRenderer::
    // FramesPlayed(), a single-caller-thread API -- this is safe to call from
    // ANY thread: the GPU render path's present-gate (EditPlayerRenderer::
    // PresentFrame, fed via EditPlayerSurface::updateClockUs from the
    // caller's UI thread) needs exactly that, so it never touches this
    // class's own single-thread-only members directly.
    [[nodiscard]] int64_t ClockSnapshotUs() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace recorder_core

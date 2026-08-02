# Timeline: real thumbnails, real tracks

Status: approved 2026-08-02.

## Problem

The edit timeline draws 72 bars whose heights come from `12.0 + (sin(i * 1.7) + 1.0) * 14.0`
(`EditTimeline.cpp:243-258`). The shape is identical for every recording — it reads as
information about the clip and is none. That is worse than an empty strip.

Separately, a recording with system and microphone audio on separate tracks plays back with
exactly one of them: the player calls `av_find_best_stream` once
(`edit_player_engine.cpp:485`) and opens a single audio stream. Nothing in the UI shows that a
second track exists, and nothing lets you hear both.

## Target

```
┌ 00:00                                                              04:18 ┐
│ ▓▓│[img][img][img][img][img][img][img][img][img][img][img]│▓▓          │  video, 40 px
│ ▓▓│ System                                                │▓▓          │  audio, 20 px
│ ▓▓│ Microphone                                            │▓▓          │  audio, 20 px
└──────────────────────────────────────────────────────────────────────────┘
```

Trim handles, markers and the playhead span all rows — one trim applies to the whole clip.

## 1 · Thumbnails

A second `EditPlayerEngine` on a worker thread. It has no global state (only a `thread_local`
error string) and owns no audio device, so a second instance alongside playback is safe. The
*session* is not safe to duplicate — it opens its own WASAPI renderer.

Per visible tile the worker calls `DecodeFrameAt(target_us)` (`edit_player_engine.h:107`,
synchronous), scales the result to row height, and **releases the full-size frame before
decoding the next**. `DecodedFrameToQImage` wraps the decoder's buffer without copying
(`EditExportPage.cpp:633-642`), so holding the unscaled `QImage` holds ~15 MB per tile at 1440p.
Finished tiles are posted to the UI thread one at a time; the strip fills in progressively.

Tile count follows the track width: row height 40 px, tile width from the recording's aspect
ratio (~71 px at 16:9, narrower for portrait, wider for ultrawide). Each tile is drawn
**left-aligned at its timestamp** — a tile answers "from here on it looks like this", which is
the question that matters when trimming.

Targets are the keyframes nearest the tile positions, taken from `keyframe_timestamps_`
(`EditExportPage.cpp:488-495`, cheap: it reads the Matroska cues, no decoding).

The timeline needs two things it does not have today: the clip path, and a `resizeEvent`
(debounced — a resize changes the tile count).

While tiles are missing the row stays empty. No spinner, no placeholder pattern.

## 2 · Track names when muxing

`matroska_stream_writer.cpp:368-416` writes no `KaxTrackName` and no `KaxTrackLanguage` — the
mapping is positional (track 1 video, 2+n audio). The semantics exist only in memory at record
time (`AudioSourceKind` in `audio_track_model.h:9-57`), and a track can merge several sources.

Each audio track gets a `KaxTrackName` derived from its resolved sources: `System`,
`Microphone`, `Application`, and for merged tracks `System + Microphone`. Names are English and
untranslated, consistent with the rest of the shipped strings.

Recordings written before this change carry no names. They fall back to `Audio 1`, `Audio 2` —
never to a guess based on track order.

## 3 · Playing every audio track

`EditPlayerEngine::Open` opens one audio stream. It now opens all of them, one decoder each, and
sums them before the renderer. Audio decoding is cheap next to video — this is not the path
carrying the known preview bottleneck.

Summing two loud tracks clips. The mix applies soft limiting rather than dividing by track
count, so a single active track keeps its level instead of being halved against silence.

The engine exposes what it found:

```cpp
struct AudioTrackDescription {
    int stream_index;
    std::string name;   // from KaxTrackName; empty for older recordings
};
[[nodiscard]] std::vector<AudioTrackDescription> AudioTracks() const;
```

This is the seam between the engine and the timeline. Callers must tolerate an empty name and an
empty vector. A track whose decoder fails to open is still listed — the question the list answers
is what the recording carries, and omitting it would misrepresent the recording rather than the
failure.

`PlaybackDeliversAudio()` comes to mean **at least one** track delivers audio, not all of them.
The flag exists so a caller pacing video off the audio clock does not wait forever on a clock
nothing advances, and that clock advances just as well on one track as on three. A track whose
resampler fails to build is dropped from the run and logged; the remaining tracks still play.

## 4 · Multi-track rows

The timeline grows from one strip to a stack: one video row of 40 px, then one 20 px row per
audio track. Total height goes from 94 px to about 126 px at two tracks; the player above
shrinks accordingly via the existing `updatePlayerHeight()`.

**Audio rows carry a label and a fill — no waveform.** A peak envelope over a 2.5-hour recording
means decoding the entire soundtrack, which costs minutes at open. An *approximated* waveform
would be the same lie this change removes. A real envelope stays possible once something else
needs the full audio read.

Audio rows share a height budget so three tracks cannot squeeze the player out at the 700 px
minimum window height. The rows do not scroll.

`xForMs()` / `msForX()` keep their signatures — tests call them directly
(`test_edit_export_page.cpp:384-386`).

## Testing

- Tile positions map to the expected timestamps at several widths, and the tile count follows the
  aspect ratio (portrait yields more tiles than ultrawide at equal width).
- A tile request that fails to decode leaves the row empty and does not abort the remaining tiles.
- The worker releases full-size frames: decoding N tiles does not retain N full frames.
- Track names round-trip: mux a two-track recording, read the names back.
- Merged sources produce the combined name; a recording without names falls back to `Audio n`.
- The mixer sums two tracks; one silent track leaves the other's level unchanged.
- Timeline height follows the track count, and `xForMs`/`msForX` still agree after the rework.

## Visual harness

`edit-main` and `edit-trimmed` gain real tiles. New: `edit-timeline-multitrack` (two named audio
rows) and `edit-timeline-loading` (tiles still arriving).

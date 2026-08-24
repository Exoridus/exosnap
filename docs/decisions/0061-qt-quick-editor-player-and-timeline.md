# ADR 0061: Qt Quick Editor — Player Transport and Timeline Design

## Status

Accepted.

## Context

ADR 0058 established the Record preview bridge: a D3D11 source texture is shared as an NT handle,
opened on Qt Quick's own D3D11 device, guarded by a keyed mutex, copied on the GPU, and imported via
`QSGD3D11Texture::fromNative` into a `QSGImageNode`. It is fast, it removed the preview's native
child HWND, and the obvious move for the editor was to reuse it.

That does not work, and the reason is ownership rather than effort.

The editor decodes through `EditPlayerSession`. The engine attempts D3D11VA, but **FFmpeg creates its
own internal D3D11 device that the application never sees**, and then immediately reads frames back
with `av_hwframe_transfer_data` plus an NV12/P010 → YUV420P deinterleave. What reaches the app is
`RawDecodedVideoFrame`: **CPU planes only** — `y/u/v_plane` with strides, a
`DecodedPixelFormat` of `Yuv420P8`/`Yuv420P10`/`Yuv444P8`, an `is_pq_source` flag, colour matrix and
range, and a `std::shared_ptr<void> backing_frame` whose deleter is `av_frame_free`.

There is no shared texture to open. Three further differences compound it:

- **Pull versus push.** The preview polls the keyed mutex at scene cadence with a zero timeout; a
  miss is a non-event. The editor is push-driven from the decode/seek thread and *deliberately drops
  late frames* against a media clock. That gate is playback policy, not an optimisation.
- **Conversion.** The preview converts BGRA8/RGB10A2 → RGBA. The editor needs planar YUV at 8 and
  10 bits, 4:2:0 and 4:4:4, with colour matrix, range, and HDR10 PQ tone-mapping — that is
  `exosnap::engine::EditFrameGpuConverter`.

Separately, the Widgets timeline is a 30 KB custom-painted widget, which invites a custom
`QQuickItem` with a hand-built scene graph. Measurement says otherwise.

## Decision

### Player

Build `ExoEditPlayerItem` as a **sibling of `ExoPreviewItem` that reuses its skeleton, not its
transport**. Reused verbatim: `updatePaintNode` with a `QSGImageNode` under a `QSGClipNode` including
the rounded-corner clip geometry, generation counters guarding stale render-state postbacks,
`beginExternalCommands()`/`endExternalCommands()` around every native pass, and
`sceneGraphInvalidated`/`sceneGraphInitialized` reconnection.

Replaced: the payload becomes a single-slot newest-wins mailbox of `RawDecodedVideoFrame` under a
mutex. **The frame's own `backing_frame` shared_ptr is the cross-thread lifetime model** — no
additional ownership scheme is introduced. Upload runs `EditFrameGpuConverter`, which takes a
*borrowed* `ID3D11Device*`/`ID3D11DeviceContext*` and therefore drops directly onto Qt's device
obtained through `QSGRendererInterface::DeviceResource`. Its BGRA8 output is chained through the
existing `QuickPreviewRgbaConverter` to the RGBA8 that `QSGD3D11Texture::fromNative` imports.

The present gate survives verbatim: a frame whose PTS is already behind the published clock is
dropped **before any GPU work**.

`EditPlayerSurface` and `EditPlayerRenderer` — roughly 800 lines carrying their own D3D11 device,
render thread, swap chain, shaders and a GDI placeholder sprite — are **deleted rather than ported**.
The placeholder becomes a QML `Text`.

**No child HWND, and it is verified rather than assumed.** `--hwnd-audit` run with the editor overlay
open reports zero child windows. This matters beyond the editor: the withdrawn native window chrome
failed because `WM_NCHITTEST` is only asked of the window owning the pixel under the cursor, and a
native child was owning the title-bar band. Any `QQuickWidget` or `createWindowContainer` shim for
the editor player would restore exactly that barrier and is rejected.

### Timeline

**Plain declarative QML — no custom `QQuickItem`, no `Canvas`, no `Shape`.** The measured data scale
does not justify a scene-graph implementation:

| Item | Realistic count |
|---|---|
| Thumbnail tiles | ~8–20, driven by track *width*, not clip duration |
| Audio rows | 0–3, label-only |
| Markers | typically <20 (hard cap 10 000) |
| Waveform points | **0 — waveforms are forbidden by the product spec** |

A `Repeater` of `Image` delegates for tiles and a `Repeater` of `Rectangle`s for markers is the right
weight. Two guards make it safe: trim handles are `MouseArea` + `Rectangle` only — never Qt Quick
Controls — and markers are thinned **in C++** to one per pixel column with a hard render cap, so the
10 000-marker ceiling can never become 10 000 delegates.

Tiles are exposed through a `QAbstractListModel` plus a `QQuickImageProvider` keyed by
`runId/index`; a `QImage` never travels in a model role. `tilesExpected`/`tilesReady` replace the
"Generating previews…" condition that used to be recomputed inside `paintEvent`, which QML cannot do.

### Two ported defects, fixed in the contract instead

1. **Trim lived twice** — authoritative in µs and snapped on the page, transient in ms and unsnapped
   in the timeline widget, with the page overwriting the widget after release. Trim now lives
   **once**: µs, always snapped, on `EditSessionAdapter`. QML holds only a between-press-and-release
   pointer preview, which is pointer position and not stored trim.
2. **`exportRunning` lived twice and diverged** — cancel declared the run finished while the remux
   thread was still winding down, and the `join()` was deferred to the next export, so a Retry
   immediately after a Cancel blocked the GUI thread. `exportRunning` now lives once on
   `EditExportAdapter`; cancel moves to a `Cancelling` state that is still "running" until the thread
   reports back, and the `join()` happens in the GUI-thread completion handler.

Blocking calls that were on the GUI thread are now async: the keyframe index read, the player session
open, and the export join. Export progress, which fires per video packet, is throttled to
whole-percent changes before crossing the thread boundary.

## Consequences

- The editor has no native child window, which keeps the premise for native window chrome intact.
- Two long-standing defects (dual trim, dual export state) are resolved as a side effect of defining
  the contract, rather than being carried into a second frontend.
- The editor player's D3D11 path has **not yet been exercised against a real decoded clip** — every
  visual state so far used an in-memory fixture with no decodable master. That is the next
  verification step and requires a real recording on the machine.
- Markers render in a neutral secondary tone because `QuickThemeTokens` exposes no second accent.
  Adding an `accent2` token is the clean fix and is deliberately deferred rather than hardcoded.
- Nothing yet hands a finished recording to `EditSessionAdapter::setEditContext`; the
  `RecordingCoordinator` → `EditContext` path is still Widgets-only, so the overlay is currently
  reachable only through the visual harness.

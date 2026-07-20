# DXGI-Active Webcam Magnifier Compositing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the hover-to-enlarge webcam PiP magnifier (merged 2026-07-20, commit `ad47e62`) actually visible and clickable during the live DXGI preview — which is the path real recording and monitor/window preview run on essentially 100% of the time — instead of being silently limited to the QImage software-paint fallback that only a handful of test/idle scenarios use. This resolves the boundary `KNOWN_LIMITATIONS.md` currently documents ("the magnifier cannot currently be pixel-verified... every Record-page scenario selects a capture target, which flips `dxgi_active_` true").

**Architecture:** No new architecture — this extends the existing "OSD sprite" pattern (`DxgiPreviewRenderer::SetOsdSprite`/`RenderOsdSprites`), already used to composite the preview's meta/stats text rows into the DXGI swapchain, to also carry the magnifier hover icon, the restore icon, and a dim scrim. The enlarged floating webcam view reuses the *existing* `SetWebcamOverlayState`/`SetWebcamOverlayFrame` webcam-PiP-compositing path, just fed an animated (interpolated) placement rect instead of the confirmed `webcam_rect_norm_` — mirroring exactly what the Qt-paint fallback's `paintEvent` already computes, never touching the persisted placement. Mouse input already reaches `PreviewSurface`'s normal Qt event handlers while the DXGI child HWND is on screen (its `WM_NCHITTEST` returns `HTTRANSPARENT`, making it click-through by design — confirmed by reading `DxgiPreviewRenderer::ChildWndProc`), so no input-plumbing work is needed, only removing the two `!dxgi_active_` guards that currently suppress the feature outright and adding the DXGI-side rendering the guards were standing in for.

**Tech Stack:** C++20, Qt 6 widgets, Direct3D 11 (existing `DxgiPreviewRenderer` swapchain compositor), GoogleTest.

## Global Constraints

- Never mutate `webcam_rect_norm_` (the confirmed, persisted, WYSIWYG placement the recording engine reads) for magnifier animation purposes. Only the *arguments passed to* `DxgiPreviewRenderer::SetWebcamOverlayState` for the preview's own swapchain may reflect the animated/enlarged rect — this mirrors the existing Qt-paint `paintEvent`'s own rule ("Preview-only and fully transient: never touches webcam_rect_norm_").
- Reuse existing rect-computation and icon-painting helpers (`webcamMagnifierIconMappedRect()`, `magnifierIconRect()`, `webcamEnlargedTargetRect()`, `paintMagnifierIcon()`) rather than re-deriving geometry — they are already implemented and (for the rect math) already unit-tested against the Qt-paint path; the DXGI path must produce pixel-identical placement, not a second slightly-different formula.
- Never interact with a running ExoSnap instance (no mouse/keyboard synthesis, no window automation). Use ctest widget tests and, where feasible, the `--visual-test` render harness. A "start once, confirm no crash, close" check is fine if a QSS/theme file is touched (not expected here).
- Full build (not just `--target exosnap`) before running tests; this project's convention.
- `KNOWN_LIMITATIONS.md`'s magnifier note must be updated once this lands (Task 2, since that's when the feature becomes fully usable) to state the limitation is resolved, per this project's rule that visible-behavior changes update the relevant spec/doc.
- Per explicit user authorization for this cleanup/feature pass: merge each task directly to `main` after it's green — no PR required.

---

### Task 1: Composite the magnifier hover icon into the DXGI preview

**Files:**
- Modify: `app/services/DxgiPreviewRenderer.h:89` (bump `kOsdSpriteSlots`), `:82-92` (doc comment)
- Modify: `app/ui/widgets/PreviewSurface.h` (add `syncMagnifierIconToDxgi()` private method declaration, near `syncOsdToDxgi()` at line 260)
- Modify: `app/ui/widgets/PreviewSurface.cpp` (implement the new method; call it; relax the hover-tracking guard)
- Test: `app/tests/test_preview_surface_webcam.cpp`

**Interfaces:**
- Consumes: `DxgiPreviewRenderer::SetOsdSprite(int slot, const uint8_t* bgra, int width, int height, int stride, int destX, int destY)` (existing, unchanged signature — just used with a new slot index) — `SetOsdSprite(slot, nullptr, 0, 0, 0, 0, 0)` clears a slot. `PreviewSurface::webcamMagnifierIconMappedRect() const -> QRect` (existing, already returns `{}` when the icon shouldn't show). `PreviewSurface::magnifierIconRect(const QRectF& pip_rect) const -> QRectF` and `PreviewSurface::paintMagnifierIcon(QPainter&, const QRectF& icon_rect, bool enlarged) const` (existing).
- Produces: `void PreviewSurface::syncMagnifierIconToDxgi()` — safe to call whenever hover/selection state changes; no-ops if `!dxgi_active_ || !dxgi_renderer_`.

**Background — read before starting:** The DXGI child HWND (`DxgiPreviewRenderer`'s `childHwnd_`) is click-through by design: its `ChildWndProc` (`app/services/DxgiPreviewRenderer.cpp:626-632`) returns `HTTRANSPARENT` from `WM_NCHITTEST`, so all mouse input already falls through to the parent Qt `PreviewSurface` HWND normally — this is *not* something this task needs to touch. The only reason the magnifier icon doesn't appear today is that nothing pushes it into the DXGI swapchain; `mouseMoveEvent`'s hover-tracking (`app/ui/widgets/PreviewSurface.cpp:1304`) is wrapped in `if (!dxgi_active_ && ...)` purely because there was nothing to show yet.

- [ ] **Step 1: Bump the sprite slot count and its doc comment**

In `app/services/DxgiPreviewRenderer.h`, change:

```cpp
    static constexpr int kOsdSpriteSlots = 2;
```

to:

```cpp
    // Slot 0/1: preview meta/stats text rows (PreviewSurface::syncOsdToDxgi).
    // Slot 2: webcam-magnifier hover/restore icon (PreviewSurface::syncMagnifierIconToDxgi).
    static constexpr int kOsdSpriteSlots = 3;
```

(This one constant drives both the `osdSprites_[kOsdSpriteSlots]` array size and the loop bounds in `RenderOsdSprites()` — no other change needed in the `.h` or `.cpp` for the slot itself.)

- [ ] **Step 2: Declare the new sync method**

In `app/ui/widgets/PreviewSurface.h`, add immediately after the existing `void syncOsdToDxgi();` declaration (line 260):

```cpp
    // Pushes the magnifier hover/restore icon (or clears it) into the DXGI
    // swapchain as OSD sprite slot 2. Mirrors paintMagnifierIcon()'s Qt-paint
    // rendering exactly, so the icon looks identical on both paths. Safe to call
    // whenever hover/selection/enlarge state changes; no-ops if the DXGI preview
    // isn't running.
    void syncMagnifierIconToDxgi();
```

- [ ] **Step 3: Implement it**

In `app/ui/widgets/PreviewSurface.cpp`, add the new method right after `syncOsdToDxgi()` (after its closing brace, currently around line 845):

```cpp
// Rasterizes the magnifier hover/restore icon into a transparent image at its
// mapped rect and hands it to the renderer as OSD sprite slot 2. Reuses the
// exact same rect math and paint routine the Qt fallback uses, so the icon is
// pixel-identical on both paths — only the destination (sprite vs. QPainter)
// differs.
void PreviewSurface::syncMagnifierIconToDxgi() {
    if (!dxgi_active_ || !dxgi_renderer_)
        return;

    const QRect icon_rect = webcamMagnifierIconMappedRect();
    if (icon_rect.isEmpty()) {
        dxgi_renderer_->SetOsdSprite(2, nullptr, 0, 0, 0, 0, 0);
        return;
    }

    const qreal dpr = devicePixelRatioF();
    QImage img(icon_rect.size() * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // paintMagnifierIcon() draws in the surface's own coordinate space; translate
    // so the icon lands at (0,0) in this offscreen image, matching icon_rect's origin.
    painter.translate(-icon_rect.topLeft());
    paintMagnifierIcon(painter, QRectF(icon_rect), webcam_enlarged_ || magnify_progress_ > 0.0);
    painter.end();

    img = img.convertToFormat(QImage::Format_ARGB32);
    dxgi_renderer_->SetOsdSprite(2, img.constBits(), img.width(), img.height(),
                                 static_cast<int>(img.bytesPerLine()), qRound(icon_rect.x() * dpr),
                                 qRound(icon_rect.y() * dpr));
}
```

- [ ] **Step 4: Enable hover tracking during DXGI-active preview, and sync the icon on every relevant state change**

In `mouseMoveEvent` (`app/ui/widgets/PreviewSurface.cpp:1304`), change:

```cpp
    if (!dxgi_active_ && webcam_enabled_ && magnify_progress_ <= 0.0) {
        const bool now_hovered = webcamPixelRect().contains(pos);
        if (now_hovered != webcam_hovered_) {
            webcam_hovered_ = now_hovered;
            update();
        }
    }
```

to:

```cpp
    if (webcam_enabled_ && magnify_progress_ <= 0.0) {
        const bool now_hovered = webcamPixelRect().contains(pos);
        if (now_hovered != webcam_hovered_) {
            webcam_hovered_ = now_hovered;
            update();
            syncMagnifierIconToDxgi();
        }
    }
```

(Only the guard changes — `!dxgi_active_ &&` is removed — plus the new call. `update()` stays: it's still needed to repaint the Qt fallback path when `!dxgi_active_`, and is a harmless no-op-ish call when the DXGI child HWND occludes the widget.)

Also call `syncMagnifierIconToDxgi()` once, right after `syncWebcamOverlayToDxgi()` and `syncOsdToDxgi()`, at the two places `tryStartDxgiPreview` and `tryStartDxgiPushedPreview` already call those two (so the icon's cleared/hidden state is established the moment DXGI preview starts, consistent with a fresh `webcam_hovered_ == false`). And call it wherever `webcam_selected_` changes in `mousePressEvent` (both places that currently call `syncWebcamOverlayToDxgi()` after toggling `webcam_selected_`, around lines 1277 and 1285) — selecting the PiP hides the hover icon via the existing `!webcam_hovered_ || show_chrome` check inside `webcamMagnifierIconMappedRect()`, so the DXGI sprite needs to be told to clear too.

Also call it from `setWebcamOverlayEnabled(false)`'s hard-reset block (`app/ui/widgets/PreviewSurface.cpp:600-605`, the block that already zeroes `webcam_hovered_`/`webcam_enlarged_`/`magnify_progress_`) so disabling the webcam clears a stale icon sprite instead of leaving one frozen on screen.

- [ ] **Step 5: Full build**

- [ ] **Step 6: Write and run tests**

This is a genuinely open test-design question — resolve it yourself and note the reasoning in your report, don't guess silently. Two known-working precedents exist in this codebase:

1. `app/tests/test_preview_surface_webcam.cpp`'s existing fixture (`PreviewSurfaceWebcamTest`) never starts a real DXGI preview — it drives `PreviewSurface` entirely through the Qt-paint fallback (`setLiveFrame`/`setWebcamFrame`), so `dxgi_active_` is always `false` there today. That fixture is the right place to add a cheap regression test that `syncMagnifierIconToDxgi()` early-returns safely (doesn't crash, doesn't touch `dxgi_renderer_`) when `dxgi_active_` is `false` — but that alone does NOT prove the new code path actually works when DXGI *is* active, which is the entire point of this task.
2. `app/tests/test_dxgi_preview_pushed_source.cpp` (added in the same PR that introduced the magnifier) is a **live-hardware test**: it creates a real `DxgiPreviewRenderer`, initializes it against a hidden test window (`CreateHiddenTestWindow()`), and pushes real frames — with a documented `GTEST_SKIP()` for headless/GPU-less CI runners (see that file's header comment and its `CMakeLists.txt` `LABELS`). This is the precedent for actually exercising `dxgi_active_ == true` behavior.

Decide whether to (a) extend `test_preview_surface_webcam.cpp` with a `dxgi_active_`-false-only regression test plus a *separate* new live-hardware test file/section following pattern 2 that drives a real `PreviewSurface` through `tryStartDxgiPreview` (or `tryStartDxgiPushedPreview`) and asserts the sprite gets set/cleared as hover state changes, or (b) some other approach that actually proves the DXGI path works — but don't settle for only testing the `dxgi_active_ == false` early-return and calling the task done, since that leaves the actual new behavior unverified. If after investigating you're not confident which is right, report NEEDS_CONTEXT and ask rather than guessing.

Whatever you land on, run it: `ctest --test-dir <build-dir> -R "preview_surface_webcam|dxgi_preview_pushed_source" -V`. Expected: PASS (with GTEST_SKIP acceptable only for the genuinely-headless-runner case, not as a way to avoid writing the real assertion).

- [ ] **Step 7: Visual check**

If the existing `--visual-test` Record-page webcam scenarios really do run with `dxgi_active_ == true` (confirmed empirically by the agent that just landed the magnifier code, contradicting a stale code comment that claims the opposite — verify this yourself rather than trusting either source), render one of them (e.g. `record-webcam-default-pip`) with the mouse-hover-simulation the harness supports, if any, or at minimum confirm the icon does NOT appear when it shouldn't (not hovered) and note in your report whether the harness can actually simulate hover for a real pixel check, or whether that remains a manual-verification item.

- [ ] **Step 8: Commit**

```bash
git add app/services/DxgiPreviewRenderer.h app/ui/widgets/PreviewSurface.h app/ui/widgets/PreviewSurface.cpp app/tests/test_preview_surface_webcam.cpp
git commit -m "Composite the webcam magnifier hover icon into the DXGI preview (3rd OSD sprite slot)"
```

(Add `app/tests/test_dxgi_preview_pushed_source.cpp` too if Step 6 extended it.)

- [ ] **Step 9: Merge directly to main and push**

---

### Task 2: Enable click-to-enlarge and composite the floating enlarged view in DXGI mode

**Files:**
- Modify: `app/services/DxgiPreviewRenderer.h` (bump `kOsdSpriteSlots` again, 3→4, for the dim scrim)
- Modify: `app/ui/widgets/PreviewSurface.h` (add `syncEnlargedWebcamToDxgi()` private method declaration)
- Modify: `app/ui/widgets/PreviewSurface.cpp` (implement it; wire into the animation tick and click handling; relax the second `!dxgi_active_` guard)
- Modify: `KNOWN_LIMITATIONS.md` (remove/update the magnifier boundary note added by commit `ad47e62`)
- Test: `app/tests/test_preview_surface_webcam.cpp` (and/or the live-hardware test file, per whatever Task 1 established)

**Interfaces:**
- Consumes: `Task 1`'s `syncMagnifierIconToDxgi()`. `DxgiPreviewRenderer::SetWebcamOverlayState(bool enabled, bool selected, float nx, float ny, float nw, float nh, bool mirror, float opacity, const recorder_core::ChromaKeyParams& chroma)` and `SetWebcamOverlayFrame(...)` (existing, unchanged — called with the *animated* rect instead of `webcam_rect_norm_` while `magnify_progress_ > 0`). `PreviewSurface::webcamEnlargedTargetRect() const -> QRectF`, `webcamPixelRect() const -> QRectF` (existing).
- Produces: `void PreviewSurface::syncEnlargedWebcamToDxgi()` — pushes the dim scrim sprite (slot 3) and the interpolated webcam overlay rect; called every animation tick while `magnify_progress_ > 0` and once more on settle/collapse to restore the normal `syncWebcamOverlayToDxgi()` state.

- [ ] **Step 1: Bump the sprite slot count again**

In `app/services/DxgiPreviewRenderer.h`, change the Task-1 result:

```cpp
    // Slot 0/1: preview meta/stats text rows (PreviewSurface::syncOsdToDxgi).
    // Slot 2: webcam-magnifier hover/restore icon (PreviewSurface::syncMagnifierIconToDxgi).
    static constexpr int kOsdSpriteSlots = 3;
```

to:

```cpp
    // Slot 0/1: preview meta/stats text rows (PreviewSurface::syncOsdToDxgi).
    // Slot 2: webcam-magnifier hover/restore icon (PreviewSurface::syncMagnifierIconToDxgi).
    // Slot 3: webcam-magnifier dim scrim, full-panel (PreviewSurface::syncEnlargedWebcamToDxgi).
    static constexpr int kOsdSpriteSlots = 4;
```

- [ ] **Step 2: Declare the new sync method**

In `app/ui/widgets/PreviewSurface.h`, add after `syncMagnifierIconToDxgi()`:

```cpp
    // While magnify_progress_ > 0: pushes a full-panel dim scrim (OSD sprite slot
    // 3, alpha scaled by magnify_progress_) and re-points the DXGI webcam overlay
    // at the interpolated (normal -> enlarged) rect instead of webcam_rect_norm_ —
    // preview-only, never persisted. At progress == 0, clears the scrim and
    // restores the normal placement via syncWebcamOverlayToDxgi().
    void syncEnlargedWebcamToDxgi();
```

- [ ] **Step 3: Implement it**

In `app/ui/widgets/PreviewSurface.cpp`, add after `syncMagnifierIconToDxgi()`:

```cpp
// Drives the DXGI-composited counterpart of paintEvent's "floating enlarged
// view" block (see the `webcam_enabled_ && magnify_progress_ > 0.0` branch):
// same dim scrim, same lerped rect, same restore icon (handled by
// syncMagnifierIconToDxgi(), already called wherever this is called). Never
// writes webcam_rect_norm_ -- only what's pushed to the renderer for THIS
// preview frame changes.
void PreviewSurface::syncEnlargedWebcamToDxgi() {
    if (!dxgi_active_ || !dxgi_renderer_)
        return;

    if (magnify_progress_ <= 0.0) {
        dxgi_renderer_->SetOsdSprite(3, nullptr, 0, 0, 0, 0, 0);
        syncWebcamOverlayToDxgi(); // restore the normal (confirmed) placement
        return;
    }

    // Scrim: a solid, panel-sized, alpha-animated rect. One pixel is enough --
    // the renderer stretches whatever it's given to destX/destY at native size
    // here, so build it at content-rect size directly (small enough to not matter
    // at preview resolution, and this only runs during a ~180ms transition).
    const QRectF frame_rect = displayedFrameRect();
    const qreal dpr = devicePixelRatioF();
    const QSize scrim_size = (frame_rect.size() * dpr).toSize().expandedTo(QSize(1, 1));
    QImage scrim(scrim_size, QImage::Format_ARGB32);
    scrim.fill(QColor(6, 6, 8, qRound(150 * magnify_progress_)));
    dxgi_renderer_->SetOsdSprite(3, scrim.constBits(), scrim.width(), scrim.height(),
                                 static_cast<int>(scrim.bytesPerLine()), qRound(frame_rect.x() * dpr),
                                 qRound(frame_rect.y() * dpr));

    // Interpolated placement, normalized the same way webcam_rect_norm_ is
    // (fraction of frame_rect), matching webcamEnlargedTargetRect()'s use in
    // paintEvent exactly.
    const QRectF normal = webcamPixelRect();
    const QRectF target = webcamEnlargedTargetRect();
    const QRectF draw_rect(normal.x() + (target.x() - normal.x()) * magnify_progress_,
                           normal.y() + (target.y() - normal.y()) * magnify_progress_,
                           normal.width() + (target.width() - normal.width()) * magnify_progress_,
                           normal.height() + (target.height() - normal.height()) * magnify_progress_);
    if (frame_rect.width() < 1.0 || frame_rect.height() < 1.0)
        return;
    const float nx = static_cast<float>((draw_rect.x() - frame_rect.x()) / frame_rect.width());
    const float ny = static_cast<float>((draw_rect.y() - frame_rect.y()) / frame_rect.height());
    const float nw = static_cast<float>(draw_rect.width() / frame_rect.width());
    const float nh = static_cast<float>(draw_rect.height() / frame_rect.height());

    const WebcamChromaKeySettings::ActiveRgb key = webcam_chroma_.active_color();
    recorder_core::ChromaKeyParams chroma;
    chroma.enabled = webcam_chroma_.enabled;
    chroma.r = key.r;
    chroma.g = key.g;
    chroma.b = key.b;
    chroma.tolerance = webcam_chroma_.tolerance;
    chroma.softness = webcam_chroma_.softness;
    chroma.spill_reduction = webcam_chroma_.spill_reduction;
    dxgi_renderer_->SetWebcamOverlayState(/*show=*/true, /*selected=*/false, nx, ny, nw, nh, webcam_mirror_,
                                          webcam_opacity_, chroma);
    if (!webcam_frame_.isNull()) {
        const QImage& img = webcam_frame_;
        dxgi_renderer_->SetWebcamOverlayFrame(img.constBits(), img.width(), img.height(),
                                              static_cast<int>(img.bytesPerLine()));
    }
}
```

- [ ] **Step 4: Enable click-to-enlarge during DXGI-active preview**

In `mousePressEvent` (`app/ui/widgets/PreviewSurface.cpp:1234`), change:

```cpp
    if (!dxgi_active_ && webcam_enabled_ &&
        (magnify_animation_ == nullptr || magnify_animation_->state() != QAbstractAnimation::Running)) {
```

to:

```cpp
    if (webcam_enabled_ &&
        (magnify_animation_ == nullptr || magnify_animation_->state() != QAbstractAnimation::Running)) {
```

(Remove `!dxgi_active_ &&` only. Everything inside the block — the enlarged-state click-to-collapse and the hovered-icon click-to-enlarge branches — is already correct and reusable as-is; it calls `setWebcamEnlarged()`, which this task wires up next.)

- [ ] **Step 5: Wire the animation tick and settle/start points to the new sync**

In `ensureMagnifyAnimation()` (`app/ui/widgets/PreviewSurface.cpp`, the `connect(magnify_animation_, &QVariantAnimation::valueChanged, ...)` lambda), change:

```cpp
    connect(magnify_animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        magnify_progress_ = value.toDouble();
        update();
    });
```

to:

```cpp
    connect(magnify_animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        magnify_progress_ = value.toDouble();
        update();
        syncEnlargedWebcamToDxgi();
        syncMagnifierIconToDxgi();
    });
```

Also call both `syncEnlargedWebcamToDxgi()` and `syncMagnifierIconToDxgi()` from the two hard-reset blocks that already zero `magnify_progress_`/`webcam_enlarged_` outside the animation (the `setWebcamOverlayEnabled(false)` block at `PreviewSurface.cpp:600-605`, and the other one around `PreviewSurface.cpp:713-718` — grep for `magnify_progress_ = 0.0` to find both), so a hard reset clears the DXGI-side scrim/icon too, not just the Qt-paint state.

- [ ] **Step 6: Update KNOWN_LIMITATIONS.md**

Find the note commit `ad47e62` added (search for "magnifier" in `KNOWN_LIMITATIONS.md` — it's in the "Capture previews" section, describing the DXGI-occlusion boundary). Remove it (or replace with a one-line note that the magnifier now composites correctly in both the DXGI-live and QImage-fallback preview paths, if you'd rather keep a positive record than delete silently — your call, but the stale "cannot currently be pixel-verified" claim must not remain once this task lands).

- [ ] **Step 7: Full build**

- [ ] **Step 8: Write and run tests**

Extend whatever test approach Task 1 established (live-hardware `DxgiPreviewRenderer`/`PreviewSurface` test, per its Step 6 resolution) to also cover: clicking the hovered icon while `dxgi_active_` enlarges the PiP (scrim sprite becomes non-empty, webcam overlay rect changes from the normal to the enlarged placement), and clicking outside the enlarged view (or the restore icon, or Escape) collapses it back (scrim clears, overlay rect returns to `webcam_rect_norm_`'s normalized value). Also confirm `webcam_rect_norm_` itself is bit-for-bit unchanged after a full enlarge+collapse cycle — the regression this whole plan must not introduce.

Run: `ctest --test-dir <build-dir> -R "preview_surface_webcam|dxgi_preview_pushed_source" -V`. Expected: PASS.

- [ ] **Step 9: Visual check**

Same as Task 1 Step 7 — render an applicable `--visual-test` scenario and note in your report what could and couldn't be pixel-confirmed this way (e.g. if the harness has no way to simulate a click/hover sequence, the enlarge transition itself may remain a manual-verification item even after this task; say so plainly rather than claiming full pixel coverage you don't have).

- [ ] **Step 10: Commit**

```bash
git add app/services/DxgiPreviewRenderer.h app/ui/widgets/PreviewSurface.h app/ui/widgets/PreviewSurface.cpp KNOWN_LIMITATIONS.md app/tests/test_preview_surface_webcam.cpp
git commit -m "Composite the enlarged webcam magnifier view into the DXGI preview; resolves the DXGI-occlusion limitation"
```

- [ ] **Step 11: Merge directly to main and push**

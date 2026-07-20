# DXGI-Active Webcam Magnifier Compositing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the hover-to-enlarge webcam PiP magnifier (merged 2026-07-20, commit `ad47e62`) actually usable during the live DXGI preview — which is the path real recording and monitor/window preview run on essentially 100% of the time — instead of being silently limited to the QImage software-paint fallback that only a handful of test/idle scenarios use. This resolves the boundary `KNOWN_LIMITATIONS.md` currently documents ("the magnifier cannot currently be pixel-verified... every Record-page scenario selects a capture target, which flips `dxgi_active_` true").

**Design change from the original version of this plan (product decision, 2026-07-20):** The original plan composited the painted hover-icon badge (circle + search/x glyph) into the DXGI swapchain as a third OSD sprite. Instead, the painted badge is removed entirely (both preview paths) and replaced with a native cursor change (`Qt::PointingHandCursor`) over the same hotspot. A cursor is OS-level and needs no swapchain compositing, so this removes an entire task's worth of DXGI-sprite work while still resolving the discoverability need ("hover shows you can click to enlarge/collapse") in both preview backends uniformly. The floating *enlarged webcam view itself* (real video content + dim scrim) is unaffected by this change and still needs DXGI compositing — that remains this plan's one substantive task.

**Architecture:** No new architecture. `PreviewSurface`'s existing `!dxgi_active_` gates around magnifier hover-tracking and click handling are removed outright (the gating existed only because nothing rendered the feature in DXGI mode — once the enlarged-view content is composited, both preview backends behave identically at the input layer, since the DXGI child HWND is already click-through by design: its `WM_NCHITTEST` returns `HTTRANSPARENT`, confirmed by reading `DxgiPreviewRenderer::ChildWndProc`). The enlarged floating webcam view reuses the *existing* `SetWebcamOverlayState`/`SetWebcamOverlayFrame` webcam-PiP-compositing path, just fed an animated (interpolated) placement rect instead of the confirmed `webcam_rect_norm_` — mirroring exactly what the Qt-paint fallback's `paintEvent` already computes, never touching the persisted placement.

**Tech Stack:** C++20, Qt 6 widgets, Direct3D 11 (existing `DxgiPreviewRenderer` swapchain compositor), GoogleTest.

## Global Constraints

- Never mutate `webcam_rect_norm_` (the confirmed, persisted, WYSIWYG placement the recording engine reads) for magnifier animation purposes. Only the *arguments passed to* `DxgiPreviewRenderer::SetWebcamOverlayState` for the preview's own swapchain may reflect the animated/enlarged rect — this mirrors the existing Qt-paint `paintEvent`'s own rule ("Preview-only and fully transient: never touches webcam_rect_norm_").
- Reuse existing rect-computation helpers (`magnifierIconRect()`, `webcamMagnifierIconMappedRect()`, `webcamEnlargedTargetRect()`) rather than re-deriving geometry — they are already implemented and unit-tested. `webcamMagnifierIconMappedRect()` is repurposed by this plan from "where the DXGI compositor should place the icon sprite" (its original purpose, never built) to "the cursor/click hotspot rect" — same geometry, no icon rendering consumes it anymore.
- `paintMagnifierIcon()` (the badge painter: circle + search/x glyph) is deleted, not reused — nothing calls it once Task 1 lands. Do not leave it as dead code.
- Never interact with a running ExoSnap instance (no mouse/keyboard synthesis, no window automation). Use ctest widget tests and, where feasible, the `--visual-test` render harness. A "start once, confirm no crash, close" check is fine if a QSS/theme file is touched (not expected here).
- Full build (not just `--target exosnap`) before running tests; this project's convention.
- `KNOWN_LIMITATIONS.md`'s magnifier note must be updated once Task 2 lands (that's when the feature becomes fully usable) to state the limitation is resolved, per this project's rule that visible-behavior changes update the relevant spec/doc.
- Per explicit user authorization for this cleanup/feature pass: merge each task directly to `main` after it's green — no PR required.

---

### Task 1: Replace the painted magnifier badge with a cursor hint; drop the DXGI hover/click gating

**Files:**
- Modify: `app/ui/widgets/PreviewSurface.cpp` (delete `paintMagnifierIcon()` and its two call sites in `paintEvent`; extend `applyHoverCursor()` with the hotspot check; remove the `!dxgi_active_ &&` guards in `mouseMoveEvent`/`mousePressEvent`; simplify the enlarged-state collapse click test)
- Modify: `app/ui/widgets/PreviewSurface.h` (remove `paintMagnifierIcon()` declaration)
- Test: `app/tests/test_preview_surface_webcam.cpp`

**Interfaces:**
- Consumes: `PreviewSurface::webcamMagnifierIconMappedRect() const -> QRect` (existing, unchanged signature and behavior — returns `{}` when there's no hotspot to show, the enlarged-icon-rect when `magnify_progress_ > 0`, or the normal-hover-icon-rect when hovered/unselected/not-dragging).
- Produces: `applyHoverCursor()` now also sets `Qt::PointingHandCursor` when the pointer is over that hotspot, taking priority over the resize/move cursors `hitTestWebcam()` would otherwise pick.

**Background — read before starting:** The DXGI child HWND (`DxgiPreviewRenderer`'s `childHwnd_`) is click-through by design (`ChildWndProc`, `app/services/DxgiPreviewRenderer.cpp:626-632`, returns `HTTRANSPARENT` from `WM_NCHITTEST`), so mouse input already reaches `PreviewSurface`'s normal Qt event handlers regardless of `dxgi_active_` — this was already true before this plan and needs no change. The only reason the magnifier didn't work during DXGI-active preview was the explicit `!dxgi_active_ &&` guards added defensively when the feature was first built (there was nothing to show, so clicks/hover were suppressed rather than left dangling). Removing those guards is safe now that there's no paint-dependent state left to desync (a cursor change has no "which backend is active" concept).

- [ ] **Step 1: Extend `applyHoverCursor()` with the magnifier hotspot check**

  In `app/ui/widgets/PreviewSurface.cpp`, at the top of `applyHoverCursor()` (currently `PreviewSurface.cpp:889`), before the existing `if (!webcamEditingAllowed() || !hover_pos_valid_)` check, add a check against `webcamMagnifierIconMappedRect()`: if it's non-empty and contains `hover_pos_.toPoint()`, call `setCursor(Qt::PointingHandCursor)` and return. This must run whether or not `webcamEditingAllowed()` is true — the magnifier is clickable even while drag/resize editing is locked (e.g. during a live recording), matching the existing comment in `mouseMoveEvent` ("Independent of the edit-lock gate below").

  Note `applyHoverCursor()` returns early today when `drag_mode_ != DragMode::None` — keep that ordering (an in-flight drag/resize keeps its cursor; the hotspot check comes after that early-out, before the edit-lock check).

- [ ] **Step 2: Call `applyHoverCursor()` from the paths that currently skip it while enlarged/transitioning**

  `mouseMoveEvent` (`PreviewSurface.cpp:1312-1316`) currently returns early — before reaching `applyHoverCursor()` — whenever `webcam_enlarged_ || magnify_progress_ > 0.0`. Add a call to `applyHoverCursor()` right before that early return so the pointing-hand hint still applies to the enlarged view's own hotspot corner (the region `webcamMagnifierIconMappedRect()` returns when `magnify_progress_ > 0`).

- [ ] **Step 3: Remove the `!dxgi_active_ &&` guards**

  In `mouseMoveEvent` (`PreviewSurface.cpp:1304`), change:
  ```cpp
      if (!dxgi_active_ && webcam_enabled_ && magnify_progress_ <= 0.0) {
  ```
  to:
  ```cpp
      if (webcam_enabled_ && magnify_progress_ <= 0.0) {
  ```

  In `mousePressEvent` (`PreviewSurface.cpp:1234`), change:
  ```cpp
      if (!dxgi_active_ && webcam_enabled_ &&
          (magnify_animation_ == nullptr || magnify_animation_->state() != QAbstractAnimation::Running)) {
  ```
  to:
  ```cpp
      if (webcam_enabled_ &&
          (magnify_animation_ == nullptr || magnify_animation_->state() != QAbstractAnimation::Running)) {
  ```
  Update the comment above each (currently describing this as "a Qt-paint-layer feature only" / "Qt-paint-layer feature only (see mousePressEvent)") to reflect that hover/click handling is now backend-independent; only the *painting* of the enlarged view's content remains backend-dependent (Task 2).

  Also update `keyPressEvent`'s `if (!dxgi_active_ && webcam_enlarged_ && event->key() == Qt::Key_Escape)` (`PreviewSurface.cpp:1363`) the same way — Escape-to-collapse should work regardless of preview backend now.

- [ ] **Step 4: Simplify the enlarged-state collapse click test**

  In `mousePressEvent`'s enlarged branch (`PreviewSurface.cpp:1236-1247`), the current logic collapses on `icon.contains(pos) || !target.contains(pos)` — the `icon.contains(pos)` half existed to let users click the painted "x" badge. With no badge painted, drop that half and collapse on `!target.contains(pos)` alone (click anywhere outside the enlarged view collapses it; click inside is still absorbed/no-op, matching today's behavior for clicks inside the target that aren't on the badge).

- [ ] **Step 5: Delete `paintMagnifierIcon()` and its call sites**

  Remove the two `paintMagnifierIcon(...)` calls in `paintEvent` (`PreviewSurface.cpp:1624` inside the `magnify_progress_ > 0.0` branch, and `:1692` inside the normal-hover branch) — delete the surrounding `if (webcam_hovered_ && !show_chrome && drag_mode_ == DragMode::None)` block at `:1691-1693` entirely (it existed only to gate the paint call), and remove the now-bare paint call at `:1624` (the rest of that branch — scrim + lerped PiP image — stays).

  Delete the `paintMagnifierIcon()` method definition (`PreviewSurface.cpp:981-1004`) and its declaration in `PreviewSurface.h`. Leave `magnifierIconRect()` and `webcamMagnifierIconMappedRect()` in place — they're still the hotspot-geometry source of truth for cursor and click handling.

- [ ] **Step 6: Full build**

- [ ] **Step 7: Update tests**

  In `app/tests/test_preview_surface_webcam.cpp`'s `PreviewSurfaceWebcamTest` fixture (the "Webcam magnifier" section starting around line 279): the existing tests already click via `webcamMagnifierIconMappedRect().center()`, which remains valid as the hotspot — most tests need no behavioral change. Specifically:
  - `HoverShowsMagnifierIconAndLeavingHidesIt`: rename to reflect cursor behavior (e.g. `HoverShowsMagnifierCursorAndLeavingRestoresIt`) and add an assertion that `surface_->cursor().shape() == Qt::PointingHandCursor` while hovered, and not while not hovered. Keep the `webcamMagnifierIconMappedRect()` non-empty/empty assertions (the hotspot geometry itself is unchanged).
  - `MagnifierIconHiddenWhileSelected`: rename similarly; the hotspot-empty assertion still holds.
  - The click/enlarge/collapse/Escape/hard-reset tests (`ClickingMagnifierIconEnlargesThePip`, `ClickingOutsideEnlargedViewCollapsesIt`, `EscapeCollapsesEnlargedView`, `DisablingWebcamHardResetsMagnifierState`) should pass unchanged since they click at the hotspot center, which is still the click target — verify this rather than assuming, and adjust names/comments that say "icon" to say "hotspot" or "cursor hint" where they describe the removed visual.
  - Do not add a `dxgi_active_`-specific test here yet — this fixture never starts a real DXGI preview, and the guard removal in Steps 3-4 has no DXGI dependency to verify (it's pure Qt event-handling code, identical regardless of `dxgi_active_`). Task 2 is where DXGI-active behavior actually gets a live-hardware assertion.

  Run: `ctest --test-dir <build-dir> -R "preview_surface_webcam" -V`. Expected: PASS.

- [ ] **Step 8: Commit**

  ```bash
  git add app/ui/widgets/PreviewSurface.h app/ui/widgets/PreviewSurface.cpp app/tests/test_preview_surface_webcam.cpp
  git commit -m "Replace the painted webcam magnifier badge with a cursor hint; enable hover/click in DXGI-active preview"
  ```

- [ ] **Step 9: Merge directly to main and push**

---

### Task 2: Composite the enlarged webcam view (video + scrim) into the DXGI preview

**Files:**
- Modify: `app/services/DxgiPreviewRenderer.h` (bump `kOsdSpriteSlots`, 2→3, for the dim scrim)
- Modify: `app/ui/widgets/PreviewSurface.h` (add `syncEnlargedWebcamToDxgi()` private method declaration)
- Modify: `app/ui/widgets/PreviewSurface.cpp` (implement it; wire into the animation tick and hard-reset points)
- Modify: `KNOWN_LIMITATIONS.md` (remove/update the magnifier boundary note added by commit `ad47e62`)
- Test: `app/tests/test_preview_surface_webcam.cpp` and/or `app/tests/test_dxgi_preview_pushed_source.cpp`

**Interfaces:**
- Consumes: Task 1's guard removal (hover/click/Escape already work regardless of `dxgi_active_`). `DxgiPreviewRenderer::SetWebcamOverlayState(bool enabled, bool selected, float nx, float ny, float nw, float nh, bool mirror, float opacity, const recorder_core::ChromaKeyParams& chroma)` and `SetWebcamOverlayFrame(...)` (existing, unchanged — called with the *animated* rect instead of `webcam_rect_norm_` while `magnify_progress_ > 0`). `PreviewSurface::webcamEnlargedTargetRect() const -> QRectF`, `webcamPixelRect() const -> QRectF` (existing).
- Produces: `void PreviewSurface::syncEnlargedWebcamToDxgi()` — pushes the dim scrim sprite (slot 2) and the interpolated webcam overlay rect; called every animation tick while `magnify_progress_ > 0` and once more on settle/collapse to restore the normal `syncWebcamOverlayToDxgi()` state.

**Note:** There is no third sprite for a hover icon in this version of the plan (Task 1 replaced it with a cursor). Only the scrim needs a new OSD slot.

- [ ] **Step 1: Bump the sprite slot count**

  In `app/services/DxgiPreviewRenderer.h`, change:
  ```cpp
      static constexpr int kOsdSpriteSlots = 2;
  ```
  to:
  ```cpp
      // Slot 0/1: preview meta/stats text rows (PreviewSurface::syncOsdToDxgi).
      // Slot 2: webcam-magnifier dim scrim, full-panel, shown only while enlarging/
      // enlarged (PreviewSurface::syncEnlargedWebcamToDxgi).
      static constexpr int kOsdSpriteSlots = 3;
  ```

- [ ] **Step 2: Declare the new sync method**

  In `app/ui/widgets/PreviewSurface.h`, add near `syncOsdToDxgi()`:
  ```cpp
      // While magnify_progress_ > 0: pushes a full-panel dim scrim (OSD sprite slot
      // 2, alpha scaled by magnify_progress_) and re-points the DXGI webcam overlay
      // at the interpolated (normal -> enlarged) rect instead of webcam_rect_norm_ --
      // preview-only, never persisted. At progress == 0, clears the scrim and
      // restores the normal placement via syncWebcamOverlayToDxgi().
      void syncEnlargedWebcamToDxgi();
  ```

- [ ] **Step 3: Implement it**

  In `app/ui/widgets/PreviewSurface.cpp`, add after `syncWebcamOverlayToDxgi()` (or wherever the other `sync*ToDxgi()` helpers live):
  ```cpp
  // Drives the DXGI-composited counterpart of paintEvent's "floating enlarged
  // view" block: same dim scrim, same lerped rect. Never writes
  // webcam_rect_norm_ -- only what's pushed to the renderer for THIS preview
  // frame changes.
  void PreviewSurface::syncEnlargedWebcamToDxgi() {
      if (!dxgi_active_ || !dxgi_renderer_)
          return;

      if (magnify_progress_ <= 0.0) {
          dxgi_renderer_->SetOsdSprite(2, nullptr, 0, 0, 0, 0, 0);
          syncWebcamOverlayToDxgi(); // restore the normal (confirmed) placement
          return;
      }

      // Scrim: a solid, panel-sized, alpha-animated rect. One pixel is enough --
      // the renderer stretches whatever it's given to destX/destY at native size
      // here, so build it at content-rect size directly (small enough to not
      // matter at preview resolution, and this only runs during a ~180ms
      // transition).
      const QRectF frame_rect = displayedFrameRect();
      const qreal dpr = devicePixelRatioF();
      const QSize scrim_size = (frame_rect.size() * dpr).toSize().expandedTo(QSize(1, 1));
      QImage scrim(scrim_size, QImage::Format_ARGB32);
      scrim.fill(QColor(6, 6, 8, qRound(150 * magnify_progress_)));
      dxgi_renderer_->SetOsdSprite(2, scrim.constBits(), scrim.width(), scrim.height(),
                                   static_cast<int>(scrim.bytesPerLine()), qRound(frame_rect.x() * dpr),
                                   qRound(frame_rect.y() * dpr));

      // Interpolated placement, normalized the same way webcam_rect_norm_ is
      // (fraction of frame_rect), matching paintEvent's lerp exactly.
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

- [ ] **Step 4: Wire the animation tick and hard-reset points to the new sync**

  In `ensureMagnifyAnimation()`'s `connect(magnify_animation_, &QVariantAnimation::valueChanged, ...)` lambda, change:
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
      });
  ```

  Also call `syncEnlargedWebcamToDxgi()` from the two hard-reset blocks that already zero `magnify_progress_`/`webcam_enlarged_` outside the animation (the `setWebcamOverlayEnabled(false)` block at `PreviewSurface.cpp:600-605`, and the other one around `PreviewSurface.cpp:713-718` — grep for `magnify_progress_ = 0.0` to find both), so a hard reset clears the DXGI-side scrim too, not just the Qt-paint state. And call it once at the two places `tryStartDxgiPreview`/`tryStartDxgiPushedPreview` already call `syncWebcamOverlayToDxgi()`/`syncOsdToDxgi()`, so a fresh DXGI preview starts with a definitely-cleared scrim slot.

- [ ] **Step 5: Update KNOWN_LIMITATIONS.md**

  Find the note commit `ad47e62` added (search for "magnifier" — it's in the "Capture previews" section, describing the DXGI-occlusion boundary). Remove it, or replace with a one-line note that the magnifier now works correctly in both the DXGI-live and QImage-fallback preview paths — your call, but the stale "cannot currently be pixel-verified" claim must not remain once this task lands.

- [ ] **Step 6: Full build**

- [ ] **Step 7: Write and run tests**

  This is a genuinely open test-design question — resolve it yourself and note the reasoning in your report, don't guess silently. Two known-working precedents exist in this codebase:
  1. `app/tests/test_preview_surface_webcam.cpp`'s existing fixture never starts a real DXGI preview (`dxgi_active_` is always `false` there) — it's the right place for a cheap regression test that `syncEnlargedWebcamToDxgi()` early-returns safely when `dxgi_active_` is `false`, but that alone does NOT prove the DXGI-active path works.
  2. `app/tests/test_dxgi_preview_pushed_source.cpp` is a **live-hardware test**: it creates a real `DxgiPreviewRenderer` against a hidden test window, with a documented `GTEST_SKIP()` for headless/GPU-less CI runners. This is the precedent for actually exercising `dxgi_active_ == true` behavior.

  Extend the live-hardware path to cover: clicking the hotspot while `dxgi_active_` enlarges the PiP (scrim sprite becomes non-empty, webcam overlay rect changes from the normal to the enlarged placement), and clicking outside the enlarged view (or Escape) collapses it back (scrim clears, overlay rect returns to `webcam_rect_norm_`'s normalized value). Also confirm `webcam_rect_norm_` itself is bit-for-bit unchanged after a full enlarge+collapse cycle — the regression this whole plan must not introduce. If after investigating you're not confident which approach is right, report NEEDS_CONTEXT and ask rather than guessing.

  Run: `ctest --test-dir <build-dir> -R "preview_surface_webcam|dxgi_preview_pushed_source" -V`. Expected: PASS (GTEST_SKIP acceptable only for the genuinely-headless-runner case).

- [ ] **Step 8: Visual check**

  If the existing `--visual-test` Record-page webcam scenarios run with `dxgi_active_ == true` (verify this yourself), render one of them (e.g. `record-webcam-default-pip`) and confirm the normal (non-enlarged) PiP still composites correctly. The enlarge transition itself likely remains a manual-verification item if the harness can't simulate a click/hover sequence — say so plainly in your report rather than claiming full pixel coverage you don't have.

- [ ] **Step 9: Commit**

  ```bash
  git add app/services/DxgiPreviewRenderer.h app/ui/widgets/PreviewSurface.h app/ui/widgets/PreviewSurface.cpp KNOWN_LIMITATIONS.md app/tests/test_preview_surface_webcam.cpp
  git commit -m "Composite the enlarged webcam magnifier view into the DXGI preview; resolves the DXGI-occlusion limitation"
  ```

  (Add `app/tests/test_dxgi_preview_pushed_source.cpp` too if Step 7 extended it.)

- [ ] **Step 10: Merge directly to main and push**

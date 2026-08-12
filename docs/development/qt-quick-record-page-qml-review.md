# Qt Quick Record Page QML Review

- Date: 2026-08-09
- Scope: the complete production Quick module (14 production QML files), its C++ adapter/lifecycle
  boundary, and the Record Quick Tests.

## Result

The deterministic QML rules and system `qmllint` finish clean. The six review tracks covered
bindings, layout, component lifecycle, delegates/models, states/accessibility, and performance.
Confirmed high-confidence findings were fixed; no unresolved high-confidence issue remains.

## Confirmed findings and mitigations

- Region selection originally kept pixel geometry and could drift after preview resize. The owner now
  stores a normalized rectangle and derives visual geometry from it.
- Long state chips could overlap the independently positioned source label. Header content now flows
  within explicit non-overlapping reservations.
- Live metrics and the right-anchored size label could collide in narrow fitted previews. Metrics use
  a constrained `Flow`; size and state occupy reserved regions.
- Central error width could become negative for an extremely narrow source fit. Width is clamped to a
  non-negative available value.
- Coordinator webcam callbacks could outlive the Quick receiver. The callback overload now binds
  delivery to receiver lifetime and shutdown clears the preview callback.
- Hidden Record pages continued requesting webcam images. Delivery and QML loading are gated by the
  adapter's active state, with a textual image-load failure state.
- Target pickers rebuilt a duplicated mixed target tree and instantiated hidden controls. The adapter
  exposes stable filtered lists; window rows use a virtualized `ListView`.
- Source and navigation selection were only visual. Standard checked/checkable and accessible state
  now expresses the selection; icon-only/dismiss actions have accessible names.
- Region editing was pointer-only. The overlay now accepts focus and supports arrow movement and
  Shift+arrow resizing.
- Audio meter callbacks invalidated the complete adapter at service cadence. Values are coalesced and
  emitted through `metersChanged`, independent from structural state.
- Webcam image URLs and full-size images caused request/upload churn. Revisions are capped near 30 Hz,
  inactive pages stop loading, and the provider honors the requested PiP size before chroma work and
  texture upload.
- The webcam provider remained active while its QML image was invisible during recording. Its source
  is now empty outside idle PiP preview, so recording does not perform hidden image-provider work.
- Pointer resize rebound `sourceSize` at drag cadence and keyboard editing persisted on every repeat.
  Provider requests now hold a quantized size for the duration of a drag, and overlay-preset writes
  are centrally debounced and flushed at shutdown.
- Backend-derived selectable buttons could uncheck the active option. They are now auto-exclusive,
  and Quick Tests cover authoritative selection and keyboard behavior.

## Investigation targets

- A deliberately injected, unusually long multi-line notice should be included in future localization
  stress validation at the minimum size. Current canonical notices fit and are dismissible.
- Focus restoration is state-driven and exercised by control tests, but a full assistive-technology
  pass should verify restoration across actual asynchronous device loss and finalization.
- Scene-cadence preview polling remains visible in benchmark data. This is an existing bridge
  scheduling optimization, not a per-frame QML allocation defect.
- A profiling-enabled build was unavailable for this slice. The real recording benchmark is a useful
  sanity check but not a substitute for a Qt Quick Profiler trace.

The review did not recommend replacing the proven D3D11 scene bridge or introducing a parallel QML
state machine.

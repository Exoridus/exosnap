# Frontend and application composition

This document owns the Qt/QML boundary, native window ownership, render integration and presentation lifetime. Observable layout, keyboard, notification and control behavior belongs to the [product specification](../product-spec.md).

## One frontend, typed boundaries

`QuickApplication` composes one `ExoSnap.Quick` QML module. Narrow QObject adapters and list models are the only application boundary visible to QML. Stores, the coordinator and a generic service registry are not exposed to JavaScript. Product policy stays in C++.

`SettingsAdapter` funnels edits through reconciliation and sanitization. Option lists contain values, labels, availability and the owner's reason. An incomplete custom-resolution entry is transient adapter state until it can become a valid model value; it must not weaken the sanitizer or persist a half-entered format.

Diagnostics has an application controller, async probe and fix dispatcher. Logs use a list model over the bounded `AppLog` history, with insert/remove notifications and a filtering proxy. Do not introduce another full history copy or one permanent visual item per log entry. Support-bundle collection and compression run asynchronously.

A settings change is persisted by the composition owner, not by QML. File/folder dialogs use the native Qt Quick dialog integration and hand their result to C++ for path conversion. Capability scans, hash computation, file open, keyframe indexing and device probing do not belong on the GUI thread.

## Native window ownership

The main window owns the client pixels; the preview and editor are scene-graph items without child HWNDs. Native child windows can intercept `WM_NCHITTEST` before the top-level shell sees it, breaking drag/resize/Snap even when the screenshot looks correct. `QQuickWidget` or a window-container shim is not an interchangeable implementation of this architecture.

C++ owns first show. QML loads hidden, final style is reasserted after QML flags settle, startup geometry is applied and checked against Windows, then the window is shown in the intended normal/maximized state. A delayed first-frame correction violates this contract. `WS_THICKFRAME` is maintained for native resizing/Snap while the client area contains no extra native caption.

The technical window title and application identity are shared constants used by activation, updater handoff and storage. They are not translatable product copy. All actual user-facing copy follows the translation policy; native identity must stay stable across locales.

The Widgets dependency is bounded to system-tray integration in the main process. The separate updater's Widgets UI is a separate process boundary, not a second main frontend.

## Preview and editor rendering

Both custom items use public scene-graph nodes, rounded clipping and Qt's external-command boundary. Their transports are deliberately different. The Record preview consumes a shared NT-handle GPU image; the editor consumes a newest-wins mailbox of decoded planes with refcounted backing frames. Sharing the render skeleton does not make those ownership models interchangeable.

Render resources live on the render thread. Generation counters invalidate late callbacks after source replacement. Scene-graph teardown releases consumers before application services and producers. Preview publication callbacks are lightweight and never invoke GUI work directly from the capture thread. See [capture and preview](capture-and-preview.md) and [edit and export](edit-and-export.md).

The editor timeline uses ordinary QML items and bounded models. Thumbnail count follows visible width, not clip duration. Markers are thinned in C++ to a bounded per-pixel representation rather than instantiating up to the recording's entire marker limit. Actual trim is stored once, in microseconds, on the adapter. QML can hold only the transient pointer position during a drag.

## Asynchronous and temporary state

The shell distinguishes requested and displayed page while asynchronous loading completes. A destination is not treated as visible until it can render. Adapters publish the actual state a control can act on; availability must not be guessed from a pending navigation request.

Edit is temporary. Closing it or navigating away drops the recipe and releases media decoders. An export already started owns an immutable snapshot independently. Cancellation remains a running/cancelling operation until its worker reports completion, so immediate retry cannot block the GUI on the preceding thread's join.

Modal recovery/crash/error requests are queued rather than replacing one another mid-read. The current surface retains precedence. Recording start is guarded while a blocking surface exists, but stop/pause/resume remain reachable for an existing session.

## Overlays and theme ownership

Webcam PiP is recorded content. Recording status, diagnostics, countdown, quick controls and notification toasts are separate capture-excluded top-level windows. Their opacity in a scene-graph grab does not prove desktop composition or capture exclusion. The exclusion API and a human desktop check answer different questions.

The first three overlay types are click-through. Quick controls and toast actions are intentionally interactive and do not steal keyboard focus. Failed capture exclusion hides an overlay; the optional exclusion of the main window instead fails visibly and logs the refusal rather than hiding the application's only control surface.

Semantic theme roles separate accent from recording/error, warning/paused and ready/success. Appearance and accent resolve through shared token data. Shell-owned surfaces such as native tray menus and desktop notifications follow the relevant Windows appearance; in-app surfaces follow the selected application appearance. Do not duplicate palette literals or reuse a warning tone for an ordinary selection.

## Deployment and verification

QML runtime dependencies are not fully visible in the executable import table. Deployment uses the QML-aware Qt deployment script and import scanner. Loader hardening intentionally removes PATH-based dependency resolution, so a build-tree launch with Qt installed is not packaging evidence. Verify the staged tree with the required runtime imports and a sanitized environment.

Deterministic QML/adapter tests prove state and layout contracts. `--hwnd-audit` proves native structure, and window/preview traces expose lifecycle behavior. Real media is required to prove editor decoding; fixtures with an empty player cannot. Native drag, Secure Desktop and actual capture-excluded desktop composition need the corresponding live/operator checks.

## Implementation and tests

See [Quick module](../../app/quick/ExoSnap/Quick/CMakeLists.txt), [composition](../../app/quick/ExoSnap/Quick/QuickApplication.cpp), [main QML](../../app/quick/ExoSnap/Quick/Main.qml), [shell](../../app/quick/ExoSnap/Quick/AppShell.qml), [settings adapter](../../app/quick/ExoSnap/Quick/SettingsAdapter.cpp), and [Quick tests](../../app/quick/tests). [Harness modes](../dev/harness-and-tracing.md) describes the instruments without duplicating the architecture.

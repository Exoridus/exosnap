# ADR 0060: Diagnostics/Logs App-Layer Boundary

## Status

Accepted.

## Context

Diagnostics, Logs, StartupTrace and the support bundle were four surfaces sharing one set of inputs
but no shared owner. The state and the policy over it lived in three places at once:

- `DiagnosticsPage` owned genuine application state (capability set, resolved config, validation,
  disk facts, the last live snapshot) *and* genuine product policy: verdict tier counting, the
  six-card issue cap, hotkey-notice synthesis, seven readiness-tile value/sub/tone rules, and
  stateful frame-drop delta accounting carried across snapshots.
- `LogsPage` held the log history a second and third time (`entries_`, `visible_entries_`) on top of
  `AppLog`'s deque and the `QPlainTextEdit` document — four copies of the same 5000 entries.
- `MainWindow` owned support-bundle assembly and the FixAction dispatch chain.

None of that is presentation. Migrating the surfaces to Qt Quick without moving it would have meant
either reimplementing product policy in QML or duplicating it across two frontends.

A second problem is orthogonal to the framework: the subsystem does blocking I/O on the GUI thread
throughout. A Win32 volume query runs on every diagnostic data push; `CheckOutputPathWritable`
performs a real file create/write/delete twice per refresh and at 2 Hz while recording;
`SelfTestRunner::Run()` creates a DXGI factory, `LoadLibraryW`s NVENC, writes a temp file and
enumerates COM audio endpoints; the support bundle reads up to six log files, runs two global regex
scrubs over each, enumerates adapters and deflates in memory. Widgets tolerated this. A Quick
frontend renders the jank.

## Decision

Introduce an app-layer owner below both frontends and above the existing services:

```text
engine / diagnostics services / AppLog     (UI-framework independent, unchanged)
                        ^
                        |
DiagnosticsController · DiagnosticsProbe · FixActionDispatcher · LogViewPolicy
SupportBundleService                              (app layer; policy and async work)
                        ^
                        |
DiagnosticsAdapter · LogsAdapter · LogEntryModel · DiagnosticIssueModel
                                                  (QObject, QML_ELEMENT, QML_UNCREATABLE)
                        ^
                        |
                  Qt Quick / QML
```

All policy listed above moves into the app layer as pure functions or plain classes with unit tests.
QML renders what the adapters provide and expresses no diagnostic, severity, or bundling rule.
`BuildTopIssueRecommendations` and the `DiagnosticTier` rails are reused verbatim rather than
restated.

**Logs are model-backed over the existing `AppLog`, not a new logging subsystem.** `AppLog` already
holds a mutex-guarded `std::deque<LogEntry>` capped at 5000 with FIFO eviction, and already emits
`entriesAppended(QVector<LogEntry>, int evicted_count)` coalesced onto the GUI thread. A
`QAbstractListModel` layers directly over it: appends become `beginInsertRows`, and the eviction
count makes rollover an exact `beginRemoveRows` instead of the full history re-pull the Widgets page
performed. Filtering is a `QSortFilterProxyModel` over the extracted predicate. Four copies of the
history collapse to one, and a virtualized `ListView` recycles delegates instead of materialising
5000 text blocks.

**Blocking work moves to workers.** `DiagnosticsProbe` bundles the blocking probes (volume query,
filesystem name, output-path write test, self-test) behind an async boundary with a `checking`
property. `SupportBundleService` collects and writes on a worker thread with a `bundleBusy` property
and a `bundleFinished(ok, message)` result.

**The FixAction confirmation is structural, not conventional.** `applyFix(id)` does not apply
anything: it emits `fixConfirmRequested(id, label, changesSummary)`, QML shows the confirm dialog,
and only acceptance reaches the dispatcher. The mandatory confirmation therefore cannot be skipped by
a future caller, and the adapter never owns settings.

## Deliberately unchanged

- **Blocker gating stays advisory.** The product spec states a Blocker prevents recording from
  starting, but no `RecommendationEngine` result reaches any start gate; the only real gate is
  capability validation in `RecordingCoordinator`. This boundary neither implements gating nor
  implies it. The divergence is recorded, not resolved here.
- **`startup-trace.txt` is still absent from the support bundle** despite the spec, ADR 0044 and
  `StartupTrace.h` promising it. `CollectBundleEntries` never adds it.
- The Widgets `DiagnosticsPage`, `LogsPage` and `MainWindow` are untouched and still compile; they
  remain the parity reference until cutover.

## Consequences

- One owner drives both frontends, so the Widgets pages can be deleted at cutover without taking
  product policy with them.
- Frame-drop delta accounting is now an explicit, tested unit (`PipelineCardBuilder`) that
  rebaselines on `session_generation` change rather than an implicit field inside a widget.
- **Behaviour change:** the output-path writability probe no longer runs at 2 Hz during recording. It
  refreshes on first visibility, on Run Check, on config change, and on a 10 s timer while recording.
  A folder that becomes read-only mid-recording therefore surfaces up to ~10 s later. The fact it
  measures changes on a human timescale; the previous cadence bought nothing and cost real file I/O
  inside the recording loop.
- **Behaviour change:** the support bundle no longer reveals the written file in Explorer afterwards.
  It reports success and leaves the file where the user chose it.
- Self-test "not run" detection is an explicit typed field rather than a substring match on
  `"not executed in this build"`.
- Two harness hooks (`EXOSNAP_VISUAL_LOG_SCENARIO`, `EXOSNAP_VISUAL_DIAG_SCENARIO`) exist for
  deterministic visual capture. They are environment-only, in-memory, and must never mutate persisted
  settings.

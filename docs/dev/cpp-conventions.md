# C++ conventions

This is the contributor orientation for C++ layout and naming. [Architecture
overview](../architecture/overview.md) owns subsystem boundaries and
constraints; this document owns where code and tests physically live and how
it reads once you are there. [Build and test](build-and-test.md) owns running
the build; [static analysis](static-analysis.md) owns the compiler/clang-tidy/
cppcheck contract.

## Where things live

| Layer | Public headers | Implementation | Tests |
|---|---|---|---|
| Recording engine | `libs/engine/include/exosnap/engine/` | `libs/engine/src/` (platform code under `src/platform/windows/`) | `libs/engine/tests/test_*.cpp`, GoogleTest fixtures in `libs/engine/testutil/` |
| Other engine-adjacent libraries | `libs/<name>/include/` | `libs/<name>/src/` | `libs/<name>/tests/` |
| Qt/QML frontend, plain C++ view-models | `app/viewmodels/`, `app/models/` | same directory as the header (no separate `src/`) | `app/tests/` |
| Qt Quick adapters (`QObject`, exposed to QML) | `app/quick/ExoSnap/Quick/` | same directory | `app/quick/tests/test_*.cpp`, `app/tests/` |
| QML | `app/quick/ExoSnap/Quick/*.qml` | - | `app/quick/**/tests` (Qt Quick Test) |
| Updater (separate Qt Widgets app) | `apps/updater/` | same directory | `apps/updater/tests/` |
| Rust developer/release tooling | `tools/<crate>/src/` | same directory | `#[cfg(test)]` modules next to the code; `tools/<crate>/tests/` for integration tests |

`libs/engine` is the recording/encode/mux/diagnostics core: no Qt, no
application policy. `app/` is the Qt Quick frontend and its C++ adapters; it
submits editable source rows and reads engine state, it does not reimplement
engine policy. `apps/updater` is a separate Qt Widgets application, not a
second product frontend. Business/product policy stays in C++; QML owns
presentation, layout and interaction (see
[Product specification](../product-spec.md) for what counts as policy).

## Naming, per layer

**Engine (`libs/`)**: snake_case file names (`recorder_session.h`,
`capture_hub_policy.h`), namespace `exosnap::engine` (or a library-specific
sub-namespace). Types and enum values are PascalCase
(`class RecorderSession`, `enum class HubFrameKind { None, Held, Live }`).
Free functions and methods are PascalCase (`DeriveSegmentPath`,
`RecorderSession::Validate`). Members are snake_case with a trailing
underscore (`history_store_`). Constants are `kCamelCase`
(`kMinGainDb`, `kUnscopedRecordRequest`). Local variables and parameters are
snake_case (`gain_db`, `request_id`).

**Qt/App layer (`app/`, `apps/updater/`)**: PascalCase file names matching
the class they declare (`RecordViewModel.h`, `DeviceAdapter.h`), namespace
`exosnap`. Same PascalCase type/method convention as the engine. A `QObject`
exposed to QML additionally follows Qt's own convention for the members QML
reads directly: `Q_PROPERTY` names and their `READ`/`WRITE`/`NOTIFY`
accessors are camelCase (`Q_PROPERTY(bool scanning READ scanning NOTIFY
scanStateChanged FINAL)`), because QML property/signal-handler binding is
case-sensitive and camelCase is what QML expects. Prefer `FINAL` on a
`Q_PROPERTY` that no further subclass overrides.

**Rust (`tools/`)**: standard Rust naming (`snake_case` functions/modules,
`PascalCase` types), enforced by `cargo fmt` and `cargo clippy`, not by this
document.

Engine and app naming differ on purpose (snake_case vs. PascalCase file
names, engine's trailing-underscore members). Do not unify them; a
repository-wide rename for its own sake is out of scope for any change here.

## Includes and dependencies

Include order, enforced by `clang-format` (`SortIncludes`, see
`.clang-format`): the file's own header first (for a `.cpp`), then a blank
line, then third-party/system headers, then the project's own headers,
roughly nearest-namespace-last. Follow the order already in a file you are
editing rather than reordering it as a side effect of an unrelated change
(see [Formatting](build-and-test.md#developer-loop-tiers) on why formatting
and behavior changes stay in separate commits).

The engine does not include Qt headers. The app layer includes engine public
headers (`#include <exosnap/engine/recorder_session.h>`) and its own
`models/`/`capability/` headers with quotes for same-tree files. A forward
declaration is preferred over an include when only a pointer/reference is
used in the header (see `class RecordingHistoryStore;` forward-declared in
`RecordViewModel.h`, defined in its own header for `.cpp` files that need the
full type).

## Ownership and lifetime

- **RAII and value semantics** are the default: engine result types
  (`RecorderResult`, `SegmentPathResult`) are plain structs returned by
  value, not allocated.
- **Pimpl** hides platform/FFmpeg/COM state behind a stable public header:
  `RecorderSession` declares `struct Impl;` and a `std::unique_ptr<Impl>`
  member, copy is deleted, move is whatever the class needs.
- **COM/Win32 wrappers**: acquire in the constructor or an explicit
  `Open`/`Initialize` call, release in the destructor; do not leave a raw
  `HRESULT`-returning handle uninitialized across a fallible constructor.
  `CComPtr`/`wil::com_ptr`-style RAII wrappers are preferred over a bare
  interface pointer with manual `Release()`.
- **`QObject` parent ownership**: a `QObject`-derived adapter that is
  parented (`new DeviceAdapter(parent)`) is owned by that parent; do not also
  wrap it in `std::unique_ptr`. An adapter with no natural Qt parent owns
  itself via `std::unique_ptr` in its holder.
- **Borrowed pointers** (`RecorderResult* out_result`, `Type*` parameters
  without ownership transfer) are never null-unsafe by contract: the header
  comment states when null is accepted (see `Validate`'s `out_result may be
  null`) or asserts otherwise.

## Error and result conventions

At a Win32 boundary, an engine result carries a signed 32-bit `HRESULT` plus
a phase enum and a free-text detail (`RecorderResult`: `error_code`,
`error_phase`, `error_detail`). At an FFmpeg boundary, negative return codes
map to the same shape rather than being propagated as raw `int`. A
filesystem/std-library boundary uses `std::error_code`
(`SegmentPathResult::error`); this codebase does not use `std::expected`
(the project targets C++20; `<expected>` needs C++23 even on the exact MSVC
toolset this repo builds with). Prefer a small domain-specific result struct
with a named `Ok`/`Fail` factory pair over throwing across a Win32/FFmpeg/Qt
boundary. A `bool` return plus an optional out-parameter
(`bool Validate(const RecorderConfig&, RecorderResult* out_result)`) is used
where the caller usually only needs the yes/no and opts into detail.

## Threading and state machines

State that changes with the recording lifecycle (record/pause/stop, capture
hub leases, edit-session playback) is modeled as an explicit enum-driven
state machine (see `UiRecordingState` in `RecordViewModel.h`,
`CaptureHubState`/`StepCaptureHub` in `capture_hub_policy.h`) rather than a
scatter of booleans. A "step" function that computes the next state and the
actions to perform from the current state and an event is preferred over
inlining the transition logic at each call site: it makes the transition a
single reviewable/testable unit (see the comment on `StepCaptureHub` in
`capture_hub_policy.h` for why this matters for a strict Win32 invariant).
Cross-thread handoff uses the project's existing worker/queue primitives
(see `libs/engine/src/` for the capture/encode thread implementations); do
not add a new synchronization primitive type without checking whether an
existing one already covers the case.

## Comments

Explain non-obvious correctness, safety, ownership/ordering, or a
compatibility workaround - not what a line visibly does. A comment that
states a measured, verified fact (see the `<expected>`/MSVC example above,
or the DXGI single-duplication-per-process comment in
`capture_hub_policy.h`) is worth more than a restated invariant. No task/PR/
commit/session references in source comments; see
[AGENTS.md](../../AGENTS.md) for the full rule.

## Adding a target, a test and running it

1. A new engine source file goes in `libs/engine/src/` (or the matching
   `libs/<name>/src/` for another library); its public header, if any, goes
   in the matching `include/exosnap/<name>/` directory. Register new source
   files in the library's `CMakeLists.txt`.
2. A new engine test is `libs/engine/tests/test_<subject>.cpp`, registered as
   a GoogleTest binary in `libs/engine/CMakeLists.txt` next to its
   neighbors. Run it narrowly while iterating:
   ```powershell
   cargo exo-dev test --filter test_<subject>
   ```
3. A new Qt Quick adapter goes in `app/quick/ExoSnap/Quick/`, registered in
   the QML module's `CMakeLists.txt`; its QML consumer lives beside the
   other `app/quick/ExoSnap/Quick/*.qml` files. Its C++ test is
   `app/quick/tests/test_<subject>.cpp`; QML-level behavior is covered by Qt
   Quick Test under `app/quick/`. Both run through the test target
   `ci-build-debug` builds (see [Build and test](build-and-test.md)).
4. A new `exo-dev`/`exo-verify`/`exo-guest` Rust subcommand goes in the
   matching crate under `tools/<crate>/src/`, with its own `#[cfg(test)]`
   module. Run it narrowly:
   ```powershell
   cd tools
   cargo test -p exo-dev <test_name_substring>
   ```

Run the full local gate once before pushing; the pre-push hook already does
this (see [Build and test](build-and-test.md#developer-loop-tiers)).

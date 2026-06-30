# ADR 0036: MSI Auto-Harvest from CMake Staging Tree

## Status

Accepted.

## Context

The MSI installer (`packaging/msi/Package.wxs`) was built from a hand-maintained list of
individual `<Component>/<File>` elements that had drifted significantly from the actual
`cmake --install` output. Specifically, the four FFmpeg shared DLLs that `exosnap.exe`
statically imports (`avformat-62.dll`, `avcodec-62.dll`, `avutil-60.dll`, `swresample-6.dll`)
and the DirectX shader compiler DLLs (`dxcompiler.dll`, `dxil.dll`) were present in the
portable ZIP (which is derived from the CMake install staging tree) but absent from the MSI's
hand-list. This caused every MSI-installed instance to fail at load with `STATUS_DLL_NOT_FOUND`
(0xC0000135) — a regression that broke WinGet validation since approximately v0.2.0.

The portable ZIP was already built correctly: the release script (`build-release-artifacts.ps1`)
runs `cmake --install` into a staging tree, prunes known-unneeded files
(`opengl32sw.dll`, `translations/`, `plugins/bearer`, `plugins/tls`), audits static runtime
dependencies with `dumpbin /dependents`, and zips the result. The MSI build step received the
same `-d StagingDir=<pruned staging tree>` variable but ignored it for most files, using the
hand-list instead. Any new runtime DLL (FFmpeg, dxcompiler, dxil, future codec DLLs) that the
CMake install rules emit would silently be omitted from the MSI.

## Decision

Replace the hand-maintained `<Component>/<File>` list in `Package.wxs` with a WiX v4
`<Files Include="$(var.StagingDir)\**" />` harvest inside a `<ComponentGroup>`:

```xml
<ComponentGroup Id="AppFiles" Directory="INSTALLFOLDER">
  <Files Include="$(var.StagingDir)\**" />
</ComponentGroup>
```

WiX v4 (≥ 4.0.0) processes this glob at build time, creates one `<Component>` per file with
an auto-generated stable GUID, and preserves the relative directory structure under
`INSTALLFOLDER` — so `plugins/platforms/qwindows.dll`, `licenses/qt.txt`, etc. are installed
in the correct subdirectories without any per-file maintenance.

The `$(var.IncludeCrashpad)` preprocessor conditional is removed: `crashpad_handler.exe` is
included in the MSI iff it is present in the staging tree (identical behaviour to the portable
ZIP). No `-d IncludeCrashpad=...` argument is passed to `wix build`.

As a defence-in-depth measure, `build-release-artifacts.ps1` now runs an MSI content
assertion immediately after a successful `wix build`: it administratively extracts the MSI
(`msiexec /a ... TARGETDIR=<tmp>`) and verifies that every `*.dll` and `*.exe` under the
staging tree is present by name in the extracted MSI. Any discrepancy calls `Add-Error` and
fails the release build (exit 1).

## Consequences

- **Correct-by-construction:** the MSI and portable ZIP are always derived from the same
  source (the pruned CMake staging tree). A new runtime DLL added by a future CMake install
  rule is automatically included in both artifacts with no manual update required.
- **Defence-in-depth:** the post-build MSI content assertion catches any future drift before
  a release can ship with a broken MSI.
- **UpgradeCode unchanged:** `8988DAFC-3AE4-4788-BA6D-62E3F73C7A7D` is permanent (ADR 0034).
  ProductCode remains auto-generated per build (`*`). No existing installation path changes.
- **Start Menu shortcut GUID unchanged:** `{C1D2E3F4-A5B6-7890-CDEF-1234567890AB}` is stable.
  Only the file components change from hand-listed to auto-harvested.
- **WiX version dependency:** requires WiX v4 or later (already the minimum; the `wix` .NET
  global tool is the build dependency, not the deprecated WiX v3 / `heat.exe` toolchain).

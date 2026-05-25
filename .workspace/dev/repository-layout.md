#Repository Layout

This repository keeps source code and generated artifacts strictly separated.

## Top-level conventions

- `apps/` contains executable applications and app-specific UI/runtime code.
- `libs/` contains reusable libraries shared by one or more applications.
- `scripts/` contains developer and packaging automation scripts.
- `.workspace/` contains product, architecture, UI specs, decisions, design reference, and process documentation.
- `build/` contains generated CMake build trees and local compile outputs.
- `dist/` contains generated packaging outputs (ZIPs, checksums, staging content).

## Distribution target shape

- Final distribution should provide `exosnap.exe` as the primary GUI entry point.
- Final distribution may provide a separate CLI binary (`exosnap-cli.exe`) for automation and validation flows.
- Keeping GUI and CLI as separate applications under `apps/` is intentional and fits the current repository model.
- Repository layout work must not force a large rename or directory move just to reach this distribution shape.

## PATH policy

- Alpha ZIP packages must not modify user PATH automatically.
- CLI PATH integration belongs to installer scope in a later release (optional installer checkbox or package-manager integration).
- For local development or manual ZIP usage, PATH changes are explicit user actions.

## `build/` vs `dist/`

- `build/` is for compiler/linker/test artifacts produced while configuring and building.
- `dist/` is for distributable artifacts produced by packaging workflows.
- Both are generated, machine-local outputs and must not be committed.

## Why generated artifacts stay out of Git

- They are reproducible from source and scripts.
- They create noisy diffs and merge conflicts.
- They are platform/toolchain specific and reduce repository portability.
- Keeping them ignored preserves review quality and clean history.

## Alpha package generation

Run:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package-alpha.ps1
```

Expected outputs are generated under `dist/`, including:

- `dist/exosnap-alpha-YYYY-MM-DD.zip`
- `dist/exosnap-alpha-YYYY-MM-DD.sha256`

See [`scripts/package-alpha.ps1`](C:\Users\User\Development\exosnap\scripts\package-alpha.ps1) for the canonical packaging flow.

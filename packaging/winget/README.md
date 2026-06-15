# WinGet manifests — Codexo.ExoSnap

Source-of-truth WinGet manifests for ExoSnap, mirroring the `microsoft/winget-pkgs`
layout under [`manifests/c/Codexo/ExoSnap/<version>/`](manifests/c/Codexo/ExoSnap).

Each version directory holds the standard multi-file manifest set (schema 1.10.0):

- `Codexo.ExoSnap.yaml` — version manifest
- `Codexo.ExoSnap.installer.yaml` — `wix` MSI installer (machine scope), with the
  release `InstallerSha256`, `ProductCode`, and the permanent `UpgradeCode`
- `Codexo.ExoSnap.locale.en-US.yaml` — default-locale metadata

## Submission

Validate, then submit to `microsoft/winget-pkgs` with the standalone tooling:

```powershell
winget validate --manifest packaging/winget/manifests/c/Codexo/ExoSnap/<version>
wingetcreate submit --prtitle "New package: Codexo.ExoSnap version <version>" `
  --token <github-token> packaging/winget/manifests/c/Codexo/ExoSnap/<version>
```

The installer always points at the immutable public GitHub Release MSI asset and its
verified SHA-256 — never a mutable or pre-release URL.

## Runtime dependency

ExoSnap (`exosnap.exe` and the shipped Qt6 DLLs) links the dynamic MSVC runtime
(`/MD`, the project default). The MSI does **not** bundle `VCRUNTIME140.dll`,
`MSVCP140.dll`, or the related runtime DLLs. `Codexo.ExoSnap.installer.yaml`
declares `Microsoft.VCRedist.2015+.x64` under `Dependencies.PackageDependencies`,
so WinGet installs that package automatically before installing ExoSnap.

The 0.1.0 WinGet PR validation initially failed sandbox install with
`STATUS_DLL_NOT_FOUND` (`0xC0000135`) because this dependency was missing from
the manifest — on a clean machine without the redistributable, `exosnap.exe`
cannot start. Do not remove the `Dependencies` block;
`scripts/validate-winget-manifest.ps1` enforces it as a regression guard.

## Status

- **0.4.0** — current manifest in this directory; to be submitted to `microsoft/winget-pkgs` for the v0.4.0 release. `InstallerSha256` and `ProductCode` are **placeholders** (all-zero SHA, null GUID) and must be filled from the official MSI build at release time — see `.workspace/release/0.4.0-release-checklist.md`.
- **0.3.0** — earlier submission for the v0.3.0 release.
- **0.2.0** — earlier submission: https://github.com/microsoft/winget-pkgs/pull/387393
- **0.1.0** — earlier submission: https://github.com/microsoft/winget-pkgs/pull/386449

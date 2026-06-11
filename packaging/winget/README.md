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

## Status

- **0.1.0** — submitted: https://github.com/microsoft/winget-pkgs/pull/386449

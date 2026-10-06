# WinGet manifests: Codexo.ExoSnap

Source-of-truth WinGet manifests for ExoSnap, mirroring the `microsoft/winget-pkgs` layout under [`manifests/c/Codexo/ExoSnap/<version>/`](manifests/c/Codexo/ExoSnap).

Each version directory holds the standard multi-file manifest set (schema 1.10.0):

- `Codexo.ExoSnap.yaml`: version manifest
- `Codexo.ExoSnap.installer.yaml`: `wix` MSI installer (machine scope), with the release `InstallerSha256`, `ProductCode`, and the permanent `UpgradeCode`
- `Codexo.ExoSnap.locale.en-US.yaml`: default-locale metadata

## Preparation and submission

The protected `Distribute release` workflow owns submission. It resolves the immutable public GitHub Release, renders this manifest set with the published MSI SHA-256, the MSI's real ProductCode, the permanent UpgradeCode and the release date, runs `winget validate`, and freezes the exact manifests into the distribution readiness report. After the `distribution` environment approval it submits those frozen manifests to `microsoft/winget-pkgs` with the official WingetCreate program, which owns the upstream pull request.

The same steps are available locally:

```powershell
cargo exo-dev distribution prepare --version <x.y.z> --source-commit <sha> `
    --release-json release.json --assets assets --out prepared
winget validate --manifest prepared/packaging/winget/manifests/c/Codexo/ExoSnap/<x.y.z>
```

The installer always points at the immutable public GitHub Release MSI asset and its verified SHA-256, never a mutable or pre-release URL.

## Runtime dependency

ExoSnap (`exosnap.exe` and the shipped Qt6 DLLs) links the dynamic MSVC runtime (`/MD`, the project default). The MSI does **not** bundle `VCRUNTIME140.dll`, `MSVCP140.dll`, or the related runtime DLLs. `Codexo.ExoSnap.installer.yaml` declares `Microsoft.VCRedist.2015+.x64` under `Dependencies.PackageDependencies`, so WinGet installs that package automatically before installing ExoSnap.

On a clean machine without the redistributable, `exosnap.exe` cannot start. Keep the `Dependencies` block. `cargo exo-dev packaging winget` enforces it.

## Installation ownership

`InstallerSwitches.Custom` passes `EXOSNAP_DISTRIBUTION_OWNER=winget` to the MSI. The MSI persists the owner beside `installed` and `InstallPath` in the 64-bit `HKLM\Software\ExoSnap` product record. A recognized explicit caller replaces an existing owner. Without that caller property, repair and major upgrades retain any nonempty persisted owner, including an unrecognized value. A missing caller and missing marker resolve to `direct`.

WinGet's `--override` replaces manifest installer switches and can bypass this marker. ExoSnap does not discover WinGet installation history at runtime, so the manifest is not a runtime ownership guarantee. A WinGet installation created before this marker defaults to Direct until an upgrade passes the owner property. Direct Setup and a raw MSI preserve an existing manager marker unless a recognized explicit property replaces it.

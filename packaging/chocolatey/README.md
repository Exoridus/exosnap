# Chocolatey package - exosnap

Source-of-truth Chocolatey package for ExoSnap, submitted to the [community repository](https://community.chocolatey.org/packages/exosnap).

- `exosnap.nuspec` - package metadata and the `vcredist140` dependency
- `tools/chocolateyinstall.ps1` - downloads and installs the official MSI

This is a thin download-at-install package. It embeds no binaries, which is why it needs neither `tools/LICENSE.txt` nor `tools/VERIFICATION.txt`: those are required only when a package ships the software itself. `url64bit` always points at the immutable public GitHub Release MSI asset, never at a mutable or pre-release URL, and `checksum64` is the SHA-256 published beside it.

## External package adapter

Chocolatey's [MSI package contract](https://docs.chocolatey.org/en-us/guides/create/create-msi-package/) uses `chocolateyinstall.ps1` to pass the remote URL, SHA-256, architecture and silent arguments to `Install-ChocolateyPackage`. The nuspec has no equivalent remote-MSI installation fields. This one file is the exact external-format exception in `exo-dev check automation-policy`; it contains only package metadata and Chocolatey helper calls. Rust owns package generation, version/checksum validation and distribution policy. The same documented contract supports automatic MSI uninstall without `chocolateyuninstall.ps1`.

## The checksum placeholder

Between a version bump and the published release the MSI does not exist, so no real hash can be written. `checksum64` carries 64 zeros in that window, and `cargo exo-dev packaging chocolatey` fails on it unconditionally so it cannot be packed or pushed by accident. Fill it from the `ExoSnap-<x.y.z>-windows-x64.msi.sha256` sidecar once the release workflow has published it. The full release order is in `docs/release-checklist.md` §8.

## Preparation and submission

The protected `Distribute release` workflow owns submission. It resolves the immutable public GitHub Release, renders this package with the published MSI hash, runs the full validator, packs `exosnap.<x.y.z>.nupkg`, rehearses install/uninstall/restore on a disposable runner, and freezes a distribution readiness report. After the `distribution` environment approval it pushes exactly that frozen `.nupkg`.

The same steps are available locally:

```powershell
cargo exo-dev distribution prepare --version <x.y.z> --source-commit <sha> `
    --release-json release.json --assets assets --out prepared
cargo exo-verify rehearse --channel chocolatey `
    --package-source prepared/packaging/chocolatey `
    --installer assets/ExoSnap-<x.y.z>-windows-x64.msi --out rehearsal
cargo exo-dev distribution validate --prepared prepared --assets assets `
    --chocolatey-rehearsal rehearsal/chocolatey-rehearsal.json
```

The version-only validator (`cargo exo-dev packaging chocolatey --version-only`) remains the pull-request gate before a release exists.

A submission is reviewed by a human moderator after two automated services have run: the *validator*, which checks the metadata and the automation scripts against the published rules, and the *verifier*, which installs and uninstalls the package on a clean machine. The validator's mechanically checkable rules are mirrored in `cargo exo-dev packaging chocolatey` so a failure costs a local second rather than a review round trip. When a moderator asks for a change, the corrected package is pushed under the **same** version, not a new one.

## Uninstall

The MSI owns the ExoSnap installation, Add/Remove Programs entry and Start Menu shortcut. Empty shared parent directories/registry keys are not necessarily owned or removed by that product. Chocolatey records the MSI installation and its automatic uninstaller delegates to Windows Installer. No repository uninstall hook is needed. User configuration under `%LOCALAPPDATA%\ExoSnap` is not installed by the MSI and deliberately survives an uninstall.

## Runtime dependency

ExoSnap (`exosnap.exe` and the shipped Qt6 DLLs) links the dynamic MSVC runtime (`/MD`, the project default). The MSI does **not** bundle `VCRUNTIME140.dll`, `MSVCP140.dll`, or the related runtime DLLs. On a clean machine without the redistributable, `exosnap.exe` cannot start and fails with `STATUS_DLL_NOT_FOUND` (`0xC0000135`). The nuspec declares `vcredist140` so Chocolatey installs it first.

Its version is a floor, not a preference: the redistributable must be at least as new as the MSVC toolset that built the release binaries, because a newer toolset may emit calls to runtime exports an older redistributable does not export. Release MSIs are built by the GitHub Actions `windows-2022` image, so the floor tracks that image's MSVC toolset and has to be re-checked whenever the image moves.

Publication follows the current [publication policy](../publication-policy.json) and [release checklist](../../docs/release-checklist.md#8-package-manager-publication). A package source file is not evidence that the community feed has published it.

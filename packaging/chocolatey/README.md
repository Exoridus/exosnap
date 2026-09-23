# Chocolatey package - exosnap

Source-of-truth Chocolatey package for ExoSnap, submitted to the [community repository](https://community.chocolatey.org/packages/exosnap).

- `exosnap.nuspec` - package metadata and the `vcredist140` dependency
- `tools/chocolateyinstall.ps1` - downloads and installs the official MSI
- `tools/chocolateyuninstall.ps1` - hands the ProductCode back to `msiexec`

This is a thin download-at-install package. It embeds no binaries, which is why it needs neither `tools/LICENSE.txt` nor `tools/VERIFICATION.txt`: those are required only when a package ships the software itself. `url64bit` always points at the immutable public GitHub Release MSI asset, never at a mutable or pre-release URL, and `checksum64` is the SHA-256 published beside it.

## The checksum placeholder

Between a version bump and the published release the MSI does not exist, so no real hash can be written. `checksum64` carries 64 zeros in that window, and `scripts/validate-chocolatey-package.ps1` fails on it unconditionally so it cannot be packed or pushed by accident. Fill it from the `ExoSnap-<x.y.z>-windows-x64.msi.sha256` sidecar once the release workflow has published it. The full release order is in `docs/release-checklist.md` §8.

## Submission

```powershell
pwsh scripts/validate-chocolatey-package.ps1 -Version <x.y.z> -RequireManifest -ManifestPath <manifest>
choco pack packaging/chocolatey/exosnap.nuspec --output-directory $env:TEMP/exosnap-choco
choco push $env:TEMP/exosnap-choco/exosnap.<x.y.z>.nupkg --source https://push.chocolatey.org/
```

`<manifest>` is the `artifact-manifest.json` written by `scripts/build-release-artifacts.ps1`. `docs/release-checklist.md` §8 names its location. `-RequireManifest` is mandatory for a real submission. Without it, a missing manifest skips the checksum cross-check instead of failing it.

A submission is reviewed by a human moderator after two automated services have run: the *validator*, which checks the metadata and the automation scripts against the published rules, and the *verifier*, which installs and uninstalls the package on a clean machine. The validator's mechanically checkable rules are mirrored in `scripts/validate-chocolatey-package.ps1` so a failure costs a local second rather than a review round trip. When a moderator asks for a change, the corrected package is pushed under the **same** version, not a new one.

## Uninstall

The MSI owns the ExoSnap installation, Add/Remove Programs entry and Start Menu shortcut. Empty shared parent directories/registry keys are not necessarily owned or removed by that product. `tools/chocolateyuninstall.ps1` therefore only resolves the ProductCode and delegates. It deletes nothing itself. User configuration under `%LOCALAPPDATA%\ExoSnap` is not installed by the MSI and deliberately survives an uninstall.

## Runtime dependency

ExoSnap (`exosnap.exe` and the shipped Qt6 DLLs) links the dynamic MSVC runtime (`/MD`, the project default). The MSI does **not** bundle `VCRUNTIME140.dll`, `MSVCP140.dll`, or the related runtime DLLs. On a clean machine without the redistributable, `exosnap.exe` cannot start and fails with `STATUS_DLL_NOT_FOUND` (`0xC0000135`). The nuspec declares `vcredist140` so Chocolatey installs it first.

Its version is a floor, not a preference: the redistributable must be at least as new as the MSVC toolset that built the release binaries, because a newer toolset may emit calls to runtime exports an older redistributable does not export. Release MSIs are built by the GitHub Actions `windows-2022` image, so the floor tracks that image's MSVC toolset and has to be re-checked whenever the image moves.

Publication follows the current [publication policy](../publication-policy.json) and [release checklist](../../docs/release-checklist.md#8-package-manager-publication). A package source file is not evidence that the community feed has published it.

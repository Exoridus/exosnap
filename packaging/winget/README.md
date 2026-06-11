# WinGet Manifest Template — Codexo.ExoSnap

**Status:** Template only. Not submitted to WinGet. Not published.

This template documents the expected future WinGet manifest structure for ExoSnap.
All values marked `REQUIRED_BEFORE_SUBMISSION` must be filled before submission.

## Package Identity

```yaml
PackageIdentifier: Codexo.ExoSnap
Publisher: Codexo
PackageName: ExoSnap
License: GPL-3.0-or-later
LicenseUrl: https://github.com/Exoridus/exosnap/blob/main/LICENSE
PackageUrl: https://github.com/Exoridus/exosnap
Description: Windows-native diagnostics-first recording application
Moniker: exosnap
Tags:
  - recorder
  - screen-capture
  - nvenc
  - av1
```

## Installer

```yaml
PackageVersion: REQUIRED_BEFORE_SUBMISSION  # e.g. 0.1.0
InstallerType: wix
Architecture: x64
Scope: machine
InstallerUrl: REQUIRED_BEFORE_SUBMISSION   # URL to ExoSnap-<version>-windows-x64.msi
InstallerSha256: REQUIRED_BEFORE_SUBMISSION
ProductCode: REQUIRED_BEFORE_SUBMISSION
UpgradeBehavior: install
ReleaseDate: REQUIRED_BEFORE_SUBMISSION
ReleaseNotesUrl: REQUIRED_BEFORE_SUBMISSION
MinimumOSVersion: 10.0.17763.0
```

## Future Submission

Use `wingetcreate` to generate and submit the manifest:

```powershell
wingetcreate new `
  --PackageIdentifier Codexo.ExoSnap `
  --PackageVersion <version> `
  --InstallerType wix `
  --InstallerUrl <url> `
  --InstallerSha256 <sha256> `
  https://github.com/Exoridus/exosnap/releases/download/v<version>/ExoSnap-<version>-windows-x64.msi
```

**Do not submit until:**
1. A public GitHub Release exists
2. The MSI is publicly downloadable
3. The repository is public
4. All product decisions are finalized

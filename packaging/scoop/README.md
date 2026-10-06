# Scoop packaging

ExoSnap is distributed via the official bucket [`Exoridus/scoop-exosnap`](https://github.com/Exoridus/scoop-exosnap):

```powershell
scoop bucket add exosnap https://github.com/Exoridus/scoop-exosnap
scoop install exosnap
```

`exosnap.json` in this directory is the canonical manifest source. It keeps the `depends` field for the main repository; the distribution preparation renders a bucket copy without it, because `extras/vcredist2022` only resolves for users who have the Extras bucket added, so the standalone bucket documents the VC++ runtime requirement in `notes` instead.

The protected `Distribute release` workflow updates `bucket/exosnap.json` in the bucket repository after preparation and validation. A user's `scoop update` refreshes the locally cloned bucket and downloads the version the committed manifest names; it does not change the remote manifest, and the bucket only receives a new version when the workflow commits one.

The manifest installs the portable archive and extracts `exosnap.exe` at the version directory root. Scoop supplies `scoop-manifest.json` and `scoop-install.json` alongside it. Older installations use `manifest.json` and `install.json`, which Scoop still reads as fallbacks. The manifest is the installed package JSON. Install metadata records the architecture and bucket or source manifest URL, with null fields omitted. See Scoop's [metadata persistence](https://github.com/ScoopInstaller/Scoop/blob/master/lib/manifest.ps1) and [installation flow](https://github.com/ScoopInstaller/Scoop/blob/master/lib/install.ps1).

ExoSnap uses the matching metadata pair and application directory context to recognize Scoop ownership. Scoop does not run the MSI or write its registry ownership marker. Keep the root executable shortcut and portable archive layout consistent with those assumptions.

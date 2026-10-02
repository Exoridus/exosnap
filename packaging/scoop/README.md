# Scoop packaging

ExoSnap is distributed via the official bucket [`Exoridus/scoop-exosnap`](https://github.com/Exoridus/scoop-exosnap):

```powershell
scoop bucket add exosnap https://github.com/Exoridus/scoop-exosnap
scoop install exosnap
```

`exosnap.json` in this directory is the canonical manifest source. It keeps the `depends` field for the main repository; the distribution preparation renders a bucket copy without it, because `extras/vcredist2022` only resolves for users who have the Extras bucket added, so the standalone bucket documents the VC++ runtime requirement in `notes` instead.

The protected `Distribute release` workflow updates `bucket/exosnap.json` in the bucket repository after preparation and validation. A user's `scoop update` refreshes the locally cloned bucket and downloads the version the committed manifest names; it does not change the remote manifest, and the bucket only receives a new version when the workflow commits one.

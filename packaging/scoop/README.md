# Scoop packaging

ExoSnap is distributed via the official bucket [`Exoridus/scoop-exosnap`](https://github.com/Exoridus/scoop-exosnap):

```powershell
scoop bucket add exosnap https://github.com/Exoridus/scoop-exosnap
scoop install exosnap
```

`exosnap.json` in this directory is the canonical manifest source. On each release, copy it into the bucket repository (`bucket/exosnap.json`) without the `depends` field. `extras/vcredist2022` only resolves for users who have the Extras bucket added, so the standalone bucket documents the VC++ runtime requirement in `notes`/README instead of hard-depending on it.

# Scoop packaging

ExoSnap is distributed via the official bucket
[`Exoridus/scoop-exosnap`](https://github.com/Exoridus/scoop-exosnap):

```powershell
scoop bucket add exosnap https://github.com/Exoridus/scoop-exosnap
scoop install exosnap
```

`exosnap.json` in this directory is the canonical manifest source. On each
release, copy it into the bucket repository (`bucket/exosnap.json`) WITHOUT the
`depends` field — `extras/vcredist2022` only resolves for users who have the
Extras bucket added, so the standalone bucket documents the VC++ runtime
requirement in `notes`/README instead of hard-depending on it.

History: the ScoopInstaller/Extras submission (PR #18018) was declined because
the project does not yet meet the Extras popularity criteria (~100 stars /
50 forks). Resubmit to Extras once those are met; until then the own bucket is
the supported channel.

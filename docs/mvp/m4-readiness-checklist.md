# M4 MVP Readiness Checklist

## 1. Status

- RecordPage is the primary MVP recording surface.
- OutputPage is visibly wired for output folder, naming, and container/audio codec selection.
- VideoPage and AudioPage are read-only/locked for MVP.
- Engine path: WGC + NVENC AV1 + MKV + WASAPI + Opus/AAC.
- MP4, Region Capture, Video Quality Controls, Hotkeys, and Auto-Split are intentionally deferred.
- Settings persistence is implemented for MVP-relevant configuration.
- Alpha packaging script exists at `scripts/package-alpha.ps1`.
- Generated `/dist/` artifacts are gitignored.
- `KNOWN_LIMITATIONS.txt` exists and is shipped in alpha packages.

## 2. MVP-ready features

- Monitor capture
- Window capture for normal windows
- Output folder selection
- Output settings persistence
- Naming pattern `{date}` / `{time}`
- MKV output
- AV1 video
- Opus/AAC audio codec selection
- System audio
- App audio for Window targets
- Mic recording
- Mic device selection
- Mic channel mode
- Audio settings persistence
- Separate output audio tracks
- Live audio activity meters
- Result/error diagnostics
- Open Folder action

## 3. Locked / deferred for MVP

- VideoPage controls are read-only
- AudioPage controls are read-only
- MP4 disabled
- Region capture not available
- Global hotkey not registered
- Auto split disabled/deferred
- Video quality/FPS/resolution settings fixed
- Disk-space preflight deferred
- AudioPage live routing deferred

## 4. Minimal manual smoke matrix

| ID | Scenario | Expected |
| -- | -------- | -------- |
| S1 | Monitor target -> Start -> 30s -> Stop | MKV file in configured folder, AV1 video, selected audio tracks. |
| S2 | Output folder changed on OutputPage -> Record | File lands in selected folder. |
| S3 | Naming pattern changed -> Record | Filename follows pattern and timestamp tokens. |
| S4 | Mic enabled + selected device -> Record | Extra Opus/AAC mic track exists; no crash. |
| S5 | OutputPage AAC selected -> Record -> ffprobe | Audio codec is AAC, not Opus. |
| S6 | Window target with App + System separate tracks -> Record | AV1 video + 2 audio tracks; no stale PID error. |
| S7 | Close selected window before/while recording | User-facing "Window closed" or equivalent diagnostics, no crash. |
| S8 | Invalid/unwritable output folder | Recording does not start and shows output-folder error. |
| S9 | Open Folder after successful recording | Explorer opens output directory. |

## 5. Known limitations

- Settings persist locally (INI format) and may change in later alpha builds.
- MP4 UI is visible but disabled.
- VideoPage/AudioPage are informational for MVP.
- Actual GPU model is not shown in sidebar; generic NVENC is shown.
- Audio meters are live during recording only, not pre-monitoring.
- Hotkey badge removed/neutralized; no global hotkey in MVP.
- No automatic hot-plug handling for mic devices; use refresh.
- Window capture can fail for minimized/cloaked/too-small windows; diagnostics should explain this.

## 6. Pre-alpha gate

- [ ] Build `exosnap`
- [ ] Run output/diagnostics/viewmodel/recorder_core/capability tests
- [ ] Run manual smokes S1-S9
- [ ] Run `powershell -ExecutionPolicy Bypass -File scripts/package-alpha.ps1`
- [ ] Verify `dist/exosnap-alpha-YYYY-MM-DD.zip` and matching `.sha256` are generated
- [ ] Run ZIP sanity checks (required Qt release DLLs, `platforms/qwindows.dll`, no `Qt6*d.dll`, no test executables)
- [ ] Inspect RecordPage, OutputPage, VideoPage, AudioPage for no placeholder copy
- [ ] Verify git status clean
- [ ] Tag or package only after passing the above

## 7. Out-of-scope for this MVP

- MP4 recording
- Full VideoPage wiring
- Full AudioPage routing editor
- Region capture
- Recording presets
- Global hotkeys
- Installer/signing

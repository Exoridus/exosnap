# Alpha Final Smoke Report

## Build / Package

| Item | Value |
| ---- | ----- |
| Commit SHA | d3726db |
| Commit title | polish(ui): Phase 19B visual and UX cleanup |
| Package command | `powershell -ExecutionPolicy Bypass -File scripts/package-alpha.ps1` |
| ZIP path | `dist/exosnap-alpha-2026-05-23.zip` |
| SHA256 path | `dist/exosnap-alpha-2026-05-23.sha256` |
| SHA256 | `E10C0E09EBBA54098CB31309819DD413F844583751AA6C8DCC306F3CA293A3E4` |
| ZIP size | 21.6 MB |
| Staging path | `dist/staging/exosnap-alpha-2026-05-23/` |

## Package Sanity

| Check | Result |
| ----- | ------ |
| `exosnap.exe` | PASS |
| `KNOWN_LIMITATIONS.txt` | PASS |
| `Qt6Core.dll` | PASS |
| `Qt6Gui.dll` | PASS |
| `Qt6Widgets.dll` | PASS |
| `Qt6Svg.dll` | PASS |
| `platforms/qwindows.dll` | PASS |
| No `Qt6*d.dll` (debug DLLs) | PASS |
| No `.pdb` files | PASS |
| No test executables | PASS |

## Manual Validation Summary

### Phase 19C Visual Smoke — PASS (2026-05-23)

| Area | Result |
| ---- | ------ |
| Monitor / Window target cards | PASS |
| PreviewSurface honest alpha-state (placeholder shown) | PASS |
| Audio activity / RMS meters | PASS |
| Mic preflight meter visible before recording | PASS |
| Sidebar footer | PASS |
| Small window 1120×700 layout stable | PASS |
| Scroll / Destination visible | PASS |
| UI regressions | None found |

### Audio MVP — PASS (validated iteratively through Phases 17–19B)

| Area | Result |
| ---- | ------ |
| Merge-first WebM output (AV1 + Opus stereo) | PASS |
| 1 AV1 video stream + 1 Opus stereo audio stream | PASS |
| Mic preflight meter active before recording | PASS |
| Mic gain / boost controls functional | PASS |
| Mic auto channel mode balanced | PASS |
| SYS/APP meters activate on recording start | PASS |
| Separate output tracks UI hidden (deferred) | PASS |

### Filename / Output

| Area | Result |
| ---- | ------ |
| `{app}` / `{datetime}` token routing | PASS |
| Output folder selection persists | PASS |

## Open Known Limitations

- WebM only in alpha; MP4 visible but disabled
- Live preview unavailable in alpha (honest alpha-state placeholder shown)
- Region capture unavailable
- Separate output tracks hidden / deferred
- Global hotkeys not registered in this build
- No installer / no code signing (SmartScreen warning on first launch)
- Mic hot-plug not automatic; use Refresh button
- SYS/APP audio meters inactive before recording start (mic preflight meter is active)
- Settings INI format may change in later alpha builds

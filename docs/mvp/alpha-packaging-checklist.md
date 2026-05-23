# Alpha Packaging Checklist

## Build/package

- Run `powershell -ExecutionPolicy Bypass -File scripts/package-alpha.ps1`
- Expect `dist/exosnap-alpha-YYYY-MM-DD.zip`
- Expect `dist/exosnap-alpha-YYYY-MM-DD.sha256`

## ZIP sanity

- `exosnap.exe` exists
- `KNOWN_LIMITATIONS.txt` exists
- Qt release DLLs exist:
  - `Qt6Core.dll`
  - `Qt6Gui.dll`
  - `Qt6Widgets.dll`
  - `Qt6Svg.dll`
- `platforms/qwindows.dll` exists
- No `Qt6*d.dll`
- No test executables
- No `.pdb` by default

## Runtime smoke

- Unzip into fresh folder
- Run `exosnap.exe`
- App starts without Qt in PATH
- Settings persistence does not require admin rights
- Output folder can be changed
- Short monitor recording works
- Generated WebM appears in configured folder
- `ffprobe` shows AV1 video + selected audio codec

## Manual MVP smoke summary

- Run S1-S9 from `docs/mvp/m4-readiness-checklist.md`

# ExoSnap 1.0.0 END-Spec

## 1. Produktziel

ExoSnap 1.0.0 ist ein Windows-first Screen-/App-/Region-Recorder mit professioneller Recording-Pipeline, NVENC-Fokus, flexiblen Container-/Codec-Profilen, Live Preview, Webcam Overlay, Track-Routing, Diagnose- und Performance-Analyse.

**Primäres Ziel:**

```text
Windows optimal
NVIDIA NVENC first
MKV default
MP4 + WebM als alternative Profile
Monitor/App/Region Capture
Mic/App/System Audio
Webcam Overlay
Diagnostics + Pipeline Trace
portable ZIP + Installer/Signing als 1.0-Ziel
```

**Linux:** geplant, aber **kein Blocker für Windows 1.0**.

---

# 2. Plattformstrategie

## 2.1 Windows 1.0

Windows ist die Produktionsplattform für 1.0.

**MUSS:**

* Windows 11 primär
* Windows 10 optional/best effort
* Windows Graphics Capture für Monitor/App/Region
* WASAPI für Mic/App/System Audio
* NVIDIA NVENC für Video-Encoding
* Media Foundation / eigene Encoder-/Muxer-Backends, wo sinnvoll
* MKV, MP4, WebM als echte Container, kein Fake-Labeling

## 2.2 Linux planned

Linux ist geplant, aber nicht 1.0-blockierend.

Linux-Ziel später:

* NVIDIA/NVENC-first
* kein Pflicht-Software-Encoder-Fallback
* PipeWire/Portal für Wayland
* X11 optional
* Monitor Capture
* Region Crop
* Mic + System Audio
* App/Window Capture nur, wo Portal/Compositor es erlaubt
* MKV/WebM zuerst
* MP4 abhängig von cross-platform Muxer-Strategie

---

# 3. Dependency- und Muxing-Policy

FFmpeg/libavformat ist **nicht grundsätzlich verboten**.

ExoSnap soll weiterhin besitzen:

* Capture-Logik
* Timing/Synchronisation
* Encoder-Konfiguration
* Capability-Validierung
* Diagnostics
* UI/Recording-Profile

Externe Media-Libraries sind erlaubt, wenn sie klar helfen bei:

* Container-Korrektheit
* Codec-Kompatibilität
* Performance
* Debuggability
* Wartbarkeit
* Cross-platform support

**Nicht Ziel:** ExoSnap als dünner Wrapper um `ffmpeg.exe`.

**Erlaubt:** libavformat als möglicher Muxing-Backend-Kandidat, falls eigener/aktueller Muxing-Pfad zu fehleranfällig wird.

---

# 4. Capture-Ziele

## 4.1 Monitor Capture

MUSS:

* bestimmter Monitor auswählbar
* Multi-Monitor-Support
* lesbare Namen: `Display 1`, `Display 2`
* Refresh / Hotplug-Handling
* Windowed, fullscreen, borderless fullscreen/windowed soweit technisch möglich
* bei exklusivem Fullscreen klare Diagnose/Empfehlung, falls Capture nicht verfügbar ist

## 4.2 App / Window Capture

MUSS:

* bestimmte App/Fenster auswählbar
* app-first Labels:

```text
Brave — Claude
Discord — Voice
Visual Studio Code — RecordPage.cpp
```

* Sortierung nach App, dann Titel
* Refresh
* Target lost Handling
* App-Audio-Zuordnung, soweit möglich

## 4.3 Region Capture

MUSS:

* vordefinierte Region
* Region per Overlay auswählbar
* Snipping-Tool-artiger Select-on-record-Modus
* Escape bricht Auswahl ab
* zu kleine Region ungültig
* Countdown startet **erst nach** gültiger Region-Auswahl
* Region als Crop vom Desktop/Monitor-Capture bevorzugt

## 4.4 Selection Border

MUSS:

* bei Auswahl kurz anzeigen
* bei Target-Picker/Region-Auswahl sichtbar
* während laufender Aufnahme **nicht** anzeigen

---

# 5. Video-Encoding

## 5.1 NVENC-Codecs

1.0 soll alle praktisch sinnvollen NVENC-Codecs unterstützen, sofern Hardware/Driver sie wirklich anbieten.

| Codec       | 1.0 Ziel                                    |
| ----------- | ------------------------------------------- |
| AV1 NVENC   | MUSS, wenn Hardware verfügbar               |
| H.264 NVENC | MUSS                                        |
| HEVC NVENC  | MUSS/SOLL, sofern sinnvoll implementierbar  |
| VP9 NVENC   | nur falls echte NVENC-Unterstützung + Smoke |

Keine UI-Freigabe ohne Capability-Probe und Smoke.

## 5.2 Framerate

MUSS:

```text
30 fps
60 fps
120 fps
Native
Custom
```

Default:

```text
CFR 60 fps
```

Native bedeutet z. B.:

```text
144 Hz Monitor → 144 fps
165 Hz Monitor → 165 fps
```

aber nur mit Capability-/Performance-Warnung.

## 5.3 CFR / VFR

MUSS:

* CFR als Default
* VFR als Advanced Option

CFR für Kompatibilität/Schnittprogramme, VFR für Preserve Timing/Advanced.

## 5.4 SDR / HDR

MUSS:

* SDR 8-bit

SOLL:

* HDR / 10-bit vorbereitet
* Diagnostics erkennt HDR-Kontext
* kein 1.0-Blocker, falls aufwendig

---

# 6. Audio-Codecs

## 6.1 Final für 1.0

```text
Opus
AAC-LC
FLAC
PCM
```

Nicht 1.0:

```text
Vorbis
Dolby / AC-3 / E-AC-3
```

## 6.2 AAC vs AAC-LC

In UI:

```text
AAC
```

Intern/Dokumentation:

```text
AAC = AAC-LC
```

AAC-LC ist das relevante Standardprofil. HE-AAC/xHE-AAC sind eher Low-Bitrate-Streaming-Themen und nicht 1.0-Ziel.

## 6.3 Codec-Auswahl pro Recording

1.0-Regel:

```text
Ein Audio-Codec pro Recording/Profile.
Alle Audiospuren nutzen denselben Audio-Codec.
```

Später optional:

```text
Per-track codec override
```

---

# 7. Container-/Codec-Matrix

## 7.1 Container

| Container      | Rolle               |
| -------------- | ------------------- |
| MKV / Matroska | Standard / flexibel |
| MP4            | Kompatibilität      |
| WebM           | offen/browsernah    |

## 7.2 Default

```text
Default: MKV
Default Profile: MKV + H.264 + AAC
```

## 7.3 1.0 Zielmatrix

### MKV

| Video | Audio | Status                                      |
| ----- | ----- | ------------------------------------------- |
| H.264 | AAC   | MUSS / Default                              |
| H.264 | Opus  | MUSS                                        |
| H.264 | FLAC  | MUSS/SOLL                                   |
| H.264 | PCM   | SOLL/Advanced                               |
| HEVC  | AAC   | MUSS/SOLL                                   |
| HEVC  | Opus  | SOLL                                        |
| HEVC  | FLAC  | SOLL                                        |
| AV1   | Opus  | nur wenn echter Matroska-DocType garantiert |
| AV1   | AAC   | optional                                    |
| AV1   | FLAC  | optional                                    |

Wichtig:

```text
MKV darf nicht WebM-DocType erzeugen.
Wenn libwebm für AV1+Opus DocType=webm schreibt, darf das nicht als MKV-Profil angeboten werden.
```

### MP4

| Video    | Audio | Status                  |
| -------- | ----- | ----------------------- |
| H.264    | AAC   | MUSS                    |
| HEVC     | AAC   | SOLL/MUSS               |
| AV1      | AAC   | optional/platform-gated |
| H.264    | Opus  | nein                    |
| HEVC     | Opus  | nein                    |
| FLAC/PCM | —     | nein                    |

### WebM

| Video | Audio    | Status                   |
| ----- | -------- | ------------------------ |
| AV1   | Opus     | MUSS                     |
| VP9   | Opus     | nur falls echter Encoder |
| H.264 | beliebig | nein                     |
| HEVC  | beliebig | nein                     |
| —     | AAC      | nein                     |

---

# 8. Audio Capture und Routing

## 8.1 Quellen

MUSS:

* Microphone
* App Audio
* System Audio

## 8.2 Microphone Controls

MUSS:

* Device-Auswahl
* Default Mic
* Refresh
* Preflight Meter
* Gain Slider
* Clip Indicator
* Channel Mode:

  * Auto
  * Preserve Stereo
  * Mono Mix
  * Left to Stereo
  * Right to Stereo

Auto-Regel:

```text
Ein einseitiges Mikrofon darf nicht left-only/right-only im finalen Recording landen.
```

## 8.3 Track Routing

MUSS:

* Merged Audio
* Separate Audio Tracks
* Hybrid Routing

Beispiele:

```text
Track 1: App + System
Track 2: Mic
```

```text
Track 1: App + System + Mic
```

```text
Track 1: System
Track 2: App + Mic
```

## 8.4 Track Sortierung

MUSS:

* Audio-Tracks sortierbar
* Track-Namen
* Track-Reihenfolge im Container kontrollierbar

Default:

```text
1. App
2. System
3. Mic
```

Merge-first default optional je Profil.

---

# 9. Video Tracks und Webcam

## 9.1 Webcam Capture

MUSS:

* Webcam-Geräteauswahl
* Auflösung/FPS-Auswahl
* Preview
* optional zuschaltbar
* Webcam composited ins Hauptvideo
* Drag/Drop-Positionierung in Preview
* Resize über Handles
* Position/Größe, Spiegelung, Aktivierung und Chroma-Key live während Recording/Paused änderbar
* Webcam-Gerät, Auflösung und FPS während Recording/Paused gesperrt; Wechsel erfordert Neustart der Webcam-Capture
* Mindest-/Maximalgröße
* Aspect-Ratio-Lock sinnvoll

Default:

```text
x = 0
y = 0
out_width = in_width
out_height = in_height
```

## 9.2 Webcam Chroma Key

MUSS für 1.0:

* Basic Chroma Key aktivierbar
* Key Color
* Color Picker direkt aus Webcam Preview
* Tolerance Slider
* optional Softness/Feather Slider
* Live Preview

Nicht 1.0:

* komplexes Background Replacement
* KI-Segmentation

## 9.3 Webcam separate Track/File

1.0:

```text
Composited Webcam: MUSS
Separate Webcam Video Track: OPTIONAL
Separate Webcam File: deferred
```

---

# 10. Multi-Track / Output-Graph

## 10.1 1.0

MUSS:

```text
Eine Containerdatei mit mehreren Tracks.
```

Darin:

* Hauptvideo
* optional composited Webcam
* mehrere Audio-Tracks
* gemergte und separate Audio-Varianten
* optional separate Webcam-Videospur, wenn Container stabil

## 10.2 Architektur vorbereitet für später

Intern sollte das Modell nicht hart auf "eine Datei für immer" festgelegt werden.

Empfohlene interne Struktur:

```text
RecordingSession
  OutputGroup[0]
    Container
    VideoTracks
    AudioTracks
```

Nach 1.0 möglich:

```text
OutputGroup[1]
  Webcam + Mic separate
```

Nicht 1.0:

```text
Mehrere parallele Output-Dateien als User-Feature
```

---

# 11. Marker

MUSS:

* Marker per Hotkey während Aufnahme setzen
* Marker in Sidecar JSON speichern

Beispiel:

```json
{
  "recording": "2026-05-24_20-00-00.mkv",
  "markers": [
    {
      "time_ms": 183420,
      "label": "Marker 1",
      "type": "manual"
    }
  ]
}
```

SOLL:

* optional Container-native Chapters/Markers
* Export für Schnittsoftware später

---

# 12. Hotkeys

## MUSS

```text
Start / Stop Recording
Pause / Resume
Mic mute / unmute
Overlay show / hide
Marker setzen
```

## SOLL

```text
Webcam show / hide
System audio mute / unmute
App audio mute / unmute
```

Anforderungen:

* global
* konfigurierbar
* Konflikterkennung
* Persistenz
* Diagnostics zeigt Registrierungsstatus
* klare Warnung bei Fehler

---

# 13. Countdown

MUSS:

```text
Off
3s
5s
10s
Custom
```

* Countdown vor Start
* sichtbar in Overlay/UI

SOLL:

* Countdown vor Resume

Bei Region Select-on-record:

```text
Region-Auswahl → Countdown → Aufnahme
```

---

# 14. Pause / Resume / Split

MUSS:

* Pause / Resume
* klare Paused UI
* Close-Warnung bei Paused

SOLL:

* Split-on-pause

Split-Verhalten:

```text
part001.mkv
part002.mkv
```

MP4 muss beim Split sauber finalisieren.

---

# 15. Recording Overlay

## 15.1 Recording Overlay

MUSS/SOLL:

* Recording Status
* Timer
* Paused State
* Mic Muted
* Marker Feedback
* Countdown
* optional FPS / Dropped Frames

## 15.2 Anti-Cheat Policy

MUSS:

```text
Keine Injection.
Keine Hooks in fremde Prozesse.
Eigenes transparentes Overlay-Fenster.
```

Bei Games/Anti-Cheat:

* Warnung
* Overlay default aus oder Bestätigung erforderlich
* bekannte Anti-Cheat-Prozesse optional erkennen
* User kann explizit überschreiben

---

# 16. Close während Recording/Pause

MUSS:

| Zustand             | Verhalten                        |
| ------------------- | -------------------------------- |
| Idle                | normal schließen                 |
| Recording           | Confirm Dialog                   |
| Paused              | Confirm Dialog                   |
| Stopping/Finalizing | blockieren / please wait         |
| MP4 finalizing      | Hinweis auf notwendiges Finalize |

Dialogoptionen:

```text
Cancel
Stop and Save
Force Close
```

Force Close nur gefährlicher Advanced-Pfad.

---

# 17. Recording Profiles

## 17.1 Grundmodell

Recording Profiles sind First-Class.

Ein Profil enthält recording-bezogene Einstellungen:

* Container
* Video Codec
* Audio Codec
* FPS / CFR / VFR
* Target Mode
* Audio Routing
* Webcam
* Region Mode
* Countdown
* Overlay
* Filename Pattern
* Output settings, soweit recording-spezifisch

Nicht enthalten:

* Logs
* Trace
* Debug
* Experimental App Flags
* UI Theme

## 17.2 Built-in vs User Profiles

| Typ                 | Verhalten                           |
| ------------------- | ----------------------------------- |
| Built-in Profile    | read-only                           |
| User Profile        | auto-save                           |
| Imported Profile    | User Profile                        |
| Last Active Profile | wird nach Restart wiederhergestellt |

Built-in Profile können nicht überschrieben werden.

Wenn Built-in geändert:

```text
MODIFIED
[Save as new profile]
[Reset]
```

User Profile:

```text
Änderungen auto-save
```

## 17.3 Aktionen

MUSS:

* New from current settings
* New from safe default
* Duplicate
* Rename
* Delete
* Reset active profile
* Import profiles
* Export selected profiles
* Export all user profiles
* Reset all settings + presets

Nach Neustart:

```text
Letztes aktives Profil mit letzten Einstellungen wird geladen.
```

---

# 18. Output Folder / Filename Pattern

## 18.1 Felder

ExoSnap trennt:

```text
Output Folder
Filename Pattern
```

## 18.2 Output Folder

Default beim ersten Start:

```text
C:\Users\<User>\Videos\ExoSnap
```

Browse schreibt immer einen absoluten Pfad.

Erlaubt:

```text
C:\Recordings
D:\Captures
\\NAS\Recordings
~/Videos/ExoSnap
%USERPROFILE%\Videos\ExoSnap
```

Trailing slashes erlaubt und werden normalisiert, außer Root-Pfade.

Erlaubte Env-Allowlist:

```text
%USERPROFILE%
%APPDATA%
%LOCALAPPDATA%
%PUBLIC%
%TEMP%
%TMP%
```

Keine beliebigen Env Vars.

## 18.3 Filename Pattern

Pattern ist immer relativ zum Output Folder.

Erlaubt:

```text
{profile}/{app}/{datetime}
{container}/{video}-{audio}/{app}/{datetime}
```

Führendes `/`, `\`, `./`, `.\` ist erlaubt und wird auf Blur entfernt.

Verboten:

```text
../
..\
C:\
\\NAS
~/
%USERPROFILE%
```

Internal `.` Segmente können normalisiert werden. `..` ist immer invalid.

## 18.4 Tokens

```text
{datetime}
{date}
{time}
{timestamp}
{YYYY}
{YY}
{MM}
{DD}
{hh}
{mm}
{ss}
{app}
{title}
{process}
{target}
{profile}
{container}
{video}
{audio}
```

Codec-Token-Werte:

```text
h264
hevc
av1
aac
opus
flac
pcm
```

## 18.5 Paste Assist

MUSS/SOLL:

Wenn User einen absoluten Pfad in Output Folder oder Filename Pattern pastet, darf ExoSnap helfen.

Automatischer Split nur bei Token-Pfaden:

```text
D:\Captures\{app}\{datetime}
```

wird:

```text
Output Folder: D:\Captures
Filename Pattern: {app}/{datetime}
```

Bei Filepath mit Extension:

```text
D:\Captures\recording.mp4
```

nicht still splitten, sondern anbieten:

```text
This looks like a full output file path. Split into folder and filename pattern?
[Split] [Cancel]
```

Nach Split:

* Confirmation
* Undo
* Output Folder normalisiert
* Pattern bleibt relativ

---

# 19. UI-Seiten

## Record

* Target
* Target Picker
* Preview
* Audio quick controls
* Webcam toggle
* Status/Readiness
* Start/Stop/Pause
* Result Panel

## Output / Profiles

* Recording Profile
* Container/Profile
* Output Folder
* Filename Pattern
* Effective Preview
* Import/Export/Reset

## Video

* Codec
* FPS
* CFR/VFR
* Bitrate/Quality
* Cursor
* Region/Crop
* Advanced Encoder Settings

## Audio

* Sources
* Routing
* Track order
* Codec
* Gain
* Channel mapping
* Meters

## Webcam

* Device
* Resolution/FPS
* Preview placement
* Chroma key
* Overlay settings

## Hotkeys

* Global hotkey assignment
* Conflicts
* Registration status

## Diagnostics

Tabs:

```text
Overview
Capabilities
Configuration
Recommendations
Performance
Logs
Self-Test
```

Advanced settings live here, not recording controls.

---

# 20. Diagnostics

## Capabilities

MUSS anzeigen:

* OS
* GPU
* Driver
* NVENC availability
* Video Codecs
* Audio Codecs
* Containers
* Valid Profiles
* Capture Backends
* Audio Devices
* Webcam Devices
* Hotkeys
* Overlay status

## Configuration

Zeigt aktuelle aktive Settings:

* Profile
* Target
* Region
* Webcam
* Container/Codec
* Audio Routing
* FPS/CFR/VFR
* Output Path
* Hotkeys

## Recommendations

Beispiele:

```text
144 Hz monitor + 60 fps recording:
Consider limiting the game to 60 or 120 fps for smoother pacing.
```

```text
MP4 is less crash-resilient. MKV is recommended for long recordings.
```

```text
AV1 NVENC unavailable. Use H.264 or HEVC.
```

## Logs

* Basic
* Verbose
* Trace
* Rotation by size/count
* Open Log Folder
* Copy Logs
* Export Diagnostics Bundle

---

# 21. Performance Trace / Waterfall

MUSS:

* Live Stage Timings
* avg / p95 / max
* Bottleneck Stage
* Queue Depths
* Dropped / Duplicated Frames
* A/V Drift

Stages:

```text
Capture
Convert
Composite
Encode Submit
Encode Output
Audio Capture
Audio Mix
Audio Encode
Mux
Disk Write
Finalize
```

SOLL:

* Waterfall Diagram
* Trace Export
* short detailed trace sessions with size/time limit

---

# 22. Packaging / Distribution

## Portable ZIP

MUSS:

```text
exosnap-v1.0.0-windows-x64.zip
exosnap-v1.0.0-windows-x64.sha256
```

## Installer

SOLL/MUSS für 1.0:

* Start Menu Shortcut
* optional Desktop Shortcut
* optional CLI PATH
* uninstall sauber

## Signing

Ziel für 1.0:

* signierte EXE
* signierter Installer

---

# 23. CLI

Bevorzugt:

```text
exosnap-cli.exe
```

Use Cases:

```text
exosnap-cli record --monitor 1 --profile "MKV H264 AAC" --duration 60
exosnap-cli list-targets
exosnap-cli list-devices
exosnap-cli capabilities
exosnap-cli self-test
```

MUSS später:

* Profile auswählen
* Target auswählen
* Duration
* Output Folder
* Filename Pattern
* Diagnostics
* Exit Codes

---

# 24. Out of Scope für 1.0

Nicht 1.0:

* Streaming zu Twitch/YouTube
* Editor/Timeline
* Cloud Upload
* Multi-PC Capture
* Linux production parity
* macOS
* Plugin-System
* Dolby/Vorbis
* Separate Output Files als Hauptfeature
* Per-track Codec Override
* AI Features

---

# 25. 1.0 Release Criteria

ExoSnap 1.0 darf released werden, wenn:

1. Windows Recording stabil ist.
2. MKV, MP4, WebM echte Container sind.
3. Alle freigegebenen Profile per Smoke validiert sind.
4. Monitor/App/Region Capture funktioniert.
5. Webcam Overlay + Chroma Key funktioniert.
6. Audio merged/separate/hybrid funktioniert.
7. Pause/Resume funktioniert.
8. Hotkeys funktionieren.
9. Countdown funktioniert.
10. Diagnostics Page funktioniert.
11. Pipeline Metrics nutzbar sind.
12. Profiles import/export/reset funktioniert.
13. Settings zuverlässig persistieren.
14. Safe Close während Recording/Pause funktioniert.
15. Packaging/Installer/Signing bereit sind oder bewusst dokumentiert.
16. Keine known-red Tests existieren.
17. Keine Fake-Controls/Fake-Profile in UI vorhanden sind.

---

# Roadmap bis 1.0

## v0.3.x — Container Matrix Completion

Ziel: Container/Codec-Basis finalisieren.

* MKV default stabilisieren
* MKV H.264 Opus
* MKV H.264 FLAC
* MKV H.264 PCM optional
* HEVC NVENC
* MP4 HEVC AAC
* MKV HEVC AAC
* WebM AV1 Opus Regression
* Docs/Capability Matrix

## v0.4.x — Profiles + Output System

Ziel: Recording Profiles und Output UX fertig.

* Built-in Profiles
* User Profiles auto-save
* Duplicate / Reset / Rename / Delete
* Import / Export
* Reset all
* Output Folder / Filename Pattern Split
* `~/` und Env-Allowlist
* Paste Assist
* Token autocomplete
* Live validation
* Effective output preview

## v0.5.x — Region + Live Preview

Ziel: Capture UX fertig.

* echte Live Preview
* Monitor Preview
* Window/App Preview
* Region Capture
* Select-on-record
* Region Overlay
* Countdown integration
* Selection border behavior

## v0.6.x — Webcam + Overlay

Ziel: Webcam und Recording Overlay.

* Webcam Capture
* Webcam Preview Overlay
* Drag/Resize
* Chroma Key
* Recording Overlay
* Anti-Cheat-aware overlay behavior
* Overlay hotkey

## v0.7.x — Advanced Audio / Tracks

Ziel: Track-Routing voll machen.

* Separate Audio Tracks
* Hybrid Routing
* Track Order
* Track Names
* Audio Page real
* Mic/App/System mute hotkeys
* Marker Sidecar JSON

## v0.8.x — Pause/Resume + Diagnostics

Ziel: Produktionsdiagnostik.

* Pause/Resume
* Split-on-pause
* Safe Close Dialogs
* Diagnostics Page
* Capabilities
* Recommendations
* Logs
* Self-Test
* Performance Metrics
* Pipeline Trace / Waterfall basic

## v0.9.x — CLI + Installer + Release Hardening

Ziel: Releasequalität.

* `exosnap-cli.exe`
* Installer
* optional PATH integration
* signing
* release automation
* package sanity
* full smoke matrix
* docs
* performance polish

## v1.0.0 — Final Hardening

Ziel: fertiger erster Release.

* Bugfixing
* UI polish
* performance tuning
* full manual matrix
* all docs current
* no known-red tests
* release notes
* stable installer/ZIP

---

# Post-1.0 Roadmap

Sortiert nach Nutzen/Aufwand.

## v1.1.0 — Linux Experimental Foundation

**Nutzen:** hoch für Plattformreichweite
**Aufwand:** hoch

* Linux NVENC diagnostics
* PipeWire monitor capture prototype
* PipeWire mic/system audio
* MKV/WebM output
* no parity promise

## v1.2.0 — Multi-Output Groups / Separate Files

**Nutzen:** hoch für Streamer/Editing
**Aufwand:** hoch

* mehrere OutputGroups
* separate Webcam/Mic-Dateien
* gemeinsame Timeline
* multi-file finalize
* sync metadata

## v1.3.0 — Per-Track Codec Overrides

**Nutzen:** mittel/hoch
**Aufwand:** mittel/hoch

* Track-spezifischer Audio-Codec
* Track-spezifischer Video-Codec, wenn sinnvoll
* erweiterte Validation
* UI-Komplexität

## v1.4.0 — HDR / 10-bit

**Nutzen:** hoch für moderne Displays
**Aufwand:** sehr hoch

* HDR capture path
* 10-bit surfaces
* HEVC Main10 / AV1 10-bit
* metadata handling
* HDR preview/tonemapping

## v1.5.0 — Advanced Marker / Editing Interop

**Nutzen:** mittel
**Aufwand:** mittel

* MKV chapters
* MP4 chapters
* EDL/XML export
* marker labels/types
* highlight markers

## v1.6.0 — AV1-in-MP4 / Advanced Codec Profiles

**Nutzen:** mittel
**Aufwand:** mittel/hoch

* platform probe
* muxer support
* player compatibility matrix
* UI gating

## v1.7.0 — Streaming / Virtual Camera

**Nutzen:** hoch, aber neues Produktfeld
**Aufwand:** sehr hoch

* RTMP/SRT/WebRTC
* virtual camera
* streaming profiles
* latency controls

## v1.8.0 — Plugin / Automation System

**Nutzen:** mittel
**Aufwand:** hoch

* scripting/hooks
* external control API
* profile automation
* event triggers

# Edit-Overlay: echtes Video-Preview / Playback

> **SHIPPED (PR #220, 2026-07-14/15).** Verifiziert 2026-07-23 gegen aktuellen Code:
> `EditExportPage` fährt einen echten `player_session_`-Decoder statt des Platzhalters. Nichts
> hier ist mehr offen.

## Problem

Das Edit/Output/Save-Overlay (`EditExportPage`, gehostet in `EditExportOverlay`, ADR 0022)
zeigt heute **kein dekodiertes Videobild**. Trim-Handles, Marker-Verticals, Playhead und ein
Positions-Clock funktionieren real, aber die Player-Fläche trägt nur den Platzhalter
`"Video preview — coming in 0.11"` (`app/pages/EditExportPage.cpp:233`). Der Play/Pause-Knopf
treibt eine reine Uhr (`onPreviewTick`, `app/pages/EditExportPage.cpp:816-824`), an die sich
ein Frame-View „andocken" soll (`app/pages/EditExportPage.h:96-103`, ADR 0022 §Forward).

Ursache ist eine bewusste Deployment-Entscheidung: das gebündelte FFmpeg ist **mux-only**.
`cmake/VendorFFmpeg.cmake:12-13` deployt nur `avformat / avcodec / avutil / swresample`;
`avfilter`, `swscale`, `avdevice` werden nicht ausgeliefert. Zusätzlich ist der gepinnte Build
`Exoridus/exosnap-ffmpeg-build r3 (n8.1.1)` minimal konfiguriert — er enthält Muxer/Demuxer für
MKV/MP4 (`cmake/VendorFFmpeg.cmake:19-25`), aber **keine Video-Decoder**. Ergebnis: es gibt im
gesamten Repo keinerlei Frame-Decode-Pfad (`avcodec_send_packet` / `avcodec_receive_frame` /
`sws_scale` → 0 Treffer in `**/*.cpp`).

Der Nutzer kann also trimmen und exportieren, aber nicht sehen, **an welcher Stelle** er trimmt.
Das ist der größte Ehrlichkeits-/Nutzbarkeits-Bruch der Edit-Fläche.

## Ist-Zustand (Datei:Zeile)

**Deployment / Build**
- `cmake/VendorFFmpeg.cmake:11-13` — nur die vier mux-only DLLs; `avfilter/swscale/avdevice`
  explizit nicht deployt.
- `cmake/VendorFFmpeg.cmake:15,31-42` — Pin auf `r3 / n8.1.1`, FetchContent per URL + SHA256
  (immutable Tag). Archiv ~2,3 MB.
- `cmake/VendorFFmpeg.cmake:19-25` — Historie r1→r3: `--enable-muxer=mp4`, `--enable-demuxer=mov`.
  Der Matroska-Demuxer ist vorhanden (siehe `ExtractKeyframeTimestamps` unten, das die MKV-Master
  liest); Decoder werden nirgends erwähnt und sind im Build nicht enthalten.
- `cmake/VendorFFmpeg.cmake:73-91` — importierte Targets `FFmpeg::avformat/avcodec/avutil/
  swresample` + Convenience-Bundle `FFmpeg::mux`.
- `cmake/VendorFFmpeg.cmake:111-119` — Deploy-/Install-Regeln kopieren exakt die vier DLLs.
- `THIRD_PARTY_NOTICES.md` / `KNOWN_LIMITATIONS.md` (Zeile ~228-234) — dokumentieren „mux-only
  DLL set … decoding frames to a displayable format is not wired up (planned for a later release)".

**Engine: was bereits existiert und wiederverwendbar ist**
- `libs/engine/include/exosnap/engine/mp4_remuxer.h:108` —
  `ExtractKeyframeTimestamps(path)` liefert alle Video-Keyframe-PTS (µs, aufsteigend), ohne zu
  dekodieren. Wird heute schon für Trim-Snapping auf dem MKV-Master gerufen → der Matroska-Demuxer
  ist einsatzfähig. **Direkt als Seek-Index wiederverwendbar.**
- `libs/engine/src/yuv_to_bgra.h:45-62` — `ConvertYuv420ToBgra(PlanarYuv420Frame,
  YuvToBgraParams, out, stride)`: reine CPU-Konvertierung **NV12/P010 → BGRA8888**, matrix-/
  range-parametrisiert (BT.709, Full/Limited). Keine GPU-Abhängigkeit, thread-safe. **Erwartet eine
  interleavte UV-Plane und (10 Bit) MSB-Alignment 15:6** (`yuv_to_bgra.h:36-53`) — der Software-
  Decoder liefert planar/LSB-aligned, daher der Repack-Adapter in Schritt 2a.
- `libs/engine/src/yuv_to_bgra.h:85` — `ConvertAyuvToBgra(...)` für **packed** AYUV (der
  Capture-Encode-Surface-Fall). Der Software-Decoder eines 8-bit-4:4:4-H.264/HEVC-Files
  (Expert-Option, `product-spec` §Not present) liefert dagegen **planares `yuv444p`** — dafür passt
  weder `ConvertYuv420ToBgra` (4:2:0/interleaved) noch `ConvertAyuvToBgra` (packed) direkt; MVP-
  Verhalten dazu ist in Section B / Schritt 2a definiert (neuer planarer `ConvertYuv444ToBgra` bzw.
  inerter Fallback).
- `libs/engine/src/hdr_preview.h:88-98` — `P010PqMonitorConverter(peak_scale)` +
  `Convert(...)`: reine CPU-Konvertierung **P010 PQ/BT.2020 → tone-gemapptes SDR BGRA** über
  vorberechnete LUTs (dieselben Referenzkurven wie der Capture-Tonemap). Genau der Pfad, den ein
  dekodierter HDR10-Frame braucht.
- `libs/engine/CMakeLists.txt:940-961` — Tests `test_hdr_preview` und `test_yuv_to_bgra`
  bestätigen: die Farb-Konvertierungsmathematik existiert und ist CI-getestet, **ohne** swscale.
- `libs/engine/include/exosnap/engine/gpu_hdr_tonemap.h:23-39` — `HdrToneMapper`: GPU-Pass
  FP16 scRGB → SDR BGRA. Relevant nur, falls je ein GPU-Decode-Pfad FP16 liefert; der Software-
  Decode liefert P010, für das der CPU-`P010PqMonitorConverter` der passende Match ist.

**UI: die Andock-Punkte**
- `app/pages/EditExportPage.h:96-103` — `setPreviewPlaying/setPreviewPositionMs/previewPositionMs`:
  „the video frame itself is still the deferred 0.11 preview — the clock, playhead, and scrub
  semantics are real and a frame view will attach to this position."
- `app/pages/EditExportPage.h:167` — `keyframe_timestamps_` (sortierte Keyframe-PTS µs) bereits im
  Page-State (für Trim-Snap geladen).
- `app/pages/EditExportPage.h:173-178,204-211` — Preview-Clock-State + `player_frame_` (QFrame,
  `minimumHeight 180`, `updatePlayerHeight` hält 16:9) + `timeline_` (EditTimeline).
- `app/pages/EditExportPage.cpp:816-824` — `onPreviewTick`: Clock-Advance; hier würde ein
  Frame-Request andocken.
- `app/pages/EditExportPage.cpp:874-887` — Scrub-Semantik: Drag pausiert, Release resumt nur wenn
  vorher gespielt; `preview_position_ms_` folgt dem Drag → exakt der Punkt, an dem ein Frame
  angefordert wird.
- `EditContext` (`app/pages/EditExportPage.h:34-59`) trägt `mkv_master_path` (Decode-Input),
  `duration_seconds`, `resolution`, `fps`, `video_codec` — **aber keine ColorMetadata/HDR-Flags**.

**DXGI-Preview-Constraint (relevant, aber im Editor nicht bindend)**
- `app/services/DxgiPreviewRenderer.h` — die Live-Record-Preview ist ein natives Child-HWND;
  ADR 0022 §Surface shape stützt sich darauf, dass das Overlay einen nativen Geschwister-Backdrop
  über dieses HWND komponiert. Im Editor läuft **keine** Live-Capture; solange die Player-Fläche
  **kein** eigenes natives Child-HWND aufmacht, greift der Overlay-Compositing-Zwang hier nicht.

## Design

Drei Achsen sind zu entscheiden: **(A) Decode-Deployment**, **(B) Decode→Display-Pipeline +
Scrubbing**, **(C) HDR + Audio-Scope**. Leitlinie: kein Zweit-„alles durch FFmpeg", maximale
Wiederverwendung der vorhandenen GPU-/CPU-Farbpfade, ehrliche Aufwandsstaffelung.

### A. Deployment-Entscheidung: was kommt in `exosnap-ffmpeg-build`?

**Option A1 — swscale (+avfilter) ins FFmpeg-Build aufnehmen, Software-Decode + `sws_scale`.**
Der klassische Weg: Decoder + swscale liefern out-of-the-box `AVFrame` → BGRA. avfilter wäre für
reines Preview überflüssig (kein Filtergraph nötig).
_Kosten:_ swscale-DLL (~0,5–1 MB) zusätzlich, plus die eigentlich benötigten Decoder. avfilter
(~mehrere MB) bringt für dieses Feature nichts.
_Nachteil:_ dupliziert Konvertierungslogik, die als reine, getestete CPU-Mathematik **bereits
existiert** (`ConvertYuv420ToBgra`, `P010PqMonitorConverter`), und liefert für HDR keine
tonemappte SDR-Ausgabe ohne zusätzliche Curve-Arbeit. Widerspricht dem „native/schlanke DLL-Set"-
Ethos (ADR 0006).

**Option A2 — nur Decoder ins FFmpeg-Build; Konvertierung über die vorhandenen Pfade (+ Repack-Adapter).**
Der Build bekommt Video-Decoder (native `h264`, `hevc` — beide software — sowie Software-AV1 via
**dav1d**) + zugehörige Parser; MKV/MP4-Demuxer sind schon da. **Wichtig — die Software-Decoder
liefern *planares* YUV, nicht die interleavten NV12/P010-Layouts, die die vorhandenen Konverter
erwarten:** native h264/hevc/dav1d geben `yuv420p` (getrennte U-/V-Planes, 8 Bit) bzw. `yuv420p10le`
(getrennte 10-Bit-LSB-aligned Planes) aus. `ConvertYuv420ToBgra`/`P010PqMonitorConverter` verlangen
aber eine **interleavte** UV-Plane und bei 10 Bit **MSB-Alignment** (aktive Bits links in 15:6, s.
`yuv_to_bgra.h:36-53`). Deshalb sitzt zwischen Decoder und Konverter ein **kleiner, reiner
Repack-Schritt** — nicht swscale: `yuv420p` → NV12-Layout (U/V zeilenweise interleaven),
`yuv420p10le` → P010-Layout (interleaven **und** je Sample `<< 6`). Das ist ein paar Dutzend Zeilen
CPU-Code (eigenes Arbeitspaket + Test in Schritt 2), kein neuer Filtergraph. Für den 8-bit-4:4:4-
Expert-Pfad (`yuv444p`) siehe Section B / Schritt 2.
_Kosten:_ avcodec-DLL wächst (native h264/hevc-Decoder) + **dav1d als neue, statisch in avcodec
gelinkte Drittabhängigkeit** (kein fünftes DLL — dav1d wird im Build-Repo per meson gebaut und
statisch in `avcodec` gelinkt), grob +2–4 MB. Bleibt weit unter dem früheren 88-MB-BtbN-Download.
Lizenz: alle genannten Decoder sind LGPL-kompatibel (dav1d = BSD; native FFmpeg-h264/hevc-**Decoder**
sind LGPL — nur die x264/x265-*Encoder* sind GPL und werden nicht gebraucht). Das aktuelle LGPL-2.1-
Modell (`cmake/VendorFFmpeg.cmake:17`) bleibt erhalten. **Patent- und Lizenz-Doku sind separat zu
pflegen** (dav1d-Eintrag in `THIRD_PARTY_NOTICES.md` + License-Staging; HEVC-Decoder-Patentposition
analog ADR 0043 — s. Schritt 0/1).
_Vorteil:_ nutzt genau die farbrichtige, HDR-tonemappende, CI-getestete Mathematik wieder, die die
Capture-Snapshot-/Preview-Pfade schon fahren → **eine** Farbwahrheit statt zweier. Der Repack ist ein
mechanischer Layout-Umbau (keine Farbmathematik), also bleibt „eine Farbwahrheit" intakt.

**Option A3 — kein FFmpeg-Decode; Media Foundation `IMFSourceReader` (OS-Decoder, D3D11).**
_Nachteil:_ MF ist im Projekt bewusst transitional/rückgebaut (ADR 0038 delay-load nur für Webcam).
AV1/HEVC-Decode über MF erfordert Store-„Video Extensions", die auf vielen Systemen fehlen → genau
die Codec-Matrix (Default AV1!) würde brüchig. Falsche Architekturrichtung. Verworfen.

**Option A4 — NVDEC (nvcuvid) direkt.**
Passt zur NVENC-Only-Haltung, aber zieht CUDA/NVDEC-SDK + ganze Integration nach sich, und ein
künftiger AMD/Intel/Software-Encode-Nutzer hätte dann keine Preview. Für ein Editor-Preview
Overkill. Verworfen (für MVP; siehe Increment 2 für die GPU-Variante über D3D11VA statt NVDEC).

**Entscheidung: A2.** In `exosnap-ffmpeg-build` **Decoder aufnehmen, swscale/avfilter NICHT**.
Begründung: Die Konvertierungs- und HDR-Tonemap-Mathematik existiert bereits als reine,
GPU-freie, getestete Helfer (`yuv_to_bgra.h`, `hdr_preview.h`) — swscale würde sie nur
duplizieren und für HDR ohnehin nicht die tonemappte SDR-Ausgabe liefern. Das hält das DLL-Set
schlank, konsistent mit ADR 0006 und der Zero-Copy-/Ein-Quellen-Farbwahrheit des Projekts.
avfilter hat für ein Trim/Scrub-Preview keinen Nutzen.

### B. Decode→Display-Pipeline + Scrubbing

**Decode-Backend — Software vs. D3D11VA.**
- **Software-Decode (MVP):** `avcodec_send_packet`/`receive_frame` → `AVFrame` im Systemspeicher →
  CPU-Konverter → `QImage` → in der Player-`QFrame` per Qt gemalt. Keine GPU, kein natives
  Child-HWND → **DXGI-Overlay-Constraint entfällt vollständig**. Für **frame-genaues Scrubbing**
  (ein Frame pro Seek/Release) ist die Latenz bei kurzen GOPs / 1080p unkritisch; **Worst-Case ist
  aber ein Ziel kurz vor dem nächsten Keyframe**: bei Default-Keyframe-Intervall 2 s = 120 Frames
  @60 fps (`product-spec` §Output) müssen bis zu ~119 Frames forward-dekodiert werden — bei
  4K-AV1-Software-Decode grob ~1 s pro exaktem Frame. Mitigation s. Section B (Scrub-on-Release) +
  Risiken. Für kontinuierliches 60-fps-Playback von 4K-AV1 ist Software-Decode grenzwertig
  (→ Playback ist bewusst nicht MVP, siehe C).
- **D3D11VA-Hwaccel (Increment 2):** avcodec dekodiert (hwaccel `d3d11va`, in avcodec eingebaut,
  keine externe Dep) in D3D11-NV12/P010-Texturen; Anzeige über die **vorhandenen** GPU-Shader
  (die NV12/P010→BGRA-Konvertierung des `DxgiPreviewRenderer`, bzw. `HdrToneMapper` für FP16 —
  hier aber P010). Ermöglicht flüssiges Playback + billiges 4K. Erfordert avcodec mit aktiviertem
  d3d11va (Build-Flag, kein neues DLL). Anzeige weiterhin ohne natives Child-HWND möglich, indem
  die konvertierte Textur einmal pro angezeigtem Frame CPU-seitig zurückgelesen und als `QImage`
  präsentiert wird (Readback bei 1080p ~wenige ms; bei 4K throttled). Ein natives D3D11-Child-HWND
  in der Player-Fläche wäre die zero-copy-Kür, würde aber denselben Compositing-Zwang wie das
  Record-Preview auslösen (nativer Geschwister im Overlay) — **bewusst nicht im MVP**.

**Entscheidung B:** MVP = **Software-Decode → vorhandene CPU-Konverter → `QImage`** in der
Player-`QFrame`. Keine GPU-Kopplung, kein Child-HWND, kein Overlay-Constraint. D3D11VA-Hwaccel ist
ein sauberes, additives Increment für Playback/4K.

**Seek/Scrubbing — keyframe-genau, dann Decode-forward.**
1. Zielzeit T (ms) kommt aus dem Playhead/Scrub (`onScrubMoved`, `preview_position_ms_`).
2. Größten Keyframe-PTS ≤ T aus dem bereits vorhandenen `keyframe_timestamps_`
   (`EditExportPage.h:167`) wählen → `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)` auf diesen Keyframe.
3. Decode-forward, Frames verwerfen bis `frame.pts >= T`; diesen Frame konvertieren + anzeigen.
4. Innerhalb desselben GOP (Scrub in eine Position **hinter** dem letzten dekodierten Frame, gleicher
   Keyframe) ohne erneuten Seek weiterdekodieren.

**Latenz-Mitigation langer GOPs (Scrub-on-Release).** Während eines aktiven Drags wird **nur bis zum
nächstliegenden Keyframe ≤ T** dekodiert und dieser (grob positionierte) Frame angezeigt — kein
Forward-Decode durch bis zu 2 s GOP pro Drag-Sample. Den **exakten** Frame (Forward-Decode bis
`pts >= T`) liefert der Worker erst **on release**. So bleibt der Drag flüssig, und die teure
Forward-Dekodierung fällt einmalig statt pro Coalescing-Sample an.

**Frame-Cache.** MVP: 1-Frame-Cache (der zuletzt dekodierte Frame bleibt sichtbar, während der
Decoder-Thread den nächsten liefert) + ein kleiner LRU der zuletzt gesehenen Keyframe-Positionen.
Increment: Ring-Cache um den Playhead (z. B. ±N Frames) für ruckelfreies Vor/Zurück-Scrubben.

**Threading.** Decode läuft auf einem eigenen Worker (nicht GUI-Thread; analog zum Export-Thread
`EditExportPage.h:181`). Scrub-Anfragen sind **coalescing**: nur die jeweils neueste Zielzeit wird
bedient, ältere verworfen (verhindert Rückstau bei schnellem Drag). Fertige `QImage` per
`QMetaObject::invokeMethod(..., QueuedConnection)` auf den GUI-Thread. Der vorhandene
Positions-Clock bleibt die Zeitquelle; der Decoder ist ein **Konsument** der Position, nie die
Uhr selbst — so bleibt die Scrub-/Playhead-Semantik (ADR 0022) unangetastet.

**Format-/HDR-Auswahl selbstbeschreibend aus dem Decoder.** Statt `EditContext` um ColorMetadata
zu erweitern, wird die Konvertierung aus den `AVFrame`-Eigenschaften abgeleitet:
`pix_fmt`/`color_trc`/`color_range`. Der Decoder liefert **planares** YUV; der Repack-Adapter (s.
Option A2) bringt es je nach `pix_fmt` ins vom Konverter erwartete Layout:
- `yuv420p10le` + `color_trc == AVCOL_TRC_SMPTE2084` (PQ) → P010-Repack (interleave + `<<6`) →
  `P010PqMonitorConverter` (peak_scale s. Section C).
- `yuv420p` (bzw. non-PQ 10-bit) → NV12-Repack (interleave, bei 10 Bit zusätzlich `<<6`) →
  `ConvertYuv420ToBgra` mit BT.709 + der vom Frame gemeldeten Range.
- `yuv444p` (8-bit-4:4:4-Expert-Aufnahme, `product-spec` §Not present: „8-bit 4:4:4 … implemented as
  an Expert option") → **MVP-Verhalten definiert**: trivialer `yuv444p → BGRA`-Pfad (kein
  Chroma-Upsampling, dieselbe BT.709/Range-Matrix wie `ConvertYuv420ToBgra`; ein `ConvertYuv444ToBgra`
  in `engine`, ~analog `ConvertAyuvToBgra` aber planar). Solange dieser Pfad nicht steht, ist
  der **definierte Fallback** die inerte Player-Fläche (kein Garbage) — nie undefinierter Zustand.
- Jedes andere/unerwartete `pix_fmt` → inerte Player-Fläche (definierter Fallback).

Das hält `EditContext` unverändert und ist robust gegen fehlende/veraltete Metadaten.

### C. HDR-Tonemap + A/V-Scope (ehrliche Descope-Entscheidung)

- **HDR im Preview:** Ein nativ als HDR10 aufgenommenes File dekodiert zu P010 PQ/BT.2020. Es wird
  über `P010PqMonitorConverter` zu **tone-gemapptem SDR BGRA** gewandelt — dieselbe Referenzkurve
  wie Live-Preview/Snapshot (Konsistenz mit `hdr_preview.h`, ADR 0040). Kein zweiter, erfundener
  Tonemap.
- **peak_scale — muss dem Live-Pfad gleichen, sonst klippt der Roll-off anders.** Der Live-/
  Snapshot-Pfad baut den Converter mit dem **Session-Display-Peak** (`video_thread.cpp:1935-1939`,
  `hdrPeakScale`). Ein davon abweichender „konservativer Referenz-Peak" im Editor würde die
  Highlights sichtbar anders klippen — der User-live-Testfall „sieht HDR wie das Live-Preview aus"
  wäre nicht erfüllbar. **Entscheidung:** Beim Editor-Open den Display-Peak über **denselben
  DisplayConfig-Query** ermitteln, den der Capture-Pfad nutzt (vgl. `dxgi_od_capture_src.cpp` /
  `libs/engine/CMakeLists.txt:929-932`), und den `P010PqMonitorConverter` damit bauen. Ist
  beim Editor-Open **kein** Display-Peak ermittelbar (Headless/Multi-Monitor-Edge), fällt der Editor
  auf den Referenz-Peak zurück — dann ist die Roll-off-Abweichung ein **explizit benannter Kauf**
  (in Spec/KNOWN_LIMITATIONS notiert), und der User-live-Test wird entsprechend als „im Normalfall
  identisch" formuliert.
- **A/V vs. nur Video — Descope:** Der MVP liefert **nur Video, stumm**. Grund: es gibt im Projekt
  **keinen Audio-Render-/Playout-Pfad** — die App captured Audio (WASAPI-Loopback/Mic), rendert es
  aber nie auf ein Ausgabegerät. Echtes A/V-Playback verlangt (a) Audio-Decoder im Build
  (Opus/AAC/FLAC/PCM), (b) einen WASAPI-Render-Client, (c) eine A/V-Sync-Master-Clock. Das ist ein
  eigenständiges Subsystem und wird **bewusst nicht** im MVP gebaut. Ehrlich benannt in Spec/
  KNOWN_LIMITATIONS: „Preview ist stumm; Ton-Wiedergabe ist ein späterer Slice."

### Zusammengefasste Entscheidung

MVP = **frame-genaues, stummes Video-Scrubbing per Software-Decode** (Decoder in
`exosnap-ffmpeg-build`, **kein** swscale/avfilter) über die vorhandenen CPU-Farbkonverter, angezeigt
als `QImage` in der Player-`QFrame`, angedockt an den existierenden Positions-Clock. **Echtzeit-
Playback, D3D11VA und Audio sind additive, klar abgegrenzte spätere Increments.**

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit Testansatz.

**Schritt 0 — `exosnap-ffmpeg-build`: Decoder-Release (r4).**
Im separaten Repo `Exoridus/exosnap-ffmpeg-build` einen neuen immutablen Release `r4` bauen mit
zusätzlich aktivierten **Software-Video-Decodern**:
- `--enable-decoder=h264,hevc` (native Software-Decoder) + `--enable-parser=h264,hevc,av1`.
- **AV1 = Software via dav1d, NICHT der native `av1`-Decoder.** Der native FFmpeg-`av1`-Decoder
  (`libavcodec/av1dec.c`) ist **hwaccel-only** und schlägt ohne konfigurierte Hwaccel mit „platform
  doesn't support hardware accelerated AV1 decoding" fehl. Software-AV1 verlangt
  `--enable-libdav1d --enable-decoder=libdav1d`; dav1d wird als **neue Drittabhängigkeit** (meson-
  Build) beschafft und **statisch in avcodec gelinkt** (kein fünftes DLL). `--enable-decoder=av1`
  wird **nicht** gesetzt (nutzloser hwaccel-Stub).
- LGPL beibehalten (keine GPL-Encoder). swscale/avfilter bleiben aus.
- **Patent-Position dokumentieren (analog ADR 0043 fdk-aac):** Ein ausgelieferter Software-H.264/
  HEVC-**Decoder** ist eine neue Patent-Expositionsfläche (HEVC-Patentpools erfassen auch Decoder);
  das Projekt verlangt für Codec-Distribution eine dokumentierte Position (`roadmap.md:35-37`,
  ADR 0007/0043). Vor r4-Adoption eine kurze ADR-/Notiz-Position (Decoder ≠ Encoder, LGPL-Linkage,
  Patent-Lage) ablegen. AV1/dav1d ist royalty-free (AOMedia).
_Verify:_ `ffmpeg -decoders` im Build-CI listet **`libdav1d`** (nicht der hwaccel-`av1`-Stub) sowie
`h264`/`hevc`, und ein echter Software-Decode-Smoke (`ffmpeg -i sample.av1.mkv -f null -`) läuft
ohne Hwaccel durch — verhindert das false-positive-Grün des reinen `-decoders`-Listings. DLL-Set
weiterhin avformat/avcodec/avutil/swresample; Größenzuwachs dokumentiert. (Maintainer-gated;
außerhalb dieses Repos.)

**Schritt 1 — `cmake/VendorFFmpeg.cmake` auf r4 pinnen + dav1d-Lizenz.**
URL/Tag/`URL_HASH` auf r4 aktualisieren; Kommentarblock r3→r4 (Decoder-Begründung) fortschreiben.
Keine neuen Targets nötig (Decoder leben in `avcodec`; dav1d ist statisch hineingelinkt). **dav1d
lizenzseitig aufnehmen:**
- Neuer `THIRD_PARTY_NOTICES.md`-Eintrag „dav1d" (BSD-2-Clause, statisch in `avcodec` gelinkt); der
  FFmpeg-Eintrag (`:155-173`) kennt heute nur FFmpeg.
- `cmake/VendorFFmpeg.cmake` License-Staging (`:124-140`) staged heute **nur** `ffmpeg.txt` — die
  dav1d-Lizenz (`libs/dav1d/COPYING` im r4-Archiv, sofern mitgeliefert) als zweite Datei
  (`licenses/dav1d.txt`) mitstagen, sonst die dav1d-COPYING im Build-Repo separat pflegen.
- FFmpeg-Eintrag auf r4/n8.1.1 anpassen und den Rollentext (heute „mux-only … no decoding")
  aktualisieren — jetzt Decode für Preview (deckt sich mit Review M-11/M-14).
_Verify:_ Build lädt r4; License-Staging produziert `ffmpeg.txt` **und** `dav1d.txt`; bestehende
Remux-/Keyframe-Tests bleiben grün.

**Schritt 2a — Planar→interleaved Repack-Adapter in `engine` (eigenes Arbeitspaket).**
Reine CPU-Helfer, die den planaren Decoder-Output ins vom vorhandenen Konverter erwartete Layout
bringen (s. Option A2 / Format-Auswahl):
- `yuv420p` → `PlanarYuv420Frame` mit interleavter UV-Plane (U/V zeilenweise verweben, 8 Bit).
- `yuv420p10le` → dito **plus** je 16-bit-Sample `<< 6` (LSB-aligned → P010-MSB-Alignment 15:6).
- `yuv444p` → neuer `ConvertYuv444ToBgra` (planar 4:4:4, kein Chroma-Upsampling, BT.709/Range wie
  `ConvertYuv420ToBgra`) für den 8-bit-4:4:4-Expert-Fall.
_Verify (CI, GPU-los):_ eigener Unit-Test `test_yuv_repack` — synthetische planare Eingabe → Repack
→ Byte-Vergleich gegen von Hand berechnetes NV12/P010-Layout (inkl. `<<6`-Alignment); `yuv444p`-
Konverter gegen Referenzpixel. **Dieser Test schließt die Lücke, die `test_yuv_to_bgra`/
`test_hdr_preview` offenlassen** (die decken nur den bereits-interleavten Pfad ab).

**Schritt 2b — `FramePreviewDecoder` in `engine` (reiner Decode-Kern, UI-agnostisch).**
Neue Klasse, Header in `libs/engine/include/exosnap/engine/frame_preview_decoder.h`:
- `Open(path)` (MKV-Master) — **mit `FILE_SHARE_DELETE`-tauglichem Öffnen** (Custom-`AVIOContext`
  über einen mit `FILE_SHARE_READ|WRITE|DELETE` geöffneten Handle), damit ein offener Decoder den
  späteren Overwrite-`rename` auf den MKV-Master nicht blockiert (s. Schritt 3/4 + Risiken).
- `std::vector<int64_t> keyframes()` — delegiert an/teilt sich Logik mit `ExtractKeyframeTimestamps`.
- `bool DecodeFrameAt(int64_t target_us, DecodedFrameBgra& out)` — Seek auf Keyframe ≤ target,
  Decode-forward bis `pts >= target`, **Repack (Schritt 2a)** → Konvertierung via
  `ConvertYuv420ToBgra` / `P010PqMonitorConverter` / `ConvertYuv444ToBgra` je nach `AVFrame`-`pix_fmt`
  und Farbeigenschaften; Ausgabe top-down BGRA + Maße. Unerwartetes `pix_fmt` → `false` (inerter
  Fallback, kein Garbage).
- Keine Qt-Typen (Engine bleibt UI-agnostisch). Kein swscale.
_Verify (CI, GPU-los):_ **Ein committetes, vor-encodetes Binär-Fixture** in
`libs/engine/tests/data/` — ein via libavformat gemuxter synthetischer Stream ist **keine
gangbare Alternative**, weil r4 per Design **keine Encoder** enthält (und CI keine NVENC-GPU hat),
also kein dekodierbarer Elementarstream zur Laufzeit erzeugbar ist. Fixtures explizit festgelegt:
je ein winziges (~einige KB, wenige Frames, kleinste Auflösung) H.264-, HEVC- und AV1-MKV in **SDR**
plus **ein HDR10-Variant** (P010/PQ, z. B. HEVC oder AV1). `DecodeFrameAt` liefert für bekannte
Zeitpunkte plausibles BGRA (Maße korrekt, Seek landet ≤ Ziel, forward trifft Ziel); Farb-/Alignment-
Korrektheit deckt Schritt 2a ab.

**Schritt 3 — `PreviewDecodeWorker` (App-Seite, Thread + Coalescing).**
In `app/` ein dünner Qt-Wrapper: eigener Thread, hält einen `FramePreviewDecoder`, nimmt
Zielzeit-Requests entgegen (nur neueste bedient), liefert fertige `QImage` per
`QueuedConnection`. Öffnet den Decoder lazily beim ersten Anzeigebedarf.
**Close-Trigger korrekt gewählt — NICHT `hideEvent`.** `hideEvent` feuert nur beim Overlay-Dismiss
(`EditExportPage.cpp:1063-1067`); beim Phasenwechsel **Edit→Output bleibt die Seite sichtbar**, es
läuft `refreshPhase`. Der reale Hook ist daher `refreshPhase`: der Player existiert nur für
`show_player = (Review || Edit)` (`EditExportPage.cpp:913, 933-934`). Der Worker schließt den Decoder
(und gibt den MKV-Handle frei), **sobald `show_player` false wird** (Übergang nach Output/Exporting/
Done/Failed) — analog dazu, dass `refreshPhase` dort schon `setPreviewPlaying(false)` ruft — **und
zusätzlich** bei `hideEvent` (Dismiss). _Verify:_ Widget-Test mit eigener `QApplication`-Fixture
(Memory: gtest-Isolation): Request-Coalescing (bei 5 schnellen Requests wird nur die letzte Zielzeit
dekodiert), **Decoder ist nach `setPhase(Output)` geschlossen** (Handle freigegeben), sauberer
Teardown ohne Thread-Leak.

**Schritt 4 — Verdrahtung in `EditExportPage`: Frame andocken.**
- `player_sub_`-Platzhalter (`EditExportPage.cpp:233`) durch eine Bildfläche ersetzen (QLabel/
  eigenes Paint-Widget), die die `QImage` skaliert einpasst (16:9 via vorhandenes
  `updatePlayerHeight`).
- In `onScrubMoved` / `onPreviewTick` / `setPreviewPositionMs` die aktuelle Position als
  Decode-Request an den Worker geben (coalescing). Der Clock bleibt Zeitquelle.
- Beim `setEditContext` den Worker auf `ctx_.mkv_master_path` initialisieren; unbekannte Dauer /
  Split-Recordings (kein Edit-Master) → Bildfläche zeigt den vorhandenen inerten Zustand statt
  Decode.
- **Harte Anforderung — Decoder-Handle geschlossen, bevor `runExport` startet.** Für MKV-Aufnahmen
  ist `mkv_master_path == output_path` (`EditExportPage.h:37`), und der Overwrite-Export renamed
  `temp → output_path` (`EditExportPage.cpp:1221`). Hielte der Decode-Worker den MKV-Master offen,
  schlüge `std::filesystem::rename` unter Windows mit Sharing-Violation fehl → „Export failed" beim
  Kern-Use-Case Trim+Overwrite. Absicherung **zweifach**: (a) Der Worker schließt den Decoder beim
  `show_player`-False-Übergang (Schritt 3) — d. h. schon der Wechsel Edit→Output gibt den Handle
  frei, bevor der Output-Phase-„Save && export"-Klick `runExport` triggert; (b) `runExport`
  ruft **defensiv** `worker->closeDecoder()` synchron **vor** dem Start des Export-Threads (Gürtel
  + Hosenträger, falls Open-`FILE_SHARE_DELETE` je regressed). _Verify:_ Test — Decoder auf einen
  MKV-Master offen, dann Trim+Overwrite-Export → `rename` erfolgreich (keine Sharing-Violation),
  Master ersetzt.
_Verify (nur User-live):_ visuelles Scrubbing zeigt den korrekten Frame; Handle-/Playhead-Position
stimmt mit dem Bild überein. Der `--visual-test`-Harness kann eine **feste** injizierte `QImage`
(Fixture) rendern und layouten (Pixel-Proof des Player-Frames), aber echte Decode-Korrektheit an
Live-Files bleibt User-Verifikation.

**Schritt 5 — Spec/Doku-Sync.**
`docs/product-spec.md` §8 „Current boundary" (Zeile ~546-548) und `KNOWN_LIMITATIONS.md` (Zeile
~228-234) aktualisieren: Video-Scrubbing implementiert, **stumm**, kein Echtzeit-Playback im MVP.
ADR 0022 §Forward fortschreiben (0.11-Zeile: „decoded frames" → geliefert als Scrub-MVP; Playback/
Audio/D3D11VA als offene Increments). _Verify:_ Doku-Kanon-Check (deckt Review M-14).

**Increment 2 (separater Slice) — D3D11VA + Echtzeit-Playback.**
avcodec-Hwaccel `d3d11va` (Build-Flag im FFmpeg-Build aktivieren, r5), Decode in D3D11-NV12/P010,
GPU-Konvertierung über die vorhandenen `DxgiPreviewRenderer`-Shader bzw. `HdrToneMapper`, getakteter
Readback → `QImage`. Ermöglicht flüssiges 60-fps-Playback + billiges 4K. _Verify:_ Perf-Messung
(Frame-Time), User-live.

**Increment 3 (separater Slice) — Audio-Playout + A/V-Sync.**
Audio-Decoder in den Build, WASAPI-Render-Client, A/V-Master-Clock. Eigene Spec.

## Test-/Verify-Plan

**CI-fähig (GPU-los):**
- **`recorder_core.test_yuv_repack` (Schritt 2a) — neu:** planar→interleaved-Repack
  (`yuv420p`→NV12, `yuv420p10le`→P010 inkl. `<<6`-Alignment) + `ConvertYuv444ToBgra`. **Diese Lücke
  ist NICHT durch die bestehenden Tests abgedeckt** — `test_yuv_to_bgra`/`test_hdr_preview` prüfen
  nur den bereits-interleavten Konverter-Eingang, nicht den neuen Repack. (Korrigiert die frühere
  Annahme „Farbkonvertierung bereits abgedeckt, unverändert".)
- `recorder_core.frame_preview_decoder` (Schritt 2b): Seek-Genauigkeit (Keyframe ≤ Ziel, forward
  trifft Ziel), Ausgabemaße, Fallback bei fehlendem/unerwartetem `pix_fmt` (leeres Ergebnis, kein
  Crash). Nutzt die **committeten vor-encodeten Fixtures** (H.264/HEVC/AV1 SDR + HDR10-Variant).
- `test_yuv_to_bgra` / `test_hdr_preview` bleiben unverändert und decken die Farbmathematik
  **hinter** dem Repack ab.
- `PreviewDecodeWorker` (Schritt 3): Request-Coalescing + Teardown + Decoder-Close bei
  `show_player`-False, mit `QApplication`-Fixture.
- **Export-Handle-Test (Schritt 4):** offener Decoder auf MKV-Master → Trim+Overwrite → `rename`
  ohne Sharing-Violation.
- Doku-Kanon-Konsistenz (Schritt 5).
- `--visual-test`: Layout/Pixel-Proof der Player-Fläche mit **injizierter** Fixture-`QImage`.

**Nur User-live (nicht CI):**
- Tatsächliche Decode-Korrektheit an echten Aufnahmen (AV1/HEVC/H.264, SDR + nativ-HDR10): stimmt
  das gescrubte Bild mit der Playhead-Position; sieht HDR wie das Live-Preview aus (**gleicher SDR-
  Roll-off, sofern der Editor denselben Display-Peak ermittelt** — s. Section C; bei Peak-Fallback
  ist eine leichte Highlight-Abweichung der benannte Kauf).
- Scrub-Responsiveness/Ruckelverhalten bei 1080p vs. 4K (informiert die Priorität von Increment 2).
- Kein Live-App-Driving durch Agenten (CLAUDE.md): diese Checks macht der User.

## Risiken

- **FFmpeg-Build-Abhängigkeit extern.** Schritt 0/1 hängen am separaten `exosnap-ffmpeg-build`-Repo
  (maintainer-gated). Bis r4 existiert, kann der App-Code (Schritte 2–4) gegen einen lokalen
  Decoder-fähigen FFmpeg entwickelt werden, aber CI/Release erfordert den gepinnten Release.
- **Software-Decode-Performance.** 4K-AV1-Software-Decode ist für kontinuierliches Playback zu
  langsam — genau deshalb ist Playback nicht im MVP. Scrubbing (Einzelframe on release) bleibt
  akzeptabel; schnelles Drag wird durch Coalescing + evtl. Decode-nur-auf-Release entschärft.
- **DLL-Größenzuwachs.** +2–4 MB avcodec (dav1d). Vertretbar; klar unter dem alten 88-MB-Download.
- **Farb-/Range-Detektion aus dem Frame.** Falls ein File untypische/fehlende `color_trc`-Tags
  trägt, könnte die SDR/HDR-Weiche danebengreifen. Mitigation: Default BT.709/Limited, HDR nur bei
  eindeutigem PQ+10-bit; da ExoSnap die Files selbst schreibt (BT.709-Tags garantiert, ADR 0032),
  ist das im Normalfall deterministisch.
- **Kein natives Child-HWND im MVP** vermeidet den Overlay-Compositing-Zwang — falls Increment 2
  je auf ein natives D3D11-HWND in der Player-Fläche geht, muss der ADR-0022-Backdrop-/Sibling-
  Mechanismus dort mitgedacht werden.
- **Overwrite-Export vs. offener MKV-Master (Windows-Sharing).** Für MKV ist
  `mkv_master_path == output_path`; ein offener Decode-Handle ohne `FILE_SHARE_DELETE` lässt den
  Overwrite-`rename` mit Sharing-Violation scheitern → „Export failed" im Kern-Use-Case. Mitigation:
  `FILE_SHARE_DELETE`-tauglicher Open (Schritt 2b) + Close bei Edit→Output + defensiver Close vor
  `runExport` (Schritt 4), abgesichert durch den Export-Handle-Test.
- **Worst-Case-Scrub-Latenz bei langen GOPs.** Default-Keyframe-Intervall 2 s = 120 Frames @60 fps
  → bis zu ~119 Frames Forward-Decode für ein Ziel kurz vor dem nächsten Keyframe; bei 4K-AV1-
  Software-Decode grob ~1 s pro exaktem Frame. Mitigation: Scrub-on-Release (während des Drags nur
  nächstliegender Keyframe, exakter Frame erst on release) + Coalescing. „Frame-genaue Latenz
  unkritisch" gilt nur für 1080p/kurze GOPs.
- **HDR-Roll-off-Abweichung bei Peak-Fallback.** Ist beim Editor-Open kein Display-Peak
  ermittelbar, klippt der Referenz-Peak Highlights leicht anders als das Live-Preview — als Kauf
  benannt (Section C); Normalfall = identischer Peak-Query wie der Capture-Pfad.
- **Patent-Exposition Software-HEVC/H.264-Decode.** Ein ausgelieferter Software-Decoder ist eine
  neue Patent-Expositionsfläche (HEVC-Pools erfassen Decoder). Mitigation: dokumentierte Position
  vor r4 (Schritt 0, analog ADR 0043); AV1/dav1d royalty-free.

## Offene Fragen

- **Echtzeit-Playback überhaupt Ziel, oder bleibt „Scrubbing genügt"?** Wenn frame-genaues
  Scrubbing für den Trim-Use-Case reicht, könnte Increment 2 (D3D11VA/Playback) dauerhaft
  descopet bleiben — das spart den GPU-Decode-Pfad komplett.
- **Ton im Preview überhaupt gewünscht?** Ein stummer Trim-Editor ist branchenüblich (Schnittmarken
  setzt man visuell). Falls A/V-Playback nie Produktziel ist, entfällt Increment 3 samt WASAPI-
  Render-Subsystem ersatzlos.

## Adversarialer Review — Ergebnis

- **[major] Planarer Decoder-Output ≠ interleavtes Konverter-Layout** — **eingearbeitet.** Bestätigt
  gegen `yuv_to_bgra.h:36-53`: die Konverter verlangen interleavte UV + (10 Bit) MSB-Alignment 15:6,
  Software-Decoder liefern `yuv420p`/`yuv420p10le` (planar, LSB). Repack-Adapter als eigenes
  Arbeitspaket (Schritt 2a) + `test_yuv_repack`; A2/Format-Auswahl/Ist-Zustand korrigiert; die
  „bereits abgedeckt/unverändert"-Testaussage revidiert.
- **[major] `--enable-decoder=av1` ist hwaccel-only, kein Software-AV1** — **eingearbeitet.**
  Bestätigt (FFmpeg `av1dec.c` = hwaccel-Bridge). Schritt 0 auf `--enable-libdav1d
  --enable-decoder=libdav1d` umgestellt, dav1d als statisch gelinkte Drittabhängigkeit benannt,
  Verify um echten Software-Decode-Smoke (statt false-positive `-decoders`-Listing) ergänzt;
  dav1d-Eintrag in THIRD_PARTY + License-Staging (Schritt 1).
- **[major] Overwrite-`rename`-Sharing-Violation + falscher Close-Trigger** — **eingearbeitet.**
  Bestätigt: `mkv_master_path == output_path` (`.h:37`), Overwrite renamed temp→master
  (`.cpp:1221`); `hideEvent` feuert nicht bei Edit→Output — der reale Hook ist `refreshPhase`/
  `show_player` (`.cpp:913,933-934`). Close an `show_player`-False gehängt, `FILE_SHARE_DELETE`-Open
  + defensiver Close vor `runExport` als harte Anforderung + Test + Risiko.
- **[minor] HDR-`peak_scale`-Inkonsistenz** — **eingearbeitet.** Bestätigt: Live-Pfad nutzt
  `hdrPeakScale` (`video_thread.cpp:1935-1939`). Editor ermittelt jetzt denselben Display-Peak per
  DisplayConfig-Query; Fallback-Abweichung explizit als Kauf benannt, User-live-Test entsprechend
  formuliert.
- **[minor] `yuv444p`-Verhalten undefiniert** — **eingearbeitet.** Bestätigt: 8-bit-4:4:4 ist
  geshippte Expert-Option (`product-spec:761-762`), SW-Decode → planares `yuv444p`, für das kein
  vorhandener Konverter passt. MVP-Verhalten definiert: planarer `ConvertYuv444ToBgra` bzw. inerter
  Fallback (Section B / Schritt 2a).
- **[minor] Worst-Case-Scrub-Latenz** — **eingearbeitet.** Bestätigt: 2 s GOP = 120 Frames @60 fps
  (`product-spec:542-543`). Zu-starke „unkritisch"-Aussage qualifiziert; Scrub-on-Release-Mitigation
  in Section B + Worst-Case ins Risiko-Kapitel.
- **[minor] Synthetischer libavformat-Stream nicht dekodierbar** — **eingearbeitet.** Bestätigt: r4
  hat per Design keine Encoder, CI keine NVENC-GPU. Scheinalternative gestrichen; committete
  vor-encodete Fixtures explizit festgelegt (H.264/HEVC/AV1 SDR + HDR10-Variant, Größe/Ort).
- **[minor] Fehlende Patentposition Software-HEVC/H.264-Decode** — **eingearbeitet.** Bestätigt:
  `roadmap.md:35-37` verlangt Patent-Audit für Codec-Distribution, ADR 0043 dokumentiert Positionen.
  Dokumentierte Decoder-Patentposition (analog ADR 0043) in Schritt 0 + Risiko eingeplant.

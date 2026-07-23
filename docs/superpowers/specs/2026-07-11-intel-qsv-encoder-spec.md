# Intel oneVPL/QSV-Hardware-Encoder (Roadmap 0.13)

**Status:** Spec, umsetzungsreif · **Zielrelease:** 0.13.0 · **Autor-Modell:** Fable (Spec-Welle 2026-07-11)

Verwandte Specs (parallel entstanden, gemeinsame Infrastruktur dort — hier referenziert, nicht dupliziert):

- `.workspace/plans/software-encoding-spec.md` (0.11) — führt `VideoEncoderFactory`, `EncoderSelectionPolicy`,
  die Backend-neutrale Codec-/Config-Modellierung (Ablösung der `*Nvenc`-Suffixe) und den GPU→CPU-Readback ein.
- `.workspace/plans/amd-amf-encoder-spec.md` (0.12) — etabliert das Multi-Vendor-Muster: per-Vendor-RuntimeFacts,
  Generalisierung der Downgrade-Regeln in `capability_builder.cpp`, Hardware-Matrix-Methodik, Device-Tab-Flip
  von „Planned" auf real.

Diese Spec setzt beide voraus (Roadmap-Reihenfolge 0.11 → 0.12 → 0.13) und spezifiziert nur, was QSV-spezifisch
ist plus die Annahmen an die gemeinsamen Schnittstellen. Ownership der geteilten Infrastruktur ist eindeutig:
**0.11 landet `VideoEncoderFactory`/`EncoderSelectionPolicy`/Codec-Rename** (software-encoding-spec D2/S2);
die AMF-Spec markiert sie als Voraussetzung (dort D0/S0 „eigene Welle 0.11") und fügt nur den `Amf`-Zweig
hinzu; diese Spec fügt den `Qsv`-Zweig hinzu. Wo diese Spec eine gemeinsame Schnittstelle beschreibt,
gilt bei der Umsetzung der dann tatsächlich gelandete Code — nicht der Wortlaut hier.

---

## Problem

ExoSnap encodiert ausschließlich über NVIDIA NVENC. Ohne unterstützte NVIDIA-GPU ist Aufnahme blockiert
(`docs/product-spec.md:435-436`, `KNOWN_LIMITATIONS.md:20-39`). Intel-Hardware ist dabei die mit Abstand
verbreitetste ungenutzte Encoder-Quelle:

1. **Hybrid-Laptops** (Intel-iGPU + NVIDIA-dGPU) sind der Normalfall im Consumer-Segment. Die iGPU wird heute
   im Device-Tab ehrlich als Adapter enumeriert, ihr Encoder aber als „Planned"-Zeile geführt
   (`app/pages/DevicePage.cpp:344-347`: „Intel · Quick Sync (QSV) — iGPU encode — detected above, backend
   not yet wired").
2. **Reine Intel-Systeme** (Laptop ohne dGPU, Arc-dGPU-Desktops) können gar nicht aufnehmen — erst 0.11
   (Software-Encoding) gibt ihnen einen CPU-Fallback; QSV gibt ihnen den Hardware-Pfad, auf Arc inklusive
   **AV1-Hardware-Encode** (dem Default-Codec des Produkts, `docs/product-spec.md:80`).
3. Die Roadmap verspricht 0.13 explizit: „Native oneVPL/QSV, allocator/surface integration, hardware test
   matrix, diagnostics provider, fallback behavior" (`docs/roadmap.md:87`), gestützt auf ADR 0006 (native
   Vendor-SDKs statt FFmpeg-Wrapper, `docs/decisions/0006-native-vendor-sdks-for-hardware-encoders.md`).

Nicht-Ziele dieser Spec (bewusst NICHT gebaut):

- **Kein Cross-Adapter-Frame-Transfer** (Capture auf GPU A, Encode auf GPU B) — siehe Design D3.
- **Kein 4:2:2** und **kein 4:4:4** auf dem QSV-Pfad in v1 (4:4:4 bleibt ein NVENC-Feature, per Capability
  ehrlich gegated).
- **Kein oneVPL-VPP** (Scaling/CSC bleibt beim D3D11 VideoProcessor) — siehe Design D5.
- **Kein Legacy-MSDK-Pfad** für Gen9–Gen11 (Skylake–Ice Lake) — siehe Design D2 und Offene Fragen.
- **Kein Encoder-Async-Umbau** — `IVideoEncoder` bleibt synchron; die Async-Pipeline ist M-1
  (`.workspace/plans/nvenc-async-pipeline-spec.md`) und wird hier nur nicht verbaut.

---

## Ist-Zustand (Stand main @ #192, alle Fakten frisch erhoben)

### Encoder-Pfad und Gerätemodell

- **Kein Factory-Zwischenschritt:** `video_thread.cpp` instanziiert den Encoder direkt als lokale Variable
  `NvencVideoEncoder nvenc;` (`libs/recorder_core/src/video_thread.cpp:550`) und konfiguriert ihn über
  Setter (`SetCodec/SetBitDepth/SetChroma/SetCq/SetRateControl/SetPreset/SetKeyframeIntervalSecs/SetColor`,
  Zeilen 552–570). `VideoEncoderFactory`/`EncoderSelectionPolicy` aus ADR 0006 existieren noch nicht —
  sie entstehen in der 0.11-Spec.
- **Interface:** `IVideoEncoder` (`libs/recorder_core/include/recorder_core/interfaces/IVideoEncoder.h:20-58`)
  ist ein Slot-Modell: `Open(void* gpu_context /* ID3D11Device* */, …)` (Z. 26), `Configure(w,h,fps)`,
  `RegisterSlotTexture(slot, GpuTextureHandle)` (Z. 34), `AcquireFreeSlot()`, synchrones
  `EncodeFrame(slot, pts_ns, …)` (Z. 45), `Flush()`, `RequestKeyframe()` (Z. 54, One-Shot-IDR für
  Segment-Splits), `Destroy()`.
- **Paketmodell ohne DTS:** `EncodedVideoPacket` trägt nur `bytes`, `pts_ns`, `keyframe`
  (`libs/recorder_core/include/recorder_core/packet_types.h:8-12`). B-Frames sind damit strukturell
  ausgeschlossen (Mux erwartet Präsentationsreihenfolge = Übergabereihenfolge); NVENC läuft entsprechend
  ohne B-Frames.
- **D3D11-Gerätepolitik:** Der VideoThread erzeugt genau EIN D3D11-Device und teilt es zwischen Capture,
  VideoProcessor, Compositor und Encoder. Für Monitor-Targets wird der Adapter gesucht, dem der HMONITOR
  gehört (`video_thread.cpp:210-216`, `FindAdapterForMonitor`), und das Device explizit darauf erzeugt
  (Z. 234-238) — Pflicht für `DuplicateOutput` auf Multi-GPU-Systemen. Für Window-Targets (WGC) wird das
  Device auf dem **Default-Hardware-Adapter** erzeugt (Z. 239-242) — es gibt heute keine Steuerung, auf
  welchem Adapter eine WGC-Session encodiert.
- **Format-Pipeline:** D3D11 VideoProcessor konvertiert RGB→NV12 (8-bit) / P010 (10-bit); 4:4:4 nutzt einen
  Compute-Shader nach BGRA-Zwischenstufe (`video_thread.cpp:589-622`). Der Encoder registriert 8 Slot-Texturen
  (`kSlotCount = 8`, Z. 606) im jeweiligen Encode-Format.
- **Bitstream-Konventionen:** H.264/HEVC werden als Annex-B mit In-Band-Parameter-Sets geliefert; der Mux-Pfad
  baut `avcC`/`hvcC` daraus (`libs/recorder_core/src/annexb_to_avcc.cpp`, `annexb_to_hvcc.cpp`). AV1 kommt als
  Low-Overhead-OBU-Stream. Jeder neue Backend muss exakt dieselbe Framing-Konvention liefern.
- **Farb-/HDR-Signalisierung:** `ApplyColorMetadataToNvenc` mappt `ColorMetadata` pur auf die
  NVENC-Bitstream-Felder (`libs/recorder_core/src/nvenc_encoder.h:44-57`). HDR10-Static-Metadata wird als
  purer Payload gebaut (`libs/recorder_core/include/recorder_core/hdr_bitstream_metadata.h:16-28`:
  HEVC-SEI-137/144-Payloads, AV1-MDCV/CLL-OBU-Payloads) und NVENC-seitig per `seiPayloadArray` /
  `obuPayloadArray` auf jedem Keyframe injiziert.
- **Rate-Control:** kanonisches Modell (ADR 0009) → NVENC-Mapping in purem `ComputeNvencRcParams`
  (`nvenc_encoder.h:93-109`); GOP aus `keyframe_interval_secs` via `ComputeGopLength` (`nvenc_encoder.h:112-121`,
  seit #181 erreicht der Wert den Encoder wirklich).
- **Config trägt NVENC-Vokabular:** `RecorderConfig` hat `nvenc_cq`, `nvenc_rate_control`, `nvenc_preset`,
  `nvenc_bitrate_kbps` (`libs/recorder_core/include/recorder_core/recorder_session.h:289-306`). Die
  Backend-Neutralisierung dieser Felder ist Aufgabe der 0.11-Spec.

### Capability-Modell

- **Codec-Enums sind NVENC-benannt:** `recorder_core::VideoCodec { Av1Nvenc, H264Nvenc, HevcNvenc }`
  (`libs/recorder_core/include/recorder_core/codec_types.h:13-17`) und spiegelbildlich
  `capability::VideoCodec` (`libs/capability/include/capability/config_types.h:10`). Die Entkopplung
  Codec ↔ Backend ist 0.11-Arbeit.
- **RuntimeSnapshot ist Single-Adapter + NVIDIA-zentrisch:** `NvidiaRuntimeFacts`
  (`libs/capability/include/capability/runtime_snapshot.h:57-81`), eingebettet in
  `RuntimeCapabilitySnapshot` (Z. 107-113). Es gibt keine Intel-/AMD-Facts.
- **Downgrade-Regel A blockiert ALLES ohne NVENC:** fehlt die NVENC-DLL, werden alle drei Video-Codecs
  systemweit `NotImplemented` (`libs/capability/src/capability_builder.cpp:117-145`) — genau die Regel, die
  AMF/QSV/Software-Wellen zu „kein GEWIRETES Backend unterstützt Codec X auf diesem System" generalisieren
  müssen (Umbau in der AMF-Spec; QSV hängt sich additiv ein).
- **Per-Adapter-Probe existiert additiv:** `capability::EnumerateAdapters()` klassifiziert Vendor via PCI-ID
  (Intel = 0x8086, `libs/capability/include/capability/adapter_enum.h:13,41`) und liefert LUID + iGPU/dGPU-
  Heuristik. `ProbeAdapterEncoderCapability()` probt NVIDIA real (NVENC-Session per LUID-gematchtem Device,
  `libs/capability/src/adapter_capability.cpp:55-179`) und gibt für Intel bewusst
  `probed=false` + „encoder backend not yet supported…" zurück (`adapter_capability.cpp:45-46,201-210`) —
  die dokumentierte Ehrlichkeitsregel: **nie proben, was kein Backend nutzen kann.**
- **Cache-Warm-Start:** Der Disk-Cache ist auf EINE Adapter-LUID + Treiberversion + App-Version + Schema
  gekeyt (`libs/capability/include/capability/capability_cache_key.h:26-33`); ein Intel-Treiberwechsel
  invalidiert ihn heute nicht.
- **Recommended-Codec-Resolver:** `BestAvailableVideoCodec` (AV1 → HEVC → H.264, Container-valide,
  `libs/capability/include/capability/codec_selection.h:26-33`) ist backend-agnostisch formuliert und
  funktioniert unverändert, sobald die Capability-Flags multi-backend-korrekt gefüllt sind.

### UI / Diagnostics

- **Device-Tab:** ein Karten-Selektor pro DXGI-Adapter + per-Adapter-Matrix. Auswahl ist heute
  **inspection-only** — „does not steer the encoder … that coupling is a documented follow-up slice"
  (`app/pages/DevicePage.h:46-49`). Der „Active encoder"-Badge ist hart als „erster NVIDIA-Adapter mit
  erfolgreicher Probe" definiert (`DevicePage.h:107-111`); QSV als Roadmap-Zeile in `buildRoadmapSection`
  (`DevicePage.cpp:344-347`). Test-Seam `setAdaptersForTest()` existiert (`DevicePage.h:72-73`).
- **Produkt-Spec:** Device-Tab-Beschreibung inkl. „honest greyed-out planned rows — never fabricated probes"
  (`docs/product-spec.md:47-51`); Blocker-Formulierung „If no supported NVIDIA NVENC encoder is detected,
  recording is blocked" (`docs/product-spec.md:435-436`).
- **Sichtbare Schreibweise:** `app/ui/CodecLabels.h` + `capability::VisibleVideoCodecLabel` sind der Kanon;
  Backend-Namen tauchen dort noch nicht auf (nur Codec-Kurzlabels „AV1 / HEVC / H.264").
- **Cross-Adapter-Grenze ist bereits Produkt-Fakt:** Preview-Sharing über GPU-Grenzen ist unsupported und
  fällt auf eigene WGC-Capture zurück (`KNOWN_LIMITATIONS.md:215-219, 288-291`).

### Vendoring-Präzedenz

- NVENC wird als einzelner Vendor-Header vendored (`third_party/nvidia/nvEncodeAPI.h`), mit
  `__has_include`-Degradierung im Capability-Code (`libs/capability/src/runtime_query.cpp:44-50`) und
  CMake-Hinweis statt Hard-Fail (`libs/recorder_core/CMakeLists.txt:1-5`). Lizenzen werden über
  `_exosnap_install_license` in `third_party/CMakeLists.txt` gestaged. Es gibt keinerlei oneVPL-Bezüge im
  Baum (grep über `third_party/`, `THIRD_PARTY_NOTICES.md`: leer).

---

## Design

### D1 — Dispatcher-Beschaffung: libvpl via FetchContent (statisch), nicht Header-only-Nachbau

**Alternativen:**

| | (a) Intel-VPL-Dispatcher (`libvpl`) via FetchContent, statisch gelinkt | (b) NVENC-Muster kopieren: nur Header vendoren, Runtime-DLL selbst per `LoadLibraryW` laden |
|---|---|---|
| Runtime-Discovery | Dispatcher findet `libmfx64-gen.dll` (VPL-Runtime) UND Legacy-`libmfxhw64.dll` im Driver-Store über die offizielle Discovery (D3DKMT/Registry) | Muss selbst nachgebaut werden: Die Intel-Runtimes liegen **nicht** unter einem festen System32-Namen, sondern versioniert im Driver-Store — genau das Problem, für das der Dispatcher existiert |
| Multi-Adapter (iGPU + Arc) | `MFXEnumImplementations` liefert pro Implementierung Gerätebeschreibung inkl. LUID (`mfxExtendedDeviceId`, API ≥ 2.6) | Selbst zu bauen |
| Lizenz | MIT — passt 1:1 ins `_exosnap_install_license`-Muster | Header sind MIT, egal |
| Kosten | Eine FetchContent-Dependency (~kleine statische Lib), Build-Zeit minimal | Kein Dependency-Zuwachs, aber dauerhaft eigener Discovery-Code mit Intel-Treiber-Kopplung |

**Entscheidung: (a).** Das NVENC-Muster funktioniert nur, weil `nvEncodeAPI64.dll` ein stabiler
System32-Name ist; für Intel ist der Dispatcher die einzige robuste Discovery. Statisch linken (keine neue
DLL im Portable-ZIP/MSI, keine Packaging-/Updater-Änderung). CMake-Gate `EXOSNAP_HAVE_ONEVPL` analog zum
NVENC-`__has_include`-Muster: ein Build ohne die Dependency (z. B. abgeschaltet) kompiliert weiter und
degradiert zu „QSV-Backend nicht eingebaut". Auf Systemen ohne Intel-Treiber liefert die Enumeration schlicht
null Implementierungen — kein Fehlerpfad, kein Crash, CI-sicher.

Version pinnen (GIT_TAG auf einen v2.x-Release-Commit, wie bei allen FetchContent-Deps im Repo), Lizenz nach
`licenses/libvpl.txt` stagen, `THIRD_PARTY_NOTICES.md` ergänzen.

### D2 — Runtime-Scope: VPL-2.x-Hardware-Runtime only (Gen12+/Arc), kein Legacy-MSDK

**Alternativen:**

- **(a) Nur VPL-2.x-Runtime** (`libmfx64-gen.dll`; Tiger Lake/Gen12 und neuer, DG1, Arc/DG2, Meteor Lake+):
  volle 2.x-Memory-API (`MFXMemory_GetSurfaceForEncode`, `mfxFrameSurfaceInterface`) — kein externer
  Allocator nötig.
- **(b) Zusätzlich Legacy-MSDK** (`libmfxhw64.dll`; Skylake–Ice Lake): der Dispatcher kann die laden, aber
  die 1.x-API verlangt die komplette `mfxFrameAllocator`-Maschinerie (externe D3D11-Allocation, Mid-Ops,
  Response-Tracking) — ein zweiter, eigener Speicherpfad nur für ≥6 Jahre alte iGPUs mit schwachen Encodern
  (teils ohne ICQ, HEVC erst ab Gen9.5 brauchbar).

**Entscheidung: (a).** Der Legacy-Pfad verdoppelt die Allocator-Komplexität für die schwächste Hardware-Klasse,
die ab 0.11 ohnehin den Software-Fallback bekommt. Konsequenz ehrlich dokumentieren: Gen9–Gen11-iGPUs zeigen im
Device-Tab „Quick Sync erkannt, aber Runtime zu alt (VPL 2.x erforderlich)" mit `probed=false`-analoger
Ehrlichkeit — nie ein fabrizierter Codec-Support. Aufnahme dort: Software-Encoder (0.11) oder NVENC, falls
vorhanden. (Bestätigung dieser Support-Grenze: siehe Offene Fragen F2.)

Minimal geforderte API-Version beim Session-Filter: **2.2** (Baseline für stabile `mfxEncoderDescription`-
Enumeration); Features mit höherer Anforderung (Surface-Import 2.10) werden zur Laufzeit per Report gegated.
**Explizite Grenze der 2.2-Baseline:** die LUID-Auskunft (`mfxExtendedDeviceId`) gibt es erst ab API 2.6.
Auf 2.2–2.5-Runtimes greift der `VendorID`/`DeviceID`-Fallback (D6) — der deckt den relevanten
Multi-Intel-Fall iGPU+Arc ab (unterschiedliche DeviceIDs), scheitert aber ehrlich (`probed=false`, nie raten)
bei **identischen** Intel-Doppeladaptern (z. B. zwei gleiche Arc-Karten). Für solche Systeme ist die
faktische Baseline 2.6; das gehört als Satz in den KNOWN_LIMITATIONS-Absatz (S9).

### D3 — Hybrid-Systeme: Encode-Device = Capture-Device („Same-Adapter-Regel"), Device-Erzeugung unangetastet

Das ist die zentrale Architektur-Entscheidung. Fakten: OD-Capture MUSS auf dem Adapter des Monitors laufen
(`video_thread.cpp:234-238`); WGC akzeptiert ein Device auf einem beliebigen Adapter (der Compositor kopiert
notfalls über die GPU-Grenze); der gesamte Konvertierungs-/Compositing-Pfad hängt am selben Device.

**Alternativen:**

1. **Same-Adapter-Regel (gewählt):** QSV ist als Backend nur wählbar, wenn das Session-Device auf einem
   Intel-Adapter liegt. Konkret:
   - **Monitor-Target:** Das Device liegt zwingend auf dem Adapter, der den Monitor treibt. Hängt das Display
     an der iGPU (Laptop-Panel im Hybrid-Normalfall!) → QSV nutzbar, zero-extra-copy. Hängt es an der
     NVIDIA-dGPU → NVENC (bzw. Fallback-Kette), niemals QSV.
   - **Window/Region-Target (WGC):** Das Device ist heute „Default-Adapter" (`video_thread.cpp:239-242`) —
     **und bleibt es in v1.** Die `EncoderSelectionPolicy` (0.11) wählt das Backend NACH feststehendem
     Session-Adapter (pure Funktion über Vendor des Session-Adapters + Probe-Facts + Codec), sie steuert
     die Device-Erzeugung nicht. Konsequenz: Auf reinen Intel-Systemen ist der Default-Adapter der (einzige)
     Intel-Adapter → QSV greift ohne jede Steuerung. Auf Hybrid-Systemen entscheidet die OS-Default-Adapter-
     Wahl; ist das die iGPU (Laptop-Normalfall mit iGPU-Panel), wählt die Policy QSV — heute scheitert auf
     genau diesem Pfad die NVENC-Session-Eröffnung auf dem Nicht-NVIDIA-Device, QSV ist dort also eine
     strikte Verbesserung. Eine gezielte Adapter-Steuerung für WGC (`encode_adapter_luid` in `RecorderConfig`
     + LUID-basierte Device-Erzeugung) ist bewusst KEIN v1-Umfang: sie diente nur dem Edge-Case
     „Fensteraufnahme zwangsweise auf einen bestimmten Adapter legen", wäre die riskanteste Änderung der
     Welle (berührt den Default-Adapter-Pfad) und gehört zum selben Follow-up-Slice wie die
     Device-Tab-Kopplung (Offene Frage F1).
2. **Cross-Adapter-Transfer in der Engine** (Capture auf GPU A, Shared-Handle/Keyed-Mutex oder CPU-Staging
   auf GPU B): verworfen. Zwei Devices im VideoThread brechen den dokumentierten Threading-/Ownership-Kontrakt
   (`video_thread.cpp:49-57`), Cross-GPU-Handle-Sharing ist im Produkt bereits als unsupported markiert
   (`KNOWN_LIMITATIONS.md:215-219`), und der CPU-Staging-Weg vernichtet genau den Zero-Copy-Vorteil, der QSV
   gegenüber dem 0.11-Software-Pfad rechtfertigt. Wer den Copy-Preis zahlen will, bekommt mit x264 ab 0.11
   bereits einen funktionierenden Fallback.
3. **QSV immer auf der iGPU erzwingen (Offload-Modell à la OBS):** verworfen für v1 — für OD-Monitor-Capture
   technisch unmöglich ohne (2), und als Default die falsche Wahl (NVENC ist auf dem dGPU-Pfad qualitativ und
   im Track-Record überlegen).

**Konsequenzen der Same-Adapter-Regel, ehrlich benannt:**

- Auf einem Hybrid-System mit dGPU-getriebenem Display bleibt QSV für Monitor-Aufnahmen unerreichbar. Das ist
  KEIN Diagnostics-Problem (Aufnahme funktioniert via NVENC) — es wird ausschließlich im Device-Tab als
  Provenance-Satz erklärt („usable for window capture; this display is driven by the NVIDIA GPU"). Keine Karte,
  kein Alarm (Diagnostics-ruhig-Regel).
- Es gibt in v1 keinen Mechanismus, eine WGC-Session gezielt auf einen bestimmten Adapter zu legen — der
  Session-Adapter ist der OS-Default. Wer auf einem Hybrid-System mit dGPU-Default-Adapter zwingend QSV für
  Fensteraufnahmen will, muss auf den F1-Follow-up warten (persistierte Expert-Adapter-Wahl inkl.
  Device-Erzeugung per LUID).
- Landet eine WGC-Session auf dem Intel-Device (Default-Adapter), kann die WYSIWYG-Preview-Freigabe scheitern,
  wenn die Preview auf einem anderen Adapter läuft — der existierende Fallback (Preview behält eigene
  WGC-Capture, `KNOWN_LIMITATIONS.md:215-219`) greift unverändert; Aufnahme unbeeinflusst.

### D4 — Surface-/Allocator-Integration: Runtime-Surfaces + `CopyResource`, Import als vermessener Follow-up

`IVideoEncoder` gibt dem Encoder 8 registrierte NV12/P010-Slot-Texturen. QSV-Optionen:

1. **Zero-Copy-Import** der Slot-Texturen (`MFXMemory_ImportFrameSurface` mit `mfxSurfaceD3D11Tex2D`,
   API ≥ 2.10): eleganteste Lösung, aber harte Runtime-Anforderung (sehr neue Treiber) und zusätzliche
   Bind-Flag-/Shared-Flag-Verhandlung mit dem VideoProcessor-Output.
2. **Runtime-eigene Surfaces** (`MFXMemory_GetSurfaceForEncode`) **+ eine GPU→GPU-`CopyResource`** vom
   Slot-Texture in die Runtime-Surface pro Frame. Das `CopyResource`-Ziel (die `ID3D11Texture2D` hinter der
   Runtime-Surface) kommt ausschließlich über `mfxFrameSurfaceInterface::GetNativeHandle` —
   **GetNativeHandle liegt damit auf dem Hot-Path** (einmal pro acquired Surface; das Handle ist pro
   Surface-Objekt stabil und darf gecacht werden, solange die Surface nicht released ist). Die Kopie läuft
   über den Immediate Context desselben Devices/Threads. Der Pfad ist
   robust über alle 2.x-Runtimes, keine Kontrakt-Änderung an `IVideoEncoder`, Kosten = eine on-GPU-Kopie
   eines NV12-Frames (bei 4K60 grob niedrige einstellige GB/s — auf iGPU-Bandbreite messbar, aber weit unter
   dem, was der VideoProcessor-Blt selbst kostet).
3. **VideoProcessor direkt in Runtime-Surfaces rendern lassen:** verworfen — Runtime-Texturen garantieren
   kein `D3D11_BIND_RENDER_TARGET`, und das Surface-Lifecycle-Modell (Release nach Sync) passt nicht zum
   statischen 8-Slot-Ring des VideoThreads.

**Entscheidung: (2) als Baseline, (1) als abgegrenzter Follow-up**, nur wenn die Perf-Messinfrastruktur aus
M-1 (`p99-Encode-Latenz`, Frame-Time-Histogramm) auf realer Intel-Hardware zeigt, dass die Kopie weh tut.
Der Follow-up ist dann eine reine `QsvEncoder`-interne Änderung hinter demselben Interface.

**Kein VPP (D5):** oneVPLs `mfxVPP` wird nicht benutzt. RGB→NV12/P010 inkl. Crop/Scale/Letterbox und
Range-Handling macht weiterhin der D3D11 VideoProcessor — der läuft auf Intel-Devices genauso (Intel-VP-Treiber
sind solide), hält die Pipeline vendor-uniform und erspart eine zweite Farbkonvertierungs-Semantik samt
eigener Verify-Matrix. 4:4:4 (AYUV-Pfad) wird für QSV nicht angeboten: `QueryChroma444` bleibt für den
QSV-Backend `NotImplemented` — ehrliche per-Backend-Gating statt ungetesteter RExt-Pfade.

### D6 — Capability-Probe: `MFXEnumImplementations` (sessionlos), LUID-gematcht, Ehrlichkeitsregel beibehalten

Die oneVPL-Probe ist **billiger und präziser** als die NVENC-Probe: `MFXEnumImplementations` mit
`MFX_IMPLCAPS_IMPLDESCSTRUCTURE` liefert pro Implementierung die komplette `mfxEncoderDescription`
(Codec-IDs mit Profilen, Memory-Types, Farb-Formaten) **ohne Session und ohne Device** — plus
`MFX_IMPLCAPS_DEVICE_ID_EXTENDED` → `mfxExtendedDeviceId` mit LUID (API ≥ 2.6) für das Adapter-Matching.
Das deckt auch iGPU+Arc-Systeme (zwei Intel-Adapter) korrekt ab; Fallback bei fehlender LUID-Auskunft
(Runtime-API < 2.6): Match über `VendorID`/`DeviceID` gegen `AdapterInfo::device_id` — eindeutig für
iGPU+Arc (unterschiedliche DeviceIDs), mehrdeutig nur bei identischen Intel-Doppeladaptern; bei
Mehrdeutigkeit `probed=false` mit ehrlicher Provenance (nie raten, Support-Grenze siehe D2).

Struktur (Muster aus der AMF-Spec übernehmen, hier die Intel-Ausprägung):

- **Purer Parser:** `ParseVplEncoderDescription(desc_view, target_luid) -> AdapterEncoderCapability` als
  pure Funktion über eine schmale, testbare Sicht auf die Implementation-Description (h264/hevc/av1-Flags,
  10-bit pro Codec, ICQ-Verfügbarkeit) — unit-testbar mit synthetischen Descriptions, kein Intel nötig.
- **Impurer Rand:** die eigentliche Enumeration (MFXLoad/MFXEnumImplementations/MFXUnload) in
  `adapter_capability.cpp` als Intel-Zweig von `ProbeAdapterEncoderCapability`; `backend_label = "Quick Sync"`,
  Provenance „probed via oneVPL implementation description". Sichtbare Schreibweise „Quick Sync (QSV)"
  konsistent mit der bestehenden Device-Tab-Zeile; alle sichtbaren Strings über den CodecLabels-Kanon.
- **Systemweite Facts:** `IntelRuntimeFacts` (Spiegel von `NvidiaRuntimeFacts`: `vpl_dispatcher_ok`,
  `runtime_api_version`, `probed`, per-Codec-Flags inkl. 10-bit, `failure_detail`) additiv in
  `RuntimeCapabilitySnapshot` (`runtime_snapshot.h:107-113`); Downgrade-Regeln in `capability_builder.cpp`
  hängen sich in die durch die AMF-Spec generalisierte Form ein („Codec X selectable ⇔ irgendein gewiretes
  Backend auf diesem System kann X").
- **Ehrlichkeitsregel bleibt — und bestimmt die Schritt-Reihenfolge:** Der Device-Tab rendert das Ergebnis
  von `ProbeAdapterEncoderCapability` DIREKT (`DevicePage.cpp:422-434`: startScan → per-Adapter-Probe →
  applyScanResults → renderCapabilityMatrix) — es gibt kein UI-seitiges Gate. Ebenso macht das Einhängen
  der Intel-Facts in die Downgrade-Regeln Codecs systemweit selectable. Deshalb landen der Intel-Zweig in
  `adapter_capability.cpp` UND der `capability_builder`-Konsum erst NACH dem gewireten Backend (S6, nach
  S5) — S4 liefert nur den puren Parser, die systemweiten Facts (erhoben + geloggt, von keiner
  Downgrade-Regel konsumiert) und den Cache-Key. Kein Zwischenzustand, der Support anzeigt oder freischaltet,
  den keine Aufnahme einlösen kann (`product-spec.md:50-51`).
- **Cache:** `CapabilityCacheKey` (Single-LUID, `capability_cache_key.h:26-33`) wird um die Identität des
  Intel-Adapters erweitert (mehrere `adapter_luid`/`driver_version`-Paare oder ein Hash über alle
  Encode-relevanten Adapter). Pre-1.0-Policy: `kCapabilityCacheSchemaVersion` bumpen, Cache resettet sich —
  keine Migration.

### D7 — Rate-Control- und Preset-Mapping (kanonisches Modell, ADR 0009)

Pures, testbares Mapping `ComputeQsvRcParams` (Spiegel von `ComputeNvencRcParams`):

| Kanonisch | QSV | Details |
|---|---|---|
| ConstantQuality | `MFX_RATECONTROL_ICQ`, `ICQQuality = cq` | Gleiche 1–51-Skala wie NVENC-CQ → Direktübernahme des Werts; wenn `MFXVideoENCODE_Query` ICQ für den Codec ablehnt (z. B. AV1 auf einer Runtime ohne ICQ), deterministischer Fallback auf `MFX_RATECONTROL_CQP` mit `QPI=QPP=cq` und Log-Fakt |
| VariableBitrate | `MFX_RATECONTROL_VBR` | `TargetKbps = bitrate`, `MaxKbps` in Parität zur NVENC-Belegung aus `ComputeNvencRcParams` (bei Umsetzung aus dem Code übernehmen) |
| ConstantBitrate | `MFX_RATECONTROL_CBR` | `TargetKbps = MaxKbps = bitrate` |
| Lossless | nicht angeboten | Parität zu NVENC (`RateControlMode::Lossless` ist überall NotImplemented) |

- **Preset:** kanonisches P1..P7 → `TargetUsage` 7..1 (P1 = schnellstes = TU7 „speed", P7 = TU1 „quality",
  P4 → TU4 balanced). Das UI-Label „NVENC encoder preset" (`product-spec.md:84,244`) muss backend-neutral
  werden („Encoder preset") — das ist ein 0.11-Deliverable; QSV konsumiert nur das kanonische Feld.
- **LowPower/VDEnc:** nicht exponieren; `MFX_CODINGOPTION_UNKNOWN` (Runtime wählt). Auf Arc sind
  HEVC/AV1 ohnehin VDEnc-only; eine User-Option wäre ein Scheinregler.
- **GOP/Keyframes:** `GopPicSize = ComputeGopLength(...)` (bestehende pure Funktion wiederverwenden),
  `GopRefDist = 1` (**keine B-Frames** — Paketmodell hat kein DTS, `packet_types.h:8-12`; B-Frames für QSV
  frühestens zusammen mit der M-2-/DTS-Arbeit). Ziel: **jedes** GOP-I ist ein IDR/Key-Frame — die
  `IdrInterval`-Belegung dafür ist in oneVPL **codec-abhängig** und darf NICHT von NVENCs uniformem
  `idrPeriod == gopLength` (`ApplyGopToNvenc`, `nvenc_encoder.h:118-120`) kopiert werden:
  - **H.264:** `IdrInterval = 0` (VPL-Semantik: jedes I ist IDR).
  - **HEVC:** `IdrInterval = 1` (VPL-Semantik: bei 0 ist NUR das erste I ein IDR — die stille Falle, die
    Segment-Splits bräche).
  - **AV1:** kein IDR-Begriff; die Key-Frame-Kadenz kommt aus `GopPicSize`. Die tatsächliche
    `IdrInterval`-Wirkung ist runtime-abhängig → per Bitstream-Check absichern (jede GOP-Grenze ist ein
    KEY_FRAME-OBU, Hardware-Verify Check 2).
  Der pure GOP-Mapper (S2) kodiert diese Divergenz explizit pro Codec und wird pro Codec getestet. Dazu
  `mfxExtCodingOption2::RepeatPPS = ON`, damit jeder Keyframe self-contained ist (Segment-Split-Anforderung;
  jeder Split-Anfang = IDR + Parameter-Sets, `IVideoEncoder.h:51-55`).
  `RequestKeyframe()` → beim nächsten Submit `mfxEncodeCtrl.FrameType = MFX_FRAMETYPE_IDR | I | REF` (One-Shot,
  exakt die NVENC-Semantik aus `IVideoEncoder.h:51-55`).
- **Sync-Betrieb:** `AsyncDepth = 1`, `EncodeFrameAsync` + sofortiges `SyncOperation` mit Timeout-Budget —
  spiegelt NVENCs synchronen Betrieb und die Bounded-Flush-Drain-Policy (`nvenc_encoder.h:36-42`): ein
  hängendes Device darf den Stop nie wedgen (Zeitbudget → Abbruch → Finalize trotzdem). Flush = `EncodeFrameAsync(nullptr)`
  bis `MFX_ERR_MORE_DATA`, unter demselben Budget.

### D8 — Farbmetadaten / HDR-Parität

Parität heißt: eine QSV-Aufnahme muss unter `ffprobe` dieselben Farb-Fakten zeigen wie die NVENC-Referenz
(color_range/matrix/primaries/transfer aus dem **Bitstream**, nicht nur dem Container — die Lektion aus dem
Color-Range-Bug, `nvenc_encoder.h:44-57`).

- **VUI/color_config:** pures `ApplyColorMetadataToQsv(ColorMetadata) -> mfxExtVideoSignalInfo`-Werte
  (H.264/HEVC; für AV1 gilt dieselbe ExtBuffer laut VPL-API — bei der Umsetzung gegen eine echte Arc-Datei
  per `ffprobe` verifizieren, siehe Verify-Plan; weicht die Runtime ab, ist der AV1-Farbweg ein Blocker für
  die AV1-Freischaltung, nicht für QSV insgesamt).
- **HDR10-Static-Metadata:** statt NVENCs Payload-Arrays nutzt QSV die typed ExtBuffers
  `mfxExtMasteringDisplayColourVolume` + `mfxExtContentLightLevelInfo` mit
  `InsertPayloadToggle = MFX_PAYLOAD_IDR` (Emission auf jedem IDR — Parität zur NVENC-Regel „auf jedem
  Keyframe", `KNOWN_LIMITATIONS.md:112-120`). Die puren Payload-Builder in `hdr_bitstream_metadata.h`
  bleiben NVENC-spezifisch (die Runtime baut die SEI/OBU-Bytes hier selbst); der Paritäts-Beweis läuft über
  die Output-Datei, nicht über Byte-Vergleich der Payloads. MaxCLL/MaxFALL: nur setzen, wenn vorhanden
  (heute: absent — Parität).
- **10-bit:** P010-Pfad identisch zum NVENC-Fall (VideoProcessor liefert P010; `mfxFrameInfo.FourCC = P010`,
  `BitDepthLuma/Chroma = 10`, `Shift = 1`). Capability-gegated pro Codec aus der Probe (HEVC Main10 ab Gen12,
  AV1-10-bit auf Arc/MTL).
- **HDR-Gating unverändert:** H.264+HDR10 bleibt der bestehende Pre-Flight-Blocker; `hdr10_native`-Map
  (`capability_set.h:52-56`) wird pro Backend korrekt gefüllt.

### D9 — Fallback-Verhalten / `EncoderSelectionPolicy`

Die Policy selbst wird in 0.11 eingeführt (pure Funktion); QSV liefert ihre Intel-Zeile:

1. Kandidaten = Backends, die (a) gewired, (b) auf dem **Session-Adapter** (D3) verfügbar und (c) den
   aufgelösten Codec laut Probe können.
2. Präferenzordnung bei mehreren Kandidaten auf demselben Adapter: **Hardware vor Software**; unter
   Hardware-Backends ist die Frage auf einem Adapter trivial (ein Adapter hat genau einen Vendor —
   NVENC vs. QSV konkurrieren nie direkt). Effektiv: NVENC auf NVIDIA-Adapter, AMF auf AMD, QSV auf Intel,
   x264/SVT-AV1 überall als letzte Stufe.
3. **Kein stiller Codec-Wechsel:** Kann kein Backend auf dem Session-Adapter den konfigurierten Codec,
   bleibt das Verhalten das heutige — Diagnostics-Blocker mit einem Fix (Recommended-Codec via
   `BestAvailableVideoCodec`, jetzt multi-backend-korrekt), niemals heimlich ein anderer Codec
   (`product-spec.md:435-436`-Muster generalisiert).
4. Der Selektionsgrund (Backend + warum) landet als ein Log-Fakt beim Session-Start und im Device-Tab
   („Active encoder"-Badge, backend-bewusst statt „erster NVIDIA-Adapter", `DevicePage.h:107-111`).

### D10 — Diagnostics-Provider (ruhig, 1 Fix pro Problem)

- **`EncoderDiagnosticsAdapter`-Zeile für QSV** (ADR 0006): typed Mapping der relevanten `mfxStatus`-Werte
  auf Fehlertexte (`MFX_ERR_DEVICE_FAILED`/`_DEVICE_LOST` → Geräteverlust-Pfad analog DXGI-DEVICE_REMOVED,
  `MFX_ERR_UNSUPPORTED`/`MFX_ERR_INVALID_VIDEO_PARAM` → Konfig-Fehler mit Parameterkontext,
  `MFX_WRN_INCOMPATIBLE_VIDEO_PARAM` nach `Query` → geloggter Fakt, kein Alarm). Pure Tabelle, unit-testbar.
- **Neue Karten nur für echte, messbare Probleme:**
  - Blocker (bestehendes Muster): konfigurierter Codec hat kein nutzbares Backend auf dem Session-Adapter →
    ein Fix (Codec-Wechsel via Recommended-Codec-Resolver).
  - Notice: Intel-Adapter vorhanden, oneVPL-Enumeration liefert nur eine zu alte Runtime (< 2.2) → ein Fix
    („Intel-Grafiktreiber aktualisieren", External-FixAction mit Link).
- **Explizit KEINE Karte:** „QSV wäre auf diesem System theoretisch da, wird aber wegen dGPU-Display nicht
  benutzt" — kein gemessenes Problem, gehört als Provenance-Satz in den Device-Tab (D3).

---

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz. Reihenfolge ist verbindlich; S1–S4 ändern
kein Nutzerverhalten und zeigen nichts an. Die Ehrlichkeitsregel (D6) erzwingt die Reihenfolge hart:
Probe-Konsum (adapter_capability/capability_builder/Device-Tab) erst in S6, NACH dem gewireten Backend (S5).

**S1 — Vendoring: libvpl-Dispatcher + Build-Gate.**
`third_party/CMakeLists.txt`: FetchContent `libvpl` (MIT, GIT_TAG gepinnt), statisches Target, Lizenz-Staging
`licenses/libvpl.txt`; `THIRD_PARTY_NOTICES.md` ergänzen. `libs/recorder_core/CMakeLists.txt` +
`libs/capability/CMakeLists.txt`: Compile-Gate `EXOSNAP_HAVE_ONEVPL` (Muster: NVENC-`__has_include`-Gate,
`runtime_query.cpp:44-50`) — ohne die Dependency baut alles weiter. *Test:* CI-Build beider Konfigurationen
(Gate an/aus); Lizenzdatei im Install-Tree (Packaging-Smoke deckt das ab).

**S2 — Pure QSV-Mapping-Schicht + Tests.**
Neu `libs/recorder_core/src/qsv_encoder.h/.cpp` (nur der pure Teil): `ComputeQsvRcParams` (D7),
`QsvTargetUsageForPreset`, `ApplyColorMetadataToQsv` (D8), `QsvFrameTypeForForcedKeyframe`, GOP-Belegung auf
Basis des bestehenden `ComputeGopLength` **inkl. der per-Codec-`IdrInterval`-Divergenz aus D7 (H.264=0,
HEVC=1, AV1 via GopPicSize)**, `mfxStatus`-Namens-/Diagnose-Tabelle (D10). Alles headless, kein
Dispatcher-Aufruf. *Test:* gtest-Suite `test_qsv_mapping.cpp` gegen handverifizierte Erwartungswerte
(CI-fähig, Muster: die bestehenden NVENC-Pure-Helper-Tests); GOP/IDR-Fälle explizit **pro Codec**
(H.264 vs. HEVC vs. AV1), nicht nur ein generischer Belegungs-Test.

**S3 — `QsvVideoEncoder` (IVideoEncoder-Implementierung).**
Neu `libs/recorder_core/src/qsv_video_encoder.h/.cpp`: `Open` = MFXLoad + Implementation-Filter (Hardware,
D3D11, API ≥ 2.2, LUID des übergebenen `ID3D11Device`) + `MFXCreateSession` + `MFXVideoCORE_SetHandle(MFX_HANDLE_D3D11_DEVICE, …)`.
**Pflichtschritt beim Device-Sharing:** vor `SetHandle` auf dem Immediate Context
`ID3D11Multithread::SetMultithreadProtected(TRUE)` aktivieren — oneVPL setzt das für geteilte D3D11-Devices
voraus (die Runtime darf das Device aus eigenen Threads anfassen); das heute single-threaded genutzte
VideoThread-Device (`video_thread.cpp:230-243`) hat diese Protection nicht an. Nur im QSV-`Open` setzen
(NVENC-Pfad unverändert), der minimale Lock-Overhead ist der Preis des Sharings;
`Configure` = `mfxVideoParam` aus S2-Mappern + `MFXVideoENCODE_Query`-Validierung + `Init`; `RegisterSlotTexture`
merkt sich die Slot-Texturen; `EncodeFrame` = `GetSurfaceForEncode` → `CopyResource` → `EncodeFrameAsync` →
`SyncOperation` (Budget) → `EncodedVideoPacket` (Annex-B/OBU, `keyframe` aus `mfxBitstream::FrameType`);
`Flush` = Drain bis `MFX_ERR_MORE_DATA` unter Zeitbudget; `RequestKeyframe` per `mfxEncodeCtrl` (D7);
`Destroy` idempotent. *Test:* CI-fähig nur der Konstruktions-/Fehlerpfad (Open auf Nicht-Intel-System liefert
sauberen Fehlertext statt Crash — läuft headless). Encode-Roundtrip als gtest mit `GTEST_SKIP()` ohne
Intel-Hardware; auf Intel-Hardware: 120-Frame-Synthetik-Encode, Assertions auf Keyframe-Kadenz, monotone PTS,
nicht-leere Pakete.

**S4 — Capability-Grundlagen: `IntelRuntimeFacts` + purer Parser + Cache-Key (noch NICHT konsumiert).**
`runtime_snapshot.h`: `IntelRuntimeFacts` (D6) additiv in `RuntimeCapabilitySnapshot`. `runtime_query.cpp`:
systemweite oneVPL-Probe (Enumeration, keine Session) — Facts werden erhoben, serialisiert und geloggt,
aber **von keiner Downgrade-Regel und keinem UI konsumiert**. Purer
`ParseVplEncoderDescription` + LUID-/DeviceID-Match als testbare freie Funktionen (D6).
`capability_cache_key.h`: Key um Intel-Adapter-Identität erweitern, `kCapabilityCacheSchemaVersion` bumpen
(Cache-Reset, keine Migration). `CapabilityCacheStore` (app/settings) serialisiert die neuen Facts.
**Bewusst NICHT in S4** (Ehrlichkeitsregel, D6): der Intel-Zweig in
`adapter_capability.cpp::ProbeAdapterEncoderCapability` und das Einhängen in die `capability_builder`-
Downgrades — beides würde im Fenster vor S5 echte Codec-Chips im Device-Tab rendern
(`DevicePage.cpp:422-434` rendert die Probe direkt) bzw. Codecs freischalten, die kein gewiretes Backend
einlösen kann. Beides landet in S6, nach dem Wiring. *Test:* Parser-Tests mit synthetischen Descriptions
(multi-Adapter, iGPU+Arc, LUID-Mismatch, DeviceID-Fallback inkl. Mehrdeutigkeit, alte Runtime),
Cache-Key-Tests, Serialisierungs-Roundtrip.

**S5 — Wiring: Factory- und Policy-Zweig (keine Device-Erzeugungs-Änderung).**
`VideoEncoderFactory` (landet in 0.11, software-encoding-spec D2/S2) bekommt den `Qsv`-Zweig;
`EncoderSelectionPolicy` die Same-Adapter-Regel (D3/D9) als pure Funktion über `AdapterVendor` des
Session-Adapters + Probe-Facts + Codec. Die Device-Erzeugung im VideoThread bleibt UNVERÄNDERT
(`video_thread.cpp:234-242`: Monitor pinnt den Adapter, WGC = Default-Adapter) — kein `encode_adapter_luid`,
keine LUID-Steuerung (D3; Follow-up mit F1). Selektionsgrund als Log-Fakt. *Test:* Policy-Unit-Tests
(alle Vendor-×-Target-Fälle), bestehende NVENC-Pfad-Regression (Default-Config verhält sich byte-identisch:
NVIDIA-Session-Adapter ⇒ NVENC).

**S6 — QSV wird sichtbar UND wählbar: Capability-Konsum + Device-Tab in einem Zug.**
Erst jetzt (Backend seit S5 gewired) erscheinen Probe und Nutzbarkeit gemeinsam:
(a) `adapter_capability.cpp`: Intel-Zweig via `ParseVplEncoderDescription` (S4) + LUID-/DeviceID-Match,
Provenance „probed via oneVPL implementation description"; `EncoderBackendLabelForVendor` liefert
„Quick Sync" für Intel. (b) `capability_builder.cpp`: Einhängen in die (per AMF-Spec generalisierte)
Downgrade-Logik — Regel-A-Form „kein gewiretes Backend kann Codec X" (`capability_builder.cpp:117-145`
heute). (c) `DevicePage.cpp`: Intel-Karten zeigen die echte Probe-Matrix (Chips + Provenance); QSV-Zeile aus
`kBackends` (`DevicePage.cpp:344-347`) entfernen; „Active encoder"-Badge backend-bewusst machen
(`DevicePage.h:107-111`: statt „erster probter NVIDIA-Adapter" → „Adapter, dessen Backend die Policy für die
aktuelle Session-Konfiguration wählen würde"); Hybrid-Provenance-Satz (D3). Alle sichtbaren Strings über
CodecLabels-Kanon. (a)–(c) dürfen als gestackte PRs landen, aber in genau dieser Reihenfolge und innerhalb
derselben Welle. *Test:* `test_runtime_merge`-Erweiterung für die neuen Downgrade-Fälle (NVENC fehlt + QSV
kann HEVC ⇒ HEVC selectable; beides fehlt ⇒ NotImplemented); Widget-Tests via `setAdaptersForTest`
(Intel probed/unprobed/Runtime-zu-alt, iGPU+Arc, Badge-Zuordnung); `--visual-test`-Render des
Device-Surfaces.

**S7 — Diagnostics: Adapter + Karten.**
`EncoderDiagnosticsAdapter`-Mapping (S2-Tabelle) in die Fehlerpfad-Texte (`RecordingErrorDetailText.h`-Kanon);
Blocker-Text `capability_builder`/Readiness generalisiert („No supported hardware encoder detected on this
system" statt NVIDIA-Wortlaut — Wortlaut-Update auch in `docs/product-spec.md:435-436`); neue Notice
„Intel-Treiber zu alt" mit External-FixAction (D10). *Test:* Unit-Tests der Mapping-Tabelle;
Diagnostics-Widget-Test mit injizierten Facts.

**S8 — HDR/10-bit-Parität auf QSV.**
P010-Configure-Pfad (D8), `mfxExtVideoSignalInfo` (inkl. AV1-Verifikation), `mfxExtMasteringDisplayColourVolume`/
`mfxExtContentLightLevelInfo` mit `InsertPayloadToggle=MFX_PAYLOAD_IDR`. HDR10+QSV startet als
`ValidUnvalidated`, bis der Live-Check gelaufen ist. *Test:* CI-fähig sind nur die Mapping-Belegungen;
der Beweis ist der Hardware-Verify (ffprobe-Checks, unten).

**S9 — Doku + Support-Grenze.**
`KNOWN_LIMITATIONS.md` (Hardware-Encoding-Abschnitt: Intel-Support-Matrix, Gen9–11-Ausschluss,
Same-Adapter-Regel, LUID-Disambiguierungs-Grenze bei Runtime < 2.6 mit identischen Intel-Doppeladaptern —
D2), `docs/product-spec.md` (Device-Tab-Absatz Z. 47-51: QSV nicht mehr „planned";
Blocker-Wortlaut), `docs/roadmap.md`-Haken, ADR-Ergänzung: kurzes ADR „oneVPL-Integration" (Dispatcher
statisch, VPL-2.x-only, Same-Adapter-Regel, kein VPP) als Nachbar von ADR 0006. Hardware-Test-Matrix als
`docs/`-kuratierte Checkliste (siehe Verify-Plan). *Test:* Doku-Review; `test_codec_labels`-Erweiterung für
neue sichtbare Strings.

---

## Test-/Verify-Plan

### CI-fähig (headless, jede PR)

- **Pure Mapper (S2):** RC-Mapping inkl. ICQ→CQP-Fallback, TargetUsage, GOP/IDR-Belegung **pro Codec**
  (H.264 `IdrInterval=0`, HEVC `IdrInterval=1`, AV1 GopPicSize-Kadenz — D7-Divergenz explizit), VUI-Werte,
  ForcedKeyframe-FrameType, mfxStatus-Tabelle.
- **Probe-Parser (S4):** synthetische Implementation-Descriptions (kein Intel nötig): Codec-/10-bit-Flags,
  LUID-Match, DeviceID-Fallback (< 2.6) inkl. Mehrdeutigkeits-Degradierung, Runtime-zu-alt.
- **Capability-Merge (S6):** Downgrade-Matrix über {NVENC ±, QSV ±} × Codec; Cache-Key-Invalidierung (S4).
- **Policy (S5):** Vendor×Target×Codec-Fälle; Default-Regression (heutiges NVENC-Verhalten unverändert).
- **UI (S6/S7):** DevicePage-Widget-Tests via Test-Seam; Diagnostics-Karten mit injizierten Facts;
  `--visual-test`-Renders.
- **Graceful-Degrade:** kompletter Testlauf auf CI ohne Intel-GPU — Enumeration leer, `Open` scheitert mit
  sauberem Text, keine Karte behauptet QSV-Support. (Voller Build + ctest, nicht nur `--target exosnap`.)

### Nur mit Intel-Hardware (Entwickler-/Community-Matrix, vor Release-Freischaltung)

Die Matrix ist das 0.13-Release-Gate für die Stufe `ValidUnvalidated → Available`; bis dahin shippt QSV als
`ValidUnvalidated` mit ehrlichem KNOWN_LIMITATIONS-Absatz (Muster: HEVC/10-bit in 0.7,
`KNOWN_LIMITATIONS.md:54-59`).

| Hardware-Klasse | Pflicht-Checks |
|---|---|
| Gen12-iGPU (Tiger/Alder/Raptor Lake) | H.264 8-bit, HEVC 8/10-bit; kein AV1 behauptet |
| Arc (DG2) dGPU | + AV1 8/10-bit, HDR10 (HEVC + AV1) |
| Meteor/Arrow-Lake-iGPU | + AV1; Hybrid-Fälle unten |
| Gen9–Gen11 (z. B. Skylake/Ice Lake) | Negativ-Check: ehrliche „Runtime zu alt"-Anzeige, kein Codec-Chip |

Pro Zelle, automatisierbar per Skript auf dem Zielgerät (ffprobe-basiert, Muster: die 0.7-HDR-Verifies):

1. 60-s-Aufnahme je Codec/Container-Kombination laut Resolver → `ffprobe`: Codec, Profil, Pixelformat
   (yuv420p/yuv420p10le), `color_range/matrix/primaries/transfer` == NVENC-Referenzwerte.
2. Split-Session (Zeit-Split) → jedes Segment beginnt mit IDR + Parameter-Sets (Segment unabhängig abspielbar).
3. HDR10-Session (Arc): MDCV/CLL in-band auf jedem IDR (`ffprobe -show_frames` / Bento4) + Container-Metadata.
4. Stop-unter-Last / Device-Verlust (Treiber-Reset via `wdreset` o. ä.): Flush-Budget greift, Datei finalisiert,
   Fehlerphase korrekt klassifiziert.
5. Perf-Fakt: p99-Encode-Tick auf 4K60 (M-1-Messinfra) — Entscheidungsgrundlage für den Import-Follow-up (D4).

### Nur User-live (auf den Maschinen des Projekts nicht erzwingbar)

- **Hybrid-Laptop-Fälle:** (a) internes Panel an iGPU → Monitor-Aufnahme via QSV; (b) externes Display an
  dGPU → NVENC + korrekter Device-Tab-Provenance-Satz; (c) Fenster-Aufnahme eines dGPU-gerenderten Spiels,
  wenn der OS-Default-Adapter die iGPU ist (D3: keine Steuerung in v1) → Intel-Session-Device,
  WGC-Cross-Adapter-Kopie durch das OS, inkl. Preview-Fallback-Verhalten.
- **iGPU+Arc-Doppel-Intel-System:** korrektes LUID-Matching beider Karten.
- Diese Checks gehören auf die bestehende Live-Verify-Liste (0.9-RELEASE-GATE-Muster) für das 0.13-Gate.
- Ehrlich benannt: Ohne Zugriff auf mindestens eine Gen12+-iGPU und eine Arc-Karte kann das Projekt die
  Matrix nicht selbst schließen — dann bleibt QSV als `ValidUnvalidated` gekennzeichnet ausgeliefert oder
  das Release verschiebt sich (Produktentscheidung F3).

---

## Risiken

1. **Runtime-Fragmentierung:** Intel-Treiberqualität variiert stark über Gens; ICQ-/AV1-/ExtBuffer-Support
   ist runtime-abhängig. Gegenmittel: `Query`-Validierung vor `Init` mit deterministischen Fallbacks (D7),
   `ValidUnvalidated`-Stufe bis zur Matrix, Fehlertexte mit `mfxStatus`-Klartext.
2. **AV1-Farbsignalisierung über `mfxExtVideoSignalInfo`** ist der am wenigsten abgesicherte Punkt (D8);
   Fallback-Plan: AV1 auf QSV erst freischalten, wenn der ffprobe-Farbcheck steht — HEVC/H.264 blockiert das
   nicht.
3. **Policy-Fehlklassifikation des Session-Adapters (S5):** die Backend-Wahl hängt an korrekten
   Probe-Facts für den tatsächlichen Session-Adapter; die Device-Erzeugung selbst bleibt in v1 unangetastet
   (die ursprünglich erwogene WGC-LUID-Steuerung ist auf den F1-Follow-up verschoben — sie war die
   riskanteste Änderung der Welle und für QSV v1 unnötig, D3). Gegenmittel: Default-Regressionstests
   (NVIDIA-Session-Adapter ⇒ NVENC, byte-identisches Verhalten), Policy-Unit-Tests über alle
   Vendor×Target-Fälle.
4. **Koordination mit den Parallel-Specs:** Factory-/Facts-Formen können bei der AMF-/Software-Umsetzung
   anders landen als hier angenommen. Gegenmittel: diese Spec definiert nur die Intel-Ausprägung + Annahmen;
   verbindlich ist der gelandete Code (Kopfzeilen-Hinweis).
5. **CopyResource-Kosten auf iGPU-Bandbreite** (D4) könnten bei 4K60 sichtbar werden. Gegenmittel: Messpunkt
   in der Hardware-Matrix (Check 5) + abgegrenzter Import-Follow-up.
6. **Capability-Cache-Erweiterung** (S4) berührt den Warm-Start-Pfad. Gegenmittel: Schema-Bump + Reset
   (pre-1.0, keine Migration), Cache gated ohnehin nie die echte Probe (`capability_cache_key.h:22-25`).

---

## Offene Fragen (echte Produktentscheidungen)

1. **Device-Tab-Kopplung (F1):** Bleibt die Backend-/Adapter-Wahl reine Auto-Policy (D9) mit dem Device-Tab
   als Inspektion, oder wird die Adapter-Auswahl dort zur **persistierten Expert-Wahl** („encode on this
   adapter"), die die Policy übersteuert? `DevicePage.h:46-49` nennt die Kopplung explizit als offenen
   Follow-up-Slice; diese Spec baut die Auto-Policy und lässt die Übersteuerung bewusst weg — braucht 0.13
   sie schon? Zum selben Follow-up gehört die gezielte WGC-Adapter-Steuerung (`encode_adapter_luid` in
   `RecorderConfig` + Device-Erzeugung per LUID im WGC-Zweig), die aus dem v1-Umfang genommen wurde (D3/S5):
   ohne persistierte Expert-Wahl gibt es keinen Anlass, die Device-Erzeugung anzufassen.
2. **Gen9–Gen11-Ausschluss (F2):** Bestätigung der Support-Grenze „VPL-2.x-Runtime only" (D2) — Skylake- bis
   Ice-Lake-iGPUs bekommen keinen QSV-Pfad, nur Software/NVENC. Sichtbar in KNOWN_LIMITATIONS; Alternative
   (Legacy-MSDK-Allocator-Pfad) wäre ein eigener, teurer Slice.
3. **Release-Gate-Härte (F3):** Darf 0.13 mit `ValidUnvalidated`-QSV shippen, wenn die Hardware-Matrix mangels
   Geräten unvollständig ist (Präzedenz: HEVC/10-bit in 0.7), oder ist die vollständige Matrix — und damit
   ggf. Hardware-Beschaffung (eine Arc-Karte, ein Gen12+-Laptop) — Ship-Blocker?

---

## Adversarialer Review — Ergebnis

Sieben Einwände eines adversarialen Reviewers, jeder gegen Code/Docs verifiziert:

1. **S4 verletzt die Ehrlichkeitsregel (major) — EINGEARBEITET.** Bestätigt: `DevicePage` rendert
   `ProbeAdapterEncoderCapability` direkt (`DevicePage.cpp:422-434`), ein Intel-Zweig in S4 wäre im
   S4→S5-Fenster sichtbar; dasselbe gilt (vom Reviewer nicht genannt, aber folgerichtig) für das
   `capability_builder`-Einhängen, das Codecs ohne gewirtes Backend freischalten würde. S4/S6
   umstrukturiert: S4 = nur Parser + Facts + Cache-Key (unkonsumiert); adapter_capability-Zweig +
   Downgrade-Konsum + Device-Tab zusammen in S6, nach dem Wiring (S5). D6 und Schritt-Intro angepasst.
2. **oneVPL-IdrInterval ist codec-abhängig (major) — EINGEARBEITET.** Bestätigt gegen die VPL-API-Doku
   (AVC: 0 = jedes I ist IDR; HEVC: 0 = nur das erste, 1 = jedes) und gegen `ApplyGopToNvenc`
   (`nvenc_encoder.h:118-120`, uniform idrPeriod==gopLength). D7 kodiert die Divergenz jetzt explizit
   (H.264=0, HEVC=1, AV1 via GopPicSize + Bitstream-Check), S2 und Test-Plan fordern per-Codec-Tests.
3. **„Gebrochene Referenz auf software-encoding-spec" (major) — ZURÜCKGEWIESEN.** Faktisch falsch:
   `.workspace/plans/software-encoding-spec.md` existiert (führt `VideoEncoderFactory`/
   `EncoderSelectionPolicy` in D2/S2 ein); dass beides im Code fehlt, stellt diese Spec selbst fest
   (Ist-Zustand) — das ist die Roadmap-Reihenfolge 0.11→0.13, kein Fehler. Ownership ist in der AMF-Spec
   geklärt (D0/S0: „Voraussetzung, eigene Welle 0.11"). Zur Robustheit ein klärender Ownership-Satz im
   Kopf ergänzt.
4. **WGC-Device-Steuerung = MVP-Expansion (major) — EINGEARBEITET.** Bestätigt: reine Intel-Systeme
   brauchen keine Steuerung (Default-Adapter = einziger Intel-Adapter), Monitor-Targets pinnen ohnehin,
   und die Steuerung wäre die riskanteste Änderung (Default-Pfad `video_thread.cpp:239-242`) für einen
   Edge-Case, den F1 bewusst zurückstellt. `encode_adapter_luid` + LUID-Device-Erzeugung aus v1 entfernt
   (D3/S5/Risiko 3), als Follow-up in F1 gebündelt.
5. **D4-Option-2 widersprüchlich zu GetNativeHandle (minor) — EINGEARBEITET.** Bestätigt: das
   `CopyResource`-Ziel kommt nur über `GetNativeHandle`, das liegt auf dem Hot-Path (pro acquired
   Surface, cachebar). D4-Text korrigiert.
6. **`ID3D11Multithread::SetMultithreadProtected` fehlt (minor) — EINGEARBEITET.** Bestätigt: dokumentierte
   oneVPL-Voraussetzung für per `SetHandle` geteilte D3D11-Devices; das VideoThread-Device
   (`video_thread.cpp:230-243`) aktiviert sie heute nicht. Als Pflichtschritt in S3 ergänzt
   (nur im QSV-`Open`, NVENC-Pfad unverändert).
7. **2.2-Baseline vs. 2.6-LUID-Match (minor) — EINGEARBEITET (mit Korrektur).** Teilrichtig: die
   Behauptung „iGPU+Arc auf < 2.6 ⇒ beide probed=false" ist überzogen — der DeviceID-Fallback (D6)
   disambiguiert iGPU+Arc (unterschiedliche DeviceIDs). Echt scheitert nur der Fall identischer
   Intel-Doppeladapter auf < 2.6. Genau so als Support-Grenze in D2/D6/S9 (KNOWN_LIMITATIONS) benannt.

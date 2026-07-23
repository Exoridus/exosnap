# AMD-AMF-Hardware-Encoder (Roadmap 0.12)

**Status:** Spec, umsetzungsreif (revidiert nach adversarialem Review) · **Ziel-Release:** 0.12.0
· **Autor:** Spec-Welle 2026-07-11
**Referenzen:** ADR 0006 (native Vendor-SDKs **und** Factory-/Encoder-Kontrakt:
`VideoEncoderFactory`/`CapabilityProbe`/`EncoderSelectionPolicy`/`EncoderDiagnosticsAdapter`),
ADR 0007 (Software-Encoding via x264 — die 0.11-Voraussetzung), ADR 0008 (Container/Encoder-
Entkopplung), ADR 0009 (kanonisches Rate-Control-Modell), ADR 0010 (Compat-Registry), ADR 0011
(Capability-Schema), ADR 0032/0033 (Color-Foundation / Diagnostics-Engine), `docs/roadmap.md`
(0.12-Zeile), `.workspace/plans/spec-wave-2026-07-11-plan.md` (Thema 5),
`.workspace/plans/software-encoding-spec.md` (0.11 — liefert das Backend-Modell, siehe D0).

---

## Problem

ExoSnap kann heute ausschließlich über NVIDIA NVENC encodieren. Auf jedem System ohne
NVIDIA-GPU ist Aufnahme blockiert — ehrlich blockiert (Blocker statt stiller Fallback,
`KNOWN_LIMITATIONS.md:32-40`), aber blockiert. Die Roadmap verspricht für 0.12 den nativen
AMD-AMF-Pfad: kein FFmpeg-Umweg (Architektur-Guardrail, ADR 0006), direkte
D3D11-Surface-Übergabe in die bestehende Zero-Copy-Pipeline, präzise Capability-Erkennung,
native Forced-Keyframes/Rate-Control/HDR-Signalisierung und vendor-spezifische, typisierte
Fehler.

Der Device-Tab zeigt AMD-Adapter bereits heute an (DXGI-Enumeration kennt den Vendor), aber mit
der ehrlichen Auskunft „encoder backend not yet supported" und einer statischen „AMD · AMF —
Planned"-Roadmap-Zeile. Diese Zeilen sollen real werden: echte Probe, echte Matrix, echter
Encoder dahinter.

Konkret liefert diese Spec:

1. SDK-Wahl und Vendoring (AMF-Header, Runtime-Discovery).
2. `AmfVideoEncoder` hinter `IVideoEncoder` mit Zero-Copy-D3D11-Integration.
3. Capability-Probe (Codecs, 10-Bit, AQ/Pre-Analysis) für CapabilitySet **und** Device-Tab.
4. Diagnostics-Provider (AMF-Fehlerklassen, Runtime-Versions-Check, Self-Test).
5. Farbmetadaten-/HDR10-Parität mit dem NVENC-Pfad aus derselben `ColorMetadata`-Quelle.
6. Fallback-/Blocker-Verhalten und die Hardware-Test-Matrix.
7. Explizite Benennung der Voraussetzungen aus der software-encoding-spec (0.11).

---

## Ist-Zustand (main @ #192, alle Fakten frisch erhoben)

### Encoder-Pfad: Interface existiert, Factory nicht

- `libs/recorder_core/include/recorder_core/interfaces/IVideoEncoder.h:20-57` — das
  plattformneutrale Interface: `Open(void* gpu_context)` (ID3D11Device*),
  `Configure(w,h,fps)`, `RegisterSlotTexture(slot_idx, GpuTextureHandle)`, `SlotCount()`,
  `AcquireFreeSlot()`, `EncodeFrame(slot, pts_ns, …) → EncodedVideoPacket`, `Flush()`,
  `RequestKeyframe()`, `Destroy()`. `GpuTextureHandle` ist ein `ID3D11Texture2D*` als `void*`
  (`IVideoEncoder.h:15-18`).
- **Es gibt keine `VideoEncoderFactory`.** `video_thread.cpp:550` instanziiert
  `NvencVideoEncoder` direkt auf dem Stack und ruft die NVENC-spezifischen Setter
  (`SetCodec/SetBitDepth/SetChroma/SetCq/SetRateControl/SetPreset/SetKeyframeIntervalSecs/
  SetColor`, `video_thread.cpp:552-570`) vor `Open()`/`Configure()`.
- `EncodedVideoPacket` ist encoder-agnostisch: `bytes / pts_ns / keyframe`
  (`libs/recorder_core/include/recorder_core/packet_types.h:8-12`). Der Keyframe-Flag wird auf
  dem NVENC-Pfad **vorhergesagt** (deterministische GOP-Phase, `NextGopKeyframePhase`,
  `nvenc_encoder.h:139-151`), nicht aus dem Bitstream gelesen.
- Der Mux-Pfad ist encoder-agnostisch: CodecPrivate wird aus dem Bitstream abgeleitet
  (AV1-OBU-Parse `codec_private.h:18-19`, SPS-Parse `annexb_to_avcc.cpp`/`annexb_to_hvcc.cpp`).
  Voraussetzung an jeden neuen Encoder: H.264/HEVC als Annex-B, AV1 als Low-Overhead-OBUs —
  exakt was NVENC heute liefert.

### D3D11-Geräte- und Slot-Modell (Zero-Copy)

- Das Session-Device wird pro Aufnahme erzeugt: bei Monitor-Capture adapter-matched auf den
  Adapter, der den HMONITOR besitzt (`FindAdapterForMonitor`, `video_thread.cpp:210-216`,
  `D3D11CreateDevice` mit `DRIVER_TYPE_UNKNOWN` `:236-238`), sonst Default-Hardware-Adapter
  (`:240-242`). **Konsequenz heute:** Auf einem System, dessen Capture-Adapter kein NVIDIA ist
  (z. B. AMD-APU-Laptop), schlägt `NVENC open` fehl — es gibt keine Cross-Adapter-Kopie.
- Encode-Ring: 8 Slots (`kSlotCount = 8`, `video_thread.cpp:606`), Texturen im Format
  NV12 (8-Bit 4:2:0) / P010 (10-Bit) / AYUV (8-Bit 4:4:4) (`video_thread.cpp:594-622`,
  `:685-708`), gefüllt per VideoProcessorBlt bzw. Compute-Shader, dann via
  `RegisterSlotTexture` an NVENC registriert. `NvencVideoEncoder::SlotCount()` liefert fest 8
  (`nvenc_video_encoder.h:66-68`).
- Alle Encoder-Aufrufe passieren exklusiv auf dem VideoThread (Threading-Kontrakt,
  `nvenc_encoder.h:3-5`).
- Flush-Drain ist über eine pure, getestete Policy begrenzt (`FlushDrainStep` /
  `NextFlushDrainStep`, `nvenc_encoder.h:36-42`) — kein Join-Timeout-Wedge bei totem Device.

### Konfigurationsmodell: NVENC-gefärbt bis in die Enums

- `recorder_core::VideoCodec` heißt `Av1Nvenc / H264Nvenc / HevcNvenc`
  (`libs/recorder_core/include/recorder_core/codec_types.h:13-17`) — Codec-Identität und
  Backend sind verschmolzen. Ebenso `capability::VideoCodec` (gespiegelt, `libs/capability/
  include/capability/config_types.h`).
- `RecorderConfig` trägt `nvenc_cq` (kanonische CQ-Skala 1-51), `nvenc_rate_control`
  (kanonisches `RateControlMode`), `nvenc_preset` (P1-P7), `nvenc_bitrate_kbps`
  (`libs/recorder_core/include/recorder_core/recorder_session.h:289-306`).
- `RateControlMode` ist bereits kanonisch (ADR 0009): ConstantQuality / VariableBitrate /
  ConstantBitrate / Lossless (`codec_types.h:110-115`), mit dokumentiertem AMD-Mapping in der
  Roadmap („AMF CQP", `docs/roadmap.md:59-64`).
- Statische Format-Policy lebt seit #190 im Resolver: `CodecSupports10Bit` /
  `CodecSupportsChroma444` sind `constexpr` auf den `*Nvenc`-Enum-Werten
  (`libs/capability/include/capability/resolver.h:88-95`), `ReconcileOutputFormat` ist pur
  (`resolver.h:126`). Die Engine-Übersetzung pflegt eine explizite Combo-Allow-List, ebenfalls
  auf `*Nvenc`-Werten (`libs/capability/src/translation.cpp:39-96`).
- `CodecSupportsHdr10Native` hardcodet `HevcNvenc/Av1Nvenc`
  (`libs/recorder_core/include/recorder_core/hdr_native.h:39-41`).

### Farbmetadaten / HDR: eine Quelle der Wahrheit, NVENC-seitig konsumiert

- `ColorMetadata` (`libs/recorder_core/include/recorder_core/color_metadata.h:66-106`) ist der
  Single-Source-of-Truth für VideoProcessor-Konversion, Container-Tags **und**
  Bitstream-Signalisierung; CICP-Codepoints, HDR10-Statik (MDCV/CLL) modelliert.
- NVENC-Konsum: `ApplyColorMetadataToNvenc` (pur, `nvenc_encoder.h:57`), HDR10-Assembly aus
  Display-Fakten `MakeHdr10ColorMetadata` (pur, `hdr_native.h:71-99`), per-Keyframe
  SEI-/OBU-Injektion NVENC-intern (`nvenc_encoder.h:353-371`,
  `libs/recorder_core/src/hdr_bitstream_metadata.cpp`).
- Matroska-/MP4-Writer lesen dieselbe `ColorMetadata` — encoder-unabhängig.

### Capability-Modell und Adapter-Enumeration

- `CapabilitySet` (single-resolved, `libs/capability/include/capability/capability_set.h:29-91`)
  treibt Settings/Diagnostics/Record; `probed==true` nur nach echtem Hardware-Query;
  `RecordingCoordinator::OnCapabilitiesReady` verweigert unprobte Sets
  (`app/services/RecordingCoordinator.cpp:489-503`).
- NVIDIA-Fakten: `NvidiaRuntimeFacts` (`libs/capability/include/capability/
  runtime_snapshot.h:57-81`), Probe `ProbeNvencCodecs` (`libs/capability/src/
  runtime_query.cpp:153`, aufgerufen `:569`), pure Refinements `ApplyNvencCodecSupport` /
  `ApplyNvencYuv444Support` (`libs/capability/include/capability/capability_builder.h:47-54`).
- Multi-Adapter (additiv, seit dem Device-Tab-Slice): `EnumerateAdapters()`
  (`libs/capability/include/capability/adapter_enum.h:64`), `ClassifyVendor` kennt AMD
  (0x1002/0x1022, `adapter_enum.h:40-41`). `ProbeAdapterEncoderCapability` probt nur NVIDIA;
  AMD/Intel/Other liefern bewusst `probed=false` mit `kNotYetSupportedMessage`
  (`libs/capability/src/adapter_capability.cpp:45-46`, `:201-210`).
  `EncoderBackendLabelForVendor` liefert für AMD den Leerstring (`:197-199`).
  `AdapterEncoderCapability` hat `h264/hevc/av1` + NVENC-spezifische `yuv444_*`-Flags
  (`adapter_capability.h:13-38`).
- Device-Tab: statische Roadmap-Liste mit „AMD · AMF … Planned"
  (`app/pages/DevicePage.cpp:337-348`, Planned-Tag `:376-380`), „Backend planned"-Badge für
  jeden nicht-aktiven Adapter (`:613-623`), Banner „…switching the encode device is planned"
  (`:509-514`). Produktspez fixiert die ehrlichen Planned-Rows (`docs/product-spec.md:47-51`).

### Diagnostics und Fehlerklassen

- Engine-Fehler tragen `ErrorPhase` (Prepare/VideoEncode/…, `libs/recorder_core/include/
  recorder_core/error_types.h:7-17`) plus Freitext-Detail; die UI mappt NVENC-Details auf
  Nutzer-Meldungen über String-Matching: „NVENC open" → „Encoder unavailable", „NVENC
  AV1/NV12" → „Codec unsupported", „NVENC init/encode/register" → „Encoder error"
  (`app/diagnostics/error_message.cpp:74-88`).
- `SelfTestRunner::CheckEncoderAvailability` = `LoadLibraryW(L"nvEncodeAPI64.dll")`
  (`app/diagnostics/SelfTestRunner.cpp:90-104`).
- `FixAction`-Modell mit Safety-Klassen Auto/Assisted/External
  (`app/diagnostics/DiagnosticResult.h:37-49`); Codec-Empfehlung läuft über den puren Resolver
  `BestAvailableVideoCodec` (`libs/capability/include/capability/codec_selection.h:32-33`).
- Sichtbare Codec-Labels sind backend-neutral kanonisiert (`VisibleVideoCodecLabel`,
  `codec_selection.h:39`; Qt-Seite delegiert). **Aber:** `RecordingCoordinator` hardcodet
  Status-Strings `L"AV1 NVENC"` etc. (`app/services/RecordingCoordinator.cpp:262-268`,
  `:1861-1868`).

### Vendoring-Muster

- NVENC: `third_party/nvidia/nvEncodeAPI.h` ist eingecheckt; Konsumenten binden das
  Include-Verzeichnis ein (`libs/capability/CMakeLists.txt:21-24`,
  `libs/recorder_core/CMakeLists.txt:1-5, 344`) und degradieren headless über
  `__has_include(<nvEncodeAPI.h>)` (`adapter_capability.cpp:34-40`).
- FetchContent-Deps: Supply-Chain-Regel aus dem Review (M-13) — neue Deps per Commit-SHA
  pinnen; Lizenztext über `_exosnap_install_license` in den Install-Tree stagen
  (`third_party/CMakeLists.txt:8-15`).

---

## Design

### D0 — Voraussetzungen aus der software-encoding-spec (0.11), explizit

Diese Spec **setzt voraus**, dass die 0.11-Welle (`.workspace/plans/software-encoding-spec.md`,
parallel in dieser Spec-Welle verfasst) folgende Enabler liefert. Sie sind hier als harte
Schnittstellen-Anforderungen benannt, damit beide Specs nicht driften; wo die
software-encoding-spec andere Namen wählt, gelten deren Namen:

1. **`VideoEncoderFactory` + `VideoEncoderBackend`-Enum in `recorder_core`.** Die direkte
   Instanziierung in `video_thread.cpp:550` ist ersetzt durch
   `CreateVideoEncoder(const RecorderConfig&, std::string& out_error) →
   std::unique_ptr<IVideoEncoder>` (software-encoding-spec D2); die NVENC-spezifischen Setter
   sind hinter die Factory gezogen (der VideoThread kennt nur noch `IVideoEncoder`).
   `VideoEncoderBackend { Nvenc, X264, SvtAv1 }` (`codec_types.h`, software-encoding-spec D1)
   — diese Spec fügt `Amf` hinzu; ebenso erhält `capability::EncoderBackendChoice`
   den Wert `Amf`.
2. **Codec-/Backend-Entkopplung.** `VideoCodec` ist backend-neutral umbenannt
   (`Av1 / H264 / Hevc`), in `recorder_core` und `capability` synchron; `RecorderConfig` und
   `UserRecorderConfig` tragen den Backend getrennt. Pre-1.0: harter Rename ohne Migration,
   Presets mit altem Schema werden resettet (feedback_prerelease_breaking_changes).
   Die statischen Gates (`CodecSupports10Bit`, `CodecSupportsChroma444`,
   `CodecSupportsHdr10Native`, `translation.cpp`-Allow-List, `ContainerCompatRegistry`) sind
   auf die neutralen Werte umgestellt — Container×Codec-Regeln sind backend-frei, Backend-
   spezifische Fähigkeiten (444, 10-Bit-per-GPU) wandern in capability-gated Abfragen.
3. **Generalisierte Encoder-Parameter.** Die kanonische Qualitätsskala (CQ 1-51,
   `RateControlMode`, Bitrate, Keyframe-Intervall in Sekunden) liegt in backend-neutralen
   `RecorderConfig`-Feldern; `NvencPreset` bleibt als NVENC-eigenes Expert-Feld bestehen
   (ADR 0039). Per-Backend-Spezifika leben in einem per-Backend-Block oder klar benannten
   Feldern — kein `if (backend)`-Geflecht in der UI.
4. **CapabilitySet kennt Backends pro Codec — es gibt KEIN `active_backend`-Feld.** Die
   software-encoding-spec (D5) liefert:
   `CapabilitySet::video_codec_backends` (Map `VideoCodec → VideoEncoderBackend →
   SupportAnnotation`; die flache `video_codecs`-Map wird zur Ableitung „beste Annotation
   über alle Backends"), die **einzige** pure Auflösungsquelle
   `ResolveEncoderBackend(caps, codec, choice)` (Auto = prefer hardware when selectable),
   `EncoderBackendChoice` in der User-Config, `RecorderConfig::video_encoder_backend`
   (immer konkret, nie Auto) und backend-spezifische Runtime-Fakten. Diese Spec formuliert
   D2/D5/D6 **gegen dieses per-Codec-Modell**; der Begriff „aktiver Hardware-Backend" ist
   darin eine reine Ableitung (pure Helper, D2), kein Set-Feld. Der GPU-lose
   CI-Encode-Smoke (x264/WARP) existiert.

**Reihenfolge ist verbindlich:** AMF baut auf 1-4 auf. Sollte 0.11 verschoben werden, ist der
Factory-Teil (1-3) als eigenständiger Enabler-Slice vorzuziehen — AMF ohne Factory würde die
NVENC-Verdrahtung duplizieren und H-6-artige Policy-Duplikate wieder einführen.

### D1 — SDK-Wahl und Vendoring

**Optionen:**

| Option | Bewertung |
|---|---|
| **A: AMF SDK nativ** (github.com/GPUOpen-LibrariesAndSDKs/AMF, MIT; Header-only-API, Runtime `amfrt64.dll` kommt mit dem Radeon-Treiber) | Direkte D3D11-Surfaces (`CreateSurfaceFromDX11Native`), präzise Caps (`AMFCaps` pro Komponente), native Forced-IDR/RC/HDR-Properties, typisierte `AMF_RESULT`-Fehler. Kein Link-Time-Artefakt: Header kompilieren, DLL zur Laufzeit laden — exakt das NVENC-Muster (`LoadLibraryW` + Function-List). |
| B: FFmpeg `h264_amf`/`hevc_amf`/`av1_amf` | Von ADR 0006 explizit verworfen: grobe Capability-Verhandlung, unpräzise Forced-Keyframes, HDR-Metadaten wrapper-abhängig, opake `AVERROR`s, zusätzliche Synchronisation am D3D11-Handoff. Kein erneutes Abwägen nötig — der Guardrail steht. |
| C: Media Foundation HMFT (AMD-H.264/HEVC-MFT) | ADR 0006/0038: MF ist transitional, wird ersetzt statt erweitert; kein AV1, keine feinen RC-/HDR-Kontrollen, Delay-Load-/Windows-N-Sonderfälle. Verworfen. |

**Entscheidung: A.** Konkret:

- **FetchContent, SHA-gepinnt** auf ein AMF-Release-Tag (bei Umsetzung: neuestes stabiles
  Release wählen, Stand Kenntnis v1.4.36; den Commit-SHA des Tags pinnen — M-13-Regel, nie
  rolling). Nur der Header-Baum `amf/public/include` wird eingebunden; nichts wird gebaut.
  MIT-Lizenztext via `_exosnap_install_license` stagen. Kein Einchecken der ~50 Header ins
  Repo (anders als der einzelne nvEncodeAPI.h) — FetchContent hält das Repo sauber und das
  Update auditierbar.
- **Runtime-Discovery zur Laufzeit:** `LoadLibraryW(AMF_DLL_NAMEA…)` → `amfrt64.dll`,
  `AMFQueryVersion` + `AMFInit(AMF_FULL_VERSION, &factory)`. Eine Konstante
  `kMinAmfRuntimeVersion` (bei Umsetzung gegen die gepinnte SDK-Version festlegen; Startpunkt:
  die Version, die die verwendeten AV1-/HDR-Properties eingeführt hat) gate-t den Encoder: zu
  alte Runtime ⇒ Backend gilt als nicht verfügbar, mit ehrlicher Provenance („AMF runtime
  x.y.z too old, need ≥ a.b.c — update the Radeon driver").
- **Headless-Degrade wie NVENC:** `__has_include(<AMF/core/Factory.h>)`-Guard analog
  `EXOSNAP_ADAPTER_CAPABILITY_HAVE_NVENC` (`adapter_capability.cpp:34-40`), damit ein Build
  ohne FetchContent-Netzzugriff weiterhin kompiliert und ehrlich `probed=false` liefert.
  (Da FetchContent den Header normal immer bereitstellt, ist der Guard vor allem für
  Offline-/Teil-Builds und die Probe-Tools relevant — Konsistenz mit dem NVENC-Muster.)

### D2 — Backend-/Adapter-Auswahl (EncoderSelectionPolicy, Stufe 1)

Das eigentliche Design-Problem: Welcher Backend ist „aktiv", und was passiert auf
Mischsystemen?

**Optionen:**

- **A (gewählt): Ein aktiver Hardware-Vendor pro System, Präferenz NVIDIA > AMD —
  ausgedrückt im per-Codec-Modell der 0.11-Spec (D0-4), nicht als neues Set-Feld.**
  `BuildFromHardwareQuery` probt beide Vendors; **befüllt werden die Hardware-Spalten von
  `video_codec_backends` aber nur für einen Vendor**: die NVENC-Spalten, wenn ein
  probe-fähiger NVIDIA-Adapter existiert, sonst die AMF-Spalten, wenn ein probe-fähiger
  AMD-Adapter existiert; die Spalten des jeweils anderen Hardware-Backends bleiben
  `NotImplemented` mit ehrlicher Reason („another hardware encoder is active (NVIDIA
  preferred); switching the encode device is planned"). `ResolveEncoderBackend`s
  Auto-Pfad („prefer hardware when selectable") löst dadurch **ohne jede Änderung** auf
  den einen befüllten Hardware-Backend auf. Für Labels, SelfTest, Blocker-Texte und
  Device-Tab-Badges kommt ein abgeleiteter purer Helfer
  `ActiveHardwareBackend(const CapabilitySet&) → std::optional<VideoEncoderBackend>`
  hinzu (der Hardware-Backend mit mindestens einer selectable Spalte) — eine Ableitung
  aus `video_codec_backends`, keine zweite Wahrheit. Das
  single-resolved-`CapabilitySet`-Modell (`capability_set.h:29-44`) bleibt unverändert in
  seiner Rolle.

  **Session-Vendor-Mismatch — Pre-Flight-Blocker mit explizit ergänztem Schritt:** Stimmt
  der Vendor des Session-D3D11-Device (Monitor-Adapter!) nicht mit
  `ActiveHardwareBackend` überein und hat der Session-Vendor keinen wired Backend, gibt es
  einen **typisierten Blocker** statt des heutigen kryptischen „NVENC open"-Fehlers. Die
  dafür nötige Monitor→Adapter→Vendor-Auflösung existiert heute **nur** engine-seitig
  (`FindAdapterForMonitor`, `video_thread.cpp:210-216`) — die Pre-Flight-Schicht braucht
  sie neu (fehlender Schritt, jetzt Teil von S2/S7):
  - `ProbeDisplays` (`runtime_query.cpp:306-345`) iteriert bereits Adapter→Outputs; die
    Display-Facts werden um Vendor + LUID des besitzenden Adapters erweitert (Schwester-
    Felder zu `DisplayHdrFacts`, gleiche Probe-Schleife, kein neuer DXGI-Walk).
  - Der `RecordingCoordinator` übersetzt das Monitor-Target in seinen Device-Namen
    (`GetMonitorInfoW` — exakt das bestehende impure-Schritt-Muster,
    `runtime_snapshot.h:41-42`) und macht den Vendor-Vergleich als pure Lookup-Funktion
    testbar.
  - Schlägt die Auflösung fehl (Facts stale, Monitor zwischenzeitlich abgesteckt), feuert
    **kein** falscher Blocker — der typisierte Session-Start-Fehler (D8/D9) bleibt der
    Backstop. Der Blocker ist die bessere Vorwarnung, nicht die einzige Verteidigung.
- B: Backend pro Session nach Capture-Adapter, mit Re-Validierung bei jedem Targetwechsel.
  Ehrlichste Semantik für Multi-Vendor-Multi-Monitor, aber: Settings müssten pro
  Capture-Target andere Codec-Mengen anbieten (AV1 auf NVENC-Monitor, kein AV1 auf
  RDNA2-Monitor), der Preset-/Resolver-Fluss würde target-abhängig — ein erheblicher
  UI-/Modell-Umbau für einen seltenen Systemtyp. Verschoben auf die Encode-Device-Switch-Stufe
  (der Device-Tab-Banner verspricht sie bereits als „planned", `DevicePage.cpp:509-514`).
- C: Nutzerwählbarer Encode-Adapter im Device-Tab ab 0.12. Setzt B voraus; gleiche Gründe.

**Konsequenzen von A, ausgeschrieben:**

- Reines AMD-System (Desktop-Radeon, AMD-APU-Laptop): `ActiveHardwareBackend` = AMF, alles
  funktioniert — die Zielgruppe des Releases.
- Reines NVIDIA-System: byte-identisches Verhalten zu heute.
- NVIDIA+AMD gemischt, Capture auf dem NVIDIA-Adapter: NVENC wie heute.
- NVIDIA+AMD gemischt, Capture auf einem AMD-getriebenen Monitor: heute „NVENC open failed
  0x…", künftig Blocker mit Klartext („Display N wird von der AMD-GPU getrieben; der aktive
  Encoder läuft auf der NVIDIA-GPU. Aufnahme dieses Displays wird mit dem Encode-Device-Switch
  einer späteren Version möglich.") — kein Verhaltensverlust, bessere Erklärung. Das ist eine
  dokumentierte Boundary (KNOWN_LIMITATIONS), keine Regression.

### D3 — `AmfVideoEncoder`: Zero-Copy-Session-Pipeline

Neue Dateien `libs/recorder_core/src/amf_encoder.{h,cpp}` (SDK-Wrapper, Struktur-Spiegel von
`nvenc_encoder.{h,cpp}`) und `amf_video_encoder.{h,cpp}` (`IVideoEncoder`-Adapter, Spiegel von
`nvenc_video_encoder.{h,cpp}`). Alle AMF-Aufrufe exklusiv auf dem VideoThread (gleicher
Threading-Kontrakt wie NVENC).

- **Open:** DLL laden, `AMFInit`, Versions-Gate, `AMFContext::InitDX11(device)` mit dem vom
  VideoThread gereichten `gpu_context`. Komponente per Codec:
  `AMFVideoEncoderVCE_AVC` / `AMFVideoEncoder_HEVC` / `AMFVideoEncoder_AV1`
  (Makro-Namen beim Vendoring gegen die gepinnte SDK-Version verifizieren).
- **Configure:** Property-Setup (D5) + `component->Init(format, width, height)`;
  `format` = `AMF_SURFACE_NV12` (8-Bit) / `AMF_SURFACE_P010` (10-Bit). **Kein 4:4:4:** AMF hat
  keinen YUV444-Encode-Pfad; `Cs444` wird upstream capability-gated (D6) und erreicht diesen
  Encoder nie — `Configure` weist es dennoch defensiv ab (out_error), analog zur
  NVENC-Selbstabsicherung in `translation.cpp:32-37`.
- **RegisterSlotTexture:** die 8 vom VideoThread erzeugten Ring-Texturen werden einmalig via
  `AMFContext::CreateSurfaceFromDX11Native(texture, observer)` als AMFSurfaces gewrappt —
  **zero-copy**, keine `AllocSurface`+`CopyResource`-Zwischenkopie. `SlotCount()` bleibt 8;
  das Ring-/Geometrie-Setup in `video_thread.cpp` ändert sich nicht.
- **Slot-Lebenszyklus:** ein Slot gilt als frei, wenn AMF die gewrappte Surface freigegeben hat
  (`AMFSurfaceObserver::OnSurfaceDataRelease` setzt ein per-Slot-Flag; der Observer wird von
  AMF auf dem Submissions-Thread gerufen — Flag als `std::atomic<bool>` genügt, kein Lock).
  Das ist strenger als nötig, aber beweisbar korrekt gegen Use-after-Write; die 8er-Ring-Tiefe
  deckt die AMF-interne Pipeline-Tiefe (typisch ≤ 4).
- **EncodeFrame:** `surface->SetPts(pts_ns / 100)` (AMF rechnet in 100-ns-Ticks; Rückweg
  `GetPts() * 100`), one-shot Forced-IDR-Property auf der Surface, wenn `RequestKeyframe()`
  armiert war (`AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE = IDR` bzw. HEVC-/AV1-Äquivalent), dann
  `SubmitInput(surface)` und ein nicht-blockierender `QueryOutput`:
  - `AMF_OK` + Buffer ⇒ Packet füllen (bytes, pts, keyframe), zugehörigen Slot freigeben.
  - `AMF_REPEAT` ⇒ `true` + leeres Packet („buffered") — deckungsgleich mit dem
    Interface-Kontrakt (`IVideoEncoder.h:42-45`).
  - `AMF_INPUT_FULL` bei Submit ⇒ erst Output drainieren, dann einmal erneut submitten; wenn
    weiter voll: fataler Fehler mit `AmfResultName` (dieselbe Ehrlichkeit wie NVENC statt
    Endlosschleife).
- **Keyframe-Flag: echt statt vorhergesagt.** Der Output-Buffer trägt den Frame-Typ als
  Property (`AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE == IDR`, HEVC/AV1-Äquivalente inkl.
  AV1 `KEY`). Der AMF-Pfad liest ihn und braucht **keine** GOP-Phasen-Vorhersage — das
  entkoppelt ihn zugleich von dem Umbau, den die nvenc-async-Spec (M-1) an
  `NextGopKeyframePhase` vornimmt.
- **HDR-Metadaten auf Keyframes:** anders als NVENC (explizite per-Frame-SEI-Payloads) bekommt
  AMF die HDR10-Statik einmalig als Property (D7); die Wiederholung pro Keyframe erledigt der
  AMF-Encoder selbst. Verifikationspunkt in der Test-Matrix, kein eigener Code.
- **Flush:** `Drain()` + `QueryOutput`-Schleife bis `AMF_EOF`, begrenzt durch **dieselbe pure
  Drain-Policy-Idee** wie NVENC: eine `NextAmfDrainStep(AMF_RESULT, elapsed_ms, budget_ms)`
  (Consume/Retry/AbortTimeout/AbortError) — pur, getestet, kein Wedge bei totem Device.
- **Fehler:** `AmfResultName(AMF_RESULT)` (Spiegel von `NvencStatusName`) + Fehlerdetails mit
  stabilem Präfix `"AMF open" / "AMF init" / "AMF register" / "AMF encode" / "AMF drain"`,
  damit `error_message.cpp` per Substring mappen kann wie beim NVENC-Pfad (`:74-88`).

### D4 — Parameter-Mapping (pur, testbar, ohne GPU)

Spiegel der NVENC-Helfer, alle als freie Funktionen in `amf_encoder.h` deklariert und ohne
AMF-Session testbar. Da AMF-Properties über `AMFComponent::SetProperty(name, variant)` gesetzt
werden, bauen die puren Funktionen eine **Property-Liste** (`std::vector<AmfProperty>` mit
`{const wchar_t* name, variant}`), die der impure Code nur noch anwendet — dadurch sind die
Mappings byte-genau unit-testbar:

1. `ComputeAmfRcProperties(codec, RateControlMode, cq, bitrate_kbps)` —
   ConstantQuality → `RATE_CONTROL_METHOD_CONSTANT_QP` + QP-I/QP-P;
   VariableBitrate → Peak-Constrained-VBR + `TARGET_BITRATE`/`PEAK_BITRATE`;
   ConstantBitrate → CBR; Lossless bleibt NotImplemented (wie NVENC).
   **CQ-Skalen-Mapping (Konstanten explizit gepinnt):** Die kanonische CQ-Skala ist
   **1**-51 (`kNvencCqMin = 1` / `kNvencCqMax = 51`, `codec_types.h:46-48`) ⇒
   `qp_i = clamp(cq, 1, 51)` — Untergrenze 1, nicht 0. Inter-Frames tragen wie auf dem
   NVENC-Pfad **+2 QP** relativ zu Intra (`ComputeNvencRcParams`,
   `nvenc_encoder.cpp:626-631`): `qp_p = min(qp_i + 2, 51)`. AMF-AV1-QP-Index ist 0-255 ⇒
   `qp_i_av1 = round(qp_i * 255 / 51)`, `qp_p_av1 = round(qp_p * 255 / 51)` (linear;
   dieselbe kanonische Semantik „1 = beste Qualität"). Alle vier Formeln sind byte-genau
   Teil des Funktionstests, inkl. Paritätstest der I/P-Spreizung gegen
   `ComputeNvencRcParams`.
2. `ComputeAmfGopProperties(codec, keyframe_interval_secs, fps_num, fps_den)` — nutzt das
   vorhandene pure `ComputeGopLength` (`nvenc_encoder.h:122`; dazu aus dem nvenc-Header in
   einen encoder-neutralen Helfer verschieben oder duplikatfrei re-exportieren) und mappt auf
   `IDR_PERIOD` (AVC) / `GOP_SIZE`(+`NUM_GOPS_PER_IDR=1`) (HEVC) / `GOP_SIZE` (AV1).
3. `AmfQualityPresetFor(AmfQualityPreset)` — eigenes 3-stufiges Enum
   `AmfQualityPreset { Speed, Balanced, Quality }` (Default Balanced), gemappt auf
   `QUALITY_PRESET_SPEED/BALANCED/QUALITY` je Codec. Bewusst **kein** Mapping des
   NVENC-P1-P7-Reglers: die Semantiken sind nicht deckungsgleich, und ADR 0011 sieht ohnehin
   per-Encoder deklarierte Preset-Mengen vor. Die Expert-UI zeigt das Control nur, wenn das
   für den gewählten Codec aufgelöste Backend AMF ist (`ResolveEncoderBackend`-Ergebnis;
   analog verschwindet der NVENC-Preset-Regler).
4. `ApplyColorMetadataToAmfProperties(codec, ColorMetadata)` — siehe D7.
5. **B-Frames/Lookahead/Pre-Analysis: aus.** Paritätsentscheid mit dem heutigen NVENC-Pfad
   (P-only, kein Lookahead, `nvenc_encoder.h:126-135`): `frameIntervalP`-Äquivalente bleiben
   auf P-only; AMF **Pre-Analysis (PA)** inkl. CAQ (Content-Adaptive Quantization — AMDs
   Spatial-AQ-Gegenstück) wird in 0.12 **nur geprobt, nicht aktiviert**. Begründung: PA fügt
   Latenz + eigene Tuning-Achsen hinzu und gehört in die SSIM/VMAF-Qualitätsmatrix der
   encoder-quality-features-Spec (M-2); ein ungeprüftes Default-On wäre versteckte
   MVP-Expansion. NVENC pinnt Spatial-AQ seit #181 explizit — die Asymmetrie (NVENC AQ an,
   AMF AQ aus) wird in KNOWN_LIMITATIONS ehrlich dokumentiert.
6. **Auflösungs-Constraints aus Caps — das Modell muss dafür erweitert werden:** AMF-AV1 hat
   auf RDNA3 eine 64-Pixel-Alignment-Eigenheit (Encoder padded intern und signalisiert Crop;
   ältere Runtimes lehnen nicht-alignte Größen ab). Das existierende
   `ResolutionConstraint`-Feld (`capability_set.h:68`) kann das **nicht** ausdrücken: es ist
   `{max_width, max_height, must_be_even}` (`config_types.h:20-24`) — global statt per-Codec,
   und nur 2-Pixel-Even-Alignment. Es unverändert zu befüllen würde entweder alle Codecs
   über-constrainen (H.264/HEVC brauchen kein 64er-Alignment) oder die Regel gar nicht
   abbilden. Erweiterung (Teil von S2, pre-1.0 breaking-frei):
   (a) `must_be_even` wird zu `width_alignment` / `height_alignment` generalisiert
   (Default 2 — die bestehende Semantik bleibt byte-identisch, `must_be_even==true` ≙
   Alignment 2), (b) `CapabilitySet` erhält zusätzlich eine per-Codec-Map
   `std::unordered_map<VideoCodec, ResolutionConstraint> codec_resolution_constraints` —
   nur AV1 bekommt das 64er-Alignment, H.264/HEVC bleiben unberührt. Die Probe liest die
   per-Codec Width/Height-Ranges + Alignment aus `AMFCaps`/IO-Caps und befüllt diese Map;
   `ReconcileOutputFormat` konsumiert das codec-spezifische Constraint statt zur
   Session-Laufzeit zu überraschen.

### D5 — Capability-Probe (CapabilitySet + Device-Tab)

Zwei Konsumenten, eine Probe-Technik (Spiegel des NVENC-Musters):

1. **`AmdRuntimeFacts`** (neu in `runtime_snapshot.h`, Spiegel von `NvidiaRuntimeFacts`):
   `amf_dll_present`, `amf_runtime_version` (+ `amf_runtime_version_ok`), `adapter_name`,
   `failure_detail`, `amf_codec_probed`, `amf_h264/hevc/av1`, per-Codec-10-Bit
   (`amf_hevc_10bit`, `amf_av1_10bit`; H.264 bleibt 8-Bit — wie NVENC), `amf_pre_analysis`
   (AQ-Fähigkeit, nur informativ/Device-Tab + spätere Quality-Welle). In
   `RuntimeCapabilitySnapshot` neben `nvidia` aufnehmen; `CapabilityCacheStore` erweitert das
   Cache-Schema (pre-1.0: Schema-Bump, Cache verwerfen statt migrieren).
2. **Probe `ProbeAmfCodecs(AmdRuntimeFacts&)`** in `runtime_query.cpp` (+ adapterspezifische
   Variante in `adapter_capability.cpp` analog `ProbeNvencGuidsOnDevice`): DLL laden,
   `AMFQueryVersion`-Gate, `InitDX11` auf dem LUID-gematchten Device. Achtung:
   `CreateD3D11DeviceForLuid` existiert zwar (`adapter_capability.cpp:55-82`), liegt aber im
   anonymen Namespace **und** innerhalb des
   `#if EXOSNAP_ADAPTER_CAPABILITY_HAVE_NVENC`-Guards (`:34-48`) — in einem Build ohne
   NVENC-Header existiert die Funktion gar nicht, genau dort muss die AMF-Probe aber laufen.
   **Eigener Vorab-Schritt (Teil von S3):** die Funktion aus dem NVENC-Guard heben (sie
   braucht nur d3d11/dxgi, keinen Vendor-Header) und file-intern ungeguarded bzw. als
   internen Shared-Helper beiden Probe-Zweigen zugänglich machen — erst dann
   „wiederverwenden". Danach pro Codec `factory->CreateComponent(...)`; Erfolg + `GetCaps()` ⇒ Codec
   supported, 10-Bit aus den Input-Format-Caps (P010) bzw. Profile-Caps (HEVC Main10),
   Alignment/Ranges für D4-6. Komponenten sofort `Terminate()`+Release. Best-effort: jeder
   Fehlschlag ⇒ `probed=false` + `failure_detail`, nie ein fabrizierter Wert.
3. **Refinement `ApplyAmfCodecSupport(CapabilitySet&, const AmdRuntimeFacts&)`** (pur,
   Spiegel `ApplyNvencCodecSupport`): befüllt/downgraded die **AMF-Spalten von
   `video_codec_backends`** (D0-4) und wird nur angewandt, wenn AMD der präferierte geprobte
   Hardware-Vendor ist (D2-Regel: kein probe-fähiger NVIDIA-Adapter); nicht unterstützte
   Codecs bekommen NotImplemented mit Klartext-Reason („AV1 encode requires
   RDNA3 or newer (probed via AMF component caps)"). Statische AMF-Spalten-Baseline:
   H.264/HEVC ValidUnvalidated, AV1 ValidUnvalidated (Downgrade per Probe), `chroma444`
   **immer** NotImplemented, `hdr10_native` wie gehabt HEVC/AV1 (per 10-Bit-Probe gated).
   Die flache abgeleitete `video_codecs`-Map trägt das Ergebnis automatisch weiter
   (software-encoding-spec D5).
4. **Device-Tab wird real:** `ProbeAdapterEncoderCapability` bekommt einen AMD-Zweig;
   `EncoderBackendLabelForVendor(Amd)` ⇒ `"AMF"`; Provenance „probed via AMF component caps"
   bzw. ehrliche Fehlklassen („AMF runtime not detected (amfrt64.dll not found)", „AMF runtime
   too old (x.y.z, need ≥ a.b.c)", „AMF probe unavailable (component creation failed)").
   `AdapterEncoderCapability` erhält per-Codec-10-Bit-Felder als **`std::optional<bool>`**
   (`hevc_10bit`, `av1_10bit`): die nackten bools des Structs (`adapter_capability.h:28-37`)
   können „geprobt: nein" nicht von „nicht gemessen" trennen, und der struct-weite
   `probed`-Flag deckt neue Felder nicht ab, die der NVENC-Zweig anfangs nicht füllt — ein
   geprobter NVIDIA-Adapter würde sonst fabrizierte Neins anzeigen. `std::nullopt` =
   „nicht erhoben" ⇒ die UI zeigt die Zeile nicht; der NVENC-Pfad darf die Felder später
   ebenfalls füllen. Die statische Roadmap-Liste im DevicePage
   (`kBackends`, `DevicePage.cpp:344-348`) verliert die AMD-Zeile; „Backend planned"-Badge
   (`:613-623`) zeigt für einen geprobten, aber nicht aktiven AMD-Adapter weiterhin ehrlich
   den Nicht-aktiv-Zustand (Text: „Backend available — not active", da „planned" dann lügt).

### D6 — Resolver-/Policy-Integration

- Container×Codec-Regeln (`ContainerCompatRegistry`) sind nach D0-2 backend-frei und ändern
  sich **nicht** — MKV/MP4/WebM-Matrix gilt identisch für AMF-encodete Streams (Annex-B/OBU,
  vom Mux-Pfad bereits bewiesen encoder-agnostisch).
- Backend-abhängige Fähigkeiten laufen ausschließlich über die `CapabilitySet`-Annotationen
  der AMF-Spalten von `video_codec_backends` (plus deren flache Ableitung): `chroma444`
  NotImplemented (AMF), 10-Bit per Probe, AV1 per Probe. `BestAvailableVideoCodec`
  (`codec_selection.h:32`) funktioniert dadurch unverändert — auf RDNA2 empfiehlt es HEVC
  statt AV1, ohne eine Zeile AMD-Code (die backend-bewusste Variante liefert 0.11,
  software-encoding-spec D5).
- `RecordingCoordinator`-Status-Strings (`:262-268`, `:1861-1868`) werden backend-aware über
  ein zentrales Label (`VisibleVideoCodecLabel` + Backend-Label des **für die Session
  aufgelösten** Backends — das `ResolveEncoderBackend`-Ergebnis, das ohnehin als
  `RecorderConfig::video_encoder_backend` gestampt wird; nicht ein globaler
  „aktiver Backend". CodecLabels-Kanon: sichtbare Schreibweise `"AV1 · AMF"` analog
  Device-Tab-Kanon `"AMD · AMF"`; exakte Schreibweise mit `CodecLabels.h` abgleichen,
  eine Quelle).

### D7 — Farbmetadaten / HDR10-Parität

`ColorMetadata` bleibt die eine Quelle der Wahrheit; der AMF-Pfad konsumiert sie an denselben
drei Stellen wie NVENC (VideoProcessor-Konversion und Container-Tags sind ohnehin
encoder-unabhängig; nur die Bitstream-Signalisierung ist neu):

1. **`ApplyColorMetadataToAmfProperties(codec, ColorMetadata)`** (pur): mappt
   primaries/transfer/matrix/range auf die AMF-Output-Color-Properties (AVC/HEVC:
   `FULL_RANGE_COLOR` bzw. HEVC-Nominal-Range + `OUTPUT_COLOR_PRIMARIES` /
   `OUTPUT_TRANSFER_CHARACTERISTIC`-Familie; AV1: die AV1-Color-Properties). CICP-Werte sind
   identisch zu NVENC (`color_metadata.h:22-46`) — die Property-Namen je Codec beim Vendoring
   gegen die gepinnte SDK-Version verifizieren; der Test fixiert die gemappten Zahlenwerte.
2. **`BuildAmfHdrMetadata(const ColorMetadata&) → AMFHDRMetadata`** (pur): füllt
   Mastering-Display-Primaries/Luminanz + MaxCLL/MaxFALL aus denselben Feldern, die der
   Matroska-Writer und `hdr_bitstream_metadata.cpp` lesen. Übergabe als
   `AMF_VIDEO_ENCODER_HEVC_INPUT_HDR_METADATA` / `AMF_VIDEO_ENCODER_AV1_INPUT_HDR_METADATA`;
   der AMF-Encoder emittiert MDCV/CLL-SEI bzw. AV1-Metadata-OBUs selbst. MaxCLL/MaxFALL = 0
   ⇒ Felder absent lassen (Parität mit `MakeHdr10ColorMetadata`, `hdr_native.h:79-80`).
3. **Paritäts-Akzeptanzkriterium (messbar):** Für dieselbe Session-Konfiguration müssen
   NVENC- und AMF-Aufnahme in `ffprobe -show_streams -show_frames` identische
   `color_primaries/color_transfer/color_space/color_range` und (HDR10) identische
   MDCV-/CLL-Side-Data-Werte zeigen; zusätzlich MP4-Remux-Erhalt wie im HEVC/HDR-Slice
   verifiziert. Abweichung = Release-Blocker für den betroffenen Codec (Downgrade auf
   NotImplemented statt „fast richtig" ausliefern).
4. Der HDR-Entscheidungspfad (`IsHdr10NativeEffective`, `MakeHdr10ColorMetadata`,
   H.264-Blocker `rec.hdr.h264`) ist codec-, nicht backend-gebunden und bleibt unangetastet.

### D8 — Diagnostics-Provider (ruhig, 1 Fix pro Problem)

Der ADR-0006-Name `EncoderDiagnosticsAdapter` wird **nicht** als neue Abstraktionsschicht
gebaut (Overengineering-Gefahr); er materialisiert sich als die folgenden konkreten Stücke im
bestehenden Diagnostics-Modell. Das erfüllt die ADR-0006-Anforderung **inhaltlich** — „must
translate vendor error codes … generic AVERROR mapping is insufficient" wird durch typisierte
`AMF_RESULT`-Namen (`AmfResultName`) plus die `error_message.cpp`-Fehlerklassen geleistet —
weicht aber in der **Form** (keine eigene Komponente) vom ADR-Wortlaut ab. Diese Abweichung
wird nicht stillschweigend uminterpretiert, sondern in S9 aktiv ins ADR zurückgeschrieben:

1. **`error_message.cpp`-Einträge** (Spiegel `:74-88`): „AMF open" ⇒ „Encoder unavailable /
   The AMD hardware encoder could not be opened. / Check GPU drivers. AMF requires a supported
   AMD GPU."; „AMF init/encode/register" ⇒ „Encoder error"; Codec-Fehlklasse analog
   „NVENC AV1/NV12". Keine neuen Panik-Texte — dieselbe nüchterne Tonlage.
2. **Pre-Flight-Check „AMF-Runtime veraltet"** (nur auf Systemen mit
   `ActiveHardwareBackend == Amf` und `amf_dll_present && !amf_runtime_version_ok`): eine Notice mit genau einem External-Fix
   (Radeon-Treiber-Update, Deep-Link + gefundene vs. benötigte Version). Kein zweiter Check,
   der dasselbe anders formuliert.
3. **Blocker-Text** bei fehlendem Backend wird vendor-neutral: heute „No working NVENC
   encoder…" — künftig aus dem aktiven/fehlenden Backend abgeleitet („No supported hardware
   encoder detected (NVIDIA NVENC or AMD AMF)…", nach 0.11 inkl. Software-Hinweis).
   D2-Mismatch-Blocker (AMD-Monitor auf NVENC-System) wie in D2 formuliert.
4. **`SelfTestRunner::CheckEncoderAvailability`** (`SelfTestRunner.cpp:90-104`) wird
   backend-aware: prüft die DLL des `ActiveHardwareBackend` (nvEncodeAPI64.dll bzw.
   amfrt64.dll inkl. Versions-Gate) und benennt das Ergebnis entsprechend (0.11 ergänzt
   den Software-Zweig ohnehin, software-encoding-spec D6-5).
5. **Session-Fehler tragen `AmfResultName`** im Detail (typisierte Vendor-Codes statt opaker
   Hex-HRESULTs) — die Logs-/Support-Bundle-Welle (diagnostics-support-bundle-spec) erbt das
   automatisch.

### D9 — Fallback-Verhalten

Produkt-Kanon bleibt: **kein stiller Fallback** (`KNOWN_LIMITATIONS.md:39-40`,
`docs/product-spec.md:435-436`).

- **Auswahlzeit (Pre-Flight):** Nicht unterstützte Kombinationen sind gar nicht wählbar
  (CapabilitySet-Gates); `BestAvailableVideoCodec`-FixActions bieten den nächstbesten Codec
  an (existierende `rec.profile.codec`-Mechanik, unverändert).
- **Session-Start-Fehler** (AMF `Open`/`Init` schlägt trotz grüner Probe fehl — Treiber-Bug,
  Ressourcendruck): typisierter Fehler über die D8-Klassen, Aufnahme startet nicht. Sobald
  0.11 geliefert ist, ergänzt eine FixAction „Mit Software-Encoder (x264) aufnehmen"
  (Auto-Safety, config-only, mit Preview/Confirm) den Fehlerpfad — Backend-Wechsel bleibt
  eine sichtbare Nutzerentscheidung, nie automatisch.
- **Mid-Session-Fehler** (Encode-Fehler während der Aufnahme): identisch zum NVENC-Pfad —
  Session endet über `RecordFailure(ErrorPhase::VideoEncode)`, bereits geschriebene Daten
  bleiben durch die Recovery-Maschinerie nutzbar. Kein Mid-Session-Backend-Swap (bewusst
  nicht gebaut: Encoder-Neustart mit anderem Backend mitten im Segment wäre ein eigener,
  riskanter Slice ohne belegten Bedarf).

### D10 — Bewusst NICHT gebaut (0.12-Abgrenzung)

- Kein Cross-Adapter-Encode (Capture auf GPU A, Encode auf GPU B) und kein
  Encode-Device-Switch im Device-Tab — bleibt „planned" (D2-Optionen B/C).
- Kein 4:2:2/4:4:4 auf AMF (Hardware kann es nicht bzw. nicht sinnvoll).
- Keine B-Frames, kein Lookahead, keine Pre-Analysis/CAQ-Aktivierung (nur Probe; Tuning gehört
  in die Quality-Welle mit SSIM/VMAF-Matrix).
- Keine AMF-spezifischen UI-Erweiterungen über das 3-stufige Quality-Preset hinaus.
- Kein Umbau der Preview-/Compositor-Pfade — die sind encoder-agnostisch und bleiben es.
- Keine Legacy-VCE-Sondertuning-Pfade (pre-VCN-GCN): laufen mit, wenn die Probe sie meldet,
  werden aber nicht gezielt unterstützt (siehe Offene Frage 2).

---

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz; Reihenfolge verbindlich.
Schritt 0 ist die externe Abhängigkeit.

**S0 (Voraussetzung, eigene Welle 0.11):** Factory-Ausbau gemäß D0 aus der
software-encoding-spec — `CreateVideoEncoder`-Factory, `VideoEncoderBackend` +
`EncoderBackendChoice`, Codec-Rename, Parameter-Generalisierung,
`video_codec_backends`-Map + `ResolveEncoderBackend` im Capability-Modell.

**S1 — AMF-SDK-Vendoring + Build-Wiring.**
`third_party/CMakeLists.txt` (FetchContent SHA-gepinnt, MIT-Lizenz-Staging),
Include-Wiring in `libs/recorder_core/CMakeLists.txt` und `libs/capability/CMakeLists.txt`
(Muster der nvidia-Includes `:344` / `:24`), `__has_include`-Guard-Makros
(`EXOSNAP_HAVE_AMF`). Noch kein Funktionscode.
*Test:* CI-Build grün (mit und ohne AMF-Header — Offline-Konfiguration simulieren);
Lizenzdatei landet im Install-Tree (Packaging-Smoke).

**S2 — Capability-Probe + Runtime-Facts.**
`runtime_snapshot.h` (`AmdRuntimeFacts`; Display-Facts um Vendor+LUID des besitzenden
Adapters erweitern — D2-Blocker-Grundlage), `runtime_query.cpp` (`ProbeAmfCodecs`, Aufruf
neben `:569`; Vendor/LUID in `ProbeDisplays` mitschreiben), `config_types.h`/
`capability_set.h` (`ResolutionConstraint`-Erweiterung: `width_alignment`/`height_alignment`
statt `must_be_even` + per-Codec-Map `codec_resolution_constraints`, D4-6),
`capability_builder.{h,cpp}` (`ApplyAmfCodecSupport` auf den AMF-Spalten von
`video_codec_backends`, statische AMF-Spalten-Baseline, Vendor-Präferenz-Regel +
`ActiveHardwareBackend`-Helfer nach D2), `capability_cache_key`/`CapabilityCacheStore.cpp`
(Schema-Bump, Reset statt Migration).
*Test (CI):* pure Refinement-Tests mit synthetischen Facts (Codec-Downgrades, 10-Bit-Gates,
chroma444 immer NotImplemented, Vendor-Präferenz NVIDIA>AMD — der nicht-präferierte Vendor
bekommt NotImplemented-Spalten, Degrade bei `probed=false`); `ActiveHardwareBackend`-Ableitung;
`ResolutionConstraint`-Tests (Default-Alignment 2 verhält sich byte-identisch zu
`must_be_even`, 64er-Alignment greift nur für AV1); Monitor→Vendor-Lookup pur mit
synthetischen Display-Facts; Cache-Schema-Reset-Test. Auf NVIDIA-Dev-Maschine: Verhalten
byte-identisch (kein AMD-Adapter ⇒ Facts leer).

**S3 — Device-Tab wird real.**
`adapter_capability.{h,cpp}` (zuerst `CreateD3D11DeviceForLuid` aus dem
`EXOSNAP_ADAPTER_CAPABILITY_HAVE_NVENC`-Guard und dem NVENC-only-Pfad heben — D5-2, eigener
Refactor-Commit ohne Verhaltensänderung; dann AMD-Probe-Zweig,
`EncoderBackendLabelForVendor(Amd)="AMF"`, Provenance-Klassen, per-Codec-10-Bit-Felder als
`std::optional<bool>`), `app/pages/DevicePage.cpp` (AMD aus `kBackends`
entfernen, Badge-Text „Backend available — not active" für geprobte Nicht-aktiv-Adapter),
`docs/product-spec.md:47-51` aktualisieren.
*Test (CI):* Unit-Tests für Provenance-/Label-Funktionen; Widget-Test mit synthetischen
`AdapterInfo`/`AdapterEncoderCapability` (der bestehende Fake-Adapter-Pfad
`MainWindow.cpp:2916-2936` zeigt das Muster); Visual-Check via `--visual-test`-Harness.
*User-live:* echte AMD-Matrix auf AMD-Hardware.

**S4 — `amf_encoder`-Wrapper: pure Mappings.**
`libs/recorder_core/src/amf_encoder.{h,cpp}`: `AmfResultName`, `AmfProperty`-Liste,
`ComputeAmfRcProperties` (inkl. AV1-QP-255-Skala), `ComputeAmfGopProperties`
(`ComputeGopLength` in encoder-neutralen Helfer heben), `AmfQualityPresetFor`,
`ApplyColorMetadataToAmfProperties`, `BuildAmfHdrMetadata`, `NextAmfDrainStep`.
*Test (CI):* vollständige Unit-Abdeckung aller Mappings ohne GPU (Zahlenwerte fixiert,
Paritäts-Tests gegen die NVENC-Mappings wo semantisch gleich: GOP-Länge, CICP-Werte,
Drain-Policy-Verhalten).

**S5 — `AmfVideoEncoder` : `IVideoEncoder` (Session-Pipeline).**
`amf_video_encoder.{h,cpp}` + Session-Teile von `amf_encoder.cpp` (Open/Init/Surface-Wrap/
Observer-Slot-Ring/EncodeFrame/QueryOutput/Forced-IDR/Flush/Destroy nach D3).
*Test (CI):* Interface-Compile-Test (Spiegel `test_nvenc_video_encoder_interface.cpp`),
Slot-Ring-Zustandslogik als pure Einheit getestet (Observer-Flag-Semantik), Drain-Budget.
*User-live:* erste echte Encodes (S7 verdrahtet sie).

**S6 — Farb-/HDR-Verifikationsgerüst.**
Keine neuen Produktionspfade — S4/S5 enthalten die Mappings; dieser Schritt liefert das
ffprobe-Paritäts-Skript (`scripts/` oder Test-Tool) das NVENC-Referenzwerte gegen
AMF-Aufnahmen diffed (D7-3) und dokumentiert die Sollwerte je Codec/SDR/HDR10.
*Test (CI):* Skript-Selbsttest gegen eingecheckte ffprobe-JSON-Fixtures.
*User-live:* Ausführung auf AMD-Hardware.

**S7 — Factory-Integration + Session-Verdrahtung.**
`CreateVideoEncoder` (aus S0) erhält den `Amf`-Zweig; `video_thread.cpp` bleibt
backend-agnostisch (Fehlertexte „AMF open" etc. kommen aus dem Encoder); D2-Blocker im
`RecordingCoordinator`: Monitor-Target → Device-Name (`GetMonitorInfoW`-Schritt) → purer
Vendor-Lookup gegen die S2-Display-Facts → Vergleich mit `ActiveHardwareBackend(caps)`;
Auflösungs-Fehlschlag ⇒ kein Blocker (D9-Backstop greift). Status-Labels backend-aware über
das aufgelöste Session-Backend (D6, CodecLabels-Kanon).
*Test (CI):* Factory-Auswahl-Tests (Backend×Codec-Matrix inkl. Fehlerfälle), Coordinator-Test
für den Mismatch-Blocker mit synthetischem CapabilitySet + synthetischen Display-Facts
(Match ⇒ kein Blocker, Mismatch ⇒ Blocker, Lookup-Fehlschlag ⇒ kein Blocker).
*User-live:* Aufnahme-Smoke auf AMD-Hardware (jeder Codec × MKV, Split/Forced-Keyframe).

**S8 — Diagnostics-Provider.**
`error_message.cpp` (AMF-Klassen), Pre-Flight-Runtime-Versions-Notice (+FixAction External),
Blocker-Texte vendor-neutral, `SelfTestRunner` backend-aware.
*Test (CI):* error_message-Mapping-Tests (Muster der bestehenden NVENC-Tests in
`app/tests/test_diagnostics.cpp`), Notice erscheint nur bei `amf_dll_present &&
!version_ok`-Facts, genau eine FixAction.

**S9 — Doku + Release-Gate.**
`KNOWN_LIMITATIONS.md` (AMF-Abschnitt: unterstützte Generationen, AQ-Asymmetrie,
Mixed-Vendor-Boundary, ValidUnvalidated-Status), `docs/product-spec.md` (Device-Tab-Absatz,
Blocker-Wortlaut), `docs/roadmap.md` (0.12-Zeile abhaken), **ADR 0006 aktiv aktualisieren
(Pflicht, nicht optional):** (a) die stale Status-Zeile — sie nennt „0.9.0 (AMF), 0.10.0
(QSV)" — auf die realen Roadmap-Versionen korrigieren (0.12 AMF); (b) den
Consequences-Punkt zu `EncoderDiagnosticsAdapter` amendieren: die Anforderung (typisierte
Vendor-Fehlercodes statt generischem Mapping) bleibt bestehen und wird von AMF erfüllt, die
Form ist per Entscheid dieser Spec keine eigene Komponente, sondern
`AmfResultName`/`NvencStatusName` + die `error_message.cpp`-Fehlerklassen (D8) — die
Abweichung wird im ADR festgehalten statt ignoriert. Kein neuer ADR nötig.
`docs/development/release-checklist.md` um die Hardware-Matrix-Gates erweitern.
*Test:* Doc-Review; keine Code-Änderung.

---

## Test-/Verify-Plan

### CI-fähig (ohne AMD-Hardware)

| Bereich | Tests |
|---|---|
| Pure Mappings (S4) | RC-Modi je Codec, AV1-QP-Skala, GOP je Codec, Quality-Preset, CICP-Property-Werte, `AMFHDRMetadata`-Feldparität zu `ColorMetadata`, Drain-Policy (Consume/Retry/AbortTimeout/AbortError) |
| Capability (S2) | Refinements mit synthetischen Facts (auf den `video_codec_backends`-Spalten), Vendor-Präferenz + `ActiveHardwareBackend`-Ableitung, per-Codec-`ResolutionConstraint` (Alignment-Default 2 unverändert, 64 nur AV1), Monitor→Vendor-Lookup, Degrade-Pfade (keine DLL / Version alt / Probe-Fehler ⇒ nie fabrizierte Werte), Cache-Schema-Reset |
| Adapter/Device (S3) | Provenance-Strings, Label-Funktionen, Widget-Test mit Fake-AMD-Adapter, `--visual-test`-Render |
| Interface (S5) | Compile-Zuweisbarkeit, SlotCount, Slot-Ring-Zustandsmaschine |
| Factory/Blocker (S7) | Backend-Auswahl-Matrix, Mismatch-Blocker, Status-Label-Kanon |
| Diagnostics (S8) | error_message-Mappings, Versions-Notice-Gating |
| Build (S1) | mit/ohne AMF-Header, Lizenz-Staging, Packaging-Smoke |
| Regression NVIDIA | volle bestehende Suite — auf der NVIDIA-Dev-Maschine muss jedes Verhalten unverändert sein (`ActiveHardwareBackend` = Nvenc, keine AMF-Probe-Nebenwirkung) |

### Nur User-live / Hardware-Matrix (Release-Gate für 0.12)

ExoSnap hat projektseitig keine AMD-Hardware; ohne mindestens **eine** physisch verifizierte
Zeile der Matrix bleibt AMF `ValidUnvalidated` und `KNOWN_LIMITATIONS` sagt das ausdrücklich
(Vorbild: HEVC/10-Bit-Formulierung `KNOWN_LIMITATIONS.md:56-59`). Matrix (pro Zeile die
Checks A-F):

| Hardware-Klasse | Beispiel | Erwartete Codecs |
|---|---|---|
| RDNA3/RDNA4 dGPU | RX 7800 XT / RX 9070 | H.264, HEVC (8/10-Bit), AV1 (8/10-Bit) |
| RDNA2 dGPU | RX 6700 XT | H.264, HEVC (8/10-Bit), **kein AV1** (Probe muss das ehrlich zeigen) |
| Aktuelle APU (VCN) | Ryzen 7840HS „Phoenix" | wie RDNA3, iGPU-Pfad + D2-Laptop-Szenario |
| Ältere GCN/Polaris (best effort) | RX 580 | H.264 (+HEVC je nach VCE-Gen) — nur Probe-Ehrlichkeit prüfen, kein Support-Versprechen |

Checks pro Zeile:
- **A Probe-Wahrheit:** Device-Tab-Matrix == tatsächlich encodierbare Codecs (Kreuzprobe:
  jede als supported gemeldete Kombination startet; jede nicht gemeldete ist nicht wählbar).
- **B Aufnahme-Smoke:** je Codec × MKV (+ MP4-Remux für H.264/HEVC, WebM für AV1), 60 s,
  ffprobe-valide, abspielbar (VLC + Windows „Filme & TV").
- **C Farb-Parität:** S6-Skript SDR (Limited/Full) + HDR10 (HEVC/AV1 10-Bit) gegen
  NVENC-Referenzwerte; MDCV/CLL-SEI/OBU auf Keyframes vorhanden (D3-Verifikationspunkt).
- **D Split/Forced-Keyframe:** automatischer Split — jedes Segment beginnt mit echtem IDR/Key
  (ffprobe-Frame-Check), Recovery-Manifest-Pfad unverändert.
- **E Fehlerklassen:** Treiber-Reset (per `wdreset`-artiger Provokation oder Abstecken der
  eGPU, wenn verfügbar) endet in typisiertem Fehler, kein Hang (Drain-Budget greift).
- **F Soak:** 30-min-Aufnahme, keine Slot-Leaks (Observer-Semantik), A/V-Sync im Rahmen der
  0.10-Soak-Kriterien.

**Abnahme durch den User/Community:** Da der Entwickler NVIDIA-Hardware nutzt, sind B-F nur
über beschaffte Test-Hardware oder strukturierte Community-Verifikation (Preview-Channel)
möglich — siehe Offene Frage 3. Live-Aufnahmen sind erlaubt, Dateien werden nie committet
(feedback_live_recording_verification_ok).

---

## Risiken

1. **Keine AMD-Hardware im Projekt.** Größtes Risiko: der gesamte Session-Pfad (S5/S7) ist
   bis zur ersten echten Hardware nur typ- und logik-verifiziert. Mitigation: strikte
   Spiegelung des bewiesenen NVENC-Muster-Codes, maximale pure Testfläche (S4), Preview-
   Channel-Rollout mit `ValidUnvalidated`-Kennzeichnung, Release-Gate über die Matrix.
2. **Treiber-/Runtime-Varianz.** `amfrt64.dll`-Version hängt am Radeon-Treiber; ältere
   Treiber fehlen Properties (AV1-Komponente, HDR-Metadata-Property). Mitigation:
   Versions-Gate + per-Property-Fehlertoleranz in der Probe (fehlende Property ⇒ Fähigkeit
   nicht gemeldet, nie Crash), D8-Notice mit Treiber-Update-Fix.
3. **AV1-64-Pixel-Alignment (RDNA3).** Ungewöhnliche Output-Größen könnten `Init` ablehnen
   oder gepaddete Streams mit Crop-Signalisierung erzeugen, die nicht jeder Player ehrt.
   Mitigation: Alignment aus Caps proben und über das **erweiterte, per-Codec**
   `ResolutionConstraint`-Modell (D4-6 — das heutige Feld kann weder 64er-Alignment noch
   per-Codec-Regeln ausdrücken) als Resolver-Constraint durchsetzen statt
   Laufzeit-Überraschung; im Zweifel AV1 für nicht-alignte Größen auf HEVC reconcilen
   (sichtbare Adjustment-Meldung, Resolver-Mechanik existiert).
4. **Surface-Wrap-Lebenszyklus.** Use-after-Release zwischen VideoProcessorBlt und
   AMF-interner Referenz wäre ein Heisenbug. Mitigation: Observer-basierte Slot-Freigabe
   (konservativ), Soak-Check F, 8er-Ring-Puffer.
5. **HDR-SEI-Wiederholungssemantik.** NVENC emittiert MDCV/CLL auf jedem Keyframe (bewusste
   Entscheidung für Apple-Player); ob AMF das identisch tut, entscheidet der Treiber.
   Mitigation: expliziter Matrix-Check C; falls AMF nur einmalig emittiert, ehrliche
   KNOWN_LIMITATIONS-Notiz statt nachgebautem SEI-Injektor (erst bei belegtem Player-Problem).
6. **Qualitäts-Erwartung.** AMD-H.264 ist historisch sichtbar schwächer als NVENC; Nutzer
   könnten „schlechtere Aufnahmen" als Bug melden. Mitigation: ehrliche Doku, HEVC/AV1 als
   empfohlene Codecs auf AMD (via `BestAvailableVideoCodec` ohnehin so), Qualitätsmatrix
   (SSIM/VMAF) am 1.0-Gate.
7. **Spec-Drift zur software-encoding-spec.** Im adversarialen Review aufgelöst: D0/D2/D5/D6
   sind jetzt gegen das tatsächliche 0.11-Modell formuliert (per-Codec
   `video_codec_backends` + `ResolveEncoderBackend`; kein `active_backend`-Feld — das stand
   hier vorher als Paradigmen-Drift). Rest-Risiko: Formänderungen während der
   0.11-**Umsetzung**; bei Abweichung gilt die software-encoding-spec als führend und diese
   Spec wird nachgezogen.
8. **Property-Namen-Nailing.** Diese Spec zitiert AMF-Property-/Makro-Namen aus
   SDK-Kenntnis; einzelne Namen können je SDK-Version abweichen. Jede „beim Vendoring
   verifizieren"-Markierung ist ein Pflicht-Checkpunkt in S1/S4, kein Freibrief.

---

## Offene Fragen (Produktentscheidungen)

1. **Mixed-Vendor-Systeme (NVIDIA + AMD gleichzeitig):** Reicht für 0.12 die D2-Stufe-1-Policy
   (NVENC bleibt aktiv; AMD-getriebene Monitore blocken mit ehrlicher Erklärung), oder soll
   der Encode-Device-Switch im Device-Tab (Stufe B/C) in die 0.12-Welle vorgezogen werden?
   (Spec-Empfehlung: Stufe 1 — der Switch ist ein eigener UI+Modell-Slice.)
2. **Offizielle Support-Untergrenze:** VCN-basierte GPUs/APUs (RDNA-dGPUs, Ryzen-4000+-APUs)
   als offizieller Support, ältere GCN/Polaris nur best-effort („Probe-ehrlich, ungetestet")?
   Bestimmt Test-Matrix-Umfang und KNOWN_LIMITATIONS-Wortlaut.
3. **Release-Gate ohne eigene AMD-Hardware:** 0.12 als `ValidUnvalidated` über den
   Preview-Channel mit Community-Verifikation ausliefern, oder Beschaffung einer
   RDNA3-Testkarte (bzw. Remote-Zugang) als hartes Gate vor dem Stable-Release?
4. **Encoder-Preset-UI auf AMF:** eigenes 3-stufiges Control (Speed/Balanced/Quality — die
   Spec-Empfehlung, D4-3) oder Beibehaltung des P1-P7-Reglers mit dokumentiertem Mapping, um
   die Settings-Oberfläche backend-invariant zu halten?

---

## Adversarialer Review — Ergebnis

Alle acht Einwände wurden gegen Code/Docs verifiziert; alle acht sind berechtigt und
eingearbeitet, keiner zurückgewiesen.

1. **Fundament-Drift `active_backend` (major) — eingearbeitet.** Bestätigt: die
   software-encoding-spec (D5) modelliert Backends per Codec (`video_codec_backends` +
   `ResolveEncoderBackend`), kein `active_backend`-Feld. D0-4, D2, D5-3 und D6 sind auf das
   per-Codec-Modell umgeschrieben; „aktiver Hardware-Backend" ist jetzt der abgeleitete pure
   Helfer `ActiveHardwareBackend(caps)`, die NVIDIA>AMD-Präferenz eine Befüll-Regel der
   Hardware-Spalten. Risk 7 als aufgelöst markiert.
2. **Mismatch-Blocker ohne Monitor→Vendor-Auflösung (major) — eingearbeitet.** Bestätigt:
   die Auflösung existiert nur engine-seitig (`FindAdapterForMonitor`). D2/S2/S7 ergänzen
   den fehlenden Schritt: `ProbeDisplays` schreibt Vendor+LUID des besitzenden Adapters in
   die Display-Facts, der Coordinator löst per `GetMonitorInfoW` + purem Lookup auf;
   Lookup-Fehlschlag ⇒ kein falscher Blocker, der D9-Session-Start-Fehler bleibt Backstop.
3. **`ResolutionConstraint` zu schwach für 64-Pixel/per-Codec (major) — eingearbeitet.**
   Bestätigt: `{max_width, max_height, must_be_even}` kann weder 64er-Alignment noch
   per-Codec-Regeln ausdrücken. D4-6/S2 spezifizieren die Erweiterung:
   `width_alignment`/`height_alignment` (Default 2, byte-identische Alt-Semantik) +
   per-Codec-Map `codec_resolution_constraints` (nur AV1 = 64). Risk-3-Mitigation
   entsprechend korrigiert.
4. **ADR-0006-Divergenz `EncoderDiagnosticsAdapter` (minor) — eingearbeitet.** Bestätigt:
   ADR 0006 schreibt die Übersetzung typisierter Vendor-Codes zwingend vor und trägt eine
   stale Status-Zeile („0.9.0 (AMF)"). D8 benennt die Form-Abweichung jetzt explizit; S9
   macht das ADR-Update zur Pflicht (Status-Zeile korrigieren + Consequences amendieren)
   statt „nach jetzigem Stand nicht nötig".
5. **Fehlreferenz „ADR 0007 (Factory-Kontrakt)" (minor) — eingearbeitet.** Bestätigt: ADR
   0007 ist software-encoding-via-x264; der Factory-/Encoder-Kontrakt steht in ADR 0006,
   die Container/Encoder-Entkopplung in ADR 0008. Referenzblock im Kopf korrigiert.
6. **`CreateD3D11DeviceForLuid` nicht einfach wiederverwendbar (minor) — eingearbeitet.**
   Bestätigt: anonymer Namespace **und** innerhalb des NVENC-`__has_include`-Guards
   (`adapter_capability.cpp:34-48`) — im NVENC-losen Build nicht vorhanden. D5-2/S3
   enthalten jetzt den expliziten Extraktions-Schritt (aus dem Guard heben; braucht nur
   d3d11/dxgi) als eigenen Refactor-Commit.
7. **Bare-bool-10-Bit-Felder mehrdeutig (minor) — eingearbeitet.** Bestätigt: `false` kann
   „geprobt: nein" nicht von „nicht gemessen" trennen, und der struct-weite `probed`-Flag
   deckt Felder nicht ab, die der NVENC-Zweig anfangs nicht füllt. D5-4/S3 verwenden
   `std::optional<bool>` (`nullopt` = nicht erhoben ⇒ UI-Zeile entfällt).
8. **CQ-Mapping-Untergrenze + ungepinnte I/P-Aufteilung (minor) — eingearbeitet.**
   Bestätigt: kanonische Skala ist 1-51 (`kNvencCqMin = 1`) und NVENC nutzt Inter = Intra+2
   (`ComputeNvencRcParams`). D4-1 pinnt jetzt `qp_i = clamp(cq, 1, 51)`,
   `qp_p = min(qp_i + 2, 51)` und beide AV1-255er-Skalierungen als Testkonstanten inkl.
   Paritätstest gegen NVENC.

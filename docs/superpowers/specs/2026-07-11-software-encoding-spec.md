# Software-Encoding: x264 (+ optional SVT-AV1) — Spec für Roadmap 0.11

> **SUPERSEDED (2026-07-23):** Der x264-Teil dieser Spec (ExoSnap baut/bündelt einen eigenen
> `X264VideoEncoder`) ist überholt. ADR 0007 wurde nach einer Patentrecherche revidiert: ExoSnaps
> eigener Build bleibt hardware-only, kein x264/x265 wird von ExoSnap selbst gebaut oder
> ausgeliefert (kein Rechtsbudget für den Patent-Audit). Falls Software-H.264/HEVC-Encoding je
> kommt, dann nur über Laufzeit-Erkennung einer vom Nutzer selbst beschafften FFmpeg-Installation
> — siehe ADR 0007 „2026-07-23 revision" für die volle Begründung. Der **SVT-AV1-Teil dieser Spec
> bleibt gültig** (AV1-Patente sind lizenzgebührenfrei, unbetroffen von dieser Revision).
>
> Der Rest dieses Dokuments ist unverändert als historischer Erhebungsstand (main @ #192,
> 2026-07-11) erhalten.

**Stand der Erhebung:** main @ #192 (2026-07-11). Alle Datei:Zeile-Referenzen frisch aus dem
Code erhoben. Bezugsdokumente: `docs/roadmap.md` (0.11-Zeile + Architecture guardrails),
ADR 0006 (native SDKs + Factory-Infrastruktur), ADR 0007 (x264/SVT-AV1-Entscheidung + Audit-Gate),
ADR 0009 (kanonisches Rate-Control-Modell), ADR 0011 (Capability-Schema), ADR 0043
(Muster für ein Lizenz-Audit), `.workspace/review-fable-2026-07-10.md:237` („ein E2E-Test …
WARP/x264-Pfad genügt bis 0.11") und `:243` (Software-Fallback vor AMD/Intel).

---

## Problem

1. **Kein universeller Fallback.** ExoSnap kann heute ausschließlich über NVIDIA NVENC
   encodieren. Ohne unterstützte NVIDIA-GPU ist Aufnahme komplett blockiert
   (`KNOWN_LIMITATIONS.md:30–40`, `docs/product-spec.md:435–437`). AMD-/Intel-/iGPU-only-
   Systeme und Systeme mit defektem NVIDIA-Treiber sind ausgesperrt.
2. **Encode-Pfad ist in CI nicht testbar.** Der E2E-Test aus #186
   (`libs/recorder_core/tests/test_session_e2e_real_file.cpp:14–20`) muss den VideoThread
   durch einen Seam ersetzen, der *synthetische* Video-Pakete publiziert, weil der einzige
   Encoder eine NVIDIA-GPU braucht. Regressionen im echten Encode-Pfad (Parametrierung,
   Keyframe-Kadenz, Codec-Private-Erzeugung, Flush) sieht CI nicht.
3. **Die geplante Encoder-Architektur existiert nur auf dem Papier.** ADR 0006 definiert
   `VideoEncoderFactory` / `CapabilityProbe` / `EncoderSelectionPolicy`; real gibt es genau
   eine Implementierung (`NvencVideoEncoder`), die der VideoThread direkt und konkret
   instanziiert. AMD (0.12) und Intel (0.13) brauchen dieselbe Factory — der Software-Pfad
   ist der billigste Weg, sie unter realen Bedingungen zu validieren, bevor Vendor-SDKs
   dazukommen.
4. **Lizenz-/Patent-Frage ist ungeklärt und release-blockierend.** ADR 0007 verlangt ein
   Lizenz- + Patent-Distributions-Audit, bevor irgendein Release-Binary x264 enthält. Das
   Audit ist nie durchgeführt worden.

---

## Ist-Zustand (mit Datei:Zeile-Referenzen)

### Encoder-Instanziierung und Interface

- Der VideoThread instanziiert den Encoder **direkt und konkret**:
  `NvencVideoEncoder nvenc;` (`libs/recorder_core/src/video_thread.cpp:550`), konfiguriert
  ihn über NVENC-spezifische Setter (`SetCodec`/`SetBitDepth`/`SetChroma`/`SetCq`/
  `SetRateControl`/`SetPreset`/`SetKeyframeIntervalSecs`/`SetColor`,
  `video_thread.cpp:552–570`) und öffnet ihn mit dem D3D11-Device
  (`video_thread.cpp:573`). Es gibt **keine Factory** und keinen zweiten `IVideoEncoder`.
- `IVideoEncoder` (`libs/recorder_core/include/recorder_core/interfaces/IVideoEncoder.h:20–58`)
  ist Slot-/GPU-Texture-orientiert: `Open(void* gpu_context, …)` (Doku Zeile 25–26 sieht
  „Null for CPU-only encoders" bereits vor), `RegisterSlotTexture`, `AcquireFreeSlot`,
  `EncodeFrame(slot, pts_ns, …)` mit Buffered-Semantik (leere `bytes` = „need more input"),
  `Flush`, `RequestKeyframe` (Default-No-op, Zeile 54–56), `Destroy`.
- **`ReleaseSlot` fehlt im Interface**: `NvencVideoEncoder::ReleaseSlot`
  (`libs/recorder_core/src/nvenc_video_encoder.h:81–83`) wird vom VideoThread auf allen
  Fehlerpfaden konkret gerufen — **elf** Aufrufstellen: sechs im CFR-Zweig
  (`video_thread.cpp:2610`, `2626`, `2643`, `2652`, `2685`, `2708`) und fünf im
  VFR-Zweig (`2962`, `2975`, `3006`, `3015`, `3047`).
- `EncodedVideoPacket` = `{bytes, pts_ns, keyframe}`
  (`libs/recorder_core/include/recorder_core/packet_types.h:8–12`).

### Zero-Copy-GPU-Pipeline

- 8 Encode-Slots (`kSlotCount` `video_thread.cpp:606`; `SlotCount()`
  `nvenc_video_encoder.h:66–68`). Slot-Texturen sind `D3D11_USAGE_DEFAULT` im Format
  NV12 (8-bit 4:2:0) / P010 (10-bit) / AYUV (8-bit 4:4:4) (`video_thread.cpp:603`,
  `685–708`) und werden per `RegisterSlotTexture` bei NVENC registriert
  (`video_thread.cpp:747`).
- Der Frame-Fluss ist vollständig GPU-seitig: Capture-Textur → (Tonemap/Compositor) →
  `VideoProcessorBlt` in die Slot-Textur (`video_thread.cpp:2666–2698`) →
  `EncodeFrame(slot, …)` (`video_thread.cpp:2725`). **CFR-Duplikate** werden per
  `CopyResource` aus einer Referenz-Textur `refNv12` in den Slot kopiert
  (`video_thread.cpp:2699–2704`); Snapshot (`performSnapshotIfRequested`, `2695`) und
  WYSIWYG-Preview-Tap (`2664`) hängen ebenfalls an den Texturen, **vor** dem Encoder.
  Es gibt nirgends einen CPU-Readback des Video-Pfads.
- Codec-Private wird encoderneutral aus dem **ersten Annex-B-Keyframe-Paket** extrahiert
  (SPS/PPS bzw. VPS/SPS/PPS bzw. AV1-Sequence-Header; CFR: `video_thread.cpp:1819–1859`,
  VFR: `3137–3168`), der Muxer baut daraus avcC/hvcC und konvertiert Annex-B→AVCC pro
  Sample (`libs/recorder_core/src/mux_thread.cpp:122–129`, `370–377`). Voraussetzung an
  jeden Encoder: Annex-B-Output mit in-band Parameter-Sets auf jedem IDR.

### Codec-Modell: Backend in den Enum-Namen eingebrannt

- Zwei parallele Enums, beide mit NVENC im Namen:
  `recorder_core::VideoCodec { Av1Nvenc, H264Nvenc, HevcNvenc }`
  (`libs/recorder_core/include/recorder_core/codec_types.h:13–17`) und
  `exosnap::capability::VideoCodec { Av1Nvenc, HevcNvenc, H264Nvenc }`
  (`libs/capability/include/capability/config_types.h:10`). Zusammen 801 Verwendungen in
  81 Dateien (rg-Zählung auf app/libs/tests).
- Persistenz ist bereits backend-neutral: Presets speichern `"h264"/"hevc"/"av1"`
  (`app/settings/RecordingPresetStore.cpp:51–72`), `kPresetSchemaVersion = 23`
  (`app/models/RecordingPreset.h:45`). Additive Felder ohne Schema-Bump haben Präzedenz
  (`RecordingPresetStore.cpp:779`: „No schema bump needed").
- Sichtbare Labels laufen über den Kanon `app/ui/CodecLabels.h` (Codec-Label ist
  backend-frei: „H.264"/„HEVC"/„AV1", Zeilen 57–67) bzw.
  `capability::VisibleVideoCodecLabel` (`libs/capability/include/capability/codec_selection.h:39`).

### Rate-Control und Encoder-Parameter

- Kanonisches Modell `RateControlMode { ConstantQuality, VariableBitrate, ConstantBitrate,
  Lossless }` (`codec_types.h:110–115`, ADR 0009). NVENC-Mapping als pure Funktion
  `ComputeNvencRcParams` (`libs/recorder_core/src/nvenc_encoder.cpp:618–662`): CQ→CQP mit
  qpIntra=cq, qpInter=cq+2; VBR→avg=kbps, max=1.5×; CBR→avg=max=kbps. Lossless ist
  überall `NotImplemented` (`capability_set.h:87–90`).
- `RecorderConfig` trägt die Encoder-Parameter unter NVENC-Namen: `nvenc_cq`,
  `nvenc_rate_control`, `nvenc_preset`, `nvenc_bitrate_kbps`
  (`libs/recorder_core/include/recorder_core/recorder_session.h:289–306`);
  `keyframe_interval_secs` (`recorder_session.h:358–363`) fließt in `ComputeGopLength`
  (`nvenc_encoder.h:122`). Der NVENC-Speed-Preset P1–P7 ist ein eigener Enum
  (`codec_types.h:98–106`, ADR 0039).
- Farb-Signalisierung ist pure gekapselt: `ApplyColorMetadataToNvenc`
  (`nvenc_encoder.h:57`) spiegelt `ColorMetadata` in die Bitstream-VUI/color_config.

### Capability, Resolver, Diagnostics

- `CapabilitySet::video_codecs` ist eine flache Codec→`SupportAnnotation`-Map
  (`libs/capability/include/capability/capability_set.h:47`). Das NVENC-DLL-Gate
  (`libs/capability/src/capability_builder.cpp:119–145`) macht ohne DLL/gültige
  API-Version **drei** Dinge: (a) **alle drei Codecs** → `NotImplemented` (`:125–127`),
  (b) `bit_depths[Bit10]` → `NotImplemented` (`:131`), (c) drei **`combo_overrides`**
  auf `NotImplemented` für {Matroska, Av1Nvenc, AacMf} (`:136`),
  {Matroska, H264Nvenc, AacMf} (`:140`) und {Mp4, H264Nvenc, AacMf} (`:144`).
  `QueryCombo` wendet einen vorhandenen Override **unbedingt** an — er überschreibt
  jedes zuvor kombinierte Ergebnis (`capability_set.cpp:139–142`:
  `result = override_it->second`). Aus (a) entsteht der Recording-Blocker `rec.003`
  mit dem FixAction-Text „Switch to H.264 (NVENC)"
  (`app/diagnostics/RecommendationEngine.cpp:196–214`).
- `BestAvailableVideoCodec` (Präferenz AV1→HEVC→H.264,
  `libs/capability/include/capability/codec_selection.h:26–33`) ist die einzige Quelle für
  „bester Codec"; `ReconcileOutputFormat` (`resolver.h:98–126`,
  Aufrufer: `RecordingPreset.cpp:237`, `ConfigPage.cpp:2342`,
  `RecordingCoordinator.cpp:748/1881`) besitzt die statischen Format-Regeln
  (Container×Codec, 10-bit-Demotion, 4:4:4-Snap, MP4→CFR). Die statischen Codec-Fakten
  `CodecSupports10Bit`/`CodecSupportsChroma444` (`resolver.h:88–95`) sind heute implizit
  NVENC-Fakten.
- `ToRecorderCoreConfig` (`libs/capability/include/capability/translation.h:13`) übersetzt
  `UserRecorderConfig`→`RecorderConfig`; Aufrufer `app/services/RecordingCoordinator.cpp:726/2175`.
- Per-Adapter-Probe: `AdapterEncoderCapability` probt nur NVIDIA; AMD/Intel/Software sind
  ein bewusster Non-Probe (`libs/capability/include/capability/adapter_capability.h:44–56`).
  Die Device-Seite zeigt „Software · x264 / SVT-AV1" als statische **Planned-Row**
  (`app/pages/DevicePage.cpp:344–348`) und einen Banner „…switching the encode device is
  planned" (`DevicePage.cpp:509–514`).
- Live-Klassifikation existiert bereits encoderneutral: `StageId::Encoder` +
  `ResolvePipelineHealth` (`libs/recorder_core/include/recorder_core/pipeline_health.h:17–62`),
  gespeist von `OnEncodeSubmitted`/`OnEncodeLatency` (`video_thread.cpp:2723–2728`).
  `SelfTestRunner::CheckEncoderAvailability` = `LoadLibraryW(L"nvEncodeAPI64.dll")`
  (`app/diagnostics/SelfTestRunner.cpp:90–97`).
- Crash-Kontext hat das Backend-Feld schon: `SetEncoderContext(encoder_backend, …)`
  (`libs/crash_capture/include/crash_capture/crash_capture.h:126–179`);
  `app/MainWindow.cpp:1554` hardcodet `"nvenc"`.

### Build, Lizenz, CI

- Projektlizenz **GPL-3.0-or-later** (`LICENSE:1`). x264 ist GPL-2.0-or-later — die
  Copyright-Seite ist kompatibel (GPLv2+ erlaubt Kombination unter v3); offen ist allein
  die **AVC-Patent-Distributionsfrage** (ADR 0007, Audit-Gate).
- FFmpeg kommt als **LGPL-shared-Prebuilt** aus einem projekteigenen Build-Repo
  (`cmake/VendorFFmpeg.cmake:15/38`, `Exoridus/exosnap-ffmpeg-build` r3) — enthält bewusst
  **kein** x264 und bleibt davon unberührt (Encode läuft nie über libavcodec, ADR 0006).
- `nvEncodeAPI.h` ist eingecheckt (`third_party/nvidia/nvEncodeAPI.h`); ohne den Header
  degradiert recorder_core zu einem Skeleton-Build (`libs/recorder_core/CMakeLists.txt:1–46`).
- WARP-D3D11-Devices sind etablierte Test-Praxis für GPU-Codepfade ohne Hardware
  (`libs/recorder_core/tests/test_gpu_compositor.cpp`, `test_gpu_rgb_to_ayuv.cpp`,
  `test_overlay_shader.cpp` u. a.).

### Produkt-Aussagen, die sich ändern

- `docs/product-spec.md:80` (Default „AV1 (NVENC)"), `:232–247` (kanonisches RC; „CRF" ist
  nirgends sichtbar; Expert-Control „NVENC encoder preset"), `:433–437` („If no supported
  NVIDIA NVENC encoder is detected, recording is blocked … rather than silently falling
  back"), `:739` (Crash-Annotation „active encoder backend"), `:752` (NVIDIA-only).
- `KNOWN_LIMITATIONS.md:30–40`: „Software (CPU) H.264 or AV1 encoding fallback … not
  available".

---

## Design

### D1 — Codec und Encoder-Backend werden getrennte Achsen

**Problem:** Beide `VideoCodec`-Enums verheiraten Codec (Format-Fakt: Container-Kompat,
Mux-IDs, Labels, Remux) mit Backend (Producer-Fakt: Verfügbarkeit, Performance, Probe).
Mit x264 gäbe es sonst „H.264, encodiert von x264, gespeichert als `H264Nvenc`".

**Alternativen:**

- **(a) Neue Enum-Member pro Backend** (`H264X264`, `Av1SvtAv1`): minimal-invasiv am Anfang,
  aber jede Codec-Regel (ContainerCompatRegistry, `ReconcileOutputFormat`,
  `BestAvailableVideoCodec`, CodecLabels, TOML-Mapping, Mux-Codec-IDs) müsste jeden
  Backend-Klon mitführen. Container×Codec ist ein Codec-Fakt — `MP4+H264X264` und
  `MP4+H264Nvenc` wären zwangsläufig Duplikate, exakt die „duplizierte
  Track-Resolution-Logik", vor der CLAUDE.md warnt. Skaliert nicht auf AMF/QSV
  (0.12/0.13 würden die Matrix nochmal verdoppeln). **Verworfen.**
- **(b) Namen behalten, Backend-Feld daneben**: kein Rename-Aufwand, aber
  `video_codec = H264Nvenc` + `backend = X264` ist eine aktiv irreführende Invariante, die
  jeder künftige Leser einzeln lernen muss. **Verworfen.**
- **(c) Enum-Member auf reine Codec-Namen umbenennen** (`Av1`, `Hevc`, `H264`) **+ neue
  Backend-Achse**: ein großer, aber rein mechanischer, compilergeprüfter Rename
  (801 Stellen / 81 Dateien); Persistenz (`"h264"` …) und sichtbare Labels sind bereits
  codec-neutral, ändern sich also nicht. **Gewählt.**

**Entscheidung:** Option (c). Die Achsen-Trennung (Codec = Format-Fakt, Backend =
Producer-Fakt) ist eine eigenständige, dauerhafte Architektur-Entscheidung, auf der
auch 0.12/0.13 aufbauen — sie bekommt einen eigenen Decision-Record **ADR 0045 „Codec
and encoder backend are separate axes"** (geschrieben in Schritt 1, dem PR, der den
Rename trägt; ADR 0044 bleibt der Lizenz-/Patent-Record aus D8).

- `recorder_core::VideoCodec { Av1, H264, Hevc }` (codec_types.h) und
  `exosnap::capability::VideoCodec { Av1, Hevc, H264 }` (config_types.h) — reiner
  Member-Rename, keine Wertänderung.
- Neu in `recorder_core/codec_types.h`:

  ```cpp
  // Which encoder implementation produces the bitstream. Orthogonal to VideoCodec
  // (the format fact). The engine only ever sees a concrete backend — "Auto" is
  // resolved by capability::ResolveEncoderBackend before a session starts.
  enum class VideoEncoderBackend {
      Nvenc,  // NVIDIA NVENC (hardware, zero-copy D3D11)
      X264,   // x264 software H.264 (GPU->CPU readback)
      SvtAv1, // SVT-AV1 software AV1 (build-time optional, EXOSNAP_WITH_SVT_AV1)
  };
  ```

- Neu in `capability/user_config.h`: `enum class EncoderBackendChoice { Auto, Nvenc, X264,
  SvtAv1 };` + Feld `UserRecorderConfig::encoder_backend = EncoderBackendChoice::Auto`.
  `RecorderConfig` bekommt `VideoEncoderBackend video_encoder_backend =
  VideoEncoderBackend::Nvenc` — **immer konkret**, nie Auto: die Engine trifft keine
  Policy-Entscheidungen (UI-Agnostik-Leitplanke; die Auflösung passiert im Resolver, D5).
- TOML: additiver Preset-Key `output.video_encoder = "auto" | "nvenc" | "x264" | "svt-av1"`,
  Default `auto`, **kein** Schema-Bump (Präzedenz `RecordingPresetStore.cpp:779`; ein
  fehlender Key fällt auf Auto — das ist für jeden Bestandspreset die richtige Antwort).

### D2 — VideoEncoderFactory + minimale Interface-Erweiterung

**Alternativen:** (a) Factory in `libs/capability` — verworfen, die Factory baut
Engine-Objekte und gehört in die Engine; capability bleibt die reine Policy-Schicht.
(b) `if (backend == …)` direkt im VideoThread — verworfen, genau die Streuung, die ADR
0006 verhindern will; 0.12/0.13 würden den Hot-Loop weiter aufblähen.

**Entscheidung:** `libs/recorder_core/src/video_encoder_factory.{h,cpp}`:

```cpp
// Constructs a fully configured encoder for the session config. The returned
// encoder is ready for Open()/Configure(); all codec/RC/color/GOP parameters
// are applied here so VideoThread never touches a concrete encoder type.
std::unique_ptr<IVideoEncoder> CreateVideoEncoder(const RecorderConfig& config,
                                                  std::string& out_error);
```

- Die NVENC-spezifischen Setter (`video_thread.cpp:552–570`) wandern in die Factory;
  der VideoThread hält nur noch `std::unique_ptr<IVideoEncoder> encoder` und ruft die
  Interface-Methoden. Verhalten byte-identisch (reine Umhängung).
- `IVideoEncoder` bekommt **eine** neue Methode: `virtual void ReleaseSlot(int32_t
  slot_idx) noexcept = 0;` — der VideoThread ruft sie heute schon konkret auf elf
  Fehlerpfaden (6 CFR + 5 VFR, Ist-Zustand); sie ist Teil des faktischen Vertrags und
  muss ins Interface.
  Mehr wird **nicht** verallgemeinert (kein `EncoderCapabilitySchema`-Vollausbau aus ADR
  0011 in dieser Welle — siehe „Bewusst nicht gebaut").

### D3 — GPU→CPU-Readback: gekapselt im Software-Encoder, nicht im VideoThread

**Alternativen:**

- **(A) Readback im Software-Encoder** (hinter `IVideoEncoder`): `RegisterSlotTexture`
  merkt sich die Slot-Texturen; `EncodeFrame(slot)` kopiert die Slot-Textur in eine
  Staging-Textur, mapped sie und füttert x264. Der VideoThread bleibt unverändert —
  insbesondere funktionieren CFR-Duplikate (`CopyResource` refNv12→Slot,
  `video_thread.cpp:2699–2704`), Snapshot und Preview-Tap weiter, weil die Slot-Texturen
  weiterhin existieren und weiterhin die Wahrheit sind.
- **(B) Readback als Stufe im VideoThread** + CPU-Submit-Methode am Interface: hält den
  Encoder D3D11-frei, aber der Hot-Loop bekommt einen zweiten Submit-Pfad, und die
  Duplikat-/Snapshot-/Referenz-Maschinerie müsste zweigleisig werden (Texturen für
  NVENC, CPU-Puffer für Software) — Streuung genau dort, wo der Code heute schon
  drei Quell-Pfade (HDR-nativ / SDR / Duplikat) balanciert.
- **(C) `IVideoEncoder` um einen generischen CPU-Frame-Pfad erweitern**: verallgemeinert
  auf Verdacht (AMF/QSV wollen D3D11-Surfaces, keinen CPU-Pfad). Spekulatives
  Overengineering.

**Entscheidung:** **(A)**, mit einem wiederverwendbaren, einzeln testbaren Baustein:

- **`Nv12CpuReadback`** (`libs/recorder_core/src/nv12_cpu_readback.{h,cpp}`): besitzt einen
  Ring von `kSlotCount` Staging-Texturen (NV12, `D3D11_USAGE_STAGING`,
  `D3D11_CPU_ACCESS_READ`), API:
  `Init(ID3D11Device*, width, height, slot_count, out_error)` ·
  `ReadSlot(ID3D11Texture2D* src, int32_t slot, MappedNv12& out, out_error)`
  (CopyResource → Map(READ) → Y-/UV-Pointer + RowPitch) · `Unmap(slot)`. Der Map-Aufruf
  ist ein bewusster GPU-Sync-Punkt (wartet auf den VideoProcessorBlt desselben Slots);
  v1 nimmt diesen Stall in Kauf und **misst** ihn über den existierenden
  `OnEncodeLatency`-Tap (Encoder-Karte klassifiziert Busy/Bottleneck von selbst).
  Eine um einen Frame versetzte Map-Pipeline ist eine dokumentierte Folge-Optimierung,
  kein 0.11-Scope. Threading: alle Aufrufe ausschließlich auf dem VideoThread — derselbe
  Vertrag, unter dem NVENC das Device heute benutzt (`video_thread.cpp:6–10`).
- **`X264Encoder`** (`libs/recorder_core/src/x264_encoder.{h,cpp}`): der pure CPU-Kern
  nach dem Vorbild `NvencEncoder` — kennt **kein** D3D11. API: `Configure(const
  X264Settings&, out_error)` · `EncodeNv12(const uint8_t* y, int y_stride, const uint8_t*
  uv, int uv_stride, uint64_t pts_ns, bool force_idr, EncodedVideoPacket& out, out_error)`
  (x264 akzeptiert `X264_CSP_NV12` direkt — kein Deinterleave nötig; x264 kopiert die
  Input-Planes beim Submit, Unmap direkt danach ist sicher) · `Flush(out_packets,
  out_error)` · `Close()`. GPU-los unit-testbar — das ist der CI-Enabler.
- **`X264VideoEncoder`** (`libs/recorder_core/src/x264_video_encoder.{h,cpp}`,
  implementiert `IVideoEncoder`): dünner Kleber = Slot-Texturen + `Nv12CpuReadback` +
  `X264Encoder`. `Open(gpu_context)` akzeptiert das D3D11-Device; `SlotCount()` = 8 wie
  NVENC (die Slot-Ring-Größe bleibt eine VideoThread-Konstante); `AcquireFreeSlot` ist
  trivial (Software kennt kein in-flight-Mapping — ein simpler Rundlauf-Cursor genügt,
  jeder Slot ist nach `EncodeFrame`-Rückkehr sofort wieder frei); `RequestKeyframe`
  armiert `force_idr` für den nächsten Submit.

### D4 — x264-Integration: Bezug, Parameter, Rate-Control-Mapping

**Bezugsquelle.** Alternativen: (a) FetchContent + autotools — x264 hat kein CMake;
MSVC-CI bräuchte eine msys2-Toolchain, neuer CI-Komplexitätsberg. (b) ShiftMediaProject-
Fork (MSVC-Solutions) — baubar, aber Fork-Pflege + kein Pinning-Muster im Projekt.
(c) vcpkg — dritter Paketmanager nur für eine Bibliothek. (d) **projekteigenes
Prebuilt-Repo** nach dem exakten Muster von `exosnap-ffmpeg-build`
(`cmake/VendorFFmpeg.cmake`): mingw-Cross-Build, gepinnte Release-URL + SHA256,
Lizenz-Staging. **Entscheidung: (d)** — `Exoridus/exosnap-x264-build` produziert
`libx264-<api>.dll` + Import-Lib + `x264.h`/`x264_config.h`; als **DLL**, nicht statisch,
weil eine mingw-`.a` nicht MSVC-linkbar ist, eine C-ABI-DLL mit Import-Lib dagegen schon
(derselbe Grund, aus dem die FFmpeg-DLLs shared sind). GPL-Quelltext-Angebot über das
Build-Repo (Tag = exakter Quellstand), `THIRD_PARTY_NOTICES.md` + gestagte
`licenses/x264.txt` wie bei jedem anderen Vendor-Baustein.
CMake: `cmake/VendorX264.cmake` + Option **`EXOSNAP_WITH_X264` (Default `OFF`)** — das
ist das in ADR 0007 geforderte, zentrale Distributions-Gate: CI-Test-Jobs schalten es
`ON` (Bauen/Testen ist keine Distribution), Release-Packaging bleibt `OFF`, bis das
Audit (D8) abgenommen ist. Compile-Definition `EXOSNAP_HAS_X264` steuert Factory- und
Capability-Registrierung.

**Parameter-Politik (fest verdrahtet in einer puren, testbaren Funktion
`BuildX264Params(const X264Settings&) -> x264_param_t`, Spiegel von
`ComputeNvencRcParams`/`ApplyColorMetadataToNvenc`/`ComputeGopLength`):**

| Parameter | Wert | Begründung |
|---|---|---|
| Preset | `veryfast`, fest | Realtime-Screen-Encode-Sweetspot; kein UI-Regler in 0.11 (siehe D7) |
| Tune | keins | `zerolatency` unnötig (Buffered-Semantik existiert) und kostet Kompression |
| `i_threads` | 0 (auto), Frame-Threads | Latenz unkritisch; Sliced-Threads kosten Qualität |
| `i_bframe` | **0** | Pipeline und Muxer erwarten Submissionsreihenfolge == Output-Reihenfolge (NVENC läuft heute `frameIntervalP=1`, `nvenc_encoder.h:139–151`); B-Frames sind ein M-2-/1.0-Thema |
| `i_keyint_max` = `i_keyint_min` | `ComputeGopLength(keyframe_interval_secs, fps)` — Funktion wird wiederverwendet | identische IDR-Kadenz wie NVENC; Quick-Trim-/Split-Verhalten bleibt gleich |
| `i_scenecut_threshold` | 0 | deterministische GOPs; kein Keyframe-Spam bei Bildschnitt-lastigem Desktop-Content |
| `b_open_gop` | 0 | Split-Segmente brauchen self-contained IDRs |
| `b_annexb` | 1 | Codec-Private-Extraktion + Annex-B→AVCC im Muxer bleiben unverändert (Ist-Zustand) |
| `b_repeat_headers` | 1 | SPS/PPS auf jedem IDR — Voraussetzung für `video_thread.cpp:1819` und Segment-Splits |
| VUI | `ApplyColorMetadataToX264(param, color)` (pure, neu) | `vui.b_fullrange`, `colorprim/transfer/colmatrix` aus derselben `ColorMetadata` wie VideoProcessor + Container — Drei-Schreiber-Konsens bleibt erhalten |
| `i_csp` | `X264_CSP_NV12` | direktes Füttern des gemappten Staging-Speichers |
| Timebase | 1/1'000'000'000; `i_pts = pts_ns` | verlustfreie PTS-Durchleitung; `param.i_fps_num/den` zusätzlich als RC-Hint |
| Profil | High (implizit aus 8-bit 4:2:0) | Scope-Grenze, siehe unten |

**Rate-Control-Mapping (kanonisches Modell → x264, gemäß ADR 0009; „CRF" bleibt ein
reines Implementierungsdetail und taucht in keinem UI-String auf):**

| Kanonisch | x264 | Detail |
|---|---|---|
| ConstantQuality | `X264_RC_CRF`, `f_rf_constant = clamp(cq, 1, 51)` | der CQ-Wert wird numerisch 1:1 übernommen; die perzeptuellen Kurven unterscheiden sich (von ADR 0009 ausdrücklich gedeckt — „the numeric range may differ … with different perceptual curves"); keine Pseudo-Äquivalenz-Tabelle |
| VariableBitrate | `X264_RC_ABR`, `i_bitrate = kbps`, `vbv_max = 1.5×kbps`, `vbv_buf = 2×kbps` | spiegelt NVENCs avg/1.5×max (`nvenc_encoder.cpp:636–645`) |
| ConstantBitrate | `X264_RC_ABR`, `i_bitrate = vbv_max = kbps`, `vbv_buf = kbps` (1 s), `i_nal_hrd = X264_NAL_HRD_VBR` | strikt genug für den Produkt-Anspruch; echtes NAL-HRD-CBR mit Filler ist Datenverschwendung für Recording |
| Lossless | bleibt `NotImplemented` | keine stille MVP-Expansion, obwohl x264 qp=0 könnte |

**Scope-Grenzen des Software-Pfads (hart, resolver-erzwungen):** 8-bit only, 4:2:0 only,
SDR only. 10-bit → 8-bit-Demotion, 4:4:4 → 4:2:0-Snap, HDR10 bleibt für H.264 ohnehin
blockiert (bestehende Regel). x264 könnte 10-bit/4:4:4 — bewusst nicht, gleiche
Zurückhaltung wie beim NVENC-MVP.

**Fehlerbild:** `x264_encoder_open`-Fehlschlag → `Open`/`Configure` liefert `false` +
`out_error`; VideoThread meldet über den bestehenden
`RecordFailure(…, ErrorPhase::VideoEncode, …)`-Pfad. Ein eigener
`EncoderDiagnosticsAdapter` (ADR 0006) wird für x264 **nicht** gebaut — x264 hat kein
Vendor-Fehlercode-Universum wie NVENC/AMF; ein String reicht ehrlich aus.
**ADR-Konflikt, explizit aufgelöst:** ADR 0007:64–65 fordert wörtlich, dass
Software-Encoding-Performance-Warnungen „through `EncoderDiagnosticsAdapter`, not
suppressed" laufen. Die *Absicht* (nicht unterdrücken) erfüllt diese Spec über den
existierenden `OnEncodeLatency`-Tap + `ResolvePipelineHealth` + die
`rec.encoder.software`-Notice (D6); den *Wortlaut* (eigene Adapter-Klasse) erfüllt sie
bewusst nicht. Damit die Implementierung nicht gegen einen offenen ADR läuft, wird
ADR 0007 in dieser Welle **amendiert** (Schritt 8): die Adapter-Klausel wird durch
„surfaced through the encoder pipeline-health path (`OnEncodeLatency` →
`ResolvePipelineHealth`) and the pre-flight software-encoder notice" ersetzt;
`EncoderDiagnosticsAdapter` bleibt Vorgabe für die Vendor-Wellen 0.12/0.13.

### D5 — Capability-Modell: Backend-Achse, Selection-Policy, Format-Gates

**Alternativen:** (a) `CapabilitySet::video_codecs` bleibt flach und Software überschreibt
das DLL-Gate — verliert die Information, *welches* Backend einen Codec trägt; Device-Seite
und FixActions könnten nicht mehr ehrlich formulieren. (b) Voller
`EncoderCapabilitySchema` aus ADR 0011 — richtig für die Vendor-Wellen, überdimensioniert
für „NVENC + statisch gelinktes x264". **Entscheidung:** minimale Backend-Achse, die auf
das ADR-0011-Schema hin erweiterbar ist:

- `RuntimeCapabilitySnapshot` erhält `SoftwareEncoderFacts { bool x264_built;
  std::string x264_version; bool svt_av1_built; std::string svt_av1_version; }`
  (`runtime_snapshot.h`) — Compile-Time-Fakten (`EXOSNAP_HAS_X264`, `x264_build`-Konstante),
  als Runtime-Fakt geführt, damit Provenance-Texte und der Capability-Cache-Key sie
  einheitlich sehen.
- `CapabilitySet` erhält
  `std::unordered_map<VideoCodec, std::unordered_map<recorder_core::VideoEncoderBackend,
  SupportAnnotation>> video_codec_backends;` + `QueryVideoCodecBackend(codec, backend)`.
  Die bestehende flache `video_codecs`-Map wird in `BuildEffectiveCapabilities` als
  **Ableitung** befüllt: beste Annotation über alle Backends. Dadurch funktionieren alle
  bestehenden Abfragen (OptionQuery, rec.003, Record-Header) unverändert weiter.
- **Das DLL-Gate (`capability_builder.cpp:119–145`) wird als Ganzes backend-bewusst** —
  alle drei Wirkungen, nicht nur die Codec-Spalte:
  - **(a) Codec-Downgrade** (`:125–127`): trifft künftig nur die NVENC-Spalte der
    `video_codec_backends`-Map; die flache Map ergibt sich aus der Ableitung.
  - **(b) `bit_depths[Bit10]` → `NotImplemented`** (`:131`): bleibt inhaltlich korrekt,
    solange kein Software-Backend 10-bit liefert (D4-Scope: 8-bit only) — wird aber als
    „kein *verfügbares* Backend liefert 10-bit" formuliert statt als NVENC-Implikat,
    damit die Regel bei D9/0.12 nicht stillschweigend falsch wird.
  - **(c) `combo_overrides`** (`:136/:140/:144`): `QueryCombo` wendet einen Override
    **unbedingt** an (`capability_set.cpp:139–142`) — die abgeleitete flache Map räumt
    ihn *nicht* weg. Ein Override wird deshalb künftig nur noch gesetzt, wenn **kein
    Backend (Hardware oder Software)** den Video-Codec des Combos tragen kann:
    {Mp4, H264, AacMf} und {Matroska, H264, AacMf} entfallen bei `x264_built`;
    {Matroska, Av1, AacMf} entfällt nur bei `svt_av1_built` (Default-OFF ⇒ bleibt
    gesetzt, aber mit ehrlichem Reason-Text „no AV1 encoder is available on this
    system" statt des dann irreführenden NVENC-Texts). Ohne diese Kopplung wäre auf
    einer GPU-losen Maschine mit x264 `QueryVideoCodec(H264) = Available`, aber die
    vetted MP4+H.264+AAC-Ausgabe (Roadmap-Matrix, `docs/roadmap.md:116`) in Resolver
    und OptionQuery weiter gesperrt — der zentrale Wave-Anspruch (D6.2) bräche genau
    auf der Zielmaschine.
- Baseline mit x264 einkompiliert: `H264/X264 = Available` („statically shipped software
  encoder"); `Av1/SvtAv1 = Available` nur bei `svt_av1_built`, sonst `NotImplemented`;
  `Hevc/<software> = NotImplemented` („no software HEVC encoder is shipped").
- **`EncoderSelectionPolicy`** (neu, pure: `libs/capability/include/capability/
  encoder_selection.h` + src + tests):

  ```cpp
  struct EncoderBackendResolution {
      recorder_core::VideoEncoderBackend backend;
      SupportAnnotation annotation;   // why this backend / why not
      bool is_software = false;       // drives the perf notice + UI hint
  };
  // Auto: prefer hardware (Nvenc) when selectable for `codec`, else the software
  // backend for `codec`. A concrete user choice is honored when selectable and
  // otherwise reported as invalid (no silent substitution).
  std::optional<EncoderBackendResolution> ResolveEncoderBackend(
      const CapabilitySet& caps, VideoCodec codec, EncoderBackendChoice choice) noexcept;
  ```

  Einzige Quelle für Backend-Auflösung; Konsumenten: `ToRecorderCoreConfig` (stampt
  `RecorderConfig::video_encoder_backend`), Diagnostics (rec.003-Fix, Software-Notice),
  ConfigPage (Anzeige des aufgelösten Backends hinter „Auto"), DevicePage,
  Crash-Kontext, SessionStats.
- **`BestAvailableVideoCodec`** wird backend-bewusst: ein Codec qualifiziert, wenn
  *irgendein* Backend selectable ist (über die abgeleitete flache Map automatisch erfüllt).
  Auf einer GPU-losen Maschine ist das Ergebnis H.264 (x264) statt `nullopt` — der
  rec.003-Fix formuliert dann „Switch to H.264 (x264 software)".
- **Format-Gates pro Backend** landen im Resolver (Policy-Leitplanke: libs/capability):
  `ReconcileOutputFormat` bekommt das aufgelöste Backend als Input und zwei neue Regeln
  in kanonischer Reihenfolge nach der Container-Regel: Software-Backend ⇒ Bit10→Bit8
  (`bit_depth_demoted`), Cs444→Cs420 (`chroma_snapped`) — dieselben Outcome-Flags,
  dieselben Aufrufer, keine neue Mechanik.

### D6 — Fallback-Policy, Performance-Warnungen, Diagnostics-Integration

**Grundsatz (bestätigt bestehende Produktlinie, ruhig statt alarmistisch):**

1. **Kein stiller Fallback, nie.** Auto wählt das Backend **vor** Session-Start über
   `ResolveEncoderBackend` und das Ergebnis ist sichtbar (Settings-Zeile + Device-Banner).
   Schlägt NVENC `Open`/`Configure` zur Laufzeit fehl, scheitert die Session sichtbar wie
   heute (`video_thread.cpp:573–586`) — es wird **nicht** heimlich mit x264 neu gestartet
   (anderes Perf-/Qualitätsprofil; ein stiller Wechsel wäre eine Lüge über das, was
   aufgenommen wurde). Diagnostics bietet nach dem Fehlschlag den Wechsel als FixAction an.
2. **Ohne NVIDIA-Hardware ist Software der normale Auto-Pfad, kein Blocker mehr.** Das
   heutige „alle Codecs NotImplemented → rec.003-Blocker" wird zu: H.264 (x264) ist
   selectable, Auto löst auf X264 auf, Aufnahme geht. Statt des Blockers erscheint eine
   **einmalige Notice** (siehe 3). `docs/product-spec.md:435–437` und
   `KNOWN_LIMITATIONS.md` werden entsprechend umformuliert (Schritt 8) — das ist die
   zentrale user-sichtbare Verhaltensänderung dieser Welle.
3. **Eine neue statische Pre-Flight-Notice** `rec.encoder.software` (Namensmuster wie
   `rec.color.range`, `RecommendationEngine.cpp:751–752`), Severity **Notice**, nie
   Blocker: feuert, wenn das aufgelöste Backend Software ist. Text ruhig und konkret:
   Software-Encoding läuft auf der CPU; ab ca. 1440p/60 oder 4K kann die CPU zum
   Engpass werden — die Live-Karten zeigen es. **Genau ein Fix:** wenn für den gewählten
   Codec ein Hardware-Backend selectable ist (User hat manuell Software gewählt) →
   FixAction „Switch to NVENC"; sonst kein Fix (es gibt nichts zu fixen — Information,
   keine Alarmierung). Keine statische CPU-Benchmark-Raterei: ob es reicht, ist eine
   **Messfrage**, und die Messinfrastruktur existiert (Punkt 4).
4. **Live:** keine neue Mechanik. `OnEncodeSubmitted`/`OnEncodeLatency` werden vom
   Factory-Encoder unverändert gespeist; `ResolvePipelineHealth` klassifiziert den
   Encoder-Bottleneck; die Sustained-Lag-Resync-Maschine (`video_thread.cpp:2481–2507`)
   macht Überlast als echte Drops sichtbar (Toast existiert). Einzige Ergänzung: die
   Encoder-Pipeline-Karte taggt **CPU** statt GPU, wenn das Backend Software ist
   (DiagnosticsPage-Zuordnung).
5. `SelfTestRunner::CheckEncoderAvailability` (`SelfTestRunner.cpp:90–97`) meldet
   künftig: NVENC-DLL vorhanden **oder** Software-Encoder einkompiliert ⇒ pass, mit
   Detail-Text, welche Backends da sind.
6. Crash-Kontext: `MainWindow.cpp:1554` übergibt statt hardcodiertem `"nvenc"` das
   aufgelöste Backend (`"nvenc"`/`"x264"`/`"svt-av1"` — das Feld ist dafür gebaut,
   `crash_capture.h:126`). `SessionStats` erhält `video_encoder_backend` für Report-Card
   und Recording-History-Detailtext.

### D7 — UI

- **Expert-Control „Encoder"** in Settings → Video, Container-&-codecs-Expert-Sektion
  (neben Rate control / NVENC preset): Optionen `Auto (recommended)` · `NVENC` ·
  `x264 (software)` (· `SVT-AV1 (software)` nur wenn einkompiliert). Nicht-selectable
  Einträge disabled mit `SupportAnnotation::reason` (OptionQuery-Muster). Bei `Auto`
  zeigt der Subtext das aufgelöste Backend („Auto — using NVENC"). Default Auto.
  Label-Kanon: Codec-Labels bleiben backend-frei; Backend-Namen sind Eigennamen in
  Original-Schreibweise („NVENC", „x264", „SVT-AV1") — neue Helper
  `videoEncoderBackendLabel(...)` in `CodecLabels.h`, delegiert an eine pure
  `capability::VisibleEncoderBackendLabel` (Spiegel von `VisibleVideoCodecLabel`,
  `codec_selection.h:39`).
- **„NVENC encoder preset" (P1–P7)** wird disabled (nicht versteckt), wenn das aufgelöste
  Backend Software ist, mit Reason-Text („applies to the NVENC backend"). Ein
  x264-Speed-Regler wird in 0.11 **nicht** gebaut (fest `veryfast`); das ist die gleiche
  Zurückhaltung wie beim ursprünglichen NVENC-MVP (hartkodiertes P4/P6 bis ADR 0039).
- **DevicePage:** die Software-Zeile wandert aus der statischen Planned-Liste
  (`DevicePage.cpp:344–348` — Eintrag entfernen) in einen echten Abschnitt „SOFTWARE
  ENCODER" mit Provenance („x264 <version>, statically shipped — available on every
  machine") und Badge `ACTIVE ENCODER`, wenn das aufgelöste Backend Software ist
  (Badge-Logik `DevicePage.cpp:613–623` wird backend-bewusst statt „probed NVIDIA ⇒
  aktiv"). AMD/Intel bleiben Planned.
- **RecordPage/Format-Zusammenfassung:** unverändert (zeigt Codec, nicht Backend).

### D8 — Lizenz-/Patent-Audit als expliziter Schritt (Release-Gate)

Nach dem Muster von ADR 0043 (fdk-aac) entsteht **ADR 0044 „x264 license and H.264/AVC
patent position"** als eigener, recherchierter Entscheidungs-Record. Inhalt (Recherche,
nicht in dieser Spec vorentschieden):

- **Copyright:** x264 GPL-2.0-or-later in GPL-3.0-or-later-Binary = kompatibel
  (or-later-Upgrade); Quelltext-Angebot über das Build-Repo; `THIRD_PARTY_NOTICES.md`.
- **Patente:** Via-LA-AVC-Pool-Status (Royalty-Schwellen für kostenlose Distribution,
  historisch 100k Units/Jahr frei), Ablaufstand der AVC-Patente nach Jurisdiktion
  (Baseline weitgehend abgelaufen, High-Profile-Restlaufzeiten), Präzedenzen
  (OBS/HandBrake/Distros shippen x264 seit Jahren), Einordnung, dass ExoSnap mit
  NVENC-H.264 und AAC bereits heute AVC-/AAC-Funktionalität distribuiert — der marginale
  neue Aspekt ist ein *Software*-AVC-Encoder im Binary. SVT-AV1 gleich mitbehandeln
  (BSD-3-Clause-Clear + AOM Patent License 1.0; AOM-PL×GPL-Kompatibilitätsdiskussion
  dokumentieren).
- **Gate-Mechanik (verbindlich):** `EXOSNAP_WITH_X264` bleibt `OFF` für
  Release-Packaging, bis ADR 0044 den Status „Accepted" trägt (Maintainer-Sign-off wie
  bei ADR 0043). Der Flip auf Default-`ON` passiert im selben PR, der den
  Accepted-Status einträgt. CI-Test-Jobs dürfen vorher `ON` bauen (Testen ≠ Distribution).
  Damit ist ADR 0007s Forderung „enforced in the release pipeline, not deferred to
  runtime" wörtlich erfüllt: ein Binary ohne das Flag enthält keinen x264-Code und
  keine Capability-Registrierung.

### D9 — SVT-AV1 (optional, Phase 2 dieser Welle)

Eigenes Flag `EXOSNAP_WITH_SVT_AV1` (Default `OFF`, auch nach dem x264-Audit — Perf-/
Binary-Size-Charakterisierung steht laut ADR 0007 aus). SVT-AV1 hat natives CMake ⇒
FetchContent nach dem libopus-Muster (`third_party/CMakeLists.txt:74–87`) statt
Prebuilt-Repo. `SvtAv1Encoder`-Kern + `SvtAv1VideoEncoder` analog D3/D4;
NV12-Input muss in EbSvtIOFormat-Planes deinterleaved werden (CPU-Kopie, im Kern
gekapselt); RC: ConstantQuality→CRF, VBR/CBR→TBR-Modi; Codec-Private über das bestehende
`DeriveAv1CodecPrivate` (parst den Sequence-Header aus dem ersten Keyframe —
encoderneutral, `video_thread.cpp:1839`). Realtime-Preset-Wahl (etwa Preset 9–10) wird
bei der Charakterisierung festgelegt. Alles Weitere (Capability-Spalte, Auto-Ordnung
Nvenc→SvtAv1 für AV1, WebM+AV1 software) fällt aus D5 automatisch heraus. **Wird nur
gebaut, wenn Phase 1 (x264) gelandet ist; kein 0.11-Release-Blocker.**

### D10 — GPU-less CI

Drei Stufen, aufeinander aufbauend:

1. **Pure Unit-Tests** (kein Device): `X264Encoder` end-to-end auf synthetischen
   NV12-Frames (Gradient/Farbbalken): Open/Configure, N Frames, Flush; Asserts:
   Annex-B-Struktur, SPS/PPS auf Frame 0 und auf jedem IDR, IDR-Kadenz == `ComputeGopLength`,
   `RequestKeyframe`-Äquivalent (force_idr) landet framegenau, PTS-Monotonie,
   `BuildX264Params`-Mapping-Tabellen (RC, VUI, GOP) als reine Werte-Tests.
2. **WARP-Integrationstest** (`test_x264_video_encoder.cpp`): WARP-Device, NV12-Textur mit
   CPU-geschriebenem Muster → `X264VideoEncoder` über das volle `IVideoEncoder`-Protokoll
   (RegisterSlotTexture/AcquireFreeSlot/EncodeFrame/Flush) → dekodierbare Pakete. Testet
   `Nv12CpuReadback` (CopyResource/Map/Strides) ohne Hardware — WARP-Präzedenz existiert
   (`test_gpu_compositor.cpp` u. a.).
3. **E2E-Ausbau von #186**: `test_session_e2e_real_file.cpp` bekommt einen dritten Fall
   „MKV/MP4 + H.264 **echt von x264 encodiert** + AAC": der vorhandene Video-Feeder-Seam
   ruft statt der synthetischen Paket-Fabrik den `X264Encoder` und publiziert dessen echte
   SPS/PPS als Codec-Private. Validierung wie gehabt über die vendored libavformat (Demux
   bis EOF, Keyframes, Monotonie, Dauer) plus — wenn der lgpl-DLL-Satz den nativen
   H.264-Decoder enthält (zu verifizieren; `avcodec` wird bereits deployt,
   `KNOWN_LIMITATIONS.md:229–234`) — ein Decode-Smoke des ersten Keyframes. Falls der
   Decoder im Mux-only-Build deaktiviert ist: Demux-Validierung genügt für 0.11, und ein
   `exosnap-ffmpeg-build`-r4 mit `h264`-Decoder wird als Follow-up notiert (den braucht
   der Editor-Video-Preview ohnehin).
   Damit ist erstmals die Kette Encoder→Mux→Finalize→Fremd-Reader **ohne NVIDIA-Hardware**
   in CI — exakt der in `.workspace/review-fable-2026-07-10.md:237` geforderte Zustand.

### Bewusst NICHT gebaut (ehrliche Scope-Grenzen)

- **Kein x265 / kein Software-HEVC** (HEVC-Patentlage kategorisch anders; kein Bedarf —
  Software-H.264 + optional Software-AV1 decken den Fallback ab).
- **Kein 10-bit, kein 4:4:4, kein HDR im Software-Pfad** (obwohl x264 es könnte).
- **Kein automatischer Mid-Session- oder Start-Retry-Fallback** Hardware→Software.
- **Kein x264-Speed-Preset-UI** (fest `veryfast`), **kein** Lossless.
- **Kein voller `EncoderCapabilitySchema`** (ADR 0011) und **kein
  `EncoderDiagnosticsAdapter`** als eigene Klassen — die Vendor-Wellen 0.12/0.13
  brauchen sie wirklich, x264 nicht. Achtung: Letzteres widerspricht dem Wortlaut von
  ADR 0007:64–65 und ist deshalb an das ADR-0007-Amendment gekoppelt (D4/Schritt 8) —
  ohne das Amendment gilt diese Scope-Grenze nicht als beschlossen.
- **Kein libavcodec-Encoder-Wrapper** (ADR 0006 bleibt verbindlich).
- **Kein ARM64-Build** (x264 macht ihn später *möglich*, mehr nicht).
- **SVT-AV1 bleibt Default-OFF** bis zur Perf-/Größen-Charakterisierung.

---

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz; Reihenfolge ist
verbindlich (1→2 sind reine Enabler ohne Verhaltensänderung).

1. **Codec/Backend-Entwirrung (mechanisch, verhaltensneutral).**
   Rename `Av1Nvenc/H264Nvenc/HevcNvenc` → `Av1/H264/Hevc` in beiden Enums
   (`libs/recorder_core/include/recorder_core/codec_types.h:13–17`,
   `libs/capability/include/capability/config_types.h:10` + alle 81 Dateien);
   neuer Enum `recorder_core::VideoEncoderBackend` (codec_types.h);
   `capability::EncoderBackendChoice` + `UserRecorderConfig::encoder_backend`
   (`user_config.h`); `RecorderConfig::video_encoder_backend` (Default `Nvenc`,
   `recorder_session.h`); Preset-TOML-Key `output.video_encoder` (Default `"auto"`,
   kein Schema-Bump; `RecordingPresetStore.cpp` To/From-String + Roundtrip-Test).
   Dazu **ADR 0045** (Codec/Backend-Achsen-Trennung, D1) als Decision-Record im selben PR.
   *Tests:* Compile ist der Hauptbeweis; Preset-Roundtrip; bestehende Suiten grün.
2. **VideoEncoderFactory + Interface.**
   `IVideoEncoder::ReleaseSlot` ergänzen (`IVideoEncoder.h`); `video_encoder_factory.{h,cpp}`
   mit NVENC-Konstruktion (Setter aus `video_thread.cpp:552–570` hierher);
   VideoThread auf `std::unique_ptr<IVideoEncoder>` umstellen (alle `nvenc.`-Aufrufe,
   inkl. **aller elf** `ReleaseSlot`-Fehlerpfade — sechs im CFR-Zweig (`2610`, `2626`,
   `2643`, `2652`, `2685`, `2708`) **und** fünf im VFR-Zweig (`2962`, `2975`, `3006`,
   `3015`, `3047`); Vollständigkeit per `rg 'nvenc\.' video_thread.cpp == 0 Treffer`
   nachweisen). Unbekanntes/nicht einkompiliertes Backend ⇒
   Factory-Fehler → `RecordFailure(ErrorPhase::Prepare)`.
   *Tests:* bestehende `test_nvenc_video_encoder_interface.cpp` erweitern
   (Factory liefert NVENC für `Nvenc`); Live-Verify durch User (eine normale Aufnahme,
   byte-gleiches Verhalten erwartet).
3. **Vendor-Build x264.**
   Repo `Exoridus/exosnap-x264-build` (mingw-Cross, DLL + Import-Lib + Header, gepinnter
   Tag, SHA256); `cmake/VendorX264.cmake` nach `VendorFFmpeg.cmake`-Muster;
   `EXOSNAP_WITH_X264` Default `OFF`; DLL-Deploy in `dist`/Packaging analog
   FFmpeg-DLLs; `THIRD_PARTY_NOTICES.md` + `licenses/x264.txt` Staging.
   *Tests:* Configure/Build-Smoke mit Flag ON in einem CI-Job; Packaging-Gate prüft,
   dass die DLL im ZIP/MSI liegt gdw. Flag ON.
4. **X264Encoder-Kern (pure) + Parametrik.**
   `x264_encoder.{h,cpp}`: `X264Settings` (codec-frei: width/height/fps/cq/rc/bitrate/
   keyframe_interval_secs/ColorMetadata), `BuildX264Params` (pure),
   `ApplyColorMetadataToX264` (pure), `EncodeNv12`/`Flush`/`Close` mit
   Buffered-Semantik (ein Output-Paket pro Aufruf, leere bytes bei Delay), `force_idr`.
   *Tests:* D10-Stufe 1 komplett (GPU-los, läuft auf jedem Runner mit Flag ON).
5. **Readback + X264VideoEncoder + Factory-Anschluss.**
   `nv12_cpu_readback.{h,cpp}`; `x264_video_encoder.{h,cpp}` (implementiert
   `IVideoEncoder` inkl. `ReleaseSlot`-No-op-Semantik); Factory-Zweig hinter
   `EXOSNAP_HAS_X264`. Software-Pfad verweigert `Configure` ehrlich bei
   P010/AYUV-Slot-Formaten (Defense-in-Depth zusätzlich zum Resolver-Gate).
   *Tests:* D10-Stufe 2 (WARP).
6. **Capability + Resolver + Selection-Policy.**
   `SoftwareEncoderFacts` (runtime_snapshot.h, in Cache-Key aufnehmen —
   `capability_cache_key.h` — damit ein Build-Wechsel mit/ohne x264 den Warm-Start-Cache
   invalidiert); `video_codec_backends`-Map + Ableitung der flachen Map
   (`capability_builder.cpp`); **Umbau des gesamten DLL-Gates (`:119–145`)** gemäß D5:
   Codec-Downgrade nur NVENC-Spalte, Bit10-Regel backend-formuliert, und die drei
   `combo_overrides` an „kein Backend (HW oder SW) trägt den Codec" gekoppelt — die
   H.264-Overrides ({Mp4, H264, AacMf}, {Matroska, H264, AacMf}) entfallen bei
   `x264_built`, der AV1-Override bleibt ohne `svt_av1_built` mit backend-neutralem
   Reason-Text; `encoder_selection.{h,cpp}` + `ResolveEncoderBackend`;
   `ToRecorderCoreConfig` stampt das Backend (`translation.cpp`); Backend-Format-Gates
   in `ReconcileOutputFormat` (`resolver.cpp:297ff`) + `SettingsResolver`-Validierung.
   *Tests:* pure Resolver-/Policy-Tests (Auto mit/ohne NVENC, manuelle Wahl unavailable,
   Bit10-Demotion, 444-Snap, HEVC-software-NotImplemented, GPU-lose Maschine ⇒
   `BestAvailableVideoCodec == H264`); **Combo-Gate-Matrix:** GPU-los + `x264_built` ⇒
   `QueryCombo(Mp4, H264, AacMf) == Available` und
   `QueryCombo(Matroska, H264, AacMf) == Available` (Resolver + OptionQuery geben den
   Pfad frei), `QueryCombo(Matroska, Av1, AacMf) == NotImplemented` mit
   backend-neutralem Reason; GPU-los ohne x264 ⇒ alle drei Overrides aktiv wie heute.
7. **UI + Diagnostics + Kontext.**
   Expert-Control „Encoder" (ConfigPage + OutputSettingsModel + OptionQuery-Erweiterung
   `GetEncoderBackendOptions`); NVENC-Preset-Control disabled bei Software;
   `videoEncoderBackendLabel` (CodecLabels.h + `VisibleEncoderBackendLabel` in
   codec_selection); rec.003-Fix über `ResolveEncoderBackend` formulieren
   (`RecommendationEngine.cpp:196–214`); neue Notice `rec.encoder.software` (+ Aufnahme in
   `AllRecommendationIds`, `RecommendationEngine.cpp:751`); Encoder-Karte CPU-Tag;
   `SelfTestRunner::CheckEncoderAvailability`-Erweiterung; `MainWindow.cpp:1554`
   Backend-String; `SessionStats::video_encoder_backend`; DevicePage-Umbau (D7).
   *Tests:* Widget-Tests für Optionslisten/Disabled-Reasons; RecommendationEngine-Tests
   (Notice feuert nur bei Software; Blocker-Verhalten ohne NVENC mit/ohne x264-Build);
   Visual-Test-Snapshot DevicePage.
8. **Doku-Sweep (user-sichtbares Verhalten).**
   `docs/product-spec.md`: §3-Tabelle (+Encoder-Zeile „Auto"), §6 (Backend-Modell,
   Software-Mapping-Absatz — „CRF" bleibt unsichtbar, Formulierung prüfen), §8
   (Blocker-Absatz: ohne NVIDIA-Hardware läuft Software-Encoding mit Notice; Blocker nur
   noch, wenn *kein* Backend existiert, d. h. Build ohne x264), Systemanforderungen
   (NVIDIA „recommended for hardware encoding" statt „required"); `KNOWN_LIMITATIONS.md`
   (Release-Zeitpunkt); `docs/roadmap.md`-Häkchen; README-Anforderungen.
   **Dazu ADR 0007 selbst amendieren** (dieser Wave implementiert ihn):
   (a) Status-Zeile `:5` „scheduled for 0.8.0" → umgesetzt in 0.11, (b) Ordering-Absatz
   `:48` (0.8.0/0.9.0/0.10.0) auf die reale Roadmap 0.11/0.12/0.13 (`docs/roadmap.md:85–87`),
   (c) die `EncoderDiagnosticsAdapter`-Klausel `:64–65` durch den Pipeline-Health-Pfad
   ersetzen (D4/D6; Adapter bleibt Vorgabe für 0.12/0.13). Querverweise auf ADR 0044
   (Audit) und ADR 0045 (Achsen-Trennung) eintragen.
   *Tests:* n/a (Doku), aber Review gegen die tatsächlich gelandeten Schritte.
9. **E2E/CI-Ausbau.** D10-Stufe 3 (#186-Erweiterung); CI-Matrix: ein Job mit
   `EXOSNAP_WITH_X264=ON`, der die neuen Suiten fährt; Verifikation, ob der vendored
   avcodec H.264 dekodiert (sonst Follow-up-Issue ffmpeg-build r4).
   *Tests:* der Schritt *ist* der Test.
10. **Lizenz-/Patent-Audit + Release-Flip.** ADR 0044 (D8) recherchieren und zur
    Entscheidung vorlegen; nach Accepted: `EXOSNAP_WITH_X264` Default `ON`,
    Release-Checklist-Punkt „x264-Quelloffer-Link im Release-Text", KNOWN_LIMITATIONS-
    Absatz final. **0.11 shippt x264 nur mit diesem Schritt; ohne ihn bleibt alles
    dunkel eingebaut (CI-only).**
11. **(Optional, nach 1–10) SVT-AV1** gemäß D9 hinter `EXOSNAP_WITH_SVT_AV1`,
    inkl. Charakterisierungs-Messung (Encode-FPS bei 1080p60/1440p60 auf der
    Dev-Maschine, Binary-Größen-Delta) als dokumentierte Zahlen im PR.

**Sequenzierung gegen parallele Wellen:** Schritt 2 berührt exakt die Encode-Submission,
die die M-1-Spec (async NVENC) umbauen wird. Schritte 1–2 sollen **vor** der
M-1-Umsetzung landen, damit M-1 gegen `IVideoEncoder` statt gegen `NvencVideoEncoder`
entworfen wird; falls M-1 zuerst landet, muss die Factory deren geänderte
Submit-Semantik übernehmen (Konflikt ist lokal auf video_thread.cpp begrenzt).

---

## Test-/Verify-Plan

### CI-fähig (GPU-los)

- **Pure:** `BuildX264Params`-Mapping (RC-Tabelle, VUI, GOP, Clamps),
  `ApplyColorMetadataToX264`, `ResolveEncoderBackend`-Matrix,
  `ReconcileOutputFormat`-Backend-Gates, Preset-TOML-Roundtrip inkl. fehlendem Key,
  Capability-Ableitung (flache Map == max über Backends), DLL-Gate-Matrix inkl.
  `combo_overrides` (GPU-los ± x264_built ⇒ MP4/MKV+H.264+AAC selectable bzw. gesperrt,
  Reason-Texte backend-ehrlich), Cache-Key-Invalidierung.
- **x264-Kern:** echte Encodes synthetischer NV12-Frames; IDR-Kadenz, forced IDR,
  Annex-B/SPS/PPS-Struktur, PTS-Durchleitung, Flush-Drain, CRF/ABR-Betrieb startet.
- **WARP:** `Nv12CpuReadback` (Muster rein == Muster raus, Stride-Korrektheit),
  `X264VideoEncoder` volles Interface-Protokoll.
- **E2E:** #186-Erweiterung — echte Datei aus echtem x264-Encode, fremdvalidiert
  (libavformat-Demux; Decode-Smoke falls verfügbar); MP4-Remux-Fall.
- **Diagnostics/UI:** RecommendationEngine-Fälle (Notice/Blocker-Matrix),
  OptionQuery-Backend-Liste, Visual-Snapshot DevicePage/Settings-Expert-Row.
- **Packaging:** DLL-Präsenz-Gate an `EXOSNAP_WITH_X264` gekoppelt.

### Nur User-live verifizierbar

- **Echte Software-Aufnahme** auf der Dev-Maschine (1080p60, Desktop + Spiel):
  Datei spielt in VLC/MPC/Windows „Films & TV", Farben/Range korrekt (Limited-Default),
  A/V-Sync, Split-Segmente self-contained. (Live-Aufnahmen sind erlaubt; Datei nie
  committen.)
- **Überlast-Verhalten:** 4K60 Software erzwingen → Encoder-Karte geht auf
  Busy/Bottleneck, Drops als Drops gemeldet, Sustained-Lag-Resync greift, kein Hänger.
- **Auto-Auflösung sichtbar:** Settings zeigt „Auto — using NVENC" auf der Dev-Maschine;
  manueller Wechsel auf x264 → Notice erscheint, NVENC-Preset-Control disabled.
- **GPU-lose Maschine/VM** (falls verfügbar): App startet, kein Blocker, Auto = x264,
  Aufnahme funktioniert — der eigentliche Produktbeweis dieser Welle. Ersatzweise deckt
  CI (WARP/E2E) die Engine-Seite ab; die App-Seite bleibt dann als bekannte Lücke
  dokumentiert.
- **Perf-Gefühl:** CPU-Last/Lüfter während Software-Encode subjektiv vertretbar bei
  1080p60 `veryfast` (Zahlen liefert der `OnEncodeLatency`-Tap in den Karten).

---

## Risiken

1. **MSVC↔mingw-Grenze:** x264-DLL muss mit MSVC-Import-Lib sauber linken (C-ABI — beim
   FFmpeg-Prebuilt bewährt, aber x264 exportiert auch Daten-Symbole wie
   `x264_chroma_format`; im Build-Repo verifizieren).
2. **Readback-Stall:** Map(READ) synchronisiert auf den VideoProcessorBlt desselben
   Slots; bei 4K kann Copy+Map+Encode das 16,6-ms-Budget sprengen. Mitigation: gemessen
   statt geraten (Encoder-Karte), Notice kommuniziert die Grenze; 1-Frame-versetzte
   Map-Pipeline als benannte Folge-Optimierung.
3. **x264-Threads vs. Spiel:** Frame-Threads (auto) konkurrieren mit dem gecaptureten
   Spiel um CPU-Kerne — der Fall „smooth game, stuttery recording" landet ehrlich in den
   vorhandenen DPC-/Pipeline-Checks, aber Erwartungsmanagement gehört in Notice + Doku.
4. **Rename-Churn (Schritt 1)** kollidiert mit jeder parallel offenen Branch, die
   Codec-Enums anfasst. Mitigation: Schritt 1 als erste, schnell gemergte PR der Welle;
   rein mechanisch, compilergeprüft.
5. **M-1-Wechselwirkung** (async NVENC): Submit-Semantik von `IVideoEncoder` könnte sich
   ändern; Sequenzierungs-Hinweis oben. Der Software-Pfad bleibt synchron — die
   Buffered-Semantik des Interfaces deckt beides.
6. **Audit-Ausgang offen:** Fällt ADR 0044 negativ aus, bleibt der Pfad dauerhaft
   CI-only (`OFF` im Release). Der CI-Nutzen (Punkte 1–9) bleibt vollständig erhalten —
   das ist bewusst so geschnitten.
7. **Scenecut-off/CBR-Vereinfachung** könnte in Nischen-Playern anders aussehen als
   NVENC-Output; der Live-Player-Check (User-Verify) ist dafür da.
8. **Capability-Cache-Staleness:** ohne Cache-Key-Erweiterung (Schritt 6) würde ein
   Build-Wechsel mit/ohne x264 alte Warm-Start-Snapshots weiterverwenden — im Schritt
   adressiert, im Review gegenprüfen.

---

## Offene Fragen (echte Produktentscheidungen)

1. **Systemanforderungs-Botschaft:** Wird 0.11 aktiv als „läuft auch ohne NVIDIA-GPU
   (Software-Encoding)" kommuniziert (README/Release-Notes/WinGet-Beschreibung), oder
   bleibt NVIDIA „required" im Wortlaut, bis der Software-Pfad live auf einer GPU-losen
   Maschine verifiziert wurde? (Spec nimmt an: Umformulierung auf „recommended" in
   Schritt 8, Vermarktung erst nach Live-Verify.)
2. **Manueller Backend-Override in 0.11:** Die Spec baut das Expert-Control (Auto/NVENC/
   x264). Alternative wäre Auto-only ohne manuelle Wahl (weniger UI, aber Überlast-Tests
   und der rec.003-Fix brauchen die Wahlmöglichkeit ohnehin). Bestätigen.
3. **SVT-AV1 im offiziellen 0.11-Binary:** einschalten, sobald charakterisiert — oder
   grundsätzlich erst in einer späteren Version? (ADR 0007 verlangt nur die
   Charakterisierung; den Zeitpunkt entscheidet der Maintainer.)
4. **Audit-Risikotoleranz:** Wer zeichnet ADR 0044 ab und welcher Risikorahmen gilt
   (analog ADR 0043: Maintainer-Entscheid mit dokumentierten Quellen)? Soll das Audit
   0.11 blocken oder darf 0.11 notfalls ohne x264 im Release-Binary shippen
   (CI-Nutzen bleibt)?

---

## Adversarialer Review — Ergebnis

Alle vier Einwände wurden gegen den Code/die Docs verifiziert; alle vier bestätigt und
eingearbeitet.

1. **DLL-Gate-Scope (major) — eingearbeitet.** Verifiziert: das Gate läuft
   `capability_builder.cpp:119–145` (Codec-Downgrade + Bit10 + drei `combo_overrides`),
   und `QueryCombo` wendet Overrides unbedingt an (`capability_set.cpp:139–142`). Die
   „abgeleitete flache Map" hätte die Overrides tatsächlich stehen lassen und
   MP4/MKV+H.264+AAC auf der GPU-losen x264-Maschine gesperrt. Ist-Zustand korrigiert;
   D5 um den Gate-Umbau (a/b/c, Override-Kopplung an „kein Backend trägt den Codec")
   erweitert; Schritt 6 + Test-Plan um die Combo-Gate-Matrix ergänzt.
2. **ADR-0007-Widerspruch `EncoderDiagnosticsAdapter` (major) — eingearbeitet.**
   Verifiziert (ADR 0007:64–65). Die Spec benennt den Konflikt jetzt explizit in D4 und
   koppelt die Scope-Grenze („Bewusst NICHT gebaut") an ein ADR-0007-Amendment in
   Schritt 8: Adapter-Klausel wird durch den Pipeline-Health-Pfad + Notice ersetzt,
   Adapter bleibt Vorgabe für 0.12/0.13. Keine Implementierung gegen offenen ADR mehr.
3. **ReleaseSlot-Fehlerpfade (minor) — eingearbeitet.** Verifiziert per rg: 11 Aufrufe
   (6 CFR + 5 VFR: zusätzlich 2962, 2975, 3006, 3015, 3047). Ist-Zustand, D2 und
   Schritt 2 auf elf korrigiert; Schritt 2 verlangt jetzt den Vollständigkeitsnachweis
   (`rg 'nvenc\.'` == 0 Treffer nach Umstellung).
4. **Doku-Sweep/ADR-Lücken (minor) — eingearbeitet.** Verifiziert: ADR 0007:5 („0.8.0")
   und :48 (0.8/0.9/0.10) widersprechen `docs/roadmap.md:85–87`. Schritt 8 amendiert
   ADR 0007 jetzt mit (Status, Ordering, Adapter-Klausel); die Achsen-Trennung (D1)
   bekommt einen eigenen Record ADR 0045, geschrieben im Rename-PR (Schritt 1);
   ADR 0044 bleibt der Lizenz-Record.

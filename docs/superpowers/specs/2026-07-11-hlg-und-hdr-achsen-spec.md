# HLG-Ausgabe + Konsolidierung der HDR-Farb-Achsen (Clamping, ACM-Query, Doc-Abgleich)

> Spec-Autor read-only gegen `main @ #192` (2026-07-11). Alle Ist-Zustands-Fakten frisch aus
> dem Code mit Datei:Zeile erhoben. Umsetzung später durch andere Agenten NUR anhand dieser Spec.

## Problem

HDR10 (PQ / BT.2020, 10-bit P010, in-band SEI/OBU + Container-Metadaten) ist geschippt und deckt
Monitor- **und** Fenster/Game-Capture ab. Vier Lücken bleiben aus dem HDR-Farb-Achsen-Audit
(`project_hdr_color_axes_handoff`) und der Roadmap offen:

1. **Kein HLG.** `TransferCharacteristics::AribStdB67 = 18` ist im Modell definiert
   (`color_metadata.h:33`), aber nirgends erzeugt. Es gibt keine HLG-Ausgabeoption, keinen
   HLG-OETF-Pfad, keine HLG-Signalisierung. KNOWN_LIMITATIONS und product-spec sagen ausdrücklich
   „no HLG".
2. **HDR-Achsen-Reconcile lebt im Coordinator, nicht im Resolver.** Die native-HDR-Ableitung
   (10-bit pinnen, 4:4:4 → 4:2:0 snappen, BT.2020/PQ-ColorMetadata setzen) passiert inline in
   `RecordingCoordinator.cpp:763-791`. Seit #190 gehört Format-Policy in den Resolver
   (`libs/capability`). HLG würde diesen Inline-Block ein zweites Mal aufblähen, statt eine reine,
   testbare Resolver-Funktion um einen Fall zu erweitern.
3. **Kein expliziter Windows-ACM-Query.** „Advanced Color aktiv?" wird allein aus dem
   Surface-Format + DXGI-Colorspace abgeleitet (`runtime_query.cpp:325`,
   `dxgi_od_capture_src.cpp:452-457`). Kein `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO`, kein
   `advanced_color_*`-Feld auf `DisplayHdrFacts` — Diagnostics kann den SdrScrgb-Fall nicht
   erklären, und `hdr_active` hängt an einer einzigen DXGI-Ableitung.
4. **Stale HDR-Fakten in Code-Kommentaren.** `user_config.h:34-38` und `translation.cpp:108-111`
   behaupten, `hdr_mode` werde „nur durchgereicht … does not yet derive BT.2020/PQ ColorMetadata
   … still to be wired up". Das ist seit der HDR10-Landung falsch: die Ableitung existiert
   (Coordinator-Seam). `OutputSettingsModel.h:107-108` behauptet „no UI control yet", obwohl
   product-spec ein Expert-HDR-Control als geschippt beschreibt.

Diese Spec legt fest: **native HLG-Ausgabe** als Schwester von HDR10, die **Konsolidierung der
HDR-Achsen in eine reine Resolver-Funktion**, einen **ACM-Query in der Capability/Diagnostics-
Schicht** und den **Doc-Abgleich**. Sie grenzt außerdem ehrlich ab, was das Thema NICHT braucht
(ein HLG→SDR-Ingest-Tonemap) und was nur der User live verifizieren kann.

## Ist-Zustand (mit Datei:Zeile-Referenzen)

### Farbmodell + Signalisierung (schon HLG-fähig, nur ungenutzt)
- `libs/recorder_core/include/recorder_core/color_metadata.h:28-34` — `TransferCharacteristics`
  enthält `SmpteSt2084 = 16` (PQ) **und** `AribStdB67 = 18` (HLG). HLG ist im Enum vorhanden,
  wird nirgends gesetzt.
- `color_metadata.h:66-106` — `ColorMetadata` (primaries/transfer/matrix/range/bits, hdr-Flag,
  MaxCLL/MaxFALL, Mastering-Display-Block). Ein einziger Wahrheitsträger für VUI **und** Container.
- `libs/recorder_core/src/nvenc_encoder.cpp:157-181` — die NVENC-VUI übernimmt
  `color.transfer` per `static_cast<NV_ENC_VUI_TRANSFER_CHARACTERISTIC>(color.transfer)` (AV1 und
  H.264/HEVC-Pfad). **Ergo: transfer=18 fließt ohne Codeänderung in den Bitstream**, sobald die
  ColorMetadata es trägt.
- `libs/recorder_core/src/matroska_stream_writer.cpp:287-288` — schreibt
  `KaxVideoColourTransferCharacter` generisch aus `m_config.color.transfer`. **HLG-Container-Tag
  ist frei.**
- `libs/recorder_core/src/mp4_remuxer.cpp:264-266` — `color_trc` wird nur überschrieben, wenn es
  `UNSPECIFIED`/`RESERVED0` ist; HLG (18) round-trippt aus dem transienten MKV verbatim in die MP4
  `colr`-Box. **HLG-MP4-Signalisierung ist frei.**

### Native-HDR-Erzeugung (heute nur PQ)
- `libs/recorder_core/src/hdr_pq.h:35-218` — reine, unit-getestete PQ-Mathematik
  (`ScrgbToPqNormalized`, `Bt709ToBt2020`, `PqOetf`, `PqRgbToYcbcr`, `QuantizeYcbcr10Limited`,
  `ScrgbToP010`, `PqRgbToP010`). Golden-getestet in `tests/test_hdr_pq.cpp`.
- `libs/recorder_core/src/gpu_hdr_pq.cpp:32-112` — HLSL-Spiegel derselben Formeln
  (`HdrPqConverter`, Luma/Chroma-Pass → P010). `flags.x` unterscheidet scRGB-FP16 (OETF anwenden)
  von „bereits PQ" R10G10B10A2-Desktop.
- `libs/recorder_core/include/recorder_core/hdr_native.h:39-99` —
  `CodecSupportsHdr10Native` (nur HEVC/AV1), `IsHdr10NativeEffective(mode, hdr_active, codec)`,
  `NativeHdr10BitDepthViolation`, `MakeHdr10ColorMetadata(facts)` (setzt primaries=BT2020,
  transfer=SmpteSt2084, matrix=Bt2020Ncl, range=Limited, bits=10, optional Mastering-Display).
- `libs/recorder_core/include/recorder_core/recorder_session.h:490-495` — `ApplyHdr10NativeEncode`
  (reine Funktion: `config.color = MakeHdr10ColorMetadata(facts); config.bit_depth = Bit10;` +
  4:4:4→4:2:0-Snap, gibt `chroma_snapped` zurück).
- `libs/recorder_core/src/video_thread.cpp:866-997` — instanziiert `HdrToneMapper` **und**
  `HdrPqConverter`; wählt pro Session `hdrToneMapActive`/`hdrNativeActive` bzw. `toneMap`/
  `nativeHdr` aus dem `OdCaptureMode`. Der Native-Zweig ruft ausschließlich `HdrPqConverter`.

### Capture-Modus-Auflösung (rein, testbar)
- `libs/recorder_core/include/recorder_core/dxgi_od_capture_src.h:152-193` — `OdCaptureMode`
  {`Sdr`, `SdrScrgb`, `HdrToneMap`, `HdrNative`}; `ResolveOdCaptureMode(...)` und
  `ResolveWgcCapturePlan(...)`.
- `libs/recorder_core/src/dxgi_od_capture_src.cpp:420-475` — Implementierung. `HdrNative` wird
  gewählt bei `hdr_active && hdr_mode == HdrMode::Hdr10 && hdr10_output_supported`. **Der Modus
  kennt nur „native ja/nein", nicht PQ-vs-HLG** — die Transfer-Wahl gehört ohnehin in die
  ColorMetadata, nicht in den Capture-Modus.
- `libs/recorder_core/include/recorder_core/hdr_color_space.h:18-21` — `IsHdrColorSpace` (PQ full
  ODER studio). Eine Definition, geteilt von runtime_query, Capture, Probes.

### HDR-Handling-Enum + Achsen-Durchreichung
- `libs/recorder_core/include/recorder_core/codec_types.h:122-126` — `enum class HdrMode { Off,
  TonemapSdr, Hdr10 }`. Geteilt von `UserRecorderConfig`, `RecorderConfig`, `OutputSettingsModel`.
- `libs/capability/include/capability/user_config.h:26` — `color_range = ColorRange::Limited`
  (Default, „Never gated"); `:38` `hdr_mode = HdrMode::TonemapSdr`.
- `libs/capability/src/translation.cpp:104-112` — trägt `color_range` und `hdr_mode` **unverändert**
  durch. **`hdr_mode` ist NICHT Teil der Combo-Allow-List** (nur container×codec×audio×chroma×depth).
- `libs/capability/include/capability/resolver.h:88-126` + `src/resolver.cpp:297-331` —
  `ReconcileOutputFormat(request)`: 4 statische Regeln (container×codec, 10-bit-Demotion,
  4:4:4-Snap, MP4→CFR). **Keine HDR-Achse.** `CodecSupports10Bit` (HEVC/AV1),
  `CodecSupportsChroma444` (H.264/HEVC).
- `app/services/RecordingCoordinator.cpp:748-757` — Coordinator ruft `ReconcileOutputFormat` nur
  für die CFR-Dimension; `:1877-1911` erneut für die vollen Achsen am Preset-Seam und trägt
  `hdr_mode` explizit nach (`:1911`).
- `app/services/RecordingCoordinator.cpp:763-791` — **inline** Native-HDR-Reconcile: holt
  `FindTargetDisplayFacts(target, RefreshedDisplayFacts())`, prüft `IsHdr10NativeEffective`, ruft
  `ApplyHdr10NativeEncode`, loggt `record.hdr` + evtl. `record.reconcile` (4:4:4→4:2:0).

### Blocker (Codec-Gate)
- `app/diagnostics/RecommendationEngine.cpp:313-375` — `checkHdrH264Blocker`: feuert nur wenn
  `hdr_mode == Hdr10` **und** Codec nicht HDR10-fähig (`caps_.QueryHdr10Native(codec)`) **und**
  `capture_target_hdr_active_`. FixAction „Switch to AV1/HEVC". `rec.hdr.h264` in der
  Blocker-ID-Liste `:752`.

### Display-Fakten / Probe (kein ACM)
- `libs/capability/include/capability/runtime_snapshot.h:18-37` — `DisplayHdrFacts`
  (name, `hdr_active`, `bits_per_color`, Primaries/WP, Luminanz). **Kein `advanced_color_*`.**
- `libs/capability/src/runtime_query.cpp:306-345` — `ProbeDisplays`: `IDXGIOutput6::GetDesc1`;
  `hdr_active = IsHdrColorSpace(d.ColorSpace)`. **Kein `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO`.**
- `libs/recorder_core/include/recorder_core/sdr_white_level.h:12-37` — SDR-White-Level-Logik
  (DISPLAYCONFIG_SDR_WHITE_LEVEL, Fallback 203 nits). Zeigt: der `DisplayConfig`-Query-Weg wird
  bereits genutzt (im Capture-Backend), ist also verfügbar.
- `tools/probes/probe_hdr/src/main.cpp:45-82` — `GetDesc1`-Dump + VideoProcessor-Conversion-Checks.
  Der Audit-Handoff empfahl, diese Probe um GetDesc1-Poll + WM-Logger + ACM zu erweitern.

### Stale-Fakten-Belege (Doc-Abgleich)
- `user_config.h:34-38`, `translation.cpp:108-111` — „does not yet derive … still to be wired up"
  (falsch seit HDR10-Landung).
- `OutputSettingsModel.h:107-108` — „Model only for now — no UI control yet" (widerspricht
  product-spec `docs/product-spec.md:290-303`, die ein Expert-HDR-Control als geschippt beschreibt).
- `KNOWN_LIMITATIONS.md:120,241,333` — „no HLG" / „HLG is not available" / (`:333`) HLG+WCG
  ausdrücklich als bewusst **deferred** gelistet.
- `docs/product-spec.md:333` — „no HLG or wide-gamut generalization beyond BT.2020".
- `docs/product-spec.md:761` — „no HLG/wide-gamut is the confirmed 1.0 scope". **Das ist eine
  bestätigte Produktentscheidung**; HLG-Shipping revidiert sie und braucht User-Sign-off (siehe PR-6).
- `docs/roadmap.md` erwähnt HLG **nirgends** (311 Zeilen, kein Treffer) — die deferred-Notiz lebt
  allein in `KNOWN_LIMITATIONS.md:333`. Ein „roadmap.md-HLG-Edit" wäre ein No-Op (Korrektur ggü.
  einer früheren Spec-Fassung, die `roadmap.md:337` referenzierte — diese Zeile existiert nicht).

## Design

### Kern-Erkenntnis: HLG ist ein AUSGABE-Transfer, kein Ingest-Format

Windows komponiert einen HDR-Desktop **immer** als scRGB-FP16 (linear, BT.709, 1.0 = 80 nits) oder
als bereits-PQ R10G10B10A2 — **nie** als HLG. Es gibt keinen Codepfad, der jemals ein HLG-Signal
aufnimmt (`ResolveOdCaptureMode`/`ResolveWgcCapturePlan` kennen nur BGRA8 / R10G10B10A2 / FP16,
`dxgi_od_capture_src.cpp:416-475`). Daraus folgt direkt:

- **HLG-„Erkennung" am Ingest existiert nicht und ist nicht baubar** — es gibt kein HLG einzulesen.
- **Ein HLG→SDR-Ingest-Tonemap-Pfad wird NICHT gebaut.** Der Brief nennt ihn; er hat aber kein
  Subjekt: Wir lesen nie ein HLG-Signal ein. Der In-App-**Preview-Tap** tappt den scRGB-FP16-
  **Vor-Encode-Frame** (vor der HLG-Kurve) und tonemappt scRGB→SDR, exakt wie heute bei PQ-Native
  (`video_thread.cpp:1932`ff, ADR 0040) — dieser Tap ist HLG-agnostisch. **ABER Vorsicht, zwei
  Monitor-Decode-Pfade sind NICHT agnostisch und werden in PR-5 explizit behandelt:**
  1. **Snapshot-Readback dekodiert das P010-Encode-Surface.** `video_thread.cpp:1931-1940,2045-2049`
     baut `P010PqMonitorConverter` (PQ-EOTF/BT.2020→tonemapped SDR BGRA) und ruft ihn für **jede**
     `hdrNativeActive`-Session. Bei HLG hält das P010 **HLG**-kodierte Daten — ein PQ-EOTF-Decode
     ergibt farbfalsche Snapshot-PNGs. **PR-5 braucht einen HLG-bewussten Monitor-Decode (inverse
     HLG-OETF statt PQ-EOTF)** + Test; Snapshot-Farbtreue ist bereits RELEASE-GATE-Klasse
     (444-Snapshot-PNG-Check, `project_followup_waves_session_2026_07_05`).
  2. **Preview-Tap ist im already-PQ-Fall komplett deaktiviert** (`preview_tap.h:67-69`,
     `pq_input_is_pq → tap_enabled=false`). Für den already-PQ-R10G10B10A2-Desktop gibt es also
     ohnehin keine linear tappbare Fläche; das ist genau der Fall, den Entscheidung G unten
     transcodiert. **Bewusste Nicht-Leistung, ehrlich benannt — aber nur für den FP16-Tap.**

„HLG-Support" heißt damit konkret: eine **native HLG-Ausgabeoption** — scRGB-FP16 wird statt mit der
PQ-OETF mit der **HLG-OETF (ARIB STD-B67)** nach BT.2020/10-bit/P010 kodiert, mit transfer=18 in
VUI + Container. Alles andere (Codec-Gate, 10-bit-Pin, Container/VUI-Signalisierung) ist strukturell
identisch zu HDR10.

### Entscheidung A — HLG als eigener `HdrMode`-Wert vs. Sub-Toggle unter „native HDR"

- **Alt. A1 (empfohlen): eigener Enum-Wert `HdrMode::Hlg`.** Enum wird `{Off, TonemapSdr, Hdr10,
  Hlg}`. Das UI-Control (product-spec `:290-303`) wird ein 3-Wege-Radio: *Tone-map to SDR* /
  *Native HDR10 (PQ)* / *Native HLG*. Das Codec-Gate und der `IsHdrNative*`-Prädikatsatz behandeln
  `Hdr10` und `Hlg` symmetrisch. Vorteile: der Modus ist die einzige Achse, an der PQ-vs-HLG hängt;
  Resolver/Blocker/Gate lesen genau ein Feld; kein „Modus + versteckter Transfer-Zustand".
- **Alt. A2: `Hdr10` bleibt „native HDR", ein separates `hdr_transfer`-Feld (PQ|HLG).** Nachteil:
  zwei gekoppelte Felder, die konsistent gehalten werden müssen; `hdr_transfer` ist bei
  `Off`/`TonemapSdr` bedeutungslos (toter Zustand); jeder Gate-/Blocker-Check muss beide lesen. Das
  ist genau die Art „Modus, dessen Nebenachse niemand bewusst wählt", die die Produktentscheidung
  vom 2026-07-09 (siehe Handoff) ablehnt.

**Entscheidung: A1.** Ein Enum-Wert `Hlg`. Pre-1.0 = keine Migration nötig; ein Preset mit altem
`hdr_mode` bleibt gültig, der neue Wert ist rein additiv.

### Entscheidung B — HDR-Achsen-Reconcile: im Coordinator lassen vs. in den Resolver ziehen

Der Native-HDR-Reconcile (welcher Modus greift, 10-bit pinnen, 4:4:4 snappen, PQ-vs-HLG-Transfer
wählen) ist **runtime-fakten-abhängig** (`facts.hdr_active`), daher **keine** statische
`ReconcileOutputFormat`-Regel. Das ist die Spannung zu „Resolver-Hoheit".

- **Alt. B1: Status quo — Inline im Coordinator, reine `ApplyHdr10NativeEncode` aus der Engine.**
  Der Coordinator plumbt Fakten und ruft eine reine Funktion. Für PQ allein vertretbar. **Mit HLG
  bricht es:** der Inline-Block (`:763-791`) müsste PQ-vs-HLG verzweigen und den Transfer wählen —
  Policy im Coordinator, die #190 gerade aus ihm herausgezogen hat. H-6-Regressions-Risiko.
- **Alt. B2 (empfohlen): eine reine, fakten-parametrisierte Resolver-Funktion in `libs/capability`.**
  Neu: `HdrAxisReconciliation ReconcileHdrAxes(const UserRecorderConfig&, const DisplayHdrFacts&)`
  (oder `recorder_core::HdrDisplayFacts` via bestehendem `ToHdrDisplayFacts`,
  `translation.h:21-35`). Rückgabe: `{engaged, transfer (PQ|HLG|none), bit_depth_pinned,
  chroma_snapped, ColorMetadata}` + Adjustment-Gründe. `libs/capability` hängt bereits an
  `recorder_core` (`translation.h` inkludiert `hdr_native.h`), darf also `MakeHdr10ColorMetadata` /
  `MakeHlgColorMetadata` aufrufen — die OETF/Metadaten-**Mathematik bleibt in der Engine**, nur die
  **Policy-Entscheidung** zieht in den Resolver. Der Coordinator wird auf `if (recon.engaged)
  ApplyHdrAxes(config, recon); log(...)` reduziert; HLG ist dann **ein zusätzlicher Zweig in EINER
  reinen Funktion mit eigenem Golden-Test**, nicht ein zweiter Inline-Block.

**Entscheidung: B2.** Erst konsolidieren (verhaltensidentisch für PQ), dann HLG als einen Fall
addieren. Das ist genau der „Messinfra/Struktur vor Feature"-Zug.

Ehrliche Grenze der Konsolidierung: `ReconcileHdrAxes` bleibt **runtime-abhängig** und ist damit
KEINE `ReconcileOutputFormat`-Regel. Die statische Combo-Allow-List in `translation.cpp:82-96`
bleibt unverändert HDR-frei — HDR-Native ändert Bit-Tiefe/Chroma **nach** der statischen Validierung
auf Basis der Live-Fakten. Das ist korrekt und wird so dokumentiert, nicht „behoben".

### Entscheidung C — Welches „Clamping" der HDR-Achsen ist legitim?

Der Audit empfahl, `hdr_mode == Hdr10 + H.264` im Preset zu clampen. Die **Produktentscheidung
(2026-07-09, Handoff) lehnt das ab**: die Kombination erzeugt **keinen falschen Output** (auf
HDR-Displays stoppt `rec.hdr.h264` und bietet Codec-Wechsel; auf SDR greift Native nie), und ein
stiller Clamp würde den Blocker aushebeln und die HDR-Absicht wortlos verwerfen. Diese Spec **hält
daran fest** und erweitert die Regel auf HLG:

- **`hdr_mode` (Hdr10/Hlg) wird NIE still auf Basis des Codecs umgeschrieben.** Der Blocker
  `rec.hdr.h264` ist der Wahrheitsträger und wird auf `Hlg` generalisiert.
- **`color_range` wird weiterhin nie geclampt** (`user_config.h:25` „Never gated"). Native HDR
  (PQ **und** HLG) **pinnt** intern `range = Limited` (in `MakeHdr10/HlgColorMetadata`), überschreibt
  also die User-Wahl nur für den nativen Pfad — das ist Format-Wahrheit (HDR10/HLG sind narrow-range),
  kein Clamp der gespeicherten Präferenz. Dokumentieren, nicht „reparieren".
- **Was `ReconcileHdrAxes` tatsächlich reconciled** (nur wenn Native **greift**, d.h. Display
  HDR-aktiv + Codec HDR-fähig): `bit_depth → Bit10`, `chroma 4:4:4 → 4:2:0`, `transfer → PQ|HLG`,
  ColorMetadata. Das ist Ableitung aus Live-Fakten, kein Preset-Clamp.

Nettoergebnis: die HDR-Achsen sind **konstruktiv konsistent**; es gibt keinen persistierten
Zustand, der still falschen Output erzeugt. Die Spec macht diese Invariante explizit + testbar,
statt einen scheinbaren „Clamp" zu bauen, den das Produkt gar nicht will.

### Entscheidung D — HLG-OETF-Normalisierung (nominaler Peak)

HLG (ARIB STD-B67 / BT.2100) ist **relativ/szenen-referenziert**: die OETF bildet normalisiertes
Szenen-Linearlicht `E ∈ [0,1]` auf Signal `E' ∈ [0,1]` ab. scRGB ist **absolut** (1.0 = 80 nits).
Die Abbildung braucht einen **nominalen Peak** `P` nits: `E = clamp(scrgb_linear · 80 / P, 0, 1)`.

- **Alt. D1 (empfohlen): fester nominaler Peak 1000 nits.** Deterministisch, GPU-unabhängig, ein
  Golden-Vektor. **BT.2100/BT.2408-konform**: HLG ist mit einem nominalen Peak von 1000 cd/m²
  (System-Gamma 1.2) definiert; Referenzweiß (203 nits, `kDefaultSdrWhiteLevelNits`,
  `sdr_white_level.h`) landet damit bei ~70 % HLG-Signal, nahe dem BT.2408-Graukarten-/Weiß-Anker
  von 75 %. Der Nachteil („entspricht nicht jedem Panel-Peak") ist für ein **szenen-referenziertes
  Liefer**format gerade **kein** Nachteil: HLG ist absichtlich display-relativ, der HLG-fähige Player
  wendet die OOTF passend zum Ausgabegerät an.
- **Alt. D2 (verworfen): adaptiver Peak = gemeldeter Display-Peak, Fallback 1000 nits.** Die
  Analogie zu `HdrPeakScale` ist **invertiert**: dort ist der Display-Peak der **Quell**-Peak eines
  Highlight-Rolloffs beim Tonemapping (HDR→SDR); beim HLG-Encode ist der nominale Peak dagegen die
  **Ziel**-Signalnormalisierung `E = clamp(scrgb_linear·80/P, 0, 1)`. Konsequenz: bei P=1000 landet
  Referenzweiß bei ~70 %, bei einem 4000-nit-EDID-Panel aber bei ~39 % — **dieselbe Szene würde je
  nach Aufnahme-Monitor drastisch dunkler kodiert**, und HLG trägt keine Metadaten (MDCV/CLL bewusst
  weg, Entscheidung E), mit denen ein Player das kompensieren könnte. D2 backt das Aufnahme-Panel
  unwiderruflich in die Datei. Falsch für ein szenen-referenziertes Format.

**Entscheidung: D1** — fester nominaler Peak **1000 nits** (BT.2408-konform, deterministisch,
reproduzierbarer Look über alle Aufnahme-Panels). Die OETF wird **direkt** angewandt (keine explizite
OOTF/System-Gamma-Stufe im Encode) — ein bewusster „scene-referred pass-through"-Encode; die OOTF
gehört auf die Anzeigeseite (HLG-Player/TV). Ob der Direkt-OETF-Encode auf einem HLG-TV natürlich
aussieht, bleibt **nur live verifizierbar** (siehe Risiken + Verify). Die Mathematik wird — wie PQ —
als reiner `hdr_hlg.h`-Header golden-gepinnt und im HLSL-Shader bit-genau gespiegelt. Ein
panel-adaptiver Peak (D2) käme höchstens später mit **Clamp + expliziter Produkt-Freigabe** infrage,
nicht als Default.

HLG-Konstanten (BT.2100): `E' = sqrt(3E)` für `0 ≤ E ≤ 1/12`, sonst `E' = a·ln(12E − b) + c` mit
`a = 0.17883277`, `b = 0.28466892`, `c = 0.55991073`. Matrix bleibt BT.2020 NCL, Quantisierung
identisch zu PQ (`QuantizeYcbcr10Limited`).

### Entscheidung E — Mastering-Display / MaxCLL für HLG

HLG ist display-relativ; MDCV (SMPTE ST 2086) und MaxCLL sind PQ/HDR10-Konzepte. HLG-Streams tragen
sie konventionell **nicht**. **Entscheidung:** `MakeHlgColorMetadata` setzt `has_mastering_display =
false` und lässt MaxCLL/MaxFALL bei 0 — der In-band-Emitter (`ShouldEmitHdrBitstreamMetadata`,
`hdr_bitstream_metadata.h:64`) feuert dann für HLG nichts, exakt wie gewünscht. Nur primaries=BT2020,
transfer=AribStdB67, matrix=Bt2020Ncl, range=Limited, bits=10 werden signalisiert (VUI + Container).

### Entscheidung F — ACM-Query: Verhalten ändern vs. Diagnose anreichern

- **Alt. F1: ACM als neue Wahrheitsquelle für `hdr_active` / Capture-Entscheidung.** Riskant: die
  Capture-Format-Entscheidung MUSS aus dem **echten ersten Frame** kommen
  (`dxgi_od_capture_src.h:135-142` — ModeDesc lügt), nicht aus einer Config-API. Verhalten ändern =
  Regressionsrisiko am heikelsten Pfad.
- **Alt. F2 (empfohlen): ACM rein additiv als Diagnose-/Erklärungsfeld.**
  `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO` in `ProbeDisplays` ergänzen; neue Felder auf
  `DisplayHdrFacts` (`advanced_color_supported`, `advanced_color_enabled`, `wide_color_enforced`,
  ggf. `color_encoding`/`bits_per_color_channel`). **Kein** Einfluss auf `hdr_active` oder die
  Capture-Wahl. Nutzen: Diagnostics kann den SdrScrgb-Fall **ruhig erklären** („Advanced Color ist
  an; SDR-Desktop wird als scRGB komponiert und mit sRGB-Transfer aufgenommen") und eine belastbare
  zweite Quelle liefern, falls DXGI-Colorspace und ACM divergieren (dann eine **Notice**, kein
  Blocker — diagnostics-calm).

**Entscheidung: F2.** ACM erklärt und ergänzt, ändert nichts am Capture-Verhalten.

### Entscheidung G — Bereits-PQ-Input (R10G10B10A2-Desktop) unter HLG

**Das ist die kritische Achse, ohne die HLG falsch etikettierte Dateien erzeugt.** Ein 10-bpc-
HDR-Desktop (typisch Monitor-/OD-Capture) komponiert **bereits als PQ-kodiertes** R10G10B10A2
(`dxgi_od_capture_src.cpp:426-437`); für diesen Input hat der `HdrPqConverter` **keinen** OETF-
Schritt, sondern reicht die PQ-Pixel unverändert durch (`gpu_hdr_pq.cpp:71-74`, `flags.x`-Zweig
„already PQ-encoded"). Würde PR-5 `HdrMode::Hlg` **identisch** zu `Hdr10` auf `HdrNative` abbilden,
liefe der already-PQ-Desktop durch diese Passthrough — Ergebnis: **PQ-Pixel mit transfer=18 (HLG)
signalisiert = objektiv falsche Datei.** „Nur die `EncodedRgb`-OETF verzweigt" (PR-4) reicht hier
gerade **nicht**, weil dieser Pfad heute überhaupt keine OETF anwendet.

Der scRGB-FP16-Input (Fenster/WGC über `ResolveWgcCapturePlan`, und der FP16-Monitorfall) hat einen
linearen Zwischenschritt und ist der **native HLG-Pfad**: scRGB→linear→BT.2020→HLG-OETF. Der
already-PQ-R10G10B10A2-Input hat keinen. Zwei Optionen:

- **Alt. G1: Transcode PQ→HLG.** Im already-PQ-Zweig (statt Passthrough) für HLG:
  `PqEotf → absolutes Linearlicht (nits) → nominal-peak-Normalisierung (D1, 1000 nits) → HLG-OETF`.
  Neue, aber **bounded** Mathematik (die PQ-Konstanten liegen im Shader schon vor; nur inverse PQ +
  HLG-OETF kommen hinzu), golden-testbar. Vorteil: der native HLG-Wunsch wird auf Monitor-Capture
  **erfüllt**, die Datei ist immer korrekt HLG-etikettiert, kein stiller SDR-Downgrade. Semantik der
  PQ-absolut→HLG-relativ-Abbildung: PQ liefert absolute cd/m² (bis 10000), auf `P=1000` normalisiert
  und geclampt — dieselbe D1-Konstante wie der FP16-Pfad, damit beide Input-Wege identisch mappen.
- **Alt. G2: Nicht nativ — auf `HdrToneMap` ausweichen.** `R10G10B10A2 && Hlg` fällt auf
  `HdrToneMap` (SDR) zurück, exakt wie „Hdr10 auf H.264" heute (`dxgi_od_capture_src.cpp:452-456`).
  Nie falsch etikettiert, aber ein **stiller SDR-Downgrade** genau da, wo HDR10 heute nativ liefert
  (Monitor-HDR-Capture) — eine überraschende Asymmetrie ggü. HDR10.

**Entscheidung: G1 (Transcode).** Der already-PQ-Zweig transcodiert für HLG statt durchzureichen;
der FP16-Zweig kodiert direkt. Beide nutzen die D1-Konstante (1000 nits), sind also golden-
verzahnt. Damit ist HLG auf Monitor- **und** Fenster-Capture native und **nie** eine mislabeled
PQ-Datei. `OdCaptureMode` bleibt unverändert `HdrNative` für beide Inputs — die PQ-vs-HLG- **und**
FP16-vs-already-PQ-Verzweigung sitzt im Konverter/Shader, gesteuert durch `config.color.transfer`
+ dem bestehenden `flags.x` (inputIsPq). **Test:** Golden-Vektor für den PQ→HLG-Transcode (Knee +
Referenzweiß + Peak) in `test_hdr_hlg.cpp`; WARP-Parität; und ein `ResolveOdCaptureMode`-Test, der
belegt, dass `R10G10B10A2 + Hlg + hdr_active + native_supported` **kein** ungetranscodetes
Passthrough erzeugt (transfer der Ausgabe == 18, Pixel-Pfad == Transcode-Shader, nicht Passthrough).

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit Testansatz. Reihenfolge ist bindend: Doc-Destale und
ACM sind unabhängig und können parallel/vorab; die Konsolidierung (PR-3) muss **vor** dem
HLG-Feature (PR-5) landen.

### PR-1 — Stale Code-Kommentare entschärfen (reine Kommentar-/Doc-Änderung, kein Verhalten)
- `libs/capability/include/capability/user_config.h:34-38` und
  `libs/capability/src/translation.cpp:108-111`: den „does not yet derive … still to be wired up"-
  Text durch die Realität ersetzen (ColorMetadata für Native-HDR wird am Reconcile-Seam aus Live-
  Display-Fakten abgeleitet; siehe `ReconcileHdrAxes`/Coordinator).
- `app/models/OutputSettingsModel.h:107-108`: „no UI control yet" gegen den tatsächlichen Zustand
  prüfen (Expert-HDR-Control lt. product-spec) und korrigieren.
- **Test:** keiner nötig (Kommentare); Build-Grün genügt.

### PR-2 — ACM-Query in die Capability/Diagnostics-Schicht (unabhängig)
- `libs/capability/include/capability/runtime_snapshot.h`: `DisplayHdrFacts` um
  `bool advanced_color_supported`, `bool advanced_color_enabled`, `bool wide_color_enforced`
  (Defaults false) erweitern.
- `libs/capability/src/runtime_query.cpp` `ProbeDisplays` (`:306-345`): pro Display
  `DisplayConfigGetDeviceInfo` mit `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO` aufrufen (Adapter/Source-
  ID über `QueryDisplayConfig`/`GetDisplayConfigBufferSizes` auflösen). **Kein Laufzeit-
  `GetProcAddress`- und kein `__has_include`-Guard nötig:** `DisplayConfigGetDeviceInfo` gibt es seit
  Win7 und wird bereits direkt aufgerufen (`dxgi_od_capture_src.cpp:89,101`, SDR-White-Level-Weg);
  `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO` ist ein SDK-Enum/-Struct in `wingdi.h` (Build-SDK-Frage,
  kein separates Header-File, das `__has_include` fände). Der Win10<1709-Fall ist ohnehin
  gegenstandslos — Mindest-OS ist Win10/11 (`README.md:28`) und WGC verlangt ≥1903. **Guard =
  einfach: Aufruf machen, bei != `ERROR_SUCCESS` die Felder false lassen** (graceful degrade wie
  überall im Probe, exakt wie der SDR-White-Level-Query es tut).
- Diagnostics: eine **ruhige Notice** (kein Blocker), wenn `advanced_color_enabled && !hdr_active`
  den SdrScrgb-Fall erklärt, bzw. wenn ACM und DXGI-Colorspace unerwartet divergieren. In
  `app/diagnostics/*` bzw. der Display-Facts-Anzeige (`app/pages/DiagnosticsPage.cpp`, `Device`-Tab
  falls dort die Display-Facts sitzen) rein informativ zeigen.
- `tools/probes/probe_hdr/src/main.cpp`: ACM-Ausgabe pro Display ergänzen (advancedColorSupported/
  Enabled/wideColorEnforced/colorEncoding/bitsPerColorChannel) — Audit-Handoff-Punkt.
- **Test (CI):** reine Helper (z.B. `AcmToNotice(facts)` — SdrScrgb-Erklärung ja/nein) unit-testen;
  die `DisplayConfig`-Roh-Query selbst ist HW/OS-abhängig → **nur live** (siehe Verify).

### PR-3 — HDR-Achsen-Reconcile in eine reine Resolver-Funktion ziehen (verhaltensidentisch, PQ)
- Neu in `libs/capability` (z.B. `hdr_axis_reconcile.{h,cpp}`):
  `struct HdrAxisReconciliation { bool engaged; recorder_core::TransferCharacteristics transfer;
  bool bit_depth_pinned_10; bool chroma_snapped_420; recorder_core::ColorMetadata color;
  std::vector<Adjustment> adjustments; };`
  und `HdrAxisReconciliation ReconcileHdrAxes(const UserRecorderConfig&, const
  recorder_core::HdrDisplayFacts&)`. Ruft `IsHdr10NativeEffective` + `MakeHdr10ColorMetadata`
  (Transfer PQ). Reihenfolge/Semantik 1:1 aus `RecordingCoordinator.cpp:763-791`.
- `RecordingCoordinator.cpp:763-791` durch Aufruf dieser Funktion ersetzen; Logging (`record.hdr`,
  `record.reconcile`) bleibt im Coordinator (UI/Diagnostics-nah).
- `ApplyHdr10NativeEncode` (`recorder_session.h:490`) bleibt als reine Engine-Metadaten-Primitive
  bestehen und wird von `ReconcileHdrAxes` genutzt (oder deren Logik zieht dorthin um) — kein
  doppelter Reconcile.
- **Test (CI):** die neuen `ReconcileHdrAxes`-Unit-Tests sind die **eigentliche
  Verhaltensidentitäts-Absicherung** (Matrix: {SDR-Display, HDR-Display} × {Hdr10, TonemapSdr, Off}
  × {HEVC, AV1, H264} × {4:2:0, 4:4:4} × {8, 10-bit}). **Klarstellung:**
  `tests/test_hdr_native_reconcile.cpp` testet **nur** die reine Engine-Primitive
  `ApplyHdr10NativeEncode` (`#include recorder_session.h`, keine Coordinator-/`RefreshedDisplayFacts`-
  Verdrahtung) und bleibt daher trivial grün, selbst wenn die Reconcile- **Entscheidung**
  (`IsHdr…NativeEffective` + Fakten-Plumbing, `RecordingCoordinator.cpp:763-791`) bricht. Es ist also
  **kein** „Coordinator-Seam-Test" und deckt die Konsolidierung nicht ab — die neuen
  `ReconcileHdrAxes`-Tests treten als **Ersatz** für diese Absicherung an, nicht als bloßer Zusatz.
  `test_hdr_native_reconcile.cpp` bleibt zusätzlich grün (unveränderte Primitive).

### PR-4 — HLG-Ausgabe-Mathematik (rein + Shader-Spiegel)
- Neu `libs/recorder_core/src/hdr_hlg.h`: HLG-OETF (BT.2100-Konstanten), `ScrgbToHlgNormalized`
  (fester nominaler Peak 1000 nits, **D1**), `Bt709ToBt2020` wiederverwenden, `ScrgbToHlgP010`
  (analog `ScrgbToP010`). **Zusätzlich (Entscheidung G): `PqToHlgNormalized`** — der already-PQ-
  R10G10B10A2-Transcode: `PqEotf` (inverse PQ, absolutes Linearlicht in cd/m²) → auf D1=1000 nits
  normalisieren+clampen → HLG-OETF. Beide Wege (FP16 und already-PQ) münden über dieselbe D1-
  Konstante in dasselbe HLG-Signal.
- Neu `libs/recorder_core/src/gpu_hdr_hlg.{h,cpp}` **oder** `HdrPqConverter` um eine Transfer-Auswahl
  erweitern (bevorzugt: **ein** Konverter mit Transfer-Auswahl, um Luma/Chroma-Pass + P010-Packing
  nicht zu duplizieren). Der `EncodedRgb`-Zweig verzweigt dann **zweidimensional**: `flags.x`
  (inputIsPq) × Transfer (PQ|HLG) — die neue Zelle ist der PQ→HLG-Transcode (G1), die den bisherigen
  „already PQ-encoded → Passthrough"-Fall für HLG **ersetzt** (Passthrough bleibt nur für PQ). HLSL
  bit-genau zur `hdr_hlg.h`-Referenz.
- **Test (CI):** Golden-Vektoren `tests/test_hdr_hlg.cpp` (hand­gerechnete E'-Werte an den Knee-
  Punkten 1/12, reference-white, peak) **für beide Wege**: scRGB→HLG **und** PQ→HLG-Transcode (G);
  WARP-Shader-Parität analog `tests/test_gpu_hdr_tonemap.cpp` / `test_gpu_compositor.cpp` (CPU-
  Referenz == GPU bit-genau, headless via WARP). Ein Golden-Vektor beweist zusätzlich, dass PQ-Input
  unter HLG **nicht** durchgereicht wird (Transcode-Output ≠ Passthrough-Input am Referenzweiß).

### PR-5 — HLG-Verdrahtung (Enum, Gate, Reconcile, video_thread, UI)
- `codec_types.h:122-126`: `HdrMode` um `Hlg` erweitern.
- `hdr_native.h`: `CodecSupportsHdr10Native` → generisch `CodecSupportsHdrNative` (identische
  Codecs, HEVC/AV1); `IsHdr10NativeEffective` → `IsHdrNativeEffective(mode, hdr_active, codec)`
  (true für Hdr10 **oder** Hlg); neue `MakeHlgColorMetadata(facts)` (transfer=AribStdB67, kein
  Mastering-Block, Entscheidung E). Alte Namen als deprecating-Alias oder pre-1.0 hart umbenennen
  (keine Back-Compat nötig).
- `ReconcileHdrAxes` (PR-3): Transfer-Auswahl PQ vs HLG aus `hdr_mode`; ruft `MakeHlgColorMetadata`
  für `Hlg`.
- `dxgi_od_capture_src.cpp:420-475` `ResolveOdCaptureMode`/`ResolveWgcCapturePlan`: `hdr_mode ==
  Hlg` genauso wie `Hdr10` auf `HdrNative` abbilden (`hdr10_output_supported` → `hdr_native_supported`
  umbenennen; gleiches Codec-Gate). **`OdCaptureMode` bleibt unverändert** — der Modus ist „native
  ja", die Transfer-Wahl reist in der ColorMetadata, nicht im Capture-Modus.
- `video_thread.cpp:866-997`: im `nativeHdr`-Zweig zwischen PQ- und HLG-Konverter anhand
  `config.color.transfer` wählen (PR-4-Konverter); für den already-PQ-Input unter HLG greift der
  Transcode-Zweig (Entscheidung G), nicht das Passthrough.
- **HLG-bewusster Monitor-Decode (Snapshot).** `video_thread.cpp:1931-1940,2045-2049` baut den
  `P010PqMonitorConverter` (PQ-EOTF/BT.2020→tonemapped SDR BGRA) für **jede** `hdrNativeActive`-
  Session — bei HLG hält das P010 HLG-Daten, ein PQ-EOTF-Decode ist farbfalsch. Den Monitor-Decode
  transfer-bewusst machen: bei `config.color.transfer == AribStdB67` einen **inverse-HLG-OETF**-
  Decode (statt PQ-EOTF) verwenden, dann wie gehabt tonemappen. Betrifft den Snapshot-Readback (der
  Live-Preview-**Tap** ist HLG-agnostisch, s. Design; der already-PQ-Tap ist ohnehin deaktiviert,
  `preview_tap.h:67-69`). Dieser Decode ist RELEASE-GATE-relevant (444-Snapshot-PNG-Farbtreue).
- `RecommendationEngine.cpp:313-375` `checkHdrH264Blocker`: Guard `hdr_mode != Hdr10` →
  `hdr_mode != Hdr10 && hdr_mode != Hlg` (bzw. `!IsHdrNativeMode(hdr_mode)`); Text neutral
  („HDR10/HLG"). `caps_.QueryHdr10Native` bleibt korrekt (gleiche Codec-Menge).
- UI: `OutputSettingsModel` + das Expert-HDR-Control (product-spec `:290-303`) auf 3-Wege
  (Tone-map SDR / HDR10 (PQ) / HLG) erweitern; HLG unter demselben AV1/HEVC-Gate + ruhiger
  Inline-Note wie HDR10 bei H.264.
- **Test (CI):** Blocker-Test (`app/tests/test_diagnostics.cpp`) um Hlg-Fall erweitern;
  `ReconcileHdrAxes`-Tests um Hlg-Zeilen; `test_nvenc_color_config.cpp` um transfer=18-VUI-Assert;
  Matroska-Writer-Test um HLG-transfer-Tag (MKV **und** WebM-DocType, s. Container-Risiko);
  `ResolveOdCaptureMode`-Test, dass `R10G10B10A2 + Hlg` den Transcode-Pfad (nicht Passthrough) nimmt;
  ein Unit-Test des HLG-bewussten Snapshot-Monitor-Decodes (inverse HLG-OETF, headless über die reine
  Decode-Funktion / CPU-Referenz); ein `--visual-test`-Render des 3-Wege-Controls.

### PR-6 — Doc-Abgleich (mit dem Feature)
- `KNOWN_LIMITATIONS.md:120,241,333` (geprüfte Zeilen — **nicht** 120/242/337): „no HLG" → HLG als
  geschippt beschreiben (native HLG-Ausgabe, BT.2020/10-bit/P010, transfer=arib-std-b67, HEVC/AV1,
  gleicher H.264-Blocker; MKV **und** WebM/MP4); Grenzen (kein MDCV/CLL für HLG, Direkt-OETF ohne
  explizite OOTF, kein HLG-Ingest, PQ→HLG-Transcode für already-PQ-Monitor).
- `docs/product-spec.md:290-333`: HDR-Handling-Control auf 3 Optionen; HLG-Verhalten spiegeln;
  „no HLG" (`:333`) streichen.
- `docs/product-spec.md:761`: „no HLG/wide-gamut is the confirmed 1.0 scope" — **muss ebenfalls
  angepasst werden.** Da diese Zeile einen **bestätigten** 1.0-Scope dokumentiert, revidiert
  HLG-Shipping eine Produktentscheidung: **User-Sign-off erforderlich, bevor PR-6 landet** (in der
  Spec explizit als Gate festgehalten, s. Offene Fragen).
- `docs/roadmap.md`: **kein Edit** — roadmap.md erwähnt HLG nirgends (die deferred-Notiz sitzt in
  `KNOWN_LIMITATIONS.md:333`). Der frühere „roadmap.md aus deferred nehmen"-Schritt war ein No-Op und
  entfällt.
- ADR (`docs/decisions/`, **nicht** `docs/adr/`): `0032-color-management-foundation.md` um die
  HLG-Ausgabe ergänzen **oder** eine neue ADR „Native HLG output transfer" (fester nominaler Peak
  **D1=1000 nits**, kein MDCV E, kein Ingest-Tonemap, PQ→HLG-Transcode G) — die Peak-/OOTF-
  Entscheidung ist ADR-würdig.
- **Test:** keiner (Docs); Konsistenz-Review mit CodecLabels-Kanon (sichtbare Schreibweise „HLG").

## Test-/Verify-Plan

### CI-fähig (headless / WARP / rein)
- `ReconcileHdrAxes`-Achsenmatrix (PR-3, PR-5) — reine Funktion, keine HW.
- `hdr_hlg.h` Golden-Vektoren + WARP-Shader-Parität (PR-4) — wie die bestehenden `test_hdr_pq` /
  `test_gpu_hdr_tonemap`.
- NVENC-VUI transfer=18 (`test_nvenc_color_config.cpp`) + Matroska-transfer-Tag (Writer-Test) —
  bestätigt HLG-Signalisierung ohne HW.
- Blocker-Generalisierung Hlg (`test_diagnostics.cpp`).
- ACM-Notice-Helper unit (PR-2).
- 3-Wege-Control-Render via `--visual-test`.

### Nur User-live (echte NVIDIA-HW + HDR/HLG-Panel) — RELEASE-GATE-Klasse
- **HLG-Signalisierung auf echter Aufnahme:** `ffprobe` zeigt `color_transfer=arib-std-b67`,
  `color_primaries=bt2020`, `color_space=bt2020nc` für MKV **und** remuxtes MP4 (colr-Box). Nur auf
  HW bit-beweisbar — reiht sich in die bestehende **SEI-RELEASE-GATE**-Liste ein
  (`project_followup_waves_session_2026_07_05`, Punkt 2): HDR10-ffprobe-SEI-Check (#137, raw-payload-
  Semantik), 444-Snapshot-PNG-Farbtreue (#142), avcC-Trailer via ffprobe/MP4Box (#144). **HLG-Punkt
  neu aufnehmen.**
- **HLG-Wiedergabe-Look:** native HLG-Aufnahme auf einem HLG-fähigen Display/TV oder in einem
  HLG-bewussten Player abspielen — sieht der Direkt-OETF-Encode (D1 = 1000 nits, keine explizite
  OOTF) natürlich aus? Das ist die kritische, **nur** live beantwortbare Frage (Risiko unten).
- **HLG auf Monitor-Capture (already-PQ-Transcode, Entscheidung G):** eine native HLG-Aufnahme eines
  echten HDR10-**Desktops** (R10G10B10A2-Input) machen und per ffprobe bestätigen, dass die Datei
  `arib-std-b67` trägt **und** die Pixel plausibel HLG sind (nicht die durchgereichten PQ-Pixel) —
  der Fall, der ohne G eine mislabeled Datei erzeugt hätte.
- **HLG-Snapshot-Farbtreue (RELEASE-GATE):** während einer nativen HLG-Aufnahme einen Snapshot
  auslösen; das PNG muss farbrichtig sein (HLG-bewusster Monitor-Decode, nicht PQ-EOTF) — reiht sich
  in den bestehenden 444-Snapshot-PNG-Check ein.
- **ACM-Query real:** auf einem Advanced-Color-SDR-Desktop und einem echten HDR-Desktop prüfen, dass
  `advanced_color_enabled` korrekt gemeldet wird und die Diagnostics-Notice ruhig erscheint
  (probe_hdr).
- **Kein Regress an HDR10:** die bestehenden HDR10-Live-Checks (SEI, WGC-HDR-Fenster, Preview-
  WYSIWYG) nach der PR-3-Konsolidierung erneut bestätigen.

## Risiken

- **HLG-OOTF-Vereinfachung (D1).** Direkter OETF-Encode ohne explizite System-Gamma/OOTF kann auf
  echten HLG-Displays zu hell/flach wirken. Mitigierung: fester nominaler Peak 1000 nits ist
  BT.2408-konform und deterministisch (Referenzweiß bei ~70 %); Look ist nur live final beurteilbar;
  falls unnatürlich, ist eine OOTF-Stufe ein Folge-Slice (die reine `hdr_hlg.h` macht das lokal).
  Ehrlich als Live-Gate benannt.
- **`DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO`-Struct-Verfügbarkeit ist eine Build-SDK-Frage**, kein
  Laufzeitrisiko: die API `DisplayConfigGetDeviceInfo` existiert seit Win7 und wird bereits genutzt
  (`dxgi_od_capture_src.cpp:89,101`); das Struct liegt in `wingdi.h` des gebauten SDK. Win11 24H2
  führt `..._INFO_2` additiv ein (optional später). Direkt aufrufen, Felder bei `!= ERROR_SUCCESS`
  false. Rein additiv → kein Capture-Risiko (F2).
- **Umbenennungen (`hdr10_output_supported`→`hdr_native_supported`, `IsHdr10NativeEffective`→
  `IsHdrNativeEffective`, `CodecSupportsHdr10Native`→`CodecSupportsHdrNative`).** Breite, aber rein
  mechanische Berührung (Capture, video_thread, Coordinator, Tests). Pre-1.0: hart umbenennen statt
  Alias-Zoo.
- **Konsolidierung (PR-3) als stiller Verhaltensbruch.** Der einzige heikle Punkt ist Verhaltens-
  identität für PQ. Mitigierung: `test_hdr_native_reconcile.cpp` unverändert grün + neue Matrix-Tests
  vor dem HLG-Diff.
- **Container-Transfer + WebM ist ein erreichbares HDR-Ziel (Korrektur einer früheren Fehlannahme).**
  Der HLG-transfer-Tag ist belegt-frei für MKV (`matroska_stream_writer.cpp:287`) und den
  MP4-Roundtrip (`mp4_remuxer.cpp:264-266`). **Aber: es gibt kein Container-Gate für Native HDR.**
  `translation.cpp:39-41` erlaubt **WebM+AV1+Opus** mit `valid_chroma_depth`, und `valid_chroma_depth`
  (`:29-38`) schließt Bit10 mit AV1 ein; `hdr_mode` ist **nicht** Teil der Combo-Allow-List (`:82-96`)
  und `IsHdr…NativeEffective`/`ResolveOdCaptureMode` prüfen keinen Container. Ein User mit
  WebM+AV1+Opus+Hdr10 auf HDR-Display bekommt heute **native HDR10 in WebM** — und nach PR-5 native
  HLG in WebM. **Entscheidung: die Kombination ist unterstützt** (WebM ist ein Matroska-Subset, das
  das `Colour`-Element trägt; derselbe `matroska_stream_writer` schreibt den transfer-Tag) und wird
  **getestet** (Writer-Test um den WebM-DocType-Fall erweitern, s. PR-5-Tests). Kein neues Gate — aber
  explizit als unterstützt benannt und abgedeckt, statt eine erreichbare Kombination ungetestet zu
  lassen. Damit ist Native HDR (PQ **und** HLG) auf **MKV, WebM und MP4** verfügbar.

## Offene Fragen (nur echte Produktentscheidungen)

1. **HLG-UI-Label + Anordnung.** 3-Wege *Tone-map to SDR / Native HDR10 (PQ) / Native HLG* — sind
   das die gewünschten sichtbaren Bezeichnungen (CodecLabels-Kanon)? Oder „HDR10" vs „HLG" ohne
   „(PQ)"?
2. **[ENTSCHIEDEN → D1] Nominaler HLG-Peak.** Fester Wert 1000 nits (BT.2408-konform,
   deterministisch, panel-unabhängig). Der adaptive Display-Peak (D2) ist verworfen: er backt das
   Aufnahme-Panel in die metadaten-lose HLG-Datei und macht dieselbe Szene je nach Monitor
   unterschiedlich hell (s. Entscheidung D). Ein späterer panel-adaptiver Modus nur mit Clamp +
   expliziter Produkt-Freigabe.
3. **HLG bit-depth.** 10-bit pinnen (empfohlen, reuse P010, HEVC/AV1-Gate) — oder soll 8-bit-HLG
   (H.264-fähig) je ein Ziel sein? (Empfehlung: nein, 10-bit-only wie HDR10.)
4. **Diagnostics-Ton bei ACM/DXGI-Divergenz.** Eine ruhige Notice (empfohlen) — oder gar nichts,
   solange das Capture-Verhalten korrekt aus dem echten Frame kommt?
5. **[PRODUKT-GATE] 1.0-Scope-Revision.** `docs/product-spec.md:761` dokumentiert „no HLG/wide-gamut
   is the confirmed 1.0 scope". HLG-Shipping revidiert diese **bestätigte** Entscheidung —
   User-Sign-off ist Voraussetzung, bevor PR-6 (und damit das sichtbare Feature) landet.

## Adversarialer Review — Ergebnis

Alle sechs Einwände wurden gegen den Code (`main`) geprüft und **bestätigt**; alle eingearbeitet.

- **[Blocker] Already-PQ-R10G10B10A2-Input unter HLG erzeugt mislabeled Datei — EINGEARBEITET.**
  Verifiziert: `dxgi_od_capture_src.cpp:426-437` (R10G10B10A2+hdr_active+native → HdrNative) und
  `gpu_hdr_pq.cpp:71-74` (`flags.x`-Passthrough ohne OETF). Neue **Entscheidung G (Transcode
  PQ→HLG)** ersetzt das Passthrough für HLG; PR-4/PR-5 + Golden-/`ResolveOdCaptureMode`-Test ergänzt.
- **[Major] Snapshot-Monitor-Decode dekodiert HLG-P010 mit PQ-Mathematik — EINGEARBEITET.**
  Verifiziert: `video_thread.cpp:1931-1940,2045-2049` (`P010PqMonitorConverter` für jede
  `hdrNativeActive`-Session) und `preview_tap.h:67-69` (Tap im already-PQ-Fall deaktiviert). PR-5 um
  HLG-bewussten Monitor-Decode (inverse HLG-OETF) + Test erweitert; „Preview HLG-agnostisch"-Behauptung
  im Design präzisiert (gilt nur für den FP16-Tap, nicht den Snapshot-Readback).
- **[Major] „Native HDR nur MKV/MP4" ist falsch — kein Container-Gate — EINGEARBEITET.**
  Verifiziert: `translation.cpp:39-41` erlaubt WebM+AV1+Opus, `:29-38` schließt Bit10/AV1 ein,
  `hdr_mode` nicht in der Allow-List (`:82-96`). Risiko-Bullet korrigiert; WebM+AV1 native HDR als
  **unterstützt** benannt und in die PR-5-Container-Tests (WebM-DocType) aufgenommen.
- **[Major] D2 (adaptiver Peak) invertiert die HdrPeakScale-Analogie — EINGEARBEITET.**
  Verifiziert: `hdr_tonemap.h:41-47` nutzt den Display-Peak als Quell-Rolloff-Peak (Tonemap), nicht
  als Ziel-Signalnormalisierung. Entscheidung D auf **D1 (fest 1000 nits, BT.2408)** revidiert; D2
  verworfen; Offene Frage 2 als entschieden markiert (kein „empfohlen-und-offen"-Widerspruch mehr).
- **[Minor] PR-6/Doc-Referenzen fehlerhaft — EINGEARBEITET.** Verifiziert: `roadmap.md` = 311 Zeilen,
  kein HLG (`:337` existiert nicht → Schritt war No-Op, entfernt); KNOWN_LIMITATIONS-Zeilen 120/241/333
  (nicht 120/242/337) korrigiert; `product-spec.md:761` (confirmed 1.0 scope) zu PR-6 + Sign-off-Gate
  ergänzt; ADR-Pfad `docs/decisions/` klargestellt.
- **[Minor] PR-2-Guard gegenstandslos + PR-3-Test mislabeled — EINGEARBEITET.** Verifiziert:
  `DisplayConfigGetDeviceInfo` wird direkt aufgerufen (`dxgi_od_capture_src.cpp:89,101`), Mindest-OS
  Win10/11 (`README.md:28`), WGC≥1903 → GetProcAddress/`__has_include`-Guard entfernt (direkter Aufruf,
  Felder bei Fehler false). `test_hdr_native_reconcile.cpp` testet nur die reine
  `ApplyHdr10NativeEncode`-Primitive (kein Coordinator-Seam) → PR-3-Framing korrigiert: die neuen
  `ReconcileHdrAxes`-Tests sind der **Ersatz** der Verhaltensabsicherung, nicht ein Zusatz.

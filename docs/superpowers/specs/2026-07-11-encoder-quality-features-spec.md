# Encoder-Qualitätsfeatures: B-Frames, Lookahead, Temporal-AQ + SSIM/VMAF-Qualitätsmatrix

**Thema:** M-2-Rest (Review 2026-07-10, §5 Punkt 10) · **Autor:** Fable, Spec-Welle 2026-07-11 ·
**Verwandte Spec:** `nvenc-async-pipeline-spec` (M-1) — harte Abhängigkeit, Schnittstelle in §„Design D5" definiert.

---

## Problem

Der NVENC-Pfad nutzt drei Qualitätsfähigkeiten der Hardware nicht, die zusammen laut
NVIDIA-Referenzmessungen und Review-Einschätzung **10–20 % Bitrate bei gleicher Qualität**
wert sein können:

1. **B-Frames** (`frameIntervalP > 1`, plus `useBFramesAsRef` = B-Ref-Mode): bidirektionale
   Prädiktion, der größte Einzelhebel — **aber per Codec capability-abhängig**.
   Ehrlichkeits-Hinweis: Der Shipped-Default-Codec ist **AV1** (Default-Preset
   `MKV + AV1 + Opus`, `product-spec.md:99`), und NVENC-AV1-B-Frames sind stark
   generationsabhängig — verbreitete Hardware meldet `NV_ENC_CAPS_NUM_MAX_BFRAMES = 0`
   für AV1; dort klemmt das Cap-Gate (D1) die B-Frames-Row für den Default-Codec
   vollständig weg und der Hebel greift nur für H.264/HEVC. Das Gate macht das korrekt;
   Doku und Erwartungsmanagement (D7/S8, `KNOWN_LIMITATIONS.md`) müssen es explizit
   benennen, damit die Spec nicht mehr verspricht, als der Default-Codec auf viel
   Hardware liefern kann.
2. **Lookahead** (`enableLookahead` + `lookaheadDepth`): Rate-Control sieht künftige Frames
   und verteilt Bits besser (besonders unter VBR/CBR).
3. **Temporal-AQ** (`enableTemporalAQ`): adaptive Quantisierung über die Zeitachse (statische
   Regionen bekommen weniger Bits, bewegte mehr) — Ergänzung zum bereits aktiven Spatial-AQ.

Gleichzeitig existiert **keine Messinfrastruktur**, die einen Qualitätsgewinn belegen oder
widerlegen könnte: kein einziges SSIM/VMAF-Ergebnis im Repo, kein Harness, kein dokumentierter
Workflow. Die Roadmap führt die „Quality validation matrix (SSIM/VMAF …)" aber explizit als
**1.0-Gate** (`docs/roadmap.md:88` und `:176-177`). Ohne die Matrix kann keine
Encoder-Qualitätsänderung (auch keine künftige: Preset-Defaults, Multipass, Software-Encoder)
ehrlich beurteilt werden.

Erschwerend: **B-Frames brechen eine tragende Invariante der heutigen Pipeline** —
`output order == submission order`. Auf dieser Invariante stehen die PTS/Slot-FIFO-Paarung im
Encoder, die submission-seitige Keyframe-Vorhersage (HDR-SEI-Attach) und die PTS-sortierte
Emission des Matroska-Writers. Diese Spec legt fest, was davon hier gelöst wird, was die
parallele `nvenc-async-pipeline-spec` (M-1) liefern muss, und in welcher Reihenfolge.

---

## Ist-Zustand (Stand main @ #192, alle Referenzen frisch erhoben)

### Encoder: Features hart deaktiviert

- `libs/recorder_core/src/nvenc_encoder.cpp:801-805` — `FetchPresetConfig` pinnt nach dem
  Preset-Fetch: `enableLookahead = 0`, `lookaheadDepth = 0`, `frameIntervalP = 1`
  (Kommentar: „Zero lookahead and P-only: prevents 8-slot NVENC input ring from exhausting").
- `libs/recorder_core/src/nvenc_encoder.cpp:696-700` — `ApplySpatialAqToNvenc` (aus #181):
  `enableAQ = 1` (Spatial-AQ an), `enableTemporalAQ = 0` (bewusst aus, weil ohne Lookahead
  undokumentiert), `aqStrength = 0` (Treiber-Auto).
- `libs/recorder_core/src/nvenc_encoder.h:126-136` — Header-Kommentar benennt das Gate
  explizit: Temporal-AQ hat einen Capability-Bit (`NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ`),
  Spatial-AQ nicht.
- GOP/Keyframe-Interval ist seit #181 real verdrahtet: `SetKeyframeIntervalSecs`
  (`nvenc_encoder.h:230-232`), `ComputeGopLength`/`ApplyGopToNvenc`
  (`nvenc_encoder.cpp:671-694`), angewandt in `InitEncoder` (`nvenc_encoder.cpp:869-870`).

### Die Invariante `input-order == output-order` und ihre Konsumenten

- **PTS/Slot-Paarung als FIFO:** `m_pendingPts`/`m_pendingSlots`
  (`nvenc_encoder.h:338-342`), Push bei Submission (`nvenc_encoder.cpp:1146-1148`), Pop in
  `LockAndConsumeBitstream` (`nvenc_encoder.cpp:1090-1104`) — der fertige Output wird blind
  mit dem **ältesten** submitteten PTS/Slot gepaart. Mit Reordering (B-Frames) ist das falsch.
- **Submission-seitige Keyframe-Vorhersage:** `NextGopKeyframePhase`
  (`nvenc_encoder.cpp:702-712`), genutzt in `EncodeFrame` (`nvenc_encoder.cpp:1169-1191`),
  um die HDR10-SEI/OBU-Payloads **beim Submitten** an den vorhergesagten IDR zu hängen.
  Der Kommentar in `InitEncoder` (`nvenc_encoder.cpp:871-877`) warnt wörtlich: B-Frames,
  Lookahead oder adaptive I desynchronisieren die Vorhersage.
- **Echte Keyframe-Flagge am Output existiert bereits:** `lockBS.pictureType ==
  NV_ENC_PIC_TYPE_IDR || _I` (`nvenc_encoder.cpp:1106-1109`) → `EncodedVideoPacket::keyframe`.
  Achtung: auch ein **non-IDR-I** zählt heute als Keyframe (relevant für D4).
- **Split nutzt die echte Output-Flagge:** Forced IDR via `RequestKeyframe()`
  (`libs/recorder_core/src/video_thread.cpp:1785`), Segmentwechsel bei
  `split_armed && pkt.keyframe` (`video_thread.cpp:1890-1900`).
- **Ein einziger Bitstream-Buffer** für den ganzen 8-Slot-Ring:
  `m_bitstreamBuffer` (`nvenc_encoder.h:310`), `CreateBitstreamBuffer`
  (`nvenc_encoder.cpp:975 ff.`), Ring `std::array<InputSlot, 8>` (`nvenc_encoder.h:313`).
  Jede Pipeline-Vertiefung (Lookahead-Depth, B-Reorder) erhöht In-Flight-Frames gegen
  dieselben 8 Slots und denselben einen Output-Buffer.

### Mux-Pfad: kein DTS, PTS-sortierte Emission

- `EncodedVideoPacket { bytes, pts_ns, keyframe }` — **kein DTS/keine Decode-Order-Info**
  (`libs/recorder_core/include/recorder_core/packet_types.h:8-12`); ebenso
  `MuxPacket { pts_ns, track_num, is_key, bytes }`
  (`libs/recorder_core/src/matroska_stream_writer.h:94-99`).
- Der Matroska-Writer hält ein Reorder-Fenster (3 s / 4096 Pakete,
  `matroska_stream_writer.h:146-147`), **sortiert aufsteigend nach PTS**
  (`matroska_stream_writer.cpp:428-439`) und emittiert SimpleBlocks in dieser Reihenfolge
  (`matroska_stream_writer.cpp:449-467, 469-529`). Für den heutigen P-only-Stream ist
  PTS-Ordnung == Decode-Ordnung; **mit B-Frames würde der Writer den Video-Stream in
  Präsentationsreihenfolge speichern — Matroska verlangt Decode-Reihenfolge** (der Demuxer
  füttert Blocks in Speicherreihenfolge an den Decoder). Das wäre ein kaputtes File.
- Cluster-Start und Cues hängen an `is_key` des Video-Tracks
  (`matroska_stream_writer.cpp:491, 519-520`).

### Capability-Infrastruktur (Pattern für das neue Gate)

- Caps-Query-Helfer existiert im Encoder: `QueryEncodeCap`
  (`nvenc_encoder.cpp:43-56`), genutzt für `WIDTH/HEIGHT_MIN/MAX`
  (`nvenc_encoder.cpp:898-905`) und `NV_ENC_CAPS_SUPPORT_YUV444_ENCODE`
  (`nvenc_encoder.cpp:571-580`).
- App-weite Probe: `ProbeNvencCodecs` in `libs/capability/src/runtime_query.cpp:153-242`
  öffnet eine echte NVENC-Session, enumeriert Codec-GUIDs und fragt YUV444-Caps pro Codec
  (`runtime_query.cpp:216-234`) → `NvidiaRuntimeFacts`
  (`libs/capability/include/capability/runtime_snapshot.h:57-81`).
- `CapabilitySet` trägt bereits per-Codec-Maps für Feature-Fähigkeiten (`hdr10_native`,
  `chroma444` — `libs/capability/include/capability/capability_set.h:56-64`) mit
  `Query…`-Methoden; der Resolver/`OptionQuery` (`libs/capability/include/capability/
  resolver.h`, `option_query.h`) ist seit #190 der einzige Ort für Auswahl-Policy.
- Device-Tab-Matrix probt pro Adapter dieselben Caps separat
  (`libs/capability/src/adapter_capability.cpp:129-138`).
- Caps-Disk-Cache ist über `kCapabilityCacheSchemaVersion` versioniert
  (`libs/capability/src/capability_cache_key.cpp:5-10`) — neue Facts ⇒ Schema-Bump,
  pre-1.0 = Reset statt Migration.

### Expert-Setting-Plumbing-Kanon (ADR 0039, Vorlage `nvenc_preset`)

Vollständige Kette, an der sich jedes neue Encoder-Expert-Setting ausrichtet:

- `OutputSettingsModel::nvenc_preset` (`app/models/OutputSettingsModel.h:106`),
  getragen von `MergeFormatSelection` (`app/models/OutputSettingsModel.cpp:218`) mit
  Red-Proof-Test (`app/tests/test_output_settings.cpp:1039-1054`).
- ConfigPage-Expert-Combo (`app/pages/ConfigPage.cpp:2574, 2798, 2831`), Recording-Lock
  analog `keyframe_interval_combo_` (`ConfigPage.cpp:5603-5614`).
- Coordinator-Mapping (`app/services/RecordingCoordinator.cpp:215-217` bzw. `:734-743`
  für das Keyframe-Interval).
- `RecorderConfig::nvenc_preset` / `keyframe_interval_secs`
  (`libs/recorder_core/include/recorder_core/recorder_session.h:302, 363`).
- VideoThread-Setter (`video_thread.cpp:557, 562`).
- Additive TOML-Persistenz ohne Schema-Bump, unbekannter Wert ⇒ Struct-Default
  (`app/settings/RecordingPresetStore.cpp:595, 779-782`;
  Tests `app/tests/test_recording_preset_store.cpp:489-521`).
- `probe_record --preset p1..p7` (`tools/probes/probe_record/src/main.cpp:20-24`).

### SDK, Harness-Landschaft, FFmpeg-Grenze

- Gebündelter Header ist **NVENC SDK 13.0** (`third_party/nvidia/nvEncodeAPI.h:118-121`).
  Vorhandene Caps/Felder: `NV_ENC_CAPS_NUM_MAX_BFRAMES` (`:838`),
  `NV_ENC_CAPS_SUPPORT_LOOKAHEAD` (`:1097`), `NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ` (`:1104`),
  `NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE` (`:1141`); `disableIadapt`/`disableBadapt`/
  `enableTemporalAQ` (`:1570-1572`); `useBFramesAsRef` in H.264- **und** AV1-Config
  (`:1896`, `:2112`) — AV1-„B-Frames" sind NVENC-seitig dieselbe `frameIntervalP`-Mechanik.
- `scripts/dev/` enthält genau ein Dev-Skript (`gen-manifest-fixture.py`) — es gibt
  **kein** Qualitäts-Harness. `probe_record` nimmt live den Bildschirm auf
  (nichtdeterministischer Input) und taugt nicht als Matrix-Quelle.
- Der ausgelieferte FFmpeg-Prebuilt ist **mux-only** (avformat/avcodec/avutil/swresample,
  kein avfilter/swscale, erst recht kein libvmaf — `KNOWN_LIMITATIONS.md:229-234`).
  Vendoring von VMAF ins Produkt ist ausgeschlossen (Design-Vorgabe dieser Spec).

---

## Design

### D1 — Capability-Gate: echte NV_ENC_CAPS-Abfrage, keine Architektur-Heuristik

**Alternativen:**

| Option | Bewertung |
|---|---|
| **(a) Architektur-/Namens-Gate** („Turing oder neuer" via Adapter-Name/PCI-ID) | Verwaltet eine eigene GPU-Tabelle, die mit jeder Generation altert; sagt nichts über Treiber-Zustand; widerspricht dem Projektprinzip „truthful detection" (vgl. `nvenc_codec_probed`-Modell). |
| **(b) NV_ENC_CAPS pro Codec-GUID in der bestehenden Probe-Session** | Exakt das YUV444-Muster (`runtime_query.cpp:216-234`): kostenlos (Session ist ohnehin offen), pro GPU und Codec ehrlich, deckt Turing+-Erwartung implizit ab. |

**Entscheidung: (b).** In `ProbeNvencCodecs` werden pro advertisetem Codec-GUID vier Facts
erhoben: `NV_ENC_CAPS_NUM_MAX_BFRAMES` (int), `NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE` (int,
0/1/2-Semantik des SDK), `NV_ENC_CAPS_SUPPORT_LOOKAHEAD` (bool),
`NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ` (bool). „Turing+" bleibt reine Doku-Erwartung, niemals Code.
Zusätzlich behält der Encoder ein **defensives Clamp am Session-Init** (Feature angefordert,
Cap fehlt ⇒ Feature still auf AUS klemmen + ein Info-Log) — der Resolver verhindert das
vorher, aber der Engine-Pfad darf nie auf UI-Korrektheit angewiesen sein. Kein
Diagnostics-Alarm dafür (ruhig, kein „Problem", nur ein Fakt im Log).

### D2 — Settings-Modell: drei kuratierte Expert-Controls, Default AUS

**Alternativen:**

| Option | Bewertung |
|---|---|
| (a) Nur interne Config-Felder, keine UI, bis die Matrix Gewinn belegt | Minimal, aber dann kann niemand außer Entwicklern messen/verifizieren; Live-Verifies des Users bräuchten Spezial-Builds. |
| (b) Volle numerische Freiheit (B-Frames 0–7, Lookahead-Depth 0–32, AQ-Strength 1–15) | MVP-Expansion; erzeugt eine Testmatrix, die niemand validiert; widerspricht „vetted combinations only". |
| **(c) Drei kuratierte Expert-Rows, Default AUS, capability-gated** | Konsistent mit ADR 0039 (Preset-Combo) und dem 4:4:4-Muster; klein genug, um jede Zelle in der Matrix zu messen. |

**Entscheidung: (c).** Drei neue Rows in der bestehenden „Container & codecs"-Expert-Sektion
(direkt unter „Encoder preset (NVENC)"):

1. **`B-frames`** — Combo `Off (default) · 2 · 3`. Mapping: `frameIntervalP = n + 1`,
   geklemmt auf `NUM_MAX_BFRAMES`. Bei `n ≥ 2` und `SUPPORT_BFRAME_REF_MODE ≥ 1` wird
   `useBFramesAsRef = NV_ENC_BFRAME_REF_MODE_MIDDLE` automatisch mitgesetzt — **kein
   eigenes UI-Control** für B-Ref (eine Stellschraube, die ohne Messkontext nur verwirrt).
2. **`Lookahead`** — Toggle `Off (default) / On`. Depth ist **nicht** user-wählbar:
   `lookaheadDepth = min(16, verfügbare In-Flight-Kapazität − 4)` (Kapazität kommt aus M-1,
   s. D5). Immer mit `disableIadapt = 1` (Begründung in D4). `disableBadapt` bleibt 0
   (adaptive B-Platzierung ist der Qualitätsgrund für Lookahead+B-Frames).
3. **`Temporal AQ`** — Toggle `Off (default) / On`. Nur wählbar (enabled), wenn Lookahead
   an ist **und** `SUPPORT_TEMPORAL_AQ` vorliegt. Begründung für die Kopplung: der eigene
   Header-Kommentar (`nvenc_encoder.h:130-133`) — ohne Lookahead ist Temporal-AQ vom SDK
   nicht als gültig dokumentiert; wir bleiben auf dem dokumentierten Pfad. `aqStrength`
   bleibt 0 (Treiber-Auto, konsistent mit #181).

Alle drei: Default AUS in jedem Shipped-Preset (auch „Efficiency"), Recording-Lock wie alle
Format-Controls, Tooltip mit einem Satz Konsequenz („höhere Encode-Latenz; Gewinn abhängig
von Inhalt und Rate-Control"). Sichtbarkeit/Enabled-Zustand kommt aus dem Resolver/
`OptionQuery` (#190-Regel: Policy nie im UI-Code). Kein Diagnostics-Check, kein
Notice-Spam — ein deaktiviertes Feature ist kein Problem.

**Staging-Regel (Ehrlichkeit):** Die Rows erscheinen in S5 nur als **disabled „planned"
Rows** (Kanon `product-spec.md:651-652`) und werden erst in dem PR enabled, der die
Engine-Wirkung tatsächlich herstellt (S6 für Lookahead/Temporal-AQ, S7 für B-Frames).
Solange `FetchPresetConfig` die Features pinnt, wäre eine schaltbare Row eine unvetted
Combination (`product-spec.md:26`): der Nutzer schaltet ein und bekommt still einen
P-only-Stream. Das ursprüngliche Gegenargument zu Option (a) („sonst kann niemand
messen") trägt für das Staging nicht — die Messung läuft ohnehin über
`probe_encode_file` (D6), nicht über die Live-UI.

**Default bleibt AUS, bis die Matrix (D6) den Gewinn belegt.** Ein Default- oder
Preset-Flip ist eine eigene, evidenzpflichtige Produktentscheidung (D7).

### D3 — Persistenz & Resolver

Additive TOML-Felder unter `[output]`: `bframes = "off"|"2"|"3"`,
`lookahead = true|false`, `temporal_aq = true|false`. Unbekannter/fehlender Wert ⇒
Struct-Default (AUS), kein Schema-Bump (exakt das `nvenc_preset`-Muster,
`RecordingPresetStore.cpp:779-782`). Der Resolver bekommt drei neue Klemm-Regeln
(Feature an + Cap fehlt ⇒ `Adjustment` auf AUS mit Grund; Temporal-AQ an + Lookahead aus ⇒
`Adjustment` Temporal-AQ auf AUS). Der Capability-Disk-Cache bekommt einen Schema-Bump
(neue Facts ⇒ alte Einträge verfallen, Re-Probe — pre-1.0-Policy).

### D4 — Keyframe-Wahrheit: explizite Forced-IDR-Kadenz statt idrPeriod-Vorhersage

Das HDR-SEI/OBU-Attach **muss** submission-seitig passieren (NVENC nimmt SEI-Payloads nur
per `NV_ENC_PIC_PARAMS` beim Submitten an — `nvenc_encoder.cpp:1180-1191`). Die heutige
Vorhersage „Submission-Index mod gopLength" bricht mit B-Frames/Lookahead/adaptive I.

**Alternativen:**

| Option | Bewertung |
|---|---|
| (a) HDR-SEI an **jeden** Frame hängen | ~40 B/Frame ≈ 2 kbit/s — verkraftbar, aber verfehlt die Spezifikationsabsicht (Metadaten an Random-Access-Punkten), bläht AV1-OBU-Streams unschön auf und maskiert das eigentliche Problem nur für HDR, nicht für die GOP-Phase an sich. |
| (b) Output-seitig erkennen und „nachreichen" | Unmöglich — SEI kann nicht nachträglich in den fertigen Bitstream. |
| **(c) IDR-Platzierung selbst erzwingen:** submission-seitiger Zähler setzt alle `gopLength` Submissions `NV_ENC_PIC_FLAG_FORCEIDR`; `idrPeriod = gopLength` bleibt als Belt-and-Braces gesetzt | FORCEIDR bindet an **das submittete Bild** — gültig unabhängig von B-Frames und Lookahead. Die Kadenz-Wahrheit wandert von „Vorhersage über NVENC-Verhalten" zu „von uns erzwungene Tatsache". `NextGopKeyframePhase` bleibt als pure Kadenz-Logik bestehen, treibt aber jetzt das Flag statt nur die Vorhersage. |

**Entscheidung: (c)**, mit zwei Zusatzregeln:

- **`disableIadapt = 1` ist Pflicht, sobald Lookahead an ist.** Adaptive I erzeugt
  non-IDR-I-Frames an Szenenschnitten; `LockAndConsumeBitstream` zählt heute auch
  `NV_ENC_PIC_TYPE_I` als Keyframe (`nvenc_encoder.cpp:1106`). Ein non-IDR-I ist in einem
  Open-GOP **kein** sicherer Split-/Cue-/Trim-Punkt. Statt die Keyframe-Semantik überall
  aufzuspalten, verbieten wir adaptive I schlicht.
- Die Keyframe-Flagge im `EncodedVideoPacket` bleibt die **Output-Wahrheit**
  (`lockBS.pictureType`); durch (c) + `disableIadapt` stimmt sie mit der erzwungenen
  Kadenz überein. Split (`video_thread.cpp:1890`), Cues und Cluster bleiben unverändert.

Dieser Umbau ist klein, rein submission-seitig und **unabhängig von M-1 landbar** — er
ändert am heutigen P-only-Bitstream nichts (FORCEIDR an der Stelle, an der ohnehin ein IDR
entstünde), beseitigt aber die im Code dokumentierte Fragilität (`nvenc_encoder.cpp:871-877`).

### D5 — Schnittstelle zur `nvenc-async-pipeline-spec` (M-1) und Reihenfolge

B-Frames (und sinnvolle Lookahead-Tiefen) sind ohne die M-1-Umbauten nicht ehrlich baubar.
**Vertrag — M-1 muss liefern, diese Spec konsumiert:**

1. **Output-Identifikation statt FIFO:** Zuordnung fertiger Outputs zu
   `{pts_ns, slot_idx}` über `lockBS.outputTimeStamp` (== submittetes
   `inputTimeStamp`/`m_frameIdx`, `nvenc_encoder.cpp:1192`), d. h. eine Map
   `frameIdx → {pts, slot}` löst `m_pendingPts`/`m_pendingSlots` ab. Ohne das ist jede
   Reorder-Konfiguration ein stiller PTS-Korruptor.
2. **Kapazitäts-Parametrisierung:** Slot-Ring-Größe und **ein Bitstream-Buffer pro
   In-Flight-Frame** (heute: einer für alles, `nvenc_encoder.h:310`) als Funktion der
   Pipeline-Tiefe `depth = lookaheadDepth + frameIntervalP + Preset-Puffer`. Diese Spec
   liefert M-1 die Formel; M-1 liefert die Mechanik. **Zusätzlich gehört die
   DPB-Dimension in den Vertrag:** das automatisch gesetzte
   `useBFramesAsRef = MIDDLE` (D2) erhöht den Referenz-Frame-Bedarf; M-1/S7 müssen
   `maxNumRefFrames` (H.264, `nvEncodeAPI.h:1870`) bzw. `maxNumRefFramesInDPB`
   (HEVC/AV1, `nvEncodeAPI.h:1968/:2090`) als Funktion des B-Ref-Modus **explizit**
   behandeln — der Wert 0 (= Treiber-Default-DPB) ist nur als dokumentierte, im
   Harness verifizierte Entscheidung zulässig, nicht durch Auslassung.
3. **Erhalt der Forced-IDR-Kadenz (D4)** als submission-seitige Wahrheit — M-1 darf die
   Keyframe-Erkennung sonst vollständig auf Output-Seite (`pictureType`) ziehen.
4. **Decode-Order-Kennzeichnung am Packet:** `EncodedVideoPacket` bekommt in M-1 ein
   monotones `decode_index`-Feld (Output-Reihenfolge). Diese Spec baut darauf den
   Mux-Pfad (D6-unabhängig, Schritt S7).

**Reihenfolge (verbindlich):**

```
S1 Caps-Probe + Device-Matrix        — unabhängig, sofort
S2 Harness-Probe (Datei→NVENC)       — unabhängig, sofort
S3 Matrix-Skript + Workflow-Doku     — nach S2; etabliert den NVIDIA-Teil des 1.0-Qualitäts-Gates
S4 Forced-IDR-Kadenz (D4)            — unabhängig, vor oder parallel zu M-1
S5 Config-Plumbing + disabled Rows   — unabhängig (Engine ignoriert Felder noch; Rows „planned")
── M-1 (nvenc-async-pipeline-spec) landet ──
S6 Lookahead + Temporal-AQ           — nach M-1 (Kapazität, Output-Map)
S7 B-Frames + B-Ref + Decode-Order-Mux — nach M-1 und S6
S8 Matrix-Läufe + ADR/Doku-Abschluss — nach S6/S7
```

**Bewusst verworfen:** Lookahead mit Mini-Tiefe (≤ 4) vor M-1 in den heutigen 8-Slot-Ring
zu quetschen. Technisch möglich (der `NEED_MORE_INPUT`-Pfad puffert schon heute P5–P7),
aber eine Tiefe von 3–4 Frames bringt messbar fast nichts, riskiert Slot-Erschöpfung im
Drop-Pfad und wäre Wegwerf-Arbeit, sobald M-1 die Kapazität parametrisiert.

### D6 — Qualitätsmatrix: Dev-Harness mit externem ffmpeg, nichts wird vendored

**Alternativen für die Encode-Quelle:**

| Option | Bewertung |
|---|---|
| (a) ffmpeg-eigene NVENC-Encodes vergleichen | Misst ffmpeg-Defaults, nicht unseren Config-Pfad (Preset-Fetch-Reihenfolge, RC-Overrides, AQ-Pins). Wertlos für unser Gate. |
| (b) Zwei Live-Bildschirmaufnahmen vergleichen | Kein deterministischer Referenz-Input ⇒ SSIM/VMAF gegen was? Unbrauchbar. |
| **(c) Neue Dev-Probe: Y4M-Datei → produktidentischer NVENC-Pfad → Elementary Stream** | Deterministisch, misst exakt unsere `NvencEncoder`-Konfiguration, minimaler Probe-Umfang. |

**Alternativen für die Metrik-Seite:**

| Option | Bewertung |
|---|---|
| (a) libvmaf ins Produkt/Prebuilt vendoren | Verstößt gegen die harte Vorgabe: Prebuilt bleibt mux-only; Produkt braucht keine Qualitätsmetriken zur Laufzeit. |
| **(b) Externes ffmpeg-CLI (Full-Build mit libvmaf) als reine Dev-Abhängigkeit** | Null Produkt-Impact; auf jeder Dev-Maschine in Minuten installierbar; Version wird ins Ergebnis protokolliert. |

**Entscheidung: (c) + (b).** Konkret:

- **`tools/probes/probe_encode_file`** (neu, Dev-only, wird nie paketiert — wie alle
  Probes): liest Y4M (4:2:0, 8-bit, v1 des Harness; 10-bit/P010 später), konvertiert
  I420→NV12 auf der CPU, lädt Frames über Staging-Texturen in den regulären
  Slot-Ring und fährt `NvencEncoder` mit der **vollen Setter-Oberfläche** (`--vcodec`,
  `--preset`, `--rc cq|vbr|cbr`, `--cq`, `--bitrate`, `--keyint`, `--bframes`,
  `--lookahead`, `--temporal-aq`). Output: Elementary Stream — Annex-B (`.h264`/`.h265`)
  bzw. IVF (`.ivf` für AV1). Kein Muxen in der Probe (kein CodecPrivate-Handling nötig;
  ffmpeg dekodiert alle drei Formate direkt).
- **`scripts/dev/encoder_quality_matrix.py`** (neu): nimmt ein Clip-Set (Y4M) + eine
  Matrix-Definition (Feature-Zellen × RC-Punkte), ruft pro Zelle `probe_encode_file`,
  dann `ffmpeg -i <dist> -i <ref> -lavfi libvmaf=…;ssim=…` (plus PSNR), sammelt
  Bitrate aus der Dateigröße, rechnet **BD-Rate** (stückweise log-Interpolation über
  ≥ 4 RC-Punkte) und schreibt CSV + Markdown-Zusammenfassung. Protokolliert
  `ffmpeg -version`, Treiberversion, GPU-Name und probe-Commit in die Ergebnisdatei.
  Bricht mit klarer Meldung ab, wenn das gefundene ffmpeg kein `libvmaf` hat.
- **Pflicht-Dimension Rate-Control:** Die Matrix misst jede Feature-Zelle unter
  **CQ (Produkt-Default)** und **VBR** getrennt — Lookahead/AQ-Gewinne materialisieren
  sich primär unter Bitraten-RC; ein nur-CQ-Ergebnis wäre irreführend.
- **`docs/development/encoder-quality-matrix.md`** (neu, tracked): Referenz-Workflow —
  ffmpeg-Bezugsquelle (Full-Build mit libvmaf, z. B. gyan.dev), Clip-Set-Konventionen
  (Gameplay schnell/langsam, Desktop/Text-Scroll, je 10–30 s, aus eigenen Aufnahmen per
  `ffmpeg`-Decode zu Y4M gewonnen), Matrix-Aufruf, Ergebnis-Ablage
  (`docs/development/quality-results/<datum>-<gpu>.md`), und die Gate-Regel aus D7.
  Dieses Dokument definiert Workflow und Gate-Regel für **NVIDIA**-Encoder-
  Qualitätsänderungen und ist damit eine **Anzahlung auf** das 1.0-Gate, nicht das Gate
  selbst: Das Roadmap-1.0-Gate ist ausdrücklich **cross-vendor** und kann per Definition
  nicht vorgezogen werden (`docs/roadmap.md:88, :104`) — das Harness ist NVIDIA-only
  (NVIDIA-Hardware + lokales ffmpeg, keine CI). Es liefert die Methodik und den
  NVIDIA-Teil, den das 1.0-Gate später über alle Vendors ausrollt. Verweis aus
  `docs/roadmap.md` (1.0-Zeile) auf dieses Dokument.

**Bewusst NICHT gebaut:** keine In-App-Qualitätsmessung, kein VMAF im Produkt, keine
CI-Ausführung der Matrix (braucht NVIDIA-Hardware + lokales ffmpeg — CI bleibt bei den
puren Helper-Tests), keine 10-bit/HDR-Matrix in v1 (eigene Ausbaustufe, sobald der
8-bit-Workflow steht), kein Multipass/`lookaheadLevel`/UHQ-Tuning (SDK-13-Features, die
ohne Messgrundlage reine Spekulation wären).

### D7 — Gate-Regel für Default-/Preset-Änderungen

Ein Feature (oder eine künftige Encoder-Qualitätsänderung generell) darf Default oder
Bestandteil eines Shipped-Presets werden, wenn über das Referenz-Clip-Set gilt:

1. **Median-BD-Rate ≤ −5 %** (Bitrate-Ersparnis bei gleicher VMAF) in der Ziel-RC-Betriebsart, und
2. **kein Clip** regressiert um mehr als **+2 % BD-Rate**, und
3. die **p99-Encode-Latenz** (Messinfra aus M-1) das Frame-Budget der Zielkonfiguration
   nicht reißt (60 fps ⇒ Encode-p99 < 16 ms bleibt erfüllt).

Die Schwellen sind eine bewusste, revidierbare Setzung (dokumentiert im Workflow-Doc);
sie zu ändern ist billig, keine zu haben wäre das eigentliche Versäumnis.

---

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz.

### S1 — Capability-Probe + Device-Matrix (unabhängig)

- `libs/capability/include/capability/runtime_snapshot.h`: `NvidiaRuntimeFacts` erweitern um
  per-Codec-Facts (Struktur statt Einzel-Bools, da 3 Codecs × 4 Werte):
  `struct NvencAdvancedEncodeFacts { int max_bframes = 0; int bframe_ref_mode = 0; bool lookahead = false; bool temporal_aq = false; };`
  je `nvenc_adv_h264 / _hevc / _av1`, nur gültig bei `nvenc_codec_probed`.
- `libs/capability/src/runtime_query.cpp` (`ProbeNvencCodecs`, nach dem YUV444-Block
  `:216-234`): dieselbe `nvEncGetEncodeCaps`-Lambda für die vier Caps pro advertisetem GUID.
- `libs/capability/src/adapter_capability.cpp` (`:129-138`-Muster): dieselben Facts pro
  Adapter für die Device-Tab-Matrix; `adapter_capability.h` ergänzen; Device-Seite zeigt
  drei neue ruhige Fakten-Zeilen (B-Frames max / Lookahead / Temporal-AQ) pro Codec.
- `libs/capability/src/capability_builder.cpp`: Facts in eine neue `CapabilitySet`-Map
  `advanced_encode` (per `VideoCodec`, `SupportAnnotation` + max_bframes) übersetzen;
  statische Baseline ohne Probe: alles NotImplemented/0 (fail-closed, analog `chroma444`).
- `capability_set.h`: `QueryBFrames(VideoCodec) → {SupportAnnotation, int max}`,
  `QueryLookahead(VideoCodec)`, `QueryTemporalAq(VideoCodec)`.
- `libs/capability/src/capability_cache_key.cpp`: `kCapabilityCacheSchemaVersion` bumpen.
- **Tests (CI):** capability_builder-Tests mit synthetischen Facts (Cap da/fehlt/nicht
  geprobt ⇒ Annotation), Cache-Key-Test auf Schema-Bump. Kein GPU-Bedarf.

### S2 — `probe_encode_file` (unabhängig)

- `tools/probes/probe_encode_file/`: Y4M-Reader (nur `C420`/`C420jpeg`/`C420mpeg2`, 8-bit;
  alles andere klare Fehlermeldung), I420→NV12, D3D11-Device + Staging-Upload in den
  Slot-Ring, `NvencEncoder`-Setter 1:1 wie `video_thread.cpp:550-565`, Elementary-Stream-
  Writer (Annex-B-Concat; IVF-Header+Frame-Header für AV1), `--bframes/--lookahead/
  --temporal-aq` zunächst als angenommene, aber wirkungslose Flags (bis S6/S7) — die Probe
  druckt ehrlich, welche Felder der Encoder tatsächlich angewandt hat.
- CMake: Probe-Target wie die bestehenden (`tools/probes/CMakeLists.txt`), nie paketiert.
- **Tests:** Y4M-Parser + I420→NV12 + IVF-Framing als pure Funktionen mit Unit-Tests (CI);
  der GPU-Teil ist Dev-only (manuell, dokumentiert in S3).

### S3 — Matrix-Skript + Workflow-Doku (nach S2)

- `scripts/dev/encoder_quality_matrix.py` wie in D6; BD-Rate-Berechnung als pure Funktion
  im Skript mit Python-Selbsttests (`python encoder_quality_matrix.py --self-test`).
- `docs/development/encoder-quality-matrix.md` (Workflow, Clip-Set, Gate-Regel D7,
  Ergebnis-Ablage); Verweis aus `docs/roadmap.md` (1.0-Zeile) ergänzen.
- **Erster Pflicht-Lauf (Dev-Maschine):** Baseline-Matrix des Ist-Zustands
  (P4 vs P7, CQ vs VBR, Spatial-AQ ist ja bereits an) — validiert das Harness selbst und
  liefert die Referenzwerte, gegen die S6/S7 antreten.

### S4 — Forced-IDR-Kadenz (unabhängig, koordiniert mit M-1)

- `nvenc_encoder.cpp` `EncodeFrame`: `NextGopKeyframePhase` treibt zusätzlich
  `NV_ENC_PIC_FLAG_FORCEIDR`, wenn `phase.is_keyframe` (heutiges Verhalten: Flag nur bei
  `RequestKeyframe`). `idrPeriod = gopLength` bleibt gesetzt. Kommentar-Blöcke
  (`nvenc_encoder.cpp:871-877`, `nvenc_encoder.h:138-146`) auf die neue Wahrheit umschreiben.
- **Tests (CI):** bestehende `test_nvenc_rc_params`/GOP-Tests bleiben grün; neuer purer
  Test, dass die Kadenz-Entscheidung (`NextGopKeyframePhase`) mit Forced-IDR-Semantik
  konsistent bleibt (Phase-Reset). P-only-Verhaltens-Check: ein Harness-Lauf (S3)
  vorher/nachher — **identische Keyframe-Positionen** via `ffprobe -show_frames` plus
  valider, vollständig dekodierbarer Stream. Bewusst **nicht** als Bitstream-/Byte-
  Identität behauptet: `NV_ENC_PIC_FLAG_FORCEIDR` ist nicht garantiert byte-identisch
  zum `idrPeriod`-getriebenen IDR (RC-/GOP-Reset-Verhalten kann minimal abweichen),
  auch wenn die Positionen exakt stimmen.

### S5 — Config-Plumbing + disabled „planned"-Rows, Default AUS (unabhängig)

- `recorder_session.h` (`RecorderConfig`, bei `:302 ff.`): `uint32_t nvenc_bframes = 0;`
  (0=aus, sonst 2/3), `bool nvenc_lookahead = false;`, `bool nvenc_temporal_aq = false;`
  mit Kommentarkanon wie `nvenc_preset`.
- `OutputSettingsModel.h/.cpp`: Felder + `MergeFormatSelection` + Red-Proof-Tests
  (Muster `test_output_settings.cpp:1039-1054`).
- `RecordingCoordinator.cpp` (`:215 ff.`): Durchreichen.
- `video_thread.cpp` (`:557 ff.`): `nvenc.SetBFrames(…)`, `SetLookahead(…)`,
  `SetTemporalAq(…)` — Setter existieren ab S5 am Encoder, wirken aber erst ab S6/S7
  (bis dahin klemmt `FetchPresetConfig` weiterhin alles auf aus; ehrlicher Kommentar).
- `RecordingPresetStore.cpp`: additive TOML-Felder + Unknown-Value-Tests
  (Muster `test_recording_preset_store.cpp:489-521`).
- `ConfigPage.cpp`: drei Expert-Rows (Muster `video_encoder_preset_combo_`), aber in S5
  **hart disabled als ehrliche „planned"-Rows** (Kanon `product-spec.md:651-652`,
  Staging-Regel aus D2): solange `FetchPresetConfig` (`nvenc_encoder.cpp:801-805`) die
  Features pinnt, darf keine Row schaltbar sein — sonst nimmt ein Nutzer mit
  eingeschaltetem B-Frames/Lookahead still einen P-only-Stream auf (Verstoß gegen
  `product-spec.md:26`). Die Freischaltung des Enabled-Zustands ist Teil von S6
  (Lookahead/Temporal-AQ) bzw. S7 (B-Frames) — im selben PR wie die Engine-Wirkung.
  Der Disabled-Grund kommt als `OptionQuery`-/Resolver-Fakt („nicht implementiert"),
  nicht als UI-Sonderfall (#190-Regel); die D3-Cap-Klemmen kommen zusätzlich dazu.
  Recording-Lock wie üblich.
- `probe_record`: gleiche Flags durchreichen.
- Resolver (`libs/capability/src/resolver.cpp`): Klemm-Regeln (Feature noch nicht
  implementiert ⇒ Row disabled; Cap fehlt ⇒ AUS; Temporal-AQ ohne Lookahead ⇒ AUS)
  mit `Adjustment`-Begründung; pure Tests.
- **Tests (CI):** Merge-Red-Proofs, Preset-Roundtrip, Resolver-Klemmen (inkl.
  „planned/disabled bis Engine-Wirkung"), ConfigPage-Widget-Test (Rows vorhanden und
  in S5 disabled; Muster `test_config_page.cpp:2238-2243`).

### S6 — Lookahead + Temporal-AQ im Encoder (nach M-1)

- Neuer purer Helper in `nvenc_encoder.h/.cpp`:
  `ApplyQualityFeaturesToNvenc(NV_ENC_CONFIG&, VideoCodec, const QualityFeatureRequest&, const QualityFeatureCaps&) noexcept → QualityFeatureApplied`
  — klemmt gegen Caps, setzt `enableLookahead/lookaheadDepth/disableIadapt/enableTemporalAQ`
  (und ab S7 `frameIntervalP/useBFramesAsRef`), gibt zurück, was wirklich gilt (fürs Log
  und die Probe-Ausgabe). Ersetzt die harten Pins in `FetchPresetConfig` (`:801-810`).
- Caps liest der Encoder selbst via `QueryEncodeCap` nach `Open()` (defensives Clamp, D1).
- `lookaheadDepth` aus der M-1-Kapazitätsformel; Flush-Budget-Interaktion prüfen
  (`FlushDrainStep`, `nvenc_encoder.h:36-42`): mehr gepufferte Frames am Stop ⇒ der
  Drain konsumiert mehr Pakete, das per-Attempt-Budget bleibt unverändert gültig.
- Resolver/`OptionQuery`: die „noch nicht implementiert"-Klemme aus S5 für **Lookahead
  und Temporal-AQ** fällt in diesem PR — die Rows werden schaltbar, exakt gleichzeitig
  mit der Engine-Wirkung (Staging-Regel D2).
- **Tests (CI):** `ApplyQualityFeaturesToNvenc` pur (Caps-Klemmen, Kopplung Temporal-AQ↔
  Lookahead, disableIadapt-Pflicht). **Dev:** Matrix-Lauf CQ+VBR; `ffprobe`-Check, dass
  Keyframe-Kadenz exakt bleibt (S4-Kadenz).

### S7 — B-Frames + B-Ref + Decode-Order-Mux (nach M-1 und S6)

- Encoder: `frameIntervalP = bframes + 1` + `useBFramesAsRef` im S6-Helper aktivieren
  (inkl. DPB-Dimension aus dem D5.2-Vertrag: `maxNumRefFrames`/`maxNumRefFramesInDPB`
  explizit behandeln); Output-Zuordnung über die M-1-Map (Vertrag D5.1).
- Resolver/`OptionQuery`: die „noch nicht implementiert"-Klemme aus S5 für **B-Frames**
  fällt in diesem PR — die Row wird schaltbar, gleichzeitig mit der Engine-Wirkung.
- `packet_types.h`: `EncodedVideoPacket` + `MuxPacket` bekommen **eine gemeinsame
  Zeitachse `uint64_t dts_ns`**, die für beide Track-Arten definiert ist:
  - **Audio:** `dts_ns := pts_ns` (Audio hat kein Reordering; Push-Reihenfolge ==
    Decode-Reihenfolge == PTS-Reihenfolge).
  - **Video:** der VideoThread vergibt `dts_ns := base_pts_ns + decode_index ×
    frame_interval_ns` mit `base_pts_ns` = PTS des ersten Video-Frames und
    `decode_index` = monotone Output-Reihenfolge aus M-1 (D5.4). Damit liegt Video-DTS
    auf derselben Uhr wie Audio-PTS und ist pro Track streng monoton.
  - `dts_ns` ist ein **writer-interner Sortier- und Horizont-Schlüssel** (Matroska
    speichert kein DTS; SimpleBlock-Timestamps bleiben `pts_ns`). Ein `dts ≤ pts`-
    Invariant wird bewusst nicht gefordert — für die Interleaving-Entscheidung ist er
    irrelevant, und der 3-s-Horizont absorbiert die Reorder-Verschiebung.
- `matroska_stream_writer.cpp`: **Sortierung UND Drain-Horizont wechseln gemeinsam auf
  `dts_ns`** — das eine Reorder-Fenster interleaved weiterhin beide Tracks, aber:
  Insertion-Sort-Schlüssel (`:428-439`) wird `dts_ns` (sekundär stabile
  Push-Reihenfolge), `m_max_pushed_pts_ns` wird `m_max_pushed_dts_ns`, und die
  `behind_horizon`-Entscheidung (`:456`) vergleicht `front.dts_ns` gegen
  `m_max_pushed_dts_ns`. Ohne diese eine gemeinsame Achse für Sortierung **und**
  Horizont wäre die A/V-Verschränkung undefiniert (Video nach DTS sortiert, Horizont
  nach PTS gemessen ⇒ inkonsistente Drain-Reihenfolge). SimpleBlock-Timestamp bleibt
  `pts_ns` (Matroska speichert PTS; Speicher-Reihenfolge = Decode-Order). Cluster-Logik
  unverändert (Cluster startet am IDR, dessen PTS das GOP-Minimum ist ⇒ relative
  Timecodes bleiben ≥ 0).
- MP4-Remux: keine Code-Änderung erwartet (libavformat rekonstruiert DTS beim
  MKV-Stream-Copy), aber **Pflicht-Verify** (siehe Test-Plan) — bei Befund eigener Slice.
- Split mit B-Frames: Forced IDR schließt das GOP; der Boundary-Keyframe wandert durch
  die Reorder-Pipeline einige Frames später in die Mux-Queue — Logik unverändert
  (`video_thread.cpp:1890` prüft die Output-Flagge), Latenz-Erwartung dokumentieren.
- **Tests (CI):** Writer-Unit-Test mit synthetischem B-Muster (Video-Pakete in
  Decode-Order mit nicht-monotonem PTS) **plus interleavten Audio-Paketen** (Audio
  `dts_ns = pts_ns`, realistische 10-ms-Kadenz) — asserts: Video-Speicher-Reihenfolge ==
  Decode-Order, Block-Timestamps == PTS, Cues nur am IDR, **A/V-Interleaving nach
  `dts_ns` korrekt und Drain-Horizont über beide Tracks konsistent** (kein Audio-Stau
  hinter Video und umgekehrt; Horizont-Entscheidung auf `dts_ns` nachgewiesen). Ein
  video-only-Test würde eine A/V-Interleave-/Horizont-Regression nicht fangen.
  Encoder-seitig: purer Test der Kapazitätsformel. **Fixture-Test:**
  ein einmalig auf der Dev-Maschine erzeugtes Mini-B-Frame-Sample (2 s, eingecheckt,
  wenige hundert KB) läuft in CI durch Writer + `RemuxToProgressiveMp4`; Assertions über
  avformat-Leseseite (monotone DTS, korrekte Frame-Zahl).

### S8 — ADR + Doku-Abschluss (nach S6/S7)

- Neuer ADR `docs/decisions/00xx-encoder-quality-features-and-quality-gate.md`:
  Capability-Gate-Modell, Default-AUS-Policy, D7-Gate-Regel, Verweis auf das Workflow-Doc.
- `docs/product-spec.md` §12/Expert-Liste (`:645-649`) + Video-Sektion: drei neue
  Expert-Controls dokumentieren. `KNOWN_LIMITATIONS.md`: Absatz „B-Frames/Lookahead/
  Temporal-AQ sind Expert-Opt-in, Default aus; validiert auf <GPU>" — **inklusive der
  ehrlichen per-Codec-Verfügbarkeit:** B-Frames für den Default-Codec AV1 sind
  generationsabhängig und auf verbreiteter Hardware nicht verfügbar
  (`NUM_MAX_BFRAMES = 0` ⇒ Row capability-geklemmt); der Hebel greift dort nur für
  H.264/HEVC.
- Matrix-Ergebnisse nach `docs/development/quality-results/` einchecken.

---

## Test-/Verify-Plan

### CI-fähig (ohne NVIDIA-Hardware)

- Alle puren Helper: Caps-Übersetzung (S1), Y4M/NV12/IVF (S2), BD-Rate-Selbsttest (S3),
  Kadenz (S4), Resolver-Klemmen + Merge-Red-Proofs + TOML-Roundtrip (S5),
  `ApplyQualityFeaturesToNvenc` (S6), Writer-Decode-Order **inkl. A/V-Interleave auf
  der gemeinsamen `dts_ns`-Achse** + Remux-Fixture (S7).
- ConfigPage-Widget-Tests (eigene QApplication-Fixture, bestehendes Muster).

### Dev-Maschine, skriptgestützt (Harness — reproduzierbar, aber GPU-gebunden)

- Baseline-Matrix (S3), Feature-Matrizen (S6/S7) unter CQ **und** VBR.
- `ffprobe`-Strukturchecks pro Feature-Encode: `has_b_frames`/`pict_type=B` vorhanden,
  Keyframe-Kadenz exakt (`-show_frames`), MP4-Remux mit monotonem DTS, MKV-Blocks in
  Decode-Order (`mkvinfo`/`ffprobe -show_packets`).

### Nur User-live (ehrlich benannt)

- **Playback-Matrix** einer B-Frame-Aufnahme (MKV + remuxtes MP4): VLC, Windows
  „Filme & TV", ein NLE (DaVinci Resolve) — Ruckeln/Ordering-Artefakte sieht nur ein Mensch.
- **Edit-Overlay-Trim** auf einer B-Frame-Aufnahme (keyframe-akkurater Schnitt + Export).
- **Split-Session** (manuell + automatisch) mit B-Frames + Lookahead: Segmentgrenzen abspielen.
- **HDR10 + B-Frames:** SEI/OBU an jedem IDR (ffprobe-gestützt, aber der HDR-Bildeindruck
  im Player ist User-Sache).
- **Langzeit-Soak** (≥ 1 h) mit allen drei Features an: Slot-/Memory-Druck, Stop-Latenz.

Diese Punkte gehören auf die bestehende 0.9/1.0-Live-Verify-Liste des Users.

---

## Risiken

1. **Matroska/MP4-B-Frame-Pfad ist die eigentliche Gefahrenzone**, nicht NVENC: Ein Fehler
   in der Decode-Order-Emission erzeugt Files, die *manche* Player abspielen (fehlertolerant)
   und andere nicht — schwer zu entdecken. Gegenmittel: der Writer-Unit-Test mit
   synthetischem B-Muster + Remux-Fixture in CI (S7), Playback-Matrix beim User.
2. **avformat-62-DTS-Rekonstruktion beim MKV→MP4-Stream-Copy** könnte für unsere Streams
   Sonderfälle haben (vgl. die `ipcm`-Erfahrung aus 0.6.0). Pflicht-Verify vor Freigabe;
   Fallback-Position: B-Frames zunächst MKV/WebM-only anbieten (Resolver-Klemme pro
   Container) — bewusst als Rückfalloption dokumentiert, nicht als Plan.
3. **Treiber-/Generationen-Varianz:** `NEED_MORE_INPUT`-Tiefe und Reorder-Verhalten variieren
   pro GPU-Generation; gemessen wird nur auf der Dev-GPU. Ehrlich machen wie beim
   10-bit-Pfad: Features starten als ValidUnvalidated-Erfahrung, KNOWN_LIMITATIONS nennt
   die validierte Hardware.
4. **Stop-/Split-Latenz wächst** mit Pipeline-Tiefe (Flush muss `depth` Frames drainieren;
   Split-Boundary erscheint Frames später). Budget-Mechanik existiert (`FlushDrainStep`),
   aber die gefühlte Stop-Dauer ist ein UX-Fakt — im Tooltip und in KNOWN_LIMITATIONS nennen.
5. **Recovery/Crash mitten im GOP:** Mit B-Frames endet ein abgeschnittenes File mitten in
   einer Reorder-Gruppe; Player verwerfen die letzten Frames. Kein neuer Mechanismus nötig
   (heute geht auch der Rest des GOP verloren), aber der Recovery-Test gehört in die
   Verify-Liste.
6. **AQ/Lookahead-Wirkung unter CQ (Produkt-Default) möglicherweise gering** — dann belegt
   die Matrix eben ehrlich, dass der Gewinn nur unter VBR/CBR existiert, und die Features
   bleiben dort dokumentiert statt pauschal beworben. Das ist ein akzeptables Ergebnis,
   kein Scheitern der Spec.
7. **Harness-Drift:** ffmpeg-/libvmaf-Versionen ändern Scores leicht. Gegenmittel:
   Versionsprotokoll in jeder Ergebnisdatei; Vergleiche nur innerhalb desselben
   ffmpeg-Builds ziehen (im Workflow-Doc festgeschrieben).

---

## Offene Fragen (echte Produktentscheidungen)

1. **Preset-Flip nach positivem Matrix-Beleg:** Dürfen Shipped-Presets (z. B. „Efficiency")
   B-Frames/Lookahead aktivieren, sobald D7 erfüllt ist — oder bleibt bis 1.0 alles
   Expert-Opt-in und nur die Doku nennt den belegten Gewinn?
2. **Gate-Schwellen bestätigen:** Median-BD-Rate ≤ −5 %, Worst-Clip ≤ +2 %, p99 < Frame-Budget
   (D7) — Setzung des Autors; der Product Owner sollte sie einmal abnicken oder verschieben.
3. **Referenz-Clip-Set:** Welche Inhalte repräsentieren die Zielnutzung (welche Spiele/Genres,
   Desktop-Anteile, Clip-Länge)? Die Clips müssen vom User aufgenommen/freigegeben werden —
   ohne sie bleibt das Gate theoretisch.
4. **B-Frames-Container-Policy bei Remux-Befund:** Falls Risiko 2 real wird — MKV/WebM-only
   anbieten (ehrliche Resolver-Klemme) oder B-Frames zurückhalten, bis MP4 mitkann?

---

## Adversarialer Review — Ergebnis

Sechs Einwände eines adversarialen Reviews (anderes Modell), jeder gegen Code/Docs
verifiziert; alle sechs eingearbeitet:

1. **[major, eingearbeitet] S5-Honesty-Verstoß:** Bestätigt — `FetchPresetConfig`
   (`nvenc_encoder.cpp:801-805`) pinnt bis S6/S7 weiter, die D3-Klemmen decken „Engine
   ignoriert das Feld" nicht ab; enabled Rows hätten still P-only aufgenommen
   (Verstoß gegen `product-spec.md:26`). Fix: Staging-Regel in D2 + S5 liefert die Rows
   hart disabled als „planned" (`product-spec.md:651-652`); Freischaltung wandert in
   S6/S7, jeweils im PR mit der Engine-Wirkung. Messung läuft über `probe_encode_file`.
2. **[major, eingearbeitet] Writer-A/V-Achse unterspezifiziert:** Bestätigt — ein
   Fenster für beide Tracks, Sortierung und `behind_horizon`
   (`matroska_stream_writer.cpp:425-467`) hängen beide an `pts_ns`. Fix: gemeinsame
   `dts_ns`-Achse explizit definiert (Audio `dts_ns := pts_ns`; Video
   `base_pts_ns + decode_index × frame_interval_ns`), Sortierung UND Drain-Horizont
   wechseln gemeinsam darauf, S7-Writer-Test um interleavte Audio-Pakete +
   Horizont-Assertions erweitert.
3. **[minor, eingearbeitet] S4 „Bitstream-Identität":** Bestätigt — FORCEIDR ist nicht
   garantiert byte-identisch zum `idrPeriod`-IDR. Assertion präzisiert auf „identische
   Keyframe-Positionen + valider Stream", Byte-Identität explizit nicht behauptet.
4. **[minor, eingearbeitet] Überzogene 1.0-Gate-Behauptung:** Bestätigt —
   `roadmap.md:88/:104`: das 1.0-Gate ist cross-vendor und nicht vorziehbar, das
   Harness NVIDIA-only. D6 umformuliert: Workflow/Gate-Regel = Anzahlung auf das
   1.0-Gate, nicht das Gate; Reihenfolge-Diagramm (S3) angepasst.
5. **[minor, eingearbeitet] DPB-Dimension fehlte im D5.2-Vertrag:** Bestätigt —
   `useBFramesAsRef = MIDDLE` erhöht den Referenzbedarf (`nvEncodeAPI.h:1870/:1968/:2090`).
   D5.2 fordert jetzt explizite Behandlung von `maxNumRefFrames`/`maxNumRefFramesInDPB`
   als Funktion des B-Ref-Modus (0 = Treiber-Default nur als dokumentierte, verifizierte
   Entscheidung); S7-Encoder-Punkt referenziert den Vertrag.
6. **[minor, eingearbeitet] AV1-Default vs. B-Frames-Hebel:** Bestätigt — Default-Preset
   ist AV1 (`product-spec.md:99`), NVENC-AV1-B-Frames sind generationsabhängig
   (verbreitete Hardware: `NUM_MAX_BFRAMES = 0`). Ehrlichkeits-Hinweis im
   Problem-Abschnitt ergänzt; S8 verlangt die per-Codec-Verfügbarkeit explizit in
   `KNOWN_LIMITATIONS.md`.

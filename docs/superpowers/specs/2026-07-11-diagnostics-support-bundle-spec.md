# Diagnostics-Ausbau: Log-Schema, Session-Report, Support-Bundle, Troubleshooting

> **SHIPPED (PR #194, 2026-07-12).** Verifiziert 2026-07-23 gegen aktuellen Code:
> `app/diagnostics/SupportBundle.cpp`, `test_support_bundle.cpp` vorhanden. Nichts hier ist mehr offen.

> Spec-Welle 2026-07-11, Thema 10 (Review §6). Autor read-only; Umsetzung später durch
> Opus/Sonnet **nur** anhand dieser Spec. Ist-Zustand frisch aus dem Code auf `main` @ #192
> erhoben (Zeilennummern aus dem Review NICHT übernommen).

## Problem

ExoSnaps Diagnostics-Fundament (FixActions, Readiness-Gate, `ResolvePipelineHealth`, der Live-
`RecordingDiagnosticsSnapshot`) ist stark, aber der Support-Kanal fehlt. Für ein Produkt **ohne
Telemetrie** ist das Diagnose-Artefakt, das ein Nutzer manuell teilt, *der* Support-Weg. Konkret
fehlt bzw. ist unvollständig (Review §6.1–§6.3, §6.6, §6.9):

1. **Log-Schema:** Der menschenlesbare App-Log (`exosnap.log`) ist Freitext ohne
   Session-Korrelations-Schlüssel und ohne strukturierte Key-Values. Der Engine-Log
   (`engine.jsonl`) ist zwar bereits strukturiertes JSON-Lines, wird aber pro Launch
   **überschrieben** (kein Append, keine Rotation) und trägt ebenfalls keine Session-ID. Es gibt
   keinen gemeinsamen Schlüssel, der beide Streams (und einen späteren Session-Report) verbindet.
2. **Session-Report-Artefakt:** existiert nicht. Der Post-Flight-Report lebt nur in der UI
   (`RecordPage::updateReportCard`), es gibt kein `session-<id>.json` auf Platte mit resolved
   Config, Capture-Backend, Encoder-Init-Parametern, Drop/Dup/Discontinuity-Zählern,
   `duration_skew_ms`, Drift, Segment-Liste und Fehlerphase.
3. **Support-Bundle:** fehlt komplett. Der `Export Report`-Button auf der Diagnostics-Seite ist
   dauerhaft **disabled** mit Tooltip „planned for a future build".
4. **Startup-Trace:** `StartupClock` misst Meilensteine, aber sie landen nur als einzelne
   Log-Zeilen; es gibt keine Tabelle in Logs/About, die Startup-Regressionen sichtbar macht.
5. **Troubleshooting-Doku:** `docs/troubleshooting.md` existiert nicht — die Diagnostics-Investition
   ist nach außen unsichtbar.

Ziel: die vorhandene Intelligenz konservieren und den Support-Kanal schließen, **ruhig statt
alarmistisch**, Engine UI-agnostisch, Policy im Resolver, keine versteckte MVP-Expansion.

## Ist-Zustand (mit Datei:Zeile-Referenzen)

### Logging — zwei getrennte Streams

- **App-Log (menschenlesbar, Freitext).** `app/diagnostics/AppLog.cpp`. `LogEntry`
  (`AppLog.h:20-26`) = `sequence, timestamp, severity, category, message`. `formatEntry`
  (`AppLog.cpp:393-401`) erzeugt `"yyyy-MM-ddTHH:mm:ss.zzz [LEVEL] [category] message"`. **Kategorie
  ist das einzige Strukturfeld**; keine Session-ID, kein Event-Code, keine Key-Values.
  - Rotation existiert seit #179: `kMaxLogFileBytes = 5 * 1024 * 1024`, `kMaxLogFileCount = 3`
    (`AppLog.h:42-43`), größenbasiert in `rotateLogFileUnlocked` (`AppLog.cpp:97-111`), Datei bleibt
    über die Session offen, jede Zeile wird einzeln geflusht (`writeLineUnlocked`
    `AppLog.cpp:133-156`).
  - Log-Verzeichnis: `ResolveAppDataDir()/logs/exosnap.log` (`AppLog.cpp:245-248`,
    `settings/ConfigPaths.h:33-38`, override via `EXOSNAP_CONFIG_DIR`).
  - Developer-Level-Filter `min_severity` (SETTINGS-HONESTY-R1, `AppLog.cpp:48`,
    `setMinSeverity`/`minSeverity` `:459-467`); nullopt = „Off".
  - Export: `exportHistoryToFile` (`AppLog.cpp:403-431`) schreibt die In-Memory-History als Freitext.

- **Engine-Log (strukturiert, JSON-Lines).** `libs/recorder_core/src/logging/logging.cpp`. `log()`
  (`:106-154`) serialisiert **pro Record ein JSON-Objekt** mit `timestamp_unix_ms`, `level`,
  `component`, `message`, `fields{}` (`:133-146`). `LogRecord`/`LogField`/`LoggerConfig`
  (`logging.h:15-40`). Ring-Buffer 512 (`snapshot_ring_buffer` `:156-160`), `minimumLevel = Info`.
  - **Defekt 1:** `basic_file_sink_mt(config.filePath.string(), true)` (`logging.cpp:88`) — der
    zweite Parameter `truncate=true` überschreibt `engine.jsonl` bei **jedem** Launch.
  - **Defekt 2:** keine Rotation, kein Größendeckel.
  - **Defekt 3:** kein Session-ID-Feld.
  - Muster (`set_pattern("%v")` `:90`) gibt genau die `json.dump()`-Zeile aus, sonst nichts — sauber
    für JSON-Lines.

- **Bridge.** `app/diagnostics/EngineLogBridge.cpp`. `InitializeEngineLogging` (`:44-60`) setzt
  `config.filePath = <logdir>/engine.jsonl` (`:49`) und einen Sink, der jeden Engine-Record in eine
  AppLog-Freitextzeile **flacht** (`Flatten` `:34-40`, hängt `key=value` an die Message an). D.h.
  App→Engine-Abhängigkeit existiert bereits (App darf `recorder_core::logging::log` rufen).

### Session-Identität

- **Kein prozessweiter Session-Schlüssel.** Die einzige UUID-Quelle ist die **per-Segment**
  `current_manifest_id_` = `QUuid::createUuid().toString(WithoutBraces)`
  (`RecordingCoordinator.cpp:955`, Folge-Segmente `:1242`), gespeichert als
  `RecoveryManifestEntry::id` (`RecoveryManifestStore.h:11-19`). Das ist ein **Recording**-Bezug,
  kein Log-/Launch-Bezug.

### Post-Flight-Daten (was am Recording-Ende verfügbar ist)

- `UiRecordingResult` (`viewmodels/RecordViewModel.h:78-109`): `succeeded, output_path,
  error_phase, hresult_text, error_detail, output_file_bytes, elapsed_seconds, source/output w/h,
  content_rect, frame_rate_num/den, cfr, container, video_codec, audio_codec, markers,
  segments[]` (`CompletedRecordingSegment`). Geliefert via `ResultReadyCallback`
  (`RecordingCoordinator.h:50`, `PostResult` `:267`).
- **Finaler, eingefrorener `RecordingDiagnosticsSnapshot`** (`pipeline_diagnostics.h:242-284`):
  `capture` (frames_captured/emitted/dropped_{coalesced,cfr,backpressure}/duplicated, source_type,
  acquire_*), `compositor`, `video_encoder` (submitted/encoded/backlog/forced_keyframes, codec,
  w/h, cfr), `audio` (packets/bytes/discontinuities, sample_rate, channels, codec, track_count),
  `video_queue`/`audio_queue`, `mux`, `disk`, `split`, `av_drift_ms` + availability,
  `duration_skew_ms` + availability, `bottleneck`, `health`. `session_generation` als Guard.
  - **Wichtig:** `av_drift_ms` misst **bereits echte Clock-Drift** (WASAPI device-position/QPC vs.
    QPC-Video-Zeitachse, Header-Doku `:260-268`) — H-3-Stufe-2 (#191) ist gelandet. Review §6.5 ist
    damit erledigt; diese Spec baut **nur darauf auf**, ändert die Metrik nicht.
- UI-Nutzung: `RecordPage::updateReportCard` (`RecordPage.cpp:4740-4820`) zeigt Drop-%, Peak-A/V-
  Drift, Pipeline-Health aus `last_completed_snapshot_`. DiagnosticsPage Phase ④ „Open last report"
  routet auf den Editor (`DiagnosticsPage.h:101-106`), dupliziert den Report nicht.
- **NVENC-Init-Parameter sind NICHT app-seitig verfügbar.** `EncoderDiagnostics`
  (`pipeline_diagnostics.h:152-165`) trägt nur codec/width/height/cfr. Die komplette
  NV_ENC-Struktur lebt im Engine-Encoder (`nvenc_encoder.cpp`).

### Umgebungs-/Snapshot-Quellen (für das Bundle)

- `CapabilitySet` (`capability/capability_set.h:29-91`): `gpu_adapter_name`, `nvenc_dll_present`,
  `probed`, `runtime`, Support-Maps. MainWindow hält `runtime_caps_` (`MainWindow.h:459`) plus
  `CapabilityCacheStore` (`settings/CapabilityCacheStore.h`).
- `RuntimeCapabilitySnapshot` (`capability/runtime_snapshot.h:107-113`): `nvidia`
  (`NvidiaRuntimeFacts`: nvenc-DLL/API-Version, adapter_name, per-Codec-/4:4:4-Probes), `mf_aac`,
  `mf_webcam`, `os` (`build_number`, `version_string`), `displays[]` (`DisplayHdrFacts`: name,
  hdr_active, bits_per_color, Primaries, Luminanz).
- `EnumerateAdapters()` (`capability/adapter_enum.h:56-64`) → `AdapterInfo[]` (name, vendor, kind,
  vendor_id, device_id, luid, VRAM); `AdapterIdentity` (`runtime_snapshot.h:120-127`): adapter_luid +
  `driver_version` „A.B.C.D". Genutzt von `DevicePage.cpp:424-428`.
- **Display-Topologie:** die Bausteine existieren (`displays[]` + `EnumerateAdapters`), aber es gibt
  keine zusammengefasste Topologie-Struktur.
- Aufbereitete Zusammenfassungen: `CapabilitySummary::FromCapabilitySet`
  (`diagnostics/CapabilitySummary.h:19-22`) und `ConfigSummary::FromCurrentSettings`
  (`diagnostics/ConfigSummary.h:19-28`, inkl. `settings_file_path`, `effective_output_path`).
- Settings: `AppSettingsStore` (settings.ini via QSettings). Presets `presets.ini`.

### Scrubber (Privacy)

- `libs/crash_capture/src/crash_scrubber.cpp` + `include/crash_capture/crash_scrubber.h`.
  `ScrubString()` (`:170-197`, pure): strippt USERPROFILE → `[path]`, generische Windows-Pfade
  (`StripWindowsPaths` `:152-159`), Username → `[user]`, Machine → `[machine]`.
  `IsAllowedTagKey()` (`:42-48`) mit **Allowlist** von 10 Keys (`kAllowedTagKeys` `:37-40`:
  `os.name, os.version, gpu.model, gpu.vendor, gpu.driver, app.version, encoder_backend,
  container, video_codec, audio_codec`). ADR 0017; PRIVACY.md nennt genau diese Allowlist.

### Startup-Trace

- `StartupClock()` (`diagnostics/StartupClock.h:19-22`): ein einzelner `QElapsedTimer`, in `main()`
  vor `QApplication` gestartet, an Meilensteinen gelesen und als AppLog-„perf"-Zeile geloggt
  (`first-paint N ms`, `preview-live N ms`). `PageHydrationController` liefert Per-Tick-Meilensteine
  (`services/PageHydrationController.cpp`, `MainWindow`). **Keine Aggregation/Tabelle** — nur Zeilen.

### ZIP-Fähigkeit

- **miniz ist bereits vendored:** `libs/update/third_party/miniz/{miniz.h,miniz.c}`, aktuell nur für
  **Extraktion** genutzt (`libs/update/src/zip_extract.cpp`), inkl. Zip-Slip-Guard
  `IsSafeZipEntryName` (`:24-40`). miniz kann auch schreiben (`mz_zip_writer_*`). Der Scope liegt
  heute in `libs/update`.
- Atomare Schreibkonvention (`QSaveFile`) etabliert in `RecoveryManifestStore`/`CapabilityCacheStore`.

### Doku/ADRs

- `docs/troubleshooting.md` existiert nicht. Neueste ADR: `0043`; eine neue wäre **0044**.
- product-spec §11 (Diagnostics, calm-not-alarmist), Logs = „runtime events and per-session
  recording diagnostics" (`product-spec.md:57`). Roadmap 0.10 = „Reliability hardening … privacy
  review" (`roadmap.md:84`) — der Support-Bundle-Scrubbing-Punkt ist die Schnittstelle zur
  `privacy-review-spec` (Thema 20).

## Design

Fünf zusammenhängende, aber einzeln lieferbare Bausteine. Leitprinzip: **vorhandene Substanz
wiederverwenden statt parallel neu bauen** (Engine-JSONL, Scrubber, Snapshot, miniz, Summaries).

### D1 — Log-Schema: Session-ID + JSONL fixen, Freitext behalten

**Kernentscheidung:** Der menschenlesbare `exosnap.log` bleibt **unverändert der crash-sichere
Support-Log** (100+ Callsites `AppLog::info(category, message)`, jede Zeile einzeln geflusht — genau
das will man vor einem `qFatal`/`abort`). Die strukturierte Maschinen-Sicht kommt **nicht** durch
Umbau von AppLog, sondern durch Adoption des **bereits existierenden** Engine-JSONL als kanonischen
strukturierten Stream, plus einen gemeinsamen Session-Schlüssel.

Abgewogene Alternativen:

- **Alt-A: AppLog komplett auf JSON-Lines umbauen** (LogEntry um session/event_code/fields
  erweitern, `formatEntry` → JSON). *Verworfen:* riesiger Blast-Radius (jede Callsite müsste
  strukturiert werden oder man erzeugt Doppel-Freitext-im-JSON), zerstört die menschenlesbare
  Support-Zeile, und dupliziert exakt die Serialisierung, die `logging.cpp:133-146` schon macht.
- **Alt-B: Zwei völlig getrennte Streams lassen, nur je eine Session-ID reinstempeln.** *Verworfen:*
  löst die Truncate-/Rotations-Defekte des Engine-Logs nicht und lässt app-seitige strukturierte
  Events (die es künftig geben soll) heimatlos.
- **Alt-C (gewählt): Ein kanonischer strukturierter Stream (Engine-JSONL) + der Freitext-Log,
  verbunden über eine prozessweite Session-ID.** App-seitige strukturierte Events werden über einen
  dünnen neuen Entry-Point *sowohl* als Freitextzeile (wie heute) *als auch* als Engine-JSONL-Record
  emittiert. Begründung: minimaler Blast-Radius (bestehende `AppLog::info`-Calls bleiben
  unangetastet), nutzt die vorhandene, getestete JSON-Serialisierung, und die zwei Log-Defekte
  werden im Zuge dessen behoben.

Konkret:

1. **Prozessweite Launch-Session-ID.** In `AppLog::init` einmal `QUuid::createUuid()` erzeugen,
   als `AppLog::sessionId()` exponieren, in den Startbanner schreiben (`AppLog.cpp:274-277`). Das ist
   der **Launch**-Schlüssel (nicht die Recording-Manifest-ID). Beide erscheinen im Bundle-Manifest.
2. **Engine-JSONL fixen (Rotation + Session-Feld):**
   - `basic_file_sink_mt(path, true)` → `rotating_file_sink_mt(path, maxFileBytes, maxFileCount)`
     (spdlog liefert das out-of-the-box). Truncate entfällt damit.
   - **`LoggerConfig` um die Rotations-Schwellwerte erweitern:** `std::size_t maxFileBytes = 5*1024*1024`
     und `std::size_t maxFileCount = 3` (spiegelt AppLog `kMaxLogFileBytes`/`kMaxLogFileCount`).
     **Ohne diese Felder ist der zugesagte Rotationstest „bei kleinem Testschwellwert" nicht
     schreibbar** — der `rotating_file_sink_mt` hätte sonst nur die hartcodierten 5 MiB, die ein Test
     nicht in vertretbarer Zeit füllt. Der Test setzt `maxFileBytes` klein und prüft, dass eine
     zweite Datei entsteht.
   - `LoggerConfig` zusätzlich um `std::vector<LogField> baseFields` erweitern; `log()` hängt sie an
     jedes `fields{}` an (oder als Top-Level `session`). `InitializeEngineLogging` setzt
     `session = AppLog::sessionId()`.
3. **App-seitiger strukturierter Entry-Point.** Neuer Header `app/diagnostics/StructuredLog.h`:
   `logEvent(LogSeverity, subsystem, event_code, std::initializer_list<LogField>)`. Er forwardet
   **ausschließlich** an `recorder_core::logging::log(level, subsystem, event_code, fields)` → landet
   mit Session-ID im JSONL. Die vertraute AppLog-Freitextzeile erzeugt **er nicht selbst**, sondern
   der bereits existierende `EngineLogBridge`-Sink (`EngineLogBridge.cpp`), der **jeden** Engine-Record
   flacht: `component`→AppLog-category, `Flatten(record)`→`message = event_code` + `key=value`-Anhang.
   Das ist byte-genau das in dieser Spec gewünschte Freitextformat — der Sink produziert es also
   **schon**.
   - **Warum nicht zusätzlich direkt in AppLog schreiben (Anti-Muster, ausdrücklich verworfen):**
     Ein direkter `AppLog::write` **und** ein Forward an den Engine-Log würden **zwei nahezu
     identische Zeilen** je Event in History **und** `exosnap.log` erzeugen — der Forward läuft durch
     denselben Bridge-Sink zurück in AppLog. `logEvent` schreibt daher **nur** in den Engine-Log; die
     Bridge bleibt die einzige Quelle der Freitextzeile (ein Record → ein JSONL-Eintrag → eine
     AppLog-Zeile).
   - `event_code` ist ein **stabiles, locale-unabhängiges Token** (z.B. `record.start`,
     `encoder.init`, `audio.discontinuity`, `mux.finalize`, `disk.hardstop`). Kein Enum-Zwang (offene
     Menge), aber Konventionsliste in der ADR.
   - Migration ist **nicht** verpflichtend: bestehende `AppLog::info`-Callsites bleiben; nur neue
     bzw. gezielt hochwertige Ereignisse (Record-Start/Stop, Encoder-Init, Fehlerphasen) wandern auf
     `logEvent`. Keine MVP-Expansion.
4. **Level-Konvention & Kopplung der beiden Filter (sonst still verloren).** Zwei **unabhängige**
   Level-Filter greifen: der Engine-Logger filtert auf `minimumLevel = Info` (`logging.cpp` +
   `EngineLogBridge.cpp` setzt `Info`), der AppLog hat einen separaten Developer-`min_severity`
   (nullopt = „Off").
   - **JSONL wird ausschließlich vom Engine-`minimumLevel` gesteuert**, **nicht** vom AppLog-Level.
     Setzt der Nutzer den Developer-Log-Level auf „Off", schweigt nur die `exosnap.log`-Freitextzeile
     (der Bridge-Sink-`AppLog::write` verwirft sie); das JSONL zeichnet bei `>= Info` **weiter** auf.
     Bewusst so: der strukturierte Support-Stream soll nicht am UI-Log-Schalter hängen (Offene Frage 5
     ist damit entschieden — JSONL immer an, `minimumLevel = Info`).
   - **`logEvent`-Konvention: `>= Info`.** Wegen `minimumLevel = Info` erreicht ein
     `logEvent(Debug, …)` weder JSONL noch (über die Bridge) AppLog — es verschwände lautlos. Die
     ADR-Konventionsliste schreibt deshalb `Info`/`Warn`/`Error` für `event_code`-Ereignisse vor.
     `minimumLevel` bleibt bei `Info` (kein Debug-Rauschen im Support-Artefakt).

Ergebnis-Schema pro JSONL-Zeile (unverändert außer `session`):
`{timestamp_unix_ms, level, component, message, fields{session, event, <k=v>...}}`.

### D2 — Session-Report-Artefakt `session-<recording-id>.json`

**Entscheidung:** „Session" im Report = **Recording**-Session (nicht Launch). Der Report wird bei
Recording-Abschluss/-Fehler **neben dem Log** geschrieben: `<logdir>/reports/session-<id>.json`
(Unterordner, damit die Log-Rotation ihn nicht anfasst). Reiner Datenschreiber, atomar via `QSaveFile`.

**Die Report-ID ist NICHT `current_manifest_id_`** (naheliegend, aber unbrauchbar). Prüfung des Codes:
`current_manifest_id_` wird (a) nur **innerhalb** `if (recovery_manifest_store_ != nullptr)` gesetzt
(`RecordingCoordinator.cpp` ~952–965; der Store ist laut Header nullable), (b) **vor** jedem
`PostResult` geleert (`current_manifest_id_.clear()` an mehreren Stellen: „now owned by the job",
Remove-Pfade, Fehlerpfade) und (c) bei Splits **pro Segment neu vergeben**. Zum Report-Schreibzeitpunkt
in `PostResult` ist sie also regelmäßig leer oder segment-lokal. Deshalb schreibt diese Spec einen
**dedizierten `recording_session_id_`-Member** vor, der **am `StartRecording` unabhängig vom
Recovery-Store** einmal via `QUuid::createUuid()` gesetzt und **nicht** vor `PostResult` geleert wird
(erst am nächsten `StartRecording` überschrieben). Bei Splits bleibt er über alle Segmente **stabil**
(Segment-Identität trägt weiter die Manifest-ID; der Report korreliert die ganze Aufnahme). Details in
D2b.

Abgewogen: Report **neben die Ausgabedatei** legen (verworfen: verschmutzt den Nutzer-Ausgabeordner,
und der Ordner kann ein Netzlaufwerk / Wechselmedium sein) vs. **neben den Log** (gewählt: zentral,
mit den Logs zusammen im Bundle, unter App-Data-Kontrolle).

Inhalt (alles bereits vorhanden außer Encoder-Init — s. D2a):

- `schema_version`, `recording_session_id`, `launch_session_id`, `started_at`, `ended_at`.
- **Resolved Output-Format** aus `UiRecordingResult`: container/video_codec/audio_codec
  (Schreibweise via `CodecLabels.h`-Kanon), source/output w/h, fps num/den, cfr, output_file_bytes,
  elapsed_seconds.
- **Resolved Config-Snapshot:** aus `ConfigSummary`/`UserRecorderConfig` (rate control, bitrate,
  bit depth, chroma, HDR-Modus, color range, audio sample_rate/channels, DSP-Stages).
- **Capture-Backend:** DXGI-OD vs. WGC (aus der Session/`CaptureSourceType`).
- **Encoder-Init-Parameter:** kompakt (s. D2a).
- **Counter aus dem finalen Snapshot:** drops (coalesced/cfr/backpressure), duplicated, audio
  discontinuities, `duration_skew_ms`, **finale** `av_drift_ms` und **Peak** `av_drift_ms` (+
  availability als „unavailable" statt Fake-0), encoder submitted/encoded/backlog/forced_keyframes,
  mux failures.
  - **Peak-Drift-Herkunft (Policy-Ort, sonst dupliziert):** Der Snapshot trägt heute nur den
    **Momentanwert** `av_drift_ms`; die Peak-Akkumulation ist aktuell **UI-Logik in `RecordPage`**
    (`peak_av_drift_ms_`, gepflegt über die PostDiagnostics-Callbacks). Diese im Report-Writer
    nachzubauen würde Policy-Logik duplizieren (explizites Review-Kriterium in CLAUDE.md). Diese Spec
    **verschiebt die Peak-Akkumulation in den Engine-Aggregator** (`pipeline_diagnostics_aggregator`),
    sodass der Snapshot ein neues Feld `peak_av_drift_ms` + `peak_av_drift_availability` trägt (Engine
    = single source of truth, konsistent damit, dass `av_drift_ms` bereits eine Engine-Metrik ist).
    **`RecordPage` stellt auf `snapshot.peak_av_drift_ms` um** und entfällt als eigener Akkumulator;
    der Report liest denselben Wert aus dem finalen Snapshot. Details/Plumbing in D2b, Snapshot-Feld
    in Schritt 2.
- **Segment-Liste:** aus `UiRecordingResult::segments[]` (Index, Dauer, Bytes, finalisiert).
- **Fehlerphase:** `error_phase`, `hresult_text`, `error_detail` (bei Fehlschlag).

**Privacy:** Der Report enthält **keine** absoluten Pfade und **keinen** Dateinamen (nur `_bytes`,
Codecs, Zähler). Falls überhaupt ein Pfad nötig wäre, durch `ScrubString`. (Offene Frage 2.)

Retention: die **N zuletzt** Reports behalten (Vorschlag N=10), älteste beim Schreiben pruns — analog
zur Log-Rotation, damit der Ordner nicht wächst.

Pure, testbare Kernfunktion: `BuildSessionReportJson(const SessionReportInputs&) -> QByteArray`
(**`QJsonDocument`/`QJsonObject`, NICHT nlohmann**), UI-frei, in `app/diagnostics/` (App-Layer, da sie
App-Typen wie `UiRecordingResult` konsumiert). Der Schreiber (`QSaveFile` + Prune) ist eine dünne
Schale drumherum.

**JSON-Bibliothek im App-Layer = Qt-JSON (bewusst).** `nlohmann::json` ist an `recorder_core`
**PRIVATE** gelinkt und im gesamten `app/`-Baum bisher **ungenutzt** (Grep: 0 Treffer). Es über eine
neue `target_link_libraries(app … nlohmann_json)`-Zeile in die UI-Schicht zu ziehen, wäre unnötige
Kopplung: der App-Layer ist durchgehend Qt, und die etablierten App-JSON-Schreiber
(`RecoveryManifestStore`, `recording-history.json`) nutzen bereits `QJsonDocument`. **Alle
App-Layer-JSON dieser Spec** (Session-Report **und** die Bundle-Summaries capability/adapters/
displays/manifest in D3) verwenden daher `QJsonDocument`. `nlohmann` bleibt ausschließlich
Engine-intern (`logging.cpp`).

#### D2a — Encoder-Init-Parameter UI-agnostisch bereitstellen

Abgewogen:

- **Alt-A: die rohe `NV_ENC_INITIALIZE_PARAMS`-Struktur an den App-Layer durchreichen.** *Verworfen:*
  leckt Engine-Internas in die UI-Schicht, gegen die Architektur-Leitplanke.
- **Alt-B: Init-Parameter aus dem JSONL-Log zurücklesen** (Encoder loggt `encoder.init`). *Verworfen:*
  fragil (Ring-Buffer-Verdrängung, String-Parsing), Report würde vom Log-Format abhängen.
- **Alt-C (gewählt): eine kleine, immutable `EncoderInitInfo`-Plain-Struct** an
  `RecordingDiagnosticsSnapshot` anhängen, vom Engine-Encoder **einmalig** bei Encode-Start befüllt
  und auf jedem Snapshot mitgeführt. Felder: `codec, preset (P1..P7), rc_mode, target_bitrate_kbps,
  max_bitrate_kbps, gop_length, bframes, lookahead_frames, temporal_aq, spatial_aq, profile,
  bit_depth, chroma, color_range, hdr_mode`. Der App-Layer bekommt den finalen Snapshot ohnehin →
  Init-Info „gratis", ohne Log-Scraping, Engine bleibt UI-agnostisch (reine Daten).
  - Zusätzlich sollte der Encoder denselben Satz einmalig als `logEvent(Info, "encoder", "encoder.init", …)`
    ins JSONL schreiben (D1), damit die Parameter auch bei einem *fehlgeschlagenen* Start (kein
    finaler Snapshot) im Bundle liegen.

#### D2b — Coordinator-Plumbing: Report-Inputs in `PostResult` verfügbar machen

Der Report entsteht in `RecordingCoordinator::PostResult`. Damit dieser Schritt **ausführbar** ist
(nicht „mit finalem Snapshot" als Handwink), schreibt die Spec drei konkrete Coordinator-Änderungen
vor — je eine Lücke im heutigen Code:

1. **`recording_session_id_`-Member** (s. D2 „Entscheidung"): am `StartRecording` gesetzt, unabhängig
   vom `recovery_manifest_store_`, **nicht** vor `PostResult` geleert. Liefert die Report-ID auch dann,
   wenn kein Recovery-Store existiert (Store nullable) oder `current_manifest_id_` bereits „owned by
   the job" ist.
2. **Coordinator stasht den finalen Snapshot.** Heute reicht `PostDiagnostics` den Snapshot nur an die
   UI durch; der Coordinator hält **keinen** Diagnostics-Snapshot (`last_completed_snapshot_` lebt in
   `RecordPage`). Die Spec ergänzt einen Coordinator-Member `last_snapshot_` (unter dem vorhandenen
   `diagnostics_guard_mutex_`), den `PostDiagnostics` bei **jedem** akzeptierten Snapshot aktualisiert.
   - **Ordering-Aussage (verbindlich):** Der Stop-Pfad emittiert den finalen Snapshot mit
     `lifecycle == Completed` **vor** dem zugehörigen `PostResult`. `PostResult` liest `last_snapshot_`;
     ist der jüngste ein `Completed`-Snapshot, trägt der Report die End-of-Session-Zähler. Existiert
     auf einem **Fehlerpfad** kein `Completed`-Snapshot, nutzt der Report den zuletzt gestashten
     Snapshot und markiert die betroffenen Metriken über ihre `MetricAvailability` als „unavailable"
     (nie Fake-0). So ist der Report auch bei Abbruch wohldefiniert.
3. **Peak-`av_drift_ms` als Engine-Snapshot-Feld** (s. D2 Content-Bullet + Schritt 2): die
   Akkumulation wandert aus `RecordPage` in den Engine-Aggregator; der finale Snapshot trägt
   `peak_av_drift_ms`. Der Report liest ihn aus `last_snapshot_`, `RecordPage` liest denselben Wert —
   **eine** Policy-Quelle.

### D3 — Ein-Klick-Support-Bundle (ZIP, scrubbed, Allowlist)

**Entscheidung:** Bundle-Builder als **pures, testbares** Kernmodul + dünne UI-Aktion. ZIP via
**miniz** — und zwar **keine neue Lib und kein Heben aus `libs/update`**: `exosnap_miniz` ist bereits
ein **eigenständiges, linkbares STATIC-Target** mit PUBLIC-Include (`libs/update/CMakeLists.txt`), das
jedes Target heute schon linken kann. Der minimale Pfad ist **eine neue `ZipWriter`-Übersetzungseinheit
im Bundle-Modul**, die `exosnap_miniz` linkt und `mz_zip_writer_*` kapselt; die Zip-Slip-sichere
`IsSafeZipEntryName` wird aus `libs/update/.../zip_extract.h` **wiederverwendet** (kein Reimplement).
Kein `libs/support_bundle`/`libs/archive` — das wäre spekulative Struktur ohne zweiten Nutzer.
`libs/update`-Verhalten (Extraktion + Guard) bleibt unangetastet.

Bundle-Inhalt (jeder Textinhalt läuft durch **`ScrubString`**, jede strukturierte Zusammenfassung
folgt der **Allowlist**-Denkweise — nur bekannte Felder aufnehmen, nicht „alles minus Blocklist"):

- `exosnap.log` + `exosnap.log.1/.2` (die rotierten Freitext-Logs, gescrubbt).
- `engine.jsonl` (+ rotierte, gescrubbt zeilenweise).
- Die letzten N `reports/session-*.json`.
- `capability.json` — aus `CapabilitySet`/`RuntimeCapabilitySnapshot` (GPU-Adapter, NVENC-DLL/-API,
  per-Codec-Probes, OS build/version, MF-AAC/Webcam).
- `adapters.json` — `EnumerateAdapters()` (Name, Vendor, Kind, IDs, VRAM) + `driver_version`.
- `displays.json` — Display-Topologie: `displays[]` (name, hdr_active, bits_per_color, Luminanz,
  Primaries) als Snapshot der Monitor-Landschaft.
- `settings.txt` — **gescrubbte** Zusammenfassung aus `ConfigSummary` (KEIN Roh-Dump von
  settings.ini, weil dort der Ausgabepfad steht → Pfad wird gescrubbt bzw. weggelassen).
- `bundle-manifest.json` — Erstellzeit, App-Version/Channel/Commit, `launch_session_id`,
  Scrubber-Version, Liste der enthaltenen Dateien, Hinweistext „no telemetry, created on user
  action".

**Nicht enthalten (bewusst):** die Recordings selbst, settings.ini/presets.ini/recording-history.json
roh, absolute Pfade, Username/Machine, Crashdumps (die haben ihren eigenen consent-gated Kanal).

**Scrubbing-blinder Fleck — Fenstertitel des Capture-Targets (explizit entschieden).** `ScrubString`
deckt **nur** Pfade/Username/Machine ab (`StripWindowsPaths` matcht Laufwerks-/UNC-Präfixe). Der
App-Log enthält bei **jedem** Aufnahmestart die Zeile `start backend=<b> target="<beschreibung>"`
(`RecordingCoordinator.cpp` ~973–974), und `<beschreibung>` ist bei Fenster-Capture der **Fenstertitel**
(z.B. `"privates-dokument.docx - Word"`). Ein Titel **ohne** Laufwerkspräfix passiert `ScrubString`
**ungeschoren** — die naive Schritt-5-Assertion („kein `C:\`, kein Username/Machine, kein
Ausgabepfad") wäre grün, während personenbezogene Titel im Bundle lägen. **Entscheidung: redigieren,
nicht dulden.** Der Bundle-Collector wendet **zusätzlich** zu `ScrubString` eine bundle-lokale Regel
`RedactCaptureTargets()` auf jeden Log-Text-Entry (Freitext **und** JSONL) an, die den Wert innerhalb
`target="…"` bzw. das JSONL-`target`-Feld durch `[capture-target]` ersetzt (Backend/`event_code`
bleiben erhalten — für Support genügt „Fenster- vs. Monitor-Capture", nicht **welches** Fenster). Die
Regel lebt **bundle-lokal** (nicht in `crash_scrubber`), um den consent-gated Crash-Pfad nicht
mitzuändern. Titel gelten damit **nicht** als akzeptierter Bundle-Inhalt; im `bundle-manifest.json`
und in PRIVACY.md wird die Redaction benannt, und sie ist ein expliziter Punkt der Schnittstelle zur
`privacy-review-spec`.

**Aufruf-Ort (Produktentscheidung):** Primär-Aktion **„Create support bundle"** auf der **Logs-Seite**.
Ist-Stand der Logs-Toolbar (frisch geprüft, `LogsPage.h`): sie trägt heute **genau Copy + Export**;
die früheren Refresh-/Open-folder-/Clear-**Buttons** wurden aus der Toolbar entfernt („D3: cut from
toolbar"), „Open folder" ist nur noch ein `folder_link_`-Label (`onOpenFolder()`-Slot bleibt). Der neue
Button reiht sich also **neben Copy + Export** ein. Logs ist der Support-Log-Heimatort (product-spec:57);
nicht Expert-gated (Support braucht es der normale Nutzer). Zusätzlich den bereits vorhandenen, disabled
`Export Report`-Button der Diagnostics-Seite (`DiagnosticsPage.cpp:197-202`) **aktivieren** und auf
dieselbe Aktion routen (zweiter Einstieg, kein zweiter Codepfad). Nach dem Schreiben: „Show in folder"
(kein Auto-Upload — konsistent mit No-Telemetry). Ablage-Vorschlag:
`<Desktop>/exosnap-support-<yyyymmdd-hhmmss>.zip` bzw. via Save-Dialog. (Offene Frage 1.)

Ruhig, nicht alarmistisch: die Aktion ist ein neutrales Werkzeug, kein Fehler-Trigger; Wording
„Create a diagnostic package to share with support", keine roten Warnungen.

### D4 — Startup-Trace als Tabelle in Logs/About

**Entscheidung:** einen leichten, prozessweiten **`StartupTrace`-Collector** neben `StartupClock`
(header-only Muster beibehalten): `record(label, elapsed_ms)` sammelt die schon existierenden
Meilensteine (`main`-Start, QSS, Fonts, first-paint, preview-live, Page-Hydration-Ticks) in einer
kleinen geordneten Liste. Rendering:

- **Logs-Seite:** eine kompakte, ausklappbare Tabelle „Startup" (Label · ms), read-only.
- **About-Seite** (optional, Offene Frage 4): dieselbe Liste als Diagnose-Fußnote.

Kein neues Timing einführen — nur die vorhandenen `StartupClock`-Lesungen zusätzlich in den Collector
schreiben (statt sie nur zu loggen). Die Tabelle wandert außerdem ins Bundle (`startup-trace.txt`).

Abgewogen: die Zahlen aus den Log-Zeilen zurückparsen (verworfen: fragil) vs. beim Messen direkt in
eine Struktur schreiben (gewählt: trivial, robust).

### D5 — `docs/troubleshooting.md`

**Entscheidung:** kuratiertes tracked Doc (docs/ = kuratiert). Top-Symptome →
Diagnostics-Karte, die es meldet → Fix. Deckt die vier Support-Top-Tickets (Review §6.8) und die
weiteren häufigen Fälle ab, jeweils mit Verweis auf die reale Diagnostics-Karte / FixAction, die
schon existiert. Struktur je Eintrag: **Symptom · Was ExoSnap dazu anzeigt · Was zu tun ist · (falls
sinnvoll) Support-Bundle erwähnen**. Beispiele:

1. Kein Ton (44,1-kHz-Endgerät → Format-Mismatch; Endpoint-Unplug) — Audio-Karte.
2. Schwarzbild (Exclusive-Fullscreen / Format-Negotiation) — Display/Capture-Karte.
3. „Encoder unavailable" (Treiber-Mindestversion) — Encoder-Blocker + External-FixAction-Deeplink.
4. Stottern/Judder (VRR vs. CFR, Encoder-bound p99) — Present/Latency-Diagnostics.
5. Aufnahme zu dunkel im VLC (Full-Range) — Color-Range-Notice + Full→Limited-Fix.
6. Update installiert nicht — Updater-Fehlerzustände.
7. Recording gestoppt (Low-Disk / FAT32-4-GiB) — Disk-Karte.
8. „Meine Logs teilen" — Verweis auf das Support-Bundle.

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz. Reihenfolge = Abhängigkeit.

**Schritt 1 — Log-Schema-Foundation (D1).**
Dateien: `app/diagnostics/AppLog.{h,cpp}` (Session-ID + `sessionId()`, Banner),
`libs/recorder_core/include/recorder_core/logging/logging.h` (+`baseFields` **und
`maxFileBytes`/`maxFileCount`** in `LoggerConfig`), `libs/recorder_core/src/logging/logging.cpp`
(`rotating_file_sink_mt` mit den Config-Schwellwerten + baseFields), neues
`app/diagnostics/StructuredLog.{h,cpp}` (`logEvent` — forwardet **nur** an `recorder_core::logging::log`,
**kein** direkter `AppLog::write`), `app/diagnostics/EngineLogBridge.cpp` (session-Feld setzen).
Konventionsliste der `event_code`-Tokens (`>= Info`) in der ADR.
Test: JSONL-Zeile enthält `session`; Rotation greift bei **klein gesetztem `maxFileBytes`** (zweite
Datei entsteht); **`logEvent` erzeugt genau EINE AppLog-Zeile** (über den Bridge-Sink) **und einen
Engine-Record** — Regressionstest gegen Doppel-Zeilen (Sink-Spy + AppLog-History-Count == 1 je Event);
`logEvent(Debug, …)` erzeugt bei `minimumLevel=Info` **keine** Ausgabe.

**Schritt 2 — `EncoderInitInfo` + `peak_av_drift_ms` auf dem Snapshot (D2a + D2b-Peak).**
Dateien: `libs/recorder_core/include/recorder_core/pipeline_diagnostics.h` (neue `EncoderInitInfo`-Struct
+ Feld; zusätzlich `peak_av_drift_ms` + `peak_av_drift_availability`), Engine-Encoder-Befüllung
(`nvenc_encoder.cpp` / der `SessionStatsCollector`-Pfad) für `EncoderInitInfo`, **Peak-Akkumulation im
`pipeline_diagnostics_aggregator`** (laufendes Maximum von `|av_drift_ms|` pro Session), zusätzlich
`encoder.init`-`logEvent`. **`RecordPage` stellt von `peak_av_drift_ms_` auf `snapshot.peak_av_drift_ms`
um** (Akkumulator dort entfällt). Test: Snapshot trägt nach Encode-Start stabile Init-Werte; Aggregator
liefert monoton wachsenden Peak; JSONL enthält `encoder.init`.

**Schritt 3 — Session-Report-Writer + Coordinator-Plumbing (D2 + D2b).**
Dateien: neu `app/diagnostics/SessionReport.{h,cpp}` (pure `BuildSessionReportJson(...) -> QByteArray`
über **`QJsonDocument`**, + `WriteSessionReport` mit `QSaveFile` + Prune N), `RecordingCoordinator.{h,cpp}`:
(a) neuer Member **`recording_session_id_`** (am `StartRecording` gesetzt, unabhängig vom Recovery-Store,
nicht vor `PostResult` geleert), (b) neuer Member **`last_snapshot_`** (unter `diagnostics_guard_mutex_`,
in `PostDiagnostics` bei jedem akzeptierten Snapshot aktualisiert), (c) Report-Aufruf im `PostResult`-Pfad
mit `UiRecordingResult` + `last_snapshot_` + resolved config. Ordering wie D2b (Completed-Snapshot vor
`PostResult`; Fehlerpfad → letzter Snapshot + `unavailable`-Markierung).
Test (CI): Fixture-Inputs → deterministisches JSON (Feldpräsenz, `unavailable` statt Fake-0,
Segment-Liste, Fehlerphase, `peak_av_drift_ms`); Prune behält N; Report-ID-Stabilität bei `store==nullptr`.

**Schritt 4 — Zip-Writer bereitstellen (Vorarbeit D3).**
**Kein neues Lib-Target und kein Heben aus `libs/update`.** Neue `ZipWriter`-Übersetzungseinheit im
Bundle-Modul, die das **bestehende `exosnap_miniz`-Target** linkt (STATIC, PUBLIC-Include) und
`ZipWriter::AddFileFromMemory(name, bytes)` über `mz_zip_writer_*` bereitstellt; `IsSafeZipEntryName`
aus `libs/update/.../zip_extract.h` wiederverwenden. `libs/update` bleibt unverändert. Test: Zip
schreiben → mit `zip_extract` round-trippen; Entry-Namen sicher.

**Schritt 5 — Support-Bundle-Builder (D3, Kern).**
Dateien: neu `app/diagnostics/SupportBundle.{h,cpp}`: pure `CollectBundleEntries(...) ->
vector<BundleEntry{name, bytes}>` (Logs gescrubbt, capability/adapters/displays/settings-JSON **über
`QJsonDocument`**, manifest) + `WriteBundleZip(path, entries)` (via ZipWriter aus Schritt 4). Text-Entries:
**`ScrubString` UND `RedactCaptureTargets()`** (Fenstertitel in `target="…"`/JSONL-`target` →
`[capture-target]`); strukturierte JSON nur Allowlist-Felder. Test (CI): Bundle aus Fixture-Snapshots →
erwartete Einträge; **Scrubber-Assertion: kein `C:\`, kein Username/Machine, kein Ausgabepfad** UND
**der Fenstertitel-Fixturewert** (`"privates-dokument.docx - Word"`) taucht in **keinem** Entry auf —
diese Titel-Assertion ist der Regressionsschutz gegen den in D3 benannten blinden Fleck.

**Schritt 6 — Bundle-UI-Aktion (D3, UI).**
Dateien: `app/pages/LogsPage.{h,cpp}` (Button „Create support bundle" **neben den vorhandenen
Copy/Export-Buttons** — Open-folder ist heute nur das `folder_link_`-Label, es gibt keinen Clear-Button
mehr; + Save-/Show-in-folder), `app/pages/DiagnosticsPage.cpp` (`export_report_btn_` enablen + auf
dieselbe Aktion routen, Tooltip ersetzen), MainWindow-Verdrahtung (liefert `runtime_caps_`,
Adapter/Display-Snapshot, ConfigSummary). Test (Widget): Button vorhanden/enabled; Klick ruft Builder
mit nicht-leeren Inputs (kein Live-App).

**Schritt 7 — Startup-Trace-Collector + Tabelle (D4).**
Dateien: neu `app/diagnostics/StartupTrace.h` (header-only Collector), Aufrufe an den vorhandenen
`StartupClock`-Lesepunkten (`main.cpp`, `MainWindow`, `RecordPage`, `DevicePage`,
`PageHydrationController`), Tabelle in `LogsPage`. Bundle-Eintrag `startup-trace.txt`. Test:
Collector ordnet Einträge, Tabelle rendert (Widget-Test); Formatter deterministisch.

**Schritt 8 — `docs/troubleshooting.md` (D5).**
Nur Doku. Verweise auf reale Karten/FixActions gegenprüfen.

**Schritt 9 — Spec-/ADR-/Doku-Sync.**
`docs/decisions/0044-diagnostics-support-bundle.md` (Log-Schema-Entscheidung, Session-Report,
Bundle-Scope + Scrubbing, event_code-Konvention); product-spec §11/Logs um Session-Report + Support-
Bundle + Startup-Trace-Tabelle ergänzen; `PRIVACY.md` um „Support bundle (local, user-initiated, no
transmission, scrubbed)" ergänzen; `KNOWN_LIMITATIONS.md` entsprechend nachziehen. Schnittstelle zur
`privacy-review-spec` (Bundle-Scrubbing als Checklistenpunkt) benennen.

## Test-/Verify-Plan

**CI-fähig (Unit/Widget, keine Live-App, kein echtes GPU):**
- D1: JSONL trägt `session`; **`logEvent` erzeugt genau eine AppLog-Zeile (kein Doppel) + einen
  Engine-Record**; `logEvent(Debug)` bei `minimumLevel=Info` = keine Ausgabe; Engine-Log-Rotation über
  klein gesetztes `LoggerConfig::maxFileBytes` (zweite Datei entsteht).
- D2: `BuildSessionReportJson` deterministisch aus Fixture (Feldpräsenz, `unavailable`-Semantik,
  Segmente, Fehlerphase, `peak_av_drift_ms`); Prune-Verhalten (N); Report-ID stabil bei `store==nullptr`.
- D2a: Snapshot trägt `EncoderInitInfo` nach Encode-Start (Fake-Encoder/Fixture); Aggregator-Peak monoton.
- D3/D4: ZIP-Round-Trip (`exosnap_miniz`-Writer → `zip_extract`); **Scrubber-Coverage-Test** über alle
  Bundle-Einträge (Regex-Assertion: kein Drive-Pfad/Username/Machine/Ausgabepfad **und kein
  Fenstertitel-Fixturewert** — `RedactCaptureTargets`); Startup-Trace-Formatter.
- Widget: Logs-/Diagnostics-Button vorhanden + enabled; Klick ruft Builder (gemockt).

**Nur User-live verifizierbar (nicht CI, echte Hardware/echte Aufnahme):**
- Ein echtes Recording erzeugt einen plausiblen `session-<id>.json` mit korrekten NVENC-Init-Werten
  der konkreten GPU.
- Ein reales Support-Bundle auf dem Entwickler-System: enthält korrekte GPU/Treiber/Display-Fakten,
  und ein manueller Blick bestätigt, dass **kein** persönlicher Pfad/Name durchrutscht (Scrubber-
  Realdaten-Check — der Unit-Test deckt nur synthetische Muster ab).
- Startup-Trace-Zahlen sind auf echter Hardware plausibel und stabil.
- (Bundle-Öffnen/Show-in-folder ist UI-Interaktion — der Entwickler klickt, der Agent nicht.)

**Bewusst NICHT gebaut:** kein Auto-Upload/Telemetrie; keine Migration alter Logs; kein Umbau der
`av_drift_ms`-Metrik (bereits echt, #191); keine neuen Diagnostics-*Checks* (Review §6.8 wird nur
dokumentiert, nicht als Runtime-Check dupliziert); keine Crashpad-Annotation-Erweiterung (§6.7 →
Crash/privacy-review-spec); keine Migration der 100+ bestehenden `AppLog::info`-Callsites auf
`logEvent` (nur neue/hochwertige Ereignisse).

## Risiken

- **Scrubbing-Lücke.** Der größte Vertrauensrisikopunkt. `ScrubString` deckt Pfade/Username/Machine
  ab, aber ein neues JSON-Feld könnte versehentlich einen Pfad tragen. Mitigation: Allowlist-Denkweise
  (nur bekannte Felder), zentraler Scrub-Pass über **jeden** Bundle-Text-Entry, plus der
  Coverage-Unit-Test + der Live-Realdaten-Check. Schnittstelle zur privacy-review-spec.
- **Fenstertitel im Log (konkreter blinder Fleck von `ScrubString`).** Die `start … target="<titel>"`-
  Zeile trägt bei Fenster-Capture den Fenstertitel, den `ScrubString` **nicht** matcht (kein
  Laufwerkspräfix). Mitigation: bundle-lokale `RedactCaptureTargets()`-Regel **zusätzlich** zum
  Scrubber + eigene Titel-Assertion im Schritt-5-Test. Ausdrücklich im Bundle-Manifest/PRIVACY.md und
  als privacy-review-Checklistenpunkt benannt.
- **Engine-Log-Truncate→Append-Wechsel** ändert Bestandsverhalten (bisher frische Datei pro Launch).
  Gewollt, aber Rotation muss sauber greifen; Test mit kleinem Schwellwert.
- **Session-ID-Verwechslung.** Launch-Session (Log-Korrelation) vs. Recording-Session (Report-Name).
  Beide klar benannt (`launch_session_id` / `recording_session_id`), im Bundle-Manifest dokumentiert.
- **miniz-Nutzung** ist rein additiv: ein neuer `ZipWriter` linkt das bestehende `exosnap_miniz`-Target
  (kein Heben, keine neue Lib). `libs/update`-Extraktion + Zip-Slip-Guard bleiben unverändert.
- **Bundle-Größe.** Bis ~15 MiB Logs × 2 Streams + Reports; unkritisch, aber Reports-Prune (N) und
  Log-Rotation halten es beschränkt.
- **`EncoderInitInfo`-Kopplung.** Neue Snapshot-Felder = Schema-Berührung; pre-1.0 ohne Migration
  ok. Nur Plain-Data, keine Engine-Internas.

## Offene Fragen (echte Produktentscheidungen)

1. **Bundle-Ablage & Abschluss:** fester Ziel-Ordner (Desktop) mit auto „Show in folder", oder
   Save-Dialog? Und: soll ein Klick optional direkt den GitHub-Issue-Report-Flow (Stage-0, ADR 0017)
   mit dem Bundle als Anhang-Hinweis öffnen, oder strikt nur die Datei erzeugen?
2. **Session-Report-Pfad:** den (gescrubbten) Ausgabe-Dateinamen mit aufnehmen (nützlich zum
   Korrelieren beim Support) oder komplett weglassen (strengste Privacy)?
3. **Report-Retention:** N zuletzt behalten (Vorschlag 10) — welcher Wert? (Hinweis: die Logs-Toolbar
   hat **keinen** Clear-Button mehr; `clearLogs()`/`onOpenFolder()` existieren noch als Slot bzw.
   `folder_link_`. Ein „Clear logs" ist also kein Toolbar-Trigger — falls Reports je mitgelöscht werden
   sollen, wäre der Ort zu entscheiden, sonst reicht das Prune-am-Schreiben.)
4. **Startup-Trace auf About:** nur Logs-Seite, oder zusätzlich als Diagnose-Fußnote auf About?
5. ~~**JSONL immer an**~~ **(entschieden in D1):** JSONL läuft dauerhaft, gesteuert nur vom
   Engine-`minimumLevel = Info`, entkoppelt vom AppLog-Developer-Level („Off" schaltet nur die
   Freitextzeile stumm, nicht das JSONL). Keine offene Frage mehr.

## Adversarialer Review — Ergebnis

Alle sieben Einwände wurden zuerst gegen den Code auf `main` geprüft und als belegt bestätigt; alle
eingearbeitet.

- **[major] D1 Doppel-Zeile im AppLog** — *eingearbeitet.* Bestätigt: der `EngineLogBridge`-Sink
  flacht **jeden** Engine-Record in eine AppLog-Freitextzeile; ein direkter `AppLog::write` **plus**
  Forward hätte doppelt geloggt. D1 Punkt 3 revidiert: `logEvent` forwardet **nur** an den Engine-Log,
  die Bridge bleibt die einzige Quelle der Freitextzeile. Schritt 1 + Testplan mit
  Doppel-Zeilen-Regressionstest (History-Count == 1).
- **[major] Session-Report in `PostResult` nicht ausführbar (3 Lücken)** — *eingearbeitet.* Bestätigt:
  `current_manifest_id_` nur unter `if (store != nullptr)`, vor `PostResult` geleert, per-Segment neu;
  Coordinator hält keinen Snapshot (`last_completed_snapshot_` lebt in `RecordPage`); Peak-Drift ist
  UI-Akkumulation. Neuer Abschnitt **D2b** + revidierte D2/Schritt 2/3: dedizierter
  `recording_session_id_`, Coordinator-`last_snapshot_` mit verbindlicher Ordering-Aussage,
  Peak-`av_drift_ms` in den Engine-Aggregator verschoben (Snapshot-Feld), `RecordPage` darauf umgestellt.
- **[major] Scrubbing-blinder Fleck: Fenstertitel** — *eingearbeitet.* Bestätigt: `start … target="<titel>"`
  (`RecordingCoordinator.cpp` ~973–974) trägt den Fenstertitel; `ScrubString` matcht ihn nicht.
  Entscheidung getroffen: bundle-lokale `RedactCaptureTargets()`-Regel, eigener Schritt-5-Test
  (Titel-Fixture absent), Risiko + PRIVACY.md/Manifest + privacy-review-Schnittstelle benannt.
- **[minor] Level-Interaktion offen** — *eingearbeitet.* Bestätigt: Engine `minimumLevel=Info` verwirft
  `logEvent(Debug)`; AppLog-„Off" ist unabhängig. Neuer D1-Punkt 4 legt `logEvent`-Konvention `>= Info`
  fest und entscheidet Offene Frage 5 (JSONL läuft entkoppelt vom AppLog-Level weiter).
- **[minor] nlohmann PRIVATE an recorder_core** — *eingearbeitet.* Bestätigt: 0 Treffer im `app/`-Baum,
  PRIVATE-Link. Entscheidung: gesamter App-Layer-JSON dieser Spec nutzt **`QJsonDocument`** (konsistent
  mit `RecoveryManifestStore`/History); `BuildSessionReportJson` liefert `QByteArray`. Kein
  nlohmann-Link in `app`.
- **[minor] miniz bereits linkbar + LoggerConfig-Schwellwerte** — *eingearbeitet.* Bestätigt:
  `exosnap_miniz` ist eigenständiges STATIC-Target mit PUBLIC-Include; `LoggerConfig` hat keine
  Rotations-Schwellwerte. D3/Schritt 4 auf „neue `ZipWriter`-Unit linkt `exosnap_miniz`, keine neue
  Lib" reduziert; Schritt 1 + D1 um `maxFileBytes`/`maxFileCount` in `LoggerConfig` ergänzt (macht den
  Rotationstest überhaupt schreibbar).
- **[minor] Ist-Stands-Staleness in D3 (Logs-Toolbar)** — *eingearbeitet.* Bestätigt (`LogsPage.h`):
  Refresh/Open-folder/Clear-Buttons aus der Toolbar entfernt; nur Copy+Export + `folder_link_`-Label.
  D3-„Aufruf-Ort", Schritt 6 und Offene Frage 3 entsprechend korrigiert.

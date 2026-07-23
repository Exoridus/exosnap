# M-1: Async-NVENC-Pipeline + Perf-Messinfrastruktur

**Status:** Spec (umsetzungsreif, Rev. 2 nach adversarialem Review — s. Schlussabschnitt) ·
**Autor:** Fable, 2026-07-11 · **Basis:** main @ #192
**Zwei Stufen:** (1) Perf-Messinfrastruktur — wird IMMER gebaut. (2) Async-Encode/Submit-Ahead —
wird NUR gebaut, wenn die Stufe-1-Daten das Entscheidungsgate (§ Design D3) reißen. Die
Async-Entscheidung ist explizit datenbasiert; diese Spec enthält beide Ausgänge.
**Verortung:** „Perf & Qualität"-Welle (nach 0.9.0 Edit/Output/Save). `docs/roadmap.md` hat für
diese Welle noch keinen Versions-Slot (die Tabelle springt 0.9 → 0.10 Reliability-Hardening) —
beim Wellenstart als eigene 0.x-Version einordnen (Roadmap-Prinzip: Pre-1.0-Versionen sind
unabhängige Wellen) und die Roadmap-Tabelle ergänzen. Die Architektur-Guardrails (native SDKs,
`IVideoEncoder`-Hierarchie, kanonisches RC-Modell) werden von dieser Spec nicht berührt.

---

## Problem

NVENC läuft synchron und unpipelined: `EncodeFrame` submitted einen Frame und blockt sofort im
`nvEncLockBitstream` (doNotWait=0), bis der Bitstream fertig ist. Capture → Convert → Encode können
nicht über Frames überlappen; der Video-Thread verbringt pro Frame die volle Encode-Dauer wartend.
Bei 4K-Inhalten und/oder hohen Presets (P6/P7 sind seit ADR 0039 user-wählbar) frisst der Lock das
16,6-ms-Budget eines 60-fps-Ticks; der 8-Slot-Input-Ring ist beim Default-Preset (P4) faktisch
vestigial, weil nie mehr als ein Frame in-flight ist. Encoder-Rückstand äußert sich heute als
Backpressure-Drops und CFR-Timeline-Resync (ehrliche Drops seit #192, „sustained encoder lag"-Pfad),
d. h. als sichtbarer Qualitätsverlust statt als genutzte Pipeline-Tiefe.

Gleichzeitig gibt es **keine Datenbasis** für die Entscheidung, ob der Umbau den Aufwand wert ist:
Encode-Latenz wird zwar live gebracketed, aber nur als latest/average/peak über ein 2-s-Fenster,
nirgends persistiert, ohne Perzentile, ohne Frame-Time-Metrik des Video-Threads, ohne
Auswertewerkzeug. Das Review (M-1) fordert: erst messen (p99-Encode-Latenz, Frame-Time-Histogramm),
dann entscheiden.

Zusätzlich hängen an der heutigen Synchronität zwei stille Annahmen, die ein Async-Umbau brechen
würde und die deshalb vorab robust gemacht werden müssen:

1. **input-order == output-order** (PTS-Zuordnung über eine blinde FIFO, `outputTimeStamp` wird nie
   gelesen).
2. **Keyframe-Vorhersage an der Submission** (deterministische GOP-Phase) für die per-Keyframe
   HDR-SEI/OBU-Anhänge — Cues, Splits und Codec-Private-Extraktion konsumieren stromabwärts das
   Keyframe-Flag jedes Pakets.

---

## Ist-Zustand (frisch erhoben, main @ #192)

### Encoder: synchron, ein Output-Buffer

- `enableEncodeAsync` wird nie gesetzt: `NV_ENC_INITIALIZE_PARAMS p{}` wird in
  `NvencEncoder::InitEncoder` befüllt (`libs/recorder_core/src/nvenc_encoder.cpp:907-921`), das
  Feld bleibt beim Zero-Init 0 → Sync-Modus.
- **Genau ein** Bitstream-Buffer für den ganzen 8-Slot-Ring: `m_bitstreamBuffer`
  (`libs/recorder_core/src/nvenc_encoder.h:310`), erzeugt in `CreateBitstreamBuffer`
  (`nvenc_encoder.cpp:975-985`). Jede Submission übergibt denselben Buffer:
  `pic.outputBitstream = m_bitstreamBuffer` (`nvenc_encoder.cpp:1156`).
- `EncodeFrame` (`nvenc_encoder.cpp:1121-1245`): mapped den Slot, pusht PTS+Slot in zwei parallele
  FIFOs (`m_pendingPts`/`m_pendingSlots`, `nvenc_encoder.h:339-342`), submitted, und blockt bei
  `NV_ENC_SUCCESS` sofort in `LockAndConsumeBitstream` mit doNotWait=0
  (`nvenc_encoder.cpp:1196-1220`; Lock selbst `:1067-1115`, `nvEncLockBitstream` `:1074`,
  Bitstream-memcpy `:1110-1111`).
- `NV_ENC_ERR_NEED_MORE_INPUT` (P5–P7-Pipeline-Tiefe) wird gepuffert (`:1221-1228`,
  `m_needMoreInputCount`) und erst im `Flush`-Drain konsumiert. **Latenter Fehler:** In diesem
  Zustand sind mehrere Submissions gegen denselben einzelnen Output-Buffer ausstehend. Der
  vendorte Header formuliert die 1:1-Bindung Output-Buffer↔ausstehender Encode nur für den
  Async-Modus explizit (`completionEvent`-Kommentar, `third_party/nvidia/nvEncodeAPI.h:2586`);
  für den Sync-Modus empfiehlt der NVENC Programming Guide, so viele Output-Buffer vorzuhalten,
  wie Frames in-flight sind (mindestens 1 + Pipeline-Tiefe). Das Single-Buffer-Aliasing ist also
  nicht explizit verboten, aber SDK-seitig nicht gedeckt — es funktioniert heute empirisch, weil
  der Lock seriell konsumiert. Stufe 2 beseitigt das strukturell.
- Slot-Ring: `std::array<InputSlot, 8> m_slots` (`nvenc_encoder.h:313`), `AcquireFreeSlot`
  (`nvenc_encoder.cpp:1034-1045`). Läuft der Ring voll (nur bei P5–P7 überhaupt möglich), zählt der
  Video-Thread einen Slot-Stall und dropt: CFR-Pfad `libs/recorder_core/src/video_thread.cpp:
  2515-2519`, VFR-Pfad `:3081-3086`.
- Flush-Drain ist bereits bounded/non-blocking über die pure Policy `NextFlushDrainStep`
  (`nvenc_encoder.cpp:302-313`, Drain `:1251-1306`, Budget 2000 ms) — dieses Muster wird in
  Stufe 2 für Event-Waits generalisiert.
- Der vendorte SDK-Header hat alles für Async: `enableEncodeAsync`
  (`third_party/nvidia/nvEncodeAPI.h:2244`), `NvEncRegisterAsyncEvent`/`NvEncUnregisterAsyncEvent`
  (`:4089` ff.), `NV_ENC_PIC_PARAMS::completionEvent` (`:2586`), Capability-Bit
  `NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT` (`:1051`), `NV_ENC_LOCK_BITSTREAM::outputTimeStamp` (`:2690`)
  und `::pictureType` (`:2695`).

### Keyframe-Metadaten-Vorhersage und ihre Konsumenten

- Submission-seitige Vorhersage: `NextGopKeyframePhase` (pur, `nvenc_encoder.cpp:702-712`; genutzt
  in `EncodeFrame` `:1176-1178`) sagt deterministisch voraus, ob der gerade submittete Frame ein
  IDR wird — gültig NUR unter `frameIntervalP=1`, `enableLookahead=0` (erzwungen in
  `FetchPresetConfig`, `nvenc_encoder.cpp:801-805`). Die Vorhersage steuert ausschließlich das
  Anhängen der HDR10-SEI/OBU-Payloads an der Submission (`:1183-1191`) — NVENC nimmt SEI/OBU nur
  bei `nvEncEncodePicture` entgegen, ein nachträgliches Anhängen ist API-seitig unmöglich.
- Das **tatsächliche** Keyframe-Flag des Pakets kommt dagegen aus `lockBS.pictureType`
  (`nvenc_encoder.cpp:1106`) — also aus der Wahrheit des Treibers, nicht aus der Vorhersage.
- `pic.inputTimeStamp = m_frameIdx++` wird gesetzt (`:1192`), aber `lockBS.outputTimeStamp` wird
  **nie gelesen** — die PTS-Zuordnung Paket↔Frame läuft blind über die FIFO-Reihenfolge
  (`m_pendingPts.front()`, `:1085-1088`).
- Stromabwärtige Konsumenten von `pkt.keyframe` (alle bleiben unverändert, weil sie das
  Actual-Flag konsumieren):
  - Codec-Private-Extraktion am ersten Keyframe (`video_thread.cpp:1814-1853` in `routePacket`).
  - Split-Grenze: `SplitSentinel` wird unmittelbar vor dem geforceten Keyframe in die Mux-Queue
    gelegt, Bedingung `split_armed && pkt.keyframe` (`video_thread.cpp:1890-1900`; Arming
    `maybeArmSplit` `:1773-1793` ruft `nvenc.RequestKeyframe()`).
  - Matroska: Cluster-Start an Video-Keyframes ≥ 2000 ms (`matroska_stream_writer.cpp:51`), Cues
    inkrementell pro Keyframe (`:401`); Mux-Item `mp.is_key = payload.keyframe`
    (`mux_thread.cpp:368`).

### Video-Thread-Struktur (Encode-Callsites)

- Der Video-Thread instanziiert `NvencVideoEncoder` direkt (`video_thread.cpp:550`), konfiguriert
  über Setter (`:552-570`, Keyframe-Intervall `:562` — seit #181 verdrahtet, GOP via
  `ComputeGopLength`, `nvenc_encoder.cpp:671-679`).
- CFR-Pfad: Catch-up-Loop `while (currentElapsed100ns >= next_tick_100ns …)`
  (`video_thread.cpp:2512-2745`); Encode-Bracket existiert bereits: `OnEncodeSubmitted()` +
  steady_clock-Paar um `nvenc.EncodeFrame` → `OnEncodeLatency` (`:2723-2728`).
- VFR/Window-Pfad: identische Brackets an beiden Callsites (`:2988-2993` HDR-nativ, `:3059-3064`
  SDR).
- „Sustained encoder lag" resynct die CFR-Timeline mit ehrlichen Drops
  (`video_thread.cpp:2491-2507`).
- Mux-Queue ist seit dem H-4-Fix bounded (`session_internal.h:238-240`,
  `WaitForMuxQueueSpace` `:287`).

### Vorhandene Messinfrastruktur (und ihre Lücken)

- `PipelineDiagnosticsAggregator` mit `RollingTimeWindow` (Kapazität 256 Samples, Horizont 2 s;
  `pipeline_diagnostics_aggregator.h:26-102`); Encode-Fenster `encode_window_` (`:240`), Input
  `OnEncodeLatency` (`:172`). Aggregate: **nur** latest/average/peak (`:31-36`) — keine Perzentile.
- `EncoderDiagnostics` im Snapshot: `latest_ms/average_ms/peak_ms/output_fps/backlog` etc.
  (`libs/recorder_core/include/recorder_core/pipeline_diagnostics.h:152-165`). Snapshot-Kadenz
  ~5 Hz aus dem `SessionStatsCollector` (`session_stats_collector.cpp:46-64`).
- **Semantik-Falle heute:** Das Encode-Bracket misst bei P1–P4 (sofortiger Lock) die echte
  Encode-Dauer, bei P5–P7 (NEED_MORE_INPUT) aber nur die Submit-CPU-Zeit — dieselbe Metrik
  bedeutet je nach Preset etwas anderes. Stufe 1 definiert das sauber (§ D1).
- Es gibt **kein** Frame-Time-Bracket über den gesamten Tick (Composite + VP-Blt + Encode + Route);
  Compositor/VP-Blt/Encode werden einzeln gebracketed, die Summe inkl. Queue-Wartezeiten nicht.
- Nichts wird persistiert: weder periodische Perf-Records noch eine Session-Ende-Zusammenfassung.
  Engine-Log ist bereits strukturiertes JSONL via spdlog (`logging/logging.cpp:133-146`) nach
  `engine.jsonl` (`app/diagnostics/EngineLogBridge.cpp:49`) mit `LogField`-Key-Values.
- Vorbild für Milestone-Logging: `StartupClock` (`app/diagnostics/StartupClock.h:19-22`) +
  `AppLog::info("perf", "first-paint N ms")`-Zeilen (`app/main.cpp:143-144`,
  `MainWindow.cpp:1767-1768`).
- `scripts/dev/` enthält nur `gen-manifest-fixture.py` — kein Perf-Auswertescript.
- `IVideoEncoder` ist synchron geformt: `EncodeFrame` liefert 0 oder 1 Paket
  (`include/recorder_core/interfaces/IVideoEncoder.h:45-49`).

---

## Design

### D1 — Stufe 1: Perf-Messinfrastruktur

**Was gemessen wird (zwei Metriken, präzise definiert):**

1. **`encode_latency_ms`** — Zeit von Submit bis Bitstream-verfügbar für genau diesen Frame.
   - Sync-Modus (heute): identisch mit dem vorhandenen Bracket um `EncodeFrame` (Lock inklusive).
   - Bei NEED_MORE_INPUT (P5–P7): der Frame ist noch nicht fertig — es wird dann **kein**
     `encode_latency`-Sample für diesen Submit erzeugt, sondern erst, wenn sein Output konsumiert
     wird (Submit-Zeitpunkt wandert in die Pending-FIFO, s. u.). Dafür wird die Pending-FIFO um
     einen `submit_time`-Member erweitert.
   - **Transportkanal der Dauer** (die Encoder→Aggregator-Verdrahtung ist explizit Teil von S2,
     nicht implizit): `NvencEncoder` hat keine Referenz auf den Aggregator, und bei P5–P7 gehört
     das in `LockAndConsumeBitstream` konsumierte Paket zu einer FRÜHEREN Submission — die Dauer
     kann also nicht an der Callsite gebracketed werden. `EncodedVideoPacket`
     (`include/recorder_core/packet_types.h:8-12`) erhält ein Feld
     `double encode_latency_ms = -1.0;` (negativ = nicht verfügbar). `LockAndConsumeBitstream`
     füllt es aus `now - PendingFrame::submit_time`; die Video-Thread-Callsite meldet
     `OnEncodeLatency(pkt.encode_latency_ms)` nur bei Wert ≥ 0. Keine Signaturänderung an
     `IVideoEncoder::EncodeFrame` nötig; das Feld überlebt den S7-Vektor-Umbau und den
     Async-Reap (D4) unverändert — Stufe-1- und Stufe-2-Zahlen bleiben über denselben Kanal
     vergleichbar. Verhaltensneutral, aber zwei Dateien: `nvenc_encoder.h/.cpp` +
     `packet_types.h`.
   - Damit ist die Metrik **preset-unabhängig vergleichbar** und behält im späteren Async-Modus
     (Submit → Event-Signal) exakt dieselbe Bedeutung.
   - Zusätzlich getrennt: **`encode_submit_ms`** — reine CPU-Kosten des `EncodeFrame`-Calls
     (das heutige Bracket, umbenannt in der Semantik). Im Sync-Modus sind beide bei P1–P4 fast
     identisch; genau diese Differenz ist später der Async-Gewinn-Nachweis.
2. **`video_tick_ms`** — Gesamtdauer pro emittiertem Frame im Video-Thread: vom Beginn des
   Tick-Bodys (CFR: `video_thread.cpp:2513`; VFR: ab Slot-Acquire) bis nach `routePacket`.
   Das ist die Metrik, gegen die das Frame-Budget (16,67 ms @ 60 fps) geprüft wird — sie enthält
   Composite, Tonemap, VP-Blt, Encode und Mux-Queue-Wartezeit.

**Wie Perzentile berechnet werden — Alternativen:**

- **(a) Exakte Sample-Aufbewahrung whole-session.** Pro: exakte Quantile. Contra: unbounded
  (60 fps × 2 h ≈ 432k Samples × 2 Metriken), Auswertung erst am Ende, Allokationen im Hot-Loop
  denkbar. Verstößt gegen das Muster „bounded, allocation-free im Worker".
- **(b) Fixed-Bucket-Log-Histogramm.** Pro: O(1)-Insert ohne Allokation, konstanter Speicher
  (~64 × 8 B pro Metrik), mergefähig, trivial als JSON serialisierbar und im Script exakt
  reproduzierbar auswertbar; Quantilfehler durch Bucket-Breite begrenzt und bekannt (~9 % relativ
  bei Ratio 2^(1/8)). Contra: Quantile sind interpoliert, nicht exakt.
- **(c) P²-Algorithmus / t-digest.** Pro: kompakt. Contra: mehr Code, approximatives Verhalten
  schwerer zu testen, nicht mergefähig (P²), keine Rohverteilung fürs Script.

**Entscheidung: (b).** Neue pure, testbare Klasse **`LatencyHistogram`**
(`libs/recorder_core/src/perf_histogram.h`, header-only):

```
class LatencyHistogram {
    // 64 geometrische Buckets: lo = 0.05 ms, hi = 500 ms, ratio = (hi/lo)^(1/62),
    // Bucket 63 = Überlauf (> hi). Add(ms) = O(1), kein Alloc. Quantile(q) via
    // linearer Interpolation innerhalb des Buckets. count(), Merge(other), Clear().
    // BucketCounts() für die Serialisierung (Session-Summary + Script-Roundtrip).
};
```

Zusätzlich bekommt `RollingTimeWindow` eine `Percentile(now, q)`-Methode (kopiert ≤ 256 Samples in
einen Scratch, `nth_element`) — exakte Fenster-Perzentile für die 5-Hz-Live-Anzeige; bei 256
Elementen und 5 Hz vernachlässigbar. Die whole-session-Histogramme leben im Aggregator (unter
dessen vorhandenem `mutex_`), gefüttert von denselben `On…`-Inputs.

**Wohin die Zahlen fließen — Alternativen für die Persistenz:**

- **ETW/TraceLogging:** professionelles Tooling (WPA), aber neue Infrastruktur, nicht vom
  Privacy-Review abgedeckt, Auswertung erfordert Windows-Tooling statt eines simplen Scripts.
- **Engine-JSONL (vorhanden):** strukturiert, rotiert mit dem App-Log-Regime, landet später
  automatisch im Support-Bundle (Spec #10), Script-Auswertung trivial.

**Entscheidung: Engine-JSONL.** Drei Ausgabekanäle:

1. **Live-Snapshot (intern, ohne UI-Änderung in Stufe 1):** `EncoderDiagnostics` erhält
   `p50_ms`/`p99_ms` (Fenster-Perzentile der encode_latency); neues, kleines
   `VideoTimingDiagnostics` im Snapshot mit `tick_p50_ms/tick_p99_ms/tick_peak_ms/budget_ms` +
   `MetricAvailability`. **Kein neuer user-sichtbarer Wert in Stufe 1:** Die Kampagne soll erst
   klären, ob Encode-Latenz überhaupt produktrelevant ist — eine p99-Karte VOR dieser Antwort
   wäre genau die Diagnostics-Aufblähung, die die Diagnostics-Linie („ruhig, nur echte/gemessene
   Probleme") vermeidet. Die Diagnostics-Encoder-Karte und die zugehörige
   `product-spec` §Diagnostics-Ergänzung werden Teil der Gate-Entscheidung (ADR 0044): zeigt die
   Kampagne dauerhaft relevante Latenz, kommt der Wert als ruhiger Zusatzwert; sonst bleibt es
   log-only. Bis dahin: Snapshot-Felder sind reine Engine-Interna (Debug-/Testkonsum).
2. **Periodischer Perf-Record (alle 10 s, nur während Recording):** der `SessionStatsCollector`
   emittiert aus seinem 33-ms-Tick (`tick % 300 == 0`) einen `logging::log(Info, "perf",
   "video-pipeline-window", fields)`-Record: `encode_p50/p95/p99/peak`, `tick_p50/p95/p99/peak`,
   `output_fps`, `backlog`, Delta der Drop-Zähler (`dropped_backpressure` inkl. separatem
   `slot_stalls`-Feld, damit die Auswertung Stall- und Resync-Anteile trennen kann — s. G2),
   `preset`, `codec`, `WxH@fps`, `perf_schema=1`.
3. **Session-Ende-Summary:** beim Verlassen von `SessionStatsCollector::Run()` (Collector-Thread,
   nach der Loop) ein Record `"session-perf-summary"` mit den kompletten Bucket-Counts beider
   whole-session-Histogramme (als kommaseparierter String + Schema-Konstanten lo/hi/n), Gesamt-
   Frame-/Drop-/Stall-/Duplicate-Zählern, `duration_skew_ms` und der Encoder-Init-Kurzform
   (Codec/Preset/RC/GOP/Auflösung). Frames nach Collector-Stop (EOS-Drain) fehlen in der Summary —
   dokumentiert und für die Fragestellung irrelevant.

4. **Auswertescript:** `scripts/dev/analyze-encode-perf.py <engine.jsonl> [vergleich.jsonl]` —
   parst `component=="perf"`-Records, gruppiert per Summary-Record in Sessions, druckt pro Session
   eine Tabelle (p50/p95/p99/max beider Metriken, Drops, Budget-Überschreitungen) und bei zwei
   Dateien ein Vorher/Nachher-Delta. Reine Stdlib, keine Dependencies; `--json` für maschinelle
   Verarbeitung.

**Overhead-Budget:** 2 zusätzliche `steady_clock::now()`-Paare + 2 Histogramm-Inkremente pro Frame
(unter dem bestehenden Aggregator-Mutex, der pro Frame ohnehin mehrfach genommen wird) und ein
Log-Record alle 10 s. Kein neuer Thread, keine Allokation im Hot-Loop.

### D2 — Robustheits-Umbau der Keyframe-/Ordnungs-Annahmen (Teil von Stufe 2, Schritt 6)

Heute wird input-order==output-order **angenommen**; nach dem Umbau wird sie **verifiziert**:

- Die zwei parallelen FIFOs `m_pendingPts`/`m_pendingSlots` werden zu **einer** FIFO von
  `PendingFrame { uint64_t pts_ns; int32_t slot_idx; uint64_t input_ts; bool predicted_keyframe;
  steady_clock::time_point submit_time; int32_t out_idx /* Stufe 2 */; }` konsolidiert.
  (Der `submit_time`-Teil kommt schon in Stufe 1, s. D1.)
- Beim Konsum eines Outputs wird `lockBS.outputTimeStamp` gegen `PendingFrame::input_ts` des
  FIFO-Kopfs geprüft. **Gestuft, weil die Echo-Annahme unverifiziert ist:** Der Check setzt
  voraus, dass der Treiber `pic.inputTimeStamp` (gesetzt in `nvenc_encoder.cpp:1192`, mit
  `enablePTD=1`, `:920`) zuverlässig in `lockBS.outputTimeStamp` echot. Der Header deklariert
  das Feld nur als „Presentation timestamp associated with the encoded output"
  (`nvEncodeAPI.h:2690`) — das Echo ist plausibel, aber gegen die Zielhardware nicht belegt. Ein
  sofort-fataler Check auf einer unverifizierten Treiberannahme riskiert Falsch-Positiv-Abbrüche
  gültiger Aufnahmen. Deshalb:
  - **Phase 1 (S6, warn-first):** Mismatch → einmaliges Warn-Log (mit beiden Werten) + Zähler
    `output_ts_mismatches` in `EncoderDiagnostics`; die PTS-Zuordnung bleibt die FIFO (heutiges
    Verhalten, keine Regression möglich). Ein dedizierter Live-Check verifiziert das
    Echo-Verhalten auf der Zielhardware (alle drei Codecs, P4+P7, inkl. NEED_MORE_INPUT-Pfad):
    Erwartung `output_ts_mismatches == 0` über eine volle Session.
  - **Phase 2 (mit S8, erst nach bestandenem Live-Check):** Eskalation zu **fatalem
    Encode-Fehler** (`ErrorPhase::VideoEncode`) — eine Aufnahme mit vertauschten Timestamps ist
    stille Datenkorruption, genau die Fehlerklasse, die das Produkt ausschließen will. Besteht
    der Live-Check NICHT (Treiber echot nicht), bleibt der Check warn-only und der Async-Umbau
    stützt die Ordnungsverifikation stattdessen allein auf die Event-Reihenfolge (FIFO-Kopf pro
    signalisiertem Event) — das ist dann als Einschränkung in ADR 0044 festzuhalten.
  - (Für M-2/B-Frames wird diese Stelle später zu einer Map-Lookup per `outputTimeStamp`; die
    Struktur ist dann schon da — B-Frames setzen zwingend voraus, dass das Echo verifiziert ist.)
- `predicted_keyframe` (aus `NextGopKeyframePhase`) wird gegen `lockBS.pictureType` verglichen.
  Mismatch ist **nicht** fatal (die SEI/OBU landete dann auf einem Nicht-IDR — legal, nur
  off-cadence): einmaliges Warn-Log + Zähler `keyframe_prediction_mismatches` in
  `EncoderDiagnostics`, und die GOP-Phase wird vom Actual resynct (`m_frameInGop = 1` nach einem
  realen IDR, sonst weiterzählen). Damit ist die Vorhersage selbstheilend statt blind.
- `pkt.keyframe` bleibt, was es heute schon ist: das Actual aus `pictureType`. Cues, Splits,
  Codec-Private brauchen **keine** Änderung — das ist der Grund, warum dieser Umbau vor dem
  Async-Umbau isoliert landen kann und auch ohne Async wertvoll ist (er härtet den heutigen
  P5–P7-Pfad, der die Pipeline-Tiefe bereits nutzt).

### D3 — Stufe 2: Async-Encode — Alternativen und Entscheidung

- **(A) Sync beibehalten und dokumentieren.** Wenn die Messung zeigt, dass p99-Encode-Latenz auf
  der Zielhardware weit unter dem Budget liegt und Backpressure-Drops praktisch nicht vorkommen,
  ist Async Komplexität ohne Kundennutzen (Anti-Ziel: versteckte MVP-Expansion). Dann: ADR
  „Synchroner NVENC-Encode ist eine bewusste Entscheidung", KNOWN_LIMITATIONS-Absatz, und der
  Slot-Ring wird NICHT vereinfacht (er ist die Zero-Copy-Registratur der Texturen und trägt die
  P5–P7-Pipeline-Tiefe — „Ring vereinfachen" aus dem Review wäre nur bei einem P1–P4-only-Produkt
  richtig).
- **(B) Submit-Ahead im Sync-Modus (doNotWait-Polling).** Nach Submit nicht blockend locken,
  sondern pro Tick mit `doNotWait=1` pollen; braucht trotzdem N Output-Buffer (das
  Single-Buffer-Aliasing muss so oder so weg). Pro: kein Event-Plumbing. Contra: Poll-Granularität
  hängt am `Sleep(1)`-Takt des Threads (bis ~1,5 ms zusätzliche Latenz pro Frame), CPU-Wakeups,
  und `nvEncLockBitstream(doNotWait=1)`-Polling ist genau das Muster, das die NVENC-Doku für
  Windows als zweite Wahl hinter Events beschreibt.
- **(C) Echter Async-Modus** (`enableEncodeAsync=1`, per-Frame-`completionEvent`, N Output-Buffer,
  Reap über `WaitForSingleObject`). Pro: SDK-kanonischer Windows-Pfad, präzise Wakeups, die
  Completion-Zeit ist als Event-Signal exakt messbar (encode_latency bleibt wohldefiniert),
  beseitigt das Output-Buffer-Aliasing strukturell. Contra: Event-Lifecycle (Register/Unregister/
  CloseHandle), Teardown-Ordnung, Device-Lost-Verhalten (Event feuert nie) muss bounded sein.

**Entscheidung: (C), aber nur hinter dem Datengate.** Das Gate (nach Stufe 1, gemessen auf der
Referenzmaschine des Entwicklers, RTX-40-Klasse, mit dem Script ausgewertet):

> Async wird umgesetzt, wenn in einem der Zielszenarien (1440p60 Spiel, 4K60 Spiel/Desktop,
> jeweils AV1 und HEVC, P4 sowie P7) gilt:
> **G1:** `encode_latency` p99 > 8 ms (halbes 60-fps-Budget) im Steady-State, oder
> **G2:** `frames_dropped_backpressure` > 0,5 % der emittierten Frames — der Zähler enthält
> Slot-Stalls (`video_thread.cpp:2517-2518` inkrementiert `slotStallCount` UND
> `OnFrameDroppedBackpressure`) und die Sustained-Lag-Resync-Skips (`:2498`) bereits; sie werden
> NICHT zusätzlich addiert. Der 10-s-Perf-Record führt `slot_stalls` als separates Feld, damit
> die Auswertung die Anteile (Stall vs. Resync) trennen kann, oder
> **G3:** `video_tick_ms` p99 > 12 ms UND die Bottleneck-Klassifikation zeigt sustained
> `VideoEncoder`.
> Reißt kein Szenario das Gate → Ausgang (A) mit ADR; die §§ D2/D4 dieser Spec bleiben die
> Blaupause für den Tag, an dem B-Frames (M-2) die Pipeline-Tiefe ohnehin erzwingen.

Begründung der Schwellen: p99 von 8 ms lässt bei 60 fps noch Composite+VP-Blt+Route im Budget;
0,5 % Drop-Rate ist die Grenze, ab der Drops in 60-s-Clips sichtbar werden (~18 Frames/Minute);
P7 ist im Gate, weil es user-wählbar ist (ADR 0039) und „Expert-Preset wählbar, aber praktisch
unbrauchbar" gegen die Honesty-Linie des Produkts liefe.

### D4 — Stufe 2: Async-Architektur (konkret)

**Encoder-intern (`NvencEncoder`):**

- `static constexpr int32_t kNumOutputResources = 4;` — Output-Ring aus
  `std::array<OutputResource, 4>` mit `{ NV_ENC_OUTPUT_PTR bitstream; HANDLE event; bool
  in_flight; }`. 4 deckt die beobachtete P6/P7-Pipeline-Tiefe; die Input-Seite behält 8 Slots
  (Capture-Jitter-Puffer). Events: `CreateEventW(nullptr, FALSE, FALSE, nullptr)` (auto-reset),
  registriert via `NvEncRegisterAsyncEvent` nach `InitEncoder`.
- Capability-Gate: `QueryEncodeCap(NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT)` vor `InitEncoder`; bei 0
  bleibt `m_asyncMode=false` und der heutige Sync-Pfad läuft unverändert (Code bleibt erhalten —
  er ist zugleich der Referenzpfad für Regressionvergleiche). Der Modus wird im Init-Diag-String
  (`BuildInitDiagString`) mitgeloggt.
- `InitEncoder`: `p.enableEncodeAsync = m_asyncMode ? 1 : 0` (`nvenc_encoder.cpp:907` ff.).
- **Submit** (`EncodeFrame`, wird submit-only): freien Output-Slot wählen; ist keiner frei →
  bounded Wait auf das Event des ältesten `PendingFrame` (Policy s. u.). Dann
  `pic.outputBitstream = out[i].bitstream; pic.completionEvent = out[i].event;`, PendingFrame
  (D2-Struktur inkl. `out_idx`) in die FIFO, `nvEncEncodePicture`. `NV_ENC_ERR_NEED_MORE_INPUT`
  ist im Async-Modus laut SDK nicht zu erwarten (Events melden Completion), wird aber wie heute
  toleriert (Frame bleibt pending).
- **Reap** (neu, `bool ReapCompleted(std::vector<EncodedVideoPacket>& out, std::string& err,
  DWORD wait_head_ms = 0)`): solange die FIFO nicht leer ist und
  `WaitForSingleObject(head.event, first ? wait_head_ms : 0) == WAIT_OBJECT_0`:
  `nvEncLockBitstream` (im Async-Modus nach Event-Signal nicht blockierend), `outputTimeStamp`-
  und `pictureType`-Validierung nach D2, Paket bauen (`pts_ns` aus dem PendingFrame,
  `keyframe` aus `pictureType`), `encode_latency = now - submit_time` melden, Unlock, Input-Slot
  unmappen/freigeben, Output-Slot freigeben. Da `frameIntervalP=1` bleibt, ist die
  Completion-Reihenfolge die Submissions-Reihenfolge — der FIFO-Kopf ist immer der nächste.
- **Bounded-Wait-Policy:** `NextFlushDrainStep` wird zu einer generischen, puren
  `NextEventDrainStep(wait_result, elapsed_ms, budget_ms)` verallgemeinert (WAIT_OBJECT_0 →
  Consume, WAIT_TIMEOUT → Retry/AbortTimeout, sonst AbortError); Flush und Slot-Voll-Wait nutzen
  dieselbe Policy. Device-Lost (Event feuert nie) endet damit garantiert im Timeout-Abbruch statt
  im Wedge — dieselbe Härtung, die der heutige Flush-Drain schon hat.
- **Flush:** EOS-`nvEncEncodePicture` bekommt im Async-Modus ein eigenes, reserviertes
  Completion-Event; danach Reap aller PendingFrames mit der Policy (Budget wie heute 2000 ms
  pro Fortschritt).
- **Teardown (`Destroy`):** Reihenfolge: alle Events `NvEncUnregisterAsyncEvent` →
  `CloseHandle` → alle N Bitstream-Buffer destroyen → Encoder destroyen. `UnregisterAllSlots`
  unverändert davor.

**Interface (`IVideoEncoder`) — pre-1.0, breaking erlaubt:**

- `EncodeFrame` ändert seine Semantik zu „submit; liefert 0..k fertige Pakete":
  `bool EncodeFrame(int32_t slot_idx, uint64_t pts_ns, uint32_t w, uint32_t h,
  std::vector<EncodedVideoPacket>& out_packets, std::string& out_error)`. Sync-Encoder (und der
  Sync-Fallback) füllen genau 0/1 Pakete — für sie ist das ein mechanischer Signaturwechsel.
- Neu: `virtual bool ReapCompleted(std::vector<EncodedVideoPacket>& out, std::string& err,
  uint32_t wait_head_ms = 0) { return true; }` — Default-No-op, damit die künftigen
  Software-Encoder (Roadmap 0.11) das Interface trivial erfüllen.
- Kein neues Vokabular im Resolver/Capability-Layer: Async ist eine Encoder-Implementierungs-
  eigenschaft, keine Container/Codec-Policy — sie bleibt vollständig unterhalb von
  `IVideoEncoder` (Leitplanke: Policy lebt in libs/capability, Engine bleibt UI-agnostisch).

**Video-Thread-Integration:**

- Pro äußerer Loop-Iteration einmal `nvenc.ReapCompleted(pkts, err, 0)` (non-blocking) und jedes
  Paket durch `routePacket` — VOR dem Tick-Emit, damit freigewordene Slots sofort nutzbar sind.
- `AcquireFreeSlot() < 0` heißt nicht mehr sofort Drop: erst `ReapCompleted(pkts, err,
  kSlotWaitMs /* z. B. 4 */)`; erst wenn danach immer noch kein Slot frei ist, zählt der
  Backpressure-Drop wie heute (`video_thread.cpp:2515-2519`, `:3081-3086`).
- Nach jedem Submit werden die zurückgegebenen Pakete (0..k) geroutet — `routePacket` ist bereits
  paketweise und ordnungsagnostisch.
- **Split-Sentinel muss an den KONKRETEN geforceten Frame gebunden werden** (Ordnungserhalt
  allein genügt NICHT): Heute ist `split_armed && pkt.keyframe` (`video_thread.cpp:1890`)
  korrekt, weil Arming (`maybeArmSplit`, `:2719`) und Routing (`:2738`) in derselben Iteration
  ohne dazwischenliegende Pakete passieren. Unter Submit-Ahead routet dieselbe Iteration aber
  0..k reaped Pakete ÄLTERER, vor dem Arming submittter Frames, während `split_armed` schon true
  ist — fällt ein natürlicher GOP-Keyframe (alle `gopLength` Frames) in die Pipeline-Tiefe,
  würde er den Split absorbieren und die Segmentgrenze läge einige Frames vor dem
  Nutzer-/Auto-Threshold (keyframe-safe, aber falsch platziert). Deshalb: `maybeArmSplit` merkt
  sich zusätzlich `split_forced_pts_ns = pts_ns` (die PTS des Frames, dessen Submission das
  `RequestKeyframe()` konsumiert — im selben Loop-Durchlauf direkt danach submitted). Die
  Routing-Bedingung wird `split_armed && pkt.keyframe && pkt.pts_ns >= split_forced_pts_ns`
  (`>=` statt `==` als Defensive: nie ein früherer Keyframe, notfalls der nächste danach). Die
  Bedingung wird als pure, CI-testbare Helper-Funktion extrahiert
  (`ShouldEmitSplitSentinel(armed, forced_pts_ns, pkt_keyframe, pkt_pts_ns)`); im Sync-Modus ist
  sie zum heutigen Verhalten äquivalent (das geroutete Paket IST der geforcete Frame), sie kann
  also gefahrlos mit S9 für beide Modi landen.
- Das `encode_latency`-Bracket ist nach S2 bereits encoder-intern (Submit-Zeit in PendingFrame,
  Transport über `EncodedVideoPacket::encode_latency_ms`, Meldung an den Aggregator weiterhin an
  der Video-Thread-Callsite) — der Async-Reap füllt exakt dasselbe Paket-Feld, die Stufe-1-Metrik
  bleibt über den Umbau hinweg vergleichbar (das ist der Beweispfad: Stufe-1-Zahlen vorher,
  dieselben Metriken nachher).

**Was bewusst NICHT gebaut wird:**

- Keine B-Frames, kein Lookahead, kein `frameIntervalP > 1` — das ist M-2
  (`encoder-quality-features-spec`) und setzt auf der D2-Struktur auf.
- Kein Async für einen hypothetischen Nicht-Windows-Pfad (NVENC-Async ist Windows-only; das
  Produkt ist Windows-nativ).
- Keine Konfigurierbarkeit der Pipeline-Tiefe im UI, kein Expert-Toggle „async on/off" — der
  Modus ist eine interne Implementierungsentscheidung mit Capability-Fallback.
- Keine Vereinfachung des 8-Slot-Rings im Ausgang (A) — siehe D3.

---

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz. Schritte 1–5 sind Stufe 1
(unbedingt); Schritte 6–9 nur nach gerissenem Gate (D3).

**S1 — `LatencyHistogram` + `RollingTimeWindow::Percentile` (pur).**
- Neu: `libs/recorder_core/src/perf_histogram.h`; Erweiterung
  `pipeline_diagnostics_aggregator.h` (`RollingTimeWindow::Percentile`).
- Tests: neu `libs/recorder_core/tests/test_perf_histogram.cpp` (Bucket-Kanten, Quantile gegen
  bekannte Verteilungen, Merge, Überlauf-Bucket, Leer-Fall) + Erweiterung
  `test_pipeline_diagnostics_aggregator` um Perzentil-Fälle (zeitinjiziert, deterministisch).
- CMake: Test-Target nach dem Muster der bestehenden `test_nvenc_*`-Registrierungen
  (`libs/recorder_core/CMakeLists.txt:527` ff.).

**S2 — Encoder: `submit_time` in die konsolidierte Pending-FIFO + Latenz-Transportkanal
(verhaltensneutral, aber mehr als „ein kleiner Eingriff": drei Dateien).**
- `nvenc_encoder.h/.cpp`: `m_pendingPts`+`m_pendingSlots` → `std::queue<PendingFrame>`
  (D2-Struktur ohne `out_idx`).
- `packet_types.h`: `EncodedVideoPacket::encode_latency_ms = -1.0` (der Transportkanal aus D1 —
  `LockAndConsumeBitstream` füllt das Feld aus `now - PendingFrame::submit_time`; ohne diesen
  Kanal käme die P5–P7-korrekte Dauer nie beim Aggregator an, weil der Encoder keine
  Aggregator-Referenz hat und das konsumierte Paket zu einer früheren Submission gehört).
  Bewusst KEINE Signaturänderung an `EncodeFrame`; alle weiteren Konsumenten von
  `EncodedVideoPacket` (Premux/Mux) ignorieren das Feld.
- Noch KEINE outputTimeStamp-Validierung (die kommt mit S6, wo die Fehlerpfade dazugehören) —
  S2 ist Datenstruktur-Konsolidierung + Kanal.
- Tests: bestehende `test_nvenc_video_encoder_interface`/`test_nvenc_flush_drain_policy` bleiben
  grün; Default-/Nichtverfügbarkeits-Semantik des neuen Feldes (−1 ohne Encoder) unit-testbar;
  kein GPU-Test nötig.

**S3 — Brackets + Aggregator + Snapshot.**
- `video_thread.cpp`: `video_tick_ms`-Bracket um den CFR-Tick-Body (`:2513-2744`) und die beiden
  VFR-Encode-Zweige; neuer Aggregator-Input `OnVideoTickTime(now, ms)`. `OnEncodeLatency` erhält
  die echte Submit→Ready-Dauer aus `pkt.encode_latency_ms` (S2-Kanal; nur bei Wert ≥ 0 melden —
  P5–P7-korrekt); zusätzlich `OnEncodeSubmitCost(now, ms)` für das bisherige Callsite-Bracket.
- `pipeline_diagnostics_aggregator.h/.cpp`: `tick_window_`, whole-session `LatencyHistogram`
  für encode+tick; `pipeline_diagnostics.h`: `EncoderDiagnostics::p50_ms/p99_ms`, neues
  `VideoTimingDiagnostics` im Snapshot.
- Tests: Aggregator-Tests zeitinjiziert (Fenster-p99 exakt nachrechenbar); Snapshot-Feld-Tests.

**S4 — Persistenz + Script.**
- `session_stats_collector.cpp`: 10-s-Perf-Record + `session-perf-summary` (D1-Felder,
  `perf_schema=1`).
- Neu: `scripts/dev/analyze-encode-perf.py` (Stdlib-only; Session-Gruppierung, Tabelle,
  Vergleichsmodus, `--json`).
- Tests: CI-fähig — ein gtest erzeugt über den Logging-Layer ein synthetisches `engine.jsonl`-
  Fixture (Muster: `test_fdk_aac_encoder.cpp:88-93` initialisiert das Logging in ein Temp-File);
  das Script wird in CI gegen ein eingechecktes Fixture ausgeführt (analog zu
  `gen-manifest-fixture.py`-Nutzung) und sein `--json`-Output verglichen.

**S5 — Doku + Messkampagne (log-only; KEINE UI-Änderung).**
- KEINE Diagnostics-Karte und KEINE `product-spec`-Änderung in Stufe 1 (s. D1 Kanal 1): ob die
  p99-Encode-Latenz einen user-sichtbaren Wert verdient, entscheidet erst die Kampagne — die
  Karte + `docs/product-spec.md` §Diagnostics-Ergänzung sind Teil des ADR-0044-Ausgangs, falls
  die Daten Relevanz zeigen.
- `KNOWN_LIMITATIONS.md`: Absatz „Video-Encode ist derzeit synchron; Messinfrastruktur vorhanden".
- Danach: Messkampagne durch den User (Szenarien aus D3), Auswertung mit dem Script,
  Gate-Entscheidung als ADR 0044 festhalten (beide Ausgänge sind ein ADR wert; die
  p99-Sichtbarkeitsfrage wird dort mitentschieden).

**— Gate: nur weiter, wenn D3 reißt —**

**S6 — Ordnungs-/Keyframe-Validierung (D2, noch im Sync-Modus; warn-first).**
- `nvenc_encoder.cpp`: `outputTimeStamp`-Check **warn-only** (Mismatch → einmaliges Warn-Log mit
  beiden Werten + Zähler `output_ts_mismatches`; PTS-Zuordnung bleibt FIFO — s. D2 Phase 1),
  `pictureType`-vs-Prediction-Check (Mismatch → Warn-Log + Zähler + Phase-Resync);
  `EncoderDiagnostics::keyframe_prediction_mismatches` + `output_ts_mismatches`.
- **Live-Verify (User, vor S8):** Echo-Verhalten von `outputTimeStamp` auf der Zielhardware
  bestätigen — je eine Session AV1/HEVC/H.264, P4 und P7 (P5–P7 deckt den
  NEED_MORE_INPUT-Pfad ab); Erwartung `output_ts_mismatches == 0`. Erst nach bestandenem Check
  wird der Mismatch in S8 fatal verdrahtet (D2 Phase 2); andernfalls bleibt er warn-only und
  ADR 0044 hält die Einschränkung fest.
- Tests: Resync-Logik als pure Erweiterung von `NextGopKeyframePhase` (z. B.
  `ResyncGopPhaseFromActual(actual_idr, frame_in_gop, gop_length)`) in
  `test_nvenc_gop_aq_config.cpp`; Mismatch-Erkennung + Warn-Formatierung als pure Logik
  unit-testbar.

**S7 — `IVideoEncoder`-Umbau (Signatur + ReapCompleted, Verhalten unverändert).**
- `IVideoEncoder.h`, `nvenc_video_encoder.h/.cpp`, `video_thread.cpp` (Callsites `:2725`,
  `:2990`, `:3061` auf Vektor-Signatur; Reap-Aufrufe einbauen, die im Sync-Modus No-ops sind).
- Tests: `test_nvenc_video_encoder_interface.cpp` erweitert (Default-Reap, Vektor-Semantik).

**S8 — Async-Modus im `NvencEncoder`.**
- `nvenc_encoder.h/.cpp`: Output-Ring (4× Buffer+Event), Caps-Gate, `enableEncodeAsync`,
  Submit/Reap nach D4, `NextEventDrainStep`-Generalisierung, Flush mit Events, Teardown-Ordnung.
- Tests (CI, GPU-los): `NextEventDrainStep` pur; Output-Ring-Buchhaltung als pure Slot-Logik
  testen (frei/in-flight/ältester); Teardown-Reihenfolge per Review + bestehende
  Interface-Tests. Echte Async-Encodes sind CI-seitig nicht testbar (WARP hat kein NVENC) —
  Live-Verify beim User (s. Test-Plan).
- Der Sync-Pfad bleibt als Caps-Fallback vollständig erhalten.

**S9 — Video-Thread-Integration + Vorher/Nachher-Messung.**
- Reap-Integration (D4), Slot-Voll-Wait statt Sofort-Drop; Split-Sentinel-Bindung an den
  geforceten Frame (`split_forced_pts_ns` + purer Helper `ShouldEmitSplitSentinel`, s. D4 —
  ersetzt die heutige `split_armed && pkt.keyframe`-Bedingung in `routePacket`); identische
  Messkampagne wie S5 auf demselben System; Script-Vergleichsmodus liefert das
  Vorher/Nachher-Delta; Ergebnis in ADR 0044 nachtragen. `KNOWN_LIMITATIONS.md`-Absatz
  aktualisieren. Mit S8: `outputTimeStamp`-Mismatch fatal schalten (nur falls der
  S6-Live-Check bestanden wurde).

---

## Test-/Verify-Plan

**CI-fähig (GPU-los):**
- `LatencyHistogram`-Quantile/Merge/Überlauf; `RollingTimeWindow::Percentile` exakt.
- Aggregator: p50/p99 in Snapshot deterministisch (Zeitinjektion, wie bestehende Tests).
- JSONL-Fixture-Roundtrip: Summary-Record schreiben → Script liest → Perzentile identisch mit den
  direkt aus dem Histogramm berechneten (Toleranz = Bucket-Interpolationsfehler).
- `NextEventDrainStep`-Policy (alle Übergänge, Budget-Ablauf).
- `ShouldEmitSplitSentinel` pur (D4): natürlicher Keyframe vor dem geforceten Frame bei armed →
  KEIN Sentinel; geforcete PTS → Sentinel; `>=`-Defensive; Sync-Äquivalenz zum heutigen
  Verhalten.
- GOP-Phase-Resync pur; PendingFrame-FIFO-Buchhaltung (Fehlerpfade Submit-Fail/Lock-Fail räumen
  konsistent auf).
- Interface-Tests (Vektor-Signatur, Default-Reap).
- Script-Selbsttest gegen eingechecktes Fixture (Session-Gruppierung, Vergleichsmodus).

**Nur User-live (echte NVENC-Hardware; Agents starten/bedienen die App nie):**
- **Stufe-1-Messkampagne (S5):** Szenarien 1080p60-Desktop, 1440p60-Spiel, 4K60, jeweils AV1+HEVC,
  P4+P7; je ≥ 5 min; `engine.jsonl` mit dem Script auswerten; Gate-Entscheidung.
- **outputTimeStamp-Echo-Verify (S6, vor S8):** je eine Session AV1/HEVC/H.264 mit P4 und P7;
  Erwartung `output_ts_mismatches == 0` — Voraussetzung dafür, dass der Check in S8 fatal wird
  (D2 Phase 2).
- **Stufe-2-Abnahme (S9):** (a) identische Kampagne, Delta-Report; (b) Datei-Integrität: manueller
  + Auto-Split während Async, `mkvinfo`/`ffprobe` — Cues zeigen auf echte Keyframes, jedes
  Segment beginnt mit IDR, Segmente einzeln abspielbar; **zusätzlich gezielt: Split-Auslösung
  unmittelbar vor einer natürlichen GOP-Grenze** (Keyframe-Intervall auf 1 s stellen, manuellen
  Split kurz vor der Sekundengrenze auslösen) — die Segmentgrenze muss auf dem geforceten Frame
  liegen, nicht auf dem natürlichen Keyframe davor (D4-Split-Bindung); (c) HDR10-Session: SEI/OBU auf jedem
  Keyframe vorhanden (bestehende SEI-Checks aus der SEI-RELEASE-GATE-Liste wiederverwenden);
  (d) P7-Session ohne Slot-Stall-Explosion; (e) Stop/Abbruch mitten in hoher Last (Flush-Drain
  terminiert, Datei finalisiert); (f) 2-h-Soak (deckt sich mit dem 0.10-Soak-Ziel) ohne
  Latenz-Kriechen (Summary-Perzentile Anfang vs. Ende) und ohne `keyframe_prediction_mismatches`
  oder `output_ts_mismatches`.
- **Device-Lost unter Async:** kann nur live provoziert werden (Treiber-Reset/`dxcap -forcetdr`
  durch den User); Erwartung: Timeout-Abbruch statt Wedge, Aufnahme bis zum letzten gemuxten
  Paket erhalten.

---

## Risiken

- **Event-/Handle-Lifecycle (S8):** Unregister/Close in falscher Reihenfolge leakt Handles oder
  crasht im Treiber. Gegenmaßnahme: eine einzige, dokumentierte Teardown-Sequenz in `Destroy()`;
  Destroy ist bereits idempotent aufgebaut.
- **Device-Lost im Async-Modus:** Event feuert nie → jeder Wait ist über `NextEventDrainStep`
  budgetiert; kein unbounded `WaitForSingleObject(INFINITE)` an keiner Stelle.
- **Ordnung bricht doch (PTD/adaptive Intra, künftige Treiber):** wird nicht mehr angenommen,
  sondern per `outputTimeStamp` verifiziert — gestuft (D2): warn-only, bis das Echo-Verhalten
  auf der Zielhardware live belegt ist, erst dann fatal. Damit ist sowohl stille PTS-Korruption
  (nach Eskalation) als auch ein Falsch-Positiv-Abbruch gültiger Aufnahmen (vor Verifikation)
  ausgeschlossen. Restrisiko nach Eskalation: fataler Abbruch statt Degradation — bewusst
  gewählt.
- **Split-Grenze unter Submit-Ahead:** ohne PTS-Bindung könnte ein natürlicher GOP-Keyframe in
  der Pipeline-Tiefe den Split absorbieren — durch die D4-Bindung (`split_forced_pts_ns`)
  ausgeschlossen; pure Helper-Tests + gezielter Live-Check (Split nahe GOP-Grenze) decken es ab.
- **Mess-Overhead / Log-Volumen:** 2 Brackets/Frame sind vernachlässigbar; 6 Perf-Records/min
  sind gegen die vorhandene Log-Rotation (#179) unkritisch.
- **Fehlinterpretation der Fenster-Perzentile:** p99 über 256 Samples (~2 s) ist rauschig; die
  Gate-Entscheidung stützt sich deshalb auf die whole-session-Histogramme, nicht auf die
  Live-Fenster. Im Script entsprechend beschriften.
- **Single-Machine-Bias:** Das Gate misst auf einer RTX-40-Maschine. Ältere NVENC-Generationen
  (Turing/Ampere, HEVC/H.264) sind langsamer — das Gate kann falsch-negativ sein. Mitigation:
  Gate-Schwellen konservativ (halbes Budget), Ergebnis als ADR mit Hardware-Angabe, Messinfra
  bleibt dauerhaft eingebaut, sodass jeder Nutzerreport nachmessbar ist.
- **Interface-Bruch (S7)** trifft die künftigen Software-Encoder-Specs (0.11): Default-Reap hält
  deren Aufwand bei null; die Spec `software-encoding-spec` muss die Vektor-Signatur übernehmen.
- **Semantik-Drift der encode_latency zwischen Sync und Async:** explizit gleich definiert
  (Submit → Bitstream-verfügbar) und in S2/S3 schon im Sync-Modus so gemessen — die
  Vorher/Nachher-Vergleichbarkeit ist Designziel, nicht Zufall.

---

## Offene Fragen (echte Produktentscheidungen)

1. **Sichtbarkeit von p99:** In Rev. 2 entschieden — Stufe 1 bleibt log-only; ob die
   p99-Encode-Latenz je auf der Diagnostics-Encoder-Karte erscheint, wird mit der
   Gate-Entscheidung in ADR 0044 beantwortet (dann inkl. product-spec §Diagnostics-Pflege).
   Keine offene Frage mehr, hier nur zur Nachvollziehbarkeit belassen.
2. **Gate-Zielprofil:** Ist 4K60 das höchste Profil, das flüssig sein MUSS, oder soll auch das
   4K144→60-Szenario aus dem Review (Capture-Quelle 144 Hz, Encode 60) als Gate-Szenario zählen?
   Beeinflusst nur die Messkampagne, nicht den Code.
3. **Falls Ausgang (A) (kein Async):** P5–P7 bleiben user-wählbar, erhöhen aber im Sync-Modus
   messbar die Latenz. Reicht die vorhandene ADR-0039-Doku, oder soll die Preset-Beschreibung im
   UI einen Latenz-Hinweis bekommen? (Empfehlung: erst nach Messdaten entscheiden — vielleicht
   ist der Effekt auf Zielhardware irrelevant.)

---

## Adversarialer Review — Ergebnis

Acht Einwände, alle gegen Code/Docs verifiziert, alle acht eingearbeitet:

1. **Split-Sentinel unter Async (major) — eingearbeitet.** Am Code bestätigt: `split_armed &&
   pkt.keyframe` (`video_thread.cpp:1890`) ist nur wegen der Same-Iteration-Adjazenz von Arming
   (`:2719`) und Routing (`:2738`) korrekt; unter Submit-Ahead könnte ein natürlicher
   GOP-Keyframe in der Pipeline-Tiefe den Split absorbieren. D4 fordert jetzt die Bindung an den
   konkreten geforceten Frame (`split_forced_pts_ns`, purer Helper `ShouldEmitSplitSentinel`);
   S9, CI-Tests und ein gezielter Live-Check (Split nahe GOP-Grenze) ergänzt.
2. **Fataler outputTimeStamp-Check auf unverifizierter Echo-Annahme (major) — eingearbeitet.**
   Bestätigt: `enablePTD=1` + `inputTimeStamp=m_frameIdx++`, das Echo ist headerseitig nur als
   „presentation timestamp" deklariert und nie gegen die Zielhardware belegt. D2/S6 sind jetzt
   zweiphasig: warn-only + Zähler `output_ts_mismatches`, dedizierter Live-Verify (alle Codecs,
   P4+P7), Eskalation zu fatal erst mit S8 nach bestandenem Check.
3. **Fehlender Latenz-Transportkanal in Stufe 1 (major) — eingearbeitet.** Bestätigt: der Encoder
   hat keine Aggregator-Referenz, und bei P5–P7 gehört das gelockte Paket zu einer früheren
   Submission. D1/S2 spezifizieren den Kanal jetzt explizit: neues Feld
   `EncodedVideoPacket::encode_latency_ms` (−1 = nicht verfügbar), Callsite meldet nur bei ≥ 0;
   S2 nicht mehr als „einziger kleiner Eingriff" deklariert (drei Dateien inkl.
   `packet_types.h`).
4. **G2-Doppelzählung (minor) — eingearbeitet.** Bestätigt: Slot-Stalls (`:2517-2518`) und
   Resync-Skips (`:2498`) speisen beide `OnFrameDroppedBackpressure`. G2 zählt jetzt nur
   `frames_dropped_backpressure`; der 10-s-Record führt `slot_stalls` separat zur Entwirrung.
5. **Beleg-Mismatch nvEncodeAPI.h:2586 (minor) — eingearbeitet.** Bestätigt: die Zeile ist der
   `completionEvent`-Kommentar (Async-Kontext). Formulierung entschärft: 1:1-Bindung nur für
   Async explizit im Header; für Sync stützt sich die Aussage auf die
   Programming-Guide-Empfehlung (Output-Buffer ≥ in-flight-Frames), als solche gekennzeichnet.
6. **Platzhalter „#—" (minor) — eingearbeitet.** Der „sustained encoder lag"-Pfad kam mit #192
   (verifiziert via `git log -S`); Referenz eingetragen.
7. **p99-Karte + product-spec-Änderung verfrüht (minor) — eingearbeitet.** Konsistent mit der
   (vormaligen) Offenen Frage 1: Stufe 1 ist jetzt log-only, S5 ohne UI-/product-spec-Änderung;
   die Sichtbarkeitsentscheidung fällt mit dem Gate in ADR 0044. Snapshot-Felder bleiben als
   Engine-Interna (Test-/Debugkonsum, kein Produkt-Surface).
8. **Roadmap-Verortung fehlt (minor) — eingearbeitet.** Bestätigt: `docs/roadmap.md` hat keinen
   Perf-Slot (0.9 → 0.10). Kopfzeile benennt jetzt die Ziel-Welle („Perf & Qualität", nach
   0.9.0) und die Pflicht, beim Wellenstart einen eigenen Versions-Slot in die Roadmap-Tabelle
   aufzunehmen; Guardrail-Konformität festgehalten.

# Reliability-Härtung 0.10: Soak-Infrastruktur, A/V-Sync-Validierung, Recovery-Drills

> **SHIPPED (PR #193, 2026-07-12).** Verifiziert 2026-07-23: `tools/soak/` (Harness,
> `SoakAbortPolicy`, `SoakMetricsAggregator`, `av-sync-check.py`, Recovery-Drills) vorhanden — deckt
> den kompletten Buildable-Scope dieser Spec ab. Einzig offen: der tatsächliche mehrstündige Soak-Lauf
> selbst ist ein User-Live-Verify, kein Implementierungs-Task (Schwellen sind laut Roadmap ohnehin
> "advisory, not a release gate").

> Spec-Autor-Deliverable (read-only erhoben aus main @ #192). Umsetzung später durch
> andere Agenten NUR anhand dieser Spec. Sprache Deutsch, Code-Bezeichner original.
> Thema aus `docs/roadmap.md` Zeile 84 (0.10.0 „Reliability hardening (vendor-independent)")
> und Review `.workspace/review-fable-2026-07-10.md` §7 Phase 1 (Zeile 236–237: „2-h-Soak
> mit A/V-Messung", „ein E2E-Test, der eine echte Datei erzeugt").

## Problem

Die Roadmap verspricht für 0.10 vendor-unabhängige Härtung: Langzeit-Aufnahme-Stabilität,
A/V-Sync-Drift-Validierung und Recovery-Drills. Der Review nennt drei konkrete Lücken:

1. **Keine Langzeit-Messung.** Es gibt keinen Benchmark und keine automatisierte Langzeit-Aufnahme
   im Repo (Review §5, Zeile 193 „Es gibt keinen einzigen Benchmark im Repo"). Ob eine 2-h-Aufnahme
   ohne Drift, Drop-Akkumulation oder RAM/Handle-Leak durchläuft, ist unbewiesen — genau der Fall,
   den 0.10 verspricht (Review H-3, Zeile 70: „Lange Aufnahmen (das erklärte 0.10-Soak-Ziel) driften").
2. **A/V-Sync ist behauptet, nicht gemessen.** Seit #191 existiert eine ehrliche Drift-*Metrik*
   (`AudioClockDriftEstimator`, device-position/QPC-Paare), aber es gibt keine reproduzierbare
   *Abnahme-Methode*, die eine fertige Datei gegen ein bekanntes Referenz-Signal misst. Genau diese
   Methode ist im Review als Voraussetzung für das eigentliche Clock-Slaving (`av-clock-slaving-spec`,
   H-3 Stufe 3) genannt: „Messmethode: 2-h-Aufnahme, Klappen-Signal Anfang/Ende, ffprobe-PTS-Vergleich"
   (Zeile 69/211).
3. **Recovery ist implementiert, aber nicht adversarial getestet.** Die Recovery-Maschinerie
   (Manifest, `RecoveryService.Finish`, Repair-Remux, Durability-Flush) ist gebaut und unit-getestet,
   aber kein Test übt einen echten *Kill mitten in einer Phase* (Recording/Finalize/Remux) gegen die
   Recovery-Kette. Der Durability-Flush (M-4, gelandet) definiert ein Powerloss-Fenster von ~2 s, das
   nie unter einem echten Abbruch verifiziert wurde.

Diese Spec definiert die **Infrastruktur**, mit der diese drei Versprechen gemessen und abgenommen
werden — und trennt sauber, was in CI (kein GPU, deterministisch) läuft und was zwingend ein
User-Live-Lauf auf der Entwicklermaschine ist.

## Ist-Zustand (mit Datei:Zeile-Referenzen)

### Was schon da ist und wiederverwendet wird

- **Headless-Aufnahme-Treiber existiert bereits.** `tools/probes/probe_record/src/main.cpp` fährt die
  *echte* Produktions-Pipeline (WGC → Compositor → NVENC → Mux → MP4-Remux-on-stop) headless über
  `RecorderSession`: `EnumerateTargets()` (`main.cpp:295`), `session.Record(cfg)` blockierend
  (`main.cpp:407`), ein `stopper`-Thread ruft `session.Stop()` nach `--seconds` (`main.cpp:402-405`),
  MP4-Remux wie im App-Layer (`main.cpp:418-430`). **Korrektur zur Gate-Semantik:** das CMake-Gate ist
  `if(NOT TARGET recorder_core)` (`tools/probes/probe_record/CMakeLists.txt:10-13`) — ein reines
  **Build-Gate** auf die NVENC-Header. Die sind aber **vendored** (`third_party/nvidia/nvEncodeAPI.h`
  existiert), also **baut `recorder_core` und damit `probe_record` auch auf dem GPU-losen CI-Runner**
  (dort laufen die `recorder_core`-Tests); nur das *Ausführen* des echten Aufnahmepfads braucht eine
  GPU. „GPU-gated" ist also für den Build falsch — korrekt ist: **Build-Gate = NVENC-Header (hier immer
  erfüllt), Runtime-Gate = GPU vorhanden.** Der Treiber abonniert heute *keine* Callbacks; er meldet nur
  die Dateigröße (`main.cpp:432-436`).
- **Alle Soak-Metriken werden von der Engine bereits ehrlich geliefert** — ohne Engine-Änderung:
  - `RecorderSession::SetStatsCallback` (`recorder_session.h:579`) → `SessionStats`
    (`session_stats.h:12`): `duration_skew_ms` (`:22`), `dropped_or_skipped_video_frames` (`:23`),
    `duplicated_video_frames` (`:24`), `video_duration_ns`/`audio_duration_ns` (`:20-21`).
  - `RecorderSession::SetDiagnosticsCallback` (`recorder_session.h:589`) →
    `RecordingDiagnosticsSnapshot` (`pipeline_diagnostics.h:242`): `av_drift_ms` +
    `av_drift_availability` (`:267-268`, die ehrliche Clock-Drift-Metrik, nicht Queue-Latenz —
    Kommentar `:260-266`), `duration_skew_ms` (`:274`), `disk_fill_eta_seconds` (`:279`),
    `bottleneck`/`bottleneck_reason` (`:281-282`), `health` (`:283`); Capture-Drops
    `frames_dropped_coalesced|cfr|backpressure` (`pipeline_diagnostics.h:93-96`), Audio
    `discontinuities` (`:173`), `MuxDiagnostics`/`QueueDiagnostics` Tiefen (`:181-216`).
  - `duration_skew_ms` wird in `session_stats_collector.cpp:77-81` aus den Media-Dauern berechnet.
- **Die ehrliche Drift-Metrik-Mathematik ist gelandet (#191).** `AudioClockDriftEstimator`
  (`libs/recorder_core/src/audio_clock_drift.h`, getestet in
  `libs/recorder_core/tests/test_audio_clock_drift.cpp`): device-position/QPC-Paare, Vorzeichen
  positiv = Audio führt (`test_audio_clock_drift.cpp:45-73`), robust gegen Discontinuity-Gaps
  (`:75-95`), gefensterte Glättung (`:97-112`). Dies *misst* Drift; die **Kompensation** (swr Richtung
  QPC) ist bewusst NICHT hier, sondern in `av-clock-slaving-spec` (H-3 Stufe 3). Diese Spec liefert
  die Abnahme-Methode für jene.
- **Durability-Flush ist gelandet (M-4).** `MatroskaStreamWriter::FlushToDisk` (fflush +
  `FlushFileBuffers`, `matroska_stream_writer.cpp:134-160`), periodisch je Cluster gated durch
  `DurabilityFlushScheduler` mit `kDurabilityFlushInterval{2000}` ms (`matroska_stream_writer.h:235`,
  Nutzung `matroska_stream_writer.cpp:575-593`), plus finaler Flush vor Finalize (`:666-678`),
  Zähler `durability_flush_count()` (`:222`). **Das definiert das Powerloss-Verlustfenster: ≤ 2 s +
  Reorder-Window.**
- **Recovery-Maschinerie ist gebaut.** `RecoveryManifestStore` (`app/settings/RecoveryManifestStore.h`):
  Eintrag mit `finalized`-Flag (`:11-19`), `Add` vor Start / `UpdateFinalized` / `Remove` bei sauberem
  Stop, jede Mutation flush-sofort (Kommentar `:24-26`). `RecoveryService.Finish`
  (`app/services/RecoveryService.cpp:100`): MKV finalized → Rename (`:113-127`); MKV nicht-finalized →
  `RemuxToMkv`-Repair (`:130-153`); MP4 → `RemuxToProgressiveMp4` (`:160-181`). Bei Repair-/Remux-Fehler
  wird das Artefakt (die MKV) **behalten** und der Teil-Output entfernt (`:131-140`, `:161-168`) — das
  Produktversprechen aus `docs/product-spec.md:162` / §Crash recovery (`:486-503`).
  `RemuxToMkv`/`RemuxToProgressiveMp4` mit Progress-Callback: `mp4_remuxer.h:98-136`.
- **Der „echte Datei"-E2E-Test existiert (Review-Basis „#186").**
  `libs/recorder_core/tests/test_session_e2e_real_file.cpp` fährt echten `AudioThread` (echter
  libopus/libfdk-aac) + echten `MuxThread` + `MatroskaStreamWriter` + Finalize zu einer echten Datei
  und validiert sie mit der gevendorten libavformat als „fremder Player" (`DemuxAndInspect`,
  `:200-272`). Der GPU-Video-Pfad ist durch einen deterministischen In-Test-„video feeder" ersetzt
  (`:327-387`), der VideoThreads `SessionState`-Kontrakt erfüllt. **Nicht `live`, läuft in CI**
  (Kommentar `:26-28`; Registrierung `CMakeLists.txt:1074-1081`). Duration heute 2,0 s (`:442`).
- **CI-Realität.** `.github/workflows/ci.yml`: `build-test`-Matrix (debug/release, `:190-252`) läuft
  `scripts/run-tests.ps1 ... -ExcludeLabel live` (`:237-242`); der Runner hat **keine GPU** (Kommentar
  `:230-236`). `-LE live` schließt Hardware-Tests aus (`run-tests.ps1:127`). Der volle Packaging-/RC-
  Pfad läuft nur auf Tags/`release/**` (`release-candidate.yml`), nicht auf PRs.
- **Präzedenz für „reproduzierbares Script + CI-Fixture".** `scripts/dev/gen-manifest-fixture.py`
  (Python-Dev-Script, das ein Fixture erzeugt, das ein CI-Test cross-verifiziert) — dasselbe Muster,
  das der Review für C-1 empfiehlt und das diese Spec für die A/V-Analyse übernimmt.

### Was fehlt (die Lücke, die diese Spec füllt)

- Kein Langzeit-Treiber: `probe_record` ist single-shot, abonniert keine Callbacks, sammelt keine
  Prozess-Metriken (RSS/Handles), hat keine Abbruchkriterien und keinen Report.
- Keine Prozess-Speicher-/Handle-Erfassung irgendwo im Repo (Grep nach `GetProcessMemoryInfo`/
  `GetProcessHandleCount`/`WorkingSet` → nur `matroska_stream_writer` für `FlushFileBuffers`, kein
  psapi-Sampling).
- Kein `ffprobe`-basiertes A/V-Sync-Analyse-Script (Grep `ffprobe` → nur Doku/README, kein
  ausführbares Script); kein Klappensignal-Generator.
- Kein Recovery-Drill-Test, der einen Prozess *mitten in einer Phase* killt und die Recovery-Kette
  gegen das Ergebnis prüft (die vorhandenen `recovery_service_tests` prüfen `Finish`/`Discard` gegen
  von Hand präparierte Artefakte, nicht gegen einen echten abgebrochenen Schreibvorgang).
- Kein Session-Report-Artefakt (`session-<id>.json` aus Review §6.2 existiert nicht — Grep
  `session_report`/`SessionReport` leer). Der Soak-Report unten ist verwandt; Cross-Reference statt
  Doppelbau (siehe Design).

## Design

Drei Bausteine, ein durchgängiges Prinzip: **die Engine bleibt unverändert und UI-/Host-agnostisch;
alle neue Logik lebt in Host-Tools, puren Resolvern und Dev-Scripts.** Kein Soak-Baustein erfordert
eine Engine-*Code*-Änderung — alle Metriken sind schon über die zwei bestehenden Callbacks verfügbar
(einzige bewusste Ausnahme: der `--synthetic`-Zwilling macht interne Test-Seam-Header sichtbar, s.
Baustein 1 „Stats/Diagnostics im `--synthetic`-Modus"). Das ist
kein Zufall, sondern die Leitplanke: RAM/Handles sind Prozess-Eigenschaften des *Hosts*, nicht der
Engine, und dürfen nicht in die Engine geleakt werden.

### Baustein 1 — Soak-Infrastruktur

**Entscheidung: eigenes Host-Tool `tools/soak` (Executable `exosnap-soak`), das `RecorderSession`
direkt fährt — NICHT die GUI-App, NICHT `probe_record` erweitert.**

Alternativen ehrlich abgewogen:

- *Die laufende GUI-App fernsteuern.* Verworfen: CLAUDE.md verbietet das Bedienen der laufenden App
  kategorisch; zudem brüchig (Fenster-Automatisierung) und vermischt UI-Zustand mit Messung.
- *`probe_record` um Soak-Flags erweitern.* Naheliegend (es parst schon Config und fährt die echte
  Pipeline), aber `probe_record` ist bewusst ein *single-shot Korrektheits-Probe* mit einem knappen
  Output-Kontrakt (Codec/Container/Tags via ffprobe). Ein Endurance-Tool hat einen anderen Kontrakt
  (Metrik-Timeline, Abbruchkriterien, Leak-Analyse, Report). Beides in einen `main` zu quetschen
  verwässert beide. **Entscheidung:** separates Tool; die kleine Config-Parsing-Logik wird bei Bedarf
  in einen gemeinsamen Header `tools/common/record_cli_args.h` gehoben (billiger Refactor, kein
  Muss für PR 1).
- *Neues Tool.* Gewählt. Klarer Kontrakt, unabhängig testbare pure Teile, keine Vermischung.

Das Tool:

1. Fährt `RecorderSession::Record` für `--minutes N` (Default-Abnahmeziel offen, s. u.), Stop per
   Dauer-Timer **oder** Ctrl-C (`SetConsoleCtrlHandler` → `session.Stop()`), gegen eine **definierte
   Quelle** (Monitor-Target, das ein deterministisches Testmuster zeigt; das Muster ist Sache des
   Live-Setups, s. Baustein 2 / „NICHT gebaut").
2. Abonniert `SetStatsCallback` + `SetDiagnosticsCallback` (keine Engine-Änderung) und **einen
   Prozess-Sampler-Thread @ ~1 Hz**: `GetProcessMemoryInfo` (`WorkingSetSize`, `PrivateUsage` via
   `PROCESS_MEMORY_COUNTERS_EX`), `GetProcessHandleCount`, `GetGuiResources` (GDI/USER-Objekte). Diese
   drei WinAPIs sind der einzige neue Mess-Code und leben ausschließlich im Host-Tool.
3. Schreibt eine **Metrik-Timeline als JSON-Lines** (eine Zeile pro Sample) neben die Aufnahme:
   `t_s, av_drift_ms, av_drift_available, duration_skew_ms, frames_captured, frames_emitted,
   frames_dropped_{coalesced,cfr,backpressure}, frames_duplicated, audio_discontinuities,
   mux_queue_depth, disk_fill_eta_s, rss_bytes, private_bytes, handle_count, gdi_objects,
   user_objects, health, bottleneck`.
4. **Abbruchkriterien** als *purer* Resolver `SoakAbortPolicy` (Input: rollierendes Metrik-Fenster →
   `{Continue}` oder `{Abort, reason}`), damit sie ohne echte Aufnahme unit-testbar sind. Abbruch bei
   nachhaltiger (nicht Einzel-Spike-)Verletzung: `|duration_skew_ms|` über Budget UND monoton
   wachsend; `|av_drift_ms|` über Budget (nur wenn `available`); Drop-Ratio über Budget; RSS-/Handle-
   **Leak-Steigung** (lineare Regression über die Laufzeit) über Schwelle; `health==Critical`
   anhaltend; `RecorderResult`-Failure.
5. Bei normalem Ende: **Soak-Report** (`soak-report-<ts>.json` + Markdown-Zusammenfassung) aus einem
   zweiten puren Aggregator `SoakMetricsAggregator` (min/max/mean/p99 je Metrik, Leak-Steigung,
   Gesamt-Drops, `durability_flush_count` falls exponiert) — plus ffprobe-Nachlauf auf die fertige
   Datei (clean EOF + Media-Dauer ≈ Wanduhr-Dauer).

**CI-fähiger Zwilling: `--synthetic`-Modus.** Statt GPU/NVENC/WGC fährt das Tool den deterministischen
Feeder aus `test_session_e2e_real_file.cpp` (als kleine Test-Seam-Bibliothek extrahiert, s. Schritt 2)
über *Media-Zeit* schneller-als-Echtzeit: 2 h Media-Dauer in Sekunden. Das übt Mux/Audio/Finalize +
die Report-/Abort-Plumbing bei Skalierung und fängt Skew-/Monotonie-Regressionen — **kann aber
prinzipiell keine echte Geräte-Clock-Drift und kein echtes RAM/Handle-Wachstum des Capture-Pfads
sehen** (ideale Clocks, kein GPU). Dieser Modus ist ausschließlich Harness-Validierung, nie A/V-Abnahme.

**Wichtig — Stats/Diagnostics im `--synthetic`-Modus:** `SetStatsCallback`/`SetDiagnosticsCallback`
sind `RecorderSession`-APIs (`recorder_session.h:579/589`), und die Timeline/Skew-Werte kommen aus dem
`SessionStatsCollector`, der **ausschließlich** in `RecorderSession::Record` instanziiert wird
(`recorder_session.cpp:674`) und `duration_skew_ms` aus den Media-Dauern berechnet
(`session_stats_collector.cpp:77-81`). Der extrahierte Feeder fährt aber `AudioThread`/`MuxThread`
direkt gegen `SessionState` — **ohne** `RecorderSession`. Ohne Gegenmaßnahme liefert `--synthetic`
daher *keine* Timeline-Samples (Schritt 4) und *keine* Skew-Snapshots für die Abort-Injektion
(Schritt 5). **Entscheidung:** Der `--synthetic`-Pfad instanziiert den internen
`SessionStatsCollector` selbst gegen den geteilten `SessionState` (Start vor dem Feeder, Stop danach —
derselbe Lebenszyklus wie in `Record`) und verdrahtet die Tool-Callbacks über `SessionState`. Das ist
kein „no-op"-Detail: es **exponiert bewusst interne Engine-src-Header** (`session_internal.h`,
`session_stats_collector.h` — heute PRIVATE `src`-Includes, `libs/recorder_core/CMakeLists.txt:1080`)
an `tools/soak`. Das ist die kleinste ehrliche Kopplung, die den CI-Zwilling messbar macht; sie ist
hier als bewusste Entscheidung ausgewiesen und **nicht** unter „keine Engine-Änderung" versteckt
(die Engine bleibt code-unverändert, aber ihre Test-Seam-Header werden für ein zweites Ziel sichtbar).
Alternativ könnte der Feeder hinter eine schmale `synthetic_session`-Fassade gezogen werden, die den
Collector kapselt und nur die öffentlichen Callback-Typen nach außen gibt — bevorzugt, wenn die
Header-Exposition den Rest von `tools/soak` mitzöge; die Fassade lebt dann in
`libs/recorder_core/testutil` (Schritt 2) und wird von Test und Tool geteilt.

Der Soak-Report teilt bewusst *kein* neues Schema-Format neu, sondern ist so gebaut, dass er später das
(geplante, noch nicht existierende) `session-<id>.json` aus `diagnostics-support-bundle-spec` §6.2
als Untermenge tragen kann — Cross-Reference dort, kein Doppelbau hier.

### Baustein 2 — A/V-Sync-Validierung (Klappensignal)

**Entscheidung: Signal-Erzeugung in C++ (Live), Analyse als standalone Python-Script
`scripts/dev/av-sync-check.py` mit System-`ffprobe`/`ffmpeg` — NICHT die App-DLLs.**

Alternativen abgewogen:

- *Analyse in das C++-Soak-Tool einbauen.* Verworfen: die App shipt bewusst nur das mux-only-FFmpeg-
  DLL-Set (avformat/avcodec/avutil/swresample) — `avfilter`/`swscale` sind **nicht** deployt
  (`KNOWN_LIMITATIONS.md:229-234`). Frame-Luma-/Audio-Amplituden-Analyse braucht genau diese Filter.
  In C++ nachzuziehen hieße, ungenutzte Deploy-Abhängigkeiten in die Engine/Tools zu holen — gegen die
  Deploy-Entscheidung. Ein Dev-Script nutzt ein *volles* System-ffmpeg (Entwicklermaschine), sauber
  getrennt.
- *Python + ffprobe.* Gewählt. Reproduzierbar, inspizierbar, folgt dem `gen-manifest-fixture.py`-
  Präzedenz, koppelt die Analyse von den App-DLLs ab.

**Klappensignal.** Ein minimaler Generator (Modus `--clapper` von `exosnap-soak` oder winziges
Geschwister-Tool): bei Aufnahme-Start und -Ende einen **Voll-Frame-Weiß-Blitz** über wenige Frames +
einen kurzen lauten **Beep** über das Default-Render-Endpoint. ExoSnap nimmt Desktop + Systemaudio
(SYS-Loopback) auf und fängt beides ein. Inhärent **live** — es braucht ein echtes Display und echte
Audio-Wiedergabe, die ExoSnap captured.

**Analyse-Script.** `av-sync-check.py <datei>`:

1. Extrahiert Video-Frame-PTS, an denen die Luma über einen Schwellwert springt (Blitz) — via
   `ffmpeg -vf "select='gt(scene,...)'"` bzw. `signalstats`/`blackframe`-artiger Schwelle und
   `-show_frames`-PTS.
2. Extrahiert Audio-Sample-Offset, an dem die Amplitude über einen Schwellwert springt (Beep) — via
   `silencedetect`/`astats`.
3. Rechnet `offset_start = blitz_pts_start − beep_pts_start` und `offset_end` analog; `drift =
   offset_end − offset_start` über die gemessene Spanne. Der **Exit-Code richtet sich nach der Drift**
   (0 = Drift-Rate/Stunde innerhalb Budget); die absoluten Offsets werden **advisory** gemeldet, nicht
   ins Pass/Fail gewertet (Begründung unten).

**Was diese Methode misst — und was nicht.** Blitz und Beep verlassen ExoSnas Beobachtung über
**verschiedene, unkontrollierte Emissions-Pfade**: der Blitz über GPU-Present → Display-Capture, der
Beep über den WASAPI-Render-Puffer → SYS-Loopback-Capture. Der gemessene `offset_start` enthält daher
eine geräteabhängige **Emissions-Skew** (typ. ~10–50 ms), die *kein* A/V-Fehler von ExoSnap ist,
sondern reine Hardware/OS-Latenz der Wiedergabe- und Capture-Kette. Ein absolutes Start-Offset-Budget
(z. B. „ein Frame @ 60 fps ≈ 16,7 ms", Offene Frage 1) ist mit dieser Methode **prinzipiell nicht
prüfbar** — es würde False-Fails/False-Passes im zweistelligen ms-Bereich erzeugen. Für die **Drift**
(`offset_end − offset_start`) kürzt sich die konstante Emissions-Skew heraus; **das** ist die für
`av-clock-slaving-spec` relevante Größe und die einzige, die ins Exit-Code-Budget geht. Wer das
absolute Offset härten will, braucht einen **kalibrierten Emissions-Skew-Abzug** (einmalige
Loopback-Referenzmessung des Setups) — bis dahin bleibt das absolute Offset advisory.

**Diese Methode ist die Drift-Abnahme (`Abnahme`) für `av-clock-slaving-spec`:** vor Clock-Slaving misst
sie die Ist-Drift, nach Clock-Slaving beweist sie die Kompensation. Sie ist **keine** Abnahme des
absoluten Start-Versatzes.

**CI-Split:** Die *Analyse* ist CI-fähig — ein kleines Golden-Fixture (kurzer Clip mit synthetisch
eingebranntem Klappensignal, unter `tests/fixtures/av-sync/` eingecheckt) + ein CI-Schritt, der das
Script darauf laufen lässt und den bekannten Offset assertet (Regressionsschutz für das Script selbst).
Die *echte Erfassung* (Blitz+Beep → ExoSnap → Datei) ist **User-live**.

### Baustein 3 — Recovery-Drills

**Entscheidung: Drill-Matrix {Recording, Finalize, Remux} × {Ordered-Stop, Process-Kill,
Powerloss}, in CI über einen Synthetic-Feeder-Subprozess wo möglich, Powerloss als Truncation-Proxy
(CI) + Hard-Reset (live).**

Warum Synthetic-Feeder statt GPU-Subprozess für die CI-Drills: der Feeder aus
`test_session_e2e_real_file.cpp` schreibt eine **echte MKV über den echten `MatroskaStreamWriter`**
(inkl. echtem Durability-Flush) und der App-Layer schreibt ein echtes Manifest — beides ohne GPU. Ein
`TerminateProcess` darauf übt die echte Teil-Datei-Recovery. Der GPU-Weg (probe_record/soak, mit
Kill) ist die Live-Obermenge, deckt aber keinen zusätzlichen Recovery-Code ab.

Erwartetes Ergebnis je Drill (das ist der Kern der Abnahme):

| Phase \ Abbruch | Ordered-Stop (`Stop()`) | Process-Kill (`TerminateProcess`) | Powerloss (Hard-Reset) |
|---|---|---|---|
| **Recording** | Saubere finalisierte Datei; Manifest entfernt; demuxbar; Dauer ≈ Ziel. | Manifest überlebt (`finalized=false`); Scan() listet es; `Finish` → `RemuxToMkv`-Repair → demuxbar bis letztem geflushten Cluster; Verlust ≤ Reorder-Window **+ 1 nicht-gerenderter Cluster** (~`kClusterBoundaryMs`, ~2 s — das ist der eigene Verlustmodell-Kommentar des Writers, `matroska_stream_writer.cpp:576-580`). | Wie Process-Kill (also Reorder + 1 Cluster), zusätzlich Verlust des OS-Write-Back seit letztem `FlushFileBuffers` → Gesamt-Verlustfenster ≤ `kDurabilityFlushInterval` (2 s) + Reorder + 1 nicht-gerenderter Cluster. |
| **Finalize** | (n/a — Stop *löst* Finalize aus.) | Datei un-finalisiert → `Finish` → `RemuxToMkv`-Repair; `finalized=false`. Kein Rename-Pfad. | Wie Process-Kill; da finaler Flush Teil von Finalize ist, kann der Trailer fehlen → Repair-Pfad greift. |
| **Remux (MP4)** | (n/a — Remux läuft nach Stop im App-Layer.) | Transiente MKV überlebt (der Coordinator-Remux benennt sie bei **Erfolg** zu `.edit.mkv` um bzw. räumt sie, `RecordingCoordinator.cpp:1626-1657` — vor Erfolg passiert nichts, also überlebt sie den Kill); Manifest `intended_container=mp4` überlebt; nächster Start re-remuxt via `RecoveryService.Finish` → `RemuxToProgressiveMp4` auf einen **frischen** Pfad (`ResolveUniqueOutputPath`, `RecoveryService.cpp:157-158`). **Ist-Befund:** eine schon am `final_output_path` liegende Halb-MP4 wird NICHT geräumt — s. „Befund" unten. | Wie Process-Kill (dieselbe Ebene). **Kein Code verwirft die korrupte Halb-MP4** — sie bleibt am user-sichtbaren Zielpfad liegen, während die Recovery daneben eine korrekte MP4 unter *anderem* Namen erzeugt. Die MKV bleibt die Wahrheit; `product-spec.md:162` deckt nur den *In-Prozess*-Remux-Fehler ab, nicht den Crash-Fall. |

**Befund (durch diesen Drill aufgedeckt): stale-partial-MP4 am Zielpfad.** Der App-Layer-Remux
schreibt `RemuxToProgressiveMp4` **direkt** in den finalen, user-sichtbaren MP4-Pfad
(`RecordingCoordinator.cpp:1619`), nicht in einen Temp-Pfad mit atomarem Rename. Ein Kill/Powerloss
mitten im Remux hinterlässt daher eine korrupte Halb-MP4 genau dort, wo der User sein Ergebnis
erwartet. `RecoveryService.Finish` erzeugt beim nächsten Start via `ResolveUniqueOutputPath`
(`RecoveryService.cpp:157-158`) eine korrekte MP4 unter *anderem* Namen und lässt die korrupte Datei
stehen — nichts überschreibt oder räumt sie. Der Manifest-Eintrag trägt bereits das nötige Feld:
`final_output_path` wird beim Start gesetzt (`RecordingCoordinator.cpp:960`). **Empfohlenes
zu-implementierendes Verhalten** (nicht Bestandteil des reinen Mess-Scopes dieser Spec, aber als
konkrete Fix-Anweisung ausgewiesen, damit der Drill-Test nicht als roter Test ohne Reparatur-Auftrag
landet): Der Remux schreibt in einen Temp-Pfad und benennt bei Erfolg atomar auf `final_output_path`
um; **und/oder** `RecoveryService.Finish` räumt/überschreibt den `final_output_path` aus dem Manifest,
bevor es die recovte MP4 platziert. Der `recovery_drill_tests`-Drill (Schritt 8) assertet dann den
gewünschten Endzustand (kein korruptes Artefakt am Zielpfad); bis der Fix landet, ist die Matrix-Zelle
oben der dokumentierte Ist-Befund und die Drill-Assertion muss ihn als *known-issue*-Xfail führen,
nicht als grün behaupten.

**Powerloss-Ehrlichkeit.** Echter Stromausfall verliert den OS-Write-Back-Cache — das ist in CI
**nicht** herstellbar. Zwei Ersatz-Ebenen, klar getrennt:

- *CI-Proxy (Repair-Robustheit):* Die Teil-MKV an einem beliebigen Offset (nicht Cluster-Grenze)
  truncaten und assertieren, dass `RemuxToMkv`-Repair **nicht crasht** und bis zum letzten vollständigen
  Cluster/Cues rettet. Das modelliert „Tail verloren", beweist aber *nicht* die Durability-Zusage.
- *User-live (Durability-Zusage):* Echter Hard-Reset / VM-Forced-Off je Phase; verifiziert, dass das
  ≤ 2 s + Reorder-Fenster hält und die Datei recovert. **Nur dieser Lauf beweist die tatsächliche
  Powerloss-Zusage.**

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit Testansatz. Reihenfolgetreu; pure Teile zuerst, damit
CI-Deckung existiert, bevor das GPU-Tool draufsattelt.

1. **`SoakMetricsAggregator` + `SoakAbortPolicy` (pur, headerbasiert, kein I/O).**
   Neu unter `tools/soak/` (oder `libs/` wenn von Tests geteilt): nimmt eine Sequenz Metrik-Samples,
   liefert Zusammenfassung (min/max/mean/p99, Leak-Steigung per linearer Regression) bzw.
   Continue/Abort. Keine WinAPI, kein FFmpeg. **Test (CI):** `soak_policy_tests` gegen synthetische
   Timelines — ideale Kurve → kein Abort; injizierte Leak-Steigung → Abort mit Grund; Skew-Rampe →
   Abort. Registrieren via `exosnap_add_gtest` ohne `live`-Label.

2. **Synthetic-Feeder als Test-Seam-Bibliothek extrahieren.**
   Den In-Test-`video_feeder` + `MockAudioCaptureSource` aus `test_session_e2e_real_file.cpp:79-153,
   320-415` in eine kleine wiederverwendbare Einheit (`libs/recorder_core/testutil/synthetic_session.*`
   o. ä.) heben, ohne das bestehende Verhalten zu ändern. **Test (CI):** der bestehende
   `test_session_e2e_real_file` läuft unverändert grün gegen die extrahierte Version (reiner Refactor).

3. **`exosnap-soak` Tool-Skelett — echter Pfad + `--synthetic`.**
   `tools/soak/CMakeLists.txt`: das Ziel **baut ganz normal auf CI** (`recorder_core` ist wegen der
   vendored NVENC-Header immer als Target vorhanden, s. Ist-Zustand) — ein `if(TARGET recorder_core)`
   gated hier real *nichts* und ist höchstens Defensive gegen einen künftig header-losen Build. Die
   echte Trennung ist **Runtime**: der `--synthetic`-Pfad läuft überall; der echte
   `RecorderSession::Record`-Pfad **muss bei fehlender GPU/NVENC sauber und sofort fehlschlagen**
   (klare Fehlermeldung + non-zero Exit), **nicht hängen** — die Spec schreibt das als Verhalten fest,
   damit ein versehentlicher CI-Aufruf des echten Pfads deterministisch scheitert statt zu blockieren.
   `main.cpp` parst `--minutes`, `--synthetic`, Container/Codec (geteilte Args), fährt
   `RecorderSession::Record` bzw. den Synthetic-Feeder, Stop per Timer/Ctrl-C. **Test (CI):** ein kurzer
   `--synthetic --minutes` (wenige Sekunden Media-Zeit) in einem CTest, der eine gültige Datei erzeugt
   und sie mit dem **gevendorten libavformat** (`DemuxAndInspect`-Muster, `test_session_e2e_real_file.cpp:200-272`)
   auf clean EOF prüft — kein System-`ffprobe` im CTest (deterministisch, versionsgepinnt). Realer
   GPU-Lauf: manuell/live.

4. **Metrik-Erfassung + Prozess-Sampler + Report.**
   Callbacks abonnieren, 1-Hz-psapi-Sampler-Thread (`GetProcessMemoryInfo`/`GetProcessHandleCount`/
   `GetGuiResources`), JSON-Lines-Timeline + Report via Aggregator aus Schritt 1, Media-Validierung
   der fertigen Datei (s. u.). Im echten Pfad speist `RecorderSession` die Callbacks; im
   `--synthetic`-Pfad instanziiert dieser Schritt den internen `SessionStatsCollector` (bzw. die
   `synthetic_session`-Fassade aus Schritt 2) selbst gegen den geteilten `SessionState` und verdrahtet
   dieselben Callbacks — **ohne diese Zeile liefert `--synthetic` keine Timeline** (s. Baustein 1).
   **Test (CI):** JSONL-Writer + Report-Serialisierung gegen einen In-Memory-Sample-Vektor (der
   Sampler-Thread selbst ist live). Verify: `--synthetic`-Lauf schreibt eine wohlgeformte, **nicht
   leere** Timeline (Skew-Spalte gefüllt) + Report.

5. **Abbruchkriterien verdrahten.**
   `SoakAbortPolicy` aus Schritt 1 in die Live-Schleife hängen: bei Abort `session.Stop()` + Exit
   non-zero + Grund in den Report. **Test (CI):** Injektions-Test über `--synthetic` mit einer
   erzwungenen Skew-Rampe (Feeder-Parameter) → Tool bricht ab und meldet den Grund.

6. **A/V-Analyse-Script `scripts/dev/av-sync-check.py`.**
   ffprobe/ffmpeg-basierte Blitz-PTS- + Beep-Offset-Extraktion, Offset/Drift/Budget, Exit-Code (Exit-Code
   an der Drift, absolutes Offset advisory — s. Baustein 2). Golden-Fixture unter
   `tests/fixtures/av-sync/` + CI-Schritt (in `ci.yml` `lint`- oder neuem `dev-scripts`-Job, kein GPU),
   der das Script auf dem Fixture laufen lässt und den bekannten Offset/Drift assertet. **Wichtig:** der
   `lint`-Runner (`windows-2022`, `ci.yml:34`) hat **kein garantiertes ffmpeg** — der Workflow muss
   ffmpeg/ffprobe **explizit provisionieren und die Version pinnen/prüfen** (Install-Schritt + `-version`-
   Assertion), sonst ist der Job umgebungsabhängig grün/rot. Anders als der Synthetic-CTest (Schritt 3,
   gevendorter Demuxer) braucht dieses Script echte `ffmpeg`-Filter (`signalstats`/`silencedetect`), die
   im mux-only-DLL-Set der App fehlen — deshalb System-ffmpeg, deshalb die Provisionierungspflicht.
   **Test (CI):** Script vs. Golden-Fixture. **Live:** echte Erfassung.

7. **Klappensignal-Generator (`--clapper`).**
   Voll-Frame-Weiß-Blitz + WASAPI-Beep bei Start/Ende, deterministisch getimt relativ zu Record-Start/
   -Stop. **Kein CI-Test** (braucht Display+Speaker); Verify ausschließlich User-live via Baustein-2-
   Analyse.

8. **Recovery-Drill-Test-Harness (`recovery_drill_tests`, App-Layer).**
   Kindprozess: schreibt einen Manifest-Eintrag + fährt einen Synthetic-Feeder-MKV-Schreibvorgang; der
   Elternprozess treibt die drei Drills (Ordered-Stop; `TerminateProcess` nach *deterministischem*
   Phasen-Marker — z. B. nach beobachteten N Clustern via Dateigröße oder stderr-Sentinel, damit
   Recording vs. Finalize vs. Remux zuverlässig getroffen wird) und assertet je Zelle der Matrix gegen
   `RecoveryService.Scan()/Finish()`. **Test (CI):** die Ordered-Stop- und Process-Kill-Zeilen für
   Recording + Finalize + Remux(MP4). Registrieren ohne `live`-Label.
   **Abgrenzung — was dieser Drill NICHT übt:** der Kindprozess **schreibt den Manifest-Eintrag selbst**
   und baut damit die Add-vor-Start-/`UpdateFinalized`-vor-Remux-/`Remove`-nach-Erfolg-Choreografie des
   echten `RecordingCoordinator` (`RecordingCoordinator.cpp:954`, `1489-1490`, `1670-1671`) **nach**
   statt sie auszuführen (der echte Coordinator braucht `RecorderSession`/GPU). Ein *Ordering*-Bug im
   Coordinator — die eigentliche Crash-Fenster-Fehlerklasse — bliebe damit unsichtbar, obwohl der Drill
   grün ist. Der Drill validiert die **Recovery-Seite** (`Scan`/`Finish`/Repair) gegen realistische
   Teil-Dateien, nicht die **Aufzeichnungs-Seite** der Manifest-Reihenfolge. Das Gegenstück ist der
   **Live-Kill-Drill gegen die echte App** (Schritt 10 Runbook, durch den User): nur er killt den
   echten Coordinator und deckt Ordering-Fehler im Add/UpdateFinalized/Remove-Zeitfenster auf.

9. **Powerloss-Truncation-Proxy-Test + Live-Runbook.**
   `recovery_truncation_tests`: eine geflushte Teil-MKV an zufälligen Offsets truncaten, `RemuxToMkv`-
   Repair assertieren (kein Crash, salvage ≤ letzter Cluster). **Test (CI).** Plus Doku (Schritt 10).

10. **Doku-Runbook `docs/dev/soak-and-recovery-drills.md`.**
    Wie man den 2-h-Soak fährt, das Klappensignal-Setup, die Live-Powerloss-Drills je Phase, die
    Schwellen/Budgets, wie man Report + Timeline liest. Kein Eintrag in `docs/product-spec.md`
    (kein user-sichtbares Verhalten ändert sich — die Recovery-/Durability-Zusagen stehen bereits in
    §4/§Crash recovery). Ein knapper Verweis in `docs/roadmap.md` (0.10-Zeile) auf die Infrastruktur.

## Test-/Verify-Plan

**CI-fähig (kein GPU, deterministisch, in `ci.yml build-test` ohne `live`-Label):**

- `soak_policy_tests` — Aggregator + Abort-Resolver gegen synthetische Timelines (Schritt 1).
- Refactor-Grün von `test_session_e2e_real_file` gegen die extrahierte Seam (Schritt 2).
- `exosnap-soak --synthetic` Kurzlauf → gültige Datei + wohlgeformte Timeline/Report + Abort-Injektion
  (Schritte 3–5).
- `av-sync-check.py` gegen Golden-Fixture (Schritt 6) — in einem `lint`-artigen Job (Python + **explizit
  provisioniertes/gepinntes** System-ffmpeg), nicht im GPU-Pfad. Exit-Code an der Drift, Offset advisory.
- `recovery_drill_tests` — Ordered-Stop + Process-Kill für Recording/Finalize/Remux (Schritt 8). Die
  Remux×Kill/Powerloss-Zelle assertet den *gewünschten* Endzustand (kein korruptes Artefakt am
  Zielpfad) und läuft bis zum stale-partial-Fix als **Xfail** (Ist-Befund, Baustein 3).
- `recovery_truncation_tests` — Powerloss-Repair-Robustheit-Proxy (Schritt 9).

**Nur User-live (Entwicklermaschine mit NVIDIA-GPU + echtem Display/Audio; nie CI):**

- Der echte 2-h+-Soak: `exosnap-soak --minutes 120` gegen die definierte Quelle; Report zeigt keine
  Abort-Auslösung, Leak-Steigung unter Schwelle, Drift/Skew im Budget, Datei demuxbar mit
  Dauer ≈ Wanduhr.
- Die echte A/V-Klappen-Erfassung: `--clapper`-Lauf → `av-sync-check.py` auf der echten Datei → Offset
  und Drift im Budget. Dies ist die Abnahme für `av-clock-slaving-spec` (vorher/nachher).
- Die echten Powerloss-Drills: Hard-Reset / VM-Forced-Off in jeder der drei Phasen; verifizieren, dass
  das ≤ 2 s + Reorder-Fenster hält und die Datei recovert.

**Bewusst NICHT gebaut:**

- Kein GPU-Recording in CI (Runner ohne GPU; `probe_record`/`exosnap-soak` bleiben Dev-Maschine).
- Keine echte Powerloss-Automatisierung in CI (physikalisch unmöglich; Proxy + Live-Drill stattdessen).
- Keine Clock-Slaving-*Implementierung* hier (das ist `av-clock-slaving-spec`; diese Spec misst/nimmt
  ab).
- Keine neuen Engine-Metriken und keine Engine-*Code*-Änderung für den Soak (alle Metriken existieren;
  RAM/Handles bleiben Host-Sache). Ausdrücklich *doch* gebaut: der `--synthetic`-Zwilling exponiert
  interne `src`-Header (`session_internal.h`, `session_stats_collector.h`) an `tools/soak`, damit er
  Timeline/Skew liefert — bewusste Kopplung, s. Baustein 1.
- Kein voller Test-Muster-Generator und kein allgemeiner Fuzzer über den gezielten Truncation-Proxy
  hinaus.
- Kein separates `session-<id>.json` hier (Cross-Reference zu `diagnostics-support-bundle-spec`).
- **Keine CI-Abdeckung der Coordinator-Manifest-Choreografie.** Die reale Add/`UpdateFinalized`/`Remove`-
  Reihenfolge (`RecordingCoordinator.cpp:954`/`1489-1490`/`1670-1671`) wird vom `recovery_drill_tests`-
  Kind nachgebaut, nicht ausgeführt; ein Ordering-Bug im echten Coordinator bleibt in CI unsichtbar.
  Abgedeckt wird das ausschließlich durch den **Live-Kill-Drill gegen die echte App** (Runbook, User).
- **Kein automatischer Fix der stale-partial-MP4.** Diese Spec *deckt den Befund auf* (halb-geschriebene
  MP4 bleibt am Zielpfad, s. Baustein 3 „Befund") und benennt die Fix-Optionen, baut den Fix aber nicht
  selbst — der Remux-in-Temp-mit-atomarem-Rename bzw. das `Finish`-räumt-`final_output_path` ist eigene
  Coordinator/Recovery-Arbeit; der Drill assertet den Zielzustand und führt ihn bis dahin als Xfail.

## Risiken

- **RSS/Handle-Wachstum hat legitime Nicht-Leak-Ursachen** (Treiber-Caches, Heap-Fragmentierung). Die
  Leak-Steigung muss über ein langes Baseline-Fenster gefittet und im Zweifel *advisory* statt harter
  Abort sein — sonst False-Positive-Abbrüche. → Schwelle konservativ, Baseline-Fenster groß.
- **Der `--synthetic`-Modus maskiert echte Geräte-Clock-Drift** (ideale Clocks). Muss überall als reine
  Harness-Validierung gekennzeichnet sein, nie als A/V-Sync-Abnahme durchgehen.
- **Emissions-Skew verfälscht das absolute A/V-Offset.** Blitz (GPU-Present → Display-Capture) und
  Beep (WASAPI-Render → SYS-Loopback) laufen über verschiedene, unkontrollierte Latenzpfade; der
  gemessene `offset_start` enthält ~10–50 ms geräteabhängige Skew, die kein ExoSnap-A/V-Fehler ist.
  Nur die **Drift** kürzt sie heraus und geht ins Exit-Code-Budget; das absolute Offset bleibt advisory
  (oder braucht einen kalibrierten Skew-Abzug), sonst produziert das Live-Gate False-Fails/-Passes.
- **Klappen-Schwellen hängen von Capture-Range (Full/Limited) und Tonemap ab** — bei HDR-Sessions wird
  der Blitz tonemappt. Das Script muss range-robust sein; HDR-A/V-Sync als eigener Lauf.
- **ffprobe/ffmpeg-Versionsunterschiede** in der PTS-Meldung. Für den Dev-Soak: ffmpeg-Version im
  Runbook pinnen. Für den **CI-Python-Job** (`av-sync-check.py` gegen das Golden-Fixture) reicht das
  nicht — der `lint`-Runner liefert kein garantiertes ffmpeg; der Workflow muss es explizit
  installieren und die Version prüfen, sonst flaked der Job je Runner-Image. Für den **Synthetic-CTest**
  gibt es das Problem gar nicht: er nutzt den gevendorten, versionsgepinnten libavformat-Demuxer
  (`DemuxAndInspect`) statt System-ffprobe.
- **`TerminateProcess`-Timing ist nichtdeterministisch** — der Drill muss die Phase über einen
  deterministischen Marker treffen (beobachtete Cluster / stderr-Sentinel), sonst trifft der Kill
  unzuverlässig Recording vs. Finalize vs. Remux und der Test flaked.
- **Durability-Flush ist best-effort**; `FlushFileBuffers` kann auf manchen Volumes langsam sein — ein
  Soak auf langsamer Platte kann die Metriken selbst stören. Volume im Report festhalten.
- **Golden-Fixture-Größe** darf das Repo nicht aufblähen — der A/V-Fixture-Clip muss sehr kurz sein
  (wenige Sekunden, niedrige Auflösung/Bitrate).

## Offene Fragen (echte Produktentscheidungen)

1. **A/V-Toleranzbudget (konkrete Zahlen):** Die pass/fail-relevante Größe ist die **Drift-Obergrenze**
   (z. B. ≤ 1 ms je 10 min, oder ≤ 10 ms über 2 h) — welche Zahl gilt als bestanden? Ein absolutes
   **Start-Offset**-Budget (ein Frame @ 60 fps ≈ 16,7 ms) ist mit der Klappensignal-Methode nicht
   sauber prüfbar, weil die Emissions-Skew der Wiedergabe-/Capture-Kette (~10–50 ms) das Offset
   dominiert (s. Baustein 2). Entscheidung offen: absolutes Offset dauerhaft **advisory** lassen, oder
   einen kalibrierten Emissions-Skew-Abzug einführen und dann doch budgetieren?
2. **Soak-Abnahme-Umfang + Gate-Status:** Pflicht-Dauer (2 h? 4 h? über Nacht?) und welche
   Container/Codec/Audio-Kombinationen müssen bestehen, damit 0.10 als „gehärtet" gilt? Ist der Soak
   ein *hartes* Release-Gate (wie die bestehende Live-Verify-Liste) oder advisory?
3. **Leak-Kriterium:** absolute RSS-/Handle-Obergrenze vs. Steigung — und ist ein Soak-Leak ein
   Release-Blocker oder advisory?
4. **Powerloss-Live-Drill:** ist der echte Hard-Reset-Drill je Phase eine verpflichtende Pre-0.10-Live-
   Abnahme (analog zum bestehenden 0.9-Live-Gate) oder advisory?

## Adversarialer Review — Ergebnis

- **Einwand 1 (major, Remux×Powerloss/Kill-Zelle) — EINGEARBEITET.** Verifiziert: `RemuxToProgressiveMp4`
  schreibt direkt in den finalen MP4-Pfad (`RecordingCoordinator.cpp:1619`); nichts räumt die korrupte
  Halb-MP4, `Finish` weicht via `ResolveUniqueOutputPath` auf anderen Namen aus. Die zitierte Erfolgs-
  Löschung `RecoveryService.cpp:170-177` ist die Recovery-*Zeit*-Ebene, nicht der Coordinator-Remux
  (Erfolg = Rename→`.edit.mkv`, `1626-1657`). Matrix auf Ist-Zustand korrigiert (korrupte MP4 bleibt
  liegen), Zitat auf die richtige Ebene gezogen, „Befund"-Absatz + Fix-Anweisung (Temp+atomarer Rename
  bzw. `Finish` räumt `final_output_path`) + Xfail-Regel für den Drill ergänzt.
- **Einwand 2 (major, `--synthetic` liefert keine Stats) — EINGEARBEITET.** Verifiziert:
  `SessionStatsCollector` wird nur in `RecorderSession::Record` (`recorder_session.cpp:674`) erzeugt;
  der Feeder fährt `AudioThread`/`MuxThread` direkt gegen `SessionState` ohne `RecorderSession`; `src`
  ist PRIVATE (`CMakeLists.txt:1080`). Expliziter Schritt/Entscheidung ergänzt (Synthetic instanziiert
  den Collector selbst bzw. `synthetic_session`-Fassade) samt bewusster src-Header-Exposition; „keine
  Engine-Änderung"-Formulierung in Design + „NICHT gebaut" entsprechend präzisiert.
- **Einwand 3 (major, absolutes Offset unmessbar) — EINGEARBEITET.** Emissions-Skew (Blitz via
  Present→Display-Capture vs. Beep via WASAPI→Loopback, ~10–50 ms) korrekt als Nicht-ExoSnap-Latenz
  eingeordnet; Methode explizit als **Drift**-Abnahme deklariert, absolutes Offset advisory, Exit-Code
  an der Drift; Offene Frage 1 umformuliert; eigener Risiko-Punkt ergänzt.
- **Einwand 4 (minor, Verlust-Budget) — EINGEARBEITET.** Writer-Kommentar `matroska_stream_writer.cpp:576-580`
  bestätigt den zusätzlichen nicht-gerenderten Cluster; Recording-Zeile (Process-Kill *und* Powerloss)
  um „+ 1 nicht-gerenderter Cluster (~`kClusterBoundaryMs`, ~2 s)" ergänzt.
- **Einwand 5 (minor, „GPU-gated" falsch) — EINGEARBEITET.** `nvEncodeAPI.h` ist vendored
  (`third_party/nvidia/nvEncodeAPI.h`), `recorder_core`/`probe_record` bauen auf CI; `if(TARGET
  recorder_core)` gated nichts. Ist-Zustand-Bullet korrigiert (Build-Gate = NVENC-Header vs. Runtime-
  Gate = GPU); Schritt 3 stellt fest, dass der echte Pfad zur Laufzeit sauber fehlschlägt statt hängt.
- **Einwand 6 (minor, System-ffprobe-Abhängigkeit) — EINGEARBEITET.** Synthetic-CTest nutzt jetzt den
  gevendorten `DemuxAndInspect`-Demuxer (`test_session_e2e_real_file.cpp:200-272`) statt System-ffprobe;
  der Python-`lint`-Job (`ci.yml:34`) muss ffmpeg explizit provisionieren/pinnen — in Schritt 3, 6,
  Risiken und Test-Plan verankert.
- **Einwand 7 (minor, Schritt 8 übt Choreografie nicht) — EINGEARBEITET.** Verifiziert: Add/`UpdateFinalized`/
  `Remove` liegen im Coordinator (`954`/`1489-1490`/`1670-1671`), der Drill-Kindprozess baut sie nach.
  Nicht-Abdeckung explizit in Schritt 8 + „Bewusst NICHT gebaut" ausgewiesen und der Live-Kill-Drill
  gegen die echte App als Gegenstück benannt.

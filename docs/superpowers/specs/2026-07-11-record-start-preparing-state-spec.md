# M-9: StartRecording-Gerätearbeit off-thread (Preparing-State)

> **SHIPPED (2026-07-12, PR #198).** Einen Tag nach dieser Spec umgesetzt und gemergt — thin-gate-
> fat-worker, `PrepareContext`-Snapshot, Webcam-Ownership-Fix, Hotkey-Cancel, kein dedizierter
> Cancel-Button, Re-Entrancy-Guard, `output.PreparingStateTest.*` +
> `viewmodel.RecordViewModelPreparingTest.*`, `Record / Preparing`-Visual-Szenario,
> `docs/product-spec.md` §"Preparing" und ADR 0041 aktualisiert — alles wie unten spezifiziert.
> Diese Spec bleibt als historischer Entwurf/Referenz stehen; nichts hier ist mehr offen.
>
> Spec-Status (Original): umsetzungsreif, adversarial gehärtet. Read-only erhoben gegen `main` @ #192
> (Branch `fix/video-timeline-honesty`). Jede Ist-Zustands-Aussage trägt eine Datei:Zeile-Referenz.
> Umsetzung später durch Opus/Sonnet allein anhand dieser Spec. Ein adversarialer Review-Durchgang
> (9 Einwände) ist eingearbeitet — s. Abschnitt „Adversarialer Review — Ergebnis" am Ende.

## Problem

`RecordingCoordinator::StartRecording` läuft heute **vollständig auf dem aufrufenden Thread**,
und das ist der GUI-Thread: Der Record-Button → `RecordPage::onStart` → `doStartRecording`
ruft `coordinator_->StartRecording(...)` synchron auf
(`app/pages/RecordPage.cpp:3848`, Rückgabewert wird ignoriert). Zwischen Button-Klick (bzw.
Countdown-Ende) und dem Start des Aufnahme-Threads blockiert der GUI-Thread durch:

- einen synchronen Freispeicher-Query auf das Ausgabevolume
  (`RecordingCoordinator.cpp:626`, kann auf Netz-/Wechsellaufwerken stallen),
- Dateisystem-Arbeit: `GenerateOutputPath` / `ResolveAvailableOutputPath` (probiert Kandidaten
  mit `exists()`) und `create_directories` (`RecordingCoordinator.cpp:686-722`),
- eine **frische DXGI-Display-Facts-Abfrage** über `RefreshedDisplayFacts()` →
  `RefreshDisplayFacts()` → `capability::CapabilityBuilder::QueryDisplayFacts()`
  (`RecordingCoordinator.cpp:772`, `:337-345`, `:339`),
- `webcam_service_.Start(...)` — Media-Foundation-Kamera-Open, dokumentiert bis zu mehreren
  hundert ms (`RecordingCoordinator.cpp:916-927`; nur wenn nicht ohnehin schon ein Reader läuft),
- den **Preview-Release-Handshake** `preview_capture_release_hook_()`
  (`RecordingCoordinator.cpp:989-990`), der laut Header-Kontrakt „block until the release has
  actually happened" (`RecordingCoordinator.h:230-239`) — konkret ein `RequestEngineLease()`
  mit `ack_cv_.wait_for(..., 750ms)` (`app/services/DxgiCaptureHubService.cpp:97-114`).

Der eigentliche schwere Geräte-Open (Capture + NVENC) läuft bereits off-thread in
`RecordingThreadProc` → `session_.Record()` (`RecordingCoordinator.cpp:1377,1379`); auch
`session_.Validate()` ist entgegen der Review-Formulierung **billig und rein** (nur
Container/Codec-/Pfad-Regeln, kein Geräte-Open — `libs/engine/src/recorder_session.cpp:155`
ff.). Der GUI-Stall stammt also aus DXGI-Facts, Webcam-Start, Release-Handshake und den
FS-/Disk-Operationen.

Sichtbare Folgen:

1. **UI-Freeze-Beat am Record-Klick.** Der Fensterinhalt (inkl. laufender Preview-Repaints,
   Hover, Titelbar) friert für die Dauer der Gerätearbeit ein.
2. **Der `Preparing`-State ist heute kosmetisch wirkungslos.** `PostStateChange(Preparing)`
   steht bereits bei `RecordingCoordinator.cpp:724`, aber `PostStateChange` marshallt per
   `Qt::QueuedConnection` (`RecordingCoordinator.cpp:2163-2169`). Da `StartRecording` danach
   synchron weiterläuft und erst bei `:967` `Recording` postet, kehrt die Kontrolle erst nach
   *beiden* Posts zur Event-Loop zurück: `SetState(Preparing)` und `SetState(Recording)` werden
   ohne Repaint dazwischen abgearbeitet — „Preparing…" wird nie gezeichnet.
3. **Kein sichtbares Feedback bei Countdown=0.** Default-Countdown ist **0 s**
   (`docs/product-spec.md:88`); dann ist Preparing die einzig mögliche Pre-Record-Rückmeldung —
   und die fehlt heute faktisch.

Ziel (Review M-9, `.workspace/review-fable-2026-07-10.md:125`, `:206`): Gerätearbeit in einen
echten, off-thread ausgeführten `Preparing`-State verlagern; UI-State-Maschine sauber erweitern
(ViewModel-Prädikate, Statuspill, Hotkey-Verhalten); Abbruch und Fehler in Preparing führen sauber
nach Ready zurück; kein Doppelstart.

## Ist-Zustand (mit Datei:Zeile-Referenzen)

### Coordinator-Kontrolle & Threading
- `state_` ist seit #175 `std::atomic<UiRecordingState>` (`RecordingCoordinator.h:353`).
- `is_recording_` / `is_paused_` sind `std::atomic<bool>` (`.h:350-351`).
- `StartRecording` (`RecordingCoordinator.cpp:607-997`) läuft komplett synchron; Ablauf in
  Reihenfolge:
  - `StopMicMeter()` (`:610`).
  - Re-Entrancy-Guard `if (is_recording_) return false;` (`:612`) — deckt **nur** die
    Recording-Phase ab, nicht ein bereits laufendes Preparing.
  - Disk-Pre-Check, synchroner Provider-Query, ggf. `Failed`+Result (`:619-655`).
  - `ValidateOutputFolder` (`:657-674`).
  - Startbarer-State-Gate: `Ready | Completed | Failed | ArmedFromRecovery` (`:675-678`);
    `has_caps_`-Gate (`:679-680`).
  - Pfaderzeugung/-auflösung + `create_directories`, setzt `current_output_path_`
    (`:686-722`).
  - `PostStateChange(UiRecordingState::Preparing)` (`:724`) — heute wirkungslos (s. o.).
  - Config-Bau: `ToRecorderCoreConfig`, Videoparameter, Keyframe-Mapping, `ReconcileOutputFormat`
    (CFR-Zwang für MP4), Webcam-/Audio-Plan (`:726-870`) — reine/threadsichere Funktionen.
  - HDR10-Native-Ableitung über `RefreshedDisplayFacts()` (DXGI-Query!) + `ApplyHdr10NativeEncode`
    (`:772-791`).
  - `session_.Validate(config, ...)`, ggf. `Failed`+Result (`:872-890`) — billig/rein.
  - Setzen der Session-Callbacks (Stats/Meter/Diagnostics/Preview-Handle/Segment) (`:892-914`).
  - `EmitInitializingDiagnostics()` (`:908`) — threadsicherer PostDiagnostics.
  - Webcam-Start (`:916-927`).
  - `is_recording_ = true;` (`:929`), Marker-Reset, Segment-Remux-Reset (`:929-946`).
  - Recovery-Manifest-Eintrag schreiben, `current_manifest_id_` setzen (`:948-965`) —
    `recovery_manifest_store_->Add` wird auch vom Mux-Worker-Thread aufgerufen
    (`OnSegmentCompleted`), ist also bereits für Fremd-Thread-Aufruf ausgelegt.
  - `PostStateChange(UiRecordingState::Recording)` (`:967`).
  - `StartDiskMonitor(...)` (`:982`) — startet `disk_monitor_thread_`.
  - `preview_capture_release_hook_()` (`:989-990`) — blockierender Lease-Handshake.
  - `recording_thread_ = std::jthread(... RecordingThreadProc ...)` (`:992-994`).
- `RecordingThreadProc` (`:1377`) ruft `session_.Record(config)` (`:1379`, blockiert für die
  gesamte Aufnahme), setzt danach `is_recording_ = false` und baut das Result/Remux.
- `PostStateChange` (`:2163-2169`) besitzt **keinen** `QCoreApplication::instance()==nullptr`-
  Fallback. **Korrektur gegenüber einer früheren Fassung dieser Spec:** einen solchen
  Direktaufruf-Fallback hat **nur** `PostDiagnostics` (`:2216-2219`). `PostStats` (`:2189-2200`),
  `PostResult` (`:2181-2187`) und `PostStateChange` selbst rufen `invokeMethod` unbedingt auf.
  Ohne laufende Qt-App würde `invokeMethod(nullptr, …)` verpuffen. Für den Testplan relevant.
- `FillResultFormat` (`:2171-2179`) liest die **mutablen** Member `resolved_user_config_` und
  `caps_`; es wird in allen sechs Worker-Fehlerpfaden aufgerufen (s. u.) und ist damit Teil der
  Shared-State-Frage (Schritt 2), sobald der Rumpf off-thread läuft.
- `RevalidateCapabilities` (`:532-553`) early-returnt, solange
  `state_ ∈ {Preparing, Recording, Paused, Stopping, ArmedFromRecovery}` (`:535-539`) — d. h. ein
  während Preparing gehaltener Zustand schützt `caps_.runtime.displays` bereits vor
  konkurrierender Re-Validierung.
- `StopRecording` (`:1067-1068`) ist ein No-op, solange `!is_recording_` — greift also heute
  **nicht** während Preparing.
- **Webcam-Geräte-Ownership hängt an `is_recording_`.** `SyncWebcamService` (`:587-601`) berechnet
  `want_running` aus `is_recording_ || webcam_preview_active_ || webcam_settings_preview_active_`
  (`:590-591`) und ruft sonst `webcam_service_.Stop()` (`:593`). `SetWebcamSettings` (`:559-575`)
  bindet seinen `force_restart`-Pfad an `!recording` mit `recording = is_recording_` (`:566`,
  `:574`). Der GUI-seitige Treiber ist `RecordPage::syncWebcamPreviewCapture` (`:1699-1711`):
  `idle = (Ready||Completed)`; jeder queued State-Callback ruft `refresh()` → diese Funktion
  (`RecordPage.cpp:4226`). Der Worker-Rumpf öffnet die Kamera direkt über
  `webcam_service_.Start(...)` (`:916-927`) und pinnt `config.webcam.frame_provider =
  &webcam_service_` (`:796`). **Relevanz für M-9:** Solange `is_recording_` erst spät im Worker
  gesetzt wird, sieht der GUI-Thread während des queued `Preparing`-Callbacks `is_recording_ ==
  false`; `syncWebcamPreviewCapture` fällt aus `idle` und ruft `SetWebcamPreviewActive(false)` →
  `SyncWebcamService` → `webcam_service_.Stop()` — **cross-thread gegen den Worker-`Start()`**
  (Race, s. Design/Schritt 2).
- **Config-Modelle sind während Preparing GUI-seitig beschreibbar.** `SetOutputSettings`
  (`:1874-1919`, mit `ReconcileOutputFormat`), `SetVideoSettings` (`:1921-1929`),
  `SetWebcamSettings` (`:559`), `SetOutputTargetContext` (`:1930-1933`) mutieren `output_settings_`
  / `split_settings_` / `resolved_user_config_` / `video_settings_` / `webcam_settings_` /
  `output_target_context_`. Der heutige `StartRecording`-Rumpf liest **alle** davon live
  (`:614`, `:726-812`, `:748-759`, `:793-812`, `:916-927`, `:682-686`) plus `caps_`
  (`:726`, `:772`). `RecordPage::isSourceSelectionLocked` (`:4527-4532`) sperrt nur die
  **Quellenauswahl**, nicht die Settings-Seiten — ein globaler Aufnahme-Hotkey kann einen Start
  auslösen, während der Nutzer auf der Settings-Seite an einem Regler zieht. Diese Structs tragen
  `wstring`/`path`-Member → nicht-atomar. **Relevanz für M-9:** sobald der Rumpf off-thread läuft,
  sind das Torn-Reads (s. Schritt 1/2).
- Preview-Release-Hook (`app/pages/RecordPage.cpp:2542-2545`): `RequestEngineLease()` — postet
  ein Kommando an den Hub-Worker-Thread und wartet auf einer Condition-Variable
  (`DxgiCaptureHubService.cpp:97-114`). **Thread-agnostisch**: der Aufruf berührt selbst keine
  GUI-only-Objekte, sondern blockiert nur den aufrufenden Thread; die Lambda liest jedoch
  RecordPage-Member (`hub_preview_active_`, `dxgi_capture_hub_`).

### UI-State-Maschine
- `enum class UiRecordingState { … Countdown, Preparing, RegionSelecting, Recording, … }` —
  `Preparing` existiert (`app/viewmodels/RecordViewModel.h:30`).
- `RecordViewModel::SetState` setzt `state_text = L"Preparing..."` für Preparing
  (`RecordViewModel.cpp:360-361`); der `switch` hat kein `default` und behandelt `Saving`/
  `ArmedFromRecovery` nicht (Preparing/Countdown/Stopping sind abgedeckt).
- `CanStart()` erlaubt Start nur aus `Ready | Completed | Failed` — Preparing ist ausgeschlossen
  (`RecordViewModel.cpp:290-305`). `CanStop()` nur `Recording | Paused`
  (`:307-309`). Es gibt **keinen** `CanCancel`/Preparing-Abbruch-Prädikat.
- `onHotkeyToggle` (`RecordPage.cpp:3102-3111`): Countdown→cancel, `CanStart`→start,
  `CanStop`→stop. Während Preparing trifft **kein** Zweig → Hotkey ist No-op.
- RecordPage behandelt Preparing bereits als „Transition/Busy": Meter-Gates
  (`:3914-3916`, `:3942-3944`, `:3963`), `starting`-Flag (`:4187-4188`), Busy-Prädikat
  (`:4528-4529`). Transport-Dock hat **keinen** eigenen Preparing-Zweig
  (`updateTransportDock`, `:4231-4290`) → Dock bleibt im Ready-Layout mit
  `primary_enabled = CanStart() && …` = false.
- Countdown-Ende ruft `doStartRecording` (`finishCountdown`, `RecordPage.cpp:2967-2991`).

### Produkt-/Doku-Kontext
- `docs/product-spec.md:431-437` „Recording lifecycle" beschreibt Pre-flight-Gate und
  Live-Monitoring, **keinen** Preparing-Zwischenzustand.
- Default-Countdown 0 s (`docs/product-spec.md:88`).
- Kein ADR zum Coordinator-Threading; ADR-0040/0041 regeln Preview-Source-Tap bzw.
  Capture-Hub-Lease (Kontext für den Release-Handshake).

## Design

### Kernentscheidung
Den gesamten `StartRecording`-Rumpf **nach** einem billigen, GUI-Thread-lokalen
Re-Entrancy-/Zulässigkeits-Gate auf den Aufnahme-Thread verlagern („thin gate, fat worker").
Der `Preparing`-State wird damit erstmals real sichtbar: Der GUI-Thread postet `Preparing`,
kehrt sofort zur Event-Loop zurück (repaint zeigt „Preparing…"), und ein Worker-Thread erledigt
Disk-/FS-Check, DXGI-Facts, Config-Bau, Validate, Webcam-Start, Manifest, Release-Handshake und
geht bei Erfolg **direkt** in `session_.Record()` über (kein zweiter Thread-Hop).

### Alternativen ehrlich abgewogen

**A — Thin gate, fat worker (GEWÄHLT).**
GUI-Thread synchron nur: Re-Entrancy-Guard (atomar), Startbar-State-Check, `has_caps_`-Check,
**vollständiger Snapshot aller Eingaben und Config-Modelle** in ein bewegbares `PrepareContext`,
`PostStateChange(Preparing)`, Worker-Thread starten. Alles andere off-thread; Fehler/Abbruch posten
`Failed`/`Ready` + Result asynchron (wie heute über die queued Callbacks).

**Wichtig (aus dem adversarialen Review):** Der Snapshot ist **nicht** auf `target` /
`audio_ui_state` / `crop_region` beschränkt. Der Worker-Rumpf liest heute live: `output_settings_`,
`split_settings_`, `video_settings_`, `webcam_settings_`, `resolved_user_config_`,
`output_target_context_` (+`has_output_target_context_`) und `caps_` — allesamt während Preparing
GUI-seitig beschreibbar (s. Ist-Zustand). `PrepareContext` kopiert deshalb **alle** diese Modelle
by-value auf dem GUI-Thread (billige Kopien); der Worker liest ausschließlich aus `ctx`, niemals
aus einem mutablen Coordinator-Member — außer `caps_` für die DXGI-Facts-Refresh, das durch die
bestehende `RevalidateCapabilities`-Early-Return-Garantie während Preparing geschützt bleibt
(Schritt 2). `FillResultFormat` in den Worker-Fehlerpfaden liest ebenfalls aus dem Snapshot
(Container/Codec-Felder aus `ctx`), nicht aus `resolved_user_config_`/`caps_`.
- *Pro:* Beseitigt **jeden** unbeschränkten GUI-Stall (auch FS/Disk auf langsamen Volumes und den
  ≤750 ms-Lease-Wait). Vereinheitlicht Prepare+Record in einem Thread. Die frühen Fehlerpfade
  gehen ohnehin schon durch queued Callbacks → UI-Verhalten identisch, nur einen Frame später.
- *Contra/Risiko:* Mehr Shared-State über die Thread-Grenze (`current_output_path_`,
  `caps_.runtime.displays`, `current_manifest_id_`). Muss diszipliniert abgesichert werden
  (s. Risiken). Der bool-Rückgabewert von `StartRecording` verliert seine „Fehlerdetail"-Bedeutung
  (Caller ignoriert ihn ohnehin, `RecordPage.cpp:3848`).

**B — Nur die schweren Geräteschritte off-thread, billige Validierungen synchron lassen.**
GUI behält Disk-Check, Folder-Validate, Pfad-Resolve, mkdir, Config-Bau, `Validate`; nur
DXGI-Facts + Webcam-Start + Release-Hook wandern in den Worker.
- *Pro:* Kleinerer Diff, weniger Shared-State.
- *Contra:* Löst das Problem **nur teilweise** — Pfad-Resolve/`create_directories`/Disk-Query
  können auf Netz-/Wechsellaufwerken sekundenlang stallen und blieben auf dem GUI-Thread. Zudem
  hängt die HDR10-Native-Ableitung (Config) an den DXGI-Facts; man müsste den Config-Bau über die
  Thread-Grenze zerreißen (erst Teil-Config auf GUI, Rest im Worker) → fragil und
  wartungsunfreundlich. Verworfen: behebt den Freeze nicht vollständig und ist unsauberer.

**C — `QtConcurrent`/`std::async`-Wrapper um den bestehenden Rumpf.**
Faktisch Option A mit anonymem Future statt benanntem `jthread`.
- *Contra:* Verliert die vorhandene, sauber besitzende `recording_thread_`-`jthread`-Semantik
  (Join im Destruktor), erschwert kooperative Cancellation und das direkte Übergehen in `Record()`.
  Verworfen zugunsten des expliziten, bereits vorhandenen Thread-Modells.

**Verworfene Nebenidee — Release-Hook per `BlockingQueuedConnection` auf den GUI-Thread
marshallen:** Da `RequestEngineLease` genau der bis-zu-750 ms blockierende Schritt ist, würde ein
blockierendes Marshalling den GUI-Thread erneut für bis zu 750 ms einfrieren — der Kern des
Problems. Der Hook läuft daher im Worker (er ist thread-agnostisch, s. Ist-Zustand); die von der
Lambda gelesenen RecordPage-Member werden beim Setzen des Hooks als Wert gesnapshottet
(s. Schritt 4).

### Webcam-Geräte-Ownership über die Prepare-Grenze (Race-Fix)
Der Off-thread-Umbau öffnet — vom Design selbst verursacht — ein Cross-Thread-Race auf
`webcam_service_`: Der queued `Preparing`-Callback läuft auf dem GUI-Thread, während der Worker
noch `is_recording_ == false` hält; `syncWebcamPreviewCapture` fällt aus `idle` und ruft über
`SyncWebcamService` `webcam_service_.Stop()` — parallel zu `webcam_service_.Start(...)` im Worker
(`:916-927`). Je nach Interleaving: Cross-Thread-Stop/Start **oder** eine Aufnahme, deren
Webcam-PiP-Reader tot ist (`config.webcam.frame_provider = &webcam_service_`). Dieselbe Klasse
trifft `SetWebcamSettings` (`force_restart` bei `recording==false`).

**Fix (Teil von Schritt 1/2):** Ein laufendes Prepare **besitzt das Kamera-Gerät** — genau wie
eine laufende Aufnahme. Konkret:
- `SyncWebcamService`: `want_running` um `|| prepare_in_flight_.load()` erweitern (verhindert den
  `Stop()` während Preparing).
- `SyncWebcamService` erhält oben einen Early-Guard `if (prepare_in_flight_.load()) return;`, damit
  der GUI-Thread während des Prepare-Fensters **weder** startet **noch** stoppt — der Worker
  (`:916-927`) ist in diesem Fenster der alleinige Zugriff auf `webcam_service_`. (Die
  Recording-Zeit-Semantik bleibt unverändert: für `is_recording_` gilt weiter der bestehende Pfad.)
- `SetWebcamSettings`: `recording`-Bedingung auf `is_recording_.load() || prepare_in_flight_.load()`
  ziehen, damit kein Mid-Prepare-`force_restart` das Gerät neu öffnet. Der Overlay-Live-Push
  (`UpdateWebcamOverlay`, an `is_recording_` gebunden) bleibt wie er ist — während Preparing läuft
  die Engine noch nicht, und die Overlay-Felder stehen bereits im `PrepareContext`-Snapshot.
- `webcam_service_` und `webcam_settings_` kommen damit ausdrücklich in die Schritt-2-Liste des
  über die Thread-Grenze geteilten Zustands.

### Cancellation-Modell (kooperativ, ehrlich begrenzt) — bewusst zugeschriebener Umfang
**Ehrlichkeitshinweis (adversarialer Review):** Der Review-Punkt M-9
(`.workspace/review-fable-2026-07-10.md:125`, `:206`) verlangt nur *Geräte-Open + Release-Handshake
off-thread in einen Preparing-State* und *keinen UI-Freeze-Beat* (Messung: GUI-Thread-Stall-Trace).
**Zwingend** ist damit nur: Fehler in Preparing → `Failed` (die sechs Worker-Fehlerpfade posten das
ohnehin) und ein sauberes Zurück nach `Ready`/`Failed`. Eine **nutzer-initiierte Abbruch-Maschinerie**
(`CancelPreparing`, Hotkey-Zweig, Dock-Cancel-Affordanz) ist **nicht** vom Review gefordert — sie ist
eine **hier bewusst getroffene Zusatzentscheidung**, begründet durch Default-Countdown 0 s (Preparing
ist die einzige Pre-Record-Rückmeldung) und dadurch, dass ein während eines mehrere-hundert-ms-
Webcam-Opens still verpuffender Aufnahme-Hotkey kaputt wirkt. Die Zusatzentscheidung ist **sauber
abtrennbar**: Wird sie descoped, bleibt der Hotkey während Preparing No-op (wie heute) und die
Schritte 5/6 (Cancel-Einstieg, Hotkey-Zweig, Dock-Affordanz) entfallen; der Kern-Slice (off-thread
Preparing, kein Freeze, Fehler→Failed, kein Doppelstart) steht ohne sie. Die beiden früheren
offenen Produktfragen sind unten **entschieden** (nicht offen gelassen), um den Slice schlank zu
halten.

Entscheidung für den (behaltenen) Abbruchpfad: **kooperative Cancellation an definierten Punkten** —
der Worker prüft `prepare_cancel_requested_` nach jedem größeren Schritt (nach Disk/FS, nach
DXGI-Facts, nach Webcam-Start, unmittelbar vor `Record()`). Ein bereits laufender blockierender
Einzelschritt (z. B. MF-Kamera-Open) wird **nicht** unterbrochen — der Abbruch greift nach dessen
Rückkehr. Das ist die ehrliche Grenze und wird so dokumentiert.

**Unwind bei Abbruch (korrigiert):** Der Worker stoppt eine von uns in diesem Prepare gestartete
Webcam, entfernt einen bereits geschriebenen Manifest-Eintrag, setzt `prepare_in_flight_ = false`
und postet **`Ready`** (kein `Failed`). **Die Lease gibt der Coordinator NICHT selbst zurück** — er
hat keinen Zugriff auf den Hub. `ReturnEngineLease` existiert ausschließlich page-seitig
(`RecordPage.cpp:2444`, `resumeHubPreviewIfHeld`) und wird vom State-Callback bei `Ready`/
`Completed`/`Failed` über `ShouldRevertPreviewFromPushedMode` (`RecordPage.cpp:2611-2623`)
automatisch getriggert. Zusätzlich gilt: Der Release-Hook (Lease-**Erwerb**,
`preview_capture_release_hook_`) ist im gemergten Worker der **letzte** Schritt unmittelbar vor
`Record()` (heute `:989`, nach allen Cancel-Checks) — ein abgebrochenes Preparing hält die Lease
also **nie**, es gibt nichts zurückzugeben. Der Cancel-Pfad braucht daher genau
`PostStateChange(Ready)`; der bestehende page-seitige Revert-Pfad ist für den nie-geleasten Fall
durch seine `hub_preview_active_`/Preview-aktiv-Guards (`RecordPage.cpp:2435-2444`) idempotent. **Kein
neues Coordinator→Hub-Rückgabe-Plumbing bauen** (Hub ist strictly refcounted, ADR 0041).

### Re-Entrancy (kein Doppelstart)
Neues `std::atomic<bool> prepare_in_flight_`. Der GUI-Thread akzeptiert einen Start nur, wenn
`prepare_in_flight_ == false` **und** `is_recording_ == false` (compare-exchange
`false→true` auf `prepare_in_flight_`). Da `StartRecording` ausschließlich vom GUI-Thread
gerufen wird, ist Check-then-set gegenüber anderen `StartRecording`-Aufrufen ohnehin
race-frei; das Atomic schützt die Sicht des Workers. `prepare_in_flight_` wird `false`, sobald
der Worker entweder scheitert/abbricht (→ Ready/Failed) **oder** `is_recording_` auf `true`
gesetzt hat (dann übernimmt die bestehende Recording-Guard). Der Startbar-State-Gate
(`Ready|Completed|Failed|ArmedFromRecovery`) bleibt als zusätzliche Prüfung erhalten.

### Statuspill / Transport
`Preparing` ist bereits als Warn-/Busy-Zustand verdrahtet (Preview-`recordState="warn"`,
Meter-Gates). Ergänzt wird: ein expliziter Transport-Dock-Zweig für Preparing (Ready-Layout,
Primary zeigt einen Busy-/Cancel-Affordanz statt „ausgegraut"), damit der State für die 0 s-
Countdown-Nutzer sichtbar ist. Timer-Text/Rolle: „Preparing…".

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz. Reihenfolge einzuhalten
(1–3 sind der Kern; 4–7 UI/Feinschliff; 8 Doku).

### Schritt 1 — Re-Entrancy-Guard + Prepare-Skelett (Coordinator)
**Dateien:** `app/services/RecordingCoordinator.{h,cpp}`
- Neue Member: `std::atomic<bool> prepare_in_flight_{false};`,
  `std::atomic<bool> prepare_cancel_requested_{false};`.
- Neues `struct PrepareContext` (Coordinator-privat), das **by-value** trägt: `target`,
  `audio_ui_state`, `crop_region`, **plus** `output_settings_`, `split_settings_`, `video_settings_`,
  `webcam_settings_`, `resolved_user_config_`, `output_target_context_`, `has_output_target_context_`
  und einen `caps_`-Snapshot (Letzterer für `ToRecorderCoreConfig`/`FillResultFormat`; die
  DXGI-Facts-Refresh im Worker arbeitet weiter auf dem Member `caps_`, geschützt über Schritt 2).
- `StartRecording` in zwei Teile schneiden:
  - **GUI-Teil (synchron, minimal):** `StopMicMeter()`; Guard
    `bool expected=false; if (is_recording_ || !prepare_in_flight_.compare_exchange_strong(expected,true)) return false;`
    danach Startbar-State-Gate (`state_`-Snapshot ∈ `Ready|Completed|Failed|ArmedFromRecovery`) und
    `has_caps_` — bei Verfehlen `prepare_in_flight_.store(false)` + `return false`.
    `prepare_cancel_requested_.store(false)`. **`PrepareContext ctx` befüllen (alle o. g. Modelle
    kopieren).** `PostStateChange(Preparing)`. `recording_thread_ = std::jthread([...]{
    PrepareAndRecordThreadProc(std::move(ctx)); });`. `return true;`.
  - **Worker-Teil:** neuer privater `void PrepareAndRecordThreadProc(PrepareContext ctx)`. Er enthält
    die heutige `StartRecording`-Rumpflogik ab `:619` (Disk-Block) und liest **ausschließlich aus
    `ctx`**, nicht aus mutablen Membern. **Präzision — folgende heute im Rumpf stehenden Zeilen
    dürfen NICHT in den Worker wandern**, weil der GUI-Teil sie bereits erledigt: der
    Re-Entrancy-Guard (`:612-613`), das Startbar-State-Gate + `has_caps_` (`:675-680`) **und** der
    `PostStateChange(Preparing)`-Post (`:724`) — Letzterer würde sonst ein **doppeltes**
    Preparing-Post erzeugen. `output_target_context_`-Ableitung (`:682-686`) wird bereits im
    GUI-Teil in den Snapshot gezogen. Am Ende führt der Worker `session_.Record(...)` + die heutige
    `RecordingThreadProc`-Nachbereitung aus (Schritt 3 führt beide zusammen).
- **Jeder der sechs Worker-Fehlerpfade** (`:646` Disk, `:664` Folder, `:694` Collision, `:715`
  CreateDir, `:861` TargetPid, `:880` Validate) muss vor `return` **`prepare_in_flight_.store(false)`**
  setzen (bisher stand das nur in der Design-Prosa zur Re-Entrancy, nicht in den Schritten). Der
  `PostStateChange(Failed)`/`PostResult`-Teil dieser Pfade bleibt unverändert; `FillResultFormat`
  liest die Format-Felder aus `ctx` statt aus `resolved_user_config_`/`caps_`.
- **Wichtig:** `current_output_path_` wird jetzt im Worker gesetzt → mit vorhandenem/kleinem
  Mutex absichern (s. Schritt 2) bzw. `EffectiveOutputFolder`/`CurrentOutputPath` entsprechend
  synchronisieren.
- **Optionaler Enabler (empfohlen, in Schritt 1):** `PostStateChange` (`:2163`),
  `PostStats` (`:2189`), `PostResult` (`:2181`) einen `QCoreApplication::instance()==nullptr`-
  Direktaufruf-Fallback geben — **denselben, den heute nur `PostDiagnostics` (`:2216-2219`) hat**
  (Korrektur eines Fakten-Irrtums der Vorfassung: `PostStats` besitzt ihn **nicht**). Macht die
  State-/Result-Tests robuster.
- **Bestehenden Test migrieren (Pflicht, sonst bricht CI):** `StartFailureFormatTest`
  (`app/tests/test_output_settings.cpp:1240-1244`) erwartet heute `EXPECT_FALSE(StartRecording(...))`
  **synchron** für den Folder-Guard-Fehler und liest das Result nach **einem** `processEvents()`
  (`:1242`). Neu kehrt der Aufruf mit **`true`** zurück und `Failed`/Result kommen zeitversetzt vom
  Worker → beide Annahmen brechen. Umbau: `EXPECT_TRUE(StartRecording(...))` und eine Wait-/
  Poll-Schleife (`processEvents` bis `failure.has_value()`, mit Timeout) statt des einzelnen
  `processEvents`.
- **Test (CI):** Coordinator-Test mit `QCoreApplication` + Event-Loop: ein Start, der über einen
  **erreichbaren** async-Fehlerpfad scheitert (Folder-as-File, s. Testplan Punkt 2 — **nicht**
  über eine `Validate`-invalide Config, die `SetOutputSettings`/`ReconcileOutputFormat` nie zu
  Validate durchlässt), belegt die Sequenz `Preparing` → `Failed` asynchron und dass der Aufruf
  sofort mit `true` zurückkehrt (Beweis: off-thread). Zweiter Start während `prepare_in_flight_`
  wird abgelehnt (Rückgabe `false`, kein zweiter Preparing-Post).

### Schritt 2 — Shared-State über die Thread-Grenze absichern
**Dateien:** `app/services/RecordingCoordinator.{h,cpp}`
- **Config-Modelle (`output_settings_`, `split_settings_`, `video_settings_`, `webcam_settings_`,
  `resolved_user_config_`, `output_target_context_`):** werden **nicht** vom Worker gelesen — der
  Worker liest nur den `PrepareContext`-Snapshot (Schritt 1). Damit sind die GUI-seitigen Setter
  (`SetOutputSettings`/`SetVideoSettings`/`SetWebcamSettings`/`SetOutputTargetContext`) während
  Preparing torn-read-frei gegenüber dem Worker. Kommentar an `PrepareContext` und an den Settern:
  „während einer laufenden Prepare/Recording-Session speist der Snapshot, nicht das Live-Member".
- **`webcam_service_` / `webcam_settings_`:** Cross-Thread-Zugriff über die Prepare-Grenze wird
  durch die Ownership-Regel aus dem Design-Abschnitt „Webcam-Geräte-Ownership" geschlossen:
  `SyncWebcamService` fasst `webcam_service_` bei `prepare_in_flight_` nicht an (Early-Guard) und
  `want_running` schließt `prepare_in_flight_` ein; `SetWebcamSettings` behandelt `prepare_in_flight_`
  wie `is_recording_`. Während des Prepare-Fensters ist der Worker (`:916-927`) der einzige
  Zugriff auf `webcam_service_`.
- `current_output_path_` mit einem `std::mutex output_path_mutex_` schützen (Setzen im Worker,
  Lesen in `CurrentOutputPath()`), oder alternativ als „vor Preparing noch leer, Anzeige hält
  vorherigen Wert" dokumentieren. Empfehlung: Mutex (deterministisch, kein Tearing von
  `std::filesystem::path`).
- `caps_.runtime.displays`-Mutation via `RefreshDisplayFacts()` läuft im Worker. Absichern durch
  die vorhandene Garantie, dass `RevalidateCapabilities` während `Preparing` early-returnt
  (`:535-539`), plus expliziten Kommentar/Assert, dass `OnCapabilitiesReady` während einer
  aktiven Session nicht erwartet wird (Caps sind Voraussetzung für den Start). Kein zweiter
  Reader auf dem GUI-Thread während Preparing.
- `current_manifest_id_` wird im Worker geschrieben; Zugriff bleibt Worker-lokal bis
  Recording — kein GUI-Reader in Preparing. Kommentar ergänzen.
- **Test (CI):** ThreadSanitizer/Helgrind ist auf Windows/MSVC nicht Teil der CI; stattdessen ein
  Review-Kommentar + gezielter Unit-Test, der `CurrentOutputPath()` unmittelbar nach einem
  akzeptierten Start liest und keinen Crash/kein leeres Tearing beobachtet (Smoke, kein
  Race-Beweis).

### Schritt 3 — Prepare und Record in einem Thread zusammenführen + Cancellation
**Dateien:** `app/services/RecordingCoordinator.{h,cpp}`
- `PrepareAndRecordThreadProc` führt nach erfolgreichem Prepare direkt in die bestehende
  `RecordingThreadProc`-Logik über (kein separater `jthread`). Bestehenden
  `RecordingThreadProc`-Body als Hilfsfunktion `RunRecordingAfterPrepare(config, output_path)`
  extrahieren und aus dem Prepare-Worker aufrufen.
- Cancellation-Checks (`prepare_cancel_requested_.load()`) einfügen nach: Disk/FS-Block,
  DXGI-Facts, Webcam-Start, unmittelbar vor `Record()`. Bei gesetztem Flag: sauberes Unwind —
  Webcam nur stoppen wenn von uns in diesem Prepare gestartet; Manifest-Eintrag entfernen wenn
  bereits `Add`; `prepare_in_flight_ = false`; `PostStateChange(Ready)`. **Keine** Coordinator-seitige
  Lease-Rückgabe: `ReturnEngineLease` ist page-seitig (`RecordPage.cpp:2444`) und wird durch den
  `Ready`-Post automatisch über `ShouldRevertPreviewFromPushedMode` getriggert; da der Release-Hook
  (Lease-Erwerb) der letzte Schritt vor `Record()` ist, hält ein Cancel die Lease ohnehin nie
  (s. Design „Unwind bei Abbruch"). Kein `Failed`. (Ob ein knapper Info-Result gepostet wird → offene
  Frage 2, unten entschieden: still + Log.)
- **Reihenfolge der Commit-Zeile:** `is_recording_ = true` erst **nach** dem letzten Cancel-Check und
  **vor** `Record()`; ab dann `prepare_in_flight_ = false` (Recording-Guard übernimmt).
- **Verlorener Cancel im Commit-Fenster (Fix):** Zwischen `is_recording_ = true` und dem Start von
  `Record()` ist `CancelPreparing` (Schritt 5) per Definition No-op (weil `is_recording_` true),
  und der Hotkey-Zweig könnte einen Druck genau hier still verpuffen lassen. Deshalb: unmittelbar
  nach `is_recording_ = true` **ein letztes Mal `prepare_cancel_requested_` prüfen**; ist es
  gesetzt, den regulären Stop-Pfad einleiten (die Aufnahme startet und wird sofort über die
  bestehende Stop-Maschinerie beendet), statt den Druck zu verwerfen. Dazu passt der Schritt-5-Fix,
  dass `CancelPreparing` bei bereits gesetztem `is_recording_` an `StopRecording` durchfällt.
- **Test (CI):** Cancel-Pfad — Start akzeptieren, sofort `prepare_cancel_requested_` über den
  neuen `CancelPreparing()`-Einstieg (Schritt 5) setzen; mit einer `Validate`-fähigen, aber
  Record-vermeidenden Konfiguration (siehe Testinfra-Notiz) die Sequenz `Preparing` → `Ready`
  belegen, ohne dass `Recording` gepostet wird. (Der reale Geräte-Open ist nicht CI-fähig; der
  Test nutzt einen Cancel *vor* dem Record-Übergang.)

### Schritt 4 — Release-Hook thread-sicher aufrufbar machen
**Dateien:** `app/pages/RecordPage.cpp` (Hook-Definition), `RecordingCoordinator` (Aufrufstelle)
- Der Hook wird jetzt aus dem Worker gerufen. Die Lambda darf keine GUI-veränderlichen Member
  live lesen. Umbau: Beim Setzen des Hooks die benötigten Werte (`dxgi_capture_hub_`-Pointer,
  `hub_preview_active_`-Zustand) so kapseln, dass der Aufruf nur `RequestEngineLease()` auf dem
  bereits thread-sicheren Hub auslöst. Da `hub_preview_active_` sich zwischen Setzen und Aufruf
  ändern könnte, den aktuellen Zustand über eine kleine thread-sichere Abfrage (atomare Flag oder
  Hub-interne Idempotenz) beziehen; `RequestEngineLease` ist bei nicht-aktivem Preview ein
  billiger No-op-Roundtrip.
- Kontrakt in `RecordingCoordinator.h:230-239` aktualisieren: „invoked on the recording preparation
  worker thread (not the UI thread), after every guard; must be thread-safe and must block until
  the duplication is released."
- **ADR 0041 im selben Zug angleichen:** dort steht „the coordinator fires a blocking release hook
  immediately before the recording thread starts" — jetzt feuert der Hook aus dem Prepare-Worker
  unmittelbar vor `session_.Record()` (kein separater Recording-Thread mehr, s. Schritt 3). Den
  Satz entsprechend nachziehen, damit Header-Kontrakt und ADR nicht auseinanderlaufen.
- **Test (User-live):** nur live verifizierbar (echter DXGI-Hub + Preview). CI deckt nur ab, dass
  ohne gesetzten Hook nichts passiert.

### Schritt 5 — ViewModel-Prädikate + Cancel-Einstieg
**Dateien:** `app/viewmodels/RecordViewModel.{h,cpp}`, `app/services/RecordingCoordinator.{h,cpp}`
- `RecordViewModel`: neues Prädikat `bool CanCancelPreparing() const noexcept { return state ==
  UiRecordingState::Preparing; }`. `CanStart()`/`CanStop()` bleiben (Preparing korrekt
  ausgeschlossen). `SetState`-`switch` um ein `default:` ergänzen (defensiv; heute fehlen
  `Saving`/`ArmedFromRecovery`), Preparing-Text bleibt „Preparing…".
- Coordinator: `void CancelPreparing()` — setzt `prepare_cancel_requested_ = true`, wenn
  `prepare_in_flight_ && !is_recording_`. **Fällt bei bereits gesetztem `is_recording_` an
  `StopRecording()` durch** (statt still No-op zu sein), damit ein Cancel-Druck im schmalen
  Commit-Fenster nicht verloren geht (s. Schritt 3). `StopRecording` bleibt sonst für Recording
  zuständig.
- **Test (CI):** ViewModel-Prädikat-Tests (Preparing → `!CanStart`, `!CanStop`,
  `CanCancelPreparing`); `SetState`-Text-Test.

### Schritt 6 — Hotkey-Verhalten während Preparing
**Dateien:** `app/pages/RecordPage.cpp`
- `onHotkeyToggle` (`:3102-3111`) um einen Zweig ergänzen: bei `view_model_.CanCancelPreparing()`
  → `coordinator_->CancelPreparing()` (analog zum Countdown-Cancel). Reihenfolge:
  Countdown-Cancel → Preparing-Cancel → Start → Stop.
- **Test (CI):** falls eine reine Resolver-Funktion für die Hotkey-Aktion extrahiert wird
  (empfohlen: `HotkeyAction Resolve(state, countdownActive, interactionMode)` als pure function),
  Tabellen-Test aller States → Aktion. Sonst Widget-Test mit QApplication-Fixture.

### Schritt 7 — Transport-Dock/Statuspill-Zweig für Preparing
**Dateien:** `app/pages/RecordPage.cpp` (`updateTransportDock`, `refresh`/`updateChrome`)
- `updateTransportDock`: expliziter Preparing-Zweig — Ready-Layout, `primary_enabled=false`
  (**keine** dedizierte Cancel-Darstellung, s. Produktentscheidung 1; Abbruch nur per Hotkey),
  Timer-Text „Preparing…", Timer-Rolle „preparing"/„warn". Split disabled.
- Sicherstellen, dass `starting`/Warn-Tönung (`:4187-4211`) für Preparing greift (bereits der Fall).
- **Test (CI):** Visual-Test-Szenario „Record / Preparing" in `app/visual_tests/VisualScenario.cpp`
  (neuer `VisualRecordState::Preparing` analog zu Countdown), gerendert per `--visual-test`. Belegt
  Pill-Text + Dock-Layout auf echten Pixeln.

### Schritt 8 — Spec/Doku
**Dateien:** `docs/product-spec.md` (Abschnitt 8 „Recording lifecycle")
- Einen Satz ergänzen: Zwischen Auslösen (bzw. Countdown-Ende) und laufender Aufnahme zeigt die
  App kurz einen **Preparing**-Zustand, während Geräte-Setup (Display-Facts, Webcam, Capture-Lease)
  off-thread läuft; die Oberfläche bleibt responsiv; ein Abbruch führt zurück nach Ready.
- Kein neuer ADR nötig (kein Format-/Kompatibilitätsentscheid; reine Threading-/State-Verfeinerung).
  Falls das Team eine Nachverfolgung wünscht: kurzer Verweis im Coordinator-Header genügt.

## Test-/Verify-Plan

### CI-fähig (deterministisch, ohne GPU/Kamera)
1. **ViewModel-Prädikate/Text** (Schritt 5): Preparing schließt Start/Stop aus, erlaubt Cancel;
   `SetState`-Text.
2. **Async-Off-thread-Beweis über Fehlerpfad** (Schritt 1): Mit `QCoreApplication` + Event-Loop
   einen Start über einen **erreichbaren** async-Fehlerpfad auslösen — **Folder-as-File**
   (`ValidateOutputFolder`, Vehikel wie `test_output_settings.cpp:1219-1226`: Output-Ordner auf eine
   existierende Datei zeigen lassen). **Nicht** „WebM+AAC/`Validate`-invalid": `SetOutputSettings`
   löst über `ReconcileOutputFormat` (`RecordingCoordinator.cpp:1881-1888`) jede Container/Codec-
   Kombination auf, bevor sie `session_.Validate` je erreicht — dieser Pfad ist nicht konstruierbar.
   Alternativ erreichbar: Stub-Disk-Provider (Punkt 3) oder Filename-Collision. Belegen, dass
   `StartRecording` sofort `true` (akzeptiert) zurückgibt und die States `Preparing` dann `Failed`
   **über die Event-Loop** eintreffen (nicht synchron; Poll-/Wait-Schleife, kein einzelnes
   `processEvents`). Nachweis, dass der GUI-Thread nicht in der Gerätearbeit hing. (Deckt zugleich
   die Migration von `StartFailureFormatTest` aus Schritt 1 ab.)
3. **Disk-Block off-thread** (Schritt 1/3): Injizierter `StubDiskSpaceProvider` (Muster:
   `app/tests/test_low_disk_guard.cpp:27`) mit Freispeicher ≤ Hard-Stop → asynchrones `Failed`
   mit `error_phase=DiskSpace`.
4. **Re-Entrancy** (Schritt 1): Zweiter `StartRecording` während `prepare_in_flight_` → `false`,
   genau ein Preparing-Post.
5. **Cancel vor Record** (Schritt 3/5): `CancelPreparing()` unmittelbar nach Accept → Sequenz endet
   in `Ready`, kein `Recording`, kein `Failed`; Unwind (kein Manifest-Leak, Webcam gestoppt).
6. **Hotkey-Resolver** (Schritt 6, falls als pure function extrahiert): Tabellen-Test.
7. **Visual-Test** (Schritt 7): `--visual-test`-Szenario „Record / Preparing" rendert Pill+Dock.

**Testinfra-Notiz:** Ein CI-Test, der bis kurz vor `session_.Record()` läuft, ohne echten
Geräte-Open, braucht einen Cancel-vor-Record-Punkt (Schritt 3) **oder** eine früh scheiternde
`Validate`-Config. `session_` ist ein konkreter `RecorderSession` und heute nicht mockbar; ein
voller Prepare→Record→Stop-Durchlauf ist deshalb **nicht** CI-fähig. Falls später ein
Session-Seam gewünscht ist, wäre das ein separater Slice — für M-9 bewusst nicht gebaut.

### Nur User-live verifizierbar (kein CI)
- **Kein GUI-Freeze mehr am Record-Klick** bei Countdown=0 mit aktiver Webcam und gehaltenem
  DXGI-Preview-Lease: Fenster bleibt responsiv, „Preparing…" ist sichtbar, danach „Recording".
  Messung wie im Review vorgeschlagen: **GUI-Thread-Stall-Trace** (`.workspace/review-…:206`) —
  kein Stall-Beat über einige ms hinaus auf dem GUI-Thread während der Vorbereitung.
- **Webcam-MF-Open** (mehrere hundert ms) blockiert die UI nicht mehr.
- **Release-Handshake**: Übergabe der Duplication von Idle-Preview an Engine funktioniert weiter
  (Preview friert korrekt ein / kehrt zurück), jetzt vom Worker-Thread aus.
- **Abbruch live**: Hotkey/Primary während Preparing kehrt sichtbar nach Ready zurück; die
  Preview nimmt ihren Idle-Feed wieder auf; keine verwaiste Duplication/kein Manifest-Rest.
- Regressionsblick auf die Startup-/Recovery-Pfade (ArmedFromRecovery → Start).

## Risiken
- **Shared-State-Races** (`current_output_path_`, `caps_.runtime.displays`,
  `current_manifest_id_`). Mitigation: Mutex bzw. die vorhandene Preparing-Gate-Garantie in
  `RevalidateCapabilities`; explizite Kommentare. MSVC-CI hat keinen TSan → Absicherung per Review.
- **Abbruch-Latenz**: Ein laufender blockierender Einzelschritt (MF-Open, Lease-Wait ≤750 ms) wird
  nicht unterbrochen; der Abbruch greift danach. Bewusst so und dokumentiert.
- **Stop im schmalen Fenster** zwischen `is_recording_=true` und `Record()`: existiert heute schon
  identisch (GUI setzte `is_recording_` vor Thread-Launch); `session_.Stop()` muss vor
  vollständigem `Record()`-Anlauf sicher sein (bestehende Garantie, hier nur beibehalten).
- **`PostStateChange` ohne Null-App-Fallback** (`:2163`): CI-Tests brauchen zwingend eine
  `QCoreApplication` + Event-Loop, sonst gehen State-Posts verloren. Optional (kleiner Enabler):
  den `instance()==nullptr`-Direktaufruf-Fallback ergänzen — den hat heute **nur `PostDiagnostics`**
  (`:2216-2219`), **nicht** `PostStats`/`PostResult`/`PostStateChange` (Fakten-Korrektur gegenüber
  der Vorfassung). Als Teil von Schritt 1 empfohlen; macht State-/Result-Tests robuster.
- **Webcam-Cross-Thread-Race** (Design-selbstverursacht): der queued `Preparing`-Callback kann
  `webcam_service_.Stop()` gegen den Worker-`Start()` feuern. Mitigation: Prepare-besitzt-Gerät-
  Regel (Design-Abschnitt + Schritt 2). Ohne diesen Fix ist der Slice **nicht** korrekt.
- **Config-Torn-Reads während Preparing**: Settings-Setter sind während Preparing nicht gesperrt;
  ein Hotkey-Start kann parallel zu einer laufenden Settings-Bearbeitung feuern. Mitigation:
  vollständiger `PrepareContext`-Snapshot (Schritt 1/2). Ohne den Snapshot UB auf `wstring`/`path`.
- **Release-Hook-Kontraktwechsel** (jetzt Worker-Thread): jede andere Hook-Implementierung muss
  thread-sicher sein; im Repo gibt es nur die eine (`RecordPage.cpp:2542`).
- **Doppelte Preparing-Semantik nicht verstecken**: `Preparing` bleibt rein Ablauf-/UI-State;
  keine Container-/Codec-Policy in den Coordinator ziehen (Resolver bleibt Owner) — Config-Bau
  ruft weiter `ReconcileOutputFormat`/`ToRecorderCoreConfig` unverändert, nur auf anderem Thread.

## Produktentscheidungen (vormals offen — hier entschieden, um den Slice schlank zu halten)
Beide Punkte gehören zur bewusst zugeschriebenen User-Cancel-Zusatzentscheidung (s. Design,
„Cancellation-Modell"). Statt sie offen zu lassen, sind sie entschieden — passt zur MEMORY-Leitlinie
„entscheidungsfreudig" und „Diagnostics ruhig, nicht alarmist":

1. **Abbruch-Affordanz während Preparing → ENTSCHIEDEN: nur Hotkey, kein dedizierter Button.**
   Der Primary-Button bleibt während Preparing busy/ausgegraut (Ready-Layout, `primary_enabled=false`);
   abgebrochen wird ausschließlich über den Aufnahme-Hotkey (Schritt 6). Kein neues
   „Abbrechen"-Control im TransportDock (hält Schritt 7 minimal). Falls später eine sichtbare
   Cancel-Affordanz gewünscht ist, ist das ein eigener kleiner Follow-up.
2. **Abbruch-Meldung → ENTSCHIEDEN: still + Log-Eintrag, kein Toast.** Rücksprung nach Ready ohne
   User-facing Meldung; ein `AppLog::info(record, "start cancelled")` genügt. Entspricht der
   Leitlinie „ruhig, nicht alarmist".

Falls die gesamte User-Cancel-Zusatzentscheidung descoped wird, entfallen beide Punkte und der
Hotkey bleibt während Preparing No-op (wie heute).

## Adversarialer Review — Ergebnis
Jeder Einwand wurde read-only gegen den Code geprüft, bevor er über- oder zurückgewiesen wurde.
Alle neun sind belegt und eingearbeitet (kein Einwand zurückgewiesen):

1. **[blocker] Webcam-Service-Race** — **eingearbeitet.** Verifiziert: queued `Preparing`-Callback →
   `syncWebcamPreviewCapture` (`RecordPage.cpp:1699-1711`, `refresh()`→`:4226`) sieht `is_recording_
   ==false` → `SetWebcamPreviewActive(false)` → `SyncWebcamService` → `webcam_service_.Stop()`
   (`:593`) cross-thread gegen Worker-`Start()` (`:916-927`). Fix: neuer Design-Abschnitt
   „Webcam-Geräte-Ownership" — `prepare_in_flight_` in `want_running` + Early-Guard in
   `SyncWebcamService` + `SetWebcamSettings`; `webcam_service_`/`webcam_settings_` in Schritt-2-Liste.
2. **[major] PrepareContext-Snapshot unvollständig** — **eingearbeitet.** Verifiziert: Worker liest
   `output_settings_`/`split_settings_`/`video_settings_`/`webcam_settings_`/`resolved_user_config_`/
   `output_target_context_`/`caps_` live; Setter (`:1874`,`:1921`,`:559`,`:1930`) während Preparing
   ungesperrt (`isSourceSelectionLocked` `:4527-4532` sperrt nur Quellen). Fix: `PrepareContext`
   kopiert **alle** Config-Modelle by-value (Design A + Schritt 1/2); `FillResultFormat` liest aus
   dem Snapshot.
3. **[major] Cancel-Unwind nicht implementierbar** — **eingearbeitet.** Verifiziert: Coordinator hat
   keinen Hub-Zugriff; `ReturnEngineLease` nur page-seitig (`RecordPage.cpp:2444`), getriggert über
   `ShouldRevertPreviewFromPushedMode` (`:2611-2623`). Fix: Schritt 3 + Design-„Unwind bei Abbruch":
   Cancel postet nur `Ready`; kein Coordinator-Lease-Return; ergänzt, dass der Release-Hook der
   letzte Schritt ist → Cancel hält die Lease ohnehin nie.
4. **[minor] Test-2-Vehikel unkonstruierbar** — **eingearbeitet.** Verifiziert:
   `SetOutputSettings`/`ReconcileOutputFormat` (`:1881-1888`) lösen WebM+AAC vor `Validate` auf. Fix:
   Testplan Punkt 2 auf Folder-as-File umgeschrieben (Stub-Disk/Collision als Alternativen genannt).
5. **[minor] Bestehender Test bricht** — **eingearbeitet.** Verifiziert: `StartFailureFormatTest`
   (`test_output_settings.cpp:1240-1242`) erwartet synchron `false` + ein `processEvents`. Fix: als
   expliziter Migrationspunkt in Schritt 1 (Return `true` + Poll-Schleife).
6. **[minor] Faktenfehler PostStats-Fallback** — **eingearbeitet.** Verifiziert: nur
   `PostDiagnostics` (`:2216-2219`) hat den `instance()==nullptr`-Fallback; `PostStats` (`:2189`),
   `PostResult` (`:2181`), `PostStateChange` (`:2163`) nicht. Ist-Zustand, Schritt-1-Enabler und
   Risiken korrigiert.
7. **[minor] Verlorener Cancel im Commit-Fenster** — **eingearbeitet.** Verifiziert: `CanStop` bis
   zum queued Recording-State false (`RecordViewModel.cpp:307-309`); `CancelPreparing` No-op ab
   `is_recording_`. Fix: Schritt 3 (letzter Cancel-Check nach `is_recording_=true`) + Schritt 5
   (`CancelPreparing` fällt an `StopRecording` durch).
8. **[minor] Schritt-1-Präzision + ADR 0041** — **eingearbeitet.** Verifiziert: Rumpfbereich enthält
   Guard, State-Gate/`has_caps_` (`:675-680`) und `PostStateChange(Preparing)` (`:724`) → Doppelpost-
   Risiko; sechs Fehlerpfade (`:646/:664/:694/:715/:861/:880`) müssen `prepare_in_flight_` zurücksetzen.
   Fix: Schritt 1 präzisiert (diese Zeilen bleiben im GUI-Teil; jeder Fehlerpfad resettet); Schritt 4
   zieht die ADR-0041-Formulierung nach.

**Ergänzend [Scope, Einwand 9]:** Verifiziert gegen `review-fable-2026-07-10.md:125`/`:206` — M-9
verlangt nur Off-thread-Geräte-Open + kein Freeze; nutzer-initiierter Abbruch ist **nicht**
gefordert. **Eingearbeitet** als bewusst gekennzeichnete, sauber abtrennbare Zusatzentscheidung
(Design „Cancellation-Modell"); die zwei vormals offenen Produktfragen sind entschieden
(Hotkey-only, still+Log), nicht offen gelassen.

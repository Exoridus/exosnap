# Geräte-Hot-Swap mid-recording: kohärente Policy pro Gerätetyp

> **SHIPPED (PR #199, #200, #206, 2026-07-12).** Verifiziert 2026-07-23: Audio-Endpoint-Verlust
> degradiert nur noch die betroffene Quelle statt die ganze Aufnahme zu beenden (#199), Monitor-Targets
> überleben Unplug/Replug (#200), stehende Benachrichtigung bei stummgeschalteter Quelle (#206,
> "Closes the standing-notification gap deferred from the device-loss policy work"). Nichts hier
> ist mehr offen.

## Problem

ExoSnap behandelt den Verlust eines Capture-Geräts *während* einer Aufnahme heute
**pro Subsystem unterschiedlich und nirgends als eine bewusste, dokumentierte
Produktentscheidung**. Die Einzelbausteine existieren und funktionieren, aber sie
sind über die Zeit gewachsen, nicht gegeneinander abgewogen worden:

- **Monitor-Verlust** (DXGI Output Duplication) → hält den letzten Frame eingefroren
  und öffnet die Duplication unbegrenzt neu (graceful).
- **Fenster-Schluss** (Windows Graphics Capture) → beendet die *gesamte* Aufnahme
  sauber (Segment bleibt gültig).
- **Audio-Endpoint-Verlust** (SYS-Loopback / MIC / APP-Process-Loopback) → beendet
  die *gesamte* Aufnahme (inkl. Video!) mit einem Fehler. Ein abgezogenes USB-Mikro
  zerstört damit eine einstündige Bildschirmaufnahme.
- **Webcam-Verlust** (Media Foundation PiP-Overlay) → hält den letzten Frame
  eingefroren und öffnet den Reader unbegrenzt neu (graceful).

Die **Asymmetrie ist das eigentliche Problem**: Der Video-Primärpfad degradiert
würdevoll (halten/reopen bzw. sauber beenden), aber jeder Audio-Endpoint-Verlust
ist *session-global fatal*. Das widerspricht dem Produktmodell „jede aktivierte
Quelle ist ihr eigener Track" (ADR 0018) und dem Ehrlichkeits-Prinzip, das der
Video-Pfad bereits lebt. `KNOWN_LIMITATIONS.md:250-252` und `product-spec.md:418-421`
sagen pauschal „Hot-Swap wird nicht unterstützt: stoppen und neu starten" — das ist
für den Monitor-/Webcam-Pfad **schlicht falsch** (die halten durch) und für Audio
**härter als nötig**.

Diese Spec friert **eine kohärente, pro Gerätetyp explizit begründete Policy** ein,
korrigiert die eine echte Inkohärenz (Audio zu fatal), zieht die Doku gerade und
grenzt scharf ab, was **bewusst nicht** gebaut wird (kein Mid-Recording-Retarget auf
ein anderes Gerät).

## Ist-Zustand (frisch aus dem Code, main @ #192)

### Video — Monitor (DXGI Output Duplication)

- **Klassifikation** `ClassifyOdAcquireFailure(HRESULT)` — pure Funktion,
  `dxgi_od_capture_src.cpp:494-509`:
  - `S_OK` / `DXGI_ERROR_WAIT_TIMEOUT` → `Idle` (kein Frame diesen Tick — normal).
  - `DXGI_ERROR_ACCESS_LOST` → `Recover` (Duplication-Handle stale, Device lebt).
  - `DXGI_ERROR_DEVICE_REMOVED` / `_HUNG` / `_RESET` / **alles andere** → `Fail`
    (fail-closed, Aufnahme sauber beenden).
- **Reopen** `DxgiOdCaptureSrc::Reopen()` — `dxgi_od_capture_src.cpp:345-366`: wirft
  die stale Duplication weg und re-resolved den Output über den **stabilen GDI-Namen**
  (`m_device_name`, in `Open()` gesetzt) gegen eine **frische DXGI-Factory** —
  überlebt also einen Hot-Plug, der mit neuem `HMONITOR` zurückkommt (`ResolveOutputForDevice`,
  `:178-231`, gefiltert auf die Adapter-LUID des Devices).
- **Retry-Policy** `DecideOdReopen(reopened, elapsed, budget, poll_delay)` — pure
  Funktion, `dxgi_od_capture_src.cpp:511-534`: Erfolg gewinnt immer (`Continue`),
  sonst `RetryAfter` (bei Budget geklemmt) bzw. `GiveUp` bei erschöpftem Budget.
  **Achtung (korrigiert nach Review):** Diese pure Funktion wird in **Produktionscode
  nirgends aufgerufen** — sie existiert nur als Definition (`:511`) und in
  `test_od_reopen_policy.cpp`. Der Video-Thread implementiert den Reopen-Throttle
  **inline** (`video_thread.cpp:2280-2288` Phase-Correct-, `:2782-2790` CFR-Pfad — nur
  ein `kOdReopenPollDelay`-Vergleich, **kein** Budget). Das *Verhalten* ist unbegrenzt;
  die frühere Behauptung „der Drain fährt `DecideOdReopen` mit `std::nullopt`-Budget"
  war falsch. **Lehre für Schritt 1/3:** Der neue Audio-Loss-Resolver muss wirklich
  **vom Thread gefahren** werden, sonst ist er (wie `DecideOdReopen`) ein test-only Pin,
  der vom echten Verhalten divergieren kann.
- **Verdrahtung im Video-Thread** `video_thread.cpp:1340-1389`: `odHolding`-State,
  `HandleOdAcquireFailure` (Recover → `odHolding=true`; Fail → `RecordFailure`),
  `kOdReopenPollDelay = 250 ms` (`:1357`). Während `odHolding` emittiert die
  Encode-Schleife weiter im **CFR-Takt mit dem letzten gehaltenen Frame** (nicht
  schwarz), und versucht `Reopen()` gedrosselt (`:2280-2288` Phase-Correct-Pfad,
  `:2783-2790` CFR-Pfad). `NextCaptureDrainStep(use_od, od_holding)`
  (`dxgi_od_capture_src.h:275-284`) stellt sicher, dass „halten" **nichts** drained
  (Crash-Regression: sonst würde der null WGC-FramePool angefasst).
- **Ergebnis:** Monitor-Mode-Change / kurzer Unplug → nahtlos weiter, letzter Frame
  eingefroren, unbegrenzt bis Rückkehr oder User-Stop. GPU-Entfernung (`DEVICE_REMOVED`)
  → `RecordFailure(VideoCapture)` → Segment finalisiert, Aufnahme endet.

### Video — Fenster (Windows Graphics Capture)

- `GraphicsCaptureItem.Closed` registriert einen Callback, der `sourceLost = true`
  setzt (`video_thread.cpp:1420`); Deregistrierung bei Teardown (`:3111`).
- Der Drain prüft `sourceLost` (`:2269-2274`): setzt `stats.source_loss = true`,
  `stop_requested = true`, bricht ab → **gesamte Aufnahme endet sauber**, Segment
  finalisiert. Es gibt **keinen Reopen-Pfad** für WGC.
- Wichtig: Ein bloß **minimiertes / verdecktes** Fenster feuert `Closed` *nicht* —
  WGC liefert weiter (letzten Frame). Nur echtes Schließen/Zerstören des `HWND` löst
  aus.
- `source_loss` wird live als „· SOURCE LOST" im Diagnostics-Live-Panel angezeigt
  (`LivePipelinePanel.cpp:309`).

### Audio — Endpoint (SYS-Loopback / MIC / APP-Process-Loopback)

**SYS ist zwei verschiedene Engine-Sources, nicht eine** (korrigiert nach Review):
- **Bei Fenster-Target** ist `AudioSourceKind::Sys` eine
  `WasapiProcessLoopbackSrc(ExcludeProcessTree)` mit **PID-Identität**
  (`recorder_session.cpp:583-586`; `wasapi_process_loopback_src.cpp:205-208, 236`
  `TargetProcessId = pid_`) — „alles außer dem Ziel-Prozessbaum".
- **Nur ohne Fenster** schreibt `NormalizeSourceRowsForTarget` (`audio_track_model.cpp:17-22`)
  `Sys → SystemOutput` um; `SystemOutput` = `WasapiLoopbackSrc` = **Default-Render-Endpoint**
  (`recorder_session.cpp:587-588`).
- **APP** ist `WasapiProcessLoopbackSrc(IncludeProcessTree)`, ebenfalls **PID-Identität**
  (`recorder_session.cpp:579-582`).

Es gibt damit **drei Loopback-Flavors** mit unterschiedlicher Re-Resolvbarkeit:
`SystemOutput`/Default-Render (Konzept-Identität), `Sys`-exclude-PID und `APP`-include-PID.
Eine PID ist nach Prozess-Ende **nicht** re-resolvbar, und PID-Reuse könnte einen
**fremden** Prozess greifen (siehe Design, Reinit-Regeln).

- **Gemeinsame Klassifikation** `ClassifyWasapiAcquireFailure(HRESULT)` — pure
  Funktion, `wasapi_capture_src.cpp:229-239`: `AUDCLNT_S_BUFFER_EMPTY` / benigne
  Erfolgs-Codes → `Idle`; `AUDCLNT_E_DEVICE_INVALIDATED`,
  `AUDCLNT_E_SERVICE_NOT_RUNNING`, **alles andere** → `Fail`.
- **SYS-Loopback (SystemOutput-Flavor)** `wasapi_loopback.cpp`: `ClassifyLoopbackAcquire`
  (`:21-42`) spiegelt obige Policy und reicht HRESULT + Text über
  `LastFatalError{Hresult,Message}` hoch. `GetNextPacketSize` gibt bei fatalem Fehler
  bewusst `1` zurück (`:162-180`), um den Caller in `GetNextPacket` zu zwingen, das
  `false` liefert (`:186-209`) — der Fix aus **#180** (M-16), der den vorher **stillen
  Tod** des SYS-Tracks im **Standalone-Pfad** beseitigt hat (siehe aber MixedAudioSrc-Loch
  unten — für gemergte Tracks lebt der stille Tod weiter).
- **MIC** bindet optional eine feste `device_id_` (`wasapi_capture_src.cpp:243-244,
  288-298`): mit ID → `GetDevice(id)`, ohne → `GetDefaultAudioEndpoint(eCapture,
  eConsole)`. **Wichtig (korrigiert nach Review):** Der Shipped-Default ist
  `selected_mic_device_id = std::nullopt` (`app/models/RecordingPreset.cpp:126`), d. h.
  der **Default-Mic** bindet zur Recording-Startzeit `GetDefaultAudioEndpoint`. Das
  Source-Objekt kann gezielt neu geöffnet werden — aber ein `Reinit` „mit derselben
  Identität" greift beim Default-Mic das **dann-aktuelle** Default-Capture-Gerät (siehe
  Design, MIC-Identitätsregel).

**Verdrahtung im Audio-Thread — und ihre Löcher** (korrigiert nach Review):
- `audio_thread.cpp:358-365`: Ein `AcquireBuffer`-Fehler ruft `RecordFailure` **nur bei
  nicht-leerem `captureErr`** (`:359` `if (!captureErr.empty())`); ein Fehler mit leerem
  Text bricht **nur die innere Schleife** ab, ohne die Session zu beenden.
- **Pause-Pfad** `audio_thread.cpp:335-341`: ein `AcquireBuffer`-Fehler wird
  **grundsätzlich ohne `RecordFailure` geschluckt** (nur `break`).
- **MixedAudioSrc-Loch (der zentrale Ist-Zustands-Fehler):** In einem `MixedAudioSrc`
  wird ein fehlgeschlagenes **inneres** `AcquireBuffer` **still verschluckt**
  (`mixed_audio_src.cpp:137-139` — `continue` ohne Fehler-Propagation), und
  `EmittableFrames` überspringt leere FIFOs (`:110-124`). `MixedAudioSrc::AcquireBuffer`
  gibt **immer `true`** zurück (`:197-257`). Ein toter innerer Source beendet damit
  **nicht** die Session — er **verschwindet lautlos** (genau der von #180 im Standalone-Pfad
  behobene „stille Tod", der für gemergte Sources **weiterlebt**). `MicDspAudioSrc` ist
  dagegen transparent: es reicht `AcquireBuffer==false` (`mic_dsp_audio_src.cpp:91`) und
  `LastCaptureHresult` (`:157-158`) durch.
- **Konsequenz:** „Audio-Endpoint-Verlust ist session-global fatal" gilt **nur für
  Tracks mit direkt propagierendem Source** — d. h. **unwrapped Single-Source-Tracks**
  (`recorder_session.cpp:631-633`, gain==1) und **MicDsp-gewrappte Single-Mic-Tracks**.
  Sobald ein Track in `MixedAudioSrc` gewrappt ist — **jeder gemergte Track *und* jeder
  Single-Source-Track mit gain≠1** (`recorder_session.cpp:623-633, 634-662`) — ist der
  Verlust eines inneren Sources **schon heute nicht fatal, sondern still**.
- **`RecordFailure`** `session_internal.h:428-443` ist **session-global**: es setzt
  `stop_requested`, weckt alle CVs — d. h. wo es *greift*, beendet der Verlust *eines*
  Audio-Endpoints **Video und alle anderen Tracks** mit. Das Segment wird noch
  finalisiert (Footage bleibt), aber die Aufnahme ist vorbei.
- **Begründungskommentar im Bestand** (`test_wasapi_acquire_failure_classify.cpp:8-11`):
  „there is no in-place recover … a WASAPI capture endpoint … cannot be reacquired on
  the same stream, so endpoint loss ends the recording cleanly." Das ist für den
  *Stream* korrekt — aber ein *neues* `WasapiCaptureSrc`/`WasapiLoopback` lässt sich
  re-`Init()`-en (genau wie OD eine frische Duplication und die Webcam einen frischen
  MF-Reader baut). Die Aussage rechtfertigt „Stream tot", nicht „Session tot".

### Webcam (Media Foundation, PiP-Overlay)

- `WebcamService` Reader-Loop `WebcamService.cpp:481-528`: **unbegrenzter** Reopen
  (`kReconnectDelay = 500 ms`); `ClassifyWebcamReadResult(hr, flags, hasSample)` →
  `Reconnect` / `Skip` / `Deliver` (`:518`). Der gespeicherte Frame wird **nie**
  gelöscht (`has_frame_` nur durch `Stop()` zurückgesetzt, `:497` Kommentar).
- Der Video-Compositor pollt den Webcam-`frame_provider->TryGetFrame(...)`
  (`video_thread.cpp:1108, 1131-1163`); ist die Kamera weg, wird der **letzte
  eingefrorene Frame** weiter einkomponiert. Aufnahme läuft normal weiter.
- `product-spec.md:382-383` dokumentiert das bereits: „During a capture-loss recovery
  the picture is held frozen instead, until the capture source is reopened."

### Live-Geräte-Signale (heute nur Idle-UI)

`AudioDeviceNotifier` / `DisplayDeviceNotifier` / `WebcamDeviceNotifier`
(`app/services/*Notifier.*`) emittieren debounced `snapshotChanged(snapshot, reason)`
mit `DiscoveryReason::DeviceRemoved` u. a. Verbraucht wird das ausschließlich zur
**Aktualisierung der Idle-Geräteauswahl** (`MainWindow.cpp:947-952, 3469-3495` →
`onAudio/Displays/WebcamDevicesChanged` reichen an Config-/Record-/Webcam-Page durch).
**Es gibt keine Kopplung dieser Notifier an eine laufende Aufnahme** — der Engine-Pfad
erkennt Verlust rein reaktiv über die Acquire-HRESULTs oben.

### Recovery-Interaktion

Alle vier Pfade beenden über den **normalen Finalize-Pfad** (kein Absturz): das aktive
Segment wird ordentlich geschlossen und ist gültig; die Recovery-Manifest-Maschinerie
(ADR 0015) legt keinen „true-crash"-Kandidaten an. Erfolgreicher Abschluss → „Saved"-
Toast (`MainWindow.cpp:3585-3606`); eine Nicht-Disk-Failure → Modal
`RecordingErrorOverlay` statt Toast (`:3615-3620`).

## Design

### Leitprinzip: Ehrlichkeit vor Magie, Symmetrie über Subsysteme

Zwei Achsen entscheiden pro Gerätetyp:

1. **Hat das verlorene Gerät eine stabile, re-resolvbare Identität?** Monitor: ja
   (GDI-Name). Fenster: **nein** — ein geschlossenes `HWND` ist endgültig weg; ein
   anderes Fenster zu greifen wäre Raten. **Audio ist hier nicht uniform** (korrigiert
   nach Review) — es hängt am Flavor:
   - **MIC mit expliziter `device_id`:** ja, stabile Endpoint-ID.
   - **MIC = Default (`nullopt`, Shipped-Default):** nur *als Konzept* („das aktuelle
     Default-Capture-Gerät"). Nach einem Unplug designiert Windows typischerweise ein
     **anderes physisches Mikro** als Default — Re-Resolve „mit derselben Identität"
     folgt also dem Default, nicht dem alten Gerät (siehe MIC-Identitätsregel unten).
   - **SystemOutput / SYS-ohne-Fenster (Default-Render):** ja, *als Konzept*
     („das aktuelle Default-Render-Endpoint" = Systemton) — Re-Resolve folgt dem Default.
   - **SYS-exclude-PID (Fenster-Target) und APP-include-PID:** **PID-Identität, nach
     Prozess-Ende NICHT re-resolvbar.** PID-Reuse könnte einen fremden Prozess greifen —
     also *keine* verlässliche Konzept-Identität. Ein toter Ziel-Prozess ist für diese
     Flavors so endgültig wie ein geschlossenes `HWND` (siehe Reinit-Regeln unten).
2. **Ist das Gerät Primärquelle oder akzessorischer Track/Overlay?** Monitor/Fenster:
   Primär (ohne Bild keine sinnvolle Aufnahme). Audio-Track & Webcam-PiP: akzessorisch
   (Verlust ⇒ ehrliche Lücke, nicht Totalverlust).

**Ehrlichkeit zur „Symmetrie über Subsysteme" (nach Review):** Die vier Pfade leben
**nicht in derselben Schicht**. OD/WGC und der neue Audio-Pfad sind **Engine-Code**
(`libs/engine`, pure Resolver + Threads). Die **Webcam-Reconnect-Policy dagegen
lebt in der App-Schicht** (`app/services/WebcamService.cpp:481-528`, Test in
`app/tests/test_webcam_read_policy.cpp` — **nicht** in `libs/engine/tests`). Die
Symmetrie ist also eine *Verhaltens*-Symmetrie (alle degradieren würdevoll), keine
Code-Symmetrie. Die neue Audio-Policy wird bewusst Engine-Code (UI-agnostisch); die
Webcam bleibt, wo sie ist. Das ist offengelegt, damit „Symmetrie" nicht als „gleicher
Code-Ort" fehlgelesen wird.

Daraus die kohärente Zielpolicy:

| Gerätetyp | Verlust-Signal | **Zielpolicy** | Status |
|---|---|---|---|
| Monitor (OD) | `ACCESS_LOST` | **Halten (letzter Frame eingefroren) + unbegrenzt Reopen** (gleiche Identität via GDI-Name) | vorhanden — KEEP |
| Monitor/GPU (OD) | `DEVICE_REMOVED`/`_HUNG`/`_RESET` | **Sauber beenden** (fail-closed, Segment gültig) | vorhanden — KEEP |
| Fenster (WGC) | `Item.Closed` | **Sauber beenden** (kein Reopen — Ziel endgültig weg) | vorhanden — KEEP + testbar machen |
| Audio-Source **MIC (feste `device_id`)** | `DEVICE_INVALIDATED` etc. | **Beitrag stummschalten + weiterlaufen**; dieselbe `device_id` unbegrenzt reaktivieren | **ÄNDERN** (heute je nach Wrap session-fatal *oder* still) |
| Audio-Source **MIC (Default, `nullopt`)** | `DEVICE_INVALIDATED` etc. | **Beitrag stumm + weiterlaufen**; Reaktivierung folgt dem *aktuellen* Default-Capture-Gerät (Konzept-Identität, ehrlich dokumentiert) — siehe Entscheidung unten | **ÄNDERN** |
| Audio-Source **SystemOutput / SYS-ohne-Fenster (Default-Render)** | `DEVICE_INVALIDATED` etc. | **Beitrag stumm + weiterlaufen**; Reaktivierung folgt dem *aktuellen* Default-Render-Endpoint (= Systemton) | **ÄNDERN** |
| Audio-Source **SYS-exclude-PID / APP-include-PID** | `DEVICE_INVALIDATED` / Ziel-Prozess tot | **Beitrag stumm + weiterlaufen**; reaktivieren **nur solange dieselbe PID lebt** (PID-Reuse-Schutz via Prozess-Startzeit); toter Ziel-Prozess ⇒ **dauerhaft stummer Beitrag**, kein Fremd-Prozess-Greifen | **ÄNDERN** |
| Webcam (MF) | Reader tot | **Halten (Frame eingefroren) + unbegrenzt Reopen** (gleiche Device-ID) | vorhanden — KEEP |

**Source-granular, nicht track-granular** (korrigiert nach Review): Die Policy gilt pro
**Source**, nicht pro Track. In einem gemergten Track (z. B. SYS+MIC) darf ein toter MIC
**nur seinen Beitrag** verstummen lassen, **nicht den ganzen Track** — die anderen
inneren Sources mixen weiter. Das ist genau der Punkt, an dem `MixedAudioSrc` heute
falsch liegt (stiller Verschluck statt bewusster, sichtbarer Beitrags-Stille).

Der einzige Verhaltens-Change ist der Audio-Pfad. Alles andere ist Konsolidierung +
Doku + Testabdeckung.

### Audio-Identität & Reinit-Regeln pro Flavor (neu nach Review)

Weil „dieselbe Identität reaktivieren" pro Flavor etwas anderes bedeutet, wird es hier
explizit gepinnt — die vier Fälle dürfen im Resolver/Reinit **nicht** kollabiert werden:

- **MIC mit fester `device_id_`:** `Reinit` re-öffnet exakt diese ID. Kommt das Gerät
  nie zurück, bleibt der Beitrag dauerhaft stumm (kein Substitut).
- **MIC = Default (`nullopt`) — Entscheidung:** Der Default-Mic behält seine
  **Konzept-Identität** („das aktuelle Default-Capture-Gerät"), **symmetrisch zu SYS/
  Default-Render**. `Reinit` ruft erneut `GetDefaultAudioEndpoint(eCapture, eConsole)`
  und greift damit bewusst das *dann* aktuelle Default-Mikro. Begründung: Der User hat
  „das System-Standardmikrofon" gewählt, nicht ein bestimmtes Gerät — dem Default zu
  folgen ist keine Substitution, sondern die unveränderte Semantik der Quelle. Das ist
  **ehrlich zu dokumentieren** (Notification-/Doku-Text: „folgt dem System-Standardmikrofon").
  *Verworfene Alternative:* die aufgelöste Endpoint-ID bei Recording-Start pinnen — würde
  den Default-Mic von SYS/Default-Render abkoppeln und wäre für den Nutzer weniger
  erwartbar (er hat gerade *nicht* ein Gerät fixiert). Beide Optionen sind vertretbar;
  diese Spec entscheidet für Konzept-Identität und macht sie sichtbar.
- **SystemOutput / SYS-ohne-Fenster:** Konzept-Identität Default-Render; `Reinit` folgt
  dem aktuellen Default-Render-Endpoint (Systemton).
- **SYS-exclude-PID / APP-include-PID:** `Reinit` re-aktiviert die
  Process-Loopback-Session **nur, wenn die gemerkte PID noch denselben Prozess bezeichnet**.
  Da Windows PIDs recycelt, wird die **Prozess-Startzeit** (o. ä. stabile Kennung) beim
  ersten `Init` gemerkt und bei `Reinit` geprüft; passt sie nicht (oder ist der Prozess
  weg), wird **nicht** reaktiviert — der Beitrag bleibt dauerhaft stumm, statt einen
  fremden Prozess zu greifen. Das schließt die „stille Quellenlüge" für diese Flavors aus.

### Alternativen für den Audio-Pfad (die einzige echte Entscheidung)

**A — Status quo: Audio-Endpoint-Verlust beendet die ganze Session (fail-closed).**
- *Pro:* Simpel, bereits implementiert; der User „verliert nie unbemerkt Audio", weil
  die Aufnahme stoppt und ein Modal erscheint. Der Bestandskommentar rechtfertigt es
  mit „Stream nicht reacquire-bar".
- *Contra:* **Disproportional.** Ein gebumptes USB-Mikro oder ein
  Bluetooth-Kopfhörer-Umschalten zerstört eine lange, ansonsten fehlerfreie
  Bildschirmaufnahme. Das widerspricht direkt dem Video-Pfad (der hält/reopened) und
  dem Track-Modell (ADR 0018: jede Quelle ihr eigener Track). Ein Track-Ausfall darf
  die anderen Tracks + das Video nicht mitreißen. „Stream nicht reacquire-bar" belegt
  nur, dass der *alte* Stream tot ist — nicht, dass die Session sterben muss.

**B — Track stummschalten + weiterlaufen; denselben Endpoint optional reaktivieren
(gewählt).**
- Bei `Fail`-Klassifikation eines Audio-**Sources**: **nicht** `RecordFailure`, sondern
  den betroffenen **Source-Beitrag** auf **ehrliche Stille** setzen und die Aufnahme
  fortsetzen (bei gemergten Tracks nur diesen Beitrag, nicht den Track — siehe
  source-granular oben). Parallel gedrosselt versuchen, ein **frisches Source-Objekt mit
  derselben Identität** neu zu `Init()`-en — **pro Flavor nach den Reinit-Regeln oben**:
  MIC-fest → `device_id_`; MIC-Default & SystemOutput → aktuelles Windows-Default
  (Capture/Render, Konzept-Identität); SYS-exclude/APP → PID **nur bei Startzeit-Match**.
  Zur **encoder-taktgleichen Stille** siehe Schritt 3 / Risiken: der `feedGapSilence`-Pfad
  reicht dafür **nicht** (er misst nur paket-getriebene Gaps), es braucht eine eigene
  encoder-frame-basierte Zeitrechnung. Gelingt der Reinit, läuft der Beitrag live weiter;
  die Stille-Lücke bleibt im File sichtbar und ehrlich.
- *Pro:* Symmetrisch zum Video-/Webcam-Pfad; rettet die Aufnahme; ehrlich (die Lücke
  ist echte Stille, in Diagnostics + Notification sichtbar, nicht kaschiert); nutzt
  bereits vorhandene Bausteine (per-Track-Mute ADR 0018, Gap-Silence, pure Classifier).
- *Contra:* Mehr Zustand im Audio-Thread; die Entscheidung „stumm statt stopp" muss
  von echten Encode-Fehlern (die weiter fatal sind) sauber getrennt werden; ein
  Sonderfall „alle Audio-Tracks tot" braucht eine Regel (siehe unten).

**C — Audio wie Video mit Budget-Give-up.** Halten (= Stille) + Reopen, aber nach
Budget doch die Session beenden.
- *Contra:* Der Video-Pfad hat sich bewusst für **unbegrenzt** entschieden (der
  Drain fährt `std::nullopt`). Ein Audio-Budget, das die ganze Session killt, brächte
  die Asymmetrie zurück. Verworfen — wenn Stille ehrlich ist, ist sie es auch nach
  10 Minuten.

**Entscheidung: B.** Sie stellt Kohärenz her (jeder akzessorische Track/Overlay
degradiert würdevoll; nur der Verlust *aller* Primär-Videoquelle bzw. eine
GPU-Entfernung beendet die Session), bleibt ehrlich (sichtbare Stille-Lücke +
Diagnostics-Karte + stehende Notification), und vermeidet Magie (kein
Auto-Umschalten auf ein *anderes* Mikro/Endpoint).

**Sonderfall „alle aktivierten Audio-Tracks tot":** Die Aufnahme läuft **video-only**
weiter (das Bild ist die Primärquelle). Wenn die Aufnahme **rein Audio** ist (kein
Videopfad — existiert das als Modus? heute nicht, Aufnahme ist immer video-getrieben),
wäre der Verlust der einzigen Quelle wie WGC-`Closed` zu behandeln (sauber beenden).
Da ExoSnap keine audio-only-Aufnahme kennt, greift dieser Zweig faktisch nie; er wird
als defensive Regel im Resolver gepinnt, nicht als UI-Feature.

### Was bewusst NICHT gebaut wird

- **Kein Mid-Recording-Retarget auf ein anderes Display.** Ein `DEVICE_REMOVED` oder
  ein für immer abgeschaltetes Display wird **nicht** durch Umschalten auf einen
  anderen Monitor „gerettet". Der OD-Pfad reopened ausschließlich **dieselbe**
  Display-Identität (GDI-Name). Kommt sie nie zurück, hält er (bei `ACCESS_LOST`) oder
  beendet sauber (bei `DEVICE_REMOVED`).
- **Kein Retarget eines geschlossenen Fensters** auf ein anderes Fenster/Instanz. Ein
  geschlossenes `HWND` ist endgültig; die Aufnahme endet.
- **Kein Auto-Switch eines Audio-Sources auf ein *frei gewähltes anderes* Gerät.** Ein
  MIC mit **fester `device_id`** bleibt an dieser ID; ein „irgendein nächstbestes Mikro
  nehmen" wäre eine stille Quellenlüge. **Sonderfall (nach Review geschärft):** Sowohl der
  **Default-MIC (`nullopt`)** als auch **SystemOutput/Default-Render (SYS)** folgen
  bewusst dem *aktuellen* Windows-Default (Capture bzw. Render) — das ist kein Substitut,
  sondern die unveränderte Semantik der Quelle („System-Standardmikrofon" / „Systemton").
  Der Unterschied zum verbotenen Auto-Switch: Der User hat hier gerade *kein* konkretes
  Gerät fixiert. Dass ein Default-Mic-Reinit nach Unplug ein anderes physisches Mikro
  greifen kann, ist gewollt und wird ehrlich benannt — es ist kein „falsches Gerät".
- **Kein Greifen eines fremden Prozesses bei PID-Reuse.** Für die PID-Flavors
  (SYS-exclude, APP) wird nach totem Ziel-Prozess **nicht** blind reaktiviert; ohne
  Startzeit-Match bleibt der Beitrag stumm.
- **Keine Kopplung der `*DeviceNotifier` an die laufende Engine** als Steuerpfad. Die
  Notifier bleiben Idle-UI. Verlust-Erkennung im Recording bleibt HRESULT-getrieben
  im Engine-Thread (eine Quelle der Wahrheit, keine Race zwischen UI-Snapshot und
  Engine-Stream).
- **Kein „follow the window to another monitor" für die HDR-Entscheidung.** Bleibt bei
  „einmal bei Start aufgelöst" (`product-spec.md:331-333`).

### Sichtbarkeit (Diagnostics ruhig, nicht alarmistisch)

- Ein **gehaltener/degradierter** Zustand (OD-Hold, Webcam-Frozen, Audio-Track-stumm)
  ist ein *echter, gemessener* Zustand → er darf/soll angezeigt werden, aber ruhig:
  eine Live-Karte im Diagnostics-Panel (analog „· SOURCE LOST", `LivePipelinePanel.cpp:309`),
  **ein** Hinweis pro Zustand, kein Blinken.
- Eine **stehende** (nicht auto-verschwindende) Notification pro degradiertem Track/
  Overlay, die bei Rückkehr des Geräts aufgelöst wird (Muster: `UnexpectedStop` ist
  stehend, `NotificationManager.h:44`). Ehrlicher Text, z. B. „Mikrofon getrennt —
  dieser Track ist stumm, bis das Gerät zurückkehrt".
- Der post-flight „Saved"-Report weist die Stille-/Hold-Lücken als Fakten aus (Dauer,
  Track), damit der User nicht überrascht wird.

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz. Reihenfolge so, dass
die reinen Resolver (CI-grün, GPU-frei) vor der Thread-Integration landen.

### Schritt 1 — Pure Policy-Resolver für Audio-Geräteverlust (CI)
**Dateien:** `libs/engine/src/wasapi_capture_src.cpp/.h` (oder neue
`audio_device_loss_policy.{h,cpp}`), `libs/engine/tests/`.
- Neue pure Funktion analog `DecideOdReopen`, z. B.
  `AudioLossDecision DecideAudioDeviceLoss(bool reactivated, milliseconds elapsed,
  milliseconds poll_delay)` → `{MuteAndReactivate, RetryAfter}` (unbegrenzt, kein
  Give-up-Budget — Entscheidung C verworfen).
- Zusätzlich einen Klassifikator, der `Fail` (heute) in **`DegradeSource`** (nicht nur
  „Track" — source-granular, s. o.) statt `KillSession` übersetzt, plus die Trennung
  „recoverable device loss" vs. „echter Encode-Fehler bleibt fatal".
- **Wirklich vom Thread gefahren:** Anders als `DecideOdReopen` (test-only, nie in Prod
  aufgerufen — s. Ist-Zustand) muss dieser Resolver den echten Audio-Thread-Pfad
  steuern; sonst pinnt der Test eine Policy, die im Betrieb niemand fährt. Ein leichter
  Integrations-Pin (Schritt 3) sichert die Verdrahtung.
- **Test (CI):** neue Unit-Pins (Muster `test_od_reopen_policy.cpp`,
  `test_wasapi_acquire_failure_classify.cpp`): DeviceInvalidated → DegradeSource;
  Reaktivierung gelingt → live; Encode-Fehler → weiter fatal.

### Schritt 2 — Reaktivierbares Audio-Source-Interface (CI-nah)
**Dateien:** `wasapi_capture_src.*`, `wasapi_loopback.*`, `wasapi_process_loopback_src.*`,
**`mixed_audio_src.*`**, **`mic_dsp_audio_src.*`** (Decorator-Kette),
`recorder_session.cpp:557-662` (Komposition — als Referenz, i. d. R. keine Änderung),
gemeinsames `IAudioCaptureSource`-Interface.
- `Reinit(std::string& err)` auf jedem Audio-Source: schließt sauber und `Init()`-et
  ein frisches Handle **mit derselben Identität — pro Flavor unterschiedlich** (siehe
  „Audio-Identität & Reinit-Regeln pro Flavor"):
  - **MIC-fest:** gespeicherte `device_id_`.
  - **MIC-Default (`nullopt`) & SystemOutput:** re-resolven das *aktuelle* Windows-Default
    (Capture bzw. Render) — Konzept-Identität, keine gepinnte ID.
  - **SYS-exclude/APP (PID):** re-aktivieren die Process-Loopback-Session **nur bei
    Startzeit-Match** der gemerkten PID; sonst `Reinit` schlägt bewusst fehl
    (dauerhaft stummer Beitrag, kein Fremd-Prozess). Dazu Prozess-Startzeit (o. ä.) beim
    ersten `Init` merken.
- **Decorator-Propagation (Blocker):** `Reinit` muss durch die Kette reichen —
  `MicDspAudioSrc::Reinit` delegiert an `inner_` (analog zu seinem transparenten
  `AcquireBuffer`/`LastCaptureHresult`-Durchreichen, `mic_dsp_audio_src.cpp:91,157`).
  `MixedAudioSrc` bekommt **per-inner-Source-Reinit** und muss dabei sein zentrales Loch
  schließen (siehe Schritt 3): der innere `AcquireBuffer==false`/`LastFatalError` darf
  nicht länger still verschluckt werden.
- **Test (CI):** Konstruktions-/State-Tests ohne echte Hardware (Reinit setzt
  `LastFatalError` zurück, behält die Flavor-Identität — feste `device_id_` bzw. den
  Default-Charakter bzw. die gemerkte PID+Startzeit); die *echte* Reaktivierung ist
  live-only (siehe Verify-Plan).

### Schritt 3 — Audio-Thread + MixedAudioSrc: stummschalten + weiterlaufen statt RecordFailure
**Dateien:** `libs/engine/src/audio_thread.cpp:335-341` (Pause-Pfad),
`:355-365` (Haupt-Acquire), `:372-389` (Timing/Silence/Drift),
**`mixed_audio_src.cpp:110-124, 126-179, 197-257`** (inneres Loch).
- **Alle drei Verlust-Pfade explizit behandeln** (korrigiert nach Review), sonst bleibt
  der „stille Tod" bestehen:
  1. **Haupt-Acquire (`:358-365`):** heute nur bei nicht-leerem `captureErr` `RecordFailure`
     — künftig `DecideAudioDeviceLoss`/Klassifikator entscheiden, und **auch der
     leer-Text-Fall** muss zwischen „recoverable device loss → DegradeSource" und „echter
     Fehler → fatal" sauber getrennt werden (kein stilles `break`).
  2. **Pause-Pfad (`:335-341`):** ein `AcquireBuffer`-Fehler wird hier heute
     grundsätzlich geschluckt — im degradierten Modell muss auch er den Source als
     `degraded` markieren (statt lautlos abzubrechen).
  3. **MixedAudioSrc-Loch (`:137-139` `continue`):** der innere `AcquireBuffer`-Fehler
     muss **sichtbar** werden — `MixedAudioSrc` braucht per-inner-Source-`degraded`-Status
     + `LastFatalError`-Weiterreichung, sodass der Audio-Thread den **einzelnen Beitrag**
     verstummen und reaktivieren kann, ohne den Track (oder die Session) zu beenden.
- Bei `DegradeSource`: **kein** `RecordFailure`. Stattdessen den Source in einen
  `degraded`-Zustand versetzen, encoder-taktgleiche Stille speisen und gedrosselt
  (`kReconnectDelay` analog Webcam) `Reinit` versuchen.
- **Eigene Zeitrechnung für die Ausfall-Lücke (Risiko-Fix, s. u.):** `feedGapSilence` ist
  **nicht** wiederverwendbar — es wird von `raw.gap_frames` getrieben, das
  `ComputeDiscontinuityGapFrames` aus den **Device-Positionen des nächsten Pakets**
  berechnet (`discontinuity_gap.h:29-41`) und auf 10 s klemmt
  (`kMaxDiscontinuityGapSeconds`). Während des Ausfalls kommen **keine Pakete** (also keine
  `gap_frames`), und nach `Reinit` startet der neue Stream mit **frischer
  Device-Position-Basis** — die Lücke (typisch > 10 s bei Unplug) ist so **weder messbar
  noch überbrückbar**. Der degradierte Zustand braucht darum eine **eigene
  encoder-frame-basierte Zeitrechnung** (Wall-Clock/QPC gegen `encoderAccumulatedFrames`),
  die die fehlenden Silence-Frames selbst berechnet und speist, plus eine definierte
  **Re-Synchronisation bei `Reinit`** inkl. **Reset des Clock-Drift-Estimators**
  (`audio_thread.cpp:372-377` füttert `drift_estimator` mit Device-Timing, das nach
  `Reinit` bei null beginnt und sonst Garbage-Drift-Diagnostik erzeugt).
- Echte Encode-Fehler (Mux, Encoder-Init, null-bytes-für-non-silent `:390-393`) bleiben
  `RecordFailure` — die Trennung ist explizit an der Fehlerquelle, nicht am HRESULT
  allein.
- Diagnostics-Hook: `m_state.diagnostics.OnAudioSourceDegraded(track_id, source_index, reason)`
  + Wiederherstellung, feed in `SessionStats`.
- **Test (CI):** Zwei `audio_thread`-Integrationstests mit Fake-Sources, die nach N Frames
  `DEVICE_INVALIDATED` liefern und nach M weiteren wieder liefern —
  **(a) Single-Source-Track** und **(b) gemergter Track (SYS+MIC, Blocker-Lücke):** die
  Session bleibt am Leben, das Segment enthält eine Stille-Lücke **exakter Länge**
  (eigene Zeitrechnung), bei (b) mixt der überlebende innere Source weiter,
  `stop_requested` bleibt `false`, und nach `Reinit` ist der Drift-Estimator zurückgesetzt.

### Schritt 4 — WGC-Source-Loss als Integrations-Pin am Drain (nicht als pure Tautologie)
**Dateien:** `video_thread.cpp:1420` (Closed-Callback), `:2269-2274` (Drain-Branch),
`libs/engine/tests/`.
- **Korrigiert nach Review:** Der ursprünglich vorgeschlagene
  `DecideWgcSourceLost(bool item_closed) → EndCleanly` ist eine **konstante Funktion ohne
  Verzweigung** — ein Unit-Test darauf testet nichts und fängt die reale Regression
  (jemand baut später einen Reopen-Zweig in den Drain) **nicht**, denn die läge in
  `video_thread.cpp:2269-2274`, nicht im Resolver. Das wäre speculative Overengineering.
- **Stattdessen:** Die Policy ist bereits Verhalten + „· SOURCE LOST"-Sichtbarkeit
  (`LivePipelinePanel.cpp:309`). Wir sichern sie mit einem **Integrations-Pin am Drain**
  ab: `sourceLost ⇒ stats.source_loss == true && stop_requested == true`, und es gibt
  **keinen** Reopen-Pfad für WGC. (Alternativ ganz streichen — der Drain-Branch ist
  selbsterklärend; der Pin lohnt nur als Regressionsschutz gegen ein späteres
  „retarget window".)
- **Test (CI):** Ein Test, der den `sourceLost`-Pfad am Drain treibt und `source_loss` +
  `stop_requested` prüft — **keine** pure Konstant-Funktion.

### Schritt 5 — Sichtbarkeit: Diagnostics-Karte + stehende Notification
**Dateien:** `app/ui/widgets/LivePipelinePanel.cpp`, `app/notifications/*`,
`app/MainWindow.cpp:3578-3622`, `libs/engine/.../pipeline_diagnostics.h`.
- Live-Karte je degradiertem Track/Overlay (Audio-Track stumm, Webcam frozen,
  OD-Hold) — ruhig, ein Hinweis, kein Alarmton.
- Stehende Notification bei Eintritt, Auflösung bei Rückkehr (neuer
  `NotificationType::DeviceDegraded` oder Wiederverwendung eines caution-Typs).
- Post-flight-Report: Lücken als Fakten (Track, Dauer).
- **Test (CI):** `test_notification_manager`-Erweiterung (stehend, dedupe, Auflösung);
  Live-Panel-Rendering via `--visual-test`-Harness (nicht die laufende App bedienen).

### Schritt 6 — Doku-Kanon geradeziehen
**Dateien:** `docs/product-spec.md:337-427` (§7), `KNOWN_LIMITATIONS.md:250-252`,
ggf. neuer ADR `docs/decisions/00XX-device-loss-policy.md`.
- §7 und KNOWN_LIMITATIONS: die pauschale „Hot-Swap nicht unterstützt"-Aussage durch
  die **differenzierte Policy-Tabelle** ersetzen (Monitor hält/reopened; Fenster-Schluss
  beendet; Audio-Track stumm+reaktiviert; Webcam frozen+reopened; **kein**
  Retarget-auf-anderes-Gerät).
- Neuer ADR dokumentiert die Entscheidung B + die vier „bewusst nicht"-Abgrenzungen und
  referenziert ADR 0013 (OD), 0041 (Held Frame), 0018 (per-Track-Audio), 0016 (Notif).
- **Kein** Code-Test; Teil der jeweiligen Feature-PR (Behavior-Change ⇒ Spec-Update,
  CLAUDE.md-Regel).

## Test-/Verify-Plan

### CI-fähig (GPU-/Hardware-frei, pure Resolver + Fake-Sources)
- `DecideAudioDeviceLoss` + Audio-Loss-Klassifikator (Schritt 1) — reine Pins, Muster
  `test_od_reopen_policy.cpp`.
- `Reinit`-State-Tests (Schritt 2): Flavor-Identität bleibt (feste `device_id_` /
  Default-Charakter / PID+Startzeit), `LastFatalError` reset; PID-Reuse-Fall (Startzeit
  passt nicht) ⇒ Reinit schlägt bewusst fehl.
- Audio-Thread-Integration mit Fake-Source (Schritt 3), **zwei Fälle**:
  **(a) Single-Source-Track** — Session überlebt den Verlust, Stille-Lücke exakter Länge,
  `stop_requested==false`; **(b) gemergter Track SYS+MIC (Blocker-Lücke)** — toter innerer
  MIC verstummt nur seinen Beitrag, der zweite Source mixt weiter, Session lebt; nach
  `Reinit` ist der `drift_estimator` zurückgesetzt; echter Encode-Fehler bleibt fatal.
- WGC-Source-Loss **Integrations-Pin am Drain** (Schritt 4) — `sourceLost ⇒ source_loss
  + stop_requested`, kein Reopen-Zweig (statt einer Konstant-Funktion).
- Notification-Dedupe/Standing/Resolution (Schritt 5).
- **Bestehende Pins bleiben grün und dienen als Regressionsnetz:**
  `test_od_acquire_failure_classify.cpp`, `test_od_reopen_policy.cpp`,
  `test_capture_drain_step.cpp`, `test_wasapi_acquire_failure_classify.cpp`
  (alle `libs/engine/tests`) sowie `app/tests/test_webcam_read_policy.cpp`
  (**App-Schicht**, s. Leitprinzip). (Der Audio-Klassifikator-Pin, der heute
  `DEVICE_INVALIDATED → Fail(=KillSession)` erwartet, muss auf `DegradeSource`
  angepasst werden — pre-1.0, kein Kompat-Zwang.)

### Nur User-live (echte Geräte; nie durch den Agenten, nie die App bedienen)
- **Monitor unplug/replug + Mode-Change mid-recording:** letzter Frame gehalten,
  nahtlose Fortsetzung, keine Timeline-Freeze. (Bestätigt bereits im Bestand.)
- **Zweiter Monitor physisch aus/an:** `ACCESS_LOST`-Hold vs. `DEVICE_REMOVED`-Stop
  auseinanderhalten.
- **Fenster schließen mid-recording:** Aufnahme endet sauber, Datei gültig.
- **USB-Mikro / Bluetooth-Headset mid-recording abziehen:** Track wird stumm (ehrliche
  Lücke), Video + andere Tracks laufen weiter; Wiedereinstecken → Track lebt wieder;
  Diagnostics-Karte + stehende Notification erscheinen/lösen sich auf. **Datei nie
  committen/veröffentlichen.**
- **Gemergter Track SYS+MIC, dann nur das MIC abziehen** (Blocker-Fall): der Track läuft
  weiter, SYS bleibt hörbar, nur der MIC-Beitrag verstummt — **nicht** der ganze Track,
  **nicht** die Session; Wiedereinstecken → MIC-Beitrag lebt wieder.
- **APP-/Fenster-SYS-Aufnahme, Ziel-Prozess beenden** (PID-Flavor): der Beitrag verstummt
  dauerhaft (kein Fremd-Prozess); PID-Reuse durch einen neuen Prozess greift ihn **nicht**.
- **Default-Mic abziehen** (Windows wählt ein anderes Default-Mikro): der Beitrag folgt
  dem neuen Default-Capture-Gerät (Konzept-Identität) — Notification-Text macht das klar.
- **Default-Render-Endpoint während SYS-Aufnahme umschalten** (Kopfhörer↔Speaker):
  SYS (SystemOutput-Flavor) folgt dem neuen Default (ehrlich), keine Session-Beendigung.
- **Webcam mid-recording abziehen:** PiP friert ein, Aufnahme läuft; Wiedereinstecken →
  live.
- **Windows-Audiodienst neu starten:** alle Audio-Tracks stumm, Video weiter; Dienst
  zurück → Tracks leben. (Härtefall für Entscheidung B / Sonderfall „alle Audio tot".)

## Risiken

- **Verhaltens-Change im Audio-Pfad** (heute session-fatal → künftig track-degradiert):
  ändert eine seit langem etablierte Semantik. Mitigation: pre-1.0 (kein Kompat-Zwang),
  scharfe Trennung „device loss (degrade)" vs. „encode error (fatal)", Live-Verify-Liste
  oben ist Pflicht-Gate.
- **Silence-Timing-Drift:** die eingespeiste Stille muss encoder-taktgenau sein, sonst
  A/V-Drift nach der Lücke. **Korrigiert nach Review:** Die frühere Mitigation „exakt den
  vorhandenen `feedGapSilence`-Pfad nutzen, keine eigene Zeitrechnung" ist **technisch
  nicht umsetzbar** — `feedGapSilence` wird von `raw.gap_frames` getrieben, das aus den
  Device-Positionen des *nächsten* Pakets stammt (`discontinuity_gap.h:29-41`, auf 10 s
  geklemmt). Während eines Unplugs kommen keine Pakete (kein `gap_frames`), und nach
  `Reinit` beginnt die Device-Position frisch — die Ausfall-Lücke ist so weder messbar
  noch (bei > 10 s) überbrückbar. **Reale Mitigation:** eine **eigene
  encoder-frame-basierte Zeitrechnung** im degradierten Zustand (Wall-Clock/QPC vs.
  `encoderAccumulatedFrames`) berechnet die zu speisenden Silence-Frames selbst; bei
  `Reinit` definierte Re-Synchronisation **inkl. Reset des `drift_estimator`**
  (`audio_thread.cpp:372-377`), damit die nach `Reinit` bei null startende Device-Position
  keine Garbage-Drift-Diagnostik erzeugt. Design gehört in Schritt 3 (dort spezifiziert).
- **SYS-Reaktivierung greift versehentlich ein anderes Gerät:** Default-Render-Endpoint
  kann sich zwischen Verlust und Reinit geändert haben. Das ist **gewollt** (Systemton =
  aktueller Default), muss aber in Doku + Notification-Text unmissverständlich sein,
  damit es nicht als „falsches Gerät" wahrgenommen wird.
- **Doppelte Wahrheitsquelle vermeiden:** Notifier-Snapshots dürfen nicht anfangen, die
  Engine-Loss-Entscheidung zu treffen (Race). Explizit: Notifier bleiben Idle-UI.
- **Diagnostics-Alarmismus:** die neuen Karten/Notifications könnten übertrieben wirken.
  Mitigation: ein ruhiger Hinweis pro Zustand, Auflösung bei Rückkehr, Post-flight nur
  Fakten (CLAUDE.md „Diagnostics ruhig, nicht alarmist").
- **UI-Leak in die Engine:** die Degradations-/Reaktivierungslogik muss im
  recorder_core-Thread + pure Resolvern leben; die App-Schicht konsumiert nur
  `SessionStats`/Diagnostics-Events (Engine bleibt UI-agnostisch).

## Offene Fragen (echte Produktentscheidungen)

1. **Audio-Track-Verlust: stumm-und-weiter (B) bestätigen?** Empfehlung dieser Spec:
   ja. Gegenargument des Bestands: „User verliert unbemerkt Audio". Mit stehender
   Notification + Diagnostics-Karte + Post-flight-Lücken-Ausweisung ist der Verlust
   *nicht* unbemerkt — ist das dem User genug, oder soll er pro Session einmalig
   entscheiden dürfen („bei Mikroverlust: weiter stumm / stoppen")?
2. **Differenziert SYS/APP (Systemton) vs. MIC in der Policy?** Vorschlag: gleiche
   Policy (degrade+reactivate). Denkbar wäre, MIC-Verlust höher zu gewichten (der User
   *spricht* evtl.) und dort früher/lauter zu warnen. Reicht ein einheitlicher Hinweis?
3. **Sonderfall „alle aktivierten Audio-Tracks tot": video-only weiterlaufen?**
   Vorschlag: ja (Bild ist Primärquelle). Oder soll der Totalverlust *aller* Audioquellen
   doch als „sauber beenden" gelten, weil eine gewollt vertonte Aufnahme ohne jeden Ton
   ihren Zweck verfehlt?
4. **Reaktivierungs-Cadence:** 500 ms (wie Webcam) für Audio übernehmen, oder eigener
   Wert? (Reine Tuning-Frage, kein Blocker.)

## Adversarialer Review — Ergebnis

- **Blocker (Merged Tracks / MixedAudioSrc stiller Tod): EINGEARBEITET.** Bestätigt:
  `mixed_audio_src.cpp:137-139` verschluckt inneres `AcquireBuffer==false` per `continue`,
  `AcquireBuffer` gibt immer `true` — der #180-Fix wirkt nur im Standalone-Pfad. Ist-Zustand
  korrigiert (session-fatal gilt nur für unwrapped Single-Source + MicDsp; jeder
  MixedAudioSrc-Wrap inkl. gain≠1 verschluckt still); Policy auf source-granular umgestellt;
  `mixed_audio_src.*`/`mic_dsp_audio_src.*` + Decorator-Reinit in Schritt 2/3 aufgenommen;
  Merged-Track-Test (CI + live) ergänzt.
- **Major (SYS = drei Flavors): EINGEARBEITET.** Bestätigt: SYS-ohne-Fenster→SystemOutput
  (`audio_track_model.cpp:17-22`), SYS-mit-Fenster + APP = PID-Loopback
  (`recorder_session.cpp:579-586`). Identitäts-Achse, Policy-Tabelle und Schritt-2-Reinit
  in drei Flavors getrennt; PID-Reuse-Schutz via Prozess-Startzeit, toter Prozess ⇒
  dauerhaft stummer Beitrag.
- **Major (Default-MIC widerspricht Policy): EINGEARBEITET.** Bestätigt:
  `RecordingPreset.cpp:126` `nullopt`, `wasapi_capture_src.cpp:288-293` bindet dann
  `GetDefaultAudioEndpoint`. Entschieden: Default-MIC = Konzept-Identität (folgt dem
  Default, symmetrisch zu SYS/Default-Render), ehrlich dokumentiert; Widerspruch in
  „Was NICHT gebaut wird" aufgelöst.
- **Major (feedGapSilence nicht nutzbar): EINGEARBEITET.** Bestätigt: `feedGapSilence` ist
  `raw.gap_frames`-getrieben (`discontinuity_gap.h:29-41`, 10-s-Clamp); während Ausfall
  keine Pakete, nach Reinit frische Device-Position. Mitigation ersetzt durch eigene
  encoder-frame-basierte Zeitrechnung + Drift-Estimator-Reset (Schritt 3 + Risiken).
- **Minor (DecideOdReopen nicht Prod-aufgerufen): EINGEARBEITET.** Bestätigt: nur Definition
  + Test; Video-Thread throttelt inline (`video_thread.cpp:2282, 2783`). Ist-Zustand
  richtiggestellt; Schritt 1 fordert nun einen wirklich thread-gefahrenen Resolver.
- **Minor (RecordFailure nur bei nicht-leerem captureErr / Pause-Pfad schluckt):
  EINGEARBEITET.** Bestätigt: `audio_thread.cpp:359` `if (!captureErr.empty())`,
  Pause-Pfad `:335-341`. Beide Pfade in Ist-Zustand präzisiert und in Schritt 3 explizit
  zu behandeln aufgenommen.
- **Minor (DecideWgcSourceLost Tautologie + Webcam-Policy ist App-Code): EINGEARBEITET.**
  Bestätigt: konstante Funktion ohne Verzweigung; reale Regression läge in
  `video_thread.cpp:2269-2274`; `test_webcam_read_policy.cpp` + `WebcamService.cpp` in
  `app/`. Schritt 4 auf einen Integrations-Pin am Drain umgestellt; die
  Subsystem-Schicht-Asymmetrie im Leitprinzip offengelegt.

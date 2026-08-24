# Exclusive-Fullscreen/Borderless-Capture-Matrix

> **SHIPPED (PR #204 + #209, 2026-07-12).** Verifiziert 2026-07-23: `KNOWN_LIMITATIONS.md` dokumentiert
> die Exclusive-Fullscreen-Erkennung (`rec.capture.exclusive_window`) als geschippt; die
> Roadmap-Selbstwidersprüche, die diese Spec anmahnte, sind ebenfalls behoben. Nichts hier ist mehr offen.

**Slice-Ziel:** Die Matrix *Anzeigemodus (Windowed / Borderless / Exclusive-Fullscreen) ×
Capture-Ziel (Monitor / Window / Region)* systematisch behandeln: was heute funktioniert, was wie
scheitert, wie ExoSnap das **erkennt** und **ehrlich meldet** — plus die bewusste Entscheidung,
welcher Pfad Exclusive-Fullscreen trägt. Roadmap-Einordnung: `docs/roadmap.md:84` zieht die Matrix
in **0.10.0** (vendor-unabhängige Härtung); `KNOWN_LIMITATIONS.md:270-271` und `:337` sagen noch
„0.12.x" — und `docs/roadmap.md` widerspricht sich selbst (die 0.3.0-Zeile `:77` und der
„Earlier waves"-Absatz `:264-265` sagen weiterhin „deferred to 0.12.x"). Alle vier Stellen sind
stale und werden in diesem Slice mitkorrigiert.

---

## Problem

Spiele in **legacy Exclusive-Fullscreen (FSE)** sind heute nicht zuverlässig aufnehmbar, und die
App sagt es dem User nicht rechtzeitig:

1. **Window-Capture (WGC) eines FSE-Fensters liefert keine Frames.** Ein FSE-Swapchain wird direkt
   gescannt, DWM komponiert das Fenster nicht — WGC hat nichts zu liefern. Beim Start läuft das in
   den 5-s-First-Frame-Timeout (Aufnahme schlägt fehl, Fehlertext nennt die Ursache nicht); wechselt
   das Spiel **mitten in der Aufnahme** in FSE, friert das Bild **stumm** ein (CFR dupliziert den
   letzten Frame, die Aufnahme läuft „grün" weiter).
2. **Monitor-Capture (DXGI OD) kann FSE grundsätzlich** (so nehmen OBS & Co. FSE-Spiele per
   Display-Capture auf), aber der FSE-Übergang wirft `DXGI_ERROR_ACCESS_LOST`, und ein Spiel, das
   die Desktop-Auflösung umschaltet, beendet die Aufnahme (Size-Guard). Beides ist im Drain
   robust behandelt, **beim Start jedoch nicht** (sofortiger Fehlschlag statt Reopen).
3. **Erkennung existiert nur hinter PresentMon** (Opt-in + Elevation): die einzige heutige
   Diagnostics-Karte (`rec.present.exclusive`) feuert nie für den Default-User.
4. Der Review (Market-Fit) benennt Exclusive-Fullscreen als eine der zwei größten Produktlücken
   gegen die Kernzielgruppe; §6 des Diagnostics-Plans fordert, „Schwarzbild"-Erkennung als
   FixAction zu bündeln.

Nicht Teil des Problems: perfektes Game-Capture à la OBS. Zu entscheiden ist, ob wir das bauen —
Abschnitt „Design" wägt es ab und lehnt es für pre-1.0 begründet ab.

---

## Ist-Zustand (alle Referenzen frisch von main @ #192)

### Backend-Wahl und Matrix-Mechanik

- **Backend-Split ist rein zielbasiert:** `libs/engine/src/video_thread.cpp:198`
  (`useOdCapture = (target.kind == Kind::Monitor)`); Kommentar `:316-318`: Monitor → DXGI OD,
  Window → WGC („only option for window/app capture").
- **Region = Monitor-OD + Crop:** `crop_region` wird nur für Monitor-Ziele angewendet
  (`video_thread.cpp:453`); `CaptureRegion` verlangt explizit `Kind::Monitor`
  (`libs/engine/include/exosnap/engine/recorder_session.h:126-128`). Die Region-Spalte der
  Matrix erbt damit vollständig das Monitor-Verhalten.
- **Es gibt nur zwei Target-Kinds:** `recorder_session.h:112-120` (`Monitor`, `Window`).

### WGC-Pfad (Window)

- Vor Init werden Fenster-Fakten erhoben und geloggt (valid/visible/minimized/cloaked, Window- und
  Client-Rect): `video_thread.cpp:288-314` — reine Log-Diagnose, keine Entscheidung daraus.
- Frame-Pool + Session: `video_thread.cpp:1394-1428`; `IsBorderRequired(false)` (`:1413`), Cursor
  manuell komponiert (`:1417`).
- **First-Frame-Wartefenster 5 s:** `video_thread.cpp:1438-1470`. Timeout →
  `RecordFailure(..., "WGC: timeout waiting for first frame (5 s)")` (`:1457-1461`) — die Aufnahme
  endet als Fehler, ohne Nennung der wahrscheinlichen Ursache (FSE, minimiert, elevated Window).
- **Drain:** liefert der Pool nichts, bleibt `pendingWgcTex` leer; der CFR-Tick nimmt dann den
  Duplicate-Pfad und re-encodiert den letzten Frame (`video_thread.cpp:2699-2704`,
  `++duplicatedFrames`). Größenwechsel des Fensters beendet die Session ehrlich
  (`:2403-2411`). Ein mid-session FSE-Wechsel ist also ein **stummer Freeze**.
- Die Bottleneck-Klassifikation schlägt bei 0 fps bewusst NICHT an (`cap_cond` verlangt
  `actual_fps > 0.0`, `libs/engine/src/pipeline_diagnostics_aggregator.cpp:635-637` —
  richtig so, ein statischer Desktop ist legitim). Sichtbar ist der Freeze nur als wachsendes
  `frames_duplicated` im LivePipelinePanel (`pipeline_diagnostics.h:96`,
  `app/ui/widgets/LivePipelinePanel.cpp:337`). Keine Karte, keine Notification.

### DXGI-OD-Pfad (Monitor/Region)

- **Acquire-Loss-Klassifikation ist pur und gepinnt:** `ClassifyOdAcquireFailure`
  (`dxgi_od_capture_src.h:210-224`) — `ACCESS_LOST` → `Recover`; der Kommentar nennt explizit
  „fullscreen" als Auslöser (`:215`). Reopen-Politik `DecideOdReopen` ist pur, **unbounded** per
  Default, mit optionalem Budget (`:240-264`); Poll-Kadenz 250 ms (`video_thread.cpp:1357`,
  Reopen-Schleifen `:2280-2288` und `:2782-2787`). Während des Holds trägt der letzte Frame die
  Timeline (eingefroren, nicht schwarz); `NextCaptureDrainStep` verhindert das Drainen des
  null-WGC-Pools (`dxgi_od_capture_src.h:275-288`).
- **Reopen re-resolved über den stabilen GDI-Namen** (`dxgi_od_capture_src.h:92-109`).
- **Size-/Format-Guard:** ein Frame in anderer Größe nach Reopen/Mode-Set beendet die Aufnahme
  sauber mit explizitem Fehler („restart recording to reconfigure",
  `video_thread.cpp:945-951`); Fremdformate werden geskippt (`:953-969`). ADR 0013 dokumentiert
  genau dieses Verhalten inkl. FSE als transienten Access-Loss
  (`docs/decisions/0013-dxgi-output-duplication-for-monitor-capture.md:122-133`).
- **Lücke beim Start:** `ACCESS_LOST` **vor dem ersten Frame** schlägt sofort fehl — keine
  Reopen-Maschinerie im Wartefenster (`video_thread.cpp:1520-1524`). Wer die Aufnahme startet,
  während ein Spiel gerade in FSE wechselt (oder FSE gerade aktiv wird), verliert den Start.

### Erkennung & Diagnostics heute

- **`rec.present.exclusive`** (Notice, `app/diagnostics/RecommendationEngine.cpp:602-629`):
  feuert nur bei `present_->mode == ExclusiveFullscreen`; FixAction `fix.present.borderless`
  (Assisted, „How to switch to borderless"). Präsent-Daten kommen ausschließlich aus PresentMon
  (ETW): **Opt-in + Elevation + offene Session** (`app/diagnostics/PresentMonProvider.h:11-27`,
  `PresentProvider.h:18-26`). Present-Mode-Mapping: Legacy-Flip-Codes 1/2 → `ExclusiveFullscreen`
  (`app/diagnostics/PresentModeMapping.cpp:7-22`). Verwandt: `checkPresentModeFlips`
  (`RecommendationEngine.cpp:696-717`).
- **PID-Attribution erst ab Record-Start:** `present_provider_.SetTargetProcessId(...)` wird beim
  Übergang in „recording" gesetzt (`app/MainWindow.cpp:1905-1911`,
  `app/pages/RecordPage.cpp:1404-1418`) — die Pre-Flight-Karte attribuiert global („whatever last
  presented").
- **Kein PresentMon ⇒ keine Erkennung.** Weder Pre-Flight noch Start noch mid-session gibt es
  einen elevationsfreien FSE-Hinweis. Das WGC-First-Frame-Timeout ist de facto der einzige
  „Detektor" — als opaker Fehler.
- **Engine-Konstruktion + Fakten-Injektion:** `app/pages/DiagnosticsPage.cpp:1376-1395`; das
  Muster für caller-gelieferte Ziel-Fakten existiert bereits
  (`RecommendationEngine::SetCaptureTargetHdrActive`, `RecommendationEngine.h:47-55`).
- **FixAction-Modell und -Routing:** eine Karte trägt **eine** optionale FixAction
  (`app/diagnostics/DiagnosticResult.h:37-63`); Auto-Fixes mutieren Settings nach Confirm im
  MainWindow-Handler, Assisted-Fixes navigieren (`app/MainWindow.cpp:4586-4662`).
- **Vorhandene ehrliche Evidenz — aber nur transient und falsch verortet:** ein Hub, dessen
  Quelle **nie** produziert hat, meldet `HubFrameKind::None`; hat sie produziert und gestoppt,
  meldet er `Held` mit dem letzten guten Frame (`app/services/CaptureHubRegistry.h:47-53`).
  Konsumiert wird die Registry heute an genau zwei Stellen: (a) **Picker-Tiles** via
  `ThumbnailCapture` (eigene Registry + `WgcSourceProducer` auf eigenem Worker-Thread,
  `app/services/ThumbnailCapture.cpp:268-328`) — lebt nur, solange der Picker offen ist
  (`releaseAll`, `SourcePickerPanel.cpp:1182/1284`), dort wird `None` als „Preview unavailable"
  sichtbar (`SourcePickerPanel.cpp:597`); (b) **DxgiCaptureHubService** — nur Monitor-Ziele.
  Die **Record-Page-Fenstervorschau läuft NICHT über die Registry**, sondern über
  `PreviewSurface::tryStartDxgiPreview` → `DxgiPreviewRenderer` (eigene WGC-Instanz ohne
  Frame-Kind-API, `RecordPage.cpp:2360-2361`; Hub-Weg nur für `Kind::Monitor`,
  `RecordPage.cpp:2349`). `DiagnosticsPage::refreshOverview` besitzt gar keine
  Fenster-Capture/Subscription (`DiagnosticsPage.cpp:1367-1397`: nur present_/dpc_/Config-Fakten).
  **Konsequenz:** ein FSE-Fenster ist als „unavailable" heute nur im offenen Picker sichtbar;
  eine pre-flight nutzbare Hub-Evidenz für das *ausgewählte* Fenster existiert noch nicht und
  muss als eigener Baustein gebaut werden (→ S2a).
- **Fensterenumeration filtert** unsichtbare/child/owned/iconic/cloaked Fenster
  (`libs/engine/src/wgc_capture.cpp:34-57`) — FSE-Fenster sind sichtbar und landen in der
  Liste.

### Produkt-/Doku-Stand

- Anti-Cheat-Posture ist produktspezifiziert: **keine Injection, kein Hooking, kein
  Speicherzugriff** — nur OS-Capture-APIs (`docs/product-spec.md:410-416`); ADR 0016:96-123 und
  ADR 0033:142-147 bestätigen dieselbe Linie (Info + Opt-out statt Auto-Disable; ETW ist Konsens-,
  kein Technik-Gate).
- product-spec §11 nennt als Assisted-Beispiel bereits „'switch the game to borderless' for
  exclusive-fullscreen black capture" (`docs/product-spec.md:621-622`) — die Karte dahinter
  existiert nur PresentMon-gated (s. o.).
- ADR 0033:138-140 skizziert den benachbarten Fall „elevated Window ohne Elevation nicht
  capturebar → Relaunch-FixAction" — nicht implementiert, hier **out of scope** (eigener Check,
  gleiche Fehlerfamilie).

### Ist-Matrix (aus dem Code abgeleitet; Zellen mit * brauchen Live-Verifikation)

| Anzeigemodus ↓ / Ziel → | **Monitor (DXGI OD)** | **Window (WGC)** | **Region (OD + Crop)** |
|---|---|---|---|
| **Windowed** | ✓ | ✓ | ✓ |
| **Borderless / FSO-„Fullscreen"** (Composed/Independent Flip) | ✓ (VRR-neutral, kein Indikator) | ✓ (WGC kann das Fenster aus MPO/iFlip in Composed zwingen — Perf-Nebeneffekt des OS, kein Bug) | ✓ |
| **Legacy FSE — Aufnahme läuft, Spiel wechselt in FSE** | `ACCESS_LOST` → Hold (frozen) → Reopen (250 ms, unbounded) → Weiteraufnahme der FSE-Frames* | **Stummer Freeze** (Duplicate-Pfad, keine Meldung) | wie Monitor; zusätzlich beendet ein Auflösungswechsel die Aufnahme (Size-Guard, ehrlicher Fehler) |
| **Legacy FSE — aktiv beim Start** | `Open()` bzw. erster Acquire kann `ACCESS_LOST` werfen → **sofortiger Startfehler** (keine Reopen-Schleife im Wartefenster)* | Kein First Frame → **Timeout-Fehler nach 5 s**, Ursache unbenannt | wie Monitor |
| **FSE mit Mode-Set** (Spiel ≠ Desktop-Auflösung) | Aufnahme endet sauber mit explizitem Size-Fehler | n/a (kein Frame) | wie Monitor, Crop-Koordinaten zusätzlich sinnlos geworden |

Windows-Kontext zur Einordnung (für die Doku, live zu verifizieren): Seit Win10 laufen die meisten
„Exclusive Fullscreen"-Settings real als FSO/eFSE (Independent Flip, DWM-sichtbar) — beide Backends
funktionieren. Echtes Legacy-FSE bleibt bei alten Titeln (DX9-Ära), deaktivierter FSO-Kompatibilität
und manchen OpenGL/Vulkan-Exclusive-Modi. Der Trend (Win11 24H2 forciert Flip-Model weiter) läuft
für uns: das Problem schrumpft von selbst — ein starkes Argument gegen Großinvestitionen.

---

## Design

### Optionen

**Option A — Hook-basiertes Game-Capture (OBS-Modell).**
DLL-Injection in den Spielprozess, Hook auf `Present`/`SwapBuffers` (DX8–12, OpenGL, Vulkan,
x86+x64), Shared-Texture-Übergabe.
*Pro:* das einzige technisch perfekte FSE-Capture; captured nur das Spiel (keine Overlays/
Notifications im Bild); minimaler Overhead.
*Contra:* (1) widerspricht der **veröffentlichten Produktzusage** „no injection"
(product-spec.md:410-416) — das ist keine interne Präferenz, sondern ein dokumentiertes
Vertrauensversprechen; (2) Anti-Cheat-Realität: OBS' graphics-hook ist über Jahre bei EAC/BattlEye/
Vanguard **namentlich gewhitelistet**; ein unsignierter Newcomer (ExoSnap ist bis heute unsigniert,
KNOWN_LIMITATIONS.md:226-227) riskiert für User **Spielsperren** — inakzeptabler Schaden;
(3) enorme, dauerhafte Wartungsfläche quer über Grafik-APIs und Bitness; (4) der adressierte Fall
(Legacy-FSE) schrumpft OS-seitig. → **Abgelehnt für pre-1.0.** Kein heimliches Offenhalten: wenn
je, dann als post-1.0-ADR mit Voraussetzungen (Code-Signing etabliert, AC-Vendor-Kontakt,
nachgewiesene Nachfrage) — siehe Offene Fragen.

**Option B — DXGI-OD als Exclusive-Pfad.**
Monitor-Capture kann FSE bereits (Ist-Matrix, Zeile 3). Sub-Optionen:
- **B1: stiller Auto-Fallback** Window→Monitor bei FSE-Erkennung. *Abgelehnt:* ändert unsichtbar,
  **was** aufgenommen wird (ganzer Desktop inkl. Notifications statt nur Spiel — Privacy-Scope) und
  **welche Audio-Tracks** entstehen (die APP-Reihe existiert nur bei Window-Target,
  product-spec/CLAUDE.md) — genau die Sorte stiller Verhaltensänderung, die das Projekt ausschließt.
- **B2: user-bestätigter Retarget als FixAction** („Record the monitor instead", Auto-Klasse mit
  Confirm + `changes_summary`, die den Scope- und APP-Audio-Verlust benennt). *Angenommen* — das
  ist der Pfad, den ExoSnap wirklich ausführen kann; die Borderless-Empfehlung kann die App nie
  selbst umsetzen (fremder Prozess).

**Option C — Ehrliches Nicht-Unterstützen von FSE-Window-Capture + Erkennung + FixAction.**
Kein neuer Capture-Pfad. Stattdessen: (1) Pre-Flight-Erkennung ohne Elevation, (2) ehrliche
Fehlertexte am Start, (3) mid-session Freeze wird gemeldet statt verschwiegen, (4) Monitor/Region
als dokumentierter FSE-Weg inkl. Schließen der Start-Lücke, (5) Matrix in die Doku.
*Pro:* deckt den Nutzerschaden (stille schwarze/eingefrorene Aufnahmen) vollständig ab, passt zur
Diagnostics-first-Identität („sagt dir *vorher*, ob die Aufnahme gelingt"), null Anti-Cheat-Risiko,
kleine, testbare Bausteine. *Contra:* ExoSnap kann weiterhin kein FSE-*Fenster* isoliert aufnehmen —
das bleibt eine benannte Grenze (gegen OBS' Hook-Capture), die der Monitor-Pfad praktisch abdeckt.

### Entscheidung

**C als Rahmen + B2 als primäre FixAction. A wird abgelehnt.** Begründung: C+B2 beseitigt den
realen Schaden (Nutzer verliert Aufnahmen, ohne zu wissen warum) mit Bordmitteln und stärkt den
USP; A kauft den letzten Zentimeter Capture-Qualität für ein Risiko, das die Produktzusage bricht
und Nutzer gefährden kann.

### Erkennungs-Leiter (ohne Elevation zuerst, Präzision oben)

Für das ausgewählte **Window**-Target, als caller-gelieferte Fakten (Muster
`SetCaptureTargetHdrActive`):

1. **Fensterform-Heuristik** (pur, testbar): `window_rect` deckt das Monitor-Rect von
   `MonitorFromWindow` vollständig ab UND Style ohne `WS_CAPTION|WS_THICKFRAME` →
   `FullscreenShaped`. Unterscheidet **nicht** Borderless von FSE — bewusst nur als schwaches
   Signal geführt.
2. **Hub-Evidenz** (stark, ehrlich, gemessen — erfordert die neue Probe aus S2a, denn keine
   heutige Subscription überlebt den Picker oder ist aus Diagnostics erreichbar, s. Ist-Zustand):
   eine dedizierte WGC-Subscription auf das *ausgewählte* Fenster. Zwei Beweisformen:
   - `HubFrameKind::None` nach ≥ 2 s aktiver Subscription: *dieselbe API, die die Aufnahme
     benutzen würde, liefert nachweislich nichts.* Stark, weil WGC beim Session-Start einen
     initialen Frame des aktuellen Inhalts liefert — auch für vollständig statische Fenster
     (gemessen; genau darum seedet die Engine den ersten WGC-Frame,
     `video_thread.cpp:1431-1436`). Nur ein Fenster ohne DWM-Surface (FSE) bleibt `None`.
   - **Stale-Held nach Shape-Übergang:** `HubFrameKind::Held` UND seit dem beobachteten Übergang
     zu `FullscreenShaped` ist ≥ 2 s **kein frischer Frame** mehr angekommen. Deckt den
     häufigsten Realfall (Fenster produzierte im Windowed/Borderless, Spiel schaltet dann in
     FSE — der Hub hält den letzten guten Frame, wird also nie wieder `None`). Die
     Übergangs-Korrelation („kein Frame *seit* dem Shape-Wechsel") schützt legitime statische
     Borderless-Fenster (z. B. pausiertes Vollbild-Video: produzierte Frames *nach* dem
     Shape-Übergang ⇒ keine Evidenz).
3. **`SHQueryUserNotificationState` == `QUNS_RUNNING_D3D_FULL_SCREEN`** (dokumentierte Shell-API,
   keine Elevation): bestätigt Legacy-D3D-FSE — nur für den **Primärmonitor** aussagekräftig.
4. **PresentMon** (Opt-in + Elevation, existiert): präziser `ExclusiveFullscreen`-Befund.

**Severity-Politik (ruhig, nicht alarmistisch):** Nur positive Evidenz erzeugt eine Karte.
`FullscreenShaped` allein → **gar nichts** (Borderless ist der Normalfall und funktioniert).
`FullscreenShaped` + (QUNS oder PresentMon-FSE) → **Notice**. **ProvenBlack** = `FullscreenShaped`
UND (Hub `None` ≥ 2 s **oder** stale-Held: kein frischer Frame seit dem Shape-Übergang, ≥ 2 s)
→ **Blocker** (die Aufnahme *wird* scheitern bzw. *ist* nachweislich eingefroren; Blocker sind
immer sichtbar und gaten den Start — CLAUDE.md/product-spec §11). Eine Karte, eine primäre
FixAction (B2); die Borderless-Empfehlung steht im `recommendation`-Text.

### Was bewusst NICHT gebaut wird

- Kein Hook-/Injection-Capture, keine Prozess-Speicherzugriffe (Option A).
- Kein stiller Backend- oder Target-Fallback (B1); kein Auto-Stop bei mid-session Freeze — der
  User entscheidet (Analogie: OD-Hold ist unbounded).
- Kein neuer Engine-Capture-Backend, keine Capability-/Resolver-Änderung (libs/capability bleibt
  unberührt — das ist keine Codec-Frage).
- Kein Versuch, FSE-Fenster über alternative WGC-Item-Konstruktionen doch zu greifen (führt zu
  undokumentiertem Verhalten).
- Kein genereller „Fenster ist statisch"-Detektor (False-Positives auf legitim statischen
  Fenstern; Erkennung nur mit positiver Zusatz-Evidenz).

---

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz. Reihenfolge: S1 → S2a → S2b sind
abhängig; S3 und S6 unabhängig; **S4 ist auf die Live-Verifikation des Freeze-Verhaltens gestaffelt**
(erst bauen, nachdem Live-Verify-Punkt 5a das stumme Einfrieren real bestätigt hat — der teuerste
Baustein für den seltensten Fall wird nicht auf Verdacht gebaut); S5 zuletzt (gated auf Live-Verify).

### S1 — Fenster-Fakten + purer Form-/Evidenz-Resolver

- **Neu** `app/diagnostics/WindowTargetFacts.h/.cpp`:
  - `struct WindowTargetFacts { bool valid, visible, minimized, cloaked, is_foreground; RECT window_rect, monitor_rect; LONG_PTR style, ex_style; bool quns_d3d_fullscreen; }`
  - Pur (kein Win32 in der Logik): `ClassifyWindowShape(facts) -> WindowShape { Normal, FullscreenShaped }`
    und `CombineFullscreenEvidence(shape, hub_evidence, optional<PresentMode>) ->
    ExclusiveEvidence { None, Suspected, ProvenBlack }`. Die Hub-Evidenz kommt als purer
    Snapshot-Struct (von S2a geliefert):
    `struct WindowHubEvidence { HubFrameKind kind; double seconds_subscribed; double seconds_since_fresh_frame; bool fresh_frame_since_fullscreen_shape; }`.
    **ProvenBlack** = `FullscreenShaped` UND (`kind == None && seconds_subscribed ≥ 2` ODER
    `kind == Held && !fresh_frame_since_fullscreen_shape && seconds_since_fresh_frame ≥ 2`);
    **Suspected** = `FullscreenShaped` + QUNS/PresentMon-FSE.
  - Dünner Win32-Gatherer `GatherWindowTargetFacts(HWND)` (GetWindowRect/MonitorFromWindow/
    GetWindowLongPtr/DwmGetWindowAttribute/SHQueryUserNotificationState) — nur Fakten, keine Wertung.
- **Tests (CI):** Tabellen-Tests der puren Resolver (Borderless-Rect == Monitor-Rect ⇒ nur
  FullscreenShaped, nie ProvenBlack ohne Hub-Evidenz; Multi-Monitor-Rects; randlose Fenster mit
  1-px-Overhang; minimiert ⇒ Normal; **stale-Held-Fälle:** Held ohne frischen Frame seit
  Shape-Übergang ⇒ ProvenBlack, Held mit Frames nach dem Übergang — pausiertes
  Borderless-Video — ⇒ kein ProvenBlack; None unter 2 s ⇒ kein ProvenBlack).

### S2a — Selected-Window-Evidenz-Probe (neues Plumbing — ohne sie ist die Leiter-Stufe 2 tot)

Es gibt heute **keine** Hub-Subscription, die pre-flight abfragbar wäre: die Picker-Registry
stirbt mit dem Panel, der DxgiCaptureHubService kennt nur Monitore, die Record-Page-Fenstervorschau
läuft am Registry-Modell vorbei (s. Ist-Zustand). Deshalb:

- **Neu** `app/services/WindowEvidenceProbe.h/.cpp` nach dem erprobten `ThumbnailCapture`-Muster
  (`ThumbnailCapture.cpp:248-350`): eigener Worker-Thread (STA-COM), eigene `CaptureHubRegistry`
  mit injizierbarer `ProducerFactory` (Default `WgcSourceProducer`), **genau eine** Subscription —
  auf das aktuell ausgewählte Window-Target.
- Lebenszyklus: subscribe bei Auswahl eines Window-Targets (Idle/Pre-Flight), unsubscribe bei
  Abwahl/Monitor-Target und **pausiert während der Aufnahme** (WGC kennt keine OD-Exklusivität,
  eine Lease-Maschinerie ist nicht nötig — die Pause vermeidet nur die doppelte Capture-Last;
  mid-session übernimmt S4 auf Basis der Pipeline-Statistik).
- Der Worker pollt zusätzlich `GatherWindowTargetFacts` (~1 Hz) und akkumuliert die
  Übergangs-Korrelation: Zeitpunkt des letzten frischen Frames (Callback-Generation) vs.
  Zeitpunkt des Übergangs zu `FullscreenShaped`. Nach außen exponiert er thread-safe einen
  **Snapshot** (`WindowHubEvidence` + `WindowTargetFacts`, per Mutex kopiert) — kein Qt, keine
  Wertung im Service.
- Ownership: MainWindow (analog `present_provider_`), Selektionswechsel angebunden am selben
  Punkt wie die S6-PID-Attribution.
- **Tests (CI):** Registry mit injizierter `ProducerFactory` (Muster
  `test_capture_hub_registry.cpp`): None-Akkumulation über 2 s; produzieren→stoppen ⇒ Held +
  `seconds_since_fresh_frame` wächst; Shape-Übergangs-Korrelation (Frames nach Übergang setzen
  `fresh_frame_since_fullscreen_shape`); Pause/Resume verliert keine Subscription-Semantik.

### S2b — Pre-Flight-Karte `rec.capture.exclusive_window` + FixActions

- `RecommendationEngine`: neuer Setter `SetCaptureWindowEvidence(std::optional<ExclusiveEvidence>)`
  (Muster `RecommendationEngine.h:53-55`); neue Check-Funktion `checkExclusiveWindowTarget`:
  - `Suspected` → Notice; `ProvenBlack` → **Blocker**. Titel/Copy faktisch: „Selected window is in
    exclusive fullscreen and cannot be captured" (Blocker-Variante: „…produces no frames").
  - Primäre FixAction `fix.capture.monitor_instead` — **Auto mit zwingendem Confirm** (nie
    One-Click-Apply), reversibel, `changes_summary` benennt explizit: ganzer Monitor statt nur
    Fenster wird aufgenommen (inkl. anderer Fenster/Notifications) und die APP-Audio-Reihe
    entfällt (nur SYS/MIC). **Taxonomie-Hinweis:** product-spec §11 definiert Auto heute als
    „safe, reversible, config-only" mit reinen Encoder-Beispielen (`product-spec.md:617-618`) —
    ein Capture-Target-Wechsel dehnt das. Assisted wäre trotzdem falsch (Assisted kann per
    Definition nichts ausführen, nur navigieren/kopieren — der Wert von B2 IST die Ausführung).
    Deshalb wird die Auto-Definition in S5 explizit erweitert (s. dort) statt still gedehnt.
  - `recommendation`-Text: „Set the game to Borderless / Windowed Fullscreen to capture the window
    directly." (Dedupe: solange diese Karte feuert, unterdrückt `checkExclusiveFullscreen` seine
    generische `rec.present.exclusive`-Karte — 1 Problem, 1 Karte.)
- `DiagnosticsPage::refreshOverview` (`DiagnosticsPage.cpp:1385-1395`): der Probe-Snapshot aus
  S2a wird — wie `present_provider_->Sample()` heute — von MainWindow an die Page gereicht;
  `refreshOverview` kombiniert ihn pur via `CombineFullscreenEvidence` und injiziert das Ergebnis
  über den neuen Setter. Die Page selbst erhebt keine Win32-Fakten und hält keine Subscription.
- FixAction-Routing (`MainWindow.cpp:4586ff`): `fix.capture.monitor_instead` → neue RecordPage-API
  `selectMonitorTargetForWindow()` (HMONITOR via `MonitorFromWindow`, Match gegen die Targets-Liste;
  kein Match ⇒ No-op + Log). Danach üblicher Propagate/Refresh-Pfad.
- **Tests (CI):** Engine-Tests mit injizierter Evidenz (Severity-Leiter, FixAction-Felder, Dedupe);
  Widget-Test für den Retarget (Window-Target ausgewählt → Fix → Monitor-Target selektiert, APP-Reihe
  verschwindet); Routing-Test analog `test_diagnostics_page`.

### S3 — Ehrliche Startfehler (Engine, minimal-invasiv)

- **WGC-First-Frame-Timeout** (`video_thread.cpp:1457-1461`): Fehlertext um die bereits erhobenen
  Fenster-Fakten (`:288-314`) und die wahrscheinliche Ursache erweitern: „…no frame within 5 s
  (window minimized=… cloaked=…). A window in exclusive fullscreen cannot be captured — switch the
  game to borderless or record the monitor instead." Zusätzlich strukturierte Log-Fields. Engine
  bleibt UI-agnostisch (nur Fehlerstring/Logs; die App zeigt ihn wie bisher als Fehler-Notification).
- **OD-Start-Lücke schließen** (`video_thread.cpp:1520-1524`): `ACCESS_LOST` vor dem ersten Frame
  nicht mehr sofort fatal, sondern dieselbe Hold/Reopen-Maschinerie wie im Drain, mit **gebundenem
  Budget** über das existierende `DecideOdReopen(budget=15 s)` (pur, bereits getestet —
  `dxgi_od_capture_src.h:240-264`). **Achtung, der Punkt ist nicht lokal:** die Zeilen liegen
  innerhalb der gemeinsamen Wait-for-first-frame-Schleife, deren eigener Guard
  `elapsed > kTimeoutSec (5.0)` in **jeder** Iteration auch für den OD-Pfad feuert
  (`video_thread.cpp:1443,1457-1461`) — ohne Umbau wäre das 15-s-Budget toter Code, der 5-s-Timeout
  beendet die Session vorher mit dem generischen Text. Deshalb wird die Schleife umgebaut:
  `ACCESS_LOST` setzt einen Start-Hold-Zustand (`odStartHolding`, Analog zu `odHolding` im Drain);
  **solange der Hold aktiv ist, ist der 5-s-Guard ausgesetzt** und die Deadline gehört allein dem
  Reopen-Budget (`odSrc.Reopen` auf `kOdReopenPollDelay`-Kadenz wie im Drain,
  `video_thread.cpp:2280-2288`; Entscheid via `DecideOdReopen(reopened, elapsed_since_loss,
  budget=15 s, poll_delay)`). Reopen-Erfolg ⇒ Hold aufgehoben, der 5-s-First-Frame-Guard läuft
  **neu ab diesem Zeitpunkt** (frische Duplication, frische Frist). `GiveUp` ⇒ ehrlicher Fehler,
  der den Fullscreen-/Mode-Übergang als wahrscheinliche Ursache nennt (statt „timeout waiting
  for first frame").
- **Tests:** puren Budget-Entscheid gibt es schon (`test_od_acquire_failure_classify.cpp` ergänzen:
  Start-Budget-Fälle inkl. Delay-Clamping ans Restbudget). Die Guard-Aussetzung wird als purer
  Zustands-Entscheid herausgezogen (z. B. `FirstFrameWaitStep(useOd, odStartHolding, elapsed,
  elapsed_since_hold_end) -> {KeepWaiting, TimeoutFail, HoldStep}`) und tabellengetestet — der
  Rest ist nur live verifizierbar (s. Verify-Plan).

### S4 — Mid-Session-Ehrlichkeit: Freeze-Karte + Standing-Notification (gestaffelt, verschlankt)

> **Nachtrag (QCR-804, umgesetzt): der gebaute Vertrag weicht an zwei Stellen ab.**
> 1. **Stufe 1 misst `capture.frames_captured`, nicht `capture.actual_fps`.** Der Aggregator
>    leitet `actual_fps` aus den **emittierten** Frames ab — im Stall pacet der CFR-Encoder
>    weiter mit Duplikaten, `actual_fps` bleibt also bei ~60. Das hier spezifizierte Gate
>    hätte nie ausgelöst.
> 2. **Der ausgelieferte Vertrag ist ein Capture-Stall-Vertrag, kein FSE-Vertrag.** Gemeldet
>    wird jedes fullscreen-shaped Fenster ohne Frame-Fortschritt; Exclusive Fullscreen wird nur
>    dann *benannt*, wenn QUNS/PresentMon es bestätigen. Ursachenlos gemeldete Stalls heißen
>    „appears to have stalled", nicht „exclusive fullscreen detected". Maßgeblich ist
>    `docs/product-spec.md` §7 und `app/diagnostics/WindowCaptureStall.h`.

**Staffelung:** S4 wird erst gebaut, nachdem Live-Verify-Punkt 5a das stumme Einfrieren real
bestätigt hat — es ist der schwerste Baustein für den seltensten (und OS-seitig schrumpfenden)
Fall. Ganz streichen wäre aber falsch: der stumme mid-session Freeze ist Schaden Nr. 1 des
Problem-Abschnitts (eine „grün" durchlaufende, wertlose Aufnahme), und Option C verspricht
explizit „(3) mid-session Freeze wird gemeldet statt verschwiegen".

**Verschlankung — zweistufig statt Dauer-Polling:**

- **Stufe 1 (kostenlos, kein Win32):** auf der bestehenden `diagnosticsUpdated`-Kadenz nur die
  Snapshot-Felder prüfen: Window-Target UND `capture.actual_fps == 0` über ≥ 10 s UND
  `frames_duplicated` steigend. Solange das nicht zutrifft, wird **nichts** gepollt.
- **Stufe 2 (nur im Verdachtsfall):** einmaliger `GatherWindowTargetFacts`-Aufruf (+ QUNS),
  dann das **pure Prädikat** (neu, z. B. `app/diagnostics/WindowCaptureStall.h`):
  `EvaluateWindowStall(snapshot_facts, window_facts, seconds_starved)` mit Ursachen-Enum
  (`Minimized`, `ExclusiveFullscreen`, `Unknown` ⇒ still bleiben). Positive Evidenz bleibt
  Pflicht (`minimized` | `FullscreenShaped`+QUNS/PresentMon-FSE); solange der Verdacht anhält,
  re-check ≤ 1 Hz — nie dauerhaft.
- RecordPage zeigt bei Befund **eine** Standing-Notification („Recording continues, but the
  captured window stopped producing frames (exclusive fullscreen). The video is frozen until
  frames return.") + Live-Karte in Diagnostics; beides räumt sich auf, sobald Frames wieder
  fließen. **Kein Auto-Stop.**
- **Tests (CI):** Prädikat-Tabellen (statisches Fenster ohne Evidenz ⇒ still; minimiert ⇒
  Minimized; Flapping ⇒ Hysterese über die 10-s-Schwelle); Stufe-1-Gating (kein Fakten-Gather
  ohne Verdacht) und Notification-Gating per injizierten Snapshots.

### S5 — Doku, Matrix, Defer-Korrektur (gated auf Live-Verify)

- `docs/product-spec.md` §7: Unterabschnitt „Fullscreen capture matrix" mit der ehrlichen
  Ist-Matrix (nach Live-Verifikation der *-Zellen) + der Regel „Exclusive-Fullscreen nimmt man über
  Monitor/Region auf; Window-Capture erfordert Borderless"; §11-Check-Katalog um
  `rec.capture.exclusive_window` ergänzen. **§11 FixAction-Taxonomie erweitern:** die
  Auto-Definition (`product-spec.md:617-618`, heute „safe, reversible, config-only" mit reinen
  Encoder-Beispielen) explizit um bestätigte Capture-Target-Änderungen ergänzen, mit der Regel:
  ändert eine Auto-FixAction den Aufnahme-Scope oder die Track-Struktur, ist der Confirm-Dialog
  mit `changes_summary` **zwingend** (kein One-Click), und die Summary muss die Scope-/Track-Folgen
  benennen. `fix.capture.monitor_instead` wird dort als Beispiel geführt.
- `KNOWN_LIMITATIONS.md`: die beiden „deferred to 0.12.x"-Stellen (Z. 270-271, 337) durch die
  tatsächliche Matrix-Grenze ersetzen (und Roadmap-Ziel 0.10.0 nennen); Freeze-/Timeout-Verhalten
  dokumentieren.
- `docs/roadmap.md` selbst konsistent machen: die 0.3.0-Zeile (`:77`, „…was deferred to
  `0.12.x`") und der „Earlier waves for reference"-Absatz (`:264-265`) sagen weiterhin 0.12.x und
  widersprechen der 0.10.0-Zeile (`:84`, „deferred from 0.3.0") — beide Stellen auf 0.10.0
  korrigieren.
- Schnittstelle zu `diagnostics-support-bundle-spec` (docs/troubleshooting.md): Symptom
  „Schwarzbild/eingefrorenes Spiel" → Karte `rec.capture.exclusive_window` → Fix. Nur der Eintrag,
  die Troubleshooting-Datei selbst gehört jener Spec.

### S6 — (klein, unabhängig) PresentMon-Attribution schon im Pre-Flight

- `SetTargetProcessId` zusätzlich bei **Auswahl** eines Window-Targets im Idle setzen (heute nur
  bei Record-Start, `MainWindow.cpp:1905-1911`), Rückstellung auf 0 bei Abwahl. Macht die
  bestehende `rec.present.exclusive`-Karte pre-flight zielgenau statt „whatever last presented".
- **Test (CI):** bestehende MainWindow-/RecordPage-Testmuster erweitern (PID-Weitergabe bei
  Selektionswechsel).

---

## Test-/Verify-Plan

### CI-fähig (headless, ohne GPU/Spiel)

- S1: pure Form-/Evidenz-Resolver (Tabellen, inkl. stale-Held-Fälle).
- S2a: Probe-Evidenz-Akkumulation mit injizierter `ProducerFactory` (None-2-s, Held+stale,
  Shape-Übergangs-Korrelation, Pause/Resume).
- S2b: RecommendationEngine mit injizierter Evidenz (Severity, FixAction-Felder, Dedupe gegen
  `rec.present.exclusive`); RecordPage-Retarget-Widget-Test; FixAction-Routing.
- S3: `DecideOdReopen`-Start-Budget (pur) + purer First-Frame-Wait-Entscheid
  (5-s-Guard ausgesetzt während Start-Hold, frische Frist nach Reopen).
- S4: Stall-Prädikat + Stufe-1-Gating + Notification-Gating mit synthetischen Snapshots.
- S6: PID-Attributions-Weitergabe.

### Nur User-live (explizite Checkliste; Ergebnis füttert S5-Doku)

Benötigt ein echtes Legacy-FSE-Spiel (alter DX9-Titel oder aktuelles Spiel mit deaktivierter
FSO-Kompatibilität) + ein Borderless-Spiel, je auf Primär- und Sekundärmonitor:

1. **Monitor-OD, Spiel wechselt in FSE mid-recording:** Reopen greift, Aufnahme enthält
   FSE-Frames nach kurzer Frozen-Lücke; Datei abspielbar. (Kernbehauptung der Matrix — vor S5
   nicht als Produktzusage dokumentieren.)
2. **Monitor-OD, Start während FSE aktiv:** mit S3 startet die Aufnahme (Budget 15 s reicht);
   ohne Reopen-Erfolg ehrlicher Fehler.
3. **FSE mit Mode-Set** (Spiel auf ≠ Desktop-Auflösung): Aufnahme endet sauber, Fehlertext nennt
   Größenwechsel; Datei bis dahin nutzbar.
4. **WGC-Window eines FSE-Spiels beim Start:** neuer Timeout-Text erscheint; Pre-Flight zeigte
   vorher Blocker/Notice (Hub-Evidenz über die S2a-Probe — beide Beweisformen prüfen: Fenster
   war von Anfang an FSE ⇒ `None`; Fenster previewte erst und schaltete dann um ⇒ stale-Held).
5. **Mid-session FSE-Wechsel bei Window-Target — zweistufig (S4-Gate):**
   a) **vor S4:** bestätigen, dass der Freeze real und stumm auftritt (`frames_duplicated`
   wächst im LivePipelinePanel, Bild eingefroren, keine Meldung) — erst dieses Ergebnis
   rechtfertigt den S4-Bau;
   b) **nach S4:** Standing-Notification + Karte binnen ~10-15 s; Rückkehr zu Borderless räumt
   beides; Aufnahme läuft durch.
6. **QUNS-Verhalten:** Primär- vs. Sekundärmonitor (erwartet: nur primär), FSO-Spiel darf QUNS
   nicht triggern (sonst Evidenz-Leiter nachjustieren).
7. **Retarget-FixAction:** Confirm nennt Scope + APP-Audio-Verlust; danach Monitor selektiert,
   Aufnahme des FSE-Spiels via OD funktioniert.
8. **False-Positive-Gegenprobe:** Borderless-Spiel als Window-Target ⇒ keinerlei Karte.

Regel aus dem Projektgedächtnis gilt: Test-Aufnahmen sind erlaubt, Dateien nie committen.

---

## Risiken

- **Heuristik-Fehlklassifikation:** Borderless ist ebenfalls „FullscreenShaped" — deshalb erzeugt
  Form allein nie eine Karte; Blocker verlangt gemessene Hub-Evidenz. Der stale-Held-Zweig ist
  durch die Übergangs-Korrelation gegen legitime statische Borderless-Fenster abgesichert (Frames
  *nach* dem Shape-Übergang ⇒ keine Evidenz); Restrisiko: Probe (noch) nicht subscribed oder unter
  2 s aktiv ⇒ keine Hub-Evidenz ⇒ nur Notice statt Blocker (akzeptiert: lieber unter- als
  überwarnen).
- **Probe-Kosten und -Lebenszyklus (S2a):** eine zusätzliche WGC-Session pro ausgewähltem
  Window-Target im Idle (klein, aber nicht null — WGC ist im Gegensatz zu OD nicht exklusiv,
  keine Lease nötig). Pausiert während der Aufnahme; Fehlerfälle (Fenster geschlossen,
  Subscription tot) degradieren zu „keine Hub-Evidenz", nie zu falscher Evidenz.
- **QUNS-Grenzen:** nur Primärmonitor, historisch teils `QUNS_BUSY`-Überlagerungen. Wird nur als
  Zusatz-Evidenz benutzt, nie allein.
- **OD-FSE-Zusagen basieren auf API-Dokumentation/Branchenpraxis, nicht auf eigenem Test:** die
  Matrix-*-Zellen dürfen erst nach der Live-Checkliste in product-spec/KNOWN_LIMITATIONS als
  Verhalten dokumentiert werden (S5 ist explizit gated).
- **S3-Start-Hold ändert Start-Semantik:** ein früher sofortiger Fehler wartet jetzt bis 15 s im
  „Preparing"-Zustand. Wechselwirkung mit `record-start-preparing-state-spec` (M-9) beachten —
  Budget bewusst kurz, Verhalten geloggt.
- **Retarget-FixAction ändert die Track-Struktur** (APP-Reihe entfällt): muss im
  `changes_summary` stehen; sonst stille Verhaltensänderung.
- **Freeze-Erkennung vs. legitime Stille:** minimierte/verdeckte, aber absichtlich gehaltene
  Fenster erzeugen eine Notification. Copy hält sie faktisch („frozen until frames return"), sie
  ist standing (kein Toast-Spam) und räumt sich selbst. Das Investitionsrisiko (schwerster
  Baustein für den seltensten Fall) ist durch die Staffelung begrenzt: S4 wird erst nach
  live-bestätigtem Freeze-Verhalten gebaut (Verify-Punkt 5a) und in der verschlankten
  Zweistufen-Form (kein Dauer-Polling).
- **Win11-Entwicklung:** Legacy-FSE stirbt weiter aus; der Slice bleibt bewusst klein, damit die
  Investition zum schrumpfenden Problem passt.

---

## Offene Fragen (echte Produktentscheidungen)

1. **Blocker vs. Notice bei nachweislich schwarzem Fenster:** Spec schlägt **Blocker** vor (Start
   wird scheitern; Diagnostics-first heißt vorher sagen). Alternative: Notice + Startversuch mit
   besserem Fehler. Bitte bestätigen.
2. **Start-Hold-Budget für OD-`ACCESS_LOST` beim Start:** 15 s gebunden (Vorschlag) vs. unbounded
   wie im Drain vs. Fail-fast wie heute.
3. **Release-Zuordnung:** Roadmap sagt 0.10.0, KNOWN_LIMITATIONS noch 0.12.x — bestätigen, dass
   dieser Slice unter 0.10.0 (vendor-unabhängige Härtung) läuft und die Doku entsprechend
   korrigiert wird.
4. **Hook-Capture-Posture festschreiben:** Ablehnung nur pre-1.0 (Backlog-Eintrag mit
   Voraussetzungen: Signing + AC-Whitelisting + Nachfrage) oder grundsätzlich „nie" als
   ADR-Ergänzung zur Anti-Cheat-Posture?

---

## Adversarialer Review — Ergebnis

Sechs Einwände (1 Blocker, 2 Major, 3 Minor), jeder gegen Code/Docs auf main geprüft:

1. **Hub-Evidenz in refreshOverview nicht verfügbar (Blocker) — EINGEARBEITET.** Bestätigt:
   Fenster-Preview läuft über `DxgiPreviewRenderer` statt der Registry (`RecordPage.cpp:2349,
   2360-2361`), Registry-Konsumenten sind nur der transiente Picker (`ThumbnailCapture`,
   `releaseAll` bei Hide/Close) und der Monitor-only-HubService; `DiagnosticsPage` hält keine
   Subscription. Neuer Schritt **S2a** (Selected-Window-Evidenz-Probe nach `ThumbnailCapture`-
   Muster, thread-safe Snapshot inkl. ≥2-s-Akkumulation, Ownership MainWindow) plus korrigierter
   Ist-Zustand und präzisiertes S2b-Wiring.
2. **15-s-Reopen-Budget unter dem 5-s-First-Frame-Timeout unerreichbar (Major) —
   EINGEARBEITET.** Bestätigt: der `kTimeoutSec=5.0`-Guard feuert in jeder Iteration auch für OD
   (`video_thread.cpp:1443,1457`). S3 umgebaut: Start-Hold-Zustand setzt den 5-s-Guard aus, die
   Deadline gehört während des Holds dem `DecideOdReopen`-Budget; nach Reopen läuft die
   First-Frame-Frist neu; purer `FirstFrameWaitStep`-Entscheid als Testpunkt ergänzt.
3. **ProvenBlack unterfeuert im Held-Fall (Major) — EINGEARBEITET.** Bestätigt via
   `CaptureHubRegistry.h:47` und Hub-Tests (produzieren→stoppen ⇒ `Held`). Prädikat erweitert um
   stale-Held **mit Shape-Übergangs-Korrelation** (kein frischer Frame seit dem Übergang zu
   FullscreenShaped, ≥2 s) — die Korrelation, nicht bloß „Held + 0 Frames", weil sonst legitime
   statische Borderless-Fenster (WGC liefert initial immer einen Frame, dann nur bei Repaint)
   als ProvenBlack fehlklassifiziert würden. S1/S2a-Tests entsprechend ergänzt.
4. **roadmap.md selbst inkonsistent (Minor) — EINGEARBEITET.** Bestätigt: `roadmap.md:77` und
   `:264-265` sagen weiter „deferred to 0.12.x". Beide Stellen in S5 und im Intro aufgenommen.
5. **Auto-Klasse für Capture-Scope-Wechsel dehnt die Taxonomie (Minor) — EINGEARBEITET (mit
   anderem Fix als vorgeschlagen).** Die Dehnung ist real (`product-spec.md:617-618`: „config-
   only", reine Encoder-Beispiele). Assisted wäre aber die falsche Antwort: Assisted kann per
   Definition nichts ausführen (navigieren/kopieren), der Wert von B2 IST die Ausführung.
   Stattdessen: Confirm + `changes_summary` als zwingend markiert (S2b) und die §11-Auto-
   Definition wird in S5 explizit um bestätigte Capture-Target-Änderungen erweitert — Taxonomie
   dokumentiert statt still gedehnt.
6. **S4 zu schwer für den seltensten Fall (Minor) — EINGEARBEITET (Staffelung + Verschlankung,
   keine Streichung).** S4 ist jetzt auf Live-Verify-Punkt 5a gestaffelt (erst bauen, wenn der
   stumme Freeze real bestätigt ist) und zweistufig verschlankt (Snapshot-only-Gating ohne
   Win32-Polling; Fakten-Gather nur im Verdachtsfall). Nicht gestrichen, weil der stumme
   mid-session Freeze Schaden Nr. 1 des Problem-Abschnitts ist und Option C die Meldung
   explizit verspricht.

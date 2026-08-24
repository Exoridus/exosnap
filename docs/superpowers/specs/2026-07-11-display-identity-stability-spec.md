# Stabile Display-Identität statt GDI-Gerätename

> **SHIPPED (PR #200, 2026-07-12).** Verifiziert 2026-07-23 gegen aktuellen Code:
> `DisplayIdentityResolver`, `DisplayIdentityEnumerator`, `DisplayDeviceNotifier`, `StableDisplayId`
> vorhanden. Nichts hier ist mehr offen.

## Problem

Ein gespeichertes Display- oder Region-Capture-Target hängt an einer instabilen
Identität: dem GDI-Gerätenamen (`\\.\DISPLAY1`) bzw. der daraus abgeleiteten
sequenziellen Nummer. Windows vergibt diese Namen/Nummern bei Topologie-Wechseln
(Monitor ab-/anstecken, KVM/EDID-Renegotiation, Mode-Set, Reboot mit anderer
Anschlussreihenfolge) neu. Zwei Fehlerbilder folgen daraus:

1. **Stiller Fehlgriff:** Die gespeicherte Kennung matcht nach dem Wechsel einen
   *anderen* physischen Monitor. Der Nutzer nimmt ungewollt den falschen Bildschirm
   auf, ohne Hinweis.
2. **Stiller Verlust:** Die Kennung matcht gar nichts mehr (Nummer verschoben) —
   die Auswahl bleibt leer, der Nutzer muss die Quelle wortlos neu wählen.

Bei Region-Targets kommt hinzu, dass das Rechteck in absoluten Virtual-Screen-Koordinaten
persistiert wird. Ändert sich die Monitoranordnung, liegt das gespeicherte Rechteck
auf falschen Pixeln oder außerhalb jeder Fläche — auch das heute ohne ehrliches Signal.

Ziel: eine **stabile, hardware-nahe Display-Identität** für die *Persistenz* von
Targets, plus ein **ehrlicher Diagnostics-Pfad**, wenn das gespeicherte Display
fehlt — statt eines stillen Fehlgriffs. Die Laufzeit-Capture-Maschinerie (Hub-Keying,
Preview, HDR) bleibt unberührt.

## Ist-Zustand (mit Datei:Zeile-Referenzen)

### Was heute persistiert wird

- `PresetCaptureTarget` (`app/models/RecordingPreset.h:78-87`) speichert die
  Capture-Auswahl als **beschreibungsbasierte Strings**: `display_key`, `window_key`,
  `region_display_key` plus `has_region` + `region` (`exosnap::engine::CaptureRegion`,
  Virtual-Screen-Koordinaten). Kommentar dort: „Raw platform handles (HWND, HMONITOR)
  are never stored … keys are description-based and matched at restore time."
- Serialisiert wird das 1:1 als TOML-`[capture]`-Tabelle
  (`app/settings/RecordingPresetStore.cpp:574-582` schreiben, `726-733` lesen).
- `kPresetSchemaVersion` steht auf **23** (`app/models/RecordingPreset.h:45`).
  Laden repariert seit v23 feldweise; fehlende/ungültige Felder fallen auf den
  Modell-Default zurück, ein reiner Versionssprung wird dem Nutzer nicht gemeldet
  (`app/models/RecordingPreset.h:26-33`).

### Was der `display_key` tatsächlich ist

- Beim Speichern setzt `RecordPage` `cap.display_key = TargetLabelFromCaptureTarget(target)`
  (`app/pages/RecordPage.cpp:1385-1398`), und `cap.region_display_key = cap.display_key`
  (Zeile 1398).
- `RecordViewModel::TargetLabelFromCaptureTarget` liefert für Monitore
  `"Desktop - " + DisplayLabelFromTarget(target.description)`
  (`app/viewmodels/RecordViewModel.cpp:700-713`).
- `DisplayLabelFromTarget` nimmt `target.description` (den GDI-Gerätenamen,
  `\\.\DISPLAY6`), strippt `\\.\` und macht aus dem **rohen Trailing-Numeric**
  `DISPLAY6` → `"Display 6"` (`app/viewmodels/RecordViewModel.cpp:671-694`).
- **Der persistierte `display_key` ist also z. B. `"Desktop - Display 6"`** — die
  rohe GDI-Nummer, nicht einmal die neu-sequenzierte „Display N"-Anzeige.

### Wie beim Restore aufgelöst wird

- `RecordPage::applyCapturePolicy` (`app/pages/RecordPage.cpp:1428-1520`) matcht den
  gespeicherten `match_key` per **String-Gleichheit** gegen
  `TargetLabelFromCaptureTarget(t)` der aktuell enumerierten Targets (Zeile 1472-1485).
- Verhalten:
  - Leerer Key → Auto-Pick (Primär/erster Monitor) — „keine Präferenz".
  - Nicht-leerer Key **ohne Match** → UNRESOLVED, Auswahl bleibt leer, **kein**
    Auto-Pick, blanke Preview (Zeile 1483-1484, 1509-1517). Kein Diagnostics-Signal.
  - Nicht-leerer Key **mit Match** → ausgewählt. **Hier sitzt der stille Fehlgriff:**
    matcht die Nummer `Display 6` nach Topologie-Wechsel einen anderen Monitor,
    wird kommentarlos der falsche gewählt.
- `region_display_key` wird geschrieben und gelesen, aber bei der Auflösung **nicht
  verwendet**. Präzise: der eigentliche **Dirty-Vergleich** `ConfigDirtyEquivalent`
  (`app/models/RecordingPreset.cpp:766-778`) schließt das **komplette `capture`-Substruct**
  (inkl. `region_display_key`) explizit **aus** — es ist ein „environment"-Feld. Der
  Feldvergleich in Zeile 500 sitzt in `NormalizedConfigEquals` (semantische Gleichheit /
  Persistenz-Round-Trip, `app/models/RecordingPreset.cpp:494-511`), das nur zur Round-Trip-
  Verifikation dient, nicht zum Dirty-State. Die Region wird beim Restore als absolute
  Virtual-Screen-Koordinaten verbatim übernommen (`app/pages/RecordPage.cpp:1488-1493`).

### Region-Anker heute

- Beim Zeichnen wird der Basis-Monitor an das Rechteck angepasst, indem der Monitor
  gesucht wird, dessen Geometrie den Origin des Rechtecks enthält
  (`app/pages/RecordPage.cpp:3760-3790`).
- Zur Laufzeit invalidiert eine Snapshot-Änderung die Region, wenn **kein** aktueller
  Display den **Origin-Punkt** des Rechtecks enthält — der Code prüft nur
  `display_geom.contains(region_origin)`, also den linken/oberen Eckpunkt, **nicht** die
  volle Rechteck-Fläche (`app/pages/RecordPage.cpp:4958-4965`). Zusätzliche Fehlerquelle:
  hier werden **physische** Region-Koordinaten (`region.x/y`) gegen die **logische,
  DPI-skalierte** `QScreen::geometry()` (`display.geometry`) geprüft — auf skalierten
  Displays ist schon dieser Live-Check unsauber. Er greift ohnehin nur live, nicht beim
  Restore, und ist geometrie-, nicht identitätsbasiert.

### Verfügbare Identitätsquellen im Code

- **QScreen (Qt):** `DisplayInfo.id = QScreen::name()` = GDI-Gerätename; der Enumerator
  liest bereits `screen->manufacturer()` und `screen->model()` (aus EDID), **nicht**
  aber `serialNumber()` (`app/services/DisplayDeviceNotifier.cpp:53-81`). **`serialNumber()`
  wird nirgends im Repo aufgerufen** (Repo-weite Suche: 0 Treffer) — d. h. es gibt keinen
  Beleg, dass Qts Windows-QPA dieses Feld überhaupt befüllt; der Enumerator behandelt sogar
  `manufacturer`/`model` bereits als potenziell leer (Zeile 59-68). Für die stabile Identität
  ist das direkt relevant (siehe Ranking-Stufe 2 unten). Der Header
  dokumentiert die Instabilität explizit und benennt „EDID or the monitor device path
  (SetupAPI / DISPLAYCONFIG_PATH_INFO)" als aufgeschobene stabile Identität
  (`app/services/DisplayDeviceNotifier.h:22-29`).
- **DisplayConfig-API ist bereits im Einsatz:** `QuerySdrWhiteLevelNits`
  (`libs/engine/src/dxgi_od_capture_src.cpp:63-107`) läuft die aktiven Pfade
  via `GetDisplayConfigBufferSizes` + `QueryDisplayConfig` durch, matcht per
  `DISPLAYCONFIG_SOURCE_DEVICE_NAME.viewGdiDeviceName` gegen `MONITORINFOEXW.szDevice`
  und liest dann Target-Infos über `path.targetInfo.adapterId/id`. **Exakt dieser Loop
  ist die Vorlage** für die Target-Identität — es fehlt nur eine zusätzliche
  `DISPLAYCONFIG_TARGET_DEVICE_NAME`-Abfrage auf demselben Target.
- **HDR-Facts** (`capability::DisplayHdrFacts.name`, `runtime_snapshot.h:18-37`) und
  **Capture-Hub-Key** (`app/services/CaptureSourceKey.h:17-29`, DXGI-Hub keyt per
  GDI-`device_name`) sind **Laufzeit**-Konstrukte, frisch bei Capture-Start aufgelöst,
  nicht persistiert. Der Hub-Key wird aus `GetMonitorInfoW(...).szDevice` gefüllt
  (`app/services/DxgiCaptureHubService.cpp:68-85`). → **Sie sind vom Identitätsproblem
  nicht betroffen** und dürfen unangetastet bleiben.

### Diagnostics-Infrastruktur

- `DiagnosticResult` (`app/diagnostics/DiagnosticResult.h:10-63`) mit
  `DiagnosticSeverity { Pass, Notice, Blocker }`, `DiagnosticGroup::Display` und
  optionaler `FixAction` (Safety `Auto`/`Assisted`/`External`) ist vorhanden — ein
  ruhiger Display-Notice mit „Assisted"-Fix (Quellenwahl öffnen) fügt sich ein.

## Design

### Kernentscheidung: Wo lebt die stabile Identität?

Die stabile Identität ist ausschließlich ein **Persistenz- und Restore-Anliegen**.
Sie ersetzt **nicht** die Laufzeit-Keys (Hub, Preview, HDR), sondern speist einen
Resolver, der beim Restore aus der gespeicherten Identität ein *konkretes aktuelles*
`CaptureTarget` (HMONITOR + GDI-`device_name`) produziert. Downstream ändert sich
nichts. Das hält die Engine UI-agnostisch und den Blast-Radius klein.

### Alternativen für die Identitäts-Kennung

**A. AdapterLUID + Output-Index (DXGI `IDXGIAdapter`/`IDXGIOutput`-Kombination).**
Reject als Primär-Identität. Der AdapterLUID ist bewusst *nicht* über Reboots/
Treiber-Neustarts stabil (Microsoft dokumentiert LUID als bootweit eindeutig, nicht
persistent); ein Treiber-Update ändert ihn. Der Output-Index ist Enumerationsreihenfolge,
also topologieabhängig — genau das Problem, das wir lösen wollen. Taugt höchstens als
Laufzeit-Cross-Check, nie als gespeicherter Schlüssel.

**B. Qt-EDID-Tripel `{manufacturer, model, serialNumber}`.**
Vorteil: praktisch **kein** neuer nativer Code — der Enumerator liest schon `manufacturer`
und `model`; es fehlt nur `serialNumber()`. Die Identität folgt dem **physischen Panel**
(Seriennummer wandert mit dem Monitor an einen anderen Port). Nachteile: Qts EDID-Read
ist historisch lückenhaft (Felder können leer sein, je nach Panel/Treiber), und —
entscheidend — **Zwillings-Monitore gleichen Modells ohne EDID-Seriennummer sind
ununterscheidbar** und lassen sich nicht per Anschluss disambiguieren.

**C. DisplayConfig `monitorDevicePath` (`DISPLAYCONFIG_TARGET_DEVICE_NAME`).**
Der Monitor-Device-Interface-Pfad (`\\?\DISPLAY#GSM5B09#5&...&UID4352#{GUID}`) ist
robust, wird von einer bereits genutzten API geliefert (siehe Ist-Zustand) und
**disambiguiert Zwillinge über den Anschluss/Connector-Instance**. Nachteil: er folgt
dem **Port, nicht dem Panel** — ein Kabelwechsel zwischen zwei Ports ändert ihn. Er ist
opak (keine schöne Anzeige).

**D. Roh-EDID-Seriennummer via SetupAPI/Registry.**
Stärkster Panel-Follow, aber der meiste neue Code (Geräte-Enumeration + EDID-Block-Parsing);
Overengineering für den MVP dieses Themas.

**Entscheidung: Komposit-Identität, primär C, angereichert um B.**
Wir persistieren einen strukturierten `StableDisplayId` mit **mehreren** Feldern und
lösen über einen **gerankten Matcher** auf. So bekommen wir Port-Stabilität *und*
Panel-Follow, wo EDID-Serien vorhanden sind, *und* Zwillings-Sicherheit:

```
StableDisplayId {
  device_path      // DISPLAYCONFIG_TARGET_DEVICE_NAME.monitorDevicePath (primär)
  edid_vendor      // edidManufactureId (PNP, z. B. "GSM")
  edid_product     // edidProductCodeId
  serial           // EDID-Seriennummer, wenn Qt sie liefert; sonst leer
  friendly_name    // monitorFriendlyDeviceName (nur Anzeige/Fallback-Match)
  gdi_name         // "\\.\DISPLAYn" — letzter Fallback + Debug
  seq_hint         // sequenzielle Nummer zum Zeitpunkt des Speicherns (nur Debug/Anzeige)
}
```

Ranking beim Restore (erster Treffer gewinnt; kein Auto-Pick eines Nicht-Treffers):

1. **`device_path` exakt** → höchste Confidence (gleicher Anschluss).
2. **`{edid_vendor, edid_product, serial}` exakt, `serial` nicht leer** → gleiches Panel
   an anderem Port (Kabelwechsel). **Provisorisch — hängt an einer verfügbaren
   Seriennummer** (siehe Serial-Quellen-Vorbehalt direkt unten).
3. **`{edid_vendor, edid_product}` + `friendly_name`, wenn systemweit eindeutig** →
   Einzel-Monitor gleichen Modells; bei **mehreren** gleichen Modells ohne Serie:
   **nicht** matchen (Ambiguität ehrlich als „unresolved" behandeln, nicht raten).
4. Kein Treffer → **UNRESOLVED** → Diagnostics-Notice (unten), Auswahl leer.

`device_path` ist die einzige verpflichtende Feldquelle; die EDID-Felder sind
best-effort. Fällt DisplayConfig komplett aus (RDP, exotischer Treiber), degradiert der
Matcher auf Stufe 3/4 mit `gdi_name`+`friendly_name` — nie schlechter als heute.

**Serial-Quellen-Vorbehalt (Stufe 2 darf nicht auf Verdacht gebaut werden).**
Der Entwurf zog `serial` ursprünglich aus `QScreen::serialNumber()`. Da dieses Feld
**nirgends im Repo verwendet** wird und Qts Windows-QPA EDID-Serien historisch nicht
zuverlässig befüllt, ist unbewiesen, dass Stufe 2 je Daten sieht — dann wäre sie toter
Code und Live-Verify Nr. 2 prüfte nichts. Verbindliche Reihenfolge:
- **Gate:** Bevor Stufe 2 als tragend gilt, wird in Schritt 2 eine **Einmal-Probe** über
  `EnumerateDisplayIdentities()` protokolliert (Log-Zeile pro Display mit
  vendor/product/**serial**-Belegung). Kommt `serial` auf realer Hardware nicht-leer an,
  bleibt Stufe 2 wie beschrieben.
- **Fallback bei leerer Qt-Serie:** Die EDID-Seriennummer wird **nativ** über den ohnehin
  persistierten `device_path` gelesen (Device-Interface-Pfad → Registry-EDID-Blob,
  gebundene Größe, deutlich weniger Code als das verworfene Option D „SetupAPI-Voll-Enumeration"
  — der Pfad steht bereits fest, nur der EDID-Block wird ausgelesen und Byte 12–15 als Serial
  entnommen).
- **Ehrliche Streichung:** Liefert weder Qt noch der native Read verlässlich eine Serie,
  wird Stufe 2 **gestrichen** und der Panel-Follow-nach-Kabeltausch offen als bekannte Grenze
  dokumentiert (statt sie zu behaupten). Stufe 1/3/4 tragen den Rest.

Diese Verzweigung ist eine echte Produkt-/Aufwandsentscheidung und wird nicht implizit im
Code getroffen — der Gate-Log aus Schritt 2 entscheidet sie sichtbar.

### Zwillings-Monitore gleichen Modells — explizit

- **Mit distinkten EDID-Serien:** Stufe 2 unterscheidet sie sauber; Identität folgt dem
  Panel. Bester Fall.
- **Ohne Serien (viele Budget-Panels), an festen Ports:** Stufe 1 (`device_path`)
  unterscheidet sie über den Connector. Solange Kabel nicht getauscht werden, korrekt.
- **Ohne Serien, nach Port-/Kabeltausch:** Nicht sicher unterscheidbar. Der Matcher
  **rät nicht** (Stufe 3 verlangt Eindeutigkeit) → UNRESOLVED + Notice. Das ist die
  ehrliche Grenze und wird in KNOWN_LIMITATIONS so benannt.

### Persistenzformat (Breaking Change, pre-1.0, keine Migration)

Die drei String-Felder `display_key` / `window_key` / `region_display_key` werden durch
strukturierte Sub-Tabellen ersetzt. `window_key` bleibt beschreibungsbasiert (Fenster
haben keine hardware-stabile Identität; das ist ein eigenes Thema und außerhalb dieses
Scopes). Neues `[capture]`-Layout:

```toml
[capture]
kind = "display"            # display | window | region
window_key = "…"            # unverändert, beschreibungsbasiert

[capture.display_id]        # StableDisplayId; leer = "keine Präferenz / primär"
device_path = "\\?\DISPLAY#GSM5B09#…#{GUID}"
edid_vendor = "GSM"
edid_product = 23305
serial = "…"
friendly_name = "LG HDR 4K"
gdi_name = "\\.\DISPLAY6"
seq_hint = 2

has_region = true
region_display_id = { … }   # StableDisplayId des Anker-Displays
region_x_norm = 0.10        # Region relativ zum Anker-Display (siehe unten)
region_y_norm = 0.05
region_w_norm = 0.50
region_h_norm = 0.50
```

Pre-1.0-Politik: **kein Migrationszwang.** Alte Dateien haben die neuen Sub-Tabellen
nicht; die feldweise Reparatur (bereits vorhanden, `RecordingPreset.h:26-33`) lässt
`display_id` leer → verhält sich wie „keine Präferenz". Konkretes Ergebnis: nach dem
Update ist das gespeicherte Display-Target einmalig leer und wird beim ersten Speichern
neu, stabil geschrieben. Das wird **nicht** als Fehler gemeldet (nur „repaired" bei
echtem Parse-Fehler). `kPresetSchemaVersion` 23 → **24**.

### Region: anker-relativ statt absolut

Statt absoluter Virtual-Screen-Koordinaten wird die Region als **`StableDisplayId` des
Anker-Displays + normalisierte Koordinaten (0..1) relativ zu dessen aktueller Geometrie**
gespeichert.

**Koordinatenraum — verbindlich physisch.** `exosnap::engine::CaptureRegion` (`view_model_.region`)
ist in **physischen Virtual-Screen-Pixeln** definiert (`app/pages/RecordPage.cpp:286-287`
`rcMonitor`-Kommentar; `:2307-2308` „virtual-screen physical pixels"). Die Anker-Geometrie
zum Normalisieren **und** zum Zurückrechnen MUSS deshalb ebenfalls physisch sein: die
**`MONITORINFOEXW.rcMonitor`** des Anker-`HMONITOR` (wie `QueryScreenPresentation`,
`app/pages/RecordPage.cpp:331-351`), **nicht** `QScreen::geometry()`, das DPI-skaliert
logisch ist (`app/services/DisplayDeviceNotifier.cpp:69`). Normalisiert man physische
Region-Pixel gegen die logische Qt-Geometrie, ist der Round-Trip auf jedem Display mit
Skalierung ≠ 100 % falsch. Konsequenz: `EnumeratedDisplayIdentity` trägt eine **physische
`RECT` (rcMonitor)** als Anker-Geometrie, nicht `QRect geometry` (siehe Resolver-Schicht).

Beim Restore:

1. Anker-Display über den Ranking-Matcher auflösen.
2. Region aus den normalisierten Werten × aktuelle **physische** Anker-`rcMonitor` in
   absolute Virtual-Screen-Pixel zurückrechnen, auf gültige Grenzen clampen.
3. Anker fehlt → Region **nicht** anwenden, Diagnostics-Notice.

Normalisiert (nicht absolute Pixel), damit ein Auflösungswechsel des Anker-Displays das
Rechteck proportional mitnimmt statt es beschnitten/verschoben zu hinterlassen. Das ist
der bewusste Trade-off gegen pixelgenaue Wiederherstellung bei identischer Auflösung —
für ein Region-Preset ist proportionale Treue das ehrlichere Verhalten. (Die pure
Rundungs-/Clamp-Mathematik ist testbar; siehe Test-Plan.)

### Diagnostics: ehrlicher Notice statt stiller Fehlgriff

Neuer, **ruhiger** Check in `DiagnosticGroup::Display`, nur wenn ein **konkret
gespeichertes** Target beim Restore/aktuellen Config-Stand **unresolved** ist:

- Severity **Notice** (kein Blocker — Aufnahme des primären/aktuellen Displays bleibt
  möglich; das Fehlen einer *gespeicherten* Präferenz blockiert nichts).
- Title z. B. „Gespeicherter Bildschirm nicht gefunden", Summary nennt den
  `friendly_name`/`seq_hint` der gespeicherten Wahl.
- **Eine** `FixAction`, Safety **Assisted**: „Quelle neu wählen" öffnet den Source-Picker
  (kein Auto-Umschalten auf ein geratenes Display).
- Erscheint **nur** bei echtem Unresolved eines nicht-leeren gespeicherten Targets —
  nicht bei „keine Präferenz", nicht als Dauer-Alarm. (Passt zur Doktrin „nur echte/gemessene
  Probleme, 1 Fix pro Problem".)

**Lebenszyklus des Notice — explizit spezifiziert (statt nur versprochen).**
Der Notice ist eine reine Projektion des `unresolved`-Flags, das der Restore/Re-Resolve-Pfad
setzt. Er verschwindet genau dann, wenn dieses Flag gelöscht wird. Drei Auslöser, jeder mit
zugeordnetem Implementierungsschritt — **keiner davon ist heute vorhanden**, daher decken die
Schritte sie ausdrücklich ab:
- **Nutzer wählt manuell neu** (Source-Picker / Combo): `syncTargetSelectionToCombo`-Pfad
  löscht das Flag und cached die neue `StableDisplayId` (Schritt 4). Manuelle Auswahl ist
  per Definition „resolved".
- **Ziel-Display kehrt zurück** (Topologie-Änderung): `RecordPage::onDisplaysChanged`
  (`app/pages/RecordPage.cpp:4997-4999`) ruft heute nur `enumerateTargets(...)` und matcht die
  **gespeicherte Identität nie neu**. Schritt 5 ergänzt hier einen **Re-Resolve-Hook**: bei
  jeder Snapshot-Änderung wird die gespeicherte `StableDisplayId` erneut gegen
  `EnumerateDisplayIdentities()` aufgelöst; Treffer → Auswahl setzen + Flag löschen,
  weiterhin Miss → Flag halten. Ohne diesen Hook wäre „verschwindet, sobald das Display
  zurückkehrt" unerfüllt.
- **Produktentscheidung — Rückkehr = nur Notice löschen oder auch Auswahl wiederherstellen?**
  Festlegung: Wenn der Nutzer seit dem Verlust **nicht** manuell etwas anderes gewählt hat,
  stellt die Rückkehr die Auswahl **wieder her** (das gespeicherte Target war ja der erklärte
  Wunsch) und löscht den Notice. Hat der Nutzer zwischenzeitlich **manuell** ein anderes
  Target gewählt, wird die Auswahl **nicht** überschrieben (die Rückkehr löscht dann höchstens
  einen bereits obsoleten Notice). Ein „unresolved-sticky"-Bit unterscheidet die beiden Fälle.

### Reine Resolver-Schicht (Engine bleibt UI-agnostisch)

- **Impure Enumeration** (Win32/Qt): eine Funktion, die je aktivem Display einen
  `EnumeratedDisplayIdentity { StableDisplayId, HMONITOR, gdi_name, RECT rc_monitor_physical }`
  liefert. Die Anker-Geometrie ist die **physische `MONITORINFOEXW.rcMonitor`** (nicht
  `QRect`, siehe Region-Abschnitt). Aufbau (siehe Schritt 2 für Details):
  `QueryDisplayConfig`-Loop (Vorlage `dxgi_od_capture_src.cpp:63-107`) liefert nur **Pfade**,
  keine HMONITORs — daher zusätzlich ein `EnumDisplayMonitors`/`GetMonitorInfoW`-Pass, der
  je `HMONITOR` die `szDevice` (GDI-Name) und `rcMonitor` holt und über `szDevice` mit der
  Pfad-Liste (`DISPLAYCONFIG_SOURCE_DEVICE_NAME.viewGdiDeviceName`) verjoint; die Vorlage
  macht genau diesen Join, nur in Gegenrichtung ab einem gegebenen HMONITOR. Auf demselben
  Target dann `DISPLAYCONFIG_TARGET_DEVICE_NAME` (device_path, EDID-vendor/product,
  friendly_name). Serial best-effort aus Qt bzw. nativem EDID-Read (siehe
  Serial-Quellen-Vorbehalt). Lebt in der App-/Services-Schicht, **nicht** in der Engine.
- **Purer Matcher** `ResolveStableDisplay(saved StableDisplayId, enumerated list)
  → optional<match + confidence>`: keine Win32-Abhängigkeit, voll testbar. Ebenso
  `AnchorRelativeRegionToAbsolute(...)` / `AbsoluteRegionToAnchorRelative(...)` als
  pure Funktionen. Dies spiegelt die vorhandene Trennung (`FindDisplayByName` pure,
  `FindTargetDisplayFacts` impure, `app/services/TargetDisplayFacts.h`).

## Implementierungsschritte

Die Schritte 1, 2, 6 und 7 sind je eine eigenständig landbare PR. Die Schritte **3–5
landen als eine Einheit** (ein PR oder ein gestackter, zusammen mergender Satz): Schritt 3
entfernt `display_key`/`region_display_key` aus `PresetCaptureTarget`, was die Aufrufstellen
in `RecordPage.cpp` (Save: `:1390`/`:1398`; Restore-Match: `:1459`) **sofort nicht mehr
kompilieren** lässt — genau diese Stellen stellen erst Schritt 4/5 um. Ein isoliertes Landen
von Schritt 3 wäre entweder ein Build-Bruch oder — mit temporärem Bridging — eine
nonfunktionale Display-Präferenz zwischen den PRs, unter der Automerge-Politik eine sichtbare
Zwischenregression. Daher: 3–5 sind **eine** atomare Verhaltensänderung. Die frühere
Behauptung „jeder Schritt ist eine PR-fähige Einheit" ist damit korrigiert.

**Schritt 1 — Werttyp + purer Matcher (CI-testbar, kein Win32).**
- Neu: `app/models/StableDisplayId.h` (`struct StableDisplayId`, Gleichheit,
  `empty()`), plus `app/services/DisplayIdentityResolver.{h,cpp}` mit
  `ResolveStableDisplay(...)` (gerankter Matcher, Confidence-Enum) und den
  Region-Normalisierungs-Funktionen. Rein, keine Qt-/Win32-Includes im Header.
- Tests: `app/tests/test_display_identity_resolver.cpp` — Ranking-Stufen, Zwillings-
  Fälle (mit/ohne Serie, mit/ohne Kabeltausch), Ambiguität → kein Match, Region-Mathe
  (Normalisieren/Zurückrechnen inkl. Clamp/Rundung).

**Schritt 2 — Impure Enumeration.**
- Neu: `app/services/DisplayIdentityEnumerator.{h,cpp}` → `EnumerateDisplayIdentities()`
  liefert `std::vector<EnumeratedDisplayIdentity>`. Aufbau:
  1. `EnumDisplayMonitors` + `GetMonitorInfoW` je Monitor → `{ HMONITOR, szDevice,
     rcMonitor (physisch) }` (die HMONITOR- und Anker-RECT-Quelle; der QueryDisplayConfig-
     Loop selbst liefert **keine** HMONITORs).
  2. `GetDisplayConfigBufferSizes` + `QueryDisplayConfig` (Vorlage
     `dxgi_od_capture_src.cpp:63-107`), Join Pfad↔Monitor über
     `DISPLAYCONFIG_SOURCE_DEVICE_NAME.viewGdiDeviceName == szDevice`.
  3. Auf dem gejointen Target `DISPLAYCONFIG_TARGET_DEVICE_NAME` →
     `monitorDevicePath`, `edidManufactureId`, `edidProductCodeId`,
     `monitorFriendlyDeviceName`.
  4. `serial` best-effort: erst Qt (`QScreen::serialNumber()`, Join über GDI-Namen),
     bei leer der native EDID-Read über `monitorDevicePath` (siehe Serial-Quellen-Vorbehalt).
- **Serial-Gate-Log:** einmal beim ersten Aufruf pro Prozess eine Debug-Log-Zeile pro
  Display mit vendor/product/**serial-Belegung** (leer/nicht-leer), damit der User-Live-Verify
  entscheiden kann, ob Ranking-Stufe 2 real trägt oder gestrichen wird.
- Tests: nur Kompilierung/Smoke-Injektion (die echte Enumeration ist nicht CI-fähig —
  siehe Test-Plan). Enumerator hinter einem `std::function`-Seam injizierbar machen
  (wie `DisplayDeviceNotifier::setEnumeratorForTest`).

**Schritt 3 — Persistenzformat umstellen (Breaking, Schema 24).** *(landet zusammen mit
4 + 5 — siehe PR-Ability-Hinweis oben; isoliert bricht es die RecordPage-Aufrufstellen.)*
- `app/models/RecordingPreset.h`: `PresetCaptureTarget` — `display_key`/
  `region_display_key`-Strings ersetzen durch `StableDisplayId display_id` /
  `StableDisplayId region_display_id`; Region-Felder auf `*_norm` umstellen.
  `kPresetSchemaVersion` → 24; Header-Kommentar (v24-Block) ergänzen.
- `app/settings/RecordingPresetStore.cpp:574-582/726-733`: Sub-Tabellen schreiben/lesen;
  fehlende Sub-Tabelle → leere Identität (feldweise Repair, kein Reset).
- `app/models/RecordingPreset.cpp`: **präzise Trennung der zwei Komparatoren.**
  - `NormalizedConfigEquals` (Feldvergleich Zeile 494-511): der bisherige
    `region_display_key`-String-Vergleich (Zeile 500) sowie `display_key` (494) werden
    durch `StableDisplayId::operator==` (bzw. `display_id`/`region_display_id`) ersetzt.
    Dieses Prädikat dient nur der Persistenz-Round-Trip-Verifikation.
  - `ConfigDirtyEquivalent` (Zeile 766-778) vergleicht das `capture`-Substruct **weiterhin
    gar nicht** — es bleibt ein „environment"-Feld; hier ändert sich nur der Kommentar
    (die Feldaufzählung „…, `region_display_key`" → „…, `region_display_id`").
  - `StripEnvironmentFields`/Header-Kommentare (Header 178-206) analog aktualisieren
    (Capture-Identität bleibt „environment", nicht dirty).
- Tests: `test_recording_preset_store.cpp`, `test_recording_preset.cpp`,
  **`test_recording_preset_registry.cpp` (Zeile ~219 setzt `capture.region_display_key`)**,
  `test_audio_encoding_preset.cpp` auf das neue Layout anpassen (die dortigen
  `region_display_key = "…"`-Fixtures ersetzen); Round-Trip-Test der Sub-Tabellen;
  Alt-Datei-ohne-`display_id` → leeres Target, keine „repaired"-Meldung. Der
  Dirty-Ausschluss-Test (`test_recording_preset.cpp:1015-1026`) bleibt inhaltlich gleich,
  nur das Feld heißt jetzt `region_display_id`.

**Schritt 4 — Save-Pfad auf stabile Identität umstellen.**
- **`currentCapturePolicy()` bleibt pur** (kritisch). Sie wird über `captureLiveConfig()`
  bei **jedem** Dirty-/Live-Config-Check aufgerufen (`app/MainWindow.cpp:2511` und von dort
  `2560, 2591, 2608, 2622, 2699, 2706, 2720-2721, 2749, 3951, 4504`) — **nicht** nur bei
  Save/Restore. Deshalb darf `EnumerateDisplayIdentities()` (QueryDisplayConfig + N×
  DisplayConfigGetDeviceInfo + Qt-Join) **nicht** aus `currentCapturePolicy()` laufen.
  Stattdessen: die `StableDisplayId` **einmal bei Selektionsänderung** auflösen
  (`enumerateTargets` / `syncTargetSelectionToCombo`) und im ViewModel cachen
  (`view_model_.selected_display_id` / `…_region_display_id` + normalisierte Region).
  `currentCapturePolicy()` liest nur noch diesen Cache und bleibt frei von Win32/Enumeration.
- Die Selektionsänderung, die den Cache füllt, **löscht** außerdem das `unresolved`-Flag
  (manuelle Wahl ist per Definition resolved; deckt einen Notice-Lebenszyklus-Auslöser).
- **Save-Zeit-Mapping-Fehler:** Kann das aktuell gewählte Target beim Cachen nicht auf eine
  `StableDisplayId` abgebildet werden (Enumeration liefert nichts / kein Join-Treffer), wird
  **mindestens `gdi_name` (+ `seq_hint`)** geschrieben, statt still eine leere Identität
  abzulegen — leere Identität bedeutet „keine Präferenz" und würde den Nutzerwunsch
  verlieren. Beim Restore trägt `gdi_name` dann Stufe 4.
- Tests: ViewModel-/Save-Test mit injiziertem Enumerator (fixe Identitätsliste) →
  korrekte `StableDisplayId` im Cache/Preset; Enumeration-liefert-nichts → `gdi_name`-Fallback
  im Preset, nicht leer; Purity-Regressionstest: `currentCapturePolicy()` löst keine
  Enumeration aus (Enumerator-Call-Count bleibt 0 über N Dirty-Checks).

**Schritt 5 — Restore-Pfad + Re-Resolve-Hook auf Matcher umstellen.**
- `app/pages/RecordPage.cpp:1457-1520`: String-Match durch `ResolveStableDisplay(...)`
  gegen `EnumerateDisplayIdentities()` ersetzen; Ergebnis (matched HMONITOR) auf den
  aktuellen Target-Index abbilden. UNRESOLVED bleibt leere Auswahl (wie heute), setzt
  aber zusätzlich das `unresolved`-Signal für Schritt 6. Kein Auto-Pick bei Nicht-Treffer.
- **Re-Resolve-Hook bei Topologie-Änderung** (deckt „Notice verschwindet, sobald das
  Display zurückkehrt"): `RecordPage::onDisplaysChanged` (`app/pages/RecordPage.cpp:4997-4999`)
  ruft heute nur `enumerateTargets(...)` und matcht die gespeicherte Identität nie neu. Neu:
  Nach der Re-Enumeration die gecachte `StableDisplayId` erneut per `ResolveStableDisplay`
  auflösen. Treffer **und** Nutzer hat seit dem Verlust nicht manuell umgewählt
  (`unresolved`-sticky-Bit) → Auswahl wiederherstellen + Flag löschen. Treffer, aber Nutzer
  hat manuell umgewählt → nur ggf. obsoleten Notice löschen, Auswahl unangetastet. Weiterhin
  Miss → Flag halten. (Diese Neuauflösung nutzt denselben injizierbaren Enumerator und ist
  test-getrieben — sie läuft nur auf dem Snapshot-Trigger, nicht pro Frame.)
- Region-Restore (`1488-1493`): anker-relativ zurückrechnen via Schritt-1-Funktion gegen die
  **physische** Anker-`rcMonitor`; Anker fehlt → Region nicht anwenden + Signal.
- Tests: Restore-Test mit injiziertem Enumerator: (a) exakter Device-Path-Match,
  (b) Panel-per-Serie an neuem Port, (c) Zwillinge ohne Serie → unresolved,
  (d) Region proportional korrekt nach simuliertem Auflösungswechsel (physische Anker-RECT),
  (e) Re-Resolve: Miss → unresolved gehalten; danach Display kehrt zurück (Enumerator liefert
  es wieder) → Auswahl wiederhergestellt + Flag gelöscht; (f) Miss → Nutzer wählt manuell →
  Rückkehr des Alt-Displays überschreibt die manuelle Wahl **nicht**.

**Schritt 6 — Diagnostics-Notice.**
- Neuer Display-Check, der ein gespeichertes-aber-unresolved Target als `Notice`
  (Group `Display`) mit einer `Assisted`-FixAction „Quelle neu wählen" meldet. Quelle
  des Zustands ist das Restore-Ergebnis aus Schritt 5 (Coordinator/RecordPage reicht es
  an die Diagnostics-Seite, analog zum bestehenden `setSelectedCaptureTarget`-Pfad,
  `app/MainWindow.cpp:2676-2680`).
- Der Notice ist eine reine Projektion des `unresolved`-Flags; sein Verschwinden wird von
  den Flag-Löschpfaden aus Schritt 4 (manuelle Neuwahl) und Schritt 5 (Re-Resolve bei
  Display-Rückkehr) getragen — Schritt 6 selbst hält keinen eigenen Zustand.
- Tests: reiner Check-Erzeuger-Test (unresolved-Flag → genau ein Notice mit FixAction;
  resolved/leeres Target → kein Notice).

**Schritt 7 — Doku.**
- `docs/product-spec.md:418-421` (**„Known target-identity boundaries"** — beschreibt die
  GDI-Grenze als sichtbares Verhalten) **MUSS** neu gefasst werden — das ist eine sichtbare
  Verhaltensänderung, und CLAUDE.md verlangt das Spec-Update dann verpflichtend (nicht
  konditional „falls sichtbar beschrieben"; es ist es nachweislich). Neu: stabile Identität
  via Device-Path/EDID, verbleibende Grenze = Zwillinge ohne Serie nach Kabeltausch, Region
  proportional statt pixelgenau.
- `KNOWN_LIMITATIONS.md:246-251` (der „Stable display identity uses the GDI device name…"-
  Absatz) analog neu fassen.
- **ADR-Amends (Historie ehrlich halten):**
  - Neuer ADR unter `docs/decisions/` (Identitätswahl: Komposit-ID primär C angereichert um
    B; Region-Anker-Relativität; physischer Koordinatenraum).
  - `docs/decisions/0003-complete-recording-presets.md` (Zeilen 30, 96, 140 dokumentieren die
    beschreibungsbasierten `display_key`/`region_display_key` und deren Dirty-Semantik) —
    Amend-/Supersede-Notiz auf den neuen ADR.
  - `docs/decisions/0005-reactive-device-discovery.md:76` („stable identity … deferred") —
    Supersede-Notiz (nicht mehr deferred).
- **Referenzen auf die alten Felder nachziehen:**
  `docs/design/exosnap-hybrid-target.md:238` und
  `docs/development/device-discovery-r1.md:121-127` erwähnen `display_key`/
  `region_display_key` bzw. das „deferred"-Framing — auf `display_id`/`region_display_id`
  bzw. den neuen Stand aktualisieren.
- `DisplayDeviceNotifier.h:22-29`-Kommentar entschärfen (Verweis auf den neuen Resolver
  statt „deferred").

Reihenfolge: 1 → 2 parallel zu 1, dann **3+4+5 als eine landende Einheit** → 6 → 7.
Schritt 1 ist der risikoärmste Einstieg; 3–5 sind das Verhaltenszentrum und der einzige
Punkt, an dem das Persistenzformat und beide Pfade zusammen kippen.

## Test-/Verify-Plan

### CI-fähig (deterministisch, ohne echte Hardware)

- **Purer Matcher** (Schritt 1): alle Ranking-Stufen, Zwillings-Matrix, Ambiguitäts-
  Ablehnung, Confidence-Enum.
- **Region-Mathe:** Normalisieren ↔ Zurückrechnen gegen die **physische** Anker-`rcMonitor`,
  Auflösungswechsel, Clamp auf gültige Grenzen, Rundungsverhalten. **Expliziter
  DPI-Skalierungs-Testfall:** Anker mit Skalierung ≠ 100 % (physische `rcMonitor` ≠ logische
  Qt-Geometrie) → Round-Trip physischer Region-Pixel bleibt korrekt (belegt, dass nicht
  versehentlich gegen die logische Geometrie normalisiert wird).
- **Persistenz-Round-Trip** (Schritt 3): TOML schreiben/lesen der Sub-Tabellen;
  Alt-Datei-ohne-`display_id` → leeres Target ohne „repaired".
- **Save/Restore mit injiziertem Enumerator** (Schritte 4/5): fixe Identitätslisten,
  Topologie-Reshuffle simuliert durch geänderte Enumerator-Rückgaben; korrekte
  Auflösung bzw. sauberes UNRESOLVED.
- **Diagnostics-Check** (Schritt 6): unresolved → genau ein Notice + FixAction.

### Nur User-live verifizierbar (nicht in CI)

Die tatsächliche `QueryDisplayConfig`/`DISPLAYCONFIG_TARGET_DEVICE_NAME`-Enumeration und
Qt-EDID-Reads lassen sich in CI nicht fälschen; echte Topologie-Wechsel erst recht nicht.
Der Nutzer muss folgende Fälle einmal real prüfen (Agents fahren die App **nicht**):

1. Zwei Monitore verschiedener Modelle: Display-Target speichern, Monitore
   ab-/anstecken bzw. rebooten mit anderer Reihenfolge → gespeichertes Target trifft
   weiterhin den **richtigen** physischen Monitor.
2. Zwillings-Monitore gleichen Modells **mit** EDID-Serie → korrekte Unterscheidung.
3. Zwillinge **ohne** Serie nach Kabeltausch → **ehrliches** UNRESOLVED + Notice
   (kein stiller Fehlgriff).
4. Region auf Display 2 speichern, Anordnung/Auflösung ändern → Rechteck landet
   proportional richtig; fehlt der Anker → Notice statt Off-Screen-Crop.
5. Gegencheck, dass Preview/HDR/Hub-Keying unverändert funktionieren (Laufzeitpfad
   nicht angefasst).

## Risiken

- **Qt-EDID-Reads unzuverlässig — insbesondere `serialNumber()`:** Das Feld wird heute
  **nirgends im Repo** genutzt, und Qts Windows-QPA befüllt EDID-Serien historisch nicht
  verlässlich. Ist `serial` real immer leer, ist Ranking-Stufe 2 toter Code. Mitigation:
  Serial-Gate-Log (Schritt 2) macht die Belegung sichtbar; Fallback = nativer EDID-Read
  über `device_path`; letzte Konsequenz = Stufe 2 ehrlich streichen. `device_path` (Stufe 1)
  trägt den Match auch ganz ohne EDID-Felder; `manufacturer`/`model` sind reine Anreicherung.
- **Physischer vs. logischer Koordinatenraum bei der Region:** `CaptureRegion` ist physisch,
  `QScreen::geometry()` logisch (DPI-skaliert). Anker-Normalisierung/-Rückrechnung MUSS die
  physische `MONITORINFOEXW.rcMonitor` verwenden; sonst ist der Round-Trip auf skalierten
  Displays falsch. Abgesichert durch einen expliziten DPI-Skalierungs-Testfall (Test-Plan).
- **DisplayConfig-Ausfall (RDP, virtuelle Displays, exotische Treiber):** kein
  Device-Path. Mitigation: Degradation auf `gdi_name`+`friendly_name` (Stufe 3/4) —
  nie schlechter als der heutige String-Match.
- **`monitorDevicePath`-Stabilität über Windows-Builds:** Format ist dokumentiert
  stabil, aber die Annahme „gleicher Port ⇒ gleicher Pfad" sollte im Live-Verify
  bestätigt werden.
- **Breaking Change der Preset-Datei:** bewusst pre-1.0 akzeptiert; einmaliger
  Verlust der gespeicherten Display-Präferenz, still repariert. Kein Datenverlust
  jenseits der Capture-Auswahl.
- **Scope-Kriechen Richtung Fenster-Identität:** bewusst **nicht** Teil dieses Themas
  (`window_key` bleibt beschreibungsbasiert). Nicht mitimplementieren.
- **Enumerationskosten in einem hochfrequenten Pfad:** `currentCapturePolicy()` (der
  Save-Pfad) wird über `captureLiveConfig()` bei **jedem** Dirty-/Live-Config-Check
  aufgerufen, nicht nur bei Save/Restore. `EnumerateDisplayIdentities()` darf deshalb **nicht**
  aus `currentCapturePolicy()` laufen. Mitigation (Schritt 4): `StableDisplayId` einmal bei
  Selektionsänderung auflösen und im ViewModel cachen; `currentCapturePolicy()` bleibt pur.
  Enumeration läuft dann nur bei Selektionsänderung, Restore, dem Snapshot-Re-Resolve und dem
  Diagnostics-Check — nie pro Frame und nie pro Dirty-Check. Ein Purity-Regressionstest
  (Enumerator-Call-Count = 0 über N Dirty-Checks) sichert das ab.

## Offene Fragen

1. **Region-Wiederherstellung — proportional (normalisiert) vs. pixelgenau bei
   gleicher Auflösung?** Der Entwurf wählt proportional (robuster bei Auflösungswechsel).
   Falls pixelgenaue Reproduktion bei identischer Auflösung Vorrang haben soll, müsste
   zusätzlich die absolute Pixel-Region als bevorzugter Pfad gespeichert werden. Echte
   Produktentscheidung.
2. **Zwillinge ohne Serie nach Kabeltausch:** Der Entwurf verweigert das Raten
   (UNRESOLVED + Notice). Alternative wäre ein „bestes Rateergebnis mit Warnung".
   Der ruhige-Diagnostics-Kanon spricht gegen Raten — aber ist das Verweigern die
   gewünschte UX?

## Adversarialer Review — Ergebnis

Alle neun Einwände wurden gegen Code/Docs geprüft und als belegt bestätigt; keiner
zurückgewiesen.

- **Eingearbeitet (major) — Region-Normalisierung physisch vs. logisch:** Bestätigt
  (`RecordPage.cpp:286-287`/`2307-2308` physisch, `DisplayDeviceNotifier.cpp:69` logisch).
  Anker-Geometrie auf physische `MONITORINFOEXW.rcMonitor` festgeschrieben,
  `EnumeratedDisplayIdentity` trägt jetzt `RECT rc_monitor_physical` statt `QRect`;
  DPI-Skalierungs-Testfall ergänzt.
- **Eingearbeitet (major) — Notice-Lebenszyklus ungedeckt:** Bestätigt (`applyCapturePolicy`
  nur aus `applyPresetConfig`; `onDisplaysChanged:4997-4999` matcht nie neu). Re-Resolve-Hook
  (Schritt 5), Flag-Löschung bei manueller Neuwahl (Schritt 4) und die Produktentscheidung
  „Rückkehr stellt Auswahl wieder her, außer der Nutzer hat manuell umgewählt" ergänzt.
- **Eingearbeitet (major) — impure Enumeration im Hochfrequenzpfad:** Bestätigt
  (`captureLiveConfig`→`currentCapturePolicy` bei jedem Dirty-Check, `MainWindow.cpp:2511`
  + 11 Call-Sites). `StableDisplayId` wird bei Selektionsänderung gecacht,
  `currentCapturePolicy()` bleibt pur; Save-Zeit-Mapping-Fehler → mindestens `gdi_name`;
  Risiko-Sektion korrigiert; Purity-Regressionstest ergänzt.
- **Eingearbeitet (major) — Ranking-Stufe 2 an unverifiziertem `serialNumber()`:** Bestätigt
  (`serialNumber` 0 Repo-Treffer). Serial-Gate-Log (Schritt 2), nativer EDID-Fallback über
  `device_path`, und ehrliche Streichung als dritte Option; Stufe 2 als „provisorisch"
  markiert.
- **Eingearbeitet (major) — Schritt 3 keine PR-fähige Einheit:** Bestätigt (Feldentfernung
  bricht `RecordPage.cpp:1390/1398/1459`). Schritte 3–5 als eine landende Einheit
  deklariert; die „jeder Schritt PR-fähig"-Behauptung korrigiert.
- **Eingearbeitet (minor) — Komparator-Charakterisierung falsch:** Bestätigt (Zeile 500 in
  `NormalizedConfigEquals`; `ConfigDirtyEquivalent:766` schließt `capture` komplett aus).
  Ist-Zustand und Schritt 3 präzisiert: `operator==` in `NormalizedConfigEquals`,
  `ConfigDirtyEquivalent` bleibt capture-frei.
- **Eingearbeitet (minor) — Test-Liste unvollständig:** Bestätigt
  (`test_recording_preset_registry.cpp:219`). In Schritt 3 ergänzt.
- **Eingearbeitet (minor) — Doku-Schritt 7 unvollständig/konditional:** Bestätigt
  (`product-spec.md:418-421` beschreibt das Verhalten definitiv). `product-spec.md` als
  Pflicht-Update; ADR 0003/0005-Amends und die zwei Design-/Dev-Docs ergänzt.
- **Eingearbeitet (minor) — Schritt 2 HMONITOR-Quelle offen + Live-Invalidierung nur
  Origin:** Bestätigt (QueryDisplayConfig liefert Pfade, kein HMONITOR;
  `RecordPage.cpp:4958-4965` prüft nur `contains(region_origin)`). `EnumDisplayMonitors`/
  `GetMonitorInfoW`-Join in Schritt 2 ergänzt; Ist-Zustand auf „Origin-Punkt" + physisch/
  logisch-Mismatch korrigiert.

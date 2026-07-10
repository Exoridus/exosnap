# Preset-Vereinfachung — Design

**Datum:** 2026-07-09
**Status:** Entwurf, zur Review

## Problem

Die Preset-Zeile in `ConfigPage` trägt heute sechs Controls (Dropdown, Save, Save As…,
Export, Import, Overflow-Button), einen `· Unsaved`-Indicator und ein Overflow-Menü mit
neun Einträgen. Darüber hinaus existiert ein eigenes `PresetManageOverlay` mit sieben
weiteren Buttons. Ein zweiter, abweichender Einstieg liegt in `OutputPage`.

Der Grund für diesen Aufwand ist das zugrundeliegende Modell: Das *Preset* gilt als
Wahrheit, die Live-Config als flüchtiger Entwurf darauf. Daraus folgen zwingend ein
Save-Button, ein Unsaved-Zustand, Verwerfen-Dialoge und die Rückkopplungs-Guards
`syncing_preset_ui_` / `applying_preset_` im `MainWindow`.

## Zustandsmodell

Das Modell wird umgedreht. **Die Live-Config ist die Wahrheit** und wird immer still
persistiert. Ein Preset ist ein benannter Snapshot, gegen den die Live-Config verglichen
wird.

Weicht die Live-Config vom ausgewählten Preset ab, lautet die Anzeige `Name (changed)`.
Das ist ein Hinweis, keine Warnung: Es gibt keinen Save-Button und keinen
Verwerfen-Dialog, weil zu keinem Zeitpunkt ungesicherte Arbeit existiert. Beim App-Start
wird die Live-Config wiederhergestellt, nicht ein Preset geladen.

Der Vergleich nutzt das vorhandene `ConfigDirtyEquivalent`.

### Umgebungsfelder

`ConfigDirtyEquivalent` klammert `capture` bereits aus, weil Capture-Identität die
*Umgebung* beschreibt und nicht die *Absicht* des Nutzers — sonst würde ein
Monitor-Replug als Änderung gelten.

`bit_depth` und `hdr_mode` gehören in dieselbe Kategorie: Sie beschreiben Anzeige und
Quelle. Daraus folgt:

- Ein Preset-Wechsel überschreibt sie nicht.
- Sie zählen nicht in den `(changed)`-Vergleich.
- Presets setzen sie nicht.

Einzige Ausnahme ist bestehendes Clamping: Beim Wechsel auf einen H.264-Codec erzwingt
`SanitizePresetConfig` 8-bit, weil H.264 kein 10-bit unterstützt.

### Wechsel zwischen Presets

Ein Wechsel überschreibt die Abweichung ohne Rückfrage. Statt eines Dialogs erscheint
eine Notification (`Zu 'Streaming' gewechselt`) mit einer **Rückgängig**-Aktion, die den
vorherigen Live-Zustand und das vorher ausgewählte Preset wiederherstellt.

Dies ist die einzige Stelle im Modell, an der Arbeit verloren gehen kann.

## Ausgelieferte Presets

Vier read-only Built-ins. Sie sind nicht umbenennbar, nicht löschbar, nicht
überschreibbar. `Save as new` ist der Weg, davon abzuleiten.

| Preset | Container | Codec | CQ | NVENC | Charakter |
|---|---|---|---|---|---|
| Default | MKV | AV1 + Opus | 19 | P4 | ausgewogen |
| Quality | MKV | AV1 + Opus | 16 | P6 | maximale Bildschärfe; kostet Platz und GPU |
| Efficiency | MKV | AV1 + Opus | 30 | P6 | kleine Dateien bei brauchbarer Qualität |
| Compatibility | MP4 | H.264 + AAC | 19 | P4 | Schnitt, Upload, GPUs ohne AV1 |

Keins der vier setzt `bit_depth`, `hdr_mode` oder `capture`.

**Begründung der Werte.** CQ läuft von 1 (beste) bis 51; die kanonischen Stufen sind
High = 19, Balanced = 24, Small = 30 (`codec_types.h:40-62`). Default steht bereits auf
19, also am hochwertigen Ende — ein Quality-Preset kann sich deshalb nicht über eine
weitere kanonische Stufe definieren und geht bewusst auf 16. `NearestQualityPreset` zeigt
dafür `High` als nächstliegenden Wert an, ohne exakten Treffer (`IsCanonicalCq`); die
Segment-Anzeige muss Zwischenwerte darstellen können.

`Efficiency` behält P6: Ist das Ziel eine kleine Datei, ist GPU-Zeit die richtige
Währung — man kauft damit Kompression, nicht Qualitätsverlust.

**Kein HEVC-Preset.** HEVC schlägt AV1 weder bei Qualität noch bei Dateigröße; sein
einziger Vorteil ist Kompatibilität, und dort ist H.264 überlegen. Seine echte Nische ist
4:4:4-Screencapture (AV1 kann kein 4:4:4, `RecordingPreset.cpp:211-219`), was per-GPU
gegated und zu speziell für ein Built-in ist.

**Kein HDR-Preset.** Ein Preset, das `hdr_mode` setzt, würde ein Umgebungsfeld
beanspruchen.

**Compatibility adressiert Hardware, nicht Geschmack.** AV1-NVENC verlangt eine RTX 40
oder neuer (`nvenc_encoder.cpp:390-400`). Auf älteren Karten sind Default, Quality und
Efficiency nicht direkt encodierbar.

## Namen

Preset-Namen sind eindeutig. Verglichen wird getrimmt und case-insensitiv, sodass
`streaming` und `Streaming ` nicht koexistieren. Leere Namen sind unzulässig. Die Namen
der vier Built-ins sind reserviert.

Der Namensdialog (bei `Save as new` und `Umbenennen…`) lehnt eine Kollision ab.

Der **Import** lehnt dagegen nie ab, sondern hängt `(2)`, `(3)` … an. Das ersetzt das
heutige Suffix ` (imported)`, welches Kollisionen nur kaschiert hat.

## UI

Die Preset-Zeile in `ConfigPage` schrumpft auf einen Dropdown und ein `…`-Menü.

Zwei Sichtbarkeitsregeln, unabhängig voneinander:

- Bei `(changed)` erscheinen **Save as new** und **Reset**.
- Ist ein eigenes Preset ausgewählt, erscheint **Delete** — unabhängig davon, ob die
  Live-Config abweicht. Ein sauberes eigenes Preset muss löschbar sein.

Ist ein Built-in ausgewählt und die Live-Config unverändert, steht neben dem Dropdown
nichts als das `…`-Menü.

Das `…`-Menü führt vier Einträge: *Save as new…*, *Umbenennen…* (bei Built-ins
deaktiviert), *Exportieren…*, *Importieren…*.

*Save as new* steht damit dauerhaft im Menü und zusätzlich als kontextueller Button, wenn
die Live-Config abweicht. Das ist die Abkürzung für den häufigen Fall und zugleich der
Ersatz für das gestrichene *Duplicate*: Ein unverändertes Preset wird dupliziert, indem
man es auswählt und über das Menü unter neuem Namen speichert.

**Reset** bedeutet immer „zurück zum ausgewählten Preset“ — für Default also zurück zu
den Werkseinstellungen.

`PresetManageOverlay` entfällt vollständig. Damit gibt es keine Ansicht mehr, die alle
Presets nebeneinander zeigt; bei der erwarteten Anzahl genügt der Dropdown. Der zweite
Preset-Einstieg in `OutputPage` wird auf dasselbe Verhalten gezogen, damit nicht zwei
Modelle nebeneinander existieren.

## Persistenz

`presets.toml` erhält eine `[live]`-Tabelle mit der aktuellen Config. `selected_id` bleibt
als Referenz für den `(changed)`-Vergleich.

**`default_id` entfällt.** Der Startup-Default-Stern hatte nur die Aufgabe, beim Start ein
Preset auszuwählen; nun lädt die Live-Config. Das Feld verschwindet aus Registry und
Store, ebenso die Aktion *Set as default preset*.

Schema-Version steigt von 22 auf 23.

### Fehlertoleranz

Statt eines Voll-Resets bei Versions-Mismatch wird feldweise saniert, entsprechend der
Hausregel *clamp rather than reject*: Unbekannte Keys werden ignoriert, fehlende Keys
erhalten ihren Default-Wert. Die gezielte Semantik-Migration `color_range full→limited`
(ADR 0032) bleibt bestehen, weil Clamping sie nicht ersetzen kann.

Drei Fälle:

1. **Die Live-Config ist beschädigt.** Sie wird feldweise saniert. Ist sie unlesbar,
   startet die App auf `Default`, ohne `(changed)`.
2. **Ein einzelnes Preset ist beschädigt.** Es wird feldweise saniert und bleibt erhalten.
   Nur wenn es unlesbar ist, wird es übersprungen (heutiges Verhalten).
3. **Das ausgewählte Preset verschwindet dadurch.** Die Auswahl fällt auf `Default`
   zurück. Die intakte Live-Config wird nun gegen `Default` verglichen und steht damit
   typischerweise auf `(changed)`. Die Einstellungen des Nutzers bleiben erhalten.

Sanierung ist nicht still: Eine Notification meldet, dass Einstellungen zurückgesetzt
wurden.

## Entfallende Elemente

- `app/ui/dialogs/PresetManageOverlay.{h,cpp}` und `app/tests/test_preset_manage_overlay.cpp`
- Toolbar-Buttons *Save*, *Save As…*, *Export*, *Import*; der `· Unsaved`-Indicator
- Fünf der neun Overflow-Einträge; verbleibend nur Save as new/Umbenennen/Export/Import
- *Duplicate preset* — funktional identisch mit `Save as new` auf einem unveränderten Preset
- *Set as default preset* und `default_id_` in Registry und Store
- *New preset from default…* — ersetzt durch Auswahl von `Default` plus `Save as new`
- *Reset all presets to factory defaults…*
- `ExportAllUserPresetsToFile` — Export arbeitet auf dem ausgewählten Preset
- Die Guards `syncing_preset_ui_` / `applying_preset_` im `MainWindow`, sofern das Anwenden
  eines Presets nicht mehr über Save-Pfade zurückschlägt

Das Flag `ProfileOption.built_in` wird **nicht** entfernt, sondern aktiviert:
`MainWindow::refreshPresetUi()` setzt es heute pauschal auf `false`. Der zugehörige
Read-only-Schutz in `ConfigPage::updatePresetActionState` existiert bereits und muss um
eine Durchsetzung in `RecordingPresetRegistry` (Save/Rename/Delete) ergänzt werden.

## Tests

- `test_recording_preset_store.cpp`: Roundtrip der `[live]`-Tabelle; Wegfall von
  `default_id`; feldweise Sanierung statt Voll-Reset bei Schema-Mismatch; unlesbare
  Live-Config führt auf `Default`.
- `test_recording_preset_registry.cpp`: Read-only-Schutz der vier Built-ins gegen Save,
  Rename und Delete; Auswahl fällt auf `Default` zurück, wenn das ausgewählte Preset
  fehlt.
- Neu: Namens-Eindeutigkeit (getrimmt, case-insensitiv, leer unzulässig, Built-in-Namen
  reserviert) und Import-Suffix `(2)`, `(3)`.
- `test_recording_preset.cpp`: `bit_depth` und `hdr_mode` bleiben beim Anwenden eines
  Presets unangetastet und zählen nicht in `ConfigDirtyEquivalent`.
- `test_config_page.cpp`: *Save as new* und *Reset* erscheinen nur bei `(changed)`;
  *Delete* erscheint bei jedem eigenen Preset, auch unverändert, und fehlt bei Built-ins;
  *Save as new…* ist im `…`-Menü dauerhaft erreichbar.
- Neu: Der Undo-Pfad des Preset-Wechsels stellt Live-Config und Auswahl wieder her.

## Abgegrenzt

Eine ruhige Empfehlung in der `RecommendationEngine` (analog `rec.profile.codec`), wenn
10-bit bei 8bpc-Anzeige aktiv ist. Kein Blocker, kein Startup-Popup — 10-bit auf einem
SDR-Panel ist Verschwendung, kein Fehler. Eigenständiges Stück, nicht Teil dieser Spec.

## Spec-Auswirkung

`docs/product-spec.md` beschreibt heute einen *preset manage dialog* mit rename,
duplicate, delete und set-default. Dieser Abschnitt ist zu ersetzen. Die
Default-Preset-Tabelle bleibt gültig; die drei zusätzlichen Built-ins kommen hinzu.

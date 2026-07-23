# Deutsche Lokalisierung (L-8 + 1.0-Slice): tr()-Sweep + Qt-Linguist-Infrastruktur

> Status: Umsetzungsreife Spec. Autor read-only bzgl. Code. Umsetzung später durch andere Agenten
> ausschließlich anhand dieser Spec. Checkout: `main` @ #192.
> Verwandt: `project_localization_de_10` (Produktentscheidung), `feedback_codec_naming_canon`
> (CodecLabels-Kanon), Review-Finding **L-8** (`.workspace/review-fable-2026-07-10.md:152`).

---

## Problem

Deutsch ist für 1.0 beschlossen. Der Code trägt heute **null** Lokalisierungs-Infrastruktur:
keine `Qt6::LinguistTools`, keine `.ts`/`.qm`, kein `QTranslator`, kein `qt_add_translations`
(verifiziert: `find_package(Qt6 ... Core Gui Widgets Svg)` in `app/CMakeLists.txt:8` — LinguistTools
fehlt; die einzigen `.qm`/`__QT_DEPLOY_I18N_CATALOGS`-Treffer stammen aus dem `build/`-Baum bzw.
PresentMon-Fremdcode). Gleichzeitig sind User-Strings über **drei unterschiedliche Schichten**
verstreut, jede mit einem anderen Anti-Pattern gegen Übersetzbarkeit:

1. **UI-Widgets** setzen Text als `setText(QStringLiteral("…"))` statt `tr()` — obwohl alle Pages
   `QObject`/`QWidget`-Subklassen sind und `tr()` sofort verfügbar wäre.
2. **Service-/ViewModel-Schicht** (`RecordingCoordinator`, `RecordViewModel`) baut User-Prosa als
   **`std::wstring`** — komplett außerhalb der Qt-Übersetzungsmaschinerie. Das ist der Kern von L-8.
3. **Diagnostics-Resolver** (`RecommendationEngine`) ist bewusst **Qt-frei** und produziert englische
   `std::string`-Prosa; die **Engine** (`libs/capability`) liefert zusätzlich englische `reason`/
   `message`-Strings, die die `DiagnosticsPage` **wörtlich** an den User durchreicht.

Ohne eine klare Schichtenentscheidung führt „einfach überall `tr()`" dazu, dass Qt in reine,
testbare Resolver gezogen wird (bricht die Qt-freien Unit-Tests) und die Engine ihre UI-Agnostik
verliert. Die Spec legt fest, **welche Schicht welchen String trägt**, wie die Linguist-Pipeline
aussieht, und was bewusst **unübersetzt** bleibt.

---

## Ist-Zustand (frisch erhoben, mit Datei:Zeile)

### Schicht A — UI-Widgets (QObject-Kontext vorhanden, aber kein tr())

- Alle Seiten sind `QWidget`-Subklassen: `AboutPage`, `ConfigPage`, `DevicePage`, `DiagnosticsPage`,
  `EditExportPage`, `HotkeysPage`, `LogsPage`, `OutputPage`, `RecordPage`, `WebcamPage`
  (`app/pages/*.h`). `tr()` ist damit in jeder Seite direkt verfügbar.
- User-Text wird durchweg als `setText(QStringLiteral("…"))` gesetzt, z. B.
  `RecordPage.cpp:768` `setText(QStringLiteral("Recent"))`,
  `:1150` `"Could not rename the file."`,
  `:3221` `"Frame saved: %1"` (`.arg(name)`),
  `:4766` `"No frames dropped"`, `:4776` `"Peak A/V drift: ±%1 ms"`, `:4778` `"A/V drift: unavailable"`.
- Rohe Größenordnung: **~4197** `QStringLiteral`-Vorkommen in `app/` (ohne `tests/`). Das ist eine
  **Mischung** aus User-Text, `objectName`/`setObjectName`, Log-Kategorien/-Nachrichten,
  Perf-Marker und QSS-Tokens — **nicht** 4197 zu übersetzende Sätze. Der übersetzbare Anteil ist
  eine Teilmenge (grob: die `setText`/`setToolTip`/`setPlaceholderText`/`addItem`-Aufrufe der Pages
  summieren sich allein auf ~351 Aufrufe in `app/pages/`). Eine exakte Zählung ist bewusst nicht
  Teil dieser Spec (siehe „repräsentative Stichprobe"); `lupdate` liefert die belastbare Zahl beim
  ersten Lauf.

### Schicht B — Service / ViewModel (std::wstring-Prosa, das L-8-Kernproblem)

- `RecordingCoordinator::BuildCapabilityStatusText` (`app/services/RecordingCoordinator.cpp:249-299`)
  baut `std::wstring` `L"Ready: "` + Container/Codec/Rate (`:289`). Es **dupliziert** außerdem die
  Codec-Schreibweisen (`L"MKV"`, `L"AV1 NVENC"`, `L"Opus"`, `:250-287`) statt `CodecLabels.h` zu
  nutzen — ein latenter Drift gegen den Naming-Kanon, den die Umsetzung mitheilen sollte.
- `RecordingCoordinator::FormatErrorPhase` (`:2317-2340`) gibt englische `std::wstring` zurück
  (`L"Prepare"`, `L"Video Capture"`, `L"Video Encoder"`, `L"Audio Capture"`, `L"Audio Encoder"`,
  `L"Mux"`, `L"Finalize"`, `L"Shutdown"`, `L"Unknown"`) — direkt aus dem `recorder_core::ErrorPhase`-
  Enum abgeleitet.
- `CapabilityStatusText()` liefert `const std::wstring&` (`:1857`); Fehlertexte werden als
  `std::wstring` gebaut: `:518`/`:549` `L"Recording unavailable"`, `:718`
  `L"Failed to create output directory: " + …`.
- `RecordViewModel` (`app/viewmodels/RecordViewModel.cpp:436`) baut ebenfalls `std::wstring`:
  `L"Recording succeeded"` / `L"Recording failed"`, und trägt `result_error_phase`/
  `result_error_detail` als `wstring` weiter (`:438`, `:440`). (Hinweis: `tr(`-Treffer in dieser
  Datei sind ausschließlich `substr(`-Falschtreffer — keine echte Übersetzung vorhanden.)
- Der `error_message`-Mapper (`app/diagnostics/error_message.h`) hat bereits die **richtige Form**
  (App-Schicht mappt Ergebnis → User-Text), aber den falschen Typ: `struct UiErrorMessage {
  std::wstring title; std::wstring message; std::wstring action_hint; }` + `MapErrorToUserMessage(
  const UiRecordingResult&)` — englisch, `std::wstring`, kein `tr()`.

### Schicht C — Diagnostics-Resolver (Qt-frei, stabile Codes) + Engine-Prosa

- `RecommendationEngine` (`app/diagnostics/RecommendationEngine.{h,cpp}`) ist **Qt-frei** (verifiziert:
  keine `#include <Q…>`, kein `QString`/`QObject`). Er produziert `DiagnosticResult` (reine
  `std::string`) mit **stabilen Codes** als natürliche Übersetzungsschlüssel: `id` wie `"rec.003"`,
  `"rec.008"` und `FixAction::id` wie `"fix.container.mkv"`. Beispiel `checkCodecAvailability`
  (`:196-233`): englischer `title`/`summary`/`recommendation` + `FixAction{id,label,changes_summary}`,
  und im `detail` wird **Engine-Prosa eingebettet**: `"… Reason: " + v_ann.reason` (`:203`, `:222`).
- Modell `DiagnosticResult` (`app/diagnostics/DiagnosticResult.h:51-63`): `std::string id, title,
  summary, detail, current_value, recommendation` + `std::optional<FixAction>` (`FixAction{std::string
  id, label, changes_summary}`, `:37-49`). Alles englisch, `std::string`.
- Die `DiagnosticsPage` reicht diese `std::string` **verbatim** an den User durch:
  `DiagnosticsPage.cpp:1085` `new QLabel(QString::fromStdString(result.title))`, `:1091` `summary`,
  `:1097` `detail`, `:1166`/`:1176` `fix->label`, `:1184` `fix->label`.
- **Engine liefert User-Prosa** (Verletzung der UI-Agnostik, sobald sie angezeigt wird):
  `libs/capability/include/capability/resolver.h` — `struct Warning { std::string code; std::string
  message; }` (`:20-23`, **hat** bereits einen Code), `struct InvalidReason { std::string field;
  std::string message; }` (`:25-28`, **kein** Code), `struct Adjustment { …; std::string reason; }`
  (`:13-18`). `libs/capability/include/capability/support_level.h:14-17` — `struct SupportAnnotation
  { SupportLevel level; std::string reason; }` (**kein** Code).
  Die `DiagnosticsPage` zeigt `InvalidReason.message` (`:1201`) und `Warning.message` (`:1220`)
  wörtlich an. `libs/recorder_core` trägt zusätzlich englische Fehlertexte (`recorder_session.cpp`,
  `mux_thread.cpp`, `*_meter_service.cpp`) — diese sind überwiegend **log-/debugnah** und laufen als
  `error_detail` in die UI (siehe unten „Was bleibt unübersetzt").

### Schicht D — Benachrichtigungen (Enum-getrieben, gute Ausgangslage)

- `app/notifications/NotificationEvent.h`: `enum class NotificationType` (**11** Werte: `LowStorage`,
  `Saved`, `UnexpectedStop`, `RecoveryAvailable`, `UpdateAvailable`, `FramesDropped`,
  `SettingsRepaired`, `PresetSwitched`, `OverlayOmitted`, `HotkeyConflict`, `SettingsSaveFailed`;
  `NotificationEvent.h:13-25`) und `enum class NotificationAction` (12 Werte — nur dieses Enum hat 12).
  `NotificationEvent` trägt bereits `QString title;
  QString body;`. Die konkreten Titel/Bodies werden **an den Wiring-Sites in `MainWindow.cpp`**
  gebaut (nicht in dieser Datei) — enum-getrieben und damit ideal für eine app-seitige tr()-Mappe.

### Kanon / Infrastruktur-Umfeld

- Kanon der sichtbaren Codec/Container/Format-Labels (`MKV`, `MP4`, `WebM`, `H.264`, `HEVC`, `AV1`,
  `Opus`, `AAC`, `PCM`, `FLAC`) plus `frameRateLabel` („%1 fps") und `resolutionLabel`. **Korrektur:**
  `app/ui/CodecLabels.h` ist **nicht** die einzige Quelle. Für **Video-Codecs** lebt der Kanon
  Qt-frei in `libs/capability`: `VisibleVideoCodecLabel(VideoCodec)` (`libs/capability/src/codec_selection.cpp:32`,
  Header `codec_selection.h:39`) liefert `"AV1"`/`"HEVC"`/`"H.264"` als `std::string_view`;
  `CodecLabels.h:50-55` (`videoCodecLabel`) **delegiert** nur dorthin, und `RecommendationEngine.cpp:262-263`
  nutzt den Pure-Kanon bereits direkt. Für **Container** (`videoContainerLabel`, `CodecLabels.h:40-48`)
  und **Audio** gibt es dagegen **keinen** Qt-freien Pure-Kanon — dort ist `CodecLabels.h` die Quelle.
  **Nützlich für Schicht C:** weil der Video-Codec-Kanon Qt-frei ist, kann der Resolver Codec-Namen
  kanonisch liefern, ohne Qt zu ziehen. Casing-Kanon im Header (`CodecLabels.h:13-22`).
- Einstiegspunkt: `app/main.cpp:117` `QApplication app(argc, argv);`, `:135`
  `setApplicationName("ExoSnap")`, Theme wird bei `:160` gesetzt. Kein Translator installiert.
- `.qrc` vorhanden: `app/assets/brand/…`, `app/assets/fonts/…`, `app/ui/theme/…` — ein zusätzliches
  `translations.qrc` bzw. das von `qt_add_translations` erzeugte Ressourcen-Embedding fügt sich ein.
- `windeployqt` setzt bereits `__QT_DEPLOY_I18N_CATALOGS "qtbase"` (Deploy-Cmake im `build/`-Baum) —
  d. h. Qt-eigene Standarddialog-Übersetzungen können mitdeployt werden.
- Build-Preset: `windows-x64-debug` (VS-Tree; s. Memory `project_build_env_vs_tree`).

---

## Design

### Leitentscheidung: „Engine liefert Codes/Enums, App-Schicht übersetzt"

Diese eine Regel löst alle vier Schichten konsistent:

- **Engine (`libs/*`) übersetzt nie und trägt keine anzuzeigende Prosa als primäre Quelle.** Sie
  liefert stabile Enums/Codes; wo heute schon englische `reason`/`message` existieren, bleiben sie
  als **Entwickler-/Log-Fallback** erhalten, werden aber nicht mehr als primärer User-Text
  angezeigt. Engine bleibt Qt-frei.
- **App-Schicht ist die einzige Übersetzungsschicht.** UI-Widgets nutzen `tr()`. Nicht-Widget-App-
  Code (Coordinator, ViewModel, Diagnostics-Presentation, Notification-Wiring) nutzt
  `QCoreApplication::translate("<Kontext>", "…")` mit einem stabilen Kontext-String — das braucht
  **kein** `QObject` und zieht Qt **nicht** in die reinen Resolver.
- **Reine Qt-freie Resolver** (`RecommendationEngine`, `libs/capability`) bleiben Qt-frei: sie
  liefern Codes; die Übersetzung passiert in einer dünnen **App-Presentation-Mappe**.

Damit ist die Antwort auf die vier Teilfragen:

| Schicht | Träger heute | Zielträger | Übersetzungsmechanismus |
|---|---|---|---|
| A UI-Widgets | `QStringLiteral` | `tr()` | `lupdate` extrahiert aus `tr()` |
| B Coordinator/ViewModel | `std::wstring`-Prosa | strukturierte Enums + App-Presentation | `QCoreApplication::translate` in Presentation-Helfern, `QString` statt `wstring` |
| C Diagnostics-Resolver + Engine-Prosa | `std::string`-Prosa / Engine-`reason` | stabile Codes (bleiben) + App-Presentation-Mappe keyed auf `id`/Enum | `QCoreApplication::translate` keyed auf Code |
| D Notifications | Enum + `QString` an Wiring-Site | Enum → App-tr()-Mappe | `tr()`/`translate` in einem `NotificationText`-Helfer |

### Alternative 1 (Schicht B/C): „Qt in die Resolver ziehen und überall tr()"

Coordinator zu `QObject` machen, `RecommendationEngine` `QCoreApplication::translate` intern rufen
lassen, Engine-`reason` per `tr()` übersetzen.

- **Pro:** Ein Mechanismus überall; keine zweite Presentation-Schicht.
- **Contra:** Bricht die bewusst **Qt-freien Unit-Tests** von `RecommendationEngine` und der
  `libs/capability`-Resolver (die heute ohne `QApplication` laufen; s. Memory
  `feedback_gtest_isolation_qapplication`). Verletzt die Guardrail „Engine bleibt UI-agnostisch"
  direkt. `translate()` in der Engine bindet zudem die Textkatalog-Verantwortung an eine Lib, die
  auch von Probes/CLI genutzt wird. **Abgelehnt.**

### Alternative 2 (Schicht C): „Engine-`reason` als vollen Satz lassen, nur UI-Rahmen übersetzen"

Nur `title`/`summary` übersetzen, das eingebettete `reason`/`detail` englisch lassen.

- **Pro:** Minimaler Aufwand.
- **Contra:** Halbdeutsche Diagnose-Karten („Ausgewählter Video-Codec nicht verfügbar. Codec: …
  Reason: HEVC requires driver ≥ …") sind schlechter als konsequent englisch. Der `reason` ist
  genau der informationstragende Teil. **Abgelehnt** als Endzustand; als **Zwischenstufe** in der
  Phasierung akzeptabel (detail/current_value zuletzt).

### Entscheidung Schicht C — gewählt: „Stabiler Code bleibt in der Engine, Text in der App"

- Der Resolver bleibt Qt-frei und behält seine englischen `std::string` als **Fallback** (nützlich
  für Logs/Support und als Default, falls ein Code keine Übersetzung hat).
- Eine neue, dünne **App-Presentation-Schicht** `diagnostics::DiagnosticText` (Qt, `QString`) mappt
  `DiagnosticResult.id` → übersetzten `title`/`summary`/`recommendation` und `FixAction.id` →
  übersetztes `label`/`changes_summary` via `QCoreApplication::translate("Diagnostics", …)`. Findet
  die Mappe keinen Eintrag, fällt sie auf den englischen Resolver-String zurück (nie leer).
- Für die **dynamischen** Teile (`detail`/`current_value` mit eingebettetem Engine-`reason`,
  Codec-Namen, Zahlen): der Resolver liefert die **Bestandteile strukturiert** (Codec-Enum,
  Zahlenwert) statt vorformatierter Prosa, wo er heute konkateniert; die App formatiert mit
  `CodecLabels` + `translate` + `QLocale`. Wo das zu teuer ist, bleibt der englische Reason als
  sekundäres Detail sichtbar (ehrlich, statt Pseudo-Übersetzung).
- **Engine-Ergänzung:** `InvalidReason` und `SupportAnnotation` bekommen ein `std::string code`
  (bzw. ein Enum) analog zu `Warning.code`, damit die App auf den Code statt auf englischen Fließtext
  mappen kann. `message`/`reason` bleiben als Fallback. Pre-1.0: kein Schema-Migrationsbedarf.
- **Coverage-Lücke schließen (adversarialer Minor).** Der Fallback „unbekannter Code → englischer
  Resolver-String" ist bequem, aber **still**: ein neuer `DiagnosticResult.id`/`FixAction.id` im
  `RecommendationEngine` (jede `MakeResult(...)`-Site, z. B. `rec.003`/`rec.004` in
  `RecommendationEngine.cpp:200-229`) ohne `DiagnosticText`-Eintrag rutscht **unbemerkt** auf Englisch
  durch. Das `lupdate`-Drift-Gate fängt das **nicht** (es sieht nur `translate()`-Literale in der Mappe,
  nicht fehlende IDs), und ein Test „jeder bekannte `id` → nichtleer" ist zirkulär, wenn seine ID-Liste
  die Mappe selbst ist. **Fix:** eine **zentrale ID-Enumeration am Resolver** — der `RecommendationEngine`
  (bzw. ein Test, der alle `MakeResult`-/`FixAction`-IDs aufzählt) liefert die Kanon-Liste; ein Test
  prüft, dass die `DiagnosticText`-Mappe **für jede** dieser IDs einen Eintrag hat (fehlt einer → rot).
  So testet das Gate echte Vollständigkeit, nicht die Mappe gegen sich selbst.
- **Bewusste Prosa-Duplikation explizit machen (adversarialer Minor).** Die englische Prosa existiert
  dann **zweimal**: einmal als Resolver-Fallback (`RecommendationEngine.cpp:200-229`) und einmal als
  `translate()`-Quell-Literal in der Mappe. Das ist gewollt (Engine bleibt Qt-frei, App trägt den
  Katalog), aber driftgefährdet. Umgang: der Resolver-Fallback-Text ist bewusst **nur** Log-/Support-Default
  und muss dem Mappe-Quelltext **nicht** wortgleich folgen; die Mappe ist die einzige **angezeigte** Quelle.
  In der Review-Checkliste festhalten, dass Änderungen am angezeigten Wortlaut in der **Mappe** passieren,
  nicht am Resolver-Fallback.

### Entscheidung Schicht B — gewählt: „Coordinator/ViewModel hören auf, Prosa zu bauen"

- `BuildCapabilityStatusText` wird **entfernt** bzw. auf einen reinen Datenlieferanten reduziert: der
  Coordinator exponiert die strukturierten Felder. **Korrektur (adversarialer Minor):** einen
  öffentlichen `resolved_config`-Accessor gibt es **heute nicht** — `resolved_user_config_` und
  `validation_result_` sind **private** Member (`RecordingCoordinator.h:333-334`); öffentlich ist nur
  `CapabilityStatusText()` (`:192`, liefert `const std::wstring&`) und `ResolvedVideoCodecLabel()`
  (`:193`). Die Umsetzung **definiert daher einen neuen Accessor** (z. B.
  `const capability::UserRecorderConfig& ResolvedConfig() const` bzw. eine schlanke
  Ready-Daten-Struktur), aus dem die UI den „Ready: MKV · AV1 · Opus · 60 fps"-String via
  `CodecLabels` + `tr("Ready: %1")` + `QLocale` baut. Das behebt zugleich den Codec-Schreibweisen-Drift.
- **Blocked-Pfad separat behandeln (adversarialer Minor):** Schritt 5 deckte nur den Ready-Pfad ab.
  Der Blocked-Pfad setzt `capability_status_text_` heute auf `L"Recording unavailable"` bzw.
  `ToWide(validation.invalidity.front().message)` (`RecordingCoordinator.cpp:517-518`, ebenso der
  Revalidate-Pfad `:546-550`, und `OnCapabilityFailure` `:528`). Dafür braucht die UI **nicht**
  `resolved_config`, sondern die **Invalidity** (`InvalidReason` mit dem in Schritt 6 ergänzten
  `code`); der leere Fall wird zu `translate("RecordStatus", "Recording unavailable")`.
- **Threadsichere Übergabe klären:** heute wird der fertige String **zusammen mit `PostStateChange`**
  auf den UI-Thread gepostet. Die strukturierten Felder (resolved_config bzw. `InvalidReason.code`)
  müssen genauso konsistent zur UI gelangen — entweder als Snapshot **im** `PostStateChange`-Payload
  transportiert oder unter derselben Sperre gelesen, die die Coordinator-Member schützt (kein
  nachträglicher, race-anfälliger Pull vom UI-Thread).
- `FormatErrorPhase` verliert die englische Prosa: der Coordinator/ViewModel gibt das
  `recorder_core::ErrorPhase`-**Enum** an die UI; ein App-Presentation-Helfer
  `RecordText::errorPhaseLabel(ErrorPhase)` liefert den übersetzten Text via `translate`.
- `UiErrorMessage`/`MapErrorToUserMessage` wird von `std::wstring` auf `QString` umgestellt und nutzt
  `translate("RecordError", …)`. Das ist die **richtige** Stelle für Fehler-Mapping (bleibt App,
  wird nur übersetzbar).
- `RecordViewModel`: `result_status_text` etc. werden nicht mehr als `L"Recording succeeded"` gebaut,
  sondern als `UiRecordingState`/Erfolgs-Bool weitergegeben; die UI übersetzt.
- Ergebnis: die `std::wstring`-Prosa-Pfade verschwinden; `QString::fromStdWString`-Konvertierungen an
  den Anzeige-Sites entfallen.

### Entscheidung Schicht A — gewählt: „`tr()`-Sweep, aber gefiltert und phasenweise"

- Reine `QStringLiteral`→`tr()`-Umstellung **nur für echten User-Text**. Ausdrücklich **nicht**
  angefasst: `objectName`/`setObjectName`, Log-Kategorien und `AppLog`-Nachrichten, QSS-Token,
  Perf-Marker, Dateipfade/Tokens, `CodecLabels`-Rückgaben (s. u.).
- `tr()`-Kontext = Klassenname (Qt-Default). Pre-1.0 ohne Kompat-Zwang: Umziehen von Strings zwischen
  Klassen invalidiert Übersetzungen folgenlos.
- Konkatenierte Laufzeit-Strings (`"A " + x + " B"`) werden zu **vollständigen** `tr("A %1 B").arg(x)`
  umgebaut, sonst kann `lupdate` sie nicht extrahieren und die deutsche Wortstellung ist nicht
  abbildbar.

### Qt-Linguist-Infrastruktur (CMake / Laden / Sprachwahl)

**Build-Pipeline (CMake):**
1. `find_package(Qt6 … COMPONENTS … LinguistTools)` in `app/CMakeLists.txt:8` ergänzen.
   **KRITISCH — CI-Skip-Falle (adversarialer Blocker):** `app/CMakeLists.txt:8` ist heute
   `find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Svg)`, und `:10-14` ersetzt bei `NOT Qt6_FOUND`
   das **gesamte** `exosnap`-Target durch ein Echo-Dummy (`add_custom_target … echo … return()`).
   Alle CI-Qt-Installationen laden nur `archives: 'qtbase qtsvg'` (`ci.yml:77`, `:217`, `:298` und
   `release-candidate.yml:32`; ebenso `crash-capture-build.yml:60`) — **`qttools` (lupdate/lrelease +
   das CMake-Paket `Qt6LinguistTools`) fehlt dort.** Ergänzt man LinguistTools naiv als COMPONENT,
   wird `Qt6_FOUND` in CI **FALSE**, der Dummy greift, und **CI bleibt grün, während App + alle
   App-Tests nicht mehr gebaut werden** (stiller Totalausfall). Deshalb sind hier **drei** Dinge Pflicht,
   nicht optional:
   - (a) `qttools` in **alle vier** Install-Qt-Steps aufnehmen: `archives: 'qtbase qtsvg qttools'` in
     `ci.yml:77/:217/:298` und `release-candidate.yml:32` (und, falls dort später App/Tests gebaut
     werden, `crash-capture-build.yml:60`).
   - (b) LinguistTools **hart** (nicht QUIET/optional) auffinden, damit ein fehlendes qttools laut
     bricht statt still zu skippen — z. B. ein separates `find_package(Qt6LinguistTools REQUIRED)`
     **nachdem** `Qt6_FOUND` bestätigt ist (also nach dem bestehenden Dummy-Guard, damit ein reiner
     „kein Qt"-Build weiterhin sauber skippen darf), **oder** LinguistTools als Component **und**
     zusätzlich einen CI-Artefakt-Check (Schritt-Ende: `exosnap.exe` muss existieren) einführen.
   - (c) Der CI-Build-Job bekommt einen expliziten **Artefakt-Assert** auf `exosnap.exe` (bzw. ein
     App-Test-Binary), sodass das Skip-Muster nie wieder unbemerkt greifen kann.
   Lokal ist die Entwickler-Qt (`C:/Qt/6.9.0/msvc2022_64`, `project_build_env_vs_tree`) mit qttools
   ausgestattet; „Build grün" in dieser Sektion ist daher **ohne (a)** nur lokal aussagekräftig.
2. `qt_add_translations(exosnap TS_FILES i18n/exosnap_de.ts LUPDATE_OPTIONS -locations none)`
   (Qt ≥ 6.7 — im Repo ist 6.9, `project_ui_framework`). **Pfad-Korrektur:** `TS_FILES` ist relativ
   zu `CMAKE_CURRENT_SOURCE_DIR` (= `app/`), also `i18n/exosnap_de.ts` — **nicht** `app/i18n/…`
   (das zeigte auf `app/app/i18n/…`). **`-locations none` ist Pflicht (adversarialer Minor):** ohne
   das schreibt `lupdate` `<location line="…"/>`-Einträge in die eingecheckte `.ts`, sodass **jede**
   Codeverschiebung oberhalb eines `tr()`-Aufrufs die `exosnap_de.ts` diffiert, obwohl kein String sich
   geändert hat → `.ts`-Churn in praktisch jeder UI-PR und ein Drift-Gate (Schritt 9), das
   Zeilennummern statt Strings testet. `qt_add_translations` erzeugt automatisch `update_translations`-
   und `release_translations`-Targets (intern `lupdate`/`lrelease`) und **embeddet** die `.qm` als
   Ressource unter `:/i18n/` in das `exosnap`-Target (**diese** `.qm` shippt korrekt in ZIP **und**
   MSI, weil sie in die `exosnap.exe` eingebettet ist — nicht betroffen vom `translations/`-Prune,
   s. Schritt 4). Die Quelle (`en`) ist der Code selbst — keine `exosnap_en.ts` nötig
   (Fallback = Original-`tr()`-Text).
3. Ablage: `app/i18n/exosnap_de.ts` wird **eingecheckt** (kuratiert, tracked). Die generierte
   `.qm` ist Build-Artefakt (untracked).
4. Qt-Standarddialoge: `qtbase_de.qm` über den bestehenden `__QT_DEPLOY_I18N_CATALOGS`-Deploy
   mitliefern; zur Laufzeit aus `QLibraryInfo::path(TranslationsPath)` bzw. dem App-Verzeichnis laden.
   **ACHTUNG Release-Prune-Kollision (adversarialer Major):** `windeployqt` legt `qtbase_de.qm` in ein
   `translations/`-Verzeichnis neben der `exe`. Genau dieses Verzeichnis **löscht** die Release-Pipeline
   heute explizit: `scripts/build-release-artifacts.ps1:621-627` prunt `translations/` mit dem Kommentar
   „no i18n in MVP", und ADR 0036 (`docs/decisions/0036-msi-auto-harvest-staging-tree.md:19-20`)
   dokumentiert diese Prune-Liste; das MSI wird aus **demselben gepruneten** Staging-Tree geharvestet.
   Ohne Anpassung shippt der Standarddialog-Katalog also **weder** im Portable-ZIP **noch** im MSI
   (deutsche App + englische Datei-/Message-Dialoge). Behebung siehe **Schritt 11** unten (eigener
   Implementierungsschritt: Prune so ändern, dass `qtbase_de.qm` erhalten bleibt bzw. gezielt in den
   Staging-Tree kopiert wird, **und** ADR 0036 fortschreiben). Nur die App-`.qm` aus Schritt 2 ist von
   diesem Prune unberührt, weil sie eingebettet ist.

**Laden (`app/main.cpp`, nach `QApplication`-Konstruktion `:117`, vor `MainWindow`):**
5. Effektive Locale bestimmen (s. Sprachwahl). Wenn Deutsch: `QTranslator` `exosnap` aus `:/i18n/`
   laden + `installTranslator`; zusätzlich `QTranslator` `qtbase_de` aus dem Qt-Translations-Pfad.
   Beide Translator als statische/gehaltene Objekte (Lebensdauer = App). Reihenfolge: App-Translator
   nach Qt-Translator installieren (letzter gewinnt bei Kollision — hier kollisionsfrei).
6. `QLocale::setDefault(effectiveLocale)` **vor** dem Bau der ersten Widgets setzen, damit `QLocale`-
   Zahl/Datum-Formatierung konsistent ist.

**Visual-Test-Harness: Locale deterministisch pinnen (adversarialer Major — Teil von Schritt 1):**
`--visual-test` läuft durch die **echte** `main()` (`app/main.cpp:117-133`, Kommentar „runs the real
application"). Sobald Schritt 1+2 einen Translator installieren und Default `System` gilt, würde der
Harness auf dem **deutschen** Entwickler-Windows deutsch rendern — heute existiert **kein einziger**
`QLocale`/Translator-Override in `app/` (verifiziert: 0 `QLocale`-Treffer). Damit wären Renders
maschinenabhängig und die Aussage „`--visual-test`-Render unverändert im Layout" auf der primären
Verifikationsmaschine falsch (visuelle Verifikation ist Pflicht, `feedback_visual_verification`).
**Pflicht:** Der Harness **pinnt die Locale deterministisch**, unabhängig von der System- und der
gespeicherten Einstellung:
- Der Locale-Auswahlpfad in `main.cpp` (Schritt 5/7) muss die Visual-Test-Anfrage
  (`exosnap::visual::HasVisualTestRequest(...)`, existiert bereits, `main.cpp:126`) berücksichtigen und
  im Harness-Modus **`en_US` als Default** erzwingen (nicht `QLocale::system()`), so wie der Harness
  schon heute den Config-Dir isoliert.
- Für die deutschen Clipping-/Wrap-Checks (Risiko „Layout-Regression") ein **expliziter Schalter**
  `--visual-test-locale=de` (oder ein Feld in `VisualTestOptions`, `ParseVisualTestOptions`), der den
  deutschen Katalog gezielt lädt. So sind beide Locales reproduzierbar renderbar, ohne von der
  Host-Sprache abzuhängen.

**Sprachwahl:**
7. Persistierter Enum `LanguageSetting { System, English, German }` im Settings-Store (TOML;
   pre-1.0 reset statt Migration). Default **`System`** → `QLocale::system()`; ist die System-UI-
   Sprache Deutsch, wird Deutsch geladen, sonst Englisch (Quellsprache).
8. UI: eine Sprach-Auswahl in Settings (Sektion **Advanced** oder ein kleiner „General/Language"-
   Block — Platzierung ist offene Produktfrage). Umschalten **wirkt nach Neustart** (siehe Risiko/
   offene Frage zu Live-`retranslateUi`); die UI zeigt einen ruhigen Hinweis „wirkt nach Neustart".

### Plural- / Format-Fallen

- **Plurale:** `tr("%n file(s)", "", n)` / `QCoreApplication::translate(ctx, "%n …", nullptr, n)`.
  `.ts` trägt für Deutsch `numerusform` (singular/plural). Betrifft u. a. „N frames dropped",
  „N recoverable session(s)", „N marker(s)".
- **Zahlen/Einheiten:** User-sichtbare Mengen über `QLocale` formatieren (deutsche Dezimal-Komma):
  A/V-Drift „±%1 ms" (`RecordPage.cpp:4776`), dB-Werte, Dateigrößen „~%1 GB", Frameraten. Konkret:
  `locale.toString(value, 'f', prec)` statt `QString::number`/`arg` für Fließkomma. **Ausnahme:**
  technische Einheiten-Kürzel (`ms`, `dB`, `fps`, `GB`, `MB`) bleiben unübersetzt (international
  üblich); nur der Zahlenteil wird lokalisiert.
- **Datum/Zeit:** Aufnahme-Zeitstempel in der Historie über `QLocale`/`QDateTime`-Lokalisierung;
  Dateinamen-Token-Zeitstempel bleiben **unverändert** (stabile, sortierbare Dateinamen).
- **Komposita vermeiden:** keine aus Fragmenten zusammengesetzten Sätze; immer ganze Sätze mit `%1`.

### Was bleibt bewusst unübersetzt

- **Codec/Container/Format-Labels** (`CodecLabels.h`): `MKV`, `MP4`, `WebM`, `H.264`, `HEVC`, `AV1`,
  `Opus`, `AAC`, `PCM`, `FLAC` — Naming-Kanon (`feedback_codec_naming_canon`). Ebenso Rate-Control-
  Kürzel (`CQ`, `VBR`, `CBR`), `fps`, Dateiendungen (`.mkv`/`.mp4`/`.webm`).
- **Log-Zeilen** (`AppLog::info/warning/error`, Kategorie-Tags, `EngineLogBridge`-Nachrichten):
  bleiben **Englisch** — Support/Diagnose muss über Sprachen hinweg lesbar und greppbar sein. Die
  `LogsPage` zeigt sie roh; das ist gewollt.
- **Engine-interne technische Fehlertexte** (`recorder_core` `recorder_session.cpp`/`mux_thread.cpp`
  „Failed to build AVCC …" etc.): sind Debug-/Log-nah. Sie erscheinen höchstens als sekundäres
  `error_detail`; der **primäre** User-Text kommt aus dem app-seitigen `error_message`-Mapper (der
  auf `ErrorPhase`-Enum + HResult mappt und übersetzt wird). Roh-Engine-Detail bleibt englisch.
- **Diagnostic-/FixAction-IDs** (`rec.003`, `fix.container.mkv`), **Preset-IDs**, **Pfade**,
  **Dateinamen-Token**, Produktname **ExoSnap**.

### Vollständigkeits-Check in CI

- Ein Target/Skript `check_translations` (in CI):
  1. `lupdate` im `-no-obsolete`-Modus **mit `-locations none`** laufen lassen und prüfen, dass die
     eingecheckte `exosnap_de.ts` **nicht driftet** (kein Diff → alle neuen `tr()`-Strings sind erfasst;
     Diff → CI rot mit Hinweis „`update_translations` ausführen"). **`-locations none` ist zwingend:**
     ohne es schreibt `lupdate` `<location line="…"/>`-Einträge, sodass jede Codeverschiebung oberhalb
     eines `tr()` die `.ts` diffiert und das Gate Zeilennummern statt Strings testen würde.
  2. Prüfen, dass die `.ts` **keine** `type="unfinished"`/leeren `<translation>` für die
     Ship-Locale `de` enthält (einfacher XML-Grep). Fehlende Übersetzung → CI rot.
  3. **Diagnostics-ID-Coverage:** die zentrale Resolver-ID-Enumeration (alle `MakeResult`-/`FixAction`-IDs)
     gegen die `DiagnosticText`-Mappe prüfen (jede ID → Mappe-Eintrag). Das `lupdate`-Gate sieht nur
     `translate()`-Literale und fängt eine **fehlende ID-Mappe** nicht — dieser Zusatz-Check schon.
- So kann kein neuer User-String unübersetzt (oder unerfasst) ins Release rutschen, und keine neue
  Diagnostic-ID rutscht still auf Englisch durch.

---

## Implementierungsschritte (reihenfolgetreu, jede Nummer = PR-fähige Einheit)

> Reihenfolge: erst Infrastruktur (klein, risikoarm, ermöglicht Messung), dann Schichten B/C/D
> (strukturell, engbegrenzt), dann der breite Sweep A phasenweise pro Seite, zuletzt CI-Gate.

1. **Linguist-Infra-Bootstrap.** `LinguistTools` in `app/CMakeLists.txt:8`; `qt_add_translations(
   exosnap TS_FILES i18n/exosnap_de.ts LUPDATE_OPTIONS -locations none)` (Pfad relativ zu `app/`,
   `-locations none` gegen `.ts`-Churn); leere/initiale `exosnap_de.ts` einchecken. **CI-Enabler
   (Pflicht, sonst stiller Skip):** `qttools` in **alle vier** Install-Qt-Steps
   (`ci.yml:77/:217/:298`, `release-candidate.yml:32`) aufnehmen (`archives: 'qtbase qtsvg qttools'`);
   LinguistTools **hart** (nicht QUIET) auffinden **nach** dem Qt6-Dummy-Guard, plus einen
   CI-Artefakt-Assert auf `exosnap.exe` (Details s. Build-Pipeline-Schritt 1a–c). `QTranslator`-
   Lade-/Install-Code in `app/main.cpp` (nach `:117`) hinter eine **feste** effektive Locale (zunächst
   `System`), inkl. `qtbase_de`-Deploy. **Visual-Test-Harness pinnt die Locale** (Default `en_US`,
   Schalter `--visual-test-locale=de`), damit Renders host-unabhängig sind (s. Laden-Abschnitt).
   **Test/Verify:** Build grün (in CI **mit** qttools — sonst nur lokal aussagekräftig, da die Dev-Qt
   qttools hat, CI ohne den Enabler nicht); CI-Artefakt-Assert bestätigt, dass `exosnap.exe` gebaut
   wurde (Skip-Falle ausgeschlossen); App startet (Startup-Crash-Check); ein einzelner Beispielstring
   wird testweise auf `tr()` gezogen und erscheint auf einem deutschen System übersetzt (User-live).
   Kein sichtbares Verhalten auf englischem System; `--visual-test` rendert unverändert `en_US`.
2. **Sprachwahl-Setting.** `LanguageSetting`-Enum im Preset/Settings-Store (TOML) + Settings-UI-Auswahl
   + „wirkt nach Neustart"-Hinweis; `main.cpp` liest die Einstellung und wählt die Locale.
   **Test:** Store-Roundtrip-Unit-Test (Enum persistiert/lädt); Widget-Test, dass die Auswahl existiert
   und den Store setzt. **User-live:** Umschalten + Neustart wechselt die Sprache.
3. **Notification-Text-Mappe (Schicht D).** Helfer `notifications::NotificationText` (`QString` via
   `translate`), der `NotificationType` (+ ggf. Payload) → `title`/`body` mappt; Wiring-Sites in
   `MainWindow.cpp` rufen ihn statt inline-Literale. **Test:** Unit-Test je `NotificationType` liefert
   nichtleeren Titel/Body; `.ts` bekommt die Einträge.
4. **Fehler-Mapper auf QString (Schicht B, Teil 1).** `UiErrorMessage` `std::wstring`→`QString`;
   `MapErrorToUserMessage` nutzt `translate("RecordError", …)` und mappt `ErrorPhase`-Enum statt
   `FormatErrorPhase`-Prosa. `FormatErrorPhase` aus dem Coordinator entfernen oder auf reinen
   App-Presentation-Helfer `RecordText::errorPhaseLabel(ErrorPhase)` verschieben. **Test:** bestehende
   `error_message`-Unit-Tests auf `QString` anpassen; ein Test je `ErrorPhase` → nichtleerer Text.
5. **Status-Text strukturieren (Schicht B, Teil 2).** `BuildCapabilityStatusText` entfernen; Coordinator
   bekommt einen **neuen öffentlichen Accessor** auf `resolved_user_config_` (heute privat,
   `RecordingCoordinator.h:333-334`; es gibt **keinen** vorhandenen `resolved_config`-Accessor) —
   z. B. `ResolvedConfig()` oder eine Ready-Daten-Struktur. UI baut „Ready: …" aus `CodecLabels`
   + `tr()` + `QLocale`. **Blocked-Pfad ebenfalls umstellen:** die Ersatzquelle für
   `capability_status_text_ = L"Recording unavailable" / ToWide(invalidity.front().message)`
   (`RecordingCoordinator.cpp:517-518`, `:546-550`, `:528`) ist der `InvalidReason.code` (Schritt 6) →
   `translate("Diagnostics"/"RecordStatus", …)`, leerer Fall → `translate("RecordStatus",
   "Recording unavailable")`. **Threadsicher:** resolved_config bzw. Invalidity-Code im
   `PostStateChange`-Payload/unter derselben Sperre übergeben (kein Pull-Race vom UI-Thread).
   `RecordViewModel::result_status_text` von `L"Recording succeeded/failed"` auf Erfolgs-Bool/State
   umstellen; UI übersetzt. **Test:** Widget-/ViewModel-Test, dass der Ready-String die
   `CodecLabels`-Schreibweisen nutzt (behebt Drift) und dass Ready/Blocked/Erfolg/Fehler korrekt
   gerendert wird.
6. **Engine-Codes ergänzen (Schicht C, Teil 1, `libs/capability`).** `code` (String oder Enum) zu
   `InvalidReason` und `SupportAnnotation` hinzufügen (analog `Warning.code`); Resolver setzt stabile
   Codes; `reason`/`message` bleiben englischer Fallback. **Test:** `libs/capability`-Unit-Tests
   (Qt-frei) prüfen, dass jeder Invalid-/Support-Pfad einen stabilen Code liefert.
7. **Diagnostics-Presentation-Mappe (Schicht C, Teil 2).** `diagnostics::DiagnosticText` (Qt), keyed
   auf `DiagnosticResult.id`/`FixAction.id`/Engine-`code` → übersetzte `title`/`summary`/
   `recommendation`/`label`/`changes_summary` via `translate("Diagnostics", …)`, Fallback = englischer
   Resolver-String. `DiagnosticsPage.cpp:1085-1220` rendert über diese Mappe statt `fromStdString`
   verbatim; `InvalidReason.message`/`Warning.message` werden über den Code gemappt.
   `RecommendationEngine` bleibt Qt-frei. **Test:** Mappe-Unit-Test (jeder bekannte `id` → nichtleer,
   unbekannter → Fallback) **plus** ein **Coverage-Test gegen die zentrale Resolver-ID-Enumeration**
   (alle `MakeResult`-/`FixAction`-IDs des `RecommendationEngine` haben einen Mappe-Eintrag — fängt
   neu hinzugefügte IDs, die sonst still englisch blieben); `DiagnosticsPage`-Widget-Test, dass Karten
   den gemappten Text zeigen.
8. **tr()-Sweep Schicht A — phasenweise, eine PR pro Seite** (`RecordPage`, `ConfigPage`,
   `DiagnosticsPage`, `DevicePage`, `OutputPage`, `WebcamPage`, `HotkeysPage`, `LogsPage`,
   `AboutPage`, `EditExportPage`, plus `notifications`/Overlays). Pro Seite: User-`QStringLiteral`→
   `tr()`, Komposita zu `%1`-Sätzen, Zahlen über `QLocale`, Plurale mit `%n`. Nach jeder Seite
   `update_translations` und die neuen `de`-Einträge in `exosnap_de.ts` übersetzen. **Test:** je Seite
   Build grün + Widget-Tests grün (Objektnamen/Verhalten unverändert); `--visual-test`-Render im
   Default (`en_US`, harness-gepinnt) **unverändert** im Layout — die deutschen Strings prüft man mit
   `--visual-test-locale=de` (Clipping/Wrap). Der Default-Render ist damit host-unabhängig, nicht mehr
   „deutsch auf deutschem Windows".
9. **CI-Vollständigkeits-Gate.** `check_translations`-Skript/Target (Drift-Check via `lupdate
   -no-obsolete` **mit `-locations none`** — sonst testet das Gate Zeilennummern statt Strings —
   + Unfinished-Check) in die PR-CI. **Test:** Gate schlägt bei absichtlich hinzugefügtem,
   unübersetztem `tr()` fehl; grün nach Übersetzung; **kein** Diff bei reiner Codeverschiebung
   (Regressions-Test für `-locations none`).
10. **Doku.** `docs/product-spec.md` (user-sichtbares Verhalten: Sprachwahl, Default = System),
    `KNOWN_LIMITATIONS.md` (welche Flächen bewusst englisch bleiben: Logs, Roh-Engine-Detail,
    Codec-Labels), `docs/roadmap.md` (1.0-Punkt „Deutsche Lokalisierung"). Ggf. kurze ADR
    „Lokalisierungs-Schichtenmodell" unter `docs/decisions/`.
11. **Release-Deploy von `qtbase_de.qm` (kann früh landen, gehört sachlich zu Schritt 1/4).**
    `scripts/build-release-artifacts.ps1:621-627` löscht heute `translations/` komplett („no i18n in
    MVP"). Prune so ändern, dass `qtbase_de.qm` (und nur die tatsächlich benötigten `qt*_de.qm`)
    **erhalten** bleibt bzw. gezielt in den Staging-Tree kopiert wird — ohne den restlichen ungenutzten
    Katalog wieder aufzunehmen. Weil das MSI aus **demselben** gepruneten Tree geharvestet wird (ADR 0036),
    landet der Katalog damit automatisch in ZIP **und** MSI. **ADR 0036 fortschreiben** (Prune-Liste
    `translations/` nicht mehr pauschal; Begründung Lokalisierung). **Test:** Release-Skript-Assert bzw.
    manuelle Prüfung, dass `translations/qtbase_de.qm` im gepruneten Staging-Tree **und** in der
    extrahierten MSI vorhanden ist; die eingebettete App-`.qm` (`:/i18n/`) ist von diesem Schritt
    unberührt (shippt via Ressource in der `exe`).

---

## Test-/Verify-Plan

**CI-fähig (automatisiert, ohne echte GPU/App-Bedienung):**
- Unit-Tests (Qt-frei): `libs/capability`-Codes (Schritt 6); `RecommendationEngine` bleibt Qt-frei
  und grün.
- Widget-Tests (mit `QApplication`-Fixture, `feedback_gtest_isolation_qapplication`):
  Notification-Text-Mappe, `DiagnosticText`-Mappe, `error_message`-Mapper, Ready-String aus
  `CodecLabels`, Sprach-Setting-Roundtrip, Pages nach dem Sweep (Objektnamen/Verhalten stabil).
- `--visual-test`-Render-Harness (Locale **gepinnt**: Default `en_US`, deutsche Checks via
  `--visual-test-locale=de` — nie host-abhängig): Layout/Clipping-Regression pro umgestellter Seite
  (deutsche Strings sind teils länger — Wrap/Clipping prüfen; `feedback_wordwrap_minimum_height_clipping`).
- CI-Gate `check_translations` (Drift + Unfinished).
- Startup-Crash-Check nach `main.cpp`-Änderungen (App startet einmal, überlebt, wird geschlossen).

**Nur User-live (nicht CI, nicht vom Agenten bedienbar — App nie selbst treiben):**
- Tatsächlicher Sprachwechsel auf einem deutschen System (Default = System) und via Settings-Override
  + Neustart: erscheinen alle Flächen deutsch, bleiben Logs/Codec-Labels englisch?
- Qt-Standarddialoge (Datei-Dialog, Message-Boxes) deutsch (`qtbase_de` korrekt deployt/geladen).
- Optische Endabnahme der längeren deutschen Strings in echten Fenstergrößen (Ready-Zeile,
  Diagnose-Karten, Notification-Toasts) — kein Abschneiden, keine gebrochenen Umbrüche.
- Zahlen-/Einheitenformat (Dezimal-Komma bei Drift/dB/GB) im laufenden Betrieb.

---

## Risiken

- **Sweep-Größe & übersehene Strings.** Laufzeit-konkatenierte oder in `std::string`/`std::wstring`
  gebaute Strings entgehen `lupdate`. Mitigation: Schichten B/C **zuerst** strukturell umbauen (nicht
  nur `tr()` drüber), dann der Widget-Sweep; CI-Drift-Gate fängt neu hinzukommende `tr()`-Strings,
  **nicht** aber weiter versteckte Nicht-`tr()`-Prosa — dagegen hilft nur der bewusste Schicht-B/C-
  Umbau.
- **Qt-Freiheit der Resolver.** Versehentliches `QString`/`translate` in `RecommendationEngine` oder
  `libs/capability` bricht die Qt-freien Tests und die Engine-Agnostik. Mitigation: Presentation strikt
  in die App-Mappe; Review-Checkliste.
- **Layout-Regression.** Deutsche Strings sind länger → Clipping/Umbruch. Mitigation: `--visual-test`
  pro Seite; `SizePolicy::Minimum`/Mindesthöhen beachten.
- **Live-Umschalten nicht gebaut.** Ohne `retranslateUi` in handgeschriebenem UI-Code wirkt der
  Wechsel erst nach Neustart. Bewusst so (ehrlicher, kleiner). Falls Produkt Live-Wechsel verlangt,
  ist das ein separater, größerer Slice (jede Page müsste `changeEvent(LanguageChange)` bedienen).
- **Übersetzungsqualität/Terminologie.** Deutsche Fachbegriffe (Aufnahme, Encoder, Container) müssen
  konsistent sein; Glossar in der `.ts`/als Kommentar pflegen. Codec-Kürzel bleiben englisch (Kanon).
- **Qt-eigene Übersetzungen im Release-Build.** `qtbase_de.qm` muss tatsächlich mitdeployt werden,
  sonst deutsche App + englische Standarddialoge. **Konkrete Kollision:** die Release-Pipeline prunt
  `translations/` (`build-release-artifacts.ps1:621-627`, ADR 0036) — der Katalog fehlt sonst in ZIP
  **und** MSI. Mitigation: Schritt 11 (Prune anpassen + ADR 0036 fortschreiben + Assert auf
  `qtbase_de.qm` im Staging-Tree und in der extrahierten MSI). Die eingebettete App-`.qm` (`:/i18n/`)
  ist davon unberührt.
- **Stiller CI-Skip durch fehlendes qttools.** Ein naives LinguistTools-COMPONENT macht `Qt6_FOUND`
  in CI FALSE (nur `qtbase qtsvg` installiert) und ersetzt das `exosnap`-Target durch ein Echo-Dummy —
  CI bliebe grün, ohne App/Tests zu bauen. Mitigation: qttools in alle Install-Steps, harter
  LinguistTools-Find nach dem Dummy-Guard, CI-Artefakt-Assert auf `exosnap.exe` (Build-Pipeline
  Schritt 1a–c).

---

## Offene Fragen (echte Produktentscheidungen)

1. **Default-Sprache:** Der Systemsprache folgen (`System` → Deutsch nur auf deutschem Windows) — oder
   bis zum expliziten Opt-in **immer Englisch**? (Empfehlung: `System`.)
2. **Umschalt-Verhalten:** Neustart-nötig (einfach, ehrlich) vs. Live-`retranslateUi` (deutlich mehr
   Aufwau über alle Pages)? (Empfehlung für 1.0: Neustart.)
3. **Sprachumfang 1.0:** Nur `de` zusätzlich zur Quellsprache `en`, oder gleich Infrastruktur für
   weitere Locales offen halten? (Infra ist so oder so mehrsprachfähig; Frage ist nur, ob weitere
   `.ts` jetzt angelegt werden.)
4. **Zahlenformat:** Deutsche Locale-Formatierung (Dezimal-Komma) für user-sichtbare Mengen (Drift/dB/
   GB/fps-Zahl) anwenden — oder aus Konsistenz mit Logs überall Punkt lassen? (Empfehlung: `QLocale`
   für sichtbare Mengen, Punkt/C-Locale nur in Logs.)
5. **Platzierung der Sprachwahl** in Settings (eigener „Sprache"-Block vs. Advanced-Sektion).

---

## Adversarialer Review — Ergebnis

Alle acht Einwände wurden gegen den Code/die Docs geprüft (Datei:Zeile) und als **berechtigt bestätigt**;
alle acht wurden eingearbeitet.

- **[Blocker] LinguistTools-COMPONENT schaltet CI still ab** — **eingearbeitet.** Bestätigt:
  `app/CMakeLists.txt:8` (`QUIET COMPONENTS Core Gui Widgets Svg`) + Echo-Dummy `:10-14` bei
  `NOT Qt6_FOUND`; CI lädt nur `qtbase qtsvg` (`ci.yml:77/:217/:298`, `release-candidate.yml:32`,
  `crash-capture-build.yml:60`). Build-Pipeline-Schritt 1 + Schritt 1 der Umsetzung + Risiko ergänzt:
  qttools in alle Install-Steps, harter LinguistTools-Find nach dem Dummy-Guard, CI-Artefakt-Assert
  auf `exosnap.exe`.
- **[Major] `qtbase_de.qm`-Deploy kollidiert mit Release-Prune** — **eingearbeitet.** Bestätigt:
  `build-release-artifacts.ps1:621-627` prunt `translations/`, ADR 0036:19-20 dokumentiert es, MSI
  harvestet denselben Tree. Neuer Umsetzungs-Schritt 11 (Prune anpassen + ADR 0036 fortschreiben +
  Assert) und Build-Pipeline-Schritt 4/Risiko ergänzt. Klargestellt: die eingebettete App-`.qm`
  (`:/i18n/`) ist **nicht** betroffen — nur der Qt-Standarddialog-Katalog.
- **[Major] Visual-Test-Harness wird host-locale-abhängig** — **eingearbeitet.** Bestätigt: `--visual-test`
  läuft durch die echte `main()` (`main.cpp:117-133`), **0** `QLocale`-Treffer in `app/`. Laden-Abschnitt,
  Schritt 1, Schritt 8 und Test-Plan: Harness pinnt Default `en_US`, Schalter `--visual-test-locale=de`.
- **[Minor] Kein öffentlicher `resolved_config`-Accessor; Blocked-Pfad übersehen** — **eingearbeitet.**
  Bestätigt: `resolved_user_config_`/`validation_result_` privat (`RecordingCoordinator.h:333-334`), nur
  `CapabilityStatusText()`/`ResolvedVideoCodecLabel()` öffentlich (`:192-193`); Blocked-Pfad
  `capability_status_text_ = L"Recording unavailable"/invalidity.front().message` (`RecordingCoordinator.cpp:517-518`,
  `:546-550`, `:528`). Schicht-B-Entscheidung + Schritt 5: neuer Accessor, Blocked-Ersatz über
  `InvalidReason.code`, threadsichere Übergabe im `PostStateChange`-Payload.
- **[Minor] CI-Drift-Gate brüchig ohne `-locations none`** — **eingearbeitet.** `-locations none` via
  `LUPDATE_OPTIONS` in `qt_add_translations`, im CI-Gate und in Schritt 1/2/9 festgeschrieben.
- **[Minor] Mappe-Coverage-Lücke + Prosa-Duplikation** — **eingearbeitet.** Schicht-C-Entscheidung,
  Schritt 7 und CI-Check: zentrale Resolver-ID-Enumeration (alle `MakeResult`-/`FixAction`-IDs) gegen
  die Mappe testen; bewusste Prosa-Duplikation (Resolver-Fallback nur Log/Support, Mappe = einzige
  angezeigte Quelle) explizit gemacht.
- **[Minor] Zwei Faktenfehler im Ist-Zustand** — **eingearbeitet.** (a) `NotificationType` = **11**
  Werte, nicht 12 (`NotificationEvent.h:13-25`; 12 gilt nur für `NotificationAction`). (b) Video-Codec-Kanon
  lebt Qt-frei in `libs/capability` (`VisibleVideoCodecLabel`, `codec_selection.cpp:32`);
  `CodecLabels.h:50-55` delegiert nur dorthin, `RecommendationEngine.cpp:262-263` nutzt ihn. „einzige
  Quelle"-Behauptung korrigiert; Container/Audio haben keinen Pure-Kanon.
- **[Minor] Falscher TS_FILES-Pfad + lokale-vs-CI-Qt** — **eingearbeitet.** Pfad korrigiert auf
  `i18n/exosnap_de.ts` (relativ zu `app/`); vermerkt, dass die Dev-Qt qttools hat, CI ohne den Enabler
  aus dem Blocker nicht — „Build grün" in Schritt 1 sonst nur lokal aussagekräftig.

# Privacy-Review als wiederholbarer Release-Schritt

> Spec-Welle 2026-07-11, Thema 20 (Roadmap 0.10 „Reliability hardening … privacy review").
> Autor read-only bzgl. Code/Docs; Umsetzung später **nur** anhand dieser Spec. Ist-Zustand
> frisch aus dem Code auf `main` @ #192 erhoben (Datei:Zeile-Referenzen unten; keine Zeilen aus
> dem Review-Dokument übernommen). Schnittstelle zu Thema 10 (`diagnostics-support-bundle-spec`)
> ist explizit ausgewiesen.

## Problem

ExoSnap verspricht ein **telemetriefreies** Produkt: „No analytics, no telemetry, no account. By
default ExoSnap makes no network connections" (`docs/product-spec.md:32,734`; `PRIVACY.md:14-17`).
Der **rohe Egress ist im Kern korrekt** (kein Socket, keine Telemetrie, alle vier Netz-Pfade nach
GitHub/Sentry), **aber der adversariale Review hat zwei Stellen aufgedeckt, an denen Ist-Code und
Docs nicht mehr deckungsgleich sind** — genau die Drift, die dieses Verfahren finden soll:

- **Ist-Zustands-Lücke 0 (Consent-Drift Update-Check):** Der automatische Update-Check ist **default-AN**
  (`AppSettingsStore.h:66` `check_updates_on_start = true`; Load-Default `AppSettingsStore.cpp:91`) und
  läuft beim Start **ohne Consent-Dialog/First-Run-Prompt** (`MainWindow.cpp:1120`; ein solcher Prompt
  existiert **nicht** — Grep über `app/` findet keinen). Ein **Official Build** kontaktiert damit
  `api.github.com` beim ersten Start ohne Opt-in — im direkten Widerspruch zu `PRIVACY.md:62`
  („When the update check is enabled (**opt-in**)"), `PRIVACY.md:70-71` („Neither the update check nor
  crash reporting runs unless you **opt in**") sowie `docs/product-spec.md` §13 („the official build's
  update check is **opt-in and consent-gated**") und §14 („each strictly **opt-in and only when the
  user acts**"). Das Compile-Gate `IsUpdateCheckEnabled()` schützt nur Self-Builds; für den Official
  Build ist der Default **opt-out**, nicht opt-in. **Erfordert eine Produktentscheidung** (Default auf
  `false` / echter First-Run-Prompt / oder Docs auf „opt-out" korrigieren), siehe Offene Frage 5 — sonst
  wird die D1-Egress-Tabelle (Spalte Gate·Consent) für E3 mit **falscher Baseline** geschrieben.
- **Ist-Zustands-Lücke 0b (Minidump-Modulpfade):** siehe Egress-Punkt E1 unten — der bei Consent
  hochgeladene Crashpad-Minidump trägt volle Modulpfade; `PRIVACY.md:45-46` behauptet für diesen Kanal
  „file paths … are stripped".

Beide sind unten im Ist-Zustand ausführlich referenziert. Unabhängig davon ist das Versprechen
ohnehin **nicht nachweisbar gehalten**: Es gibt kein wiederkehrendes Verfahren, das vor jedem Release
prüft, ob

1. **alle Datenabflüsse** (Crash-Reports/Sentry, Stage-0-GitHub-Issue, Update-Check, Update-Download,
   künftig Support-Bundle) noch genau die Felder transportieren, die `PRIVACY.md` + `docs/product-spec.md`
   §14 behaupten — und **kein** neuer, unbeabsichtigter Abfluss dazugekommen ist;
2. der **Scrubber/Allowlist** jede neue strukturierte Crash-Annotation tatsächlich erfasst
   (heute kann ein neues Sentry-Tag/Context-Feld an der Allowlist vorbeigehen);
3. das dokumentierte **Allowlist-Versprechen** noch mit dem Code übereinstimmt (die Allowlist
   existiert **doppelt** im Code und driftet leicht);
4. das **Support-Bundle** (Thema 10) beim Bau tatsächlich Pfade/Hostnames/Fenster-Titel scrubbt.

Das Ziel ist bewusst schmal: **kein** neues Datenschutz-Feature, **keine** Telemetrie — sondern ein
Verfahren + eine Handvoll automatisierter Prüfungen, die das bereits erreichte „nichts verlässt die
Maschine außer opt-in Crash/Update" **beweisbar und regressionssicher** machen. Ruhig statt
alarmistisch: die Prüfungen melden nur echte Abweichungen, ein grüner Lauf ist der Normalfall.

## Ist-Zustand (mit Datei:Zeile-Referenzen)

### Datenfluss-Inventar (die tatsächlichen Egress-Punkte)

Erhoben durch Code-Suche über `libs/`, `app/`, `apps/`. Es existieren **genau vier** Runtime-Netz-
Pfade, alle über WinHTTP, alle nach GitHub oder Sentry; **kein** rohes Socket, kein weiterer Host.

**E1 — Crash-Report-Upload (Sentry, Stage 1).** `libs/crash_capture/src/crash_capture.cpp`.
- DSN nur unter `EXOSNAP_OFFICIAL_BUILD` einkompiliert (`:213-220`, hartcodierter EU-Endpoint
  `ingest.de.sentry.io`); Self-Builds setzen leeren DSN → nie Netzwerk.
- Consent-Gate `sentry_options_set_require_user_consent(options, 1)` (`:234-235`); Upload erst nach
  `GiveUserConsent()` (`:288-293`).
- `before_send` = `BeforeSendHook` (`:66-130`, gesetzt `:244`). Der Hook:
  - scrubbt `exception.values[].value` und `.module` via `ScrubString` (`:76-93`);
  - hängt eine **per-Report** `correlation_id` an (`:97-99`, nicht persistent);
  - **entfernt** `user` (`:102`) und `breadcrumbs` (`:106`);
  - **baut `tags` neu** aus einer **im Hook literal wiederholten** 10-Key-Liste (`:114-115`) und
    scrubbt jeden Wert (`:109-127`).
- Set-Time-Pfad: `SetTag` (`:334-346`) filtert über `IsAllowedTagKey` **und** scrubbt den Wert
  **vor** `sentry_set_tag` — also bereits beim Setzen, nicht erst bei `before_send`. `SetEncoderContext`
  (`:351-357`) setzt die vier Encoder-Tags. Einzige Callsites: `app/MainWindow.cpp:1405,1554`.
- Non-fatal-Pfad: `ReportNonFatalError` (`:313-329`) **pre-scrubbt** `phase` + `detail`, weil
  `before_send` den Message-Body nicht abdeckt.
- Session-Sidecar `last_session.json` (`WriteSession` `:489-509`): alle Felder durch `ScrubString`.
- **Minidump-Binärkanal (nicht vom Scrubber erfasst):** Bei Consent lädt Crashpad den Minidump
  hoch. Ein Minidump enthält per Format eine `MINIDUMP_MODULE_LIST` mit den **vollen Modulpfaden**,
  darunter der Installationspfad von `exosnap.exe`. Bei einer **portablen Installation unter
  `%USERPROFILE%`** steht damit der **Username** im übertragenen Report. `BeforeSendHook` scrubbt nur
  das **strukturierte** Event (`crash_capture.cpp:66-130`), **nicht** den Minidump-Binärinhalt — und
  für den **Hard-Crash läuft der Hook ohnehin nicht** (out-of-process Crashpad-Upload). Damit ist
  `PRIVACY.md:45-46` („file paths … are stripped before the report leaves the process") **für diesen
  Kanal nicht zutreffend**. Siehe Ist-Zustand-Lücke 1b und den erweiterten Live-Check.

**E2 — Stage-0-GitHub-Issue.** `app/services/CrashIssueReport.h`. Baut eine vorbefüllte
„new issue"-URL (`BuildCrashIssueUrl`) aus **ausschließlich allowlisted** Feldern
(`CrashIssueData`: app_version, os, gpu, encoder, exception, correlation_id). Header-Vertrag: „No
file paths, usernames or machine names ever appear here — callers scrub upstream" (`:9-12`). Öffnet
den Browser; IP geht in transit an GitHub. Immer verfügbar (kein Official-Gate).

**E3 — Update-Check.** `libs/update/src/update_checker.cpp`. `HttpsGet` (`:19-83`), User-Agent
`ExoSnap-UpdateChecker/1.0` (`:21`), reiner `GET` auf `api.github.com/repos/Exoridus/exosnap/releases?per_page=30`
(Default `update_checker.h:30`), Header nur `Accept`/`X-GitHub-Api-Version` (`:45-48`), **kein
Request-Body, kein Auth-Token, keine Query mit Nutzerdaten**. Compile-Gate `IsUpdateCheckEnabled()`
(`:158`), Recording-Guard davor (`:145-155`). Versionsvergleich passiert **client-seitig**
(`:189`); an GitHub gehen nur die HTTP-Request-Metadaten (IP + fixe UA). Der Standalone-Updater
nutzt denselben Host (`apps/updater/UpdaterWorker.cpp:63`). **Doc↔Code-Drift (siehe Lücke 5):** Der
Request sendet **keine** Version — die UA ist fix `ExoSnap-UpdateChecker/1.0` (das „1.0" ist die
Checker-Protokollversion, nicht die App-Version), Header nur `Accept`/`X-GitHub-Api-Version`. `PRIVACY.md:64-65`
(„transmits your IP address to GitHub … and the ExoSnap version string") und `docs/product-spec.md`
§14 („sends only the version string") behaupten jedoch, die App-Version werde gesendet. Sie wird es
**nicht**; der Versionsvergleich ist rein client-seitig.

**E4 — Update-Download.** `libs/update/src/http_download.cpp`. WinHTTP-`GET` (`:93-105`, UA
`ExoSnap-Updater/1.0`) auf GitHub-Release-Asset-URLs aus dem Manifest. Kein Body, kein Token.

**Nicht-Runtime:** RNNoise-Modell wird zur **Build-Zeit** gezogen (`third_party/CMakeLists.txt`),
kein Egress der laufenden App.

**E5 (künftig) — Support-Bundle.** Existiert noch nicht (Thema 10). Es ist **lokal, user-initiiert,
keine Transmission** — aber es aggregiert Logs, Capability/Adapter/Display-Snapshots und Settings zu
einer Datei, die der Nutzer manuell teilt. Damit ist es ein „privacy surface", auch ohne Netzabfluss:
Pfade, Hostnames und **Fenster-Titel** (WGC-Capture-Target-Namen tauchen in Logs auf) müssen vor dem
Verpacken gescrubbt werden. `diagnostics-support-bundle-spec` §D3 legt den zentralen `ScrubString`-Pass
+ Allowlist-Denkweise + Coverage-Test bereits fest und benennt genau diese Spec als Eigentümer des
Scrubbing-Standards.

**Lokal gespeichert, nie übertragen** (`PRIVACY.md:19-30`): `settings.ini`, `presets.ini`,
`recording-history.json`, Recovery-Manifest, Logs (`exosnap.log`, `engine.jsonl`), `crashes/*.dmp`,
Aufnahmen. Bestätigt: keine weiteren `WinHttpOpen`/Socket-Aufrufe außerhalb `libs/update` +
`libs/crash_capture`.

### Scrubber + Allowlist

- `libs/crash_capture/src/crash_scrubber.cpp`: `ScrubString` (`:170-197`, pure) strippt USERPROFILE →
  `[path]`, generische Windows-Pfade (`StripWindowsPaths` `:152-159`, Drive- + UNC-Regex), Username →
  `[user]`, Machine → `[machine]`. `SensitiveValueCache` (`:56-110`) cached Username/Machine/Profile.
- **Allowlist existiert doppelt:** `kAllowedTagKeys` (`crash_scrubber.cpp:37-40`, 10 Keys) **und** die
  literal wiederholte Liste in `BeforeSendHook` (`crash_capture.cpp:114-115`). Zwei Quellen der
  Wahrheit → Drift-Risiko: ein hier ergänzter Key, dort vergessen, führt zu inkonsistentem Verhalten.
- **Test-Abdeckung heute:** `test_crash_scrubber.cpp` prüft die 10 Keys **namentlich einzeln**
  (`:100-121`) und ein paar Scrub-Muster (`:36-94`). Es gibt **keinen** Test, der erzwingt, dass
  `BeforeSendHook` **dieselbe** Liste benutzt wie `kAllowedTagKeys`, und **keinen**, der erzwingt,
  dass eine **neue** strukturierte Annotation die Allowlist passieren muss.

### Lücken, die eine systematische Prüfung aufdeckt (frisch, kein Review-Übertrag)

0. **Consent-Drift Update-Check (Blocker).** Siehe Problem oben: `check_updates_on_start` default `true`
   (`AppSettingsStore.h:66`, `AppSettingsStore.cpp:91`), Auto-Check beim Start ohne Consent-Dialog
   (`MainWindow.cpp:1120`, kein First-Run-Prompt im Baum). Official Build kontaktiert GitHub beim ersten
   Start ohne Opt-in ↔ „opt-in and consent-gated" in `PRIVACY.md:62,70-71` + product-spec §13/§14. Diese
   Lücke ist eine **echte Produktentscheidung** (Offene Frage 5) und **muss** vor der D1-Egress-Tabelle
   geklärt sein, weil sie deren Spalte Gate·Consent für E3 bestimmt.

1. **`before_send` fasst `contexts` nicht an — aber das behauptete `server_name`/`device`-Leck
   existiert in dieser sentry-native-Version nicht (korrigiert).** Der Hook entfernt `user`/`breadcrumbs`
   und baut `tags` neu, lässt aber `contexts` **unberührt** (`crash_capture.cpp:66-130`).
   **Faktenkorrektur gegen die gelinkte Version (0.15.0, `cmake/VendorSentry.cmake:74-78,103`):**
   sentry-native setzt bei `sentry_init` **keinen** Default-`server_name` und **keinen** `device`-Context.
   Der einzige Auto-Context ist `os` (`src/sentry_scope.c`: `sentry__get_os_context()`), der auf Windows
   nur `name`/`kernel_version`/`version`/`build` enthält — **kein Hostname, kein Username** (verifiziert
   gegen die gepinnte Quelle: `src/sentry_os.c` Windows-Zweig; `include/sentry.h` enthält **kein**
   `server_name`). Die zuvor als „stärkster Inventar-Fund" deklarierte Aussage war damit **falsch**, und
   die in D3/Alt-B vorgeschlagene API `sentry_options_set_server_name` **existiert in der sentry-native-
   Public-API nicht**. Was bleibt (kein Fund, sondern billiger Backstop + Live-Verifikation): der Hook
   kann `server_name`/`contexts.device` **defensiv** entfernen, falls eine künftige Version oder eigener
   Code sie doch setzt; die **echte** Prüfung, dass kein Hostname/Pfad im hochgeladenen Event steht,
   bleibt der Live-Sentry-Abgleich. Siehe revidiertes D3.

1b. **Minidump-Modulpfade (der echte Hard-Crash-Fund).** Der bei Consent hochgeladene Crashpad-Minidump
   enthält die `MINIDUMP_MODULE_LIST` mit vollen Modulpfaden inkl. des `exosnap.exe`-Installationspfads;
   bei portabler Installation unter `%USERPROFILE%` steht damit der Username im Report (E1-Detail oben).
   Kein Scrubber fasst den Minidump-Binärinhalt an, und für den Hard-Crash läuft `before_send` gar nicht.
   `PRIVACY.md:45-46` ist für diesen Kanal falsch. **Aufnahme:** entweder Doc präzisieren („der Minidump
   enthält Modulpfade; bei Nicht-Standard-Installationspfad kann der Username-Anteil des Pfads erscheinen")
   **oder** eine Minidump-seitige Mitigation prüfen (Standard-Installationspfad erzwingen / Crashpad-
   Scrubbing) — Offene Frage 1. Der [Live]-Check wird explizit um „Modulpfade im hochgeladenen Minidump
   ansehen" erweitert (bisher prüfte er nur Event-Felder).
2. **Allowlist-Keys `os.*`/`gpu.*` werden nie gesetzt.** Nur `encoder_backend/container/video_codec/
   audio_codec` werden via `SetEncoderContext` gesetzt (`MainWindow.cpp:1405,1554`). Die sechs
   restlichen Allowlist-Keys (`os.name, os.version, gpu.model, gpu.vendor, gpu.driver, app.version`)
   werden vom App-Code **nirgends** als Sentry-Tag gesetzt; sentry-native detektiert keine GPU. Damit
   verspricht `PRIVACY.md:41-44` / product-spec §14 „GPU model and driver version" für den **Sentry**-
   Pfad Felder, die dort aktuell gar nicht ankommen (sie kommen nur über E2/den Crash-Dialog). Kein
   Datenschutz-Leck, aber eine **Doc↔Code-Drift**, die die Review sichtbar machen und auflösen soll
   (entweder Felder tatsächlich setzen — dann sind sie allowlisted & gescrubbt — oder das Versprechen
   präzisieren).
3. **Kein Egress-Register.** Nichts hindert eine künftige PR daran, einen fünften Netz-Aufruf
   hinzuzufügen (neue Lib, `WinHttpOpen`, `curl`, Socket), ohne dass es auffällt. Das „no telemetry"-
   Versprechen ist nur so stark wie die Aufmerksamkeit des Reviewers.
4. **Kein PRIVACY.md↔Code-Abgleich.** `PRIVACY.md` (Effective 2026-06-15) und product-spec §14 zählen
   die Allowlist von Hand auf; nichts prüft, dass diese Aufzählung == `kAllowedTagKeys`.
5. **Update-Check-„Version string"-Drift (Doc↔Code).** `PRIVACY.md:64-65` und product-spec §14 behaupten,
   der Update-Check sende „the ExoSnap version string". Der Code sendet **keine** Version (fixe UA
   `ExoSnap-UpdateChecker/1.0`, `update_checker.cpp:21`; nur `Accept`/`X-GitHub-Api-Version`-Header
   `:45-48`; Vergleich client-seitig `:189`). Kein Datenschutz-Leck (weniger wird gesendet als behauptet),
   aber eine konkrete Drift, die der Doku-Abgleich auflösen muss (Schritt 3/Schritt 6-Scope): entweder die
   Docs auf „nur IP + fixe UA, keine App-Version" korrigieren oder — falls die Version für Diagnose
   gewünscht ist — sie bewusst in die UA aufnehmen (Produktentscheidung, hier als Doc-Korrektur empfohlen,
   da der Egress absichtlich minimal ist).

### Vorhandene Anker für die Umsetzung

- CI `lint`-Job (`.github/workflows/ci.yml:32-48`) reiht **build-freie** pwsh-Checks aneinander
  (`check-format.ps1`, `validate-winget-manifest.ps1`, `validate-msi-harvest.ps1`) — der natürliche
  Ort für einen neuen, billigen Privacy-Doc-Check.
- CI `crash-capture-build.yml`: baut heute **nur** `--target exosnap` (`:90-91`), **ohne** `ctest`,
  und läuft auf PRs **nur** mit Label `crash-capture` (`:39-41`; sonst push-to-main / dispatch). Für
  den sentry-gebundenen Enforcement-Test muss dieser Job erst Tests bauen + `ctest` laufen lassen
  (Schritt 2a). Der reguläre `build-test`-Job (`ci.yml`) trägt den sentry-freien Golden-Set-Test auf
  jedem PR.
- Release-Prozess: `docs/release-checklist.md` (human-gated Schritte + Live-Checks).
- ADR-Historie: neueste `0043`; Support-Bundle-Spec beansprucht `0044` → diese Spec = **`0045`**.
- Doku-Policy (Memory `feedback_doc_tracking_policy`): `docs/` = kuratiert/tracked; `.workspace/` =
  Scratch. Das durable Review-Verfahren gehört nach `docs/`.
- Hinweis: die in `CLAUDE.md` als Pflichtlektüre gelistete
  `.workspace/architecture/system-overview.md` **existiert im Checkout nicht** (Glob leer) — hier
  nicht fabriziert, sondern als Fehlanzeige vermerkt.

## Design

Leitprinzip: **Verfahren + Enforcement, kein neues Produktverhalten.** Ein durable, tracked
Review-Dokument hält das Inventar; wo eine Prüfung billig automatisierbar ist, ersetzt sie Handarbeit;
wo nur ein echter Official-Build/echtes Sentry-Event die Wahrheit zeigt, bleibt es ein benannter
User-Live-Check. Fünf einzeln lieferbare Bausteine.

### D1 — Durable Datenfluss-Inventar: `docs/privacy-review.md`

**Entscheidung:** ein kuratiertes, tracked Dokument als **einzige** Quelle des Verfahrens. Es enthält
(a) die Egress-Tabelle E1–E5 (Zweck · Gate · Consent · gesendete Felder · Empfänger/Host), (b) die
Liste der lokalen Nie-übertragen-Stores, (c) die **Review-Checkliste** (unten D5), (d) Verweise auf
die automatisierten Prüfungen (D2–D4) mit der Aussage, welcher Checklistenpunkt durch welchen Test
abgedeckt ist und welcher **nur manuell/live** geht.

**Baseline-Abhängigkeit (Ist-Zustands-Lücke 0):** Die Spalte **Gate·Consent** für **E3** kann erst
korrekt geschrieben werden, wenn Offene Frage 5 entschieden ist. Der **Ist-Zustand** ist „default-AN,
kein First-Run-Consent" (opt-out) — das **muss** in der Tabelle so stehen und **nicht** als „opt-in
consent-gated" geschönt werden. Wird OF5 zugunsten „Default false / First-Run-Prompt" entschieden,
ändert Schritt 9 den Code und die Tabelle wird auf „opt-in" gesetzt; wird zugunsten „Docs auf opt-out"
entschieden, bleibt die Tabelle „default-AN" und PRIVACY.md/§14 werden angeglichen. So oder so ist die
Tabelle danach code-wahr.

Abgewogene Alternativen:
- **Alt-A: Inventar nur in `PRIVACY.md`.** *Verworfen:* `PRIVACY.md` ist die **nutzergerichtete**
  Aussage (plain-language, keine Datei:Zeile-Interna); das Review-Verfahren mit Codepfaden/Tests
  gehört nicht dorthin. Beide bleiben getrennt, D2 hält sie konsistent.
- **Alt-B: Inventar in `.workspace/` lassen.** *Verworfen:* `.workspace` ist Scratch/untracked; ein
  Release-Gate braucht ein tracked Artefakt, das mit dem Code altert.
- **Alt-C (gewählt): `docs/privacy-review.md` (tracked) + `PRIVACY.md` (nutzergerichtet) + ein
  Konsistenz-Check dazwischen.** Klarste Rollen, minimaler Overhead.

### D2 — Allowlist als Single Source of Truth + Enforcement-Tests

**Kernentscheidung:** die doppelte Allowlist zusammenführen und beweisen, dass **jeder** strukturierte
Crash-Pfad genau durch sie geht.

**Ehrliche CI-Reichweite (korrigiert).** „CI-erzwungene Eigenschaft statt Reviewer-Hoffnung" gilt nur,
wenn der Enforcement-Test auch wirklich läuft. Ist-Zustand: `crash-capture-build.yml` baut heute **nur**
`--target exosnap` (`:90-91`, baut **keine** Tests), hat **keinen** `ctest`-Schritt, und läuft auf PRs
**nur** mit Label `crash-capture` (`:39-41`). Der sentry-gebundene `BeforeSendHook`-Test (Schritt 2)
würde dort also nie ausgeführt — auf normalen PRs nicht einmal der ON-Build. Daher **zwei getrennte
Enforcement-Ebenen**, beide unten präzise ausgewiesen:
- **Immer, auf jedem PR:** der **sentry-freie** Golden-Set-Test (`test_crash_scrubber.cpp`) im regulären
  `build-test`-Job von `ci.yml` — er erzwingt Allowlist == Golden und deckt `IsAllowedTagKey`/`ScrubString`
  ab, **ohne** sentry-native.
- **Nur auf `main`-Push / Label / Dispatch:** der `BeforeSendHook`-Test (braucht `EXOSNAP_SENTRY_AVAILABLE`)
  im `crash-capture-build.yml`-Job — **setzt aber voraus, dass dieser Job Tests baut und `ctest` läuft**
  (heute nicht der Fall → eigener Implementierungsschritt, siehe Schritt 2a).

So bleibt der Kern („neue Crash-Annotation muss durch die Allowlist") auf **jedem** PR grün via Golden-Set,
und der Hook-Level-Beweis läuft auf main/Label — ohne die Illusion, der Hook-Test liefe auf unlabeled PRs.

Konkret:
1. **Dedupe:** `BeforeSendHook` (`crash_capture.cpp:114-115`) iteriert **nicht** mehr über ein
   literales Brace-Array, sondern über `kAllowedTagKeys` (exponiert als
   `span<const string_view> AllowedTagKeys()` in `crash_scrubber.h`). Eine Quelle.
2. **Enforcement-Unit-Test** (`test_crash_scrubber.cpp`, erweitert, sentry-frei):
   - `BeforeSendHook`-Tag-Filterung == `kAllowedTagKeys`: baut ein Fake-`sentry_value` mit einem
     allowlisted + einem nicht-allowlisted Tag, ruft den Hook (unter `EXOSNAP_SENTRY_AVAILABLE`, im
     crash-capture-build.yml-Job) und prüft, dass genau die Allowlist-Keys überleben. **Der Kern:**
     ein Test, der fehlschlägt, sobald jemand einen neuen Key in `kAllowedTagKeys` aufnimmt, ohne die
     nutzergerichtete Doku zu aktualisieren (siehe D2-Golden unten).
   - **Golden-Set-Test:** `kAllowedTagKeys` == ein in-Test hartcodiertes, kommentiertes Golden-Array.
     Jede Änderung an der Allowlist zwingt den Autor, dieses Golden bewusst zu ändern — der Diff wird
     im Review sichtbar und triggert die PRIVACY.md-Pflicht (D2b).
3. **`before_send`-Context-Härtung** ist eigenständig genug für D3.

**D2b — PRIVACY.md ↔ Allowlist-Konsistenz-Check (CI, build-frei).** Neues
`scripts/validate-privacy-allowlist.ps1`, eingehängt im `lint`-Job (`ci.yml:38-48`, Muster wie
`validate-*.ps1`). Es liest die Allowlist-Keys aus `crash_scrubber.h` (ein maschinenlesbarer Block,
z.B. zwischen `// PRIVACY-ALLOWLIST-BEGIN`/`END`-Markern) und prüft, dass **jeder** Key in einer
Mapping-Tabelle in `PRIVACY.md` **und** `docs/product-spec.md` §14 genannt ist (und umgekehrt kein
dort genanntes Feld fehlt). Rein textuell, kein Build, Sekunden. Abgewogen: den C++-Header zur
Compile-Zeit gegen ein eingebettetes Doc-Fragment prüfen (verworfen: koppelt Doku an Kompilat); ein
pwsh-Textabgleich ist billiger und liegt neben den existierenden Lint-Checks.

### D3 — `before_send`-Backstop + Minidump-Modulpfad-Entscheidung (revidiert)

**Faktenlage (korrigiert, Ist-Zustand-Lücke 1/1b):** Die frühere Annahme, sentry-native hänge einen
`server_name`(=Hostname) + `device`-Context an, ist für die gelinkte Version **0.15.0 falsch** (kein
`device`-Context, kein `server_name`, kein `sentry_options_set_server_name` in der Public-API; `os`-Context
ohne Hostname/Username — verifiziert gegen die gepinnte Quelle). Es gibt hier also **kein aktives Context-
Leck zu schließen**. Zwei getrennte Dinge bleiben:

1. **Billiger Backstop im `before_send` (defensiv, kein Fund).** Der Hook entfernt zusätzlich
   `server_name` und `contexts.device`, falls eine **künftige** sentry-native-Version oder eigener Code
   sie doch setzt. Das ist ein einzeiliger Riegel ohne Verhaltensänderung heute — **nicht** als
   Motivations-Anker eines existierenden Lecks verkauft. Kein Init-seitiges „Nullen" mehr, keine Nutzung
   der nicht existierenden `sentry_options_set_server_name`-API.

2. **Minidump-Modulpfade (der reale Fund, Lücke 1b) — Produktentscheidung.** Der out-of-process
   Crashpad-Minidump trägt volle Modulpfade; `before_send` läuft dafür nicht und scrubbt den Binärinhalt
   ohnehin nicht. Optionen (Offene Frage 1):
   - **(a) Doc präzisieren:** `PRIVACY.md`/§14 stellen klar, dass der Minidump Modulpfade enthält und bei
     Nicht-Standard-Installationspfaden (z.B. portabel unter `%USERPROFILE%`) der Username-Anteil des
     Pfads im Report erscheinen kann — die „paths are stripped"-Zusage gilt für das strukturierte Event,
     nicht für den Minidump-Binärkanal.
   - **(b) Mitigation prüfen:** ob ein erzwungener Standard-Installationspfad (Program Files, kein
     Username im Pfad) oder eine Crashpad-seitige Modulpfad-Redaktion praktikabel ist. Der reine
     `before_send`-Pfad kann das **nicht** lösen.

- **Verifikation (Live):** Die endgültige Wahrheit zeigt nur ein echtes Official-Build-Event in der
  Sentry-UI — inkl. **Blick auf die Modulpfade im hochgeladenen Minidump**, nicht nur die Event-Felder.
  User-Live, siehe Test-Plan. Offene Frage 1.

### D4 — Egress-Register-Guard (CI, build-frei): „no new telemetry" beweisbar

**Entscheidung:** ein billiger Grep-Guard, der jeden **neuen** Netz-Aufruf sichtbar macht. Neues
`scripts/validate-network-egress.ps1` im `lint`-Job:
- Sucht projektweit (tracked `libs/`, `app/`, `apps/`, ohne `third_party/` + `build/`) nach den
  Netz-Primitiven `WinHttpOpen`, `WinHttpConnect`, `WinHttpWebSocket`, `socket(`, `WSAStartup`,
  `getaddrinfo`, `connect(`, `InternetOpen`, `curl_easy`, sowie — weil die App **Qt 6** ist und ein
  künftiger `Qt6::Network`-Link der wahrscheinlichste Weg wäre, wie neuer Egress am Guard vorbeikäme —
  nach `QNetworkAccessManager`, `QTcpSocket`, `QUdpSocket`, `QSslSocket` und `Qt6::Network`; dazu
  `http://`/`https://`-Literale. (Heute: 0 Treffer für alle Qt-Netz-/Socket-Primitive im Baum, per
  Grep über `app/`, `libs/`, `apps/` verifiziert — die Aufnahme kostet je eine Zeile im Skript und
  sichert den wahrscheinlichsten künftigen Egress-Weg ab.)
- Vergleicht die Fundstellen gegen eine **im Skript gepflegte Allowlist von Dateien** (heute exakt:
  `libs/update/src/update_checker.cpp`, `libs/update/src/http_download.cpp`,
  `apps/updater/UpdaterWorker.cpp`, `libs/crash_capture/src/crash_capture.cpp`) + eine Allowlist
  erlaubter Hosts (`api.github.com`, `github.com`, `objects.githubusercontent.com`,
  `*.ingest.de.sentry.io`). Jede Fundstelle außerhalb → **Fail** mit klarer Meldung „neuer
  Egress-Punkt: in privacy-review.md inventarisieren und die Allowlist hier erweitern".
- Abgewogen: eine echte statische Taint-Analyse (verworfen: massiv überdimensioniert, false positives,
  kein CI-Budget) vs. ein bewusst grober Grep-Guard, der **nicht** beweist, *was* gesendet wird, aber
  garantiert, dass **kein Egress-Punkt unbemerkt** dazukommt (gewählt: proportional, wartbar). Die
  „was wird gesendet"-Frage bleibt beim Feld-Inventar (D1) + Scrubber-Tests (D2/D5).

### D5 — Support-Bundle-Scrubbing-Standard (Schnittstelle zu Thema 10) + Review-Checkliste

**Entscheidung:** Diese Spec ist **Eigentümer des Scrubbing-Standards**, den Thema 10 implementiert.
`diagnostics-support-bundle-spec` §D3 baut den Bundle-Builder samt „Scrubber-Assertion: kein `C:\`,
kein Username/Machine, kein Ausgabepfad in irgendeinem Entry" (dortiger Schritt 5, Test-Plan). Diese
Spec ergänzt die **Anforderung** (nicht die Implementierung):

1. **Pflicht-Scrub-Felder fürs Bundle:** absolute Pfade (Drive + UNC), Username, Machine/Hostname und
   **Fenster-/Capture-Target-Titel**. Letztere sind der über `ScrubString` heute **nicht** abgedeckte
   Fall: WGC-Capture-Zielnamen (Fenstertitel, App-Namen) erscheinen in `exosnap.log`/`engine.jsonl`
   und sind potenziell sensibel (Dokumenttitel, Chatpartner-Namen im Fenstertitel). `ScrubString`
   kennt sie nicht (kein Pfad-/Namensmuster).
   - **Design für Fenster-Titel:** an der **Log-Quelle** neutralisieren, nicht beim Bundling raten.
     Dort, wo ein Capture-Target-Titel geloggt wird, den Titel entweder weglassen oder durch einen
     stabilen Platzhalter ersetzen (`window="[title]"`), analog zu `[path]`/`[user]`. Das Bundle
     scrubbt zusätzlich als Backstop, kann aber einen beliebigen Titel nicht zuverlässig erkennen —
     deshalb ist die Quelle die primäre Verteidigung. (Konkrete Callsites identifiziert Thema-10-
     Umsetzung; diese Spec macht es zur **Abnahmebedingung** des Bundles.)
2. **Coverage-Anforderung:** der Bundle-Scrubber-Test (Thema 10, Schritt 5) muss zusätzlich einen
   **Fenster-Titel-Fixture** enthalten (ein Log-Eintrag mit einem synthetischen Fenstertitel) und
   asserten, dass der Titel im gepackten Entry nicht auftaucht.
3. **Bundle im Egress-Register:** das Bundle ist E5 im Inventar (lokal, keine Transmission) — es taucht
   in `privacy-review.md` mit genau dieser Kennzeichnung auf, damit „Bundle == kein Netzabfluss"
   dokumentiert ist.

**Review-Checkliste** (der wiederholbare Release-Schritt, in `docs/privacy-review.md` + verlinkt aus
`docs/release-checklist.md`). Jeder Punkt kennzeichnet **[CI]** (automatisch grün/rot) oder **[Live]**
(nur manuell/echte Hardware/echtes Official-Event):

- [CI] `validate-privacy-allowlist.ps1` grün — Allowlist == PRIVACY.md == product-spec §14.
- [CI] `validate-network-egress.ps1` grün — kein neuer Egress-Punkt/Host.
- [CI] Crash-Scrubber-Tests grün — Allowlist-Golden + `before_send`-Tag-Filterung + Context-Härtung.
- [CI] (sobald Thema 10 gelandet) Support-Bundle-Scrubber-Coverage grün, inkl. Fenster-Titel-Fixture.
- [Live] **PRIVACY.md-Realitätsabgleich (Event):** ein echtes Official-Build sendet ein Test-Event
  (`SendTestEvent`, `crash_capture.cpp:302-311`) nach Consent; in der Sentry-UI verifizieren, dass
  **kein** Hostname (`server_name`/`device.name` — heute ohnehin nicht gesetzt, Lücke 1), **kein**
  Pfad/Username im Event steht und genau die Allowlist-Tags plus Stack ankommen.
- [Live] **Minidump-Modulpfad-Sichtung (Hard-Crash, Lücke 1b):** ein **provozierter echter Hard-Crash**
  mit aktivem Consent; im hochgeladenen Minidump (Sentry-UI oder lokale `.dmp`) die **Modulpfade**
  ansehen und prüfen, ob der `exosnap.exe`-Pfad einen Username-Anteil trägt (relevant bei
  Nicht-Standard-/portabler Installation). Dies ist der einzige echte Test des Binärkanals — der
  Event-basierte Check oben deckt ihn **nicht** ab.
- [Live] **Update-Check-Sichtprüfung:** Fiddler/Proxy-Mitschnitt eines Update-Checks zeigt nur den
  `GET` auf `api.github.com/.../releases` mit fixer UA, keine Nutzerdaten in der Query.
- [Manuell/Doku] `PRIVACY.md`-`Effective date` und `docs/product-spec.md` §14 wurden für dieses
  Release gegen das Inventar durchgegangen (auch wenn CI grün ist — neue Felder, neue Empfänger).

Ruhig, nicht alarmistisch: die Checkliste ist ein Häkchen-Ritual pro Release, kein Dauer-Monitor; die
CI-Checks sind still, solange nichts driftet.

## Implementierungsschritte

Jeder Schritt ist eine PR-fähige Einheit mit eigenem Testansatz. Reihenfolge = Abhängigkeit; D5 hat
eine weiche Abhängigkeit zu Thema 10 (kann ohne dessen Merge als Anforderung/Doku landen, der
Coverage-Test kommt mit Thema-10-Schritt 5).

**Schritt 1 — Allowlist deduplizieren + exponieren (D2).**
**Wichtig (Konsistenz mit D2b):** Das Key-Array `kAllowedTagKeys` liegt heute **im `.cpp`**
(`crash_scrubber.cpp:37-40`, `static constexpr std::array<std::string_view,10>`), aber D2b/Schritt 3
parst die Keys **aus dem Header** zwischen den `PRIVACY-ALLOWLIST`-Markern. Damit das Skript etwas zu
lesen hat, wird das Array **in den Header verschoben** (als `inline constexpr std::array<...>
kAllowedTagKeys` in `crash_scrubber.h`, umschlossen von den Markern) — **eine** Definition, im Header,
für C++-Code **und** Skript.
Dateien: `libs/crash_capture/include/crash_capture/crash_scrubber.h` (`kAllowedTagKeys` als `inline
constexpr` + `AllowedTagKeys()`-Accessor, umschlossen von `// PRIVACY-ALLOWLIST-BEGIN/END`),
`libs/crash_capture/src/crash_scrubber.cpp` (Array-Definition entfällt, `IsAllowedTagKey` nutzt den
Header), `libs/crash_capture/src/crash_capture.cpp` (`BeforeSendHook` `:114-115` iteriert über
`AllowedTagKeys()` statt Literal).
Test: `test_crash_scrubber.cpp` — Golden-Set == `kAllowedTagKeys`; `IsAllowedTagKey`-Verhalten
unverändert. **Reiner Refactor, kein Verhaltenswechsel** → grün ohne Sentry.

**Schritt 2 — `before_send`-Tag-Filterung + Context-Härtung testbar machen (D2/D3).**
Dateien: `libs/crash_capture/src/crash_capture.cpp` (`BeforeSendHook`: zusätzlich `server_name` +
`contexts.device` entfernen; Init-seitig `server_name` neutralisieren, sofern die sentry-native-Version
es erlaubt — sonst nur Scope-Override + Backstop im Hook), ggf. den Hook als testbare freie Funktion
`ScrubEventInPlace(sentry_value_t)` faktorisieren.
Test (`crash-capture-build.yml`-Job, `EXOSNAP_SENTRY_AVAILABLE`): Fake-Event mit nicht-allowlisted Tag
+ gesetztem `server_name` + `device`-Context → nach Hook nur Allowlist-Tags, kein `server_name`, kein
`device` (das ist der **defensive Backstop** aus D3-Punkt 1, kein existierendes Leck). Der
Hard-Crash-Pfad selbst bleibt **[Live]** (Schritt 6-Checkliste, inkl. Minidump-Modulpfad-Sichtung).
**Voraussetzung:** dieser Test läuft nur, wenn `crash-capture-build.yml` Tests baut und `ctest` läuft
— das liefert Schritt 2a.

**Schritt 2a — `crash-capture-build.yml` um Test-Build + `ctest` erweitern (D2, eigene PR-Einheit).**
Dateien: `.github/workflows/crash-capture-build.yml`. Heute baut der Job nur `--target exosnap` (`:90-91`)
und hat keinen Testlauf. Ergänzen: einen vollen Test-Build (kein isoliertes `--target`) **und** einen
`ctest`-Schritt (mind. die crash-capture-Tests), damit der sentry-gebundene `BeforeSendHook`-Test aus
Schritt 2 in CI tatsächlich ausgeführt wird. Kein Verhaltenswechsel am Produkt, nur CI. **Ehrlich
dokumentiert** (in D2 + `privacy-review.md`): dieser Hook-Test greift **nur** auf `main`-Push / Label
`crash-capture` / Dispatch; auf **unlabeled PRs** greift ausschließlich der sentry-freie Golden-Set-Test
im regulären `build-test`-Job. Diese PR ist von Schritt 1/2 getrennt, weil sie reine CI-Plumbing ist.

**Schritt 3 — `validate-privacy-allowlist.ps1` (D2b).**
Dateien: neu `scripts/validate-privacy-allowlist.ps1`, Einhängung im `lint`-Job (`ci.yml:38-48`).
Parst die Marker-Keys aus `crash_scrubber.h`, prüft Präsenz in `PRIVACY.md` + `docs/product-spec.md`
§14 (bidirektional). Test: das Skript selbst mit einem absichtlich fehlenden Key rot, mit
Ist-Zustand grün (Self-Check-Kommentar). Diese PR **muss** zugleich die PRIVACY.md-Mapping-Tabelle
in das erwartete Format bringen (heute Fließtext `:41-44` → aufzählbare Zeilen/Tabelle).

**Schritt 4 — `validate-network-egress.ps1` (D4).**
Dateien: neu `scripts/validate-network-egress.ps1`, Einhängung im `lint`-Job. Datei- + Host-Allowlist
im Skriptkopf. Test: läuft gegen den aktuellen Baum grün; ein temporär eingefügter Dummy-`WinHttpOpen`
in einer nicht-allowlisted Datei macht ihn rot (im Skript-Kommentar als Self-Check dokumentiert, nicht
committet).

**Schritt 5 — `docs/privacy-review.md` + `docs/decisions/0045-privacy-review.md` (D1).**
Dateien: neu `docs/privacy-review.md` (Egress-Tabelle E1–E5, lokale Stores, Checkliste D5,
CI-vs-Live-Zuordnung), neu ADR `0045` (Entscheidung: durable Inventar + Enforcement-Trias, Allowlist
Single-Source, Egress-Register, Support-Bundle-Scrubbing-Standard-Eigentümerschaft). Nur Doku.

**Schritt 6 — `docs/release-checklist.md`-Sektion „Privacy review" (D1/D5).**
Dateien: `docs/release-checklist.md` (neuer Abschnitt zwischen „Pre-cut" und „Publish"): die
[CI]-Punkte als „muss grün sein"-Verweis, die [Live]-Punkte (Sentry-Test-Event-Sichtprüfung,
Update-Check-Mitschnitt) als manuelle Häkchen, plus „PRIVACY.md/§14 gegen Inventar durchgesehen; bei
Feld-/Empfänger-Änderung `Effective date` bumpen". Nur Doku.

**Schritt 7 — Support-Bundle-Scrubbing-Anforderung verankern (D5, Schnittstelle Thema 10).**
Dateien: `docs/privacy-review.md` (E5 + Fenster-Titel-Pflicht), und — **nur falls Thema 10 bereits
gelandet ist** — Ergänzung des Bundle-Scrubber-Tests um den Fenster-Titel-Fixture sowie der
Log-Callsites um den `[title]`-Platzhalter. Andernfalls bleibt es die dokumentierte Abnahmebedingung,
die Thema-10-Schritt 5 erfüllt. Kein Doppelbau des Bundles hier.

**Schritt 8 — Doc-Drift-Auflösung `os.*`/`gpu.*` (D2, Ist-Zustand-Lücke 2).**
Entscheidung nötig (Offene Frage 2). Zwei Umsetzungen, je nach Antwort:
- (a) Felder **tatsächlich setzen**: `SetTag("gpu.model", …)` etc. an der `SetEncoderContext`-Callsite
  (`MainWindow.cpp:1405`), Werte aus `RuntimeCapabilitySnapshot`/`AdapterIdentity` (bereits gescrubbt
  & allowlisted); Test: Tags erscheinen. Dann stimmt product-spec §14 wieder.
- (b) **Versprechen präzisieren**: PRIVACY.md/§14 stellen klar, dass GPU/OS-Fakten im Sentry-Pfad nur
  im Stack/Release-Kontext, im Stage-0-Issue und im Crash-Dialog erscheinen — nur Doku.
Diese PR ist bewusst getrennt, weil sie eine Produktentscheidung trägt.

**Schritt 9 — Consent-Drift Update-Check auflösen (Ist-Zustands-Lücke 0, Blocker).**
Entscheidung nötig (Offene Frage 5). Zuerst zu klären, dann eine der Umsetzungen:
- (a) **Opt-in herstellen** (Code): `check_updates_on_start` Default auf `false` (`AppSettingsStore.h:66`
  + Load-Default `AppSettingsStore.cpp:91`) **und/oder** ein echter First-Run-Consent-Schritt, bevor
  `MainWindow.cpp:1120` den Auto-Check auslöst. Test: `test_app_settings_store.cpp` erwartet den neuen
  Default; ein Widget-/Flow-Test, dass ohne Consent kein Auto-Check läuft. Dann stimmen PRIVACY.md §62/70
  + product-spec §13/§14 wieder.
- (b) **Docs auf „opt-out" korrigieren** (nur Doku): PRIVACY.md + product-spec §13/§14 sagen ehrlich,
  dass der Update-Check **default-aktiv** ist und in Settings deaktiviert werden kann („opt-out", nicht
  „opt-in/consent-gated"). Billiger, aber schwächt das Datenschutz-Narrativ.
Empfehlung der Spec: **(a)**, weil „opt-in" das dokumentierte und beworbene Versprechen ist und ein
default-stiller Netz-Kontakt beim ersten Start dem „no network by default" widerspricht. Diese PR ist
getrennt, trägt eine Produktentscheidung, und **blockiert die D1-Egress-Tabelle** (E3-Baseline).

**Schritt 10 — Update-Check-„version string"-Doc-Drift auflösen (Ist-Zustands-Lücke 5).**
Nur Doku (empfohlen): PRIVACY.md:64-65 + product-spec §14 auf „sendet **keine** App-Version — nur IP +
fixe UA `ExoSnap-UpdateChecker/1.0`" korrigieren. Kann mit Schritt 6/9 gebündelt werden. Alternativ, falls
die Version diagnostisch gewünscht ist, sie bewusst in die UA aufnehmen (dann Code + Doc) — hier nicht
empfohlen, da der Egress absichtlich minimal ist.

## Test-/Verify-Plan

**CI-fähig (Unit/Lint, keine Live-App, kein echtes GPU/Netz):**
- Allowlist-Golden == `kAllowedTagKeys` (sentry-frei, regulärer `build-test`-Job, **jeder PR**).
- `BeforeSendHook` filtert Tags == Allowlist und entfernt defensiv `server_name`/`contexts.device`
  (crash-capture-build-Job mit `EXOSNAP_SENTRY_AVAILABLE` — **nur** main/Label/Dispatch, **setzt
  Schritt 2a voraus**: Test-Build + `ctest` im Job).
- `ScrubString`-Bestandstests bleiben grün (Pfad/UNC/Username/Machine, Case-Insensitivität).
- `validate-privacy-allowlist.ps1`: Ist-Zustand grün; injizierter Mismatch rot (Self-Check).
- `validate-network-egress.ps1`: Ist-Zustand grün; injizierter Fremd-Egress rot (Self-Check).
- (mit Thema 10) Bundle-Scrubber-Coverage inkl. Fenster-Titel-Fixture: kein Titel/Pfad/Name im Entry.
- Update-Check-Bestandstests (`libs/update/tests/*`) unverändert — kein Verhaltenswechsel.

**Nur User-live verifizierbar (echtes Official-Build / echtes Sentry-Event / Netz-Mitschnitt):**
- **Sentry-Realitätsabgleich (Event):** Official-Build, Consent geben, `SendTestEvent` auslösen; in der
  Sentry-EU-UI bestätigen, dass **kein** `server_name`/Hostname, **kein** Pfad/Username erscheint und
  genau die Allowlist-Tags + Stack ankommen (bestätigt zugleich, dass 0.15.0 keinen Hostname-Context
  anhängt — Lücke 1).
- **Minidump-Modulpfad-Sichtung (Hard-Crash, Lücke 1b):** ein **provozierter echter Hard-Crash** mit
  aktivem Consent; im hochgeladenen Minidump die **Modulpfade** prüfen (Username-Anteil im
  `exosnap.exe`-Pfad?). `SendTestEvent` erzeugt **keinen** Minidump — dieser Check braucht den echten
  Crash und ist der Goldstandard für den Binärkanal; der Agent baut/klickt das nicht, der Entwickler tut
  es.
- **Update-Check-Mitschnitt:** Proxy-Trace zeigt nur den erwarteten GitHub-`GET`, keine Nutzerdaten.
- **Support-Bundle-Realdaten-Check** (mit Thema 10): ein echtes Bundle auf dem Dev-System öffnen und
  bestätigen, dass kein persönlicher Pfad/Name/Fenstertitel durchrutscht (Unit-Test deckt nur
  synthetische Muster).

**Bewusst NICHT gebaut (kein Overengineering, keine MVP-Expansion):**
- **Keine Telemetrie, kein Analytics, kein Consent-Dashboard** — das Produkt bleibt telemetriefrei;
  die Review *beweist* genau das.
- **Keine statische Taint-/Datenfluss-Analyse** — der Grep-Egress-Guard ist bewusst grob.
- **Kein automatischer Sentry-Event-Introspektor in CI** — der GPU-lose Runner hat keinen Official-
  Build/DSN; der Event-Abgleich bleibt der benannte Live-Check.
- **Keine neuen Diagnostics-Checks/Runtime-Prüfungen** in der App (Diagnostics-Ruhe-Prinzip).
- **Kein Umbau der Update-/Crash-Netzpfade** — sie sind korrekt; die Review kapselt sie nur.

## Risiken

- **Scheinsicherheit durch den Grep-Guard.** `validate-network-egress.ps1` beweist *nicht*, welche
  Bytes fließen, nur *dass kein neuer Egress-Punkt* unbemerkt dazukommt. Mitigation: klar in
  `privacy-review.md` dokumentieren, dass das Feld-Inventar (D1) + Scrubber-Tests (D2/D5) + der
  Live-Sentry-Abgleich die „was wird gesendet"-Frage tragen, nicht der Guard.
- **Minidump-Binärkanal nicht scrubbbar (D3, Lücke 1b).** Der `before_send`-Backstop deckt den
  Minidump **nicht** ab (out-of-process, Binärinhalt); Modulpfade bleiben. Mitigation: der Fix ist
  **nicht** code-am-Hook, sondern Doc-Präzisierung und/oder Installationspfad-Policy (Offene Frage 1);
  der Live-Minidump-Sichtungs-Check ist die finale Wahrheit. Der frühere „`server_name`/`device`-Leck"-
  Anker ist **entfallen** (existiert in 0.15.0 nicht), damit auch die API-Versions-Unsicherheit.
- **Doc-Format-Kopplung (D2b).** Der Allowlist↔Doc-Check erzwingt ein parsebares Format in
  `PRIVACY.md`/§14. Mitigation: eine schmale, klar markierte Mapping-Tabelle; der Fließtext ringsum
  bleibt frei.
- **False positives im Egress-Guard** bei legitimen neuen URL-Literalen (z.B. Doku-Links im Code-
  Kommentar). Mitigation: Guard sucht Netz-**Primitive** + `http(s)://` nur in `.cpp/.h`-Codezeilen,
  Host-Allowlist großzügig für github/sentry; Kommentare/Doku via einfacher Heuristik oder einer
  `// egress-allow`-Inline-Suppr (Muster wie `.cppcheck-suppress`).
- **Wartungslast der Datei-Allowlist.** Jeder legitime neue Egress-Punkt zwingt zu einer bewussten
  Skript-Änderung — genau der gewünschte Reibungspunkt, aber er muss im PR-Template/ADR erklärt sein,
  damit er nicht als „nervig" umgangen wird.

## Offene Fragen (echte Produktentscheidungen)

1. **Minidump-Modulpfade (D3, Lücke 1b):** Der Crashpad-Minidump trägt volle Modulpfade inkl.
   `exosnap.exe`-Installationspfad; bei portabler Installation unter `%USERPROFILE%` steht der Username
   im Report. (Der zuvor vermutete `server_name`/`device`-Context existiert in sentry-native 0.15.0
   **nicht** — das ist geklärt, keine offene Frage mehr.) Zu entscheiden: (a) `PRIVACY.md`/§14
   **präzisieren** (der Minidump enthält Modulpfade; die „paths stripped"-Zusage gilt fürs strukturierte
   Event, nicht den Binärkanal) oder (b) eine **Mitigation** (Standard-Installationspfad erzwingen /
   Crashpad-Modulpfad-Redaktion prüfen)? Empfehlung: mindestens (a), (b) falls praktikabel.
2. **`os.*`/`gpu.*`-Drift (Schritt 8):** GPU-/OS-Fakten im Sentry-Pfad **tatsächlich setzen** (mehr
   Diagnosewert, bleibt allowlisted+gescrubbt) oder das Versprechen in PRIVACY.md/§14 **präzisieren**
   (diese Felder erscheinen nur in Stage-0-Issue/Crash-Dialog)? Beeinflusst, was „what is sent" ehrlich
   behauptet.
3. **Fenster-Titel-Policy (D5):** Fenstertitel in Logs generell durch `[title]` ersetzen (strengste
   Privacy, kostet Debug-Kontext beim WGC-Target) oder nur im Support-Bundle scrubben und im lokalen
   Log vollständig lassen (mehr Debug-Wert, aber der Nutzer teilt das Log evtl. auch manuell außerhalb
   des Bundles)? Empfehlung der Spec: an der Quelle ersetzen (lokal wie Bundle), weil der lokale Log
   ohnehin manuell geteilt wird.
4. **Review-Kadenz:** Privacy-Review als **hartes Gate pro Release** (jede Version) oder nur bei
   Releases, die einen Egress-Pfad berühren? Empfehlung: leichtes Gate jedes Release (die CI-Punkte
   sind gratis; nur der Live-Sentry-Check ist Aufwand und kann an „Official-Build-Änderung berührt
   Crash/Update" gekoppelt werden).
5. **Consent-Drift Update-Check (Blocker, Schritt 9):** Der Auto-Update-Check ist **default-AN** ohne
   First-Run-Consent, die Docs behaupten „opt-in and consent-gated". Auflösen durch (a) **Code**:
   Default `false` und/oder echter First-Run-Consent-Schritt, damit „opt-in" wieder stimmt — oder (b)
   **Docs** auf „opt-out (default-aktiv, in Settings abschaltbar)" korrigieren? Empfehlung: (a), weil
   ein stiller Netz-Kontakt beim ersten Start dem „no network connections by default" widerspricht.
   **Diese Entscheidung blockiert die D1-Egress-Tabelle** (E3-Baseline Gate·Consent).

## Adversarialer Review — Ergebnis

Jeder Einwand wurde gegen Code/Docs bzw. die gepinnte sentry-native-Quelle (0.15.0) selbst geprüft.

- **[Blocker] Update-Check default-AN, kein Consent — Kernprämisse „heute korrekt umgesetzt" falsch:**
  **Eingearbeitet.** Verifiziert: `AppSettingsStore.h:66`/`AppSettingsStore.cpp:91` (`= true`),
  `MainWindow.cpp:1120` (Auto-Check beim Start), kein First-Run-Prompt im Baum ↔ `PRIVACY.md:62,70-71`
  + product-spec §13/§14 („opt-in/consent-gated"). Als **Ist-Zustands-Lücke 0** aufgenommen (Problem +
  Lücken-Liste), D1-Egress-Tabelle mit ehrlicher E3-Baseline gekoppelt, **Schritt 9** + **Offene Frage 5**
  ergänzt.
- **[Major] „stärkster Inventar-Fund" (server_name/device-Context) faktisch falsch, nicht existierende
  API:** **Eingearbeitet.** Gegen die gepinnte Quelle bestätigt: kein `device`-Context, kein `server_name`,
  kein `sentry_options_set_server_name` in `include/sentry.h`; `os`-Context (Windows) nur
  name/kernel_version/version/build. Lücke 1, D3, Schritt 2, Offene Frage 1 und die Risiken umgeschrieben:
  Falschbehauptung entfernt, `before_send`-Entfernung bleibt als **billiger Backstop** ohne Motivations-Anker.
- **[Major] Reales Hard-Crash-Leck (Minidump-Modulpfade) fehlt:** **Eingearbeitet.** Als **Lücke 1b** +
  E1-Detail aufgenommen (MINIDUMP_MODULE_LIST → `exosnap.exe`-Pfad; Username bei portabler Installation;
  `PRIVACY.md:45-46` für diesen Kanal falsch). Live-Check um **Minidump-Modulpfad-Sichtung** erweitert,
  Entscheidung in Offene Frage 1/D3.
- **[Major] D2 „CI-erzwungene Eigenschaft" mit dem Job nicht erreichbar:** **Eingearbeitet.** Verifiziert:
  `crash-capture-build.yml` baut nur `--target exosnap` (`:90-91`), kein `ctest`, PR-Lauf nur mit Label
  (`:39-41`). D2 um ehrliche zweistufige CI-Reichweite ergänzt, **Schritt 2a** (Job um Test-Build + `ctest`)
  hinzugefügt; klargestellt, dass auf unlabeled PRs nur der sentry-freie Golden-Set-Test greift.
- **[Minor] Schritt 1 vs. D2b: `kAllowedTagKeys` im `.cpp`, Skript parst `.h`:** **Eingearbeitet.**
  Verifiziert (`crash_scrubber.cpp:37-40`). Schritt 1 legt fest, das Array als `inline constexpr` **in den
  Header** (zwischen die Marker) zu verschieben — eine Quelle für C++ **und** Skript.
- **[Minor] E3-Doc-Drift „version string" nicht benannt:** **Eingearbeitet.** Verifiziert: fixe UA
  `ExoSnap-UpdateChecker/1.0`, keine Version gesendet (`update_checker.cpp:21,45-48,189`) ↔ `PRIVACY.md:64-65`
  + §14. Als **Lücke 5** + E3-Notiz aufgenommen, **Schritt 10** für die Doc-Korrektur ergänzt.
- **[Minor] D4-Primitivliste unvollständig (Qt 6 Network):** **Eingearbeitet.** Grep bestätigt 0 Treffer
  heute; `QNetworkAccessManager`/`QTcpSocket`/`QUdpSocket`/`QSslSocket`/`Qt6::Network` + `getaddrinfo`/
  `connect(`/`WinHttpWebSocket` in die D4-Grep-Liste und Schritt 4 aufgenommen (wahrscheinlichster künftiger
  Egress-Weg).

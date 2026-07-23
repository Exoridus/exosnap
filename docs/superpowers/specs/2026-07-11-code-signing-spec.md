# Code-Signing (Authenticode) + SmartScreen-Reputation

> Roadmap 0.10 — vendor-unabhängige Härtung. Diese Spec macht die ExoSnap-Release-Artefakte
> (portable ZIP, MSI, `exosnap-updater.exe`) Authenticode-signierbar, integriert einen
> Signier-Provider (SignPath Foundation als Primär, Azure Trusted Signing als Fallback) in den
> Release-Workflow, klärt die kritische Reihenfolge **Signieren → Hashen → ed25519-Manifest**,
> ergänzt einen optionalen Authenticode-Verify-Pfad im Updater und beschreibt SmartScreen-Reputation
> ehrlich (OV statt EV → Vertrauen über Zeit, nicht sofort).

---

## Problem

Alle ausgelieferten Artefakte sind **unsigniert**. Konsequenzen:

- Windows **SmartScreen** blockt den Erststart des MSI/der EXE mit „Der Computer wurde durch Windows
  geschützt" (unbekannter Herausgeber). Das ist die konkrete Adoptionshürde, die im Fable-Review als
  Market-Fit-Loch und in `KNOWN_LIMITATIONS.md:226` als Grenze steht.
- Der **UAC-Prompt** des MSI-Updates (`msiexec /qn` elevated, `UpdaterWorker.cpp:544`) zeigt
  „Unbekannter Herausgeber" statt eines verifizierten Namens — genau an der Stelle, an der der Nutzer
  erhöhte Rechte vergibt.
- Der komplett gelandete Updater (ADR 0034) hat einen starken Integritäts-Trust-Root (detached
  ed25519 + SHA-256), aber **keine Herausgeber-Identität**: ein Angreifer, der je an den ed25519-Key
  käme, könnte ein bösartiges Paket ausliefern, ohne sich als ExoSnap ausweisen zu müssen.

Der Authenticode-Signier-Haken ist seit 0.4.0 als **dormanter** CI-Block angelegt
(`sign-manifest.yml:162-173`, auskommentiert, „awaiting SignPath cert") und in ADR 0012 als
„separate concern, not gating this wave" dokumentiert. 0.10 löst diese Zusage ein.

---

## Ist-Zustand (frisch aus dem Code, main @ #192)

### Release-Pipeline

- **`release-candidate.yml`** — ein `build`-Job (Zeilen 16–190) baut Release, `cmake --install` in
  einen Staging-Tree, ruft `build-release-artifacts.ps1 -SkipConfigure` (Zeile 117) als
  Release-Gate. Der Job liest die SHA-256-Sidecars (`steps.shas`, Zeilen 165–177) und exportiert
  `portable_sha256` / `msi_sha256` als Job-Outputs.
- Der `sign-manifest`-Job (Zeilen 201–212) wird **nach** `build` aufgerufen, nur auf `refs/tags/v*`
  und wenn `EXOSNAP_UPDATE_PUBLIC_KEY_HEX` gesetzt ist (Official-Build-Gate). Er reicht die
  Build-Job-Hashes als Inputs an `sign-manifest.yml` weiter (Zeilen 208–209).
- **Kein** Authenticode-Schritt existiert. Trigger: `tags: ['v*']` + `branches: ['release/**']`
  (Zeilen 8–10); PRs bewusst ausgeschlossen (Zeile 7).

### ed25519-Manifest (das, was gehasht wird)

- **`sign-manifest.yml:100-138`** baut das Manifest-JSON, das je Paket `sha256` (installer +
  portable) einbettet, serialisiert **einmal** (`json.dumps(indent=2)`, Zeile 127) und erzeugt eine
  **detached** Signatur über exakt diese Bytes → `update-manifest.json` + `update-manifest.json.sig`.
- Die eingebetteten `sha256`-Werte **kommen als Inputs von außen** (Zeilen 95–96,
  `INSTALLER_SHA256`/`PORTABLE_SHA256`), berechnet im `build`-Job aus den Sidecars, die
  `build-release-artifacts.ps1` schreibt.
- **Ordering-Konsequenz:** Das ed25519-Manifest bindet den SHA-256 der **finalen** Artefakt-Bytes.
  Jede Byte-Änderung am Artefakt nach dem Hashen (Authenticode hängt an das PE-Security-Directory
  bzw. die MSI an) invalidiert den im Manifest gebundenen Hash → der Updater verweigert das Update
  (SHA-256-Gate, `UpdaterWorker.cpp:365`). **Signieren muss also VOR dem Hashen passieren.**

### `build-release-artifacts.ps1` (wo Hashen passiert)

- Staging-Install: Zeilen 486–487. Prune der Dev-Trees + Validierung + `dumpbin`-Runtime-Audit:
  Zeilen 503–643.
- **ZIP erstellen: Zeilen 648–654. ZIP-SHA-256-Sidecar: Zeilen 662–663.**
- **MSI bauen (`wix build`): Zeile 764. MSI-SHA-256-Sidecar: Zeilen 767–768.**
- Per-File-SHA-256 in `artifact-manifest.json`: Zeilen 949–970.
- Required-Files-Audit enthält bereits `exosnap.exe`, `exosnap-updater.exe`, `Qt6*.dll` (Zeilen
  520–524). Ausgelieferte PE-Binaries im Staging-Tree: `exosnap.exe`, `exosnap-updater.exe`,
  `crashpad_handler.exe` (optional, Zeile 546), Qt6-DLLs, FFmpeg-DLLs (avformat/avcodec/avutil/
  swresample), ggf. `dxcompiler.dll`/`dxil.dll`. Die ZIP selbst ist **kein** Authenticode-fähiges
  Format; nur die PE-Files darin und die MSI sind signierbar.
- MSI-Smoke (Zeilen 828–917) und ZIP-Smoke (ab Zeile 972) laufen **innerhalb** dieses Skripts, aktuell
  auf den unsignierten Artefakten.

### Updater-Verify-Pfad (kein Authenticode)

- **`UpdaterWorker.cpp`**: ed25519-Manifest-Signatur wird vor jedem Feldzugriff geprüft (Zeile 306,
  `VerifyManifestSignature`), erst danach Parsen (Zeile 312). Paket-SHA-256 unter Lock
  (`LockAndVerifyPackage`, Zeilen 431–448; Aufruf Zeile 365). MSI-Pfad re-locked+re-verified vor der
  elevated Übergabe (Zeilen 536–542), dann `msiexec runas` (Zeilen 544–560). Deny-Write/Deny-Delete-
  Handle: `OpenPackageWriteLock`, Zeilen 151–160.
- **`package_verifier.cpp`**: ausschließlich SHA-256 via BCrypt (Zeilen 30–146). **Kein**
  `WinVerifyTrust`, kein `wintrust`. Repo-weit existiert keinerlei Authenticode-Verify-Code (Grep
  `WinVerifyTrust`/`signtool` trifft nur Docs + den dormanten CI-Hook).
- ADR 0012:100–110 trennt Authenticode („Herausgeber-Bindung, SmartScreen") sauber von der
  ed25519-Integritätssignatur; ADR 0012:149–151: MOTW wird erst **nach** ed25519+SHA-256-Pass
  gestrippt — self-verification, nicht SmartScreen, ist der Trust-Root des portablen Pfads.

### Sichtbare Aussagen (müssen nach Rollout aktualisiert werden)

- `docs/product-spec.md:726-728` („**Signing status.** Builds are **not yet code-signed** …
  SignPath Foundation … signed once the certificate is issued") und `:756` („both unsigned for now").
- `KNOWN_LIMITATIONS.md:226-227` („No code signing … SmartScreen may warn").
- `docs/roadmap.md:84` (0.10.0-Zeile nennt „updater/installer/signing/SmartScreen reputation"),
  `:175-176` (Cross-cutting „Installer & reputation").
- `docs/release-checklist.md` — **kein** Signier-Schritt vorhanden.

### CI-Kontext

- PR-CI (`ci.yml:282-322`) hat einen `packaging-smoke`-Job (Ninja-Release, `-SkipMsi`), der bewusst
  **ohne** Signieren läuft (nur bei packaging-relevanten Pfad-Änderungen). Signieren bleibt
  ausschließlich Release-Sache — kein SmartScreen-Bedarf auf PRs, und der Signier-Provider hat
  Kontingente/Policies.

---

## Design

### D1 — Signier-Umfang: welche Dateien?

**Frage aus dem Brief:** alle EXEs/DLLs oder nur eigene?

Abwägung:

- **Nur die zwei EXEs** (`exosnap.exe`, `exosnap-updater.exe`) + MSI: minimal, deckt genau die
  SmartScreen-/UAC-Oberfläche ab (SmartScreen bewertet, was der Nutzer **startet** — MSI und
  Haupt-EXE; DLLs lösen keinen SmartScreen-Prompt aus). Günstigstes Kontingent.
- **Alle PE-Files** inkl. Qt/FFmpeg re-signieren: uniformer Herausgeber über den ganzen Tree, hilft
  gegen AV-Heuristiken und WDAC/AppLocker-Policies. Aber: re-signieren einer **gültig von The Qt
  Company signierten** Qt-DLL **strippt** deren Signatur und ersetzt sie durch unsere → Provenienz-
  Verlust, mehr Signier-Volumen, kein SmartScreen-Gewinn.

**Wichtige Erkenntnis:** Die **Pipeline-Form ändert sich nicht** mit der Dateizahl — ob 2 oder 12
Files signiert werden, das Signieren muss am selben Punkt (vor Hashen, vor MSI-Bau) passieren. Die
Dateizahl ist nur ein Glob. Damit ist „mehr signieren" fast gratis, sobald die Pipeline steht.

**Entscheidung — zwei Tiers, ein Signier-Pass:**

- **Tier 1 (reputationsrelevant, zwingend):** die **MSI**, `exosnap.exe`, `exosnap-updater.exe`. Das
  bewertet SmartScreen und der UAC-Herausgeber-Prompt.
- **Tier 2 (defense-in-depth, gleicher Pass, geringe Grenzkosten):** alle übrigen PE-Files im
  Staging-Tree, **die nicht bereits gültig von einem vertrauenswürdigen Upstream signiert sind** —
  konkret `crashpad_handler.exe` und die projekteigenen FFmpeg-DLLs. **Qt-DLLs mit gültiger
  Qt-Company-Authenticode-Signatur bleiben unangetastet** (Provenienz erhalten, Kontingent sparen).

Die Selektion „unsigniert ODER von uns signiert → signieren; gültig-fremdsigniert → in Ruhe lassen"
wird per `Get-AuthenticodeSignature` je Datei entschieden. Ehrlich und quotenschonend. **Achtung
(D8d):** Diese dynamische Selektion muss in die **vorab registrierte SignPath-Artifact-Config** passen
(File-Metadata-Restrictions) — die registrierte Config deckt genau die Tier-1/2-Menge ab, sonst lehnt
SignPath den Request ab.

*Nicht signierbar:* die portable **ZIP** selbst (Authenticode kennt kein ZIP). Ihre Integrität trägt
weiterhin das ed25519-Manifest + SHA-256; ihre **Inhalte** (die PE-Files) sind Tier-1/2-signiert.

### D2 — Provider: SignPath vs. signtool-kompatibler Cloud-Key

**Korrektur ggü. einer früheren Fassung (adversarialer Review):** Der eigentliche Trade-off ist
**nicht** „kostenlos vs. Job-Split", sondern **„fremder Herausgeber-Name gratis vs. eigener Name
gegen Geld"** (siehe D7). Und die frühere Prämisse „man kann nicht mitten im PowerShell-Skript
synchron signieren" ist **falsch**: das offizielle SignPath-PowerShell-Modul bietet
`Submit-SigningRequest -WaitForCompletion -OutputArtifactPath` (Default-Timeout
`-WaitForCompletionTimeoutInSeconds` = 600 s) — Submit, Warten und Download der signierten Datei in
**einem** synchronen Aufruf. Damit ist **Inline-Signieren aus `build-release-artifacts.ps1`**
grundsätzlich auch mit SignPath möglich.

Zwei reale Randbedingungen bleiben und formen die Pipeline (nicht der Round-Trip an sich):

1. **Origin-Verification-Pflicht (Open Source Edition).** SignPaths *Open-Source*-Signing verlangt
   Origin-Verifikation, die aus einem **Trusted Build System** stammen soll. Die offizielle
   GitHub-Action `SignPath/github-action-submit-signing-request` liefert diese Origin-Daten
   nachweislich; ob das PS-Modul mit manuell gesetzten `-Origin`-Feldern aus GitHub Actions von der
   Foundation-Policy akzeptiert wird, ist die **eine offene Vendor-Frage** (Offene Frage 5).
2. **Manuelle Freigabe pro Release.** Jeder Release muss in SignPath von einem Approver freigegeben
   werden (D8). Das ist eine menschliche Wartezeit **mitten** im Signier-Aufruf.

Provider-Optionen:

- **SignPath Foundation** (kostenlos, OSS): Private Key verlässt SignPath nie (HSM); in CI liegen nur
  ein API-Token + Org/Projekt/Policy-Slugs, **kein Schlüsselmaterial** — beste Key-Custody, passt zum
  ADR-0012-Geist. **Preis dafür:** der Herausgeber-Name ist **„SignPath Foundation"**, nicht
  ExoSnap/Codexo (D7), plus die Prozesspflichten aus D8.
- **Azure Trusted Signing / signtool-Cloud-Key** (DigiCert KeyLocker, SSL.com eSigner): signtool +
  Provider-`dlib` signieren inline; **eigener** Herausgeber-Name. **Nachteil:** kostenpflichtig
  (Azure ~10 $/Monat bzw. OV ~200–400 $/Jahr), Auth (OIDC bevorzugt) in CI, und Azure Trusted Signing
  hat eine **3-Jahre-Rechtsträger-Alters-Hürde**.

**Entscheidung:** **SignPath Foundation als Primär** (kostenlos, beste Key-Custody), Herausgeber-Name
„SignPath Foundation" bewusst akzeptiert (D7). **Integration primär über das SignPath-PowerShell-Modul
inline** in `build-release-artifacts.ps1` an den zwei natürlichen Punkten (nach Runtime-Audit, nach
`wix build`) — kein Job-Split, kein Staging-Tree-Artefakt-Roundtrip (siehe D4-Revision). **Fallback,
falls die Foundation-Policy den Action-Konnektor für Origin-Verification erzwingt:** die offizielle
GitHub-Action als **mehrere Steps innerhalb eines Jobs** (gemeinsames Runner-Filesystem, ebenfalls
ohne Cross-Job-Artefakt). Der Signier-Schritt bleibt **provider-abstrahiert** („aus unsignierten
Artefakten signierte machen"); ein D6-Fallback tauscht nur diese Implementierung — der
**Ordering-Vertrag D3 bleibt identisch**.

### D3 — Reihenfolge: Signieren → Hashen → Manifest-Signieren (Kernpunkt)

Der invariante, provider-unabhängige Vertrag:

```
1. Build + cmake --install → validierter, geprüfter Staging-Tree (UNSIGNIERT)
2. Authenticode-Signieren der Tier-1/2-PE-Files IM Staging-Tree
3. Portable ZIP aus dem SIGNIERTEN Staging-Tree  → SHA-256(zip)
4. MSI aus dem SIGNIERTEN Staging-Tree bauen (wix)
5. Authenticode-Signieren der MSI               → SHA-256(msi)
6. Smoke-Tests auf den SIGNIERTEN Artefakten (MSI + ZIP)
7. ed25519-Manifest über SHA-256(zip) + SHA-256(msi) signieren (detached .sig)
```

Zwei Signier-Runden sind **inhärent** und unvermeidbar für volle Abdeckung: die MSI kann erst
signiert werden, wenn sie gebaut ist, und sie muss aus **bereits signierten** Binaries gebaut werden,
damit die auf Platte installierten `exosnap.exe` etc. signiert sind (MSI-Signatur signiert das
Paket, **nicht** transitiv seine entpackten Payload-Files). Also: signieren (loose PEs) → packen →
signieren (MSI).

Der ed25519-Schritt (`sign-manifest.yml`) bleibt **mechanisch unverändert** — er bindet jetzt nur
Hashes signierter Bytes. Weil er die Hashes als Inputs bekommt, „funktioniert" die bestehende
Verdrahtung, sobald die Hashes aus Schritt 3/5 (statt aus unsignierten Artefakten) stammen.

### D4 — Pipeline-Umbau (revidiert: **kein** 3-Job-Artefakt-Roundtrip)

**Revision (adversarialer Review):** Die frühere 3-Job-Struktur (`build -StageOnly` → `sign` →
`package -FromStagedTree`) mit Staging-Tree-Upload/-Download zwischen Jobs war **speculative
Overengineering** — sie folgte aus der falschen Prämisse, SignPath könne nur asynchron über eine
GitHub-Action signieren (D2-Korrektur). Beide Sign-Punkte liegen ohnehin auf **demselben Runner**;
weder das PS-Modul (inline) noch die GitHub-Action (Steps in einem Job) brauchen einen
Cross-Job-Artefakt-Roundtrip.

**Primärvariante — Inline über das SignPath-PS-Modul (ein Job):**
`build-release-artifacts.ps1` bleibt die **einzige** Packaging-Autorität ohne Modus-Split. An genau
zwei Stellen wird `Submit-SigningRequest -WaitForCompletion -OutputArtifactPath` (provider-abstrahiert
über einen `Invoke-SignArtifacts`-Wrapper) synchron aufgerufen:

1. **nach** Runtime-Audit (Skript-Zeile 643), **vor** ZIP-Erstellung (648): signiert die Tier-1/2-PEs
   im Staging-Tree (D1-Selektion).
2. **nach** `wix build` (764), **vor** MSI-SHA-256 (767): signiert die MSI.

Der bestehende `build`-Job von `release-candidate.yml` bleibt **ein** Job; es kommt nur der
Signier-Aufruf hinzu (gated auf `SIGNPATH_API_TOKEN`; Forks skippen wie beim ed25519-Gate). Der
`sign-manifest`-Job bleibt **mechanisch unverändert** und bindet die Hashes signierter Bytes.

**Fallback-Variante — GitHub-Action (falls die Foundation-Policy den Konnektor für Origin-Verification
erzwingt, Offene Frage 5):** die Action läuft als **eigene Steps im selben Job** (gemeinsames
Runner-Filesystem, **kein** Artefakt-Upload). Dafür — und **nur** dafür — braucht
`build-release-artifacts.ps1` einen Phasen-Seam, damit es an den zwei Punkten die Kontrolle an den
Action-Step zurückgibt (S1/S3). Da die Action mitten in einem PS-Skript nicht laufen kann, ist der
MSI-Seam ein **echter dritter Modus** (`-BuildMsiOnly` / `-FinalizeMsi`), nicht nur ein eingefügter
Aufruf (siehe S3). Kein Staging-Artefakt-Roundtrip auch hier.

**Manuelle Freigabe (D8):** In beiden Varianten blockiert der Signier-Aufruf, bis ein SignPath-Approver
den Release freigibt. `-WaitForCompletionTimeoutInSeconds` großzügig setzen; überschreitet die
Approval-Latenz das Fenster, läuft der Request **server-seitig weiter** und wird über die Request-Id
per `Get-SignedArtifact` **resumt**, statt den Job hart scheitern zu lassen (GitHub-Job-Max 6 h).

Wenn D6-Fallback (Azure/signtool) greift, ersetzt Inline-signtool den `Invoke-SignArtifacts`-Wrapper an
denselben zwei Punkten — der Ordering-Vertrag D3 gilt unverändert.

### D5 — Updater: optionaler Authenticode-Verify-Pfad

**Motivation ehrlich:** ed25519 + SHA-256 sind bereits ein vollständiger **Integritäts**-Trust-Root;
Authenticode ist dafür **redundant**. Der Mehrwert ist **Herausgeber-Bindung**: der Updater kann ein
Paket ablehnen, das nicht vom erwarteten ExoSnap-Zertifikat signiert ist — zusätzliche
Verteidigung, falls je der (niederwertige, selbstverwaltete) ed25519-Key leckt.

**Entscheidung — optional, standardmäßig advisory, hart nur wenn der Build weiß, dass er signiert
sein muss:**

- Neue Funktion `VerifyAuthenticode(handle, expected)` in `libs/update` (`package_verifier.cpp` +
  neuer Header), via `WinVerifyTrust(WINTRUST_ACTION_GENERIC_VERIFY_V2)` + `CryptQueryObject`/
  `CertGetNameString` zum Auslesen von Signer-Subject/Thumbprint. Rückgabe als neuer
  `VerifyResult`-Wert (z. B. `AuthenticodeUntrusted`).
- **Gate:** nur erzwungen, wenn ein Compile-Time-Konstant eingebacken ist — **selbes Muster wie
  `EXOSNAP_UPDATE_PUBLIC_KEY_HEX`**. Bei leer (Self-Build, Übergangs-Builds vor dem ersten signierten
  Release) wird der Check **komplett übersprungen** → bricht unsignierte Builds nie.
- **Pin-Strategie — Subject + Issuer, NICHT Thumbprint (Revision, adversarialer Review):** Ein reiner
  Cert-Thumbprint-Pin (`EXOSNAP_EXPECTED_AUTHENTICODE_SHA1`) **bricht bei jeder Cert-Erneuerung** —
  Code-Signing-Certs laufen nach 1–3 Jahren ab, und bei SignPath Foundation rotiert das Cert
  **außerhalb der Projektkontrolle**. Ausgelieferte Clients mit eingebackenem altem Thumbprint würden
  dann legitime, korrekt signierte Updates ablehnen (False-Positive `AuthenticodeUntrusted`). Deshalb
  pinnt der Updater auf **Signer-Subject + Issuer** (bei Foundation stabil = Subject „SignPath
  Foundation", Issuer = die CA), was routinemäßige Erneuerung überlebt. Der Konstant heißt
  entsprechend `EXOSNAP_EXPECTED_AUTHENTICODE_SUBJECT` (+ optional Issuer). Ein zusätzlicher
  Thumbprint-**Allowlist**-Eintrag mit **Übergangsfenster** (alt + neu gleichzeitig gültig) ist nur
  nötig, wenn ein stärkerer Pin gewünscht ist; er muss dann vor jedem Cert-Wechsel per Update
  ausgerollt werden. **Wichtig:** Der gepinnte Subject ist die **fremde** Identität „SignPath
  Foundation" (D7/D1) — der Pin bindet an SignPaths Herausgeber, nicht an eine projektkontrollierte
  Identität.
- **Reihenfolge im Updater:** **nach** ed25519+SHA-256 (ed25519 bleibt primärer Trust-Root;
  Authenticode ist sekundäres Gate). Verifikation **durch dasselbe gelockte Handle**
  (`WINTRUST_FILE_INFO.hFile`), damit die Verify-under-Lock-TOCTOU-Garantie (ADR 0012:132–142)
  erhalten bleibt.
- **MSI-Pfad:** `WinVerifyTrust` validiert MSI-Authenticode; Prüfung vor dem elevated `msiexec`
  (Paket ist bei `UpdaterWorker.cpp:536-542` bereits gelockt).
- **Portable-Pfad — schwächere Garantie, ehrlich benannt (Revision, adversarialer Review):** Die
  **ZIP** ist nicht Authenticode-signierbar; die starke *Verify-under-Lock*-Garantie von ADR
  0012:132–142 gilt für die **gelockte ZIP-Datei** (Hash durch dasselbe Deny-Write-Handle, keine
  TOCTOU). Die Authenticode-Prüfung läuft dagegen auf den **entpackten** `exosnap.exe` /
  `exosnap-updater.exe` im **user-schreibbaren, ungelockten** Staging-Dir — zwischen Check und
  `StageRename` besteht genau das TOCTOU-Fenster, das ADR 0012 für das **Paket** ausschließt, für die
  entpackten PEs aber **nicht**. Das wird nicht kaschiert: der portable Authenticode-Check ist
  **Best-Effort-Defense-in-Depth**, kein Verify-under-Lock. Der harte Integritäts-Trust-Root des
  portablen Pfads bleibt ed25519 + SHA-256 unter Lock auf der ZIP; der PE-Herausgeber-Check ist eine
  zusätzliche Schicht. (Ein Per-File-Lock über Check+Rename ist nicht praktikabel, weil offene
  Handles auf Dateien im umzubenennenden Staging-Verzeichnis das `MoveFileEx` des Verzeichnisses
  blockieren würden.)
- **Platzierung — beide Staging-Pfade abdecken:** `StagePortablePackage` wird aus **zwei** Stellen
  aufgerufen — im Download-Schritt (`UpdaterWorker.cpp:376`) **und** im Retry-Re-Staging
  (`UpdaterWorker.cpp:486`). Der Check gehört deshalb in `runInstallPortable` **unmittelbar vor**
  `StageRename` (Zeile 499), wo **beide** Staging-Ursprünge zusammenlaufen — nicht in den
  Download-Pfad, der das Retry-Re-Staging verpassen würde.
- **MSI-Pfad behält die starke Garantie:** dort prüft `WinVerifyTrust` das gelockte Paket-Handle vor
  `msiexec` (kein TOCTOU) — anders als der portable Pfad.

Dieser Punkt ist eine **Folge-Härtung** und **kein Gate** für den Signier-Rollout: D1–D4 liefern den
SmartScreen-Nutzen sofort; D5 kommt danach.

### D6 — Fallback, falls SignPath scheitert/sich verzögert

Leiter, provider-abstrahiert (D2):

1. **Azure Trusted Signing** — MS-nativ, signtool-kompatibel, HSM-gestützt, ~10 $/Monat, OIDC-Auth
   (kein Secret). Erlaubt **Inline**-Signieren und kollabiert die Job-Aufteilung. **Vorbehalt:** die
   3-Jahre-Rechtsträger-Alters-Hürde kann einen jungen Org blocken → offene Frage.
2. **Kommerzieller OV-Cert über Cloud-Signing** (SSL.com eSigner / DigiCert KeyLocker), ~200–400 $/Jahr,
   Key im HSM, signtool-`dlib`. Wenn Azures Alters-Gate blockt.
3. **Status quo (unsigniert)** — Reputation wächst nie; nur akzeptabel, solange keiner der obigen
   verfügbar ist. Nicht als Endzustand.

Weil der CI-Signier-Schritt eine **abstrakte** „signiere-diese-Files"-Einheit ist, wechselt ein
Provider-Fallback nur die Implementierung dieses Schritts — der Ordering-Vertrag D3 und die
Release-Checklist bleiben identisch.

### D7 — Herausgeber-Name + SmartScreen-Reputation ehrlich (Kern-Korrektur)

**Blocker-Korrektur (adversarialer Review): Der Herausgeber ist „SignPath Foundation", nicht
ExoSnap/Codexo.** Ein Foundation-Zertifikat wird laut den Foundation-Bedingungen
(`signpath.org/terms.html`) **auf „SignPath Foundation" ausgestellt**: „The code signing certificate
is issued to *SignPath Foundation*. This means that *SignPath Foundation* is the publisher of the OSS
project." UAC und SmartScreen zeigen daher **„SignPath Foundation"** als verifizierten Herausgeber —
**nicht** „ExoSnap" oder „Codexo". Die frühere Formulierung „zeigt ExoSnap/Codexo als verifizierten
Herausgeber" war **falsch** und ist gestrichen.

- **Was das Signieren tatsächlich liefert:** Es entfernt „**unbekannter** Herausgeber" und ersetzt ihn
  durch den **verifizierten, aber fremden** Namen „SignPath Foundation" (ein etablierter, vielen
  Windows-Nutzern als OSS-Signer bekannter Herausgeber). Der Projektname erscheint allenfalls in
  sekundären Cert-Feldern (OU/Description), **nicht** als primärer Publisher-String im UAC-Prompt.
- **Reputation hängt an der geteilten Foundation-Identität, nicht an einem ExoSnap-Cert.** SignPath
  Foundation signiert viele OSS-Projekte unter **derselben** Herausgeber-Identität; SmartScreen-
  Reputation akkumuliert projektübergreifend auf **dieser geteilten Identität**. Praktische Folge:
  die Foundation-Identität ist plausibel **bereits „warm"** — die „Reputation baut erst über Zeit ab
  Null auf"-Story eines **brandneuen, projekteigenen** OV-Certs trifft hier **so nicht** zu. Der
  konkrete SmartScreen-Erstlauf-Zustand ist **empirisch beim ersten Release zu beobachten** (nicht in
  CI messbar, S6/User-live).
- **Zertifikatstyp:** SignPath Foundation stellt **OV** (Organization Validation), **nicht EV**. Seit
  CA/B-Forum-Baseline (Juni 2023) liegen **alle** Code-Signing-Keys (OV wie EV) auf FIPS-Hardware
  (HSM/Token) — file-basierte PFX gibt es nicht mehr. **EV** gäbe sofortigen SmartScreen-Bonus mit
  **eigenem** Namen, ist aber nicht Teil des Foundation-Programms.
- **Der eigentliche Trade-off (D2):** **eigener** Herausgeber-Name gibt es nur über **Azure Trusted
  Signing** oder ein **kommerzielles OV-Cert** (D6-Leiter) — das kostet Geld (und ggf. die
  3-Jahre-Hürde). Foundation = gratis + geteilte, plausibel warme Reputation, aber **fremder Name**.
- **Konsequenz:** die Zertifikatsidentität muss **über Releases stabil** bleiben; ein Wechsel des
  Providers/Certs setzt die (an der Identität hängende) Reputation zurück. RFC-3161-**Timestamping ist
  zwingend** (Signaturen überleben Cert-Ablauf) — SignPath macht das automatisch; bei
  signtool-Fallback `/tr` + `/td sha256` setzen.

### D8 — SignPath-Foundation-Prozesspflichten (Pipeline- und Doc-relevant)

Aus `signpath.org/terms.html` ergeben sich harte Auflagen mit direkter Wirkung auf Pipeline, Repo und
Website — vorher unerwähnt, hier verankert:

- **(a) Manuelle Freigabe pro Release** („every release needs manual approval for signing"). Die
  Freigabe liegt **mitten** im Signier-Aufruf → blockierende Wartezeit; Timeout/Resume-Pfad in D4
  verankert.
- **(b) Code-Signing-Policy auf der Projekt-Homepage** mit dem Term „**Code signing policy**",
  SignPath-Foundation-Credit („Free code signing provided by SignPath.io, certificate by SignPath
  Foundation") **und Offenlegung der Team-Rollen + Mitglieder**. → Aufgabe in S4/S6.
- **(c) MFA-Pflicht** für **alle** Team-Mitglieder, sowohl bei SignPath als auch beim Source-Repo
  (GitHub). → Voraussetzung, in S6-Checkliste.
- **(d) Vorab registrierte Artifact-Configuration** mit **erzwungenen File-Metadata-Restrictions**
  (Product-Name/-Version). Die **dynamische D1-Selektion** (per `Get-AuthenticodeSignature` je Datei)
  muss zu **einer statisch in SignPath registrierten Artifact-Config passen**, sonst wird der Request
  abgelehnt. → Die Config wird einmalig registriert und deckt genau die Tier-1/2-Menge ab (S2/S6).

---

## Implementierungsschritte (PR-fähige Einheiten)

**Reihenfolge-Hinweis:** S1→S2→S3 sind der Signier-Rollout (liefert SmartScreen-Nutzen). S4 (Docs)
folgt dem ersten signierten Release. S5 (Updater-Authenticode) ist eine unabhängige Folge-Härtung.

### S1 — `build-release-artifacts.ps1`: Inline-`Invoke-SignArtifacts` + Signatur-Verify-Audit
- **Primärvariante (PS-Modul inline, D4):** einen provider-abstrahierten `Invoke-SignArtifacts`-Wrapper
  einführen, der intern `Submit-SigningRequest -WaitForCompletion -OutputArtifactPath` aufruft. Zwei
  Aufrufstellen: nach Runtime-Audit (Zeile 643, vor ZIP 648) für die Tier-1/2-PEs; nach `wix build`
  (764, vor MSI-SHA 767) für die MSI. **Kein** `-StageOnly`/`-FromStagedTree`, **kein**
  Artefakt-Roundtrip.
- **Fallback-Variante (GitHub-Action, nur falls Origin-Verification den Konnektor erzwingt, D4):** dann
  — und nur dann — den Phasen-Seam einführen: `-StageOnly` (stoppt nach Zeile 643) und **einen dritten
  Modus** `-BuildMsiOnly` (baut MSI, Zeile 764, **stoppt vor** Hash 767) + `-FinalizeMsi` (nimmt die
  signierte MSI, macht Hash + Content-Assertion + Smoke). Siehe S3 zur Begründung des dritten Modus.
- Neuer Validierungsschritt `Test-Signatures` (nur wenn `EXOSNAP_EXPECT_SIGNED=1`):
  `Get-AuthenticodeSignature` über Tier-1-Files + MSI → `Status -eq 'Valid'` und
  `SignerCertificate.Subject` matcht die erwartete Herausgeber-Identität **„SignPath Foundation"**
  (D7 — nicht „Codexo"/„ExoSnap"!); sonst `Add-Error`.
- **Test:** Unit-Test des Selektions-Predicates (D1) als reine PS-Funktion + Pester; Skript-Lauf in
  PR-CI-Packaging-Smoke (ohne echtes Signieren) grün.

### S2 — SignPath-Signier-Aufruf + `release-candidate.yml` (ein Job)
- **Primärvariante:** kein Job-Umbau — der bestehende `build`-Job ruft `build-release-artifacts.ps1`
  mit gesetztem `EXOSNAP_EXPECT_SIGNED` + SignPath-Vars auf; `Invoke-SignArtifacts` signiert inline.
  Fallback-Variante: SignPath-Action als **Steps im selben Job** (kein Cross-Job-Artefakt).
- **Vorab-Setup (D8, einmalig, User/Maintainer):** SignPath-Projekt anlegen, **Artifact-Configuration
  registrieren** (deckt Tier-1/2 + File-Metadata-Restrictions ab, D8d), **Approver-Rolle** besetzen
  (D8a), **MFA** für alle Team-Mitglieder aktivieren (D8c).
- **Secrets/Vars:** `SIGNPATH_API_TOKEN` (Secret, Scope = submit-signing-request);
  `SIGNPATH_ORGANIZATION_ID`, `SIGNPATH_PROJECT_SLUG`, `SIGNPATH_SIGNING_POLICY_SLUG` (repo-Vars).
  Fork-Skip via `if: vars.SIGNPATH_ORGANIZATION_ID != ''`.
- **Approval-Latenz (D4/D8a):** `-WaitForCompletionTimeoutInSeconds` großzügig; Resume über Request-Id
  (`Get-SignedArtifact`), falls die Freigabe das Fenster überschreitet.
- **Test:** CI-fähig nur als Struktur/Gating-Dry-Run (Job läuft ohne Token durch, überspringt
  Signieren) — der echte Round-Trip braucht Cert + Approval (User/Maintainer-live).

### S3 — MSI-Signatur-Punkt: Abschnitt 6 real umbauen (nicht nur „ein Aufruf")
- **Ehrlichkeitskorrektur (adversarialer Review):** Abschnitt 6 verschränkt aktuell in **einem**
  try-Block direkt nach `wix build` (764): MSI-**SHA-256** (766–768), MSI-**Content-Assertion**
  (779–816) und MSI-**Smoke** (828–917). „Sign nach `wix build`, vor Hash" ist **kein** eingefügter
  Einzeiler — der Block muss aufgebrochen werden, damit Hash/Assertion/Smoke auf der **signierten**
  MSI laufen.
- **Primärvariante (inline):** `Invoke-SignArtifacts` **direkt nach Zeile 764, vor 766** einfügen; die
  MSI-SHA (767) und alles danach beziehen sich damit automatisch auf die signierte MSI. Minimaler,
  aber **echter** Re-Sequencing-Eingriff im try-Block.
- **Fallback-Variante (Action):** dritter Skript-Modus `-BuildMsiOnly` (stoppt nach 764, vor 766) →
  Action-Step signiert → `-FinalizeMsi` nimmt die signierte MSI und macht Hash + Assertion + Smoke.
  Die zwei Schalter aus S1 reichen **nicht**; dieser dritte Modus ist Pflicht, weil die Action nicht
  mitten im Skript laufen kann.
- **Test:** MSI-Smoke grün auf **signierter** MSI (User/Maintainer-live wegen Cert); Struktur/Ordering
  in PR-CI (unsigniert) grün.

### S4 — Docs nach erstem signierten Release + Foundation-Pflicht-Doku
- `product-spec.md:726-728` + `:756` von „not yet code-signed"/„both unsigned" auf den echten Zustand:
  **OV-signiert durch SignPath Foundation, Herausgeber = „SignPath Foundation"** (nicht ExoSnap/Codexo,
  D7), SmartScreen-Erstlauf abhängig vom Reputationsstand der geteilten Foundation-Identität. **Nicht**
  behaupten, ExoSnap/Codexo erscheine als Publisher.
- `KNOWN_LIMITATIONS.md:226-227` entsprechend.
- **Projekt-Homepage — Code-Signing-Policy (D8b, Foundation-Pflicht):** Abschnitt „Code signing
  policy" mit Credit „Free code signing provided by SignPath.io, certificate by SignPath Foundation"
  **plus Team-Rollen + Mitglieder** anlegen. Ohne diese Seite verletzt der Release die
  Foundation-Bedingungen.
- `release-checklist.md`: neuer Abschnitt (S6 unten).
- **Test:** der bestehende Doc-Version-Audit (`build-release-artifacts.ps1:600-602`) bleibt grün; kein
  neuer automatisierter Test nötig.

### S5 — Updater: optionaler Authenticode-Verify (Folge-Härtung, D5)
- `VerifyAuthenticode(handle, expected)` in `libs/update` (WinVerifyTrust + Cert-Subject/Thumbprint),
  neuer `VerifyResult`-Wert.
- Compile-Gate `EXOSNAP_EXPECTED_AUTHENTICODE_SUBJECT` (+ optional Issuer; CMake, wie
  `EXOSNAP_UPDATE_PUBLIC_KEY_HEX`); leer → Check übersprungen. **Subject/Issuer-Pin statt Thumbprint**
  (D5-Rotations-Argument): überlebt routinemäßige Cert-Erneuerung.
- Einhängen in `UpdaterWorker`: MSI-Pfad vor `msiexec` (nach 542, durch das Lock-Handle, starke
  Garantie); Portable-Pfad in `runInstallPortable` **unmittelbar vor** `StageRename` (499) — deckt
  **beide** Staging-Ursprünge (Download 376 **und** Retry-Re-Staging 486) ab; Best-Effort (D5-TOCTOU).
- **Test:** Unit-Test mit Fixtures — (a) mit Test-Cert signierte PE, deren Test-Root im Trust-Store
  liegt → `Ok`; (b) unsigniert → `AuthenticodeUntrusted` bei gesetztem Gate, `Ok`/skip bei leerem
  Gate; (c) valid signiert, aber falsches Subject/Issuer → `AuthenticodeUntrusted`.
- **Test-Root-Trust-Strategie (adversarialer Review):** `New-SelfSignedCertificate` erzeugt ein Cert,
  das auf eine **nicht vertrauenswürdige Root** chained →
  `WinVerifyTrust(WINTRUST_ACTION_GENERIC_VERIFY_V2)` liefert `CERT_E_UNTRUSTEDROOT`, d. h. Fixture
  (a) schlüge ohne Vorkehrung fehl. Das Fixture-Setup **installiert die Test-Root in den
  `CurrentUser\Root`-Store** (programmatisch, **ohne Admin/keine Runner-Machine-Store-Mutation**) und
  **entfernt sie im Teardown**. `VerifyAuthenticode` verlangt bewusst **beides** — gültige
  WinVerifyTrust-Chain **und** Subject/Issuer-Match —, damit ein bloßer Subject-Treffer ohne
  Trust-Chain nicht als `Ok` durchgeht. Damit ist der Test **CI-fähig ohne Prod-Cert**.

### S6 — `release-checklist.md`: Signier-Punkte
Neuer Abschnitt „Signing verification" (Abschnitt 2/3):
- [ ] Signier-Aufrufe liefen und die SignPath-**Approver-Freigabe** (D8a) wurde erteilt.
- [ ] `Get-AuthenticodeSignature` auf MSI + `exosnap.exe` + `exosnap-updater.exe` = `Valid`, Subject
  = **„SignPath Foundation"** (D7 — nicht „Codexo"/„ExoSnap").
- [ ] Signatur trägt einen RFC-3161-**Timestamp** (überlebt Cert-Ablauf).
- [ ] **Herausgeber-Identität (Subject+Issuer) unverändert** ggü. dem letzten Release. **Bei
  Foundation-Cert-Rotation** (außerhalb Projektkontrolle): S5-Pin ist Subject/Issuer, nicht
  Thumbprint → Rotation bricht ausgelieferte Clients **nicht** (D5). Falls eine Thumbprint-Allowlist
  genutzt wird: neuen Thumbprint **vor** dem Wechsel im Übergangsfenster ausgerollt?
- [ ] **Foundation-Auflagen erfüllt (D8):** Homepage-„Code signing policy" mit Credit + Team-Rollen
  online (b); **MFA** für alle Team-Mitglieder aktiv (c); SignPath-**Artifact-Configuration** deckt die
  signierte Datei-Menge ab (d).
- [ ] **User-live:** frischer Download auf einer Maschine ohne Vor-Reputation — UAC/SmartScreen zeigt
  **„SignPath Foundation"** als verifizierten Herausgeber (nicht „unbekannt"); ob die
  SmartScreen-Warnung erscheint, hängt vom Reputationsstand der **geteilten** Foundation-Identität ab
  (D7 — beobachten, kein Fehler).

---

## Test-/Verify-Plan

**CI-fähig (automatisiert):**
- D1-Selektions-Predicate (rein, Pester-Unit).
- Struktur/Gating des Signier-Steps + Fork-Skip (der `build`-Job läuft ohne Token durch, überspringt
  das Signieren). **Kein** 3-Job-Artefakt-Roundtrip mehr (D4-Revision).
- Skript-Lauf im PR-Packaging-Smoke (unsigniert) grün; in der Fallback-Variante zusätzlich die
  Phasen-Modi (`-StageOnly`/`-BuildMsiOnly`/`-FinalizeMsi`).
- S5-Updater-Authenticode-Unit-Tests mit **selbst erzeugtem Test-Cert** (kein Prod-Cert), dessen
  **Test-Root im `CurrentUser\Root`-Store** installiert + im Teardown entfernt wird (sonst
  `CERT_E_UNTRUSTEDROOT`): signiert+getrusted → Ok; unsigniert/falsches Subject+Issuer → untrusted bei
  gesetztem Gate; leeres Gate → skip.
- `Get-AuthenticodeSignature`-Audit im `build`-Job (greift, sobald echt signiert; erwartet Subject
  „SignPath Foundation").

**Nur User/Maintainer-live (nicht CI-fähig):**
- Der echte SignPath-Signier-Round-Trip (braucht provisioniertes HSM-Cert + Policy-Approval).
- **SmartScreen-/UAC-Verhalten** auf einem realen Erst-Download: verifizierter Herausgeber sichtbar;
  ob/wann die SmartScreen-Warnung verschwindet (Reputationsaufbau) ist beobachtbar, nicht messbar in
  CI.
- MSI-/ZIP-Smoke auf den **signierten** Artefakten (Cert erforderlich).

**Bewusst NICHT gebaut:**
- **EV-Cert** und der zugehörige Sofort-Reputationsbonus (D7 — kosten/nutzen pre-1.0 nicht
  gerechtfertigt).
- **Re-Signieren gültig-fremdsignierter Qt-DLLs** (D1 — Provenienz-Erhalt).
- Signieren der **ZIP** als Container (technisch unmöglich — Authenticode kennt kein ZIP).
- Kernel-/Treiber-Signierung, Cross-Signing (ExoSnap liefert keine Treiber).

---

## Risiken

- **SignPath-Zusage verzögert/scheitert** → Signieren blockiert. Mitigation: D6-Fallback-Leiter; die
  Provider-Abstraktion (`Invoke-SignArtifacts`) hält den Umbau lokal.
- **Herausgeber-Name ist „SignPath Foundation", nicht ExoSnap/Codexo** (D7). Kein technisches Risiko,
  aber eine Produkt-/Marken-Entscheidung: fremder verifizierter Name gratis vs. eigener Name gegen
  Geld (Offene Frage 1). Mitigation: D7-ehrliche Doku; Azure/OV als D6-Leiter, falls eigener Name
  gefordert wird.
- **Approval-Latenz mitten im Workflow** (D8a): der blockierende Signier-Aufruf kann den
  `-WaitForCompletion`-Timeout überschreiten. Mitigation: großzügiger Timeout + Resume über
  Request-Id (`Get-SignedArtifact`); GitHub-Job-Max 6 h beachten.
- **Foundation-Auflagen unerfüllt** (D8b/c/d): fehlende Homepage-Policy, fehlende MFA oder
  nicht-passende Artifact-Config → Request abgelehnt oder Programm-Verstoß. Mitigation: S2-Vorab-Setup
  + S6-Checkliste.
- **Doppelte Signier-Runde** erhöht Release-Latenz und Kontingent-Verbrauch (zwei Submits).
  Mitigation: Tier-2 quotenschonend (nur unsignierte PEs); MSI ist der zweite, unvermeidbare Submit.
  (Der frühere „Async-Job-Split"-Risikopunkt entfällt — D4-Revision: ein Job, kein
  Artefakt-Roundtrip.)
- **Ordering-Regression:** wenn je wieder gehasht wird, bevor signiert ist, verweigert der Updater
  jedes Update (invalidierter Manifest-Hash). Mitigation: der `EXOSNAP_EXPECT_SIGNED`-Verify-Schritt
  (S1) fängt „unsigniert trotz erwartet" ab, bevor gehasht/veröffentlicht wird.
- **Reputationsaufbau** (OV): unklar, ob die geteilte Foundation-Identität schon „warm" ist (D7).
  Mitigation: ehrliche Kommunikation; Identität stabil halten; ggf. später EV erwägen.
- **S5-Gate falsch gesetzt / Cert-Rotation** (Subject/Issuer eingebacken, aber Release anders signiert;
  **oder** Foundation rotiert das Cert außerhalb Projektkontrolle) → der Updater lehnt legitime Updates
  ab. Mitigation: **Subject+Issuer-Pin statt Thumbprint** (D5) übersteht Erneuerung; Gate nur setzen,
  wenn der Signier-Pfad steht; S6-Checklist verankert die Rotations-Prüfung.

---

## Offene Fragen (echte Produktentscheidungen)

1. **Fremder vs. eigener Herausgeber-Name (ersetzt die alte „Codexo oder ExoSnap?"-Frage).** Unter
   SignPath Foundation ist der Herausgeber **zwingend „SignPath Foundation"** (D7) — die Wahl
   „Codexo vs. ExoSnap" existiert dort **nicht**. Die echte Entscheidung: **fremder verifizierter Name
   gratis** (Foundation) **akzeptieren**, oder für einen **eigenen** Namen (Azure Trusted Signing /
   kommerzielles OV, D6) **zahlen**? Falls eigener Name gefordert wird, verschiebt sich der Primär-
   Provider — und erst dann wird „Codexo vs. ExoSnap" als Subject wieder relevant.
2. **Falls SignPath scheitert / eigener Name gewünscht — Budget für den Fallback?** Azure Trusted
   Signing (~10 $/Monat) bzw. ein kommerzieller OV-Cert (~200–400 $/Jahr) sind kostenpflichtig.
   Freigabe nötig, plus Klärung der **3-Jahre-Rechtsträger-Alters-Hürde** von Azure Trusted Signing für
   den ExoSnap/Codexo-Org.
3. **Updater-Authenticode-Gate (S5) hart oder advisory im ersten signierten Release?** Vorschlag:
   erste Releases advisory (loggen, nicht ablehnen), bis die Herausgeber-Identität über mehrere
   Releases stabil bewiesen ist — dann hart. Ist das der gewünschte Vorsichtsgrad?
4. **EV pre-1.0 endgültig vom Tisch,** oder soll die Sofort-Reputation + der eigene Name eines EV-Certs
   (Kosten) für den 1.0-Launch nochmals bewertet werden?
5. **Vendor-Frage (D2/D4-Integration):** Akzeptiert die SignPath-**Open-Source-Policy** vom
   **PS-Modul** manuell gesetzte `-Origin`-Metadaten aus GitHub Actions, oder erzwingt sie den
   **GitHub-Action-Konnektor** für die Origin-Verification? Antwort entscheidet zwischen der
   Inline-Primärvariante (ein PS-Aufruf) und der Fallback-Variante (Action als Steps im selben Job +
   `-BuildMsiOnly`/`-FinalizeMsi`). Beide **ohne** Cross-Job-Artefakt.

---

## Adversarialer Review — Ergebnis

Alle sieben Einwände wurden gegen Code (`UpdaterWorker.cpp`, `build-release-artifacts.ps1`, ADR 0012)
und die Foundation-Bedingungen (`signpath.org/terms.html`, `docs.signpath.io`) selbst geprüft und
**eingearbeitet**.

1. **[blocker → eingearbeitet]** Herausgeber = **„SignPath Foundation", nicht ExoSnap/Codexo** — durch
   die Foundation-Terms wörtlich bestätigt. D7 komplett neu (falsche „ExoSnap/Codexo"-Behauptung
   gestrichen); Offene Frage 1 umformuliert (Trade-off fremder-Name-gratis vs. eigener-Name-bezahlt);
   S6-Subject-Check + S5-Pin auf die fremde Identity umgestellt; Reputations-Frage (geteiltes
   Foundation-Cert, plausibel warm) in D7 aufgenommen.
2. **[major → eingearbeitet]** Die Prämisse „nicht inline signierbar" ist falsch —
   `Submit-SigningRequest -WaitForCompletion -OutputArtifactPath` (Default 600 s) verifiziert. D2/D4
   auf **Inline-PS-Modul, ein Job, kein Artefakt-Roundtrip** umgestellt; der vom Einwand geforderte
   Vorbehalt (Origin-Verification der Open-Source-Edition) als Offene Frage 5 + Fallback-Variante
   verankert.
3. **[major → eingearbeitet]** MSI-Block (Hash 767 direkt nach `wix build` 764, dann Assertion+Smoke in
   einem try-Block) im Code bestätigt. S3 sagt jetzt ehrlich „Abschnitt 6 real aufbrechen": Inline-
   Einfügung vor 766 **oder** dritter Modus `-BuildMsiOnly`/`-FinalizeMsi` (nicht nur „ein Aufruf").
4. **[major → eingearbeitet]** Alle vier Foundation-Prozesspflichten (manuelle Approval, Homepage-
   Policy+Team-Rollen, MFA, vorab registrierte Artifact-Config) durch die Terms bestätigt — als neues
   **D8** plus Aufnahme in S2/S4/S6/Risiken; Approval-Latenz + Resume-Pfad in D4.
5. **[minor → eingearbeitet]** `New-SelfSignedCertificate` → `CERT_E_UNTRUSTEDROOT` korrekt. S5-Test
   legt jetzt die Root-Trust-Strategie fest (Test-Root in `CurrentUser\Root`, Setup+Teardown, kein
   Admin) und definiert `VerifyAuthenticode` als „Trust-Chain UND Subject/Issuer-Match".
6. **[minor → eingearbeitet]** Thumbprint-Pin bricht bei Cert-Rotation (bei Foundation außerhalb der
   Kontrolle). Pin auf **Subject+Issuer** umgestellt (`EXOSNAP_EXPECTED_AUTHENTICODE_SUBJECT`),
   optionaler Thumbprint-Allowlist mit Übergangsfenster; Rotations-Fall in Risiken + S6.
7. **[minor → eingearbeitet]** Zwei Staging-Pfade (376/486) und die ungelockten entpackten PEs im Code
   bestätigt; ADR 0012:132–142 gilt nur für die gelockte ZIP. D5 benennt den portablen Check jetzt
   ehrlich als **Best-Effort** (TOCTOU-Fenster, kein Verify-under-Lock) und platziert ihn vor
   `StageRename` (499), wo beide Staging-Ursprünge zusammenlaufen.

# Deferred Audio: 5.1/7.1, Float-PCM, PCM/FLAC in MP4

## Problem

Drei Audio-Themen wurden im 0.6.0-Audio-v2-Wave (ADR 0030) bewusst deferred, weil sie
außerhalb des damaligen Scope lagen: (1) mehr als zwei Kanäle (5.1/7.1), (2) Float-PCM
(`A_PCM/FLOAT_IEEE`), (3) PCM/FLAC in MP4 (das `ipcm`-Problem). Sie stehen seither
unverändert in `KNOWN_LIMITATIONS.md` und `docs/roadmap.md` als offene Posten. Diese Spec
erhebt den echten Ist-Zustand nach den seit dem Review gemergten PRs #164–#192, bewertet
für jedes der drei Themen Aufwand und Nutzen ehrlich gegeneinander, und trifft für jedes
eine 1.0-vs-post-1.0-Entscheidung, damit eine spätere Umsetzung ohne weitere
Grundlagenrecherche starten kann.

Die drei Themen sind technisch unabhängig voneinander (kein Thema blockiert ein anderes),
werden aber in einer Spec zusammengefasst, weil sie alle denselben Teil des Systems
berühren: das Kanal-/Sample-Format-Modell aus ADR 0030 und den Audio-Encoder-/Mux-Layer.

## Ist-Zustand

### A. Capture-Layer: WASAPI erzwingt heute überall Stereo/48 kHz

Alle drei Audioquellen fordern beim `IAudioClient::Initialize()` explizit ein festes
48 kHz/2-Kanal-Format an — unabhängig vom nativen Format des Endpoints. Rate und
Kanalzahl sind bei allen dreien identisch fixiert; **das Sample-Format weicht bei APP
ab** (Korrektur siehe unten):

- **SYS-Loopback** (`libs/recorder_core/src/wasapi_loopback.cpp:57-58,120-132`):
  `kRequiredSampleRate = 48000`, `kRequiredChannels = 2`. Der Kommentar an Zeile 106-112
  dokumentiert es explizit: *"The render endpoint's shared-mode mix format is whatever the
  user has configured (e.g. a 44.1 kHz DAC, or a 5.1/7.1 speaker layout) — it is not
  guaranteed to be 48 kHz/stereo. Rather than rejecting those endpoints, request our fixed
  48 kHz/2 ch float format and set `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`."* D. h.: ein
  5.1/7.1-Wiedergabegerät wird von Windows' eigenem Audio-Engine-Resampler/-Downmixer
  **bereits vor der Übergabe an ExoSnap** auf Stereo heruntergemischt. ExoSnap sieht nie
  die diskreten 5.1/7.1-Kanäle.
- **APP-Prozess-Loopback** (`libs/recorder_core/src/wasapi_process_loopback_src.cpp:318-334`):
  Rate/Kanäle fest auf 48 kHz/2 ch wie bei SYS, `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`
  gesetzt — **aber das angeforderte Sample-Format weicht ab**: `fmt.wFormatTag =
  WAVE_FORMAT_PCM` mit `wBitsPerSample = 16` (Zeile 319/322), nicht
  `WAVE_FORMAT_IEEE_FLOAT`/32 wie SYS (`wasapi_loopback.cpp:121-125`) und MIC
  (`wasapi_capture_src.cpp:329-333`). APP liefert also Int16-PCM an
  `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM`, nicht Float32 — "identisches Muster" gilt nur
  für Rate/Kanäle/`AUTOCONVERTPCM`, nicht für das Sample-Format. Ändert nichts an den
  Schlussfolgerungen unten (der Mixbus konvertiert ohnehin jede Quelle auf Float32,
  bevor Float-PCM überhaupt relevant wird), korrigiert aber die Prämisse.
- **MIC** (`libs/recorder_core/src/wasapi_capture_src.cpp:20,330-379`):
  `kRequiredOutputChannels = 2`. Das native Eingangsformat wird zwar erkannt
  (`selectedInputChannels`, Zeile 341-379), aber unabhängig davon immer auf 2 Kanäle
  gemappt (`MicChannelMode`-Logik, Zeile 79-165) — Mikrofone liefern in der Praxis ohnehin
  nie mehr als Stereo.

**Konsequenz:** Es gibt heute **keinen** Pfad, auf dem ExoSnap tatsächlich diskrete
Mehrkanal-PCM-Daten von Windows empfängt. Das ist keine reine UI-Beschränkung, sondern
eine Entscheidung auf der WASAPI-`Initialize()`-Ebene, drei Dateien tief.

### B. Mixbus: `MixedAudioSrc` ist intern hart auf Stereo verdrahtet

`libs/recorder_core/src/mixed_audio_src.h:39-41`:
```cpp
static constexpr uint32_t kMixFrameCount = 480;
static constexpr uint32_t kOutputSampleRate = 48000;
static constexpr uint32_t kOutputChannels = 2;
```
`kOutputChannels` ist an über einem Dutzend Stellen in `mixed_audio_src.cpp` verwendet
(Puffergrößen, FIFO-Adressierung, `ConvertToFloat32Stereo`-Downmix). Wichtiger
Ist-Zustand-Fund, der vom Review abweicht: **H-1 (Frame-Count-Bug) ist bereits behoben** —
der Klassenkommentar in `mixed_audio_src.h:18-33` beschreibt jetzt explizit
sample-count-erhaltendes "min-of-ready"-Mixing statt der alten festen 480-Frame-Emission;
das war im Review noch als offener Bug (H-1) vermerkt. Für dieses Thema bleibt relevant:
der Mixer selbst kennt nur zwei Kanäle, nicht N.

### C. Format-Modell nach dem Mixbus (ADR 0030, 0.6.0, aktueller Stand)

`libs/capability/include/capability/audio_ui_state.h:106-122` trägt `audio_sample_rate`
(44100/48000/96000), `audio_channels` (nur 1 oder 2), `audio_bit_depth` (16/24/32) und
`flac_compression_level`. `BuildAudioPlan()` (`libs/capability/src/audio_ui_state.cpp:58-62`)
reicht die vier Felder unverändert durch — **keine Sanitisierung/Clamping auf der
`libs/capability`-Resolver-Ebene selbst** (`SettingsResolver`/`BuildAudioPlan`):

- `app/pages/ConfigPage.cpp:3961-3965` — die Channels-Combobox bietet hart nur
  `"Stereo"` (2) und `"Mono"` (1) an; es gibt keinen dritten Eintrag.
- `app/pages/ConfigPage.cpp:3572-3591` — Opus snapt `audio_sample_rate` im UI-Code auf
  48000, wenn Opus aktiv ist (nicht im Resolver).

**Korrektur (Review-Einwand):** Die ursprüngliche Formulierung "das Clamping passiert
ausschließlich in der UI" war zu weitgehend und faktisch falsch — es gibt eine dritte,
tiefere Ebene: `libs/recorder_core/src/recorder_session.cpp:249-279` validiert in der
`Prepare`-Phase des Engines hart (nicht nur klammend, sondern mit hartem Reject via
`E_INVALIDARG`): `audio_channels ∈ {1, 2}` (Zeile 254-256), `audio_sample_rate` gegen
das vetted Set `{44100, 48000, 96000}` (Zeile 259-261), den Opus-48-kHz-Lock
(Zeile 264-266) und `audio_bit_depth` codec-gated (`Pcm` → `{16, 24, 32}`,
`Flac` → `{16, 24}`, Zeile 269-281). Das ist exakt das ADR-0030-Muster "die Engine
schützt sich selbst gegen inkonsistente Configs" — es existiert bereits, nur nicht auf
der `capability`-Resolver-Ebene, sondern eine Schicht tiefer in `recorder_core`. Für das
neue `audio_pcm_float`-Feld (Thema 2) muss dieselbe Schicht erweitert werden, sonst ist
sie die einzige der drei Konsistenz-Schichten (UI, Preset-Load, Engine-Prepare), die das
neue Feld nicht kennt — siehe neuer Punkt in Schritt 1 unten.

Das widerspricht dem in ADR 0030 beschriebenen Soll ("Sanitization clamps each field to
its codec-gated vetted set") nur oberflächlich auf Resolver-Ebene: die Werte-Räume sind
so klein (2 Sampleraten-Optionen minus Opus-Lock, 2 Kanal-Optionen), dass UI +
Engine-Prepare sie bisher gemeinsam abdecken, ohne dass der `capability`-Resolver selbst
etwas beitragen muss. Für 5.1/7.1 reicht das nicht mehr — dazu unten mehr.

`OutputFormatAudioSrc` (Decorator, `libs/recorder_core/src/output_format_audio_src.*`)
wandelt per `SwrContext` **Sample-Rate und Kanalzahl** um; laut ADR 0030 rematrixiert
swresample bereits heute mit "standard ITU downmix coefficients" für Stereo↔Mono. Die
Klasse ist die architektonisch richtige Stelle für eine spätere N-Kanal-Erweiterung —
`swr_convert` beherrscht beliebige `AVChannelLayout`-zu-`AVChannelLayout`-Konvertierung,
nicht nur Stereo/Mono.

### D. Encoder-Layer: jeder Encoder ist einzeln auf ≤2 Kanäle begrenzt

- **PCM** (`libs/recorder_core/src/pcm_audio_encoder.cpp:72-80`): `Init()` nimmt
  `channels` entgegen und speichert es unverändert (`m_channels = channels`) — **kein
  Channel-Limit im Code selbst**. `FeedFloat32` konvertiert Sample für Sample
  unabhängig von der Kanalzahl. PCM ist also bereits N-Kanal-fähig, *sobald* Daten mit
  N Kanälen ankommen.
- **FLAC** (`libs/recorder_core/src/flac_audio_encoder.cpp:109-172`): ruft
  `FLAC__stream_encoder_set_channels(enc, channels)` durch — libFLAC unterstützt nativ
  1–8 Kanäle mit einer **festen, in der FLAC-Spezifikation definierten
  Default-Kanalreihenfolge pro Kanalzahl** (z. B. 6 Kanäle = FL,FR,C,LFE,BL,BR). Auch
  FLAC ist im Code bereits N-Kanal-fähig (bis 8).
- **Opus** (`libs/recorder_core/src/opus_audio_encoder.cpp:50-108`): `opus_encoder_create()`
  — die von ExoSnap verwendete **Single-Stream-API** — akzeptiert laut libopus-API nur
  `channels ∈ {1, 2}`. Mehr als 2 Kanäle erfordern die **Opus-Multistream-API**
  (`opus_multistream_encoder_create`) mit einer expliziten Kanal-Mapping-Tabelle
  (RFC 7845 §5.1.1, "Vorbis channel order", *nicht* WAV-Kanalreihenfolge). Der
  aktuelle Code hat davon nichts: `codec_private.cpp:458-480`
  (`BuildOpusCodecPrivate`) schreibt einen festen 19-Byte-`OpusHead` mit
  `mapping_family = 0` (Byte 18, Zeile 479) — Mapping-Family 0 ist per RFC **nur für
  1–2 Kanäle gültig**. Für >2 Kanäle bräuchte es Mapping-Family 1 (Vorbis-Reihenfolge,
  Stream-/Coupled-Count + Kanal-Mapping-Tabelle als zusätzliche Bytes im Header) oder
  Family 255 (applikationsdefiniert).
- **AAC** (`libs/recorder_core/src/fdk_aac_encoder.cpp:69`): `AACENC_CHANNELMODE` wird
  hart auf `MODE_2` (Stereo) oder `MODE_1` (Mono) gesetzt — jeder andere Wert von
  `channels` würde denselben `AACENC_CHANNELMODE` bekommen wie Stereo, was für FDK-AAC
  falsch ist. FDK-AAC hat eigene `CHANNEL_MODE`-Konstanten für 5.1
  (`MODE_1_2_2_2_1`) und 7.1 (mehrere Varianten je nach Rear-/Front-Surround-Layout) —
  keine davon ist verdrahtet.

### E. Mux-Layer

- **Matroska** (`libs/recorder_core/src/matroska_stream_writer.cpp:338-393`): schreibt
  `KaxAudioChannels` als reine Zahl (Zeile 390) — **kein** `KaxAudioBitDepth`-Sonderfall
  für Float, und **kein** Konzept einer Kanal-Layout-Angabe über die reine Zahl hinaus.
  Der `CodecID`-String wird pro `StreamAudioCodec`-Enum-Wert fest gewählt: `"A_OPUS"`
  (Z. 354), `"A_PCM/INT_LIT"` (Z. 365, **hart codiert, kein Float-Zweig**), `"A_FLAC"`
  (Z. 375), `"A_AAC"` (Z. 379). Für Float-PCM fehlt schlicht ein `"A_PCM/FLOAT_IEEE"`-Zweig.
- **MP4-Remux** (`libs/recorder_core/src/mp4_remuxer.cpp`): generischer
  Stream-Copy-Remuxer über libavformat (`avformat-62.dll`, gepinnt in
  `cmake/VendorFFmpeg.cmake:15,73`, aus dem projekteigenen Build-Repo
  `Exoridus/exosnap-ffmpeg-build`, Release `r3`). Für HEVC-in-MP4 überschreibt der Code
  bereits explizit `out_st->codecpar->codec_tag = MKTAG('h','v','c','1')`
  (`mp4_remuxer.cpp:230`) **bevor** `avformat_write_header()` läuft, um den
  libavformat-Default (`hev1`) zu übersteuern — mit Kommentar, warum (Apple/QuickTime-
  Kompatibilität) und Verweis auf ADR 0010/0014. Für PCM gibt es **keinen** analogen
  Codec-Tag-Override; `out_st->codecpar->codec_tag = 0` (Zeile 218) lässt libavformat
  den Default wählen — und der ist laut ADR 0030 (live mit `ffprobe` verifiziert) `ipcm`
  (ISO/IEC 23003-5), nicht die breit kompatiblen QuickTime-Sample-Entries
  (`sowt`/`in24`/`lpcm`).
- **Resolver-Policy** (`libs/capability/src/container_compat_registry.cpp`, seit #190
  die Quelle für Container×Video×Audio-Kompatibilitäts-**Empfehlungen/Labels**
  (`ContainerCompatLevel` je Kombination)): MP4 + PCM ist für **jede** Video-Codec-
  Kombination `Experimental` (Zeilen 168-174, 185-187, 196-197) mit demselben
  `ipcm`-Grund im Kommentar; MP4 + FLAC ebenso durchgängig `Experimental`.
  `ReconcileCodecs()` (Zeile 264-305) behandelt `Experimental` wie `Prohibited` — beide
  zählen nicht als `IsWorkingCombo` — und reconciled automatisch auf AAC. Diese Regel ist
  bereits korrekt und muss für PCM/FLAC-in-MP4 nur an den betroffenen Query-Zeilen auf
  `Allowed`/`Recommended` angehoben werden, sobald eine funktionierende Sample-Entry-
  Zuordnung verifiziert ist — keine Struktur-Änderung am Resolver nötig.

  **Korrektur (Review-Einwand, Blocker):** "alleinige Quelle für Kompatibilität" ist
  falsch — es gibt eine **zweite, unabhängige Whitelist** in
  `libs/capability/src/translation.cpp:39-96` (`ToRecorderCoreConfig()`), die die
  Container×Video×Audio-Kombination ein zweites Mal hart als Bool-Flag-Liste
  durchdekliniert (`is_mp4_h264_aac`, `is_mp4_hevc_aac`, `is_mkv_h264_pcm`, …) und jede
  nicht gelistete Kombination mit `throw std::invalid_argument(...)` verwirft
  (Zeile 82-96; die Meldung nennt explizit nur "MP4+(H264|HEVC)+AAC" für MP4). Es gibt
  aktuell **kein** `is_mp4_*_pcm`. Würde `container_compat_registry.cpp` per Schritt 5
  auf `Allowed` angehoben, ohne `translation.cpp` gleichzeitig zu erweitern, wäre
  MP4+PCM in der UI wählbar (Resolver sagt "geht"), aber `ToRecorderCoreConfig()` würde
  beim Aufnahmestart eine Exception werfen — ein zweiter, unabhängiger Gate, der nicht
  automatisch mit der Registry mitzieht. `container_compat_registry.cpp` ist also die
  Quelle für die **UI-Empfehlung/Kompatibilitätsanzeige**, `translation.cpp` ist ein
  **zweites, separates Gate** auf dem tatsächlichen Aufnahme-Pfad — beide müssen bei
  jeder neuen Freigabe synchron gehalten werden. Konsequenz für Schritt 5 unten.
- **`AudioCodec`-Enum** (`libs/capability/include/capability/config_types.h:11`):
  `enum class AudioCodec { Opus, AacMf, Pcm, Flac }` — geschlossen, vierwertig, an jedem
  `switch` im Resolver (`container_compat_registry.cpp`, `translation.cpp`) durchdekliniert.
  Ein neuer fünfter Wert für Float-PCM würde jeden dieser `switch`-Blöcke berühren.

### F. UI-Labels

`app/ui/CodecLabels.h:69-95` — `audioCodecLabel()` kennt nur die vier bestehenden
`AudioCodec`-Werte; PCM wird immer als `"PCM"` angezeigt (Casing-Kanon,
feedback_codec_naming_canon). Kein Float-spezifisches Label vorhanden.

### G. Preset-Schema

`app/models/RecordingPreset.h:45`: `kPresetSchemaVersion = 23` (Stand nach #164–#192,
höher als die 16 aus ADR 0030 — seither mehrfach gestiegen). Pre-1.0-Policy gilt
unverändert: inkompatible Presets werden zurückgesetzt, nicht migriert
(`feedback_prerelease_breaking_changes`).

## Design

Die drei Themen werden einzeln bewertet; sie sind unabhängig implementierbar und werden
nicht als ein Gesamtpaket verstanden.

---

### Thema 1 — 5.1/7.1-Mehrkanalaufnahme

**Alternativen:**

**(A) Vollständige Mehrkanal-Pipeline** — WASAPI-Quellen fordern das native
Endpoint-Format statt fest 48 kHz/Stereo an (`GetMixFormat()` statt hartcodierter
`WAVEFORMATEX`), `MixedAudioSrc` wird auf `AVChannelLayout`-generisch umgebaut,
`OutputFormatAudioSrc` rematrixiert N→M Kanäle statt nur Stereo↔Mono, jeder Encoder
bekommt einen echten Mehrkanalpfad (Opus-Multistream + Mapping-Family-1-Header, FDK-AAC
`CHANNEL_MODE` pro Kanalzahl, PCM/FLAC sind bereits fähig), UI bekommt 5.1/7.1-Einträge
in der Channels-Combobox, gated per Codec (FLAC max 8, Opus via Multistream, AAC via
FDK-Modi, PCM immer).

**(B) Nur der SYS/APP-Loopback-Pfad bekommt echte Mehrkanalfähigkeit; MIC bleibt fest
bei ≤2 Kanälen** — dieselbe Pipeline-Arbeit wie (A), aber MIC (`wasapi_capture_src.cpp`)
bleibt unverändert, weil Mikrofone in der Praxis nie mehr als Stereo liefern. Ein
"Merge with above" von MIC in einen 5.1-SYS-Track müsste dann definiert werden: entweder
verboten (Merge-Checkbox deaktiviert, wenn Kanalzahlen der beteiligten Quellen
divergieren) oder MIC wird beim Merge auf die Zielkanalzahl hochgemischt (z. B. Mono-MIC
→ Center-Kanal). Realistischere, kleinere Variante von (A).

**(C) Nichts bauen — deferred bleiben** — Status quo, mit einer präziseren
Dokumentation der Root-Cause (der WASAPI-`Initialize()`-Zwang, nicht nur "UI bietet es
nicht an").

**Abwägung:**

5.1/7.1 ist **kein Bugfix, sondern eine neue Fähigkeit über die gesamte Pipeline**: die
Ist-Zustand-Recherche zeigt, dass *jede* der vier Schichten (WASAPI-Capture, Mixbus,
mindestens zwei der vier Encoder, Matroska-Kanalreihenfolgen-Konvention) eigene, nicht
triviale Änderungen bräuchte, und dass die Kanalreihenfolgen-Konvention selbst ein
Fehlerquellen-Minenfeld ist:

- **Opus** erwartet für >2 Kanäle die *Vorbis*-Kanalreihenfolge (RFC 7845 §5.1.1; für
  5.1 z. B. FL, FC, FR, RL, RR, LFE — Center *vor* Right).
- **FLAC** hat seine *eigene* feste Kanalreihenfolge pro Kanalzahl
  (libFLAC-Spezifikation; für 6 Kanäle FL, FR, C, LFE, BL, BR — Center *nach* Right).
- **WAV/PCM-Konvention** (die, die WASAPI über `dwChannelMask`/`WAVEFORMATEXTENSIBLE`
  liefert) folgt wiederum ihrer eigenen SMPTE-nahen Reihenfolge.

Drei verschiedene Kanalreihenfolgen für dieselben physikalischen Lautsprecherpositionen
bedeuten: ein einziger "Kanalzahl"-Parameter reicht nicht — jeder Encoder-Pfad braucht
eine **explizite Remapping-Tabelle** von der WASAPI-Quellreihenfolge auf seine eigene
Zielreihenfolge, sonst landet z. B. bei Opus der Center-Kanal auf dem Right-Kanal-Slot.
Das ist in keinem der drei Encoder heute vorbereitet und ist der Kern des Aufwands, nicht
Beiwerk.

Dazu kommt eine **Nutzennutzenfrage**: ExoSnap ist ein Desktop-/Spiele-/Streaming-
Recorder. Die realistische Nutzerpopulation für "System-Audio in echtem 5.1/7.1
aufnehmen" ist klein — die meisten Spiele/Anwendungen rendern ohnehin auf Stereo, echte
diskrete Mehrkanal-Ausgabe über WASAPI-Loopback ist die Ausnahme, nicht die Regel
(Heimkino-Setup mit tatsächlich 5.1/7.1-Lautsprechern und einer Anwendung, die das
Windows-Mixformat auch wirklich mehrkanalig nutzt). Es gibt heute **keine dokumentierte
Nutzeranfrage** für dieses Feature in den Roadmap-/KNOWN_LIMITATIONS-Referenzen — es
steht dort ausschließlich als technische Ehrlichkeits-Notiz, nicht als priorisierter
Wunsch.

**Entscheidung: (C) — explizit NICHT für 1.0, bleibt post-1.0-Backlog-Posten ohne
festen Termin.** Begründung: großer, über vier Schichten verteilter Umbau (WASAPI-Init,
Mixbus, zwei Encoder-Pfade mit eigenen Kanalreihenfolgen-Remapping-Tabellen, UI), hohes
Risiko stiller Kanalvertauschungs-Bugs (falscher Lautsprecher bekommt falsches Signal —
schwer zu testen ohne echte 5.1/7.1-Hardware), gegen eine kleine, unbelegte
Nutzerpopulation abgewogen. Das ist gegenteilig zu "kleine/reversible Enabler einfach
tun" (feedback_be_decisive) — dies ist kein kleiner Enabler. Sollte das Feature künftig
priorisiert werden, ist Alternative (B) (SYS/APP mehrkanalig, MIC bleibt Stereo-Cap) die
richtige Zielarchitektur, weil sie den größten Umbauteil (MIC-DSP-Kette, die komplett auf
Stereo ausgelegt ist — `wasapi_capture_src.cpp` MicChannelMode-Logik) unangetastet lässt.

Die **Implementierungsschritte unten sind trotzdem ausformuliert** (als Backlog-Skizze),
damit eine spätere Umsetzung nicht bei null anfängt — sie sind nicht Teil der
1.0-Deliverables.

---

### Thema 2 — Float-PCM (`A_PCM/FLOAT_IEEE`)

**Alternativen:**

**(A) Neuer `AudioCodec`-Enum-Wert** (`AudioCodec::PcmFloat`) — würde in jedem
`switch (audio_codec)` im Resolver (`container_compat_registry.cpp`,
`translation.cpp`), in `CodecLabels.h`, in `ConfigPage.cpp`s Codec-Auswahl und im
Preset-Schema als neuer Wert auftauchen. Spiegelt PCM/FLAC als eigenständige Codecs.

**(B) Orthogonales Flag am bestehenden PCM-Codec** (`bool audio_pcm_float` bzw.
`enum class PcmSampleFormat { Int, Float }`), analog zum bereits existierenden
`audio_bit_depth`-Feld — sichtbar nur wenn `AudioCodec::Pcm` gewählt ist, mit
`bit_depth` bei Float fest auf 32 gesetzt (32-bit IEEE754 ist das einzige Format, das
der Mixbus nativ liefert — Float64 wäre eine Scheinoption ohne echten Quellwert).

**(C) Nichts bauen.**

**Abwägung:**

Float-PCM ist – anders als 5.1/7.1 – ein **kleiner, isolierter Zusatz auf einer bereits
gebauten Fähigkeit**, kein neuer Systempfad: `PcmAudioEncoder::FeedFloat32` bekommt die
Daten schon als Float32 vom Mixbus (`OutputFormatAudioSrc` liefert immer Float32,
ADR 0030) — der 32-bit-Float-Pfad ist tatsächlich **einfacher** als die bestehenden
Int16/Int24/Int32-Pfade, weil er *keine* Konvertierung braucht (reines Byte-Kopieren der
bereits vorliegenden Float32-Samples statt `Float32ToS16`/`Float32ToS24LE`/`Float32ToS32`).
Der einzige neue Bestandteil ist der `"A_PCM/FLOAT_IEEE"`-Zweig im Matroska-Writer.

Alternative (A) würde denselben Nutzen mit deutlich mehr Blast-Radius liefern: jeder
`switch`-Block, der heute vier `AudioCodec`-Werte kennt, müsste einen fünften kennen —
reine Enum-Wert-Vermehrung für etwas, das sich sauber als Formatvariante des
*bestehenden* PCM-Codecs modellieren lässt (genau wie Bit-Tiefe schon heute eine
Formatvariante ist, kein eigener Codec).

**Entscheidung: (B).** Ein Float-Flag orthogonal zu `audio_bit_depth`, sichtbar nur bei
PCM, MKV-only (folgt automatisch der bestehenden PCM-Container-Policy — MP4+PCM ist
unabhängig vom Float-Flag `Experimental`, siehe Thema 3). **Empfehlung: 1.0-würdig**,
weil klein, risikoarm, gut testbar (reiner Byte-Kopierpfad + ein neuer CodecID-String)
und ein reales Archiv-/Post-Produktions-Use-Case (Float32-WAV/-PCM ist in
DAW-/NLE-Workflows der verbreitete verlustfreie Zwischenformat-Standard, teils
bevorzugt gegenüber Int32, weil kein Clipping-Headroom-Management nötig ist).

---

### Thema 3 — PCM/FLAC in MP4 (das `ipcm`-Problem)

**Alternativen:**

**(A) Expliziter `codec_tag`-Override im eigenen Remux-Code**, exakt nach dem Muster,
das für `hvc1` bereits produktiv ist (`mp4_remuxer.cpp:220-231`): vor
`avformat_write_header()` `out_st->codecpar->codec_tag` für PCM-Audio-Streams explizit
auf die QuickTime-FourCCs setzen — `sowt` (16-bit signed little-endian PCM),
`in24`/`in32` (24-/32-bit signed little-endian PCM, laut QuickTime-Spec), `fl32`
(32-bit float PCM, falls Thema 2 vorher landet). Kein FFmpeg-Patch, keine neue
Build-Repo-Version nötig — reine Anwendungscode-Änderung, exakt im bereits etablierten
Muster.

**(B) Patch im eigenen FFmpeg-Build-Repo** (`Exoridus/exosnap-ffmpeg-build`) — den
Mov-Muxer in libavformat selbst so patchen, dass er für "mp4" (nicht nur "mov")
standardmäßig die QuickTime-Sample-Entries statt `ipcm` wählt, oder einen expliziten
Build-Flag dafür ergänzt. Erfordert einen neuen Release-Tag (`r4`), Downstream-Pin-Update
in `cmake/VendorFFmpeg.cmake`, und Pflege eines Fork-Patches über künftige
FFmpeg-Versionssprünge hinweg.

**(C) Ehrlich MKV-only lassen** — Status quo, nur die Dokumentation schärfen.

**Abwägung:**

(A) ist der mit Abstand günstigere Weg **und** die Codebasis hat für genau dieses Muster
bereits einen funktionierenden, live-verifizierten Präzedenzfall (`hvc1`): ein einzelner
expliziter `codec_tag`, vom Anwendungscode gesetzt, bevor libavformat den Header
schreibt, wird von FFmpegs mov/mp4-Muxer respektiert, wenn der FourCC in der
Codec-Tag-Kompatibilitätstabelle des Muxers für den jeweiligen `codec_id` gelistet ist.
Ob `sowt`/`in24`/`in32` für `AV_CODEC_ID_PCM_S16LE`/`S24LE`/`S32LE` in der
`avformat-62`-Build tatsächlich akzeptiert werden (statt vom Muxer verworfen/auf einen
Fallback zurückgesetzt zu werden), ist **nicht aus dem Quellcode dieses Repos
verifizierbar** — `avformat-62.dll` ist ein Prebuilt ohne vendorierte
libavformat-Quellen in diesem Checkout (bestätigt: `cmake/VendorFFmpeg.cmake` lädt ein
Release-Zip von GitHub, kein `third_party/ffmpeg`-Quellbaum liegt im Repo).

**Desk-Check-Ergebnis (Review-Einwand, major — nachgeholt statt nur gefordert):** Der
Review-Einwand hatte recht, dass ein Desk-Check gegen die öffentlichen n8.1.1-Quellen
möglich und sinnvoll ist, bevor Schritt 4 gebaut wird — dieser Desk-Check wurde für
diese Spec-Revision nachgeholt (gegen `FFmpeg/FFmpeg` Tag `n8.1.1` auf GitHub, nicht
gegen dieses Repo, das FFmpeg nur als Prebuilt konsumiert). Ergebnis: `sowt`, `in24`,
`in32` und `fl32` (nicht `in24`s Big-Endian-Alias `42ni`/`23ni`) stehen in
`libavformat/isom_tags.c::ff_codec_movaudio_tags`, und `movenc.c::mov_get_codec_tag()`
zieht diese Tabelle **unconditional** für `AVMEDIA_TYPE_AUDIO`-Streams heran — es gibt
in n8.1.1 **keinen** `MOV_MODE_MP4`-vs-`MOV_MODE_MOV`-Gate, der PCM-FourCCs für den
`mp4`-Muxer anders behandelt als für `mov`. Das entkräftet das befürchtete
DOA-Szenario ("Alternative (A) könnte für den `mp4`-Muxer strukturell ausgeschlossen
sein") erheblich, ersetzt aber **nicht** den Live-Verify — der Desk-Check zeigt nur,
dass der Muxer den Tag *akzeptieren kann*, nicht dass die geschriebene Datei von echten
Playern korrekt gelesen wird oder dass keine anderen, hier nicht geprüften
Versions-/Profil-Bedingungen greifen. Zusätzlich bestätigt: `fpcm` (ISO/IEC 23003-5,
das vom Review als Alternative zu `fl32` genannte Sample-Entry-Format) kommt in
`isom_tags.c` **nirgends** vor — dieser FFmpeg-Build kennt nur die QuickTime-Variante
`fl32` für Float-PCM. Der `fl32`-Fall im Skizzen-Code (Schritt 4) ist damit die
einzige im Build verfügbare Option, nicht eine von zwei zu klärenden Alternativen —
das schließt den vom Review aufgeworfenen `fl32`-vs-`fpcm`-Diskussionspunkt für dieses
Repo.

Der zentrale technische Unsicherheitsfaktor bleibt trotzdem bestehen, nur schwächer:
Erfolg oder Misserfolg von Alternative (A) lässt sich abschließend nur durch
tatsächliches Remuxen + Kontrolle des geschriebenen `codec_tag` feststellen, nicht
durch Code-Lektüre allein. Genau dieses Verify-Muster ("verified on real files")
fordert `docs/roadmap.md:126-127` bereits für die `hvc1`-Entscheidung — dieselbe
Beweislage wird hier verlangt; wie dieser Nachweis technisch geführt wird (nicht per
`ffprobe`, s. Korrektur in Schritt 4), ist unten präzisiert.

Sollte (A) am Muxer-internen Tag-Whitelisting scheitern (der Muxer verwirft den
gesetzten Tag und fällt auf `ipcm` zurück, oder bricht mit einem Fehler ab), ist (B) die
einzige verbleibende Option, bei erheblich höherem Wartungsaufwand (Fork-Patch über
FFmpeg-Versionssprünge). Da das Projekt bereits ein eigenes FFmpeg-Build-Repo betreibt
(präzedenzlos ist das nicht — es existiert für genau diesen Zweck, siehe
`project_ffmpeg_build_repo`-Memory), ist (B) kein Fremdkörper, aber deutlich teurer als
nötig, falls (A) reicht.

**Entscheidung: (A) zuerst versuchen, mit (B) als dokumentierter Fallback, falls der
Live-Verify von (A) scheitert.** **Empfehlung: 1.0-würdig, mit hartem Verify-Gate vor
Freigabe** — der Implementierungsaufwand für (A) ist klein (identisches Muster wie
`hvc1`, keine neue Abhängigkeit), der Nutzen hoch (PCM/verlustfreies Audio in MP4 ist ein
oft nachgefragtes Editing-/Archiv-Feature, und MP4 ist für viele NLE-Workflows der
bequemere Container als MKV). **FLAC-in-MP4 bleibt bewusst außen vor** — anders als PCM
hat FLAC-in-MP4 kein etabliertes, breit unterstütztes Sample-Entry-Muster wie
`sowt`/`in24` für PCM; ISO/IEC 14496-3 kennt `fLaC`, aber die reale Player-/NLE-Abdeckung
dafür ist laut `docs/roadmap.md:123` ("FLAC-in-MP4 is not a 1.0 target") schlechter
belegt als für PCM. Diese Spec ändert daran nichts — FLAC bleibt MKV-only, auch wenn
PCM-in-MP4 landet.

---

## Implementierungsschritte

Reihenfolge nach Priorität: **Thema 2 (Float-PCM) → Thema 3 (PCM-in-MP4) → Thema 1
(5.1/7.1, Backlog-Skizze, nicht 1.0)**.

### Schritt 1 — Float-PCM: Modell + Encoder (`recorder_core`, `capability`)

- `libs/recorder_core/src/pcm_audio_encoder.h/.cpp`: neues Feld `bool m_float = false`
  (oder `enum class PcmSampleFormat`), Setter `SetFloatFormat(bool)`. In `FeedFloat32`:
  wenn Float aktiv, `pkt.bytes` direkt aus den eingehenden Float32-Samples befüllen
  (`memcpy`/`std::copy`, keine `Float32ToSxx`-Konvertierung) — `m_bit_depth` wird bei
  Float intern ignoriert bzw. auf 32 erzwungen (Validierung in `SetFloatFormat`, analog
  zu `SetBitDepth`s bestehendem Silent-Ignore-Muster für ungültige Werte).
- `libs/recorder_core/src/matroska_stream_writer.cpp:362-365`: neuer Zweig — wenn
  `StreamAudioCodec::Pcm` **und** das neue `MatroskaStreamConfig`-Feld (Float, s.u.)
  gesetzt ist, `KaxCodecID` auf `"A_PCM/FLOAT_IEEE"` statt `"A_PCM/INT_LIT"`;
  `KaxAudioBitDepth` bleibt (32).
- `libs/recorder_core/include/recorder_core/recorder_session.h`: `RecorderConfig` bekommt
  `bool audio_pcm_float = false` neben dem bestehenden `audio_bit_depth`
  (`recorder_session.h:339`).
- **Mux-Plumbing (Review-Einwand, major — fehlte komplett):** `MatroskaStreamWriter`
  branch nicht auf `RecorderConfig` selbst, sondern auf seiner eigenen
  `MatroskaStreamConfig` (`libs/recorder_core/src/matroska_stream_writer.h:116-140`).
  Das neue Float-Feld muss dort als eigenes Feld ergänzt werden (analog zu
  `audio_bit_depth` in Zeile 140) und in
  `libs/recorder_core/src/mux_thread.cpp:190` von `m_state.config.audio_pcm_float` nach
  `sw_config_template.audio_pcm_float` kopiert werden (direkt neben der bestehenden
  `sw_config_template.audio_bit_depth = m_state.config.audio_bit_depth;`-Zeile). Ohne
  diesen Schritt sieht `MatroskaStreamWriter::Open()` das Flag nie, unabhängig davon, ob
  `RecorderConfig` es korrekt trägt.
- `libs/recorder_core/src/audio_thread.cpp:51-59`: `MakeEncoderSetup()` ruft
  `enc->SetFloatFormat(config.audio_pcm_float)` zusätzlich zu `SetBitDepth`.
- **Engine-Validierung (Review-Einwand, major):** `libs/recorder_core/src/
  recorder_session.cpp:269-281` (der bestehende codec-gated `audio_bit_depth`-Block,
  s. korrigierter Ist-Zustand C) bekommt einen zusätzlichen Check: `audio_pcm_float &&
  audio_codec != AudioCodec::Pcm` → `E_INVALIDARG`-Reject ("audio_pcm_float requires
  AudioCodec::Pcm"); `audio_pcm_float && audio_bit_depth != 32` → `E_INVALIDARG`-Reject
  ("audio_pcm_float requires audio_bit_depth == 32"). Das schließt die Engine-seitige
  Lücke, die sonst als einzige der drei Konsistenz-Schichten (UI, Preset-Load,
  Engine-Prepare) das neue Feld nicht kennen würde.
- Test: `libs/recorder_core/tests/` — neue Fälle analog zu den bestehenden
  Bit-Tiefen-Tests des PCM-Encoders (Float32-Bytes bit-identisch zum Input,
  `total_float_samples * 4` Byte-Länge, PTS-Fortschreibung unverändert); neuer
  `recorder_session`-Validate-Test für die beiden neuen Reject-Fälle
  (`audio_pcm_float=true` mit `AacMf`/`Opus`/`Flac`; `audio_pcm_float=true` mit
  `audio_bit_depth=16`).

### Schritt 2 — Float-PCM: Resolver + UI

- `libs/capability/include/capability/audio_ui_state.h`: `AudioUiState` und
  `AudioPlanResult` bekommen `bool audio_pcm_float = false` neben `audio_bit_depth`.
  `BuildAudioPlan()` reicht es durch (analog zu den bestehenden Pass-through-Zeilen
  58-62 in `audio_ui_state.cpp`).
- **Korrektur (Review-Einwand, major — falsche Stelle):** Die ursprüngliche Referenz auf
  `libs/capability/src/translation.cpp` für das Durchreichen von
  `AudioPlanResult → RecorderConfig` war falsch. `translation.cpp` sieht
  `AudioPlanResult` nicht (Signatur: `ToRecorderCoreConfig(const UserRecorderConfig&,
  const CapabilitySet&, ...)`) und kennt heute **kein einziges** Audio-Format-Feld
  (weder `audio_sample_rate` noch `audio_channels`/`audio_bit_depth`). Die vier Felder
  fließen stattdessen in `app/services/RecordingCoordinator.cpp:849-853` direkt vom
  `AudioPlanResult` in die `RecorderConfig`
  (`config.audio_bit_depth = plan.audio_bit_depth;` etc.). Richtige Änderung: in
  `RecordingCoordinator.cpp` neben Zeile 852 `config.audio_pcm_float =
  plan.audio_pcm_float;` ergänzen. `translation.cpp` bleibt für dieses Feld unberührt —
  es kennt Audio-Format-Felder grundsätzlich nicht, nur den `AudioCodec`-Enum-Wert
  selbst (relevant für Thema 3, siehe Schritt 5).
- `app/pages/ConfigPage.cpp`: Bit-Tiefen-Combobox (Zeile ~3993-4008) bekommt einen
  vierten Eintrag `"32-bit float"` — sichtbar nur wenn Codec == PCM (nicht FLAC, libFLAC
  kennt kein Float). Auswahl setzt `audio_ui_state_.audio_bit_depth = 32` **und**
  `audio_pcm_float = true`; die drei bestehenden Int-Einträge setzen `audio_pcm_float =
  false`.
  **Korrektur (Review-Einwand, minor — Combobox-Kodierungskollision):** Die Combobox
  kodiert Einträge heute als reines `int`-`ItemData` (`addItem(..., 16/24/32)`,
  `ConfigPage.cpp:3996-3998`); Restore läuft über `findData(audio_bit_depth)`
  (Zeile 3454, 4000, 5259), Read-back über `itemData(idx).toInt()` (Zeile 4264). Ein
  vierter Eintrag `"32-bit float"` mit `ItemData = 32` würde beim Restore mit dem
  bestehenden `"32-bit"`-Int-Eintrag kollidieren (`findData(32)` matcht den ersten
  Treffer, nicht notwendig den gewünschten) und beim Read-back nicht von Int32
  unterscheidbar sein. **Fix:** eindeutiges Encoding — der Float-Eintrag bekommt
  `ItemData = -32` (negativer Sentinel, kollisionsfrei mit den positiven 16/24/32-Werten).
  Read-back: `const int raw = audio_bit_depth_combo_->itemData(idx).toInt(); if (raw <
  0) { audio_ui_state_.audio_bit_depth = 32; audio_ui_state_.audio_pcm_float = true; }
  else { audio_ui_state_.audio_bit_depth = static_cast<uint32_t>(raw);
  audio_ui_state_.audio_pcm_float = false; }`. Restore: `findData(audio_ui_state_
  .audio_pcm_float ? -32 : static_cast<int>(audio_ui_state_.audio_bit_depth))`.
- `app/models/SettingsHintText.h:66` (`kAudioBitDepth`): Hinweistext um den
  Float-Hinweis ergänzen ("32-bit float is the mix bus's native format — no
  conversion, no clipping headroom needed").
- `app/ui/CodecLabels.h`: kein neues Label nötig — PCM bleibt `"PCM"`; die
  Float-Eigenschaft ist eine Format-Detail-Anzeige (Bit-Tiefen-Feld), kein Codec-Name,
  konsistent mit dem bestehenden Muster für 16/24/32-bit Int.
- Preset-Schema: `kPresetSchemaVersion` (`app/models/RecordingPreset.h:45`) hochzählen;
  `[audio]`-TOML-Sektion bekommt `pcm_float` (bool, default `false`). Pre-1.0: kein
  Migrationscode, ältere Presets werden beim Laden zurückgesetzt (Standardverhalten).
  **Ergänzung (Review-Einwand, minor):** `app/settings/RecordingPresetStore.cpp:956-957`
  clampt heute nur `audio_bit_depth` selbst (auf `{16,24,32}`, Default 16), kennt aber
  keine Cross-Feld-Konsistenz. Der Load-Pfad braucht direkt daneben eine
  Konsistenz-Reparatur analog zum bestehenden Muster: wenn
  `pcm_float == true && (audio_codec != AudioCodec::Pcm || audio_bit_depth != 32)`, wird
  `pcm_float` beim Laden still auf `false` zurückgesetzt (kein Crash, kein Blocker —
  gleiches "narrow back to a safe default" wie beim bestehenden Bit-Tiefen-Clamp).

### Schritt 3 — Float-PCM: Doku

- `docs/decisions/0030-channel-sample-format-model.md`: Abschnitt "Deferred" — den
  Float-PCM-Punkt streichen, stattdessen im Hauptteil dokumentieren (neue Zeile bei
  "Bit depth" in der Vetted-Value-Tabelle: `16, 24, 32-bit int, 32-bit float`).
- `docs/product-spec.md` Abschnitt 5 (Audio-Modell, Zeile 212-226): "Deferred: ... float
  PCM ..." → entfernen; Bit-Tiefen-Satz um "or 32-bit float" ergänzen.
- `KNOWN_LIMITATIONS.md`: den "float PCM"-Punkt aus der Deferred-Liste (Zeile 337)
  streichen.

### Schritt 4 — PCM-in-MP4: `codec_tag`-Override + Live-Verify (Alternative A)

- `libs/recorder_core/src/mp4_remuxer.cpp`: im Stream-Mapping-Loop
  (`RemuxStreamCopy`, ab Zeile 201), analog zum bestehenden HEVC-`hvc1`-Block
  (Zeile 220-231), einen neuen Block für Audio ergänzen:
  ```cpp
  if (out_is_mp4 && out_st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      switch (out_st->codecpar->codec_id) {
      case AV_CODEC_ID_PCM_S16LE:
          out_st->codecpar->codec_tag = MKTAG('s','o','w','t'); break;
      case AV_CODEC_ID_PCM_S24LE:
          out_st->codecpar->codec_tag = MKTAG('i','n','2','4'); break;
      case AV_CODEC_ID_PCM_S32LE:
          out_st->codecpar->codec_tag = MKTAG('i','n','3','2'); break;
      case AV_CODEC_ID_PCM_F32LE: // nur falls Schritt 1-3 vorher gelandet sind
          out_st->codecpar->codec_tag = MKTAG('f','l','3','2'); break;
      default: break;
      }
  }
  ```
  Kommentar im Code muss (wie beim `hvc1`-Block) exakt begründen, warum: `ipcm`
  (ISO/IEC 23003-5) hat schwache Player-Abdeckung, die QuickTime-FourCCs sind der
  Industriestandard für PCM-in-ISOBMFF.
- **Verify vor jeder Weiterarbeit (blockierend, s. Test-Plan unten):** **Korrektur
  (Review-Einwand, major):** Die ursprüngliche Formulierung verlangte `ffprobe` als
  Verify-Werkzeug und behauptete ein bestehendes `ffprobe`-Präzedenzmuster für die
  `hvc1`-Verifikation — beides ist falsch. `ffprobe` ist im Projekt nicht vorhanden:
  `cmake/VendorFFmpeg.cmake:12` shippt ausschließlich die vier Mux-DLLs
  (avformat/avcodec/avutil/swresample), kein `ffprobe`-Binary, und `ffprobe` kommt in
  der Testbasis nur in Kommentaren vor. Das tatsächliche `hvc1`-Verify-Muster in
  `libs/recorder_core/tests/test_mp4_remuxer.cpp` (z. B. Zeile 767-779) öffnet die
  erzeugte MP4 direkt per `avformat_open_input()` und prüft
  `video_st->codecpar->codec_tag` gegen `MKTAG('h','v','c','1')` — ganz ohne externe
  Prozess-Abhängigkeit, rein in-process im Testbinary, das ohnehin gegen `FFmpeg::mux`
  linkt. **Richtiges Vorgehen:** ein eigenständiges, CI-fähiges Remux-Test-File erzeugen
  (synthetische MKV mit `A_PCM/INT_LIT`-Track → `RemuxToProgressiveMp4`), die
  Ausgabe-MP4 im Test per `avformat_open_input()` + `avformat_find_stream_info()`
  öffnen und `audio_st->codecpar->codec_tag` gegen `MKTAG('s','o','w','t')` (bzw.
  `in24`/`in32` je nach Bit-Tiefe des Testsignals) prüfen — exakt dasselbe Muster wie
  der bestehende `hvc1`-Test, nur für den Audio-Stream statt Video. Erst wenn der Tag
  tatsächlich `sowt`/`in24`/`in32` zeigt (nicht `ipcm`, nicht ein Muxer-Fehler), geht
  Schritt 4 weiter. Schlägt der Tag-Override fehl (Muxer verwirft ihn oder bricht ab),
  stoppt diese Spec an dieser Stelle — Alternative (B) (Build-Repo-Patch) wird dann als
  separate Folge-Spec neu bewertet, nicht blind nachgezogen.
- Nach erfolgreichem `ffprobe`-Verify: dieselbe Behandlung für `AV_CODEC_ID_PCM_S16LE`/
  `S24LE`/`S32LE` im **RemuxToMkv**-Pfad **nicht** nötig (Matroska ignoriert
  `codec_tag`, mapped über `CodecID`-Strings, s. Ist-Zustand D) — nur der MP4-Zweig ist
  betroffen.

### Schritt 5 — PCM-in-MP4: Resolver-Freigabe

- `libs/capability/src/container_compat_registry.cpp`: **Korrektur (Review-Einwand,
  major — falsche Begründung):** Die ursprüngliche Formulierung wollte MP4+H264+PCM auf
  `Allowed` heben, aber MP4+HEVC+PCM "weiterhin Experimental für HEVC/AV1-in-MP4 (die
  selbst noch nicht über `Allowed` hinaus sind)" lassen. Das ist falsch — MP4+HEVC+AAC
  ist bereits `Allowed` (Zeile 180-184, "Implemented in 0.7.0"), nur AV1-in-MP4 ist
  durchgängig `Experimental` (Zeile 192-199). Da der PCM-`codec_tag`-Override in Schritt 4
  im Audio-Zweig des Remux-Loops sitzt und unabhängig vom Video-Codec-Zweig ist (der
  `hvc1`-Override ist ein separater, bereits produktiver Block), gilt der identische
  Remux-Pfad für H.264+PCM und HEVC+PCM in MP4 gleichermaßen — es gibt keinen sachlichen
  Grund, HEVC+PCM nach erfolgreichem Verify auf `Experimental` zu belassen, während
  HEVC+AAC `Allowed` ist. **Korrigierte Regel:** nach erfolgreichem Verify (Schritt 4)
  die drei `MP4 | * | Pcm`-Einträge differenziert anheben — `MP4+H264+PCM` (Zeile
  168-174) und `MP4+HEVC+PCM` (Zeile 185-187) auf `Allowed`; `MP4+AV1+PCM`
  (Zeile 196-197) bleibt `Experimental`, weil AV1-in-MP4 selbst (unabhängig vom
  Audio-Codec) noch nicht validiert ist — der Grund ist die Video-Container-Kombination,
  nicht PCM. Kommentarblock am Dateikopf (Zeilen 26-100) entsprechend aktualisieren —
  die `ipcm`-Erklärung durch die neue `sowt`/`in24`/`in32`-Lösung ersetzen.
- `ReconcileCodecs()` braucht **keine Code-Änderung** — sie behandelt `Allowed`
  automatisch als funktionierende Kombination (Zeile 270-271).
- **Zweites Gate (Review-Einwand, Blocker — s. korrigierter Ist-Zustand E):**
  `libs/capability/src/translation.cpp` muss **zwingend gleichzeitig** erweitert
  werden, sonst wirft `ToRecorderCoreConfig()` beim Aufnahmestart trotz
  UI-seitig "Allowed"-Anzeige eine `std::invalid_argument`-Exception — die Registry und
  `translation.cpp` sind zwei unabhängige Whitelists, die Registry-Freigabe hebt
  `translation.cpp`s Gate nicht automatisch mit an. Konkret: neue Bool-Flags
  `is_mp4_h264_pcm` und `is_mp4_hevc_pcm` (analog zu `is_mp4_h264_aac`/
  `is_mp4_hevc_aac`, Zeile 75-80) ergänzen, in die `if`-Disjunktion (Zeile 82-84) und in
  die `if/else if`-Kette (Zeile 118-175) mit `core_config.audio_codec =
  recorder_core::AudioCodec::Pcm;` aufnehmen, und die Fehlermeldung (Zeile 88-91) um
  "MP4+(H264|HEVC)+PCM" ergänzen.
- `app/pages/ConfigPage.cpp` / Audio-Codec-Auswahl für MP4: prüfen, wo die
  MP4-Container-Auswahl PCM aktuell aus der Codec-Liste ausblendet (folgt demselben
  `ContainerCompatRegistry::Query`-Aufruf wie die übrigen Kombinationen — kein
  separater Hardcode erwartet, aber verifizieren).
- Test: `libs/capability/tests/test_container_compat_registry.cpp` — bestehende
  `MP4 + H264/HEVC + PCM = Experimental`-Assertions auf `Allowed` ändern (AV1+PCM bleibt
  `Experimental`); neuer Test für `ReconcileCodecs` bestätigt, dass ein MP4+H264+PCM-
  und ein MP4+HEVC+PCM-Preset jetzt **nicht** mehr auf AAC reconciled werden.
- **Neuer Test (Review-Einwand, Blocker):** `libs/capability/tests/test_translation.cpp`
  (bzw. wo `ToRecorderCoreConfig()` heute getestet wird) — `ToRecorderCoreConfig()` mit
  MP4+H264+PCM und MP4+HEVC+PCM wirft **nicht** mehr (liefert
  `core_config.audio_codec == AudioCodec::Pcm`, `core_config.container ==
  Container::Mp4`). Dieser Test ist der einzige, der die reale Aufnahmestart-Regression
  aus dem korrigierten Ist-Zustand E fängt — die Registry- und Reconcile-Tests allein tun
  das nicht, weil sie unterhalb bzw. neben `translation.cpp` liegen.

### Schritt 6 — PCM-in-MP4: Doku

- `docs/decisions/0030-channel-sample-format-model.md` "Deferred"-Abschnitt: den
  MP4-PCM-Punkt durch eine Verweis-Zeile auf eine neue ADR 0044 ersetzen (siehe unten).
- Neue **ADR 0044** (`docs/decisions/0044-pcm-in-mp4-sample-entry.md`): dokumentiert die
  `sowt`/`in24`/`in32`-Entscheidung, den `ffprobe`-Verify-Nachweis (Player-Matrix, siehe
  Test-Plan), und explizit: FLAC-in-MP4 bleibt außen vor (kein etabliertes
  Sample-Entry-Muster).
- `docs/roadmap.md` "Final container / codec / audio matrix" (Zeile 108-123): Zeile
  "MP4 | AV1, HEVC, AVC | AAC, PCM; Opus only ..." — den erläuternden Punkt "PCM in MP4
  exists ... must be specified as a concrete sample-entry/player matrix" durch einen
  Verweis auf ADR 0044 ersetzen (als erledigt markieren, nicht löschen).
- `docs/product-spec.md` Abschnitt 4 (Container/Codec-Matrix, Zeile 137-170): den
  PCM-in-MP4-deferred-Absatz durch die neue Regel ersetzen: MP4 bietet PCM (nicht FLAC)
  mit den QuickTime-Sample-Entries.
- `KNOWN_LIMITATIONS.md`: den PCM-in-MP4-Abschnitt auf den neuen Stand bringen — FLAC-
  in-MP4 bleibt in der Deferred-Liste, PCM verlässt sie. **Korrektur (Review-Einwand,
  minor — falsche Zeilenreferenz):** die korrekten Stellen sind
  **`KNOWN_LIMITATIONS.md:66-71`** (Container/Codec-Regeln-Absatz) und
  **`KNOWN_LIMITATIONS.md:337`** (Deferred-Liste — nicht 336). `KNOWN_LIMITATIONS.md:
  250-254` behandelt **Hot-Swap-Limitationen**, nicht PCM-in-MP4, und ist hier fehl am
  Platz. Der gemeinte "MP4 PCM deferred (Experimental)"-Konsolidierungs-Absatz mit dem
  `ipcm`-Live-Verify-Nachweis steht stattdessen in **`docs/roadmap.md:250-254`** und muss
  dort mit-aktualisiert werden (bereits als eigener Punkt oben in Schritt 6 erfasst,
  Zeile 108-123 → jetzt korrekt referenziert 250-254 für den Konsolidierungs-Absatz).

### Schritt 7 (Backlog, nicht 1.0) — 5.1/7.1: Grobskizze für eine spätere Spec

Nur als Startpunkt für eine künftige, eigene Spec — **nicht Teil dieser Umsetzung**:

1. WASAPI-Capture (`wasapi_loopback.cpp`, `wasapi_process_loopback_src.cpp`):
   `IAudioClient::GetMixFormat()` statt hartcodiertem `WAVEFORMATEX` abfragen, bei
   `nChannels > 2` einen `WAVEFORMATEXTENSIBLE`-Pfad mit `dwChannelMask` nehmen; MIC
   bleibt bei Alternative (B) unverändert.
2. `MixedAudioSrc`: `kOutputChannels` durch eine Laufzeit-Kanalzahl ersetzen (FIFO-/
   Puffer-Arithmetik generisch machen); Downmix-Pfad für gemischte Kanalzahlen
   (5.1-SYS + Stereo-MIC im selben Track) explizit entscheiden — vermutlich: Merge nur
   erlaubt, wenn alle beteiligten Quellen dieselbe Zielkanalzahl haben, sonst
   UI-seitig blockiert (keine stille Kanalvertauschung).
3. Je Encoder eine explizite **Kanalreihenfolgen-Remapping-Tabelle** von der
   WASAPI-Quellreihenfolge auf die jeweilige Zielreihenfolge:
   - Opus: Mapping-Family-1-Header (Streams/Coupled-Count/Mapping-Tabelle) in
     `codec_private.cpp::BuildOpusCodecPrivate` + `opus_multistream_encoder_create()`
     statt `opus_encoder_create()` in `opus_audio_encoder.cpp`.
   - FDK-AAC: `AACENC_CHANNELMODE` pro Kanalzahl (5.1/7.1-Modi) in
     `fdk_aac_encoder.cpp:69`.
   - PCM/FLAC: bereits kanalzahl-transparent (Ist-Zustand D) — nur die
     Eingangsreihenfolge muss stimmen.
4. UI: Channels-Combobox (`ConfigPage.cpp:3961-3965`) um 5.1/7.1 erweitern, gated
   per gewähltem Codec (Opus/AAC nur nach Schritt 3, FLAC max 8, PCM immer).
5. Test-Strategie ohne echte 5.1/7.1-Hardware: synthetische Multi-Kanal-Testsignale mit
   pro Kanal unterschiedlicher, eindeutig identifizierbarer Frequenz/Amplitude durch die
   Pipeline schicken und per `ffprobe`/manuellem Decode verifizieren, dass jeder Kanal am
   erwarteten Lautsprecher-Slot landet (kein Kanaltausch).

## Test-/Verify-Plan

**CI-fähig (automatisiert, kein Live-System nötig):**

- Float-PCM: Unit-Tests für `PcmAudioEncoder::FeedFloat32` mit `m_float = true` —
  Byte-für-Byte-Vergleich der Ausgabe gegen die rohen Float32-Eingabe-Samples
  (Little-Endian-IEEE754), PTS-Fortschreibung identisch zum Int-Pfad. Analog zu den
  bestehenden Tests in `libs/recorder_core/tests/test_matroska_stream_writer.cpp` für
  den neuen `"A_PCM/FLOAT_IEEE"`-CodecID-Zweig (Track-Header korrekt geschrieben,
  `KaxAudioBitDepth == 32`).
- Float-PCM: `libs/capability/tests/test_audio_plan_builder.cpp` — neuer Fall, der
  `audio_pcm_float = true` durch `BuildAudioPlan()` durchreicht.
- PCM-in-MP4: neuer Test in `libs/recorder_core/tests/test_mp4_remuxer.cpp` — eine
  synthetische MKV-Quelle mit PCM-Audio-Track durch `RemuxToProgressiveMp4` schicken,
  die resultierende MP4-Datei **in-process per `avformat_open_input()` +
  `avformat_find_stream_info()`** öffnen (**Korrektur, Review-Einwand, major**: kein
  `ffprobe` — das Tool existiert nicht im Projekt, s. korrigierter Schritt 4; das
  tatsächlich bereits produktive Muster ist der direkte `avformat_open_input()`-Aufruf,
  den `test_mp4_remuxer.cpp` schon für den `hvc1`-Test verwendet, z. B. Zeile 767-779)
  und `audio_st->codecpar->codec_tag` prüfen: erwartet `sowt`/`in24`/`in32`, **nicht**
  `ipcm`. **Dieser Test ist das Verify-Gate für Schritt 4** — er muss vor Schritt 5 grün
  sein, sonst ist Alternative (A) gescheitert.
- PCM-in-MP4: `libs/capability/tests/test_container_compat_registry.cpp` — aktualisierte
  Assertions (`Experimental` → `Allowed` für MP4+H264+PCM **und** MP4+HEVC+PCM;
  MP4+AV1+PCM bleibt `Experimental`), neuer Reconcile-Test.
- **PCM-in-MP4, zweites Gate (Review-Einwand, Blocker):** `translation.cpp`-Test —
  `ToRecorderCoreConfig()` mit MP4+H264+PCM und MP4+HEVC+PCM wirft nicht mehr (s.
  Schritt 5). Ohne diesen Test bleibt die Lücke zwischen Registry-Freigabe und dem
  tatsächlichen Aufnahmestart-Pfad unentdeckt — keiner der beiden oben genannten Tests
  (Remux-Verify, Registry-Assertions) deckt `translation.cpp` ab.
- Preset-Roundtrip: `app/tests/test_recording_preset_store.cpp` /
  `test_audio_encoding_preset.cpp` — neues Feld `pcm_float` rundtrip-testen (Save/Load),
  Schema-Versionssprung testen (altes Preset ohne das Feld lädt mit Default `false`,
  kein Crash).

**Nur User-live verifizierbar (nicht durch Agenten, laut CLAUDE.md "Never drive the
running application"):**

- **Tatsächliches Abspielen** einer Float-PCM-MKV-Datei in mindestens VLC + einem
  NLE (z. B. DaVinci Resolve/Premiere, sofern verfügbar) — bestätigt, dass
  `A_PCM/FLOAT_IEEE` nicht nur strukturell korrekt geschrieben ist, sondern auch
  tatsächlich mit hörbarem, unverfälschtem Audio dekodiert wird.
- **Tatsächliches Abspielen** einer PCM-in-MP4-Datei in der in ADR 0030 genannten
  Player-/Tool-Riege: Windows "Filme & TV", QuickTime (falls verfügbar), mindestens
  ein NLE. Der `ffprobe`-Tag-Check (CI-fähig) beweist nur, dass der *Container* den
  richtigen Sample-Entry-Typ trägt — er beweist nicht, dass jeder reale Player ihn
  akzeptiert. Diese Live-Player-Matrix ist die eigentliche Freigabe-Bedingung, die
  `docs/roadmap.md:121-122` fordert ("must be specified as a concrete sample-entry/
  player matrix, not a bare 'PCM'"); der Agent kann sie vorbereiten (Testdatei
  erzeugen, Anleitung schreiben), aber nicht selbst durchführen.
- 5.1/7.1 (falls je priorisiert): Test mit echter 5.1/7.1-Lautsprecher-Hardware oder
  zumindest einem Windows-Sound-Panel, das auf 5.1/7.1 konfiguriert ist, plus einer
  Anwendung, die diskret mehrkanalig rendert — beides ist auf einer Standard-Dev-Maschine
  typischerweise nicht vorhanden und müsste vom User bereitgestellt/verifiziert werden.

## Risiken

- **PCM-in-MP4, Kernrisiko:** der `codec_tag`-Override könnte von libavformats
  mov/mp4-Muxer verworfen werden, oder der Muxer könnte trotz gesetztem Tag weiterhin
  `ipcm`-spezifische Boxen schreiben (z. B. wenn die Entscheidung nicht rein tag-basiert,
  sondern zusätzlich versions-/profil-abhängig ist). Der nachgeholte Desk-Check gegen die
  öffentlichen n8.1.1-Quellen (s. Thema-3-Abwägung) zeigt, dass `sowt`/`in24`/`in32`/
  `fl32` in `ff_codec_movaudio_tags` stehen und `mov_get_codec_tag()` diese Tabelle ohne
  MP4/MOV-Modusunterscheidung heranzieht — das Risiko eines strukturellen DOA ist damit
  deutlich kleiner als ursprünglich angenommen, aber ohne Zugriff auf jeden internen
  Kontrollpfad des Muxers nicht vollständig aus dem Repo heraus auszuschließen —
  Schritt 4 hat deshalb weiterhin ein hartes, aber jetzt besser abgesichertes Verify-Gate
  (per `avformat_open_input()`, nicht `ffprobe` — s. Korrektur in Schritt 4).
- **PCM-in-MP4, Silent-Regression-Risiko:** wird der `Experimental`→`Allowed`-Sprung in
  `container_compat_registry.cpp` vor einem grünen `ffprobe`-Verify gemacht, landen
  PCM-in-MP4-Aufnahmen bei Nutzern, die von den meisten Playern nicht abgespielt werden
  können — exakt der Fehler, den ADR 0030 ursprünglich vermeiden wollte ("if a depth
  proves fragile it is narrowed back to Experimental ... rather than shipped
  silently"). Die Implementierungsreihenfolge (Schritt 4 vor Schritt 5) ist bewusst so
  gewählt, dass das nicht passieren kann.
- **Float-PCM, gering:** einziges Risiko ist ein vergessener `SetFloatFormat`-Aufruf,
  der stillschweigend auf den Int16-Default zurückfällt — durch den vorgeschlagenen
  Unit-Test (Byte-Vergleich) abgedeckt.
- **5.1/7.1, falls doch priorisiert:** das größte Einzelrisiko ist eine stille
  Kanalvertauschung (Center-Kanal landet auf Right-Slot o. ä.) — hörbar falsch, aber
  nicht crash-detektierbar, und ohne echte Mehrkanal-Hardware auch nicht einfach
  manuell zu erkennen. Jede künftige Umsetzung braucht die in Schritt 7.5 skizzierte
  synthetische Pro-Kanal-Testsignatur, sonst ist ein Kanaltausch-Bug plausibel
  monatelang unentdeckt.
- **Scope-Kopplung:** sollte Float-PCM (Thema 2) *nach* PCM-in-MP4 (Thema 3) gebaut
  werden, muss Schritt 4 (MP4-`codec_tag`-Override) nachträglich um den `fl32`-Fall
  ergänzt werden — die Implementierungsschritte sind in der oben empfohlenen
  Reihenfolge (2 vor 3) genau deshalb sortiert.

## Offene Fragen

- **Priorisierung innerhalb 1.0:** Passt Thema 2 (Float-PCM) + Thema 3 (PCM-in-MP4) in
  den 1.0-Scope, oder werden beide auf eine Post-1.0-Audio-Welle verschoben? Diese Spec
  empfiehlt 1.0-Aufnahme (klein, hoher Nutzen), trifft aber keine Termin-/Meilenstein-
  Zusage — das ist eine Produktentscheidung außerhalb des Spec-Scopes.
- **PCM-in-MP4-Fallback-Bereitschaft:** Falls der `codec_tag`-Override (Alternative A)
  am Live-Verify scheitert — soll dann sofort in Alternative (B) (FFmpeg-Build-Repo-
  Patch) investiert werden, oder bleibt MP4+PCM bis auf Weiteres MKV-only (Status quo)?
  Diese Spec empfiehlt, das erst nach einem tatsächlichen Scheitern von (A) zu
  entscheiden (neue, kleine Folge-Spec), nicht vorab zu committen.
- **5.1/7.1-Nachfrage:** Gibt es eine reale Nutzeranfrage für Mehrkanalaufnahme, die in
  den bisherigen Roadmap-/Backlog-Dokumenten nicht sichtbar ist? Falls ja, ändert das
  die Bewertung in Thema 1 grundlegend (von "kein belegter Bedarf" zu "belegter
  Bedarf, aber weiterhin hoher Aufwand") — eine reine Produktfrage, die diese Spec
  nicht beantworten kann.

## Adversarialer Review — Ergebnis

Alle neun Einwände wurden gegen den Code (Zeilen einzeln nachgelesen) bzw. gegen die
öffentlichen FFmpeg-n8.1.1-Quellen geprüft und bestätigten sich; keiner wurde
zurückgewiesen.

1. **Eingearbeitet (Blocker).** `translation.cpp:39-96` kennt kein `is_mp4_*_pcm` und
   würde bei MP4+PCM beim Aufnahmestart werfen, obwohl die Registry es erlaubt —
   bestätigt durch Code-Lektüre. Ist-Zustand E korrigiert (zweites, unabhängiges Gate
   statt "alleinige Quelle"), Schritt 5 um `translation.cpp`-Erweiterung
   (`is_mp4_h264_pcm`/`is_mp4_hevc_pcm`) + neuen Translation-Test ergänzt.
2. **Eingearbeitet (Major).** `translation.cpp` sieht `AudioPlanResult` nicht und kennt
   keine Audio-Format-Felder; die Plumbing-Stelle ist tatsächlich
   `RecordingCoordinator.cpp:849-853` — bestätigt. Schritt 2 korrigiert. `mux_thread.cpp:
   190` (`sw_config_template.audio_bit_depth = m_state.config.audio_bit_depth;`) und
   `MatroskaStreamConfig` (`matroska_stream_writer.h:116-140`) fehlten in Schritt 1 als
   Plumbing-Strecke — bestätigt und ergänzt.
3. **Eingearbeitet (Major).** `recorder_session.cpp:249-279` validiert
   `audio_channels`/`audio_sample_rate`/Opus-Lock/`audio_bit_depth` codec-gated hart in
   der Prepare-Phase — bestätigt durch Code-Lektüre; die Ist-Zustand-C-Aussage
   "Clamping ausschließlich in der UI" war zu weitgehend. Ist-Zustand C korrigiert,
   Schritt 1 um einen `recorder_session.cpp`-Validate-Punkt für `audio_pcm_float`
   ergänzt.
4. **Eingearbeitet (Major).** `ffprobe` wird im Projekt nirgends als Binary geshippt
   (`VendorFFmpeg.cmake:12`) oder in Tests aufgerufen; das tatsächliche `hvc1`-
   Verify-Muster in `test_mp4_remuxer.cpp` (Zeile 767-779 u. a.) nutzt
   `avformat_open_input()` + `codecpar->codec_tag` in-process — bestätigt. Schritt 4 und
   Test-Plan auf dieses Muster umgestellt, `ffprobe`-Referenz entfernt.
5. **Eingearbeitet (Major).** Der Einwand, dass ein Desk-Check der öffentlichen
   n8.1.1-Quellen vor Schritt 4 sinnvoll ist, wurde nicht nur übernommen, sondern für
   diese Revision nachgeholt: `ff_codec_movaudio_tags` in `isom_tags.c` enthält
   `sowt`/`in24`/`in32`/`fl32`, und `movenc.c::mov_get_codec_tag()` zieht diese Tabelle
   ohne MP4/MOV-Modusunterscheidung heran — das DOA-Risiko für Alternative (A) ist
   damit deutlich kleiner als ursprünglich dargestellt (Thema-3-Abwägung und Risiken
   entsprechend präzisiert). Zusätzlich bestätigt: `fpcm` (ISO/IEC 23003-5) kommt in
   diesem FFmpeg-Build gar nicht vor — `fl32` ist keine von zwei zu klärenden Optionen,
   sondern die einzige verfügbare.
6. **Eingearbeitet (Major).** MP4+HEVC+AAC ist bereits `Allowed`
   (`container_compat_registry.cpp:180-184`, "Implemented in 0.7.0"), nur AV1-in-MP4 ist
   `Experimental` (Zeile 192-199) — die ursprüngliche Parenthese in Schritt 5 war
   sachlich falsch. Schritt 5 korrigiert: MP4+HEVC+PCM wird nach erfolgreichem Verify
   ebenfalls auf `Allowed` gehoben (identischer Remux-Pfad wie H.264), nur MP4+AV1+PCM
   bleibt `Experimental` (Grund: AV1-in-MP4 selbst, nicht PCM).
7. **Eingearbeitet (Minor).** `wasapi_process_loopback_src.cpp:319/322` fordert
   `WAVE_FORMAT_PCM`/16-bit an (nicht Float32); SYS (`wasapi_loopback.cpp:121-125`) und
   MIC (`wasapi_capture_src.cpp:329-333`) fordern tatsächlich `WAVE_FORMAT_IEEE_FLOAT`/
   32-bit — bestätigt durch Code-Lektüre, nur APP weicht ab. Ist-Zustand A korrigiert.
8. **Eingearbeitet (Minor).** `audio_bit_depth_combo_` kodiert Einträge als reines
   `int`-`ItemData` (`ConfigPage.cpp:3996-3998`, `findData`/`itemData().toInt()`) — ein
   vierter Eintrag mit Data `32` würde mit dem bestehenden `"32-bit"`-Eintrag
   kollidieren — bestätigt. Schritt 2 um ein kollisionsfreies Encoding (negativer
   Sentinel `-32`) und eine Preset-Load-Konsistenzreparatur
   (`RecordingPresetStore.cpp:956-957`-Umfeld) ergänzt.
9. **Eingearbeitet (Minor).** `KNOWN_LIMITATIONS.md:250-254` behandelt Hot-Swap-
   Limitationen, nicht PCM-in-MP4; der gemeinte Konsolidierungs-Absatz steht in
   `docs/roadmap.md:250-254`; die Deferred-Liste liegt auf Zeile 337, nicht 336 —
   bestätigt durch Nachlesen beider Dateien. Schritt 6 korrigiert.

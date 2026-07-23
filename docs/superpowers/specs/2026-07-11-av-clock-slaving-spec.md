# A/V-Clock-Slaving (H-3 Stufe 3) — sanfte swresample-Drift-Kompensation Richtung QPC

> **SHIPPED (PR #202, 2026-07-12).** Verifiziert 2026-07-23 gegen aktuellen Code: `clock_slaving.h`,
> `audio_clock_drift.h`, `test_clock_slaving.cpp` vorhanden. Nichts hier ist mehr offen.

**Status:** Spec, umsetzungsreif (adversarialer Review eingearbeitet, Ergebnis am Ende) ·
**Stand des Ist-Zustands:** main @ #192 (2026-07-11)
**Vorarbeiten:** Stufe 1 (Discontinuity-Gap-Fill, #183) und Stufe 2 (ehrliche Drift-Metrik, #191)
sind gemerged; diese Spec baut direkt darauf auf.
**Schnittstellen:** `reliability-soak-spec` (Soak-Infrastruktur, Klappensignal-Werkzeug),
`diagnostics-support-bundle-spec` (Metrik-Export ins Log-Schema).

---

## Problem

Video und Audio laufen auf zwei physisch verschiedenen Uhren:

- **Video** wird auf der QPC-Zeitachse gepaced: CFR-Slots sind `frame_index × frame_interval`
  gegen `QueryPerformanceCounter` (Scheduler in `video_thread.cpp`, Pacing-Policy in
  `libs/recorder_core/include/recorder_core/frame_pacing.h:19-35`).
- **Audio-PTS** entsteht aus dem akkumulierten Sample-Zähler (`encoderAccumulatedFrames` in
  `libs/recorder_core/src/audio_thread.cpp:302`, fortgeschrieben je `FeedFloat32`), tickt also
  mit dem Quarz der Soundkarte.

Konsumer-Quarze weichen typisch 10–100 ppm voneinander ab. 50 ppm sind **360 ms Drift über
2 Stunden** — deutlich über der Lippen-Synchronitäts-Wahrnehmungsschwelle (~45 ms Audio-Lead).
Seit #191 wird diese Drift ehrlich **gemessen** (`audio_clock_drift.h`, WASAPI
`device_position`/`qpcPosition`-Paare), aber nichts **korrigiert**: Die Datei driftet, die
Diagnostics schauen zu. Das erklärte 0.10-Ziel „Long-recording soak, A/V-sync drift validation"
(`docs/roadmap.md:84`) fordert wörtlich nur die **Validierung** (Drift messen, Bound
bestätigen) — ob das Gate ohne Korrektur besteht, hängt vom Quarz-Los der konkreten Hardware
ab. Diese Spec baut deshalb keine „für 0.10 zwingende" Korrektur, sondern eine **bedingte
Versicherung**: Sie greift nur, wenn die gemessene Baseline die Schwelle reißt, und macht das
Bestehen des Soak-Gates damit hardware-unabhängig. Bei kleiner Baseline-Drift bleibt sie eine
No-op (Engage-Schwelle, unten).

Diese Spec definiert diese Korrektur: eine sanft dosierte, hysterese-behaftete
swresample-Ratenkompensation, die die Audio-Ausgabe-Timeline auf die QPC-Achse zieht — nur wenn
messbare Drift vorliegt, mit begrenzter Regelrate (keine hörbaren Artefakte), und mit ehrlicher
Anzeige in den Diagnostics.

---

## Ist-Zustand (main @ #192, alle Referenzen frisch erhoben)

### Messung (Stufe 2, vorhanden)

- `libs/recorder_core/src/audio_clock_drift.h:40-98` — `AudioClockDriftEstimator`: pro
  Capture-Paket `AddObservation(device_position_ns, qpc_position_ns)`;
  `drift_ms = qpc_elapsed − device_elapsed` (Zeile 59), normalisiert auf die erste Beobachtung,
  geglättet über ein Rolling-Window von 128 Paketen ≈ 1,3 s (Zeile 44). **Vorzeichen-Konvention
  (Zeile 23-25): positiv = Audio-Geräte-Uhr läuft LANGSAM gegenüber QPC → Audio-Events landen auf
  früheren PTS → Audio führt vor Video.** Pure, hardware-frei, getestet
  (`libs/recorder_core/tests/test_audio_clock_drift.cpp`, registriert in
  `libs/recorder_core/CMakeLists.txt:1030-1036`).
- `libs/recorder_core/include/recorder_core/interfaces/IAudioCaptureSource.h:31-34` —
  `AudioDeviceTiming { device_position_ns, qpc_position_ns }`; Interface-Methode
  `LastBufferDeviceTiming` (Zeile 83-86, Default `false`).
- Timing liefern alle drei gerätegebundenen Quellen: `wasapi_loopback_src.h:48-52`,
  `wasapi_process_loopback_src.cpp:454-458` (`qpcPos * 100` → ns; `qpcPos == 0` ⇒ ungültig),
  `wasapi_capture_src.h:107` ff. Decorators leiten durch: `output_format_audio_src.cpp:245-248`,
  `mic_dsp_audio_src.cpp:161-164`.
- `libs/recorder_core/src/audio_thread.cpp:306-308` — je AudioThread ein Estimator; Zeile
  372-378: pro Paket `AddObservation` + `m_state.diagnostics.OnAudioClockDrift(track_id_, DriftMs())`.

### Kompensations-Ansatzpunkt: OutputFormatAudioSrc

- `libs/recorder_core/src/output_format_audio_src.h:43-92` — Decorator (ADR 0030), der jeden
  Track auf Ziel-`{sample_rate, channels}`/Float32 bringt. **Jeder** AudioThread wrappt seine
  Quelle darin (`audio_thread.cpp:157-166`; Opus fest 48 kHz, sonst `config.audio_sample_rate`).
- Zwei Betriebsarten (`output_format_audio_src.cpp`):
  - **Passthrough** (Ziel == Inner, der Default 48 kHz/Stereo): Zeile 63-67, **kein** SwrContext,
    Float32-Bytes werden zero-copy durchgereicht (Zeile 142-145).
  - **Resample-Modus:** ein `SwrContext` via `swr_alloc_set_opts2` (Zeile 87-88), `swr_init`
    (Zeile 113), pro Acquire ein `swr_convert` (Zeile 198). Gap-Längen werden über
    `ScaleDiscontinuityGapFrames` in Ziel-Frames umgerechnet (Zeile 214-215).
- `swr_set_compensation` wird **nirgends** im Repo benutzt (verifiziert per Volltextsuche).
  Vendored FFmpeg: avutil-60 / swresample-6 (Header-Kommentar `output_format_audio_src.h:20`,
  `cmake/VendorFFmpeg.cmake`).

### Discontinuity-Gap-Fill (Stufe 1, vorhanden)

- `libs/recorder_core/src/discontinuity_gap.h:29-41` — Gap-Länge aus dem Device-Position-Sprung,
  geclampt auf 10 s (Zeile 20). `IAudioCaptureSource.h:12-23` transportiert `gap_frames` im
  `RawAudioBuffer`.
- `audio_thread.cpp:289-300` (`feedGapSilence`) + Zeile 382-384: gemessene Gaps werden als
  Stille **direkt in den Encoder** gefüttert (am OutputFormatAudioSrc vorbei), damit die
  Sample-Timeline kontinuierlich bleibt. Wichtig: Die Geräte-Position zählt durch den Gap durch,
  daher stört ein Underrun die Drift-Messung nicht (`audio_clock_drift.h:27-30`).

### Lücken im Timing-/Gap-Durchstich (relevant für Slaving-Abdeckung)

- `MixedAudioSrc` überschreibt `LastBufferDeviceTiming` **nicht** (`mixed_audio_src.h:60-70`) →
  Default `false` → gemergte Tracks melden keine Drift (dokumentiert in
  `pipeline_diagnostics_aggregator.cpp:545-552`). Das trifft aber nicht nur echte Multi-Source-
  Merges: **ein Single-Source-Track mit Gain ≠ 1.0 wird ebenfalls in MixedAudioSrc gewickelt**
  (`recorder_session.cpp:623-633`) und verliert damit heute Drift-Metrik und künftig Slaving.
- `MixedAudioSrc` liest `src_buf.gap_frames` nie (`mixed_audio_src.cpp:126-178`) und füllt Gaps
  nicht — auch im Single-Source-Fall gehen gemessene Gap-Längen dort verloren. (FIFO-Drop-Relief
  `kMaxFifoFrames` = 0,5 s, `mixed_audio_src.h:48`, greift nur bei ≥2 Quellen; bei einer Quelle
  wird die FIFO pro Acquire vollständig geleert, `mixed_audio_src.cpp:204-228`.)

### Diagnostics-Oberfläche der Metrik

- Engine: `pipeline_diagnostics_aggregator.h:190-194` (`OnAudioClockDrift`), Members Zeile
  295-299 (3 Tracks, gespiegelt an `CodecPrivateData::kMaxAudioTracks`); Sink
  `pipeline_diagnostics_aggregator.cpp:333-340`; Snapshot-Bau Zeile 545-564 (größter Betrag
  gewinnt); Reset Zeile 140-141. Snapshot-Felder:
  `pipeline_diagnostics.h:258-268` (`av_drift_ms`, `av_drift_availability`); daneben getrennt
  `duration_skew_ms` (Zeile 270-275). Der Pipeline-Health-Resolver (`pipeline_health.h`) kennt
  Drift **nicht** — Drift ist reine Metrik, kein Stage-Verdict.
- App: `RecordPage.cpp:2644-2655` (Snapshot → ViewModel + Peak-Tracking),
  `RecordPage.cpp:4088-4093` (Statuszeile `DRIFT ±X ms`), `RecordPage.cpp:4392-4396`
  (Chrome-Metrics → Overlay), `RecordPage.cpp:4773-4781` (Report-Card „Peak A/V drift: ±X ms"),
  `MainWindow.cpp:867-880` (Diagnostics-Overlay-Text). `EditExportPage` konsumiert
  `ctx.peak_av_drift_ms` (`RecordPage.cpp:1576-1577`).
- Produktspec: `docs/product-spec.md:447-452` beschreibt die Metrik explizit als „measured, not
  inferred" — muss bei Verhaltensänderung mitgezogen werden.

### Settings-Kette (Muster für den neuen Schalter)

`limiter_enabled` als Vorbild: `libs/capability/include/capability/audio_ui_state.h:56`
(AudioUiState) → `libs/capability/src/audio_ui_state.cpp:40-41` (Plan-Pass-through) →
`app/services/RecordingCoordinator.cpp:836` (→ `RecorderConfig`) →
`libs/recorder_core/include/recorder_core/recorder_session.h:405` → TOML-Persistenz
`app/settings/RecordingPresetStore.cpp:636/921` → UI `app/pages/ConfigPage.cpp:4049-4051`
(`limiterCheck` in `audio_expert_section_`).

---

## Design

### Grundsatzentscheidung: Was wird geregelt?

Ehrlich abgewogene Alternativen:

**A) PTS-Restamping (Audio-PTS periodisch an QPC anpassen, Samples unangetastet).**
Verworfen. Audio-PTS ist im gesamten Engine-Design eine reine Funktion des Sample-Zählers
(Opus/AAC/FLAC/PCM-Encoder rechnen Frames → ns); Restamping bräuchte Sprünge oder nicht-
sample-treue PTS, bricht die Matroska-Block-Kadenz (feste Opus-Frame-Dauern) und verletzt genau
die „Sample-Timeline bleibt kontinuierlich"-Invariante, die Stufe 1 hergestellt hat. Sprünge
< 1 Frame sind nicht darstellbar, Sprünge ≥ 1 Frame sind hörbar adressierbare Artefaktquellen.

**B) Grobes Sample-Drop/Insert im AudioThread (periodisch 1 Sample einfügen/entfernen).**
Verworfen. Funktional äquivalent zu einer Ratenänderung, aber als Impulsfolge: einzelne
eingefügte/entfernte Samples erzeugen Knackser bzw. erfordern eigenes Crossfading — das wäre
ein zweiter, schlechterer Resampler neben dem vorhandenen.

**C) swresample-Soft-Kompensation im vorhandenen OutputFormatAudioSrc.** **Gewählt.**
`swr_set_compensation(swr, sample_delta, compensation_distance)` verstellt die effektive
Resample-Rate um `sample_delta/compensation_distance` — eine Tonhöhenänderung im ppm-Bereich
statt Impulse. Bei ≤ 500 ppm sind das ≤ 0,87 Cent Pitch-Shift (JND ≈ 5 Cent): unhörbar. Der
Decorator sitzt bereits in **jedem** Track-Pfad, besitzt im Resample-Fall den einzigen
SwrContext (keine doppelte Resampling-Qualitätseinbuße) und ist der einzige Ort, an dem
Kompensation und Ratenkonvertierung durch **eine** Engine laufen. FFmpeg aktiviert bei
`swr_set_compensation` den Resampler intern selbst nach (setzt `SWR_FLAG_RESAMPLE` und
re-initialisiert), d. h. auch ein 48 k→48 k-Kontext kompensiert korrekt — der
Charakterisierungstest (unten) macht das zur geprüften Tatsache statt zur Annahme.

**D) Separater ClockSlavingAudioSrc-Decorator über/unter OutputFormatAudioSrc.**
Verworfen. Bei Nicht-48-kHz-Zielen (44,1 k/96 k) stünden zwei SwrContexte in Serie (doppelte
Filterung, doppelte Latenz, doppelte Buchführung); im Default-Fall bringt er nichts, was eine
Erweiterung des vorhandenen Decorators nicht auch kann. Single-Responsibility bleibt gewahrt:
OutputFormatAudioSrc heißt „bring den Track ins Ausgabeformat" — die Ausgaberate exakt auf die
QPC-Achse zu legen *ist* Teil des Ausgabeformats.

**E) Video an Audio slaven (CFR-Intervall nachführen).**
Verworfen. CFR-Video mit konstantem `frame_interval` ist Produktzusage (Default-Profil
`CFR 60 fps`); NLE-Kompatibilität hängt daran. Audio ist der elastische Freiheitsgrad.

### Regelkreis (pure, testbar: `clock_slaving.h`)

Die Regel-Logik lebt als reiner, hardwarefreier Header neben `audio_clock_drift.h` — gleiche
Bauart, gleiche Testbarkeit. Kein Qt, kein FFmpeg, kein I/O.

**Größen (alle in ms bzw. ppm, Vorzeichen konsistent zur Estimator-Konvention):**

- `D` — gemessene Drift (`AudioClockDriftEstimator::DriftMs()`): positiv = Audio führt.
- `A` — bereits applizierte Korrektur, **aus realer Frame-Buchführung des Decorators**, nicht
  aus einem Modell (unten). Positiv = Ausgabe-Timeline wurde gestreckt (Events später).
- `R = D − A` — Residual: die Fehlausrichtung, die tatsächlich in der Datei landet. Das ist die
  Zahl, die der Nutzer sieht.
- `p` — aktuelle Kompensationsrate in ppm. Konvention: **p > 0 = Ausgabe strecken** (mehr
  Output-Samples pro Input) = korrigiert positives D.

**Warum A aus Frame-Buchführung statt aus `∫p dt`:** Gap-Stille umgeht den Decorator
(`feedGapSilence` → Encoder direkt), Pausen entleeren Quellen ohne Zeitfortschritt, und der
SwrContext hat Filterverzögerung. Ein Zeitintegral über `p` würde in all diesen Fällen vom
tatsächlich Applizierten abweichen und der Regler würde gegen ein Phantom regeln. Der Decorator
zählt stattdessen exakt: `in_total` (konsumierte Input-Frames), `out_total` (produzierte
Output-Frames), und meldet
`A_ms = (out_total·inner_rate − in_total·target_rate) · 1000 / (inner_rate · target_rate)`
(int64-Arithmetik; bei 48 kHz beidseitig sind das > 60 Jahre ohne Überlauf). Die konstante
swr-Filterlatenz (< 1 ms) hebt sich nicht heraus, liegt aber weit unter jeder Schwelle.

**Regelgesetz (bewusst P-Regler, kein PI):**

```
engage:  einmal je Session, wenn |D| > kEngageThresholdMs; danach gelatcht (kein Disengage)
p_target = clamp(R_ms / kControlHorizonS × 1000, −kMaxPpm, +kMaxPpm)
p_next   = p + clamp(p_target − p, −kMaxSlewPpmPerS·Δt, +kMaxSlewPpmPerS·Δt)
anwenden nur wenn |p_next − p| ≥ kMinPpmStep (Quantisierung gegen Dauer-Nachstellen)
Update-Takt: höchstens alle kUpdatePeriodS, getaktet über qpc_position_ns der Beobachtungen
```

**Feste Parameter (Konstanten im Header, keine Settings — Begründung s. u.):**

| Konstante | Wert | Begründung |
|---|---|---|
| `kEngageThresholdMs` | 15,0 | Deutlich über Estimator-Jitter (Window glättet 1,3 s), deutlich unter der ~45-ms-Hörbarkeitsschwelle. Bei 50 ppm Drift nach ~5 min erreicht. |
| `kControlHorizonS` | 60,0 | 15 ms Residual → 250 ppm; sanfte Korrektur über eine Minute statt Ruck. |
| `kMaxPpm` | 500,0 | ≤ 0,87 Cent Pitch — unhörbar; deckt > 5× typische Quarz-Toleranz ab. |
| `kMaxSlewPpmPerS` | 125,0 | Rate erreicht den Cap frühestens nach 4 s; keine hörbare Raten-Stufe. |
| `kUpdatePeriodS` | 1,0 | Estimator-Fenster (1,3 s) ≈ Update-Takt ≪ Horizont (60 s) → stabil, keine Oszillation. |
| `kMinPpmStep` | 10,0 | Unter der swr-Auflösung sinnloser Kleinkram; verhindert Log-/Diag-Rauschen. |

**Stationäres Residual (die ehrliche Wirksamkeitskurve):** Ein P-Regler mit Horizont hat bei
echtem Ratenfehler `r` ppm ein stationäres Residual `R_ss = r·kControlHorizonS/1000` ms. Das
skaliert linear mit der Driftrate — die Kurve bis zum Cap, damit niemand mehr Wirksamkeit
hineinliest, als der Regler liefert:

| Driftrate `r` | `R_ss` (T = 60 s) | Einordnung |
|---|---|---|
| 50 ppm (typisch) | 3 ms | weit unter allem Hörbaren |
| 100 ppm (obere Konsumer-Norm) | 6 ms | dito |
| 250 ppm | 15 ms | = Engage-Schwelle; Residual pendelt an der Schwelle, wächst aber nicht mehr |
| 500 ppm (= `kMaxPpm`) | 30 ms | unter ~45-ms-Hörschwelle, aber Regler am Anschlag |
| > 500 ppm | unbegrenzt wachsend | Saturation-Warn (unten); defekte Hardware/Treiber |

Ab ~250 ppm drückt der Regler das Residual also **nicht mehr unter die Engage-Schwelle** — er
verwandelt unbegrenzt wachsende Drift in ein begrenztes, unhörbares Residual. Das ist der
Anspruch, nicht „Residual ≈ 0 bei jeder Rate"; als Limitation dokumentiert (KNOWN_LIMITATIONS,
Schritt 6). Alle Testschwellen unten sind aus `R_ss` abgeleitet, nicht frei gegriffen.

**Warum kein Disengage (Latch statt Voll-Hysterese):** Mit Disengage-Schwelle unterhalb von
`R_ss` entstünde ein ewiger Sägezahn (engage → korrigieren → disengage → neu andriften). Der
Latch macht das Verhalten monoton und erklärbar: „Ab 15 ms Drift regelt die Aufnahme für den
Rest der Session sanft nach." Ein PI-Integrator würde das stationäre Residual auf 0 bringen,
kauft das aber mit Tuning-Aufwand und Windup-Behandlung — 3–6 ms Rest im typischen ppm-Bereich
ist den zweiten Regler-Term nicht wert. **Bewusst nicht gebaut; Testschwellen müssen `R_ss`
respektieren statt implizit einen Integrator zu fordern.**

**Warum keine Settings für die Regler-Parameter:** Es gibt keine sinnvolle Nutzerentscheidung
zwischen „14 ms" und „16 ms" Schwelle; jeder Knopf wäre Pseudo-Kontrolle. Einziger Schalter:
Slaving an/aus (Expert, s. u.), primär damit Soak-Läufe eine A/B-Basislinie messen können.

### Vorzeichen-Kontrakt zu swresample

Erwartetes Mapping (aus FFmpeg `swresample/resample.c`: positives `sample_delta` verkleinert
`dst_incr` → mehr Output-Samples): `sample_delta = lround(p × 1e-6 × distance)` mit
`distance = 10 × target_rate` (10-s-Fenster ⇒ ppm-Auflösung 0,1 ppm; das Fenster läuft nie ab,
weil pro Acquire re-armiert wird). **Der Kontrakt wird nicht geglaubt, sondern getestet:** ein
Charakterisierungstest speist N Input-Frames in einen 48 k→48 k-Kontext mit +500 ppm und
verlangt messbar **mehr** Output-Frames (und umgekehrt). Fällt der Test anders aus, wird die
Mapping-Konstante im Decorator invertiert — die Engine-Konvention (`p>0 = strecken`) bleibt.

### Einbettung in OutputFormatAudioSrc

Neue, auf dem Audio-Worker-Thread aufzurufende API (gleiche Single-Thread-Zusicherung wie der
Rest des Interfaces, `output_format_audio_src.h:23-24`):

```cpp
// p in ppm; >0 streckt die Ausgabe-Timeline (mehr Output-Frames pro Input).
// Erstmaliger Aufruf mit p != 0 verlässt den Passthrough dauerhaft (lazy SwrContext).
void SetCompensationPpm(double ppm);

// Kumulierte Kompensation in ms der Ausgabe-Timeline (Frame-Buchführung, s. o.).
// 0.0 solange nie kompensiert wurde.
double AppliedCompensationMs() const;
```

- **Passthrough-Fall (der Default 48 k/Stereo):** solange `p == 0` bleibt alles wie heute
  (zero-copy, kein SwrContext — Startverhalten und CPU-Kosten unverändert; die meisten
  Sessions erreichen die 15-ms-Schwelle nie). Beim ersten `p ≠ 0` wird lazy ein
  48 k→48 k-FLT-Kontext erzeugt und der Pfad wechselt dauerhaft in den Resample-Zweig
  (`passthrough_ = false`). Der Wechsel ist sample-kontinuierlich; die einmalige
  swr-Filterlatenz (wenige Samples) taucht in der Frame-Buchführung auf und wird vom Regler
  mitkorrigiert.
- **Resample-Fall (44,1 k/96 k/Mono):** `swr_set_compensation` auf dem vorhandenen Kontext;
  keine strukturelle Änderung.
- Re-Arm des Kompensationsfensters in jedem `AcquireBuffer` vor `swr_convert` (Zeile 198),
  solange `p ≠ 0`.
- Buchführung: `in_total_ += in_frames` / `out_total_ += produced` im Resample-Zweig; im
  Passthrough zählen beide Achsen identisch (A bleibt 0). Silent-Buffer laufen bereits heute
  als Nullen durch swr (Zeile 190-194) — zählen also mit, korrekt.
- `gap_frames`-Skalierung (Zeile 214-215) bleibt bei den **nominalen** Raten: Der ppm-Fehler
  auf einen Gap ist ≤ 0,05 % der Gap-Länge (beim 10-s-Clamp-Monster einmalig ≤ 5 ms, bei realen
  ms-Gaps Nanosekunden) — akzeptiert und in Risiken dokumentiert.

### Wer regelt: AudioThread

Der AudioThread besitzt heute schon Estimator + Timing-Zugriff (Zeile 306-308, 372-378) und
erzeugt den Decorator selbst (Zeile 165). Er behält dabei einen typisierten Rohzeiger
(`OutputFormatAudioSrc*`) neben dem `unique_ptr<IAudioCaptureSource>` und ruft im Paket-Pfad,
gated auf 1 Hz über `qpc_position_ns`:

```
if (slaving_enabled && controller.Update(drift.DriftMs(), fmt->AppliedCompensationMs(), qpc_ns))
    fmt->SetCompensationPpm(controller.Ppm());
diagnostics.OnAudioClockSlaving(track_id, D, R, p);   // ersetzt OnAudioClockDrift (Schritt 4, VOR dieser Verdrahtung)
```

Mehrere Tracks = mehrere unabhängige Geräte-Uhren = unabhängige Regler je AudioThread. Das ist
korrekt so; es gibt keinen Track-übergreifenden Zustand.

**Gate:** `RecorderConfig::audio_clock_slaving_enabled = true` (neben Zeile 405,
Limiter-Muster). Engine bleibt UI-agnostisch: Die Engine kennt nur das Bool.

### Abdeckungslücke schließen: Single-Source-MixedAudioSrc

Damit der häufige Fall „MIC-Row mit Gain ≠ 1" (Wrap in `recorder_session.cpp:623-633`) Messung
und Slaving nicht verliert, leitet `MixedAudioSrc` künftig bei **genau einer** Quelle durch:

- `LastBufferDeviceTiming` → `sources_[0]` (der FIFO-Versatz ist ≤ ein Device-Period, konstant,
  und hebt sich in der Estimator-Normalisierung heraus);
- `gap_frames` → `ScaleDiscontinuityGapFrames(inner_gap, sources_[0]->SampleRate(), 48000)`
  (heute werden Gaps dort stillschweigend verworfen — das repariert zugleich den
  Gap-Fill-Durchstich von Stufe 1 für gain-gewrappte Tracks).

Multi-Source-Merges (≥ 2 Quellen) bleiben **bewusst** ohne Messung und ohne Slaving: Sie mischen
mehrere Geräte-Uhren; das FIFO-Drop-Relief (`kMaxFifoFrames`, `mixed_audio_src.h:43-48`)
begrenzt die Inter-Quellen-Drift bereits auf 0,5 s Puffer. Eine „Master-Quelle wählen und die
anderen darauf resamplen"-Lösung wäre ein eigenes Projekt (pro Quelle ein Resampler + Regler)
für einen Nischenfall — dokumentierte Limitation statt versteckter MVP-Expansion.

### Diagnostics: ehrlich, ruhig

Grundsatz: **`av_drift_ms` zeigt künftig das Residual R** — die Fehlausrichtung, die wirklich
in der Datei liegt. Alles andere wäre nach Aktivierung des Slavings eine Falschaussage (die
Roh-Drift wüchse weiter, obwohl die Datei synchron ist). Zusätzlich wird die Korrektur selbst
sichtbar, als Information, nicht als Alarm:

- **Aggregator:** `OnAudioClockDrift` wird durch
  `OnAudioClockSlaving(track_id, raw_drift_ms, residual_ms, applied_ppm)` **ersetzt**
  (pre-1.0, kein Parallel-API). Per-Track-Arrays analog Zeile 295-299.
- **Snapshot (`pipeline_diagnostics.h`, bei den bestehenden Feldern Zeile 258-268):**
  - `av_drift_ms` — Semantik neu: Residual des Tracks mit größtem |Residual|;
    Doku-Kommentar entsprechend umschreiben. `av_drift_availability` unverändert.
  - `av_drift_raw_ms` (neu) — gemessene Geräte-vs-QPC-Drift desselben Tracks.
  - `clock_slaving_ppm` (neu) — aktuelle Kompensationsrate desselben Tracks; 0.0 = nicht aktiv.
  - `clock_slaving_active` (neu, bool) — irgendein Track hat gelatcht.
- **Verhältnis zu `duration_skew_ms` (Zeile 270-275) — keine zwei widersprüchlichen Zahlen:**
  Slaving verändert die produzierte Audio-Output-Framezahl und damit die Audio-Media-Dauer.
  Das ist kein Konflikt, sondern Konvergenz: Der Uhr-Drift-Anteil, der heute in
  `duration_skew_ms` mit auflief, wird durch Slaving herausgeregelt — die Metrik nähert sich
  dem, was sie messen soll (Encoder-Starvation: Video-Timeline komprimiert, nicht Uhrenfehler).
  Semantik und Feld-Kommentar bleiben unverändert; der Kommentar zu `av_drift_ms` erhält einen
  Satz, der die Arbeitsteilung benennt (drift/residual = Uhren, duration_skew = Starvation).
  Der synthetische End-to-End-Test (Test-Plan #4) prüft explizit, dass die Output-Framezahl der
  QPC-Achse folgt — d. h. duration_skew konvergiert unter Slaving statt zu divergieren.
- **UI-Verhalten:**
  - Statuszeile (`RecordPage.cpp:4088-4093`), Overlay (`MainWindow.cpp:867-880`),
    Peak-Tracking (`RecordPage.cpp:2649-2655`): unverändert verdrahtet — sie zeigen jetzt
    automatisch das Residual. Kein alarmistischer Zusatz.
  - Report-Card (`RecordPage.cpp:4773-4781`): wenn `clock_slaving_active` während der Session
    war, wird das Drift-Label um den Fakt ergänzt, exakter Text:
    `Peak A/V drift: ±X ms · clock slaving corrected Y ms` (Y = |A| gerundet, aus dem letzten
    Snapshot; ohne Slaving unverändert). Ein Problem, eine Zeile, keine zweite Warnung.
  - **Einzige Warnbedingung** (calm-not-alarmist, nur echte gemessene Probleme): App-Log-Warn
    einmal pro Session, wenn `|p| == kMaxPpm` **und** |R| über 60 s weiter wächst — d. h. die
    Drift übersteigt die Korrekturfähigkeit (defekte Hardware/Treiber). Kein Toast, kein
    Blocker; Diagnostics-Log reicht, das Residual ist ohnehin sichtbar.
- **Logging** (`recorder_core::logging`, `logging.h:48`): Komponente `audio.clock_slaving` —
  Info bei Engage (Felder: `track`, `drift_ms`), Debug bei ppm-Änderung, Warn bei Saturation
  (einmalig). Kein 1-Hz-Spam.

### Sichtbares Verhalten / Settings

- **Expert-Schalter** „Audio clock slaving" in der Audio-Expert-Sektion der Settings
  (`audio_expert_section_`, neben `limiterCheck`, `ConfigPage.cpp:4049` ff.), Default **an**.
  ObjectName `clockSlavingCheck`. Hint-Text (SettingsHintText.h-Muster): erklärt in einem Satz,
  dass Audio bei messbarer Uhren-Drift unhörbar (≤ 0,05 %) auf die Video-Uhr nachgeregelt wird.
  Voller Persistenz-Durchstich nach Limiter-Muster (AudioUiState → Plan → Coordinator →
  RecorderConfig → TOML). Pre-1.0: fehlender TOML-Key ⇒ Default `true`, keine Migration.
- **PCM/FLAC gelten mit (Empfehlung, nicht Beschluss):** Slaving greift codec-unabhängig.
  Begründung: „Lossless" ist eine Encode-Zusage, keine Capture-Zusage; eine driftende Aufnahme
  ist das größere Übel als ein ppm-Resample. Praktisch bleibt der Passthrough ohnehin
  byte-identisch, solange die 15-ms-Schwelle nie reißt (die Mehrheit aller Sessions). Der
  Expert-Schalter ist der Bit-Exaktheits-Opt-out für Archiv-Ansprüche.
- **Getrackte Produktzusagen, die Default-ON bricht — beide werden explizit reformuliert, nicht
  nur der Metrik-Absatz:** `docs/product-spec.md:215-216` (Abschnitt 5: „The default 48 kHz /
  stereo path is a byte-identical no-op") und `KNOWN_LIMITATIONS.md:143` („capture is
  byte-identical when all [mic-DSP stages] are off") sind nach Engage falsch — der Default-Pfad
  wird dann resampelt, auch für PCM/FLAC-Tracks. Beide Stellen erhalten in Schritt 6 den
  Zusatz sinngemäß „…unless audio clock slaving has engaged (> 15 ms measured device-clock
  drift); disable *Audio clock slaving* (expert) for bit-exact capture." **Default-ON plus
  PCM/FLAC-Einbeziehung ist eine echte Produktentscheidung und braucht Produkt-Owner-Sign-off,
  bevor Schritt 6 landet** (offene Frage 1) — die Spec empfiehlt sie, nimmt sie aber nicht
  vorweg. Fällt das Sign-off anders aus (Default off oder PCM/FLAC ausgenommen), ändert sich
  nur das Config-Default bzw. ein Codec-Gate im Coordinator; Regler und Decorator bleiben
  identisch.
- `docs/product-spec.md:447-452` wird um das Slaving-Verhalten erweitert (Messung → Regelung →
  Residual-Anzeige); KNOWN_LIMITATIONS.md erhält den Multi-Source-Merge-Punkt, die
  Residual-vs-Rate-Limitation (≥ ~250 ppm, Tabelle oben) und den auf den Default-Pfad
  ausgeweiteten Stop-Tail (unten, Risiken).
- **ADR 0044** „A/V clock slaving via swresample compensation" (nächste freie Nummer nach
  0043): Entscheidung C inkl. verworfener Alternativen A/B/D/E und der Latch-Begründung.

---

## Implementierungsschritte (reihenfolgetreu; jeder Schritt PR-fähig mit Tests)

**Schritt 1 — Purer Regler (`ClockSlavingController`).**
Neu: `libs/recorder_core/src/clock_slaving.h` (Header-only, Bauart `audio_clock_drift.h`),
`libs/recorder_core/tests/test_clock_slaving.cpp`; Test-Registrierung in
`libs/recorder_core/CMakeLists.txt` nach dem Muster `test_audio_clock_drift` (Zeile 1030-1036;
Achtung: Registrierungen existieren doppelt für beide Build-Zweige, z. B. Zeile 202-209 und
781-788 beim OutputFormat-Test — beide pflegen).
API: `bool Update(double drift_ms, double applied_ms, uint64_t qpc_now_ns)` (true = neuen Wert
anwenden), `double Ppm() const`, `bool Engaged() const`, `double ResidualMs() const`;
Konstanten-Tabelle aus dem Design. Tests: Engage-Schwelle + Latch; P-Gesetz-Werte; Slew-Limit;
Cap; `kMinPpmStep`-Quantisierung; Update-Takt-Gating; Closed-Loop-Simulation (synthetische
Geräte-Uhr ±100 ppm, modellierte Plant: `out = in·(1+p·1e-6)`) → Residual konvergiert gegen
das stationäre `R_ss = r·kControlHorizonS/1000` (bei ±100 ppm: 6 ms), Assertion `|R| < 7 ms`
(R_ss + 1 ms Marge — bewusst NICHT enger: eine engere Schwelle würde implizit den verworfenen
PI-Integrator fordern) und `p` oszilliert nicht (settelt in ±20-ppm-Band um r); unter der
Schwelle bleibt `p == 0` für immer. Zusatzfall 500 ppm: Residual bleibt < 31 ms (Cap-Grenzfall
der Tabelle).

**Schritt 2 — OutputFormatAudioSrc: Kompensation + Buchführung.**
`output_format_audio_src.h/.cpp`: `SetCompensationPpm`, `AppliedCompensationMs`,
Lazy-SwrContext im Passthrough, Re-Arm pro Acquire, `in_total_`/`out_total_`-Zählung.
Tests in `tests/test_output_format_audio_src.cpp` (linkt bereits swresample):
Charakterisierung des swr-Vorzeichens (48 k→48 k, +500 ppm ⇒ über 100 × 480-Frame-Pakete
messbar mehr Output; −500 ppm ⇒ weniger); `AppliedCompensationMs`-Werte; Passthrough→Engaged-
Übergang erhält `silent`/`data_discontinuity`/`gap_frames`; Kompensation auf dem
44,1-k-Resample-Pfad; `SetCompensationPpm(0)` nach Engage (Kontext bleibt, Delta 0);
`p == 0`-Sessions bleiben byte-identischer Passthrough.

**Schritt 3 — MixedAudioSrc-Durchstich für Single-Source.**
`mixed_audio_src.h/.cpp`: `LastBufferDeviceTiming`-Override (nur `sources_.size() == 1`),
`gap_frames`-Forwarding im Single-Source-Fall (Pump merkt sich den Gap des zuletzt gezogenen
Pakets; Emission auf dem nächsten `AcquireBuffer`-Ergebnis). Tests in
`tests/test_mixed_audio_src.cpp`: Timing durchgereicht bei 1 Quelle, `false` bei 2; Gap skaliert
durchgereicht bei 1 Quelle, weiterhin 0 bei 2; Frame-Invariante unverändert.

**Schritt 4 — Diagnostics-Umbau** *(bewusst VOR der AudioThread-Verdrahtung: die neue API muss
existieren, bevor Schritt 5 sie ruft — sonst ist Schritt 5 nicht eigenständig PR-fähig).*
`pipeline_diagnostics_aggregator.h/.cpp`: `OnAudioClockDrift` → `OnAudioClockSlaving(track,
raw, residual, ppm)`; Snapshot-Bau (Zeile 545-564) auf Residual-Auswahl + neue Felder; Reset
(Zeile 140-141) erweitert. `pipeline_diagnostics.h`: Felder + umgeschriebene Kommentare
(Zeile 258-268). Die bestehende Aufrufstelle `audio_thread.cpp:372-378` wird im selben PR auf
`OnAudioClockSlaving(track_id_, D, /*residual=*/D, /*ppm=*/0.0)` umgestellt — ohne Regler ist
das Residual per Definition die Roh-Drift; Verhalten identisch zu heute, kompiliert
eigenständig. Tests: `tests/test_pipeline_diagnostics.cpp` — Feld-Durchreichung,
Größter-|Residual|-Auswahl, Reset, Availability-Regeln unverändert.

**Schritt 5 — AudioThread-Verdrahtung + Config-Gate.**
`audio_thread.cpp/.h`: typisierter Decorator-Zeiger, Controller-Instanz, 1-Hz-Update im
Timing-Block (heute Zeile 372-378), echte `(D, R, p)`-Werte an die Schritt-4-API, Logging
(Engage/Change/Saturation-Warn); `recorder_session.h`: `audio_clock_slaving_enabled = true`
(+ Doku-Kommentar). Kein neuer Test-Harness nötig: Verhalten ist durch Schritt 1-4 abgedeckt;
ein Smoke-Assert im bestehenden Engine-Testumfeld, dass das Gate `false` jede Kompensation
unterbindet, genügt (Controller wird dann gar nicht gefüttert).

**Schritt 6 — App-Oberfläche + Settings + Doku.**
Report-Card-Text (`RecordPage.cpp:4773-4781`) inkl. `clock_slaving_active`-Durchreichung durch
den Snapshot-Konsum (`RecordPage.cpp:2644-2655`); Expert-Checkbox `clockSlavingCheck` +
Hint-Text; Persistenzkette nach Limiter-Muster (audio_ui_state.h/.cpp, RecordingCoordinator.cpp
Zeile ~836, RecordingPresetStore.cpp Zeile ~636/~921); Doku: `docs/product-spec.md:447-452`
(Metrik → Regelung → Residual-Anzeige) **und** `docs/product-spec.md:215-216` (byte-identical-
Zusage reformulieren, siehe Settings-Abschnitt); KNOWN_LIMITATIONS.md: Zeile 143 (byte-identical
Mic-Capture) reformulieren, Zeile 138-140 (Stop-Tail) auf aktive Resample-Kontexte
verallgemeinern, Multi-Source-Merge nicht geslavt, Residual-vs-Rate-Limitation (≥ ~250 ppm);
ADR 0044 anlegen. **Gate: Produkt-Owner-Sign-off zu Default-ON + PCM/FLAC-Einbeziehung
(offene Frage 1) muss vor diesem Schritt vorliegen.**
Tests: `test_config_page` (Checkbox existiert, Roundtrip in AudioUiState),
`test_audio_encoding_preset` (TOML-Roundtrip + Default bei fehlendem Key).

**Schritt 7 — Validierungs-Schnittstelle (Doku, kein Produktcode).**
Abschnitt „A/V-Sync-Validierung" in der Soak-Doku (Übergabe an `reliability-soak-spec`, die die
Infrastruktur besitzt): Messmethode wie unten, Akzeptanzkriterien, A/B via Expert-Schalter.
Liefert außerdem das Auswertungs-Rezept (ffprobe-Kommandos) als `docs/`- oder
`scripts/`-Baustein, sobald die Soak-Spec den Ort festlegt.

---

## Test-/Verify-Plan

### CI-fähig (deterministisch, hardwarefrei bzw. nur FFmpeg-Link)

1. **Controller-Unit-Tests** (Schritt 1) — pur, inkl. Closed-Loop-Konvergenzsimulation über
   simulierte Stunden (kein Realzeit-Bezug: QPC ist Parameter).
2. **swr-Charakterisierung + Decorator-Tests** (Schritt 2) — linkt swresample wie der
   bestehende `test_output_format_audio_src`; klemmt den Vorzeichen-Kontrakt und die
   Frame-Buchführung fest. Bei einem FFmpeg-Vendor-Bump schlägt eine Verhaltensänderung hier
   auf, nicht im Feld.
3. **MixedAudioSrc-, Aggregator-, Preset-, ConfigPage-Tests** (Schritte 3, 4, 6).
4. **End-to-End-Rechenprobe ohne Hardware:** kombinierter Test (Estimator + Controller +
   Decorator mit Stub-Quelle, deren `AudioDeviceTiming` eine +100-ppm-Uhr simuliert): nach
   simulierten 30 min ist `|R| < 7 ms` (stationäres Residual 6 ms bei 100 ppm + 1 ms Marge,
   konsistent zur R_ss-Tabelle im Design) und die Gesamt-Output-Framezahl entspricht der
   QPC-Zeitachse ±1 Paket. Das ist der eigentliche Beweis, dass die drei Bausteine
   zusammenpassen — rein synthetisch, ctest-tauglich.

Hinweis Build: voller Test-Build + ctest, nicht `--target exosnap` (Tests hängen nicht am
App-Target).

### Nur User-live (explizit nicht CI-behauptbar)

1. **2-h-Soak mit Klappensignal (die H-3-Messmethode, Schnittstelle zur reliability-soak-spec):**
   - Signalquelle: ein Klappen-Ereignis mit hartem visuellem UND akustischem Marker bei
     Aufnahmebeginn und -ende (z. B. Fullscreen-Weißblitz + gleichzeitiger Klick-Ton aus
     derselben Quelle; die Soak-Spec liefert das Werkzeug).
   - Auswertung: `ffprobe -select_streams v -show_frames` → PTS des ersten Blitz-Frames
     (Luma-Sprung); Audio dekodieren, Klick-Peak-Sample → PTS. `Offset_start` vs `Offset_end`;
     die Differenz ist die Netto-Drift über 2 h.
   - **Akzeptanz:** Lauf A (Schalter aus): Roh-Drift dokumentieren (Baseline der Hardware).
     Lauf B (Schalter an): `|Offset_end − Offset_start| ≤ 20 ms` (Engage-Schwelle + stationäres
     Residual + Messunschärfe) **und** Diagnostics-Residual am Ende ≤ 20 ms **und** Log zeigt
     Engage nur, wenn Lauf A > 15 ms Drift hatte. Vorbehalt aus der R_ss-Tabelle: Das
     20-ms-Gate ist mit dem P-Regler nur für Baselines ≲ 330 ppm erreichbar (R_ss = 20 ms bei
     333 ppm); misst Lauf A mehr, ist das Gerät außerhalb der Design-Annahme — dokumentieren
     statt am Regler drehen.
2. **Hörprobe:** Musik-/Sprachmaterial während eines erzwungenen Regelvorgangs (Engage-Moment,
   Slew-Phase, Saturation) — keine Knackser, kein wahrnehmbarer Pitch. Nicht automatisierbar.
3. **Underrun-Interaktion live:** Während einer Aufnahme künstlich Audio-Discontinuities
   provozieren (Gerätelast) und prüfen, dass Gap-Fill + Slaving zusammen kein Springen des
   Residuals erzeugen.
4. Bestehende Live-Verify-Liste des 0.9-Gates bleibt unberührt; dieser Punkt gehört in die
   0.10-Soak-Liste.

---

## Risiken

- **Vorzeichen-Inversion** (Drift ↔ swr-Delta ↔ Residual): dreifache Konvention; ein Fehler
  verdoppelt die Drift statt sie zu korrigieren. Mitigation: Charakterisierungstest (CI) + der
  synthetische End-to-End-Test (CI) + Soak-A/B (live). Kein Pfad, auf dem das unbemerkt bliebe.
- **Verlust des byte-identischen Passthrough nach Engage:** gewollt, aber irreversibel für die
  Session; bei PCM/FLAC bedeutet das resampelte statt roher Samples — und es widerspricht dem
  Wortlaut zweier getrackter Zusagen (`product-spec.md:215-216`, `KNOWN_LIMITATIONS.md:143`),
  die deshalb in Schritt 6 reformuliert werden (siehe Settings-Abschnitt; Sign-off-pflichtig,
  offene Frage 1). Risiko begrenzt: passiert nur bei realer Drift > 15 ms, und genau dann ist
  Resampling das kleinere Übel.
- **Stop-Tail trifft nach Engage auch den Default-Pfad:** Der heute dokumentierte ~10-ms-Tail
  bei Nicht-Default-Sample-Rates (`KNOWN_LIMITATIONS.md:138-140`: swr-Puffer wird bei Stop
  nicht drainiert) betrifft nach dem Lazy-SwrContext-Engage auch 48-k-Sessions — dort in
  Filterlängen-Größenordnung (Sub-ms bis wenige ms), nicht ~10 ms. Entscheidung: **nicht**
  drainen (ein `swr_convert(NULL)`-Drain-Pfad nur für diesen Fall lohnt die Komplexität nicht),
  sondern die KNOWN_LIMITATIONS-Passage in Schritt 6 von „non-default sample rate" auf „sobald
  ein Resample-Kontext aktiv ist (Nicht-Default-Rate oder engagiertes Clock-Slaving)"
  verallgemeinern.
- **swr-Verhalten über FFmpeg-Versionen:** `swr_set_compensation`-Semantik ist stabil, aber
  vendored Bumps könnten Auflösung/Init-Verhalten ändern → der Charakterisierungstest ist als
  Regressionszaun designt, nicht nur als Erstbeweis.
- **Gap-Stille umgeht die Kompensation:** ppm-Fehler auf Gap-Länge (max. einmalig ~5 ms beim
  10-s-Clamp-Fall, sonst Sub-µs). Akzeptiert; der Regler sieht den Effekt nicht (weder in D
  noch in A) — bewusst nicht „mitmodelliert", weil der Fall eine ohnehin degradierte Aufnahme
  ist.
- **Estimator-Qualität einzelner Treiber:** `qpcPosition == 0` oder springende Positionswerte
  einzelner WASAPI-Treiber machen D unbrauchbar → dank Engage-Schwelle + Window-Glättung regelt
  der Controller dann schlicht nie; schlimmster Fall ist der Status quo (keine Korrektur).
  Springt die Geräte-Position pathologisch (Riesen-|D|), begrenzen Cap + Slew den Schaden auf
  ≤ 500 ppm Rate — hörbar unkritisch, im Log sichtbar (Saturation-Warn).
- **Semantikwechsel `av_drift_ms` (Residual):** nachgelagerte Konsumenten (Overlay, Report-Card,
  EditExportPage) interpretieren die Zahl neu. Pre-1.0 ohne Kompat-Pflicht; die Feld-Kommentare
  und product-spec werden im selben PR gezogen (Schritt 4/6), damit keine zweite Wahrheit
  entsteht.
- **Merged-Tracks bleiben unkorrigiert:** dokumentierte Limitation (KNOWN_LIMITATIONS), kein
  stiller Defekt.

---

## Offene Fragen (echte Produktentscheidungen)

1. **Default-ON + PCM/FLAC unter Slaving — Sign-off-pflichtig, blockiert Schritt 6:** Die Spec
   **empfiehlt** „Slaving Default an, codec-unabhängig" (Sync vor Bit-Exaktheit; greift ohnehin
   nur bei > 15 ms realer Drift; Expert-Schalter als Bit-Exaktheits-Opt-out), entscheidet das
   aber nicht — es bricht den Wortlaut der byte-identical-Zusagen
   (`product-spec.md:215-216`, `KNOWN_LIMITATIONS.md:143`), deren Reformulierung Teil von
   Schritt 6 ist. Alternativen mit identischem Regler/Decorator: Default off (A/B via Schalter
   bleibt möglich) oder PCM/FLAC-Tracks ausnehmen (Codec-Gate im Coordinator). Der
   Produkt-Owner entscheidet vor Schritt 6.
2. **Report-Card-Wortlaut:** Vorgeschlagen ist `Peak A/V drift: ±X ms · clock slaving
   corrected Y ms`. Falls die Korrektur dem User gar nicht genannt werden soll (reine
   Diagnostics-/Log-Sichtbarkeit), entfällt nur dieser Textzusatz — Verhalten identisch.
3. **Akzeptanzschwelle des 0.10-Soak-Gates:** Spec setzt ≤ 20 ms Netto-Drift über 2 h. Wenn das
   1.0-Quality-Gate (Roadmap `1.0.0`, SSIM/VMAF/A-V-Matrix) strenger zielen soll (z. B. ≤ 10 ms),
   muss das dort festgelegt werden — die Regler-Parameter geben es für typische Hardware her
   (stationäres Residual 3–6 ms bei 50–100 ppm; ≤ 10 ms erst ab Baselines > ~165 ppm nicht mehr
   erreichbar, siehe R_ss-Tabelle), dann werden Baseline-Rate und Messunschärfe des
   Klappensignals zu den limitierenden Faktoren.

---

## Adversarialer Review — Ergebnis

Sieben Einwände, alle gegen Code/Docs verifiziert; alle eingearbeitet (keiner zurückgewiesen):

1. **Testschwellen widersprachen R_ss (major) — eingearbeitet.** Nachgerechnet: bei 100 ppm und
   T = 60 s ist R_ss zwingend 6 ms; „< 3 ms"/„< 5 ms" waren per Design unbestehbar und hätten
   den bewusst verworfenen PI-Integrator erzwungen. Beide Schwellen auf `|R| < 7 ms`
   (R_ss + 1 ms Marge) korrigiert, explizit aus der Formel abgeleitet; kControlHorizonS = 60
   bleibt (Sanftheits-Begründung trägt), das 20-ms-Soak-Gate war konsistent.
2. **Default-ON bricht byte-identical-Zusage (major) — eingearbeitet.** `product-spec.md:215-216`
   und `KNOWN_LIMITATIONS.md:143` bestätigt. Die Spec präjudiziert nicht mehr: Default-ON +
   PCM/FLAC-Einbeziehung ist jetzt als Empfehlung mit **Sign-off-Gate vor Schritt 6** markiert;
   die Reformulierung BEIDER Zusagen-Stellen (nicht nur des Metrik-Absatzes 447-452) ist
   explizites Schritt-6-Deliverable; Alternativen (Default off / Codec-Gate) sind benannt.
3. **Stop-Tail trifft nach Engage den Default-Pfad (minor) — eingearbeitet.** Korrekt:
   Lazy-SwrContext bringt den undrainierten Tail (`KNOWN_LIMITATIONS.md:138-140`) in
   Filterlängen-Größe auf 48-k-Sessions. Als Risiko dokumentiert; Entscheidung: nicht drainen,
   KNOWN_LIMITATIONS-Passage in Schritt 6 verallgemeinern.
4. **Schritt 4 rief eine Schritt-5-API (minor) — eingearbeitet.** Schritte getauscht:
   Diagnostics-Umbau ist jetzt Schritt 4 (inkl. Übergangs-Callsite `residual = D, ppm = 0`,
   eigenständig kompilierbar), AudioThread-Verdrahtung Schritt 5; Querverweise nachgezogen.
5. **R_ss nur für 50 ppm genannt (minor) — eingearbeitet.** Residual-vs-Rate-Tabelle bis 500 ppm
   ergänzt (inkl. „ab ~250 ppm nicht mehr unter die Engage-Schwelle"), als
   KNOWN_LIMITATIONS-Punkt eingeplant, Soak-Gate-Vorbehalt (≲ 330 ppm) und offene Frage 3
   nachgezogen.
6. **„Ohne Korrektur nicht bestehbar" war Interpretation (minor) — eingearbeitet.** Problem-
   Abschnitt umformuliert: Roadmap fordert Validierung; die Korrektur ist als bedingte
   Versicherung gerahmt (No-op unter der Engage-Schwelle), nicht als Roadmap-Pflicht.
7. **duration_skew_ms unadressiert (minor) — eingearbeitet.** Diagnostics-Abschnitt klärt:
   Slaving lässt den Uhr-Drift-Anteil in duration_skew konvergieren (Metrik misst dann reiner
   die Starvation, für die sie da ist); Semantik unverändert, Arbeitsteilung wandert in die
   Feld-Kommentare, End-to-End-Test prüft die Konvergenz implizit mit.

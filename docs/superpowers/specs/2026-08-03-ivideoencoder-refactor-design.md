# IVideoEncoder-Refactor (Encoder-Stellschrauben, Schritt 1 von 3)

## Kontext

Encoder-Parameter (Preset, Rate-Control, Active Depth, Lookahead) stehen aktuell größtenteils
als Konstanten im Code statt einstellbar zu sein. Der beschlossene Arbeitsstrang dazu hat drei
Schritte:

1. **`IVideoEncoder`-Refactor** (dieser Schritt, M) — Vertrag zwischen `video_thread.cpp` und dem
   konkreten Encoder vervollständigen. Verhaltensneutral.
2. Active Depth >1 (S) — eigenes Brainstorming, wenn dieser Schritt steht.
3. Lookahead + Temporal AQ (M) — eigenes Brainstorming, baut auf Schritt 2 auf.

B-Frames sind als eigener, XL-großer Strang bewusst vertagt (niedrige Priorität, würde
FIFO-Slot-Zuordnung, VFR-GOP-Backstop und Keyframe-Vorhersage brechen).

Diese Spec deckt **nur Schritt 1** im Detail ab.

## Ziel

Nach diesem Schritt bleibt jede Aufnahme bit-identisch zu heute — nur die interne Kopplung
zwischen `video_thread.cpp` und dem Encoder ändert sich. Keine neue Konfigurationsmöglichkeit,
kein neues sichtbares Verhalten.

## Ausgangslage (verifiziert)

- `IVideoEncoder` (`libs/recorder_core/include/recorder_core/interfaces/IVideoEncoder.h`) ist in
  seiner Signatur sauber (keine NVENC-Typen), deckt aber nur einen Teil der Encoder-API ab:
  `Open`, `Configure`, `RegisterSlotTexture`, `SlotCount`, `AcquireFreeSlot`, `EncodeFrame`,
  `Flush`, `ReapCompleted`, `RequestKeyframe`, `Destroy`.
- `video_thread.cpp:558` hält ein **konkretes `NvencVideoEncoder`-Objekt als Wert** (nicht einmal
  einen Pointer): `NvencVideoEncoder nvenc;`. Es ruft direkt elf Methoden auf, die im Interface
  fehlen: `SetCodec`, `SetBitDepth`, `SetChroma`, `SetCq`, `SetRateControl`, `SetPreset`,
  `SetKeyframeIntervalSecs`, `SetConstantFrameRate`, `SetColor`, `GetInitInfo`, `ReleaseSlot`
  (Call-Sites: Zeilen 558–3628, `ReleaseSlot` allein an elf Stellen).
- Es existiert **keine Test-Implementierung** von `IVideoEncoder` — nur die echte
  `NvencVideoEncoder`. `video_thread.cpp`-Logik ist dadurch nur über echte
  GPU/NVENC-Hardware testbar (`test_session_e2e_real_file.cpp`).
- `video_thread.cpp` fragt **nirgends** einen `AdapterVendor` ab — das Encoder-Setup ist heute
  implizit NVIDIA-only, es gibt keine Vendor-Verzweigung.
- `RecorderConfig` trägt herstellernamige Felder (`nvenc_preset`, `nvenc_rate_control`,
  `nvenc_bitrate_kbps`); `EncoderInitInfo` (generische Diagnostik-Struktur) enthält direkt
  `NvencPreset`.
- `libs/capability` hat bereits ein echtes generisches Vendor-Modell
  (`AdapterVendor::{Nvidia,Amd,Intel,Other}`); AMD/Intel geben ehrlich "not wired" zurück statt zu
  faken.

## Entscheidungen

- **`NvencPreset` bleibt vendor-spezifisch.** Keine Kanonisierung zu einer generischen
  Preset-Skala in diesem Schritt — die UI zeigt P1–P7 nur bei erkanntem NVIDIA-Adapter. Eine
  echte Abstraktion würde nur gegen sich selbst geprüft, solange kein zweiter Hersteller real
  angebunden ist; kommt frühestens mit der AMD-Welle (0.12).
- **`RecorderConfig`s `nvenc_*`-Feldnamen bleiben unverändert.** Nur ein Vendor existiert real,
  Umbenennen wäre Spekulation.
- **`VideoEncoderFactory`/Dispatch wird jetzt gebaut, `EncoderCapabilitySchema` nicht.** Die
  Dispatch-Mechanik ("welcher Vendor → welche Implementierung") ist reine Steuerlogik und mit
  einem Fake-Encoder als zweitem "Vendor" hart testbar. Die Feld-*Form* für echte AMD/Intel-
  Capabilities ist dagegen unbekannt, bis ein echter zweiter Hersteller angebunden wird — dort
  würde ein selbstgebauter Fake nur die eigene Annahme gegen sich selbst prüfen, nicht gegen die
  Realität. Das Schema bleibt daher explizit außerhalb dieses Schritts.
- **Der Vendor-Wert am Aufrufpunkt bleibt hart codiert** (`AdapterVendor::Nvidia`), keine neue
  Config-Plumbing. Eine echte Geräteauswahl-Weiterleitung von `libs/capability` bis zum
  Factory-Aufruf wäre neue Plumbing für eine Fähigkeit, die noch nicht existiert, und würde das
  Verhaltensneutralitäts-Ziel verletzen.
- **APP-Row-Spec/Code-Widerspruch** (`product-spec.md` sagt "APP row defaults enabled", Code
  erzeugt sie im Default-Preset gar nicht) wird **separat** behandelt — thematisch unabhängig
  vom Encoder (Audio-Presets, nicht Video-Encoding).

## Design

### 1. Interface-Erweiterung

`IVideoEncoder` bekommt die elf oben genannten Methoden als virtuelle Methoden, 1:1 aus den
heutigen `NvencVideoEncoder`-Signaturen übernommen. `SetPreset` behält `NvencPreset` als
Parametertyp (siehe Entscheidungen).

### 2. `VideoEncoderFactory`

Neuer Typ in `libs/recorder_core/include/recorder_core/interfaces/VideoEncoderFactory.h` +
zugehöriger `.cpp`:

```cpp
class VideoEncoderFactory {
  public:
    virtual ~VideoEncoderFactory() = default;
    virtual std::unique_ptr<IVideoEncoder> Create(capability::AdapterVendor vendor) const;
};
```

Default-Implementierung: `Nvidia → make_unique<NvencVideoEncoder>()`, alles andere → `nullptr`.
Virtuell, damit Tests eine eigene Factory injizieren können, die für einen beliebigen
Vendor-Wert den `FakeVideoEncoder` liefert.

### 3. `video_thread.cpp`-Integration

- Zeile 558 wechselt von `NvencVideoEncoder nvenc;` (Wert) zu
  `std::unique_ptr<IVideoEncoder> encoder = factory.Create(capability::AdapterVendor::Nvidia);`
  (hart codierter Vendor-Wert, mit Kommentar, dass die echte Geräteauswahl erst mit der
  AMD-Welle durchgereicht wird).
- Alle ~15 `nvenc.Xyz(...)`-Aufrufe werden zu `encoder->Xyz(...)`. Rein mechanische Umbenennung,
  keine Logikänderung an den Call-Sites selbst.
- **Neuer Fehlerpfad** (heute unerreichbar, aber sauber definiert): `Create()` liefert `nullptr`,
  wenn kein Encoder für den Vendor verdrahtet ist. `video_thread.cpp` prüft das direkt nach dem
  Aufruf und behandelt `nullptr` als denselben fatalen Init-Fehler wie ein fehlgeschlagenes
  `Open()`/`Configure()` heute schon (bestehender `out_error`/Blocker-Pfad, keine neue
  Fehlerklasse).

### 4. Teststrategie

`FakeVideoEncoder` (neu, `libs/recorder_core/tests/fakes/fake_video_encoder.h`): implementiert
`IVideoEncoder` vollständig gegen einfache In-Memory-Zustände — konfigurierbare Slot-Anzahl,
`EncodeFrame` erzeugt einen synthetischen `EncodedVideoPacket` pro Aufruf (fixe Größe,
inkrementierender PTS), `Open`/`Configure`/`Flush` erfolgreich per Default, mit Hooks zum
Erzwingen von Fehlern (`out_error` setzen, `false` zurückgeben) für Fehlerpfad-Tests.

Neue Testdatei `libs/recorder_core/tests/test_video_thread_encoder_dispatch.cpp` deckt, was
bisher nur über echte Hardware indirekt geprüft war:

- Slot-Erschöpfung/`ReleaseSlot`-Pfade
- `Create()`-liefert-`nullptr`-Fehlerpfad
- Encode-Fehler mitten in der Aufnahme (`EncodeFrame` liefert `false`) → korrekte
  Fatal-Eskalation

Nicht abgedeckt: echte Bild-/Encode-Korrektheit — bleibt Domäne der bestehenden E2E-Tests mit
echter Hardware (`test_session_e2e_real_file.cpp`). Der Fake produziert keine validen
Bitstreams, nur strukturell korrekte `EncodedVideoPacket`s.

## Out of Scope

- `EncoderCapabilitySchema` (AMD/Intel-Feldform)
- `NvencPreset`-Kanonisierung
- `RecorderConfig`-Feldumbenennung
- Active Depth (Schritt 2), Lookahead/Temporal AQ (Schritt 3), B-Frames (vertagt)
- APP-Row-Spec/Code-Widerspruch (separates Thema)
- Echte Vendor-Weiterleitung von der Geräteauswahl zum Factory-Aufruf (kommt mit AMD-Welle)

## Tests

- Neu: `test_video_thread_encoder_dispatch.cpp` (siehe Teststrategie oben)
- Bestehend, muss weiterhin grün bleiben: `test_session_e2e_real_file.cpp`,
  `test_nvenc_gop_aq_config.cpp`, `test_frame_pacing.cpp`, `test_split_segments.cpp`

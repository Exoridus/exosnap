# Spec-Welle 2026-07-11 — Fable-Intelligenz in Specs konservieren

**Auftrag:** Letzter Fable-Tag. Umsetzungsreife Specs für alle offenen Review-Findings mit
Design-Bedarf + alle offenen Roadmap-/KNOWN_LIMITATIONS-Themen. KEINE Code-Änderungen.
Start der Welle: erst beim Session-Cron 18:12 (nach Usage-Reset). Umsetzung später mit
Opus/Sonnet auf Basis der Specs.

**Jede Spec enthält:** Problem · Ist-Zustand mit Datei-Referenzen · Design mit Alternativen
+ Entscheidung · Implementierungsschritte · Test-/Verify-Plan · Risiken.

**Ablage:** `.workspace/plans/<slug>-spec.md` (eine Datei pro Thema; Design-Kapitel inklusive).

**Prozess:** Autor-Agent (read-only bzgl. Code, schreibt nur die Spec-Datei) → adversarialer
Review durch ein ANDERES Modell (Fable-Spec → Opus-Review, Opus/Sonnet-Spec → Fable-Review)
→ Revisions-Agent (Autor-Modell) arbeitet Einwände ein. Pipeline pro Thema, keine Barrieren.

## Themenliste (Autor-Modell → Review-Modell)

### Fable-Autoren (harte Design-Themen)
1. `nvenc-async-pipeline-spec` — M-1: async NVENC + Submit-Ahead + Perf-Messinfrastruktur
   (Frame-Time-Histogramm, p99-Encode-Latenz); Keyframe-Metadaten-Vorhersage umbauen. → Opus
2. `encoder-quality-features-spec` — M-2-Rest: B-Frames/Lookahead/Temporal-AQ hinter
   Capability-Gate + SSIM/VMAF-Qualitätsmatrix (verschränkt mit 1.0-Quality-Gate;
   Keyframe-Prediction-Abhängigkeit zu M-1 beachten; #181 hat GOP+Spatial-AQ schon geliefert). → Opus
3. `av-clock-slaving-spec` — H-3 Stufe 3: swr-Drift-Kompensation Richtung QPC; Messmethode
   2-h-Soak/Klappensignal; baut auf echter Drift-Metrik (#191) auf. → Opus
4. `software-encoding-spec` — Roadmap 0.11: x264 (+optional SVT-AV1), Encoder-Factory-Ausbau,
   GPU→CPU-Readback, Performance-Warnungen, Fallback-Policy, Lizenz-/Patent-Audit, GPU-less-CI. → Opus
5. `amd-amf-encoder-spec` — Roadmap 0.12: nativer AMF-Encoder, Capability-Probe, Diagnostics-
   Provider, Test-Matrix, Fallback. → Opus
6. `intel-qsv-encoder-spec` — Roadmap 0.13: oneVPL/QSV, Allocator/Surface-Integration, Matrix. → Opus
7. `exclusive-fullscreen-capture-spec` — Fullscreen/Borderless/Exclusive-Matrix (0.12.x-Defer):
   Erkennungspfad, WGC/OD-Grenzen, ggf. Hook-Frage, Anti-Cheat-Posture-Schnittstelle. → Opus

### Opus-Autoren (mittlere Themen)
9. `record-start-preparing-state-spec` — M-9: schwere Gerätearbeit aus dem GUI-Thread in einen
   „Preparing"-State (StartRecording, Webcam-Start, Preview-Release-Handshake). → Fable
10. `diagnostics-support-bundle-spec` — Review §6: strukturiertes Log-Schema (spdlog, JSON-Lines,
    Session-ID), Session-Report-Artefakt, Ein-Klick-Support-Bundle (privacy-gescrubbt),
    Startup-Trace-Anzeige, Encoder-/Audio-Metriken, docs/troubleshooting.md. → Fable
11. `reliability-soak-spec` — Roadmap 0.10: Soak-Infrastruktur (Langzeit-Aufnahme automatisiert),
    A/V-Sync-Validierung (Klappensignal + ffprobe), Recovery-Drills. → Fable
12. `code-signing-spec` — Roadmap 0.10: Authenticode/SignPath abschließen, SmartScreen-Reputation,
    MSI+ZIP+Updater-Kette (signierte Artefakte im Updater-Verify-Pfad). → Fable
13. `hlg-und-hdr-achsen-spec` — HLG-Support + offene HDR-Farb-Achsen-Audit-Punkte (stale
    HDR-Facts, hdr_mode/color_range-Clamping, ACM-Query). Memory: project_hdr_color_axes_handoff,
    project_followup_waves_session_2026_07_05 (SEI-RELEASE-GATE-Liste beachten). → Fable
14. `editor-video-preview-spec` — KNOWN_LIMITATIONS: Video-Playback im Edit-Overlay (avfilter/
    swscale-Deployment-Entscheidung, Decode→Display-Pipeline, Scrubbing, HDR-Tonemap im Preview). → Fable
15. `localization-de-spec` — L-8 + 1.0-Slice: tr()-Sweep (Coordinator-Strings!), Qt-Linguist-Infra,
    Sprache-Umschalten, Zusammenspiel mit CodecLabels-Kanon. Memory: project_localization_de_10. → Fable
18. `display-identity-stability-spec` — KNOWN_LIMITATIONS: stabile Monitor-Identität statt GDI-Name
    (Topologie-Wechsel), Persistenz von Region/Display-Targets. → Fable
19. `device-hotswap-policy-spec` — KNOWN_LIMITATIONS: Capture-Device-Unplug mid-recording — bewusste
    Policy (sauber beenden vs. retargeten), Abgrenzung zu vorhandener OD-Hold/Reopen-Maschinerie. → Fable
20. `privacy-review-spec` — Roadmap 0.10: Privacy-Review-Checkliste (Scrubber-Abdeckung, PRIVACY.md-
    Abgleich, Crash-Annotations, Support-Bundle-Scrubbing — Schnittstelle zu 10.). → Fable

### Sonnet-Autoren (Bestandsaufnahme-lastig)
21. `multichannel-audio-spec` — Deferred aus 0.6: 5.1/7.1, Float-PCM, PCM/FLAC-in-MP4
    (Sample-Entry-/Player-Matrix statt ipcm). → Fable

## Kontext für alle Autoren
- Review-Volltext: `.workspace/review-fable-2026-07-10.md` (Findings + §5 Perf-Plan + §6 Diagnostics-Plan)
- Roadmap: `docs/roadmap.md` · Grenzen: `KNOWN_LIMITATIONS.md` · Architektur: `.workspace/architecture/system-overview.md`
- Alle Review-Findings außer M-1/M-2-Rest/H-3-St.3/M-9/L-8 sind bereits gemerged (#164–#192) —
  Ist-Zustand IMMER frisch aus dem Code erheben, nicht aus dem Review übernehmen.
- Engine bleibt UI-agnostisch; Container/Codec-Policy lebt im Resolver (#190); keine MVP-Expansion verstecken.

## Abschluss-Deliverable
Übersicht aller Specs: Pfad · Einzeiler · empfohlene Umsetzungsreihenfolge (gestaffelt nach
Abhängigkeiten: Messinfra vor Encoder-Tuning; Software-Encoding vor AMD/Intel; Signing früh).

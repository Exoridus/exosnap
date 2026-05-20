/* Record page — the main operational console */

function RecordPage() {
  const [state, setState] = React.useState("ready"); // ready | live | paused
  const [elapsed, setElapsed] = React.useState(0);
  const [target, setTarget] = React.useState("monitor");
  const [audioOn, setAudioOn] = React.useState({ app: true, mic: false, sys: true });
  const [levels, setLevels] = React.useState({ app: 0.55, mic: 0, sys: 0.32 });
  const [bitrate, setBitrate] = React.useState(38.4);
  const [frameMs, setFrameMs] = React.useState(2.1);
  const [dropped, setDropped] = React.useState(0);
  const [diskUsed, setDiskUsed] = React.useState(0);

  // Live tick when recording
  React.useEffect(() => {
    if (state !== "live") return;
    const t = setInterval(() => {
      setElapsed(e => e + 1);
      setLevels(l => ({
        app: audioOn.app ? 0.35 + Math.random() * 0.45 : 0,
        mic: audioOn.mic ? 0.15 + Math.random() * 0.35 : 0,
        sys: audioOn.sys ? 0.20 + Math.random() * 0.30 : 0,
      }));
      setBitrate(36 + Math.random() * 6);
      setFrameMs(1.8 + Math.random() * 0.8);
      setDiskUsed(d => d + (36 / 8) / 60); // rough MB/sec at ~38 Mbps -> per tick
    }, 1000);
    return () => clearInterval(t);
  }, [state, audioOn]);

  // ambient idle meter wiggle
  React.useEffect(() => {
    if (state === "live") return;
    const t = setInterval(() => {
      setLevels(l => ({
        app: audioOn.app ? 0.25 + Math.random() * 0.25 : 0,
        mic: audioOn.mic ? 0.10 + Math.random() * 0.20 : 0,
        sys: audioOn.sys ? 0.12 + Math.random() * 0.18 : 0,
      }));
    }, 700);
    return () => clearInterval(t);
  }, [state, audioOn]);

  const fmtTime = (s) => {
    const h = Math.floor(s / 3600).toString().padStart(2, "0");
    const m = Math.floor((s % 3600) / 60).toString().padStart(2, "0");
    const ss = (s % 60).toString().padStart(2, "0");
    return `${h}:${m}:${ss}`;
  };

  const live = state === "live";
  const paused = state === "paused";

  const checks = [
    { k: "NVENC AV1 encoder",    ok: true,  detail: "Ada · driver 555.85" },
    { k: "Display capture",      ok: true,  detail: "DDA · DISPLAY1 · 2560×1440 @ 165 Hz" },
    { k: "Audio loopback (APP)", ok: true,  detail: "WASAPI · Game.exe + 3 children" },
    { k: "Output destination",   ok: true,  detail: "D:\\Captures · 412 GB free" },
    { k: "Microphone",           ok: !audioOn.mic ? "off" : "warn", detail: audioOn.mic ? "No signal in last 8 s" : "Disabled by user" },
  ];

  const allOk = checks.every(c => c.ok === true || c.ok === "off");

  return (
    <div className="win">
      <Titlebar subtitle="Record"/>
      <div className="win-body">
        <Sidebar active="record" status={live ? "live" : "ready"}/>
        <div className="content">
          <ContentHead
            crumb="01 · RECORD"
            title="Record"
            sub="Operational view — target, readiness, and live runtime."
            right={
              <>
                <span className="mono">CPU 8.2%</span>
                <span style={{ color: "var(--text-3)" }}>·</span>
                <span className="mono">GPU 14.4%</span>
                <span style={{ color: "var(--text-3)" }}>·</span>
                <span className="mono">RAM 612 MB</span>
              </>
            }
          />
          <div className="content-body">

            {/* ====== Live preview + record controls ====== */}
            <div style={{ display: "grid", gridTemplateColumns: "1fr 280px", gap: 14, marginBottom: 18 }}>
              <div className="preview" style={{ aspectRatio: "16 / 9", minHeight: 280 }}>
                <span className="corners"><span/><span/><span/><span/></span>
                {/* status badge over preview */}
                <div style={{ position: "absolute", top: 12, left: 16, display: "flex", gap: 8, alignItems: "center" }}>
                  {live && <Pill kind="live">● REC {fmtTime(elapsed)}</Pill>}
                  {paused && <Pill kind="warn">❚❚ PAUSED {fmtTime(elapsed)}</Pill>}
                  {!live && !paused && <Pill kind="ok">READY</Pill>}
                  <span className="mono" style={{ fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.06em" }}>DISPLAY1 · 2560×1440 · 60 fps</span>
                </div>
                {/* preview placeholder content */}
                <div className="crosshair">
                  ◇ PREVIEW SURFACE<br/>
                  <span className="res">{live ? "CAPTURING" : "IDLE · DDA TEX SHARED"}</span>
                </div>
                {/* scanline indicator on live */}
                {live && (
                  <div style={{ position: "absolute", inset: 0, pointerEvents: "none", background: "linear-gradient(180deg, transparent 50%, rgba(241,180,0,0.04) 50%)", backgroundSize: "100% 4px", mixBlendMode: "screen" }}/>
                )}
                {/* runtime overlay bottom-left */}
                <div style={{ position: "absolute", bottom: 12, left: 16, display: "flex", gap: 12, fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.04em" }}>
                  <span>FRAME {frameMs.toFixed(2)} ms</span>
                  <span>·</span>
                  <span>BITRATE {bitrate.toFixed(1)} Mb/s</span>
                  <span>·</span>
                  <span style={{ color: dropped > 0 ? "var(--err)" : "var(--text-2)" }}>DROP {dropped}</span>
                </div>
                {/* aperture corner badge */}
                <div style={{ position: "absolute", bottom: 12, right: 16, fontFamily: "var(--font-mono)", fontSize: 10, color: "var(--accent)", letterSpacing: "0.1em" }}>
                  AV1 · CQ 24
                </div>
              </div>

              {/* Right rail — controls */}
              <div className="col" style={{ gap: 12 }}>
                <div className="panel panel-pad" style={{ display: "flex", flexDirection: "column", gap: 10 }}>
                  <div className="row between">
                    <div style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.14em", textTransform: "uppercase" }}>
                      {live ? "Recording" : paused ? "Paused" : "Ready to record"}
                    </div>
                    <span className="btn kbd">ALT+F9</span>
                  </div>

                  {!live && !paused ? (
                    <button className="btn record" disabled={!allOk} onClick={() => { setState("live"); setElapsed(0); setDiskUsed(0); }}>
                      <Icon.Record/> Start recording
                    </button>
                  ) : (
                    <div style={{ display: "flex", gap: 6 }}>
                      <button className="btn record stop" style={{ flex: 1 }} onClick={() => setState("ready")}>
                        ■ Stop
                      </button>
                      <button className="btn" style={{ height: 46, padding: "0 14px" }} onClick={() => setState(paused ? "live" : "paused")}>
                        {paused ? "▶" : "❚❚"}
                      </button>
                    </div>
                  )}

                  {/* big timer */}
                  <div style={{ fontFamily: "var(--font-mono)", fontSize: 28, color: live ? "var(--accent)" : "var(--text-1)", letterSpacing: "0.04em", textAlign: "center", padding: "6px 0 2px", fontVariantNumeric: "tabular-nums" }}>
                    {fmtTime(elapsed)}
                  </div>
                  <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 8, fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)" }}>
                    <div className="row between"><span>SIZE</span><b style={{ color: "var(--text-0)", fontWeight: 500 }}>{diskUsed.toFixed(1)} MB</b></div>
                    <div className="row between"><span>EST</span><b style={{ color: "var(--text-0)", fontWeight: 500 }}>~{(bitrate * 60 / 8 / 1024).toFixed(2)} GB/h</b></div>
                  </div>
                </div>

                <div className="panel panel-pad">
                  <div style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.14em", textTransform: "uppercase", marginBottom: 8 }}>Quick toggles</div>
                  <div className="col" style={{ gap: 4 }}>
                    <div className="row between" style={{ padding: "2px 0" }}>
                      <span style={{ fontSize: 12, color: "var(--text-0)" }}>Capture cursor</span>
                      <Toggle on/>
                    </div>
                    <div className="row between" style={{ padding: "2px 0" }}>
                      <span style={{ fontSize: 12, color: "var(--text-0)" }}>Hardware encode</span>
                      <Toggle on/>
                    </div>
                    <div className="row between" style={{ padding: "2px 0" }}>
                      <span style={{ fontSize: 12, color: "var(--text-0)" }}>Auto-split @ 4 h</span>
                      <Toggle on={false}/>
                    </div>
                  </div>
                </div>
              </div>
            </div>

            {/* ====== Capture target ====== */}
            <div className="section">
              <SectionHead meta={`${target === "monitor" ? "DISPLAY1" : target === "window" ? "Game.exe" : "Region"} · 2560×1440 · 60 fps`}>Capture target</SectionHead>
              <div style={{ display: "grid", gridTemplateColumns: "repeat(3, 1fr)", gap: 8 }}>
                {[
                  { id: "monitor", label: "Monitor", sub: "DISPLAY1 · 2560×1440 · DDA" },
                  { id: "window",  label: "Window",  sub: "Game.exe · DXGI hook" },
                  { id: "region",  label: "Region",  sub: "1920×1080 · custom rect" },
                ].map(opt => {
                  const selected = target === opt.id;
                  return (
                    <div key={opt.id}
                         onClick={() => setTarget(opt.id)}
                         className="panel"
                         style={{
                           padding: "12px 14px",
                           cursor: "pointer",
                           borderColor: selected ? "var(--accent)" : "var(--line-1)",
                           background: selected ? "var(--accent-soft)" : "var(--bg-2)",
                         }}>
                      <div className="row between" style={{ marginBottom: 4 }}>
                        <span style={{ fontWeight: 500, color: "var(--text-0)" }}>{opt.label}</span>
                        <span style={{ color: selected ? "var(--accent)" : "var(--text-3)", fontFamily: "var(--font-mono)", fontSize: 10 }}>
                          {selected ? "● ACTIVE" : "○"}
                        </span>
                      </div>
                      <div style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.04em" }}>{opt.sub}</div>
                    </div>
                  );
                })}
              </div>
            </div>

            {/* ====== Readiness checks ====== */}
            <div className="section">
              <SectionHead meta={allOk ? "ALL CLEAR" : "BLOCKERS PRESENT"}>Readiness</SectionHead>
              <div className="panel">
                {checks.map((c, i) => (
                  <div key={c.k} className="row between" style={{
                    padding: "10px 14px",
                    borderTop: i === 0 ? "none" : "1px solid var(--line-1)",
                  }}>
                    <div className="row" style={{ gap: 10 }}>
                      {c.ok === true && <span style={{ color: "var(--ok)", fontFamily: "var(--font-mono)", width: 14 }}>✓</span>}
                      {c.ok === "warn" && <span style={{ color: "var(--warn)", fontFamily: "var(--font-mono)", width: 14 }}>!</span>}
                      {c.ok === "off" && <span style={{ color: "var(--text-3)", fontFamily: "var(--font-mono)", width: 14 }}>·</span>}
                      {c.ok === false && <span style={{ color: "var(--err)", fontFamily: "var(--font-mono)", width: 14 }}>✕</span>}
                      <span style={{ fontSize: 12.5, color: "var(--text-0)" }}>{c.k}</span>
                    </div>
                    <span style={{ fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--text-2)" }}>{c.detail}</span>
                  </div>
                ))}
              </div>
            </div>

            {/* ====== Audio activity ====== */}
            <div className="section">
              <SectionHead meta="OPUS · 48 kHz · 3 TRACKS">Audio activity</SectionHead>
              <div className="panel">
                {[
                  { id: "app", tag: "APP", name: "Game.exe + child processes", on: audioOn.app, lvl: levels.app },
                  { id: "mic", tag: "MIC", name: "Microphone — Windows default", on: audioOn.mic, lvl: levels.mic },
                  { id: "sys", tag: "SYS", name: "Other system audio", on: audioOn.sys, lvl: levels.sys },
                ].map((row, i) => (
                  <div key={row.id} className="row" style={{
                    padding: "9px 14px",
                    gap: 14,
                    borderTop: i === 0 ? "none" : "1px solid var(--line-1)",
                  }}>
                    <span style={{
                      fontFamily: "var(--font-mono)", fontSize: 10.5, letterSpacing: "0.1em",
                      color: row.on ? "var(--accent)" : "var(--text-3)",
                      border: "1px solid " + (row.on ? "var(--accent-line)" : "var(--line-2)"),
                      padding: "2px 6px", borderRadius: "var(--r-sm)",
                      background: row.on ? "var(--accent-soft)" : "transparent",
                      width: 38, textAlign: "center",
                    }}>{row.tag}</span>
                    <span style={{ flex: 1, fontSize: 12.5, color: row.on ? "var(--text-0)" : "var(--text-2)" }}>{row.name}</span>
                    <Meter level={row.lvl} segs={24}/>
                    <span style={{ width: 44, fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--text-2)", textAlign: "right", fontVariantNumeric: "tabular-nums" }}>
                      {row.on ? `${Math.round(-40 + row.lvl * 36)} dB` : "—"}
                    </span>
                    <Toggle on={row.on} onClick={() => setAudioOn(a => ({ ...a, [row.id]: !a[row.id] }))}/>
                  </div>
                ))}
              </div>
            </div>

            {/* ====== Output target footer ====== */}
            <div className="section">
              <SectionHead meta="MKV · AV1 · OPUS">Destination</SectionHead>
              <div className="panel panel-pad row between">
                <div className="col" style={{ gap: 2 }}>
                  <span style={{ fontFamily: "var(--font-mono)", fontSize: 11.5, color: "var(--text-0)" }}>
                    D:\Captures\exosnap_<span style={{ color: "var(--accent)" }}>2026-05-18_14-22-08</span>.mkv
                  </span>
                  <span style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)" }}>
                    412.6 GB free · est. ~16.8 GB/h at current settings
                  </span>
                </div>
                <div className="row" style={{ gap: 6 }}>
                  <button className="btn ghost sm">Reveal in folder</button>
                  <button className="btn ghost sm">Change…</button>
                </div>
              </div>
            </div>

          </div>
        </div>
      </div>
    </div>
  );
}

window.RecordPage = RecordPage;

/* Video + Audio settings pages — establishes the settings system style */

function VideoPage() {
  const [codec, setCodec] = React.useState("av1");
  const [quality, setQuality] = React.useState("balanced");
  const [res, setRes] = React.useState("source");
  const [cursor, setCursor] = React.useState(true);
  const [advOpen, setAdvOpen] = React.useState(true);
  const [cq, setCq] = React.useState(24);

  const codecMeta = {
    av1:  { encoder: "NVENC AV1",  preset: "P4",   note: "Recommended" },
    hevc: { encoder: "NVENC HEVC", preset: "P5",   note: "Wide playback" },
    h264: { encoder: "NVENC H.264", preset: "P5",  note: "Maximum compatibility" },
  };
  const cur = codecMeta[codec];

  return (
    <div className="win">
      <Titlebar subtitle="Video"/>
      <div className="win-body">
        <Sidebar active="video"/>
        <div className="content">
          <ContentHead
            crumb="02 · VIDEO"
            title="Video"
            sub="Codec, frame rate, and quality."
            right={<><span className="mono">PROFILE · Default</span><span style={{color:"var(--text-3)"}}>·</span><span className="mono amber">UNSAVED</span></>}
          />
          <div className="content-body">

            <div style={{ display: "grid", gridTemplateColumns: "1fr 320px", gap: 18 }}>
              <div>

                <div className="section">
                  <SectionHead meta="CFR · LOCKED THIS RELEASE">Frame rate</SectionHead>
                  <div className="panel panel-pad row between">
                    <div>
                      <div style={{ fontFamily: "var(--font-mono)", fontSize: 24, color: "var(--text-0)" }}>60.00 <span style={{ color: "var(--text-2)", fontSize: 14 }}>fps</span></div>
                      <div style={{ fontSize: 11.5, color: "var(--text-2)", marginTop: 2 }}>Constant frame rate — variable will return in 0.5.</div>
                    </div>
                    <span className="pill">CONSTANT</span>
                  </div>
                </div>

                <div className="section">
                  <SectionHead>Resolution</SectionHead>
                  <div className="panel">
                    <div style={{ padding: "10px 14px" }}>
                      <Radio checked={res==="source"} onClick={() => setRes("source")} tag="2560×1440">
                        Source resolution
                      </Radio>
                    </div>
                    <div style={{ borderTop: "1px solid var(--line-1)", padding: "10px 14px" }}>
                      <div className="row between">
                        <Radio checked={res==="scale"} onClick={() => setRes("scale")} tag="1080p">
                          Scale to…
                        </Radio>
                        <select className="select" style={{ width: 140 }} disabled={res !== "scale"}>
                          <option>1920 × 1080</option>
                          <option>2560 × 1440</option>
                          <option>1280 × 720</option>
                        </select>
                      </div>
                    </div>
                  </div>
                </div>

                <div className="section">
                  <SectionHead meta="HARDWARE ENCODE">Codec</SectionHead>
                  <div style={{ display: "grid", gridTemplateColumns: "repeat(3, 1fr)", gap: 8 }}>
                    {[
                      { id: "av1",  name: "AV1",  sub: "Best compression",      tag: "RECOMMENDED" },
                      { id: "hevc", name: "HEVC", sub: "Wide playback support",  tag: "H.265" },
                      { id: "h264", name: "H.264", sub: "Maximum compatibility", tag: "FALLBACK" },
                    ].map(opt => {
                      const sel = codec === opt.id;
                      return (
                        <div key={opt.id} onClick={() => setCodec(opt.id)}
                             className="panel"
                             style={{
                               padding: "12px 14px", cursor: "pointer",
                               borderColor: sel ? "var(--accent)" : "var(--line-1)",
                               background: sel ? "var(--accent-soft)" : "var(--bg-2)",
                             }}>
                          <div className="row between" style={{ marginBottom: 6 }}>
                            <span style={{ fontWeight: 500, fontSize: 14, color: "var(--text-0)" }}>{opt.name}</span>
                            <span style={{
                              fontFamily: "var(--font-mono)", fontSize: 9,
                              color: sel ? "var(--accent)" : "var(--text-3)",
                              letterSpacing: "0.08em",
                            }}>{sel ? "● SELECTED" : opt.tag}</span>
                          </div>
                          <div style={{ fontSize: 11.5, color: "var(--text-2)" }}>{opt.sub}</div>
                        </div>
                      );
                    })}
                  </div>
                </div>

                <div className="section">
                  <SectionHead>Quality</SectionHead>
                  <div className="panel">
                    {[
                      { id: "high",     name: "High quality", sub: "CQ 19 · ~62 Mb/s · large files" },
                      { id: "balanced", name: "Balanced",     sub: "CQ 24 · ~38 Mb/s · default" },
                      { id: "smaller",  name: "Smaller files", sub: "CQ 30 · ~18 Mb/s · streaming-friendly" },
                    ].map((q, i) => (
                      <div key={q.id} style={{
                        padding: "9px 14px",
                        borderTop: i === 0 ? "none" : "1px solid var(--line-1)",
                      }}>
                        <div className="row between">
                          <Radio checked={quality===q.id} onClick={() => setQuality(q.id)}>
                            {q.name}
                          </Radio>
                          <span style={{ fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--text-2)" }}>{q.sub}</span>
                        </div>
                      </div>
                    ))}
                  </div>
                </div>

                <div className="section">
                  <SectionHead>Cursor</SectionHead>
                  <div className="panel panel-pad row between">
                    <div>
                      <Check checked={cursor} onClick={() => setCursor(!cursor)}>Capture mouse cursor</Check>
                      <div style={{ fontSize: 11.5, color: "var(--text-2)", marginTop: 2, paddingLeft: 22 }}>
                        Includes hardware cursor overlay in the output stream.
                      </div>
                    </div>
                  </div>
                </div>

                {/* Advanced */}
                <div className="section">
                  <div className="row" style={{ cursor: "pointer", padding: "4px 0" }} onClick={() => setAdvOpen(!advOpen)}>
                    <span style={{ transform: advOpen ? "rotate(90deg)" : "none", transition: "transform .12s", display: "inline-flex" }}>
                      <Icon.Chevron/>
                    </span>
                    <span style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.14em", textTransform: "uppercase" }}>Advanced encoder settings</span>
                  </div>
                  {advOpen && (
                    <div className="panel panel-pad" style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 14 }}>
                      <div className="col" style={{ gap: 4 }}>
                        <label className="field-label">Rate control</label>
                        <select className="select"><option>CQP — Constant quantizer</option><option>VBR</option><option>CBR</option></select>
                      </div>
                      <div className="col" style={{ gap: 4 }}>
                        <label className="field-label">CQ / CQP value <span className="mono" style={{ color: "var(--accent)" }}>· {cq}</span></label>
                        <div className="row" style={{ gap: 8 }}>
                          <input type="range" min="14" max="40" value={cq} onChange={e => setCq(+e.target.value)} style={{ flex: 1, accentColor: "var(--accent)" }}/>
                        </div>
                      </div>
                      <div className="col" style={{ gap: 4 }}>
                        <label className="field-label">NVENC preset</label>
                        <select className="select" defaultValue="P4"><option>P1 — Fastest</option><option>P4 — Balanced</option><option>P7 — Highest quality</option></select>
                      </div>
                      <div className="col" style={{ gap: 4 }}>
                        <label className="field-label">GOP / keyframe interval</label>
                        <input className="input" defaultValue="60"/>
                      </div>
                      <div className="col" style={{ gap: 4 }}>
                        <label className="field-label">B-frames</label>
                        <input className="input" defaultValue="0"/>
                      </div>
                      <div className="col" style={{ gap: 4 }}>
                        <label className="field-label">Color space</label>
                        <select className="select"><option>BT.709 · Limited</option><option>BT.709 · Full</option><option>BT.2020</option></select>
                      </div>
                    </div>
                  )}
                </div>

              </div>

              {/* Right rail — effective settings */}
              <div>
                <div style={{ position: "sticky", top: 0 }}>
                  <div className="panel panel-pad">
                    <div style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.14em", textTransform: "uppercase", marginBottom: 12 }}>
                      Effective output
                    </div>
                    <div className="kv">
                      <span className="k">ENCODER</span>     <span className="v amber">{cur.encoder}</span>
                      <span className="k">PRESET</span>      <span className="v">{cur.preset} <span className="muted">({quality})</span></span>
                      <span className="k">RATE CTRL</span>   <span className="v">CQP</span>
                      <span className="k">CQ</span>          <span className="v">{cq}</span>
                      <span className="k">FRAME RATE</span>  <span className="v">CFR 60.00</span>
                      <span className="k">RESOLUTION</span>  <span className="v">{res === "source" ? "2560×1440" : "1920×1080"}</span>
                      <span className="k">GOP</span>         <span className="v">60</span>
                      <span className="k">B-FRAMES</span>    <span className="v">0</span>
                      <span className="k">CURSOR</span>      <span className="v">{cursor ? "Captured" : "Hidden"}</span>
                    </div>
                    <hr className="rule"/>
                    <div className="kv">
                      <span className="k">EST. BITRATE</span><span className="v">~{quality === "high" ? "62" : quality === "balanced" ? "38" : "18"} Mb/s</span>
                      <span className="k">EST. SIZE/H</span> <span className="v">~{quality === "high" ? "27.9" : "17.1" } GB</span>
                    </div>
                  </div>

                  <div className="panel panel-pad" style={{ marginTop: 10 }}>
                    <div className="row between" style={{ marginBottom: 6 }}>
                      <span style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.14em", textTransform: "uppercase" }}>Compatibility</span>
                      <Pill kind="ok">VERIFIED</Pill>
                    </div>
                    <div style={{ fontSize: 11.5, color: "var(--text-2)", lineHeight: 1.5 }}>
                      Detected GPU supports {cur.encoder}. Driver 555.85 meets minimum.
                    </div>
                  </div>
                </div>
              </div>
            </div>

          </div>
        </div>
      </div>
    </div>
  );
}

function AudioPage() {
  const [rows, setRows] = React.useState([
    { id: "app", tag: "APP", name: "Selected application audio", src: "Game.exe + child processes", on: true,  merge: false },
    { id: "mic", tag: "MIC", name: "Microphone",                  src: "Follow Windows default",     on: true,  merge: false },
    { id: "sys", tag: "SYS", name: "Other system audio",          src: "Everything except selected app", on: true, merge: false },
  ]);
  const [levels, setLevels] = React.useState({ app: 0.6, mic: 0.3, sys: 0.4 });

  React.useEffect(() => {
    const t = setInterval(() => {
      setLevels({
        app: rows.find(r => r.id === "app").on ? 0.30 + Math.random() * 0.45 : 0,
        mic: rows.find(r => r.id === "mic").on ? 0.10 + Math.random() * 0.30 : 0,
        sys: rows.find(r => r.id === "sys").on ? 0.20 + Math.random() * 0.30 : 0,
      });
    }, 600);
    return () => clearInterval(t);
  }, [rows]);

  const resulting = (() => {
    const tracks = [];
    let cur = null;
    rows.forEach((r, i) => {
      if (!r.on) return;
      if (r.merge && i > 0 && cur) cur.parts.push(r.tag);
      else { cur = { parts: [r.tag] }; tracks.push(cur); }
    });
    return tracks;
  })();

  const toggle = (id, key) => setRows(rs => rs.map(r => r.id === id ? { ...r, [key]: !r[key] } : r));

  return (
    <div className="win">
      <Titlebar subtitle="Audio"/>
      <div className="win-body">
        <Sidebar active="audio"/>
        <div className="content">
          <ContentHead
            crumb="03 · AUDIO"
            title="Audio"
            sub="Sources, track layout, and encoding."
            right={<><span className="mono">OPUS · 48 kHz · 192 kb/s</span></>}
          />
          <div className="content-body">

            <div className="section">
              <SectionHead meta="DRAG TO REORDER · MERGE COMBINES ROWS">Sources</SectionHead>
              <div className="col" style={{ gap: 8 }}>
                {rows.map((r, i) => (
                  <div key={r.id} className="panel" style={{
                    padding: "12px 12px 12px 8px",
                    borderColor: r.on ? "var(--line-2)" : "var(--line-1)",
                    opacity: r.on ? 1 : 0.55,
                  }}>
                    <div className="row" style={{ gap: 10 }}>
                      <span style={{ cursor: "grab", color: "var(--text-3)", padding: "0 4px" }}><Icon.Grip/></span>
                      <span style={{
                        fontFamily: "var(--font-mono)", fontSize: 10.5, letterSpacing: "0.1em",
                        color: r.on ? "var(--accent)" : "var(--text-3)",
                        border: "1px solid " + (r.on ? "var(--accent-line)" : "var(--line-2)"),
                        padding: "3px 8px", borderRadius: "var(--r-sm)",
                        background: r.on ? "var(--accent-soft)" : "transparent",
                        width: 44, textAlign: "center",
                      }}>{r.tag}</span>
                      <div style={{ flex: 1 }}>
                        <div style={{ fontWeight: 500, fontSize: 13, color: "var(--text-0)" }}>{r.name}</div>
                        <div style={{ fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--text-2)", marginTop: 2 }}>
                          <span style={{ color: "var(--text-3)" }}>SOURCE · </span>{r.src}
                        </div>
                      </div>
                      <Meter level={levels[r.id]} segs={20}/>
                      <span style={{ fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--text-2)", width: 44, textAlign: "right", fontVariantNumeric: "tabular-nums" }}>
                        {r.on ? `${Math.round(-40 + levels[r.id] * 36)} dB` : "—"}
                      </span>
                      <div style={{ width: 1, height: 22, background: "var(--line-1)" }}/>
                      {i > 0 && (
                        <Check checked={r.merge} onClick={() => toggle(r.id, "merge")}>
                          <span style={{ fontSize: 11.5, color: "var(--text-1)" }}>Merge ↑</span>
                        </Check>
                      )}
                      <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
                        <Toggle on={r.on} onClick={() => toggle(r.id, "on")}/>
                      </div>
                    </div>
                  </div>
                ))}
              </div>
            </div>

            <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 14, marginTop: 18 }}>
              <div className="section" style={{ margin: 0 }}>
                <SectionHead meta={`${resulting.length} TRACK${resulting.length === 1 ? "" : "S"}`}>Resulting tracks</SectionHead>
                <div className="panel">
                  {resulting.length === 0 ? (
                    <div style={{ padding: 14, color: "var(--text-2)", fontSize: 12.5 }}>No tracks — all sources disabled.</div>
                  ) : resulting.map((t, i) => (
                    <div key={i} className="row between" style={{
                      padding: "10px 14px",
                      borderTop: i === 0 ? "none" : "1px solid var(--line-1)",
                    }}>
                      <span style={{ fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--text-2)" }}>{String(i + 1).padStart(2, "0")}</span>
                      <span style={{ flex: 1, paddingLeft: 14, fontSize: 12.5, color: "var(--text-0)" }}>
                        {t.parts.map((p, k) => (
                          <span key={k}>
                            <span style={{ fontFamily: "var(--font-mono)", color: "var(--accent)" }}>{p}</span>
                            {k < t.parts.length - 1 && <span style={{ color: "var(--text-3)" }}> + </span>}
                          </span>
                        ))}
                      </span>
                      <span className="mono" style={{ fontSize: 11, color: "var(--text-2)" }}>STEREO · 48 kHz</span>
                    </div>
                  ))}
                </div>
              </div>

              <div className="section" style={{ margin: 0 }}>
                <SectionHead>Encoding</SectionHead>
                <div className="panel panel-pad">
                  <div className="kv">
                    <span className="k">CODEC</span>      <span className="v amber">Opus</span>
                    <span className="k">BITRATE</span>    <span className="v">192 kb/s · per track</span>
                    <span className="k">SAMPLE RATE</span><span className="v">48 000 Hz</span>
                    <span className="k">CHANNELS</span>   <span className="v">Stereo</span>
                    <span className="k">APP CAPTURE</span><span className="v">WASAPI loopback · per-process</span>
                  </div>
                  <hr className="rule"/>
                  <div style={{ fontSize: 11.5, color: "var(--text-2)", lineHeight: 1.5 }}>
                    Codec is determined by the container — switch to MP4 on the Output page to use AAC instead.
                  </div>
                </div>
              </div>
            </div>

          </div>
        </div>
      </div>
    </div>
  );
}

window.VideoPage = VideoPage;
window.AudioPage = AudioPage;

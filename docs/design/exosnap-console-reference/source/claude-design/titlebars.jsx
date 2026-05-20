/* ExoSnap — Integrated titlebar / app-chrome system (Part B)
 *
 * Three variants of an integrated top-chrome applied to the Record screen.
 * Each merges the OS window titlebar with the app's top bar — no more
 * stacked "OS chrome → blank gap → page header". Drag region, window
 * controls, brand, page context, and state are unified in one band.
 */

// Refined Snap Moment mark for chrome use
const SnapMark = ({ w = 22, color = "var(--text-0)", dot = "var(--accent)" }) => (
  <svg width={w} height={w} viewBox="0 0 32 32" fill="none">
    <path d="M5 9V5h10" stroke={color} strokeWidth="2" strokeLinecap="square"/>
    <path d="M27 17v10H21" stroke={color} strokeWidth="2" strokeLinecap="square"/>
    <circle cx="16" cy="16" r="2.4" fill={dot}/>
  </svg>
);

// Reusable Windows control glyphs
const WinCtrls = ({ height = 44 }) => (
  <div className="row" style={{ height, alignSelf: "stretch", WebkitAppRegion: "no-drag" }}>
    <span style={{ width: 46, height: "100%", display: "inline-flex", alignItems: "center", justifyContent: "center", color: "var(--text-2)" }}>
      <svg width="10" height="10" viewBox="0 0 10 10"><path d="M1 5h8" stroke="currentColor" strokeWidth="1"/></svg>
    </span>
    <span style={{ width: 46, height: "100%", display: "inline-flex", alignItems: "center", justifyContent: "center", color: "var(--text-2)" }}>
      <svg width="10" height="10" viewBox="0 0 10 10"><rect x="1.5" y="1.5" width="7" height="7" fill="none" stroke="currentColor" strokeWidth="1"/></svg>
    </span>
    <span style={{ width: 46, height: "100%", display: "inline-flex", alignItems: "center", justifyContent: "center", color: "var(--text-2)" }}>
      <svg width="10" height="10" viewBox="0 0 10 10"><path d="M1.5 1.5l7 7M8.5 1.5l-7 7" stroke="currentColor" strokeWidth="1"/></svg>
    </span>
  </div>
);

// ─────────────────────────────────────────────────────────────
// Variant A — Brand-led chrome
// 64 px tall. Brand mark + wordmark dominate the left. Center holds the
// current screen as a quiet breadcrumb. Right side reserves "Default ·
// Unsaved" profile state. CPU/GPU/RAM stay inside the page.
// ─────────────────────────────────────────────────────────────
function ChromeA() {
  return (
    <div style={{
      height: 64,
      background: "linear-gradient(180deg, #1a1813 0%, #14130f 100%)",
      borderBottom: "1px solid var(--line-1)",
      display: "flex",
      alignItems: "stretch",
      flexShrink: 0,
      position: "relative",
    }}>
      {/* drag region marker — diagonal hairline at top-left corner of bar */}
      <div style={{
        position: "absolute", inset: 0, pointerEvents: "none",
        background: "repeating-linear-gradient(135deg, transparent 0 22px, rgba(241,180,0,0.025) 22px 24px)",
      }}/>

      {/* Brand block */}
      <div style={{ display: "flex", alignItems: "center", padding: "0 20px", gap: 12, borderRight: "1px solid var(--line-1)" }}>
        <SnapMark w={26}/>
        <div style={{ display: "flex", flexDirection: "column", gap: 1 }}>
          <span style={{ fontFamily: "var(--font-mono)", fontSize: 14, letterSpacing: "0.18em", color: "var(--text-0)", fontWeight: 500, lineHeight: 1 }}>
            EXO<span style={{ color: "var(--accent)" }}>·</span>SNAP
          </span>
          <span style={{ fontFamily: "var(--font-mono)", fontSize: 8.5, letterSpacing: "0.22em", color: "var(--text-3)", lineHeight: 1 }}>
            PRECISION CAPTURE
          </span>
        </div>
      </div>

      {/* Page context (drag region) */}
      <div style={{ flex: 1, display: "flex", alignItems: "center", padding: "0 20px", gap: 14 }}>
        <span style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-3)", letterSpacing: "0.14em" }}>01</span>
        <span style={{ fontSize: 14, color: "var(--text-0)", fontWeight: 500 }}>Record</span>
        <span style={{ color: "var(--line-3)" }}>/</span>
        <span style={{ fontSize: 12.5, color: "var(--text-2)" }}>Operational view</span>
      </div>

      {/* Profile + window controls */}
      <div className="row" style={{ gap: 0 }}>
        <div className="row" style={{ gap: 8, padding: "0 16px", borderLeft: "1px solid var(--line-1)" }}>
          <div className="col" style={{ gap: 1, alignItems: "flex-end" }}>
            <span style={{ fontFamily: "var(--font-mono)", fontSize: 10, letterSpacing: "0.12em", color: "var(--text-3)" }}>PROFILE</span>
            <span style={{ fontFamily: "var(--font-mono)", fontSize: 11.5, color: "var(--text-0)" }}>Default <span className="amber">· unsaved</span></span>
          </div>
        </div>
        <WinCtrls height={64}/>
      </div>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────
// Variant B — Operational chrome
// 56 px tall. Mark stays compact on the left. Center holds the active
// screen + a status pill (READY / ● REC 00:42 / BLOCKED). Right packs
// CPU / GPU / RAM micro-readouts in mono — every screen sees these.
// ─────────────────────────────────────────────────────────────
function ChromeB({ recording = false, elapsed = 0 }) {
  const fmt = (s) => {
    const m = Math.floor(s / 60).toString().padStart(2, "0");
    const ss = (s % 60).toString().padStart(2, "0");
    return `${m}:${ss}`;
  };
  return (
    <div style={{
      height: 56,
      background: "#14130f",
      borderBottom: "1px solid " + (recording ? "var(--accent-line)" : "var(--line-1)"),
      display: "flex", alignItems: "stretch", flexShrink: 0,
      boxShadow: recording ? "inset 0 -1px 0 var(--accent), inset 0 2px 0 rgba(241,180,0,0.06)" : "none",
      position: "relative",
    }}>
      {/* Brand */}
      <div className="row" style={{ padding: "0 16px", gap: 10 }}>
        <SnapMark w={22}/>
        <span style={{ fontFamily: "var(--font-mono)", fontSize: 12, letterSpacing: "0.18em", color: "var(--text-0)" }}>
          EXO<span style={{ color: "var(--accent)" }}>·</span>SNAP
        </span>
      </div>

      <div style={{ width: 1, background: "var(--line-1)", margin: "12px 0" }}/>

      {/* Page + status (drag region) */}
      <div className="row" style={{ flex: 1, padding: "0 18px", gap: 14 }}>
        <span style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-3)", letterSpacing: "0.14em" }}>01 · RECORD</span>
        <span style={{ color: "var(--line-3)" }}>•</span>
        {recording ? (
          <span className="pill live">● REC {fmt(elapsed)}</span>
        ) : (
          <span className="pill ok">READY</span>
        )}
        <span style={{ fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--text-2)" }}>DISPLAY1 · 2560×1440 · 60 fps · AV1</span>
      </div>

      {/* Runtime micro-status */}
      <div className="row" style={{ gap: 14, padding: "0 18px", borderLeft: "1px solid var(--line-1)", fontFamily: "var(--font-mono)", fontSize: 11, color: "var(--text-2)" }}>
        <span>CPU <b style={{ color: "var(--text-0)", fontWeight: 500 }}>8.2%</b></span>
        <span>GPU <b style={{ color: "var(--text-0)", fontWeight: 500 }}>14.4%</b></span>
        <span>RAM <b style={{ color: "var(--text-0)", fontWeight: 500 }}>612M</b></span>
      </div>

      <WinCtrls height={56}/>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────
// Variant C — Minimal chrome
// 40 px tall. Mark only on the left (no wordmark — saves vertical space).
// A tiny dot indicates state. Page title moves entirely into the content
// area below. The quietest of the three; feels premium and unobtrusive.
// ─────────────────────────────────────────────────────────────
function ChromeC({ recording = false }) {
  return (
    <div style={{
      height: 40,
      background: "#14130f",
      borderBottom: "1px solid var(--line-1)",
      display: "flex", alignItems: "stretch", flexShrink: 0,
    }}>
      <div className="row" style={{ padding: "0 14px", gap: 10 }}>
        <SnapMark w={18}/>
        <span style={{ fontFamily: "var(--font-mono)", fontSize: 11, letterSpacing: "0.16em", color: "var(--text-2)" }}>EXOSNAP</span>
        <span style={{
          width: 6, height: 6, borderRadius: "50%",
          background: recording ? "var(--accent)" : "var(--ok)",
          marginLeft: 4,
          animation: recording ? "blink 1.4s infinite" : "none",
        }}/>
      </div>

      {/* drag region — empty */}
      <div style={{ flex: 1 }}/>

      {/* one tiny meta */}
      <div className="row" style={{ padding: "0 14px", fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-3)", letterSpacing: "0.06em" }}>
        <span>v0.4.2 · NVENC AV1</span>
      </div>

      <WinCtrls height={40}/>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────
// Compact "Record" preview content — shared across the 3 variants so the
// titlebar comparison is apples-to-apples.
// ─────────────────────────────────────────────────────────────
function RecordBodyCompact({ showInternalTitle = true, recording = false, elapsed = 0 }) {
  const fmt = (s) => {
    const h = Math.floor(s / 3600).toString().padStart(2, "0");
    const m = Math.floor((s % 3600) / 60).toString().padStart(2, "0");
    const ss = (s % 60).toString().padStart(2, "0");
    return `${h}:${m}:${ss}`;
  };
  return (
    <div className="win-body" style={{ flex: 1, minHeight: 0 }}>
      <Sidebar active="record" status={recording ? "live" : "ready"}/>
      <div className="content">
        {showInternalTitle && (
          <ContentHead
            crumb="01 · RECORD"
            title="Record"
            sub="Operational view — target, readiness, and live runtime."
          />
        )}
        <div className="content-body" style={{ padding: showInternalTitle ? "18px 24px 24px" : "20px 24px 24px" }}>

          {/* preview + control rail */}
          <div style={{ display: "grid", gridTemplateColumns: "1fr 240px", gap: 14, marginBottom: 14 }}>
            <div className="preview" style={{ aspectRatio: "16/9", minHeight: 220 }}>
              <span className="corners"><span/><span/><span/><span/></span>
              <div style={{ position: "absolute", top: 10, left: 14, display: "flex", gap: 8, alignItems: "center" }}>
                {recording
                  ? <Pill kind="live">● REC {fmt(elapsed)}</Pill>
                  : <Pill kind="ok">READY</Pill>}
                <span className="mono" style={{ fontSize: 10.5, color: "var(--text-2)" }}>DISPLAY1 · 2560×1440 · 60 fps</span>
              </div>
              <div className="crosshair">
                ◇ PREVIEW SURFACE<br/>
                <span className="res">{recording ? "CAPTURING" : "IDLE · DDA TEX SHARED"}</span>
              </div>
              {recording && <div style={{ position: "absolute", inset: 0, background: "linear-gradient(180deg, transparent 50%, rgba(241,180,0,0.04) 50%)", backgroundSize: "100% 4px", mixBlendMode: "screen", pointerEvents: "none" }}/>}
              <div style={{ position: "absolute", bottom: 10, right: 14, fontFamily: "var(--font-mono)", fontSize: 10, color: "var(--accent)", letterSpacing: "0.1em" }}>AV1 · CQ 24</div>
            </div>

            <div className="col" style={{ gap: 10 }}>
              <div className="panel panel-pad col" style={{ gap: 8 }}>
                <div className="row between">
                  <span style={{ fontFamily: "var(--font-mono)", fontSize: 10, color: "var(--text-2)", letterSpacing: "0.14em", textTransform: "uppercase" }}>
                    {recording ? "Recording" : "Ready"}
                  </span>
                  <span className="btn kbd">ALT+F9</span>
                </div>
                {recording ? (
                  <button className="btn record stop">■ Stop</button>
                ) : (
                  <button className="btn record"><Icon.Record/> Start</button>
                )}
                <div style={{ fontFamily: "var(--font-mono)", fontSize: 22, color: recording ? "var(--accent)" : "var(--text-1)", textAlign: "center", letterSpacing: "0.04em", fontVariantNumeric: "tabular-nums" }}>
                  {fmt(elapsed)}
                </div>
              </div>
            </div>
          </div>

          <div className="section">
            <SectionHead meta="DISPLAY1 · 2560×1440 · 60 fps">Capture target</SectionHead>
            <div style={{ display: "grid", gridTemplateColumns: "repeat(3, 1fr)", gap: 8 }}>
              {[
                { id: "monitor", label: "Monitor", sub: "DISPLAY1 · DDA",     sel: true },
                { id: "window",  label: "Window",  sub: "Game.exe · hook",    sel: false },
                { id: "region",  label: "Region",  sub: "1920×1080 · custom", sel: false },
              ].map(opt => (
                <div key={opt.id} className="panel" style={{
                  padding: "10px 14px",
                  borderColor: opt.sel ? "var(--accent)" : "var(--line-1)",
                  background: opt.sel ? "var(--accent-soft)" : "var(--bg-2)",
                }}>
                  <div className="row between" style={{ marginBottom: 2 }}>
                    <span style={{ fontWeight: 500, color: "var(--text-0)" }}>{opt.label}</span>
                    <span style={{ color: opt.sel ? "var(--accent)" : "var(--text-3)", fontFamily: "var(--font-mono)", fontSize: 10 }}>
                      {opt.sel ? "● ACTIVE" : "○"}
                    </span>
                  </div>
                  <div style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)" }}>{opt.sub}</div>
                </div>
              ))}
            </div>
          </div>

          <div className="section">
            <SectionHead meta="ALL CLEAR">Readiness</SectionHead>
            <div className="panel">
              {[
                { k: "NVENC AV1", ok: true, d: "Ada · driver 555.85" },
                { k: "Display capture", ok: true, d: "DDA · DISPLAY1" },
                { k: "Output destination", ok: true, d: "D:\\Captures · 412 GB" },
              ].map((c, i) => (
                <div key={c.k} className="row between" style={{ padding: "8px 14px", borderTop: i === 0 ? "none" : "1px solid var(--line-1)" }}>
                  <div className="row" style={{ gap: 10 }}>
                    <span style={{ color: "var(--ok)", fontFamily: "var(--font-mono)", width: 12 }}>✓</span>
                    <span style={{ fontSize: 12, color: "var(--text-0)" }}>{c.k}</span>
                  </div>
                  <span style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)" }}>{c.d}</span>
                </div>
              ))}
            </div>
          </div>

        </div>
      </div>
    </div>
  );
}

// Variant frames
function VariantAFrame() {
  return (
    <div className="win">
      <ChromeA/>
      <RecordBodyCompact showInternalTitle={true}/>
    </div>
  );
}

function VariantBFrame() {
  const [elapsed, setElapsed] = React.useState(42);
  const [rec, setRec] = React.useState(true);
  React.useEffect(() => {
    if (!rec) return;
    const t = setInterval(() => setElapsed(e => e + 1), 1000);
    return () => clearInterval(t);
  }, [rec]);
  return (
    <div className="win">
      <ChromeB recording={rec} elapsed={elapsed}/>
      <RecordBodyCompact showInternalTitle={false} recording={rec} elapsed={elapsed}/>
    </div>
  );
}

function VariantCFrame() {
  return (
    <div className="win">
      <ChromeC recording={false}/>
      <RecordBodyCompact showInternalTitle={true}/>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────
// Comparison strip — three chromes stacked on their own for direct compare
// ─────────────────────────────────────────────────────────────
function ChromeComparison() {
  return (
    <div className="dir-card" style={{
      background: "var(--bg-0)", padding: 28, gap: 18,
      "--card-fg": "var(--text-0)", "--card-muted": "var(--text-2)",
    }}>
      <div className="dir-head">
        <div className="num">Part B · Integrated chrome</div>
        <h3 style={{ fontSize: 26 }}>Three chrome variants — direct comparison</h3>
        <div className="dir-desc" style={{ maxWidth: 580 }}>
          Each variant unifies the OS window titlebar with the app's top bar — one continuous band carries brand, drag region, page context, state, and the Windows controls. No more stacked chrome.
        </div>
      </div>

      <div className="col" style={{ gap: 18 }}>
        <div>
          <div className="row" style={{ marginBottom: 6 }}>
            <span style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.14em" }}>VARIANT A · BRAND-LED · 64 px</span>
            <span style={{ flex: 1, height: 1, background: "var(--line-1)", marginLeft: 12 }}/>
            <span style={{ fontFamily: "var(--font-mono)", fontSize: 10, color: "var(--text-3)" }}>strongest brand presence</span>
          </div>
          <div style={{ background: "var(--bg-0)", border: "1px solid var(--line-1)", borderRadius: 4, overflow: "hidden" }}>
            <ChromeA/>
          </div>
        </div>

        <div>
          <div className="row" style={{ marginBottom: 6 }}>
            <span style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.14em" }}>VARIANT B · OPERATIONAL · 56 px ⭐ RECOMMENDED</span>
            <span style={{ flex: 1, height: 1, background: "var(--line-1)", marginLeft: 12 }}/>
            <span style={{ fontFamily: "var(--font-mono)", fontSize: 10, color: "var(--accent)" }}>state-aware accent rail when live</span>
          </div>
          <div style={{ background: "var(--bg-0)", border: "1px solid var(--accent-line)", borderRadius: 4, overflow: "hidden" }}>
            <ChromeB recording={true} elapsed={42}/>
          </div>
          <div style={{ background: "var(--bg-0)", border: "1px solid var(--line-1)", borderRadius: 4, overflow: "hidden", marginTop: 6 }}>
            <ChromeB recording={false}/>
          </div>
        </div>

        <div>
          <div className="row" style={{ marginBottom: 6 }}>
            <span style={{ fontFamily: "var(--font-mono)", fontSize: 10.5, color: "var(--text-2)", letterSpacing: "0.14em" }}>VARIANT C · MINIMAL · 40 px</span>
            <span style={{ flex: 1, height: 1, background: "var(--line-1)", marginLeft: 12 }}/>
            <span style={{ fontFamily: "var(--font-mono)", fontSize: 10, color: "var(--text-3)" }}>quietest, most premium-feel</span>
          </div>
          <div style={{ background: "var(--bg-0)", border: "1px solid var(--line-1)", borderRadius: 4, overflow: "hidden" }}>
            <ChromeC/>
          </div>
        </div>
      </div>

      {/* Information / interaction considerations */}
      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr 1fr", gap: 14 }}>
        {[
          { lbl: "DRAG REGION", body: "Everything in the bar that is NOT the brand block, status pills, or window controls. Implement in Qt with setAttribute(Qt::WA_NoSystemBackground) on the chrome widget + mouse handlers." },
          { lbl: "PROFILE STATE", body: "Variant A places 'Default · unsaved' in the chrome (always visible). Variant B+C keep it inside the page next to the relevant screen — keeps the chrome scannable." },
          { lbl: "CPU / GPU / RAM", body: "Variant B promotes this to the chrome — every screen benefits. Variant A and C keep it as page-local meta on Record only." },
          { lbl: "PAGE INDEX (01–08)", body: "Lives in the chrome breadcrumb (A and B). The sidebar still shows the full numbered nav. Redundancy is fine — repeating the section number is wayfinding, not noise." },
          { lbl: "HELP / SETTINGS / ABOUT", body: "None of the three variants put these in the chrome. They belong in a context menu off the brand mark, or in the Advanced page. The chrome stays operational." },
          { lbl: "QT FEASIBILITY", body: "All three are drawable with QWidget + paintEvent. No transparency or backdrop-blur required. Variant C only needs careful pixel alignment with the Windows control glyphs on HiDPI scales." },
        ].map(c => (
          <div key={c.lbl} className="panel panel-pad">
            <div className="lbl" style={{ fontFamily: "var(--font-mono)", fontSize: 10, letterSpacing: "0.14em", color: "var(--text-3)", marginBottom: 6 }}>{c.lbl}</div>
            <div style={{ fontSize: 11.5, color: "var(--text-1)", lineHeight: 1.55 }}>{c.body}</div>
          </div>
        ))}
      </div>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────
// Final recommendation card
// ─────────────────────────────────────────────────────────────
function FinalLock() {
  return (
    <div className="dir-card" style={{
      background: "var(--bg-0)", padding: 32, gap: 22,
      "--card-fg": "var(--text-0)", "--card-muted": "var(--text-2)",
    }}>
      <div className="dir-head">
        <div className="num" style={{ color: "var(--accent)" }}>● PART C · IDENTITY LOCK</div>
        <h3 style={{ fontSize: 28 }}>Snap Moment + Operational Chrome</h3>
        <div className="dir-desc" style={{ maxWidth: 600 }}>
          The locked combination for ExoSnap going into the full screen pass.
        </div>
      </div>

      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 22 }}>

        <div className="panel panel-pad col" style={{ gap: 14, padding: 22 }}>
          <div className="row between">
            <span className="lbl" style={{ fontFamily: "var(--font-mono)", fontSize: 10, letterSpacing: "0.14em", color: "var(--text-2)" }}>LOGO · LOCKED</span>
            <span className="pill live">● PRIMARY</span>
          </div>
          <div className="row" style={{ gap: 16, padding: "12px 0", justifyContent: "center", background: "var(--bg-2)", border: "1px solid var(--line-1)", borderRadius: 4 }}>
            <SnapMark w={56}/>
            <div className="col" style={{ gap: 2 }}>
              <span style={{ fontFamily: "var(--font-mono)", fontSize: 22, letterSpacing: "0.18em", color: "var(--text-0)" }}>
                EXO<span style={{ color: "var(--accent)" }}>·</span>SNAP
              </span>
              <span style={{ fontFamily: "var(--font-mono)", fontSize: 10, letterSpacing: "0.22em", color: "var(--text-3)" }}>SNAP MOMENT MARK</span>
            </div>
          </div>
          <div style={{ fontSize: 12, color: "var(--text-1)", lineHeight: 1.6 }}>
            <b style={{ color: "var(--text-0)" }}>V3 — Snap Moment.</b> Two opposing brackets with deliberately unequal arm lengths plus a dot held in tension between them. Distinctive silhouette, survives 16 px, ownable geometry.
          </div>
        </div>

        <div className="panel panel-pad col" style={{ gap: 14, padding: 22 }}>
          <div className="row between">
            <span className="lbl" style={{ fontFamily: "var(--font-mono)", fontSize: 10, letterSpacing: "0.14em", color: "var(--text-2)" }}>CHROME · LOCKED</span>
            <span className="pill live">● PRIMARY</span>
          </div>
          <div style={{ background: "var(--bg-2)", border: "1px solid var(--line-1)", borderRadius: 4, overflow: "hidden" }}>
            <ChromeB recording={true} elapsed={42}/>
          </div>
          <div style={{ fontSize: 12, color: "var(--text-1)", lineHeight: 1.6 }}>
            <b style={{ color: "var(--text-0)" }}>Variant B — Operational.</b> 56 px tall. Brand on the left, screen context + state pill in the middle, CPU/GPU/RAM mono readouts on the right, then the Windows controls. The bottom border becomes amber when recording — the whole window signals state.
          </div>
        </div>
      </div>

      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "16px 28px" }}>
        <div className="col" style={{ gap: 8 }}>
          <div className="lbl" style={{ fontFamily: "var(--font-mono)", fontSize: 10, letterSpacing: "0.14em", color: "var(--text-2)" }}>WHY THEY WORK TOGETHER</div>
          <ul style={{ margin: 0, paddingLeft: 16, fontSize: 12, color: "var(--text-1)", lineHeight: 1.65 }}>
            <li>The mark's unequal arms <i>echo</i> the chrome's asymmetric composition — brand-left, context-center, system-right. They share a geometric language.</li>
            <li>The chrome's amber state-rail and the mark's amber dot are the only two amber surfaces visible during normal idle — when the user looks at the window, they see the brand and the readiness verdict in one glance.</li>
            <li>Operational chrome surfaces the exact context that the Snap Moment mark implies — capture state, in real time, with no decoration.</li>
            <li>Both choices stay defensibly minimal — nothing here demands a transparent acrylic backdrop or a custom font that Qt would struggle with.</li>
          </ul>
        </div>

        <div className="col" style={{ gap: 8 }}>
          <div className="lbl" style={{ fontFamily: "var(--font-mono)", fontSize: 10, letterSpacing: "0.14em", color: "var(--text-2)" }}>RISKS TO WATCH</div>
          <ul style={{ margin: 0, paddingLeft: 16, fontSize: 12, color: "var(--text-1)", lineHeight: 1.65 }}>
            <li>The mark's asymmetry depends on stroke weight balance — wrong weight at 16 px makes it read as "broken." Pixel-snapped 16 px and 24 px variants are mandatory.</li>
            <li>Variant B's CPU/GPU/RAM readout must update at a calm cadence (every 1 s, not every frame) or it becomes visually noisy.</li>
            <li>The amber state-rail under recording is potent — resist using it for warnings or other states. It must mean exactly one thing: "ExoSnap is recording right now."</li>
            <li>Window controls on Windows 11 expect specific hit-region behavior (snap layouts, hover preview). The drag region must NOT cover the maximize button — implement WM_NCHITTEST correctly.</li>
          </ul>
        </div>
      </div>
    </div>
  );
}

Object.assign(window, {
  SnapMark, ChromeA, ChromeB, ChromeC,
  VariantAFrame, VariantBFrame, VariantCFrame,
  ChromeComparison, FinalLock, RecordBodyCompact,
});

/* ExoSnap Console — shared chrome + icons */

// Tiny icon set — stroke 1.5, minimal
const Icon = {
  Record: (p) => (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" {...p}>
      <rect x="1.5" y="2.5" width="13" height="11" rx="1"/>
      <circle cx="8" cy="8" r="2.5" fill="currentColor" stroke="none"/>
    </svg>
  ),
  Video: (p) => (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" {...p}>
      <rect x="1.5" y="4" width="9" height="8" rx="1"/>
      <path d="M10.5 7l4-2v6l-4-2z"/>
    </svg>
  ),
  Audio: (p) => (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" {...p}>
      <path d="M2 6v4M5 4v8M8 2v12M11 5v6M14 7v2"/>
    </svg>
  ),
  Output: (p) => (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" {...p}>
      <path d="M2 11v2a1 1 0 001 1h10a1 1 0 001-1v-2"/>
      <path d="M8 2v8M5 7l3 3 3-3"/>
    </svg>
  ),
  Keys: (p) => (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" {...p}>
      <rect x="1.5" y="4.5" width="13" height="7" rx="1"/>
      <path d="M4 7v0M7 7v0M10 7v0M13 7v0M4 9h8"/>
    </svg>
  ),
  Diag: (p) => (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" {...p}>
      <path d="M1.5 8h2l1.5-4 3 8 1.5-4h5"/>
    </svg>
  ),
  Logs: (p) => (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" {...p}>
      <path d="M3 2h7l3 3v9a1 1 0 01-1 1H3a1 1 0 01-1-1V3a1 1 0 011-1z"/>
      <path d="M5 8h6M5 11h6M5 5h3"/>
    </svg>
  ),
  Advanced: (p) => (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" {...p}>
      <circle cx="8" cy="8" r="2"/>
      <path d="M8 1v2M8 13v2M15 8h-2M3 8H1M12.95 3.05l-1.4 1.4M4.45 11.55l-1.4 1.4M12.95 12.95l-1.4-1.4M4.45 4.45l-1.4-1.4"/>
    </svg>
  ),
  Chevron: (p) => (
    <svg width="10" height="10" viewBox="0 0 10 10" fill="none" stroke="currentColor" strokeWidth="1.5" {...p}>
      <path d="M3 1l4 4-4 4"/>
    </svg>
  ),
  Grip: (p) => (
    <svg width="10" height="14" viewBox="0 0 10 14" fill="currentColor" {...p}>
      <circle cx="3" cy="3" r="1"/><circle cx="7" cy="3" r="1"/>
      <circle cx="3" cy="7" r="1"/><circle cx="7" cy="7" r="1"/>
      <circle cx="3" cy="11" r="1"/><circle cx="7" cy="11" r="1"/>
    </svg>
  ),
};

// Brand mark — "frame-cut": a refined corner-bracket pair + slash
function BrandMark({ size = 22, color = "var(--accent)", inner = "var(--text-0)" }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      {/* top-left bracket */}
      <path d="M3 9V4a1 1 0 011-1h5" stroke={color} strokeWidth="1.6" strokeLinecap="square"/>
      {/* bottom-right bracket */}
      <path d="M21 15v5a1 1 0 01-1 1h-5" stroke={color} strokeWidth="1.6" strokeLinecap="square"/>
      {/* signal slash */}
      <path d="M16 6L8 18" stroke={inner} strokeWidth="1.6" strokeLinecap="square"/>
      <circle cx="16" cy="6" r="1.4" fill={color}/>
    </svg>
  );
}

const NAV = [
  { id: "record",   label: "Record",      icon: Icon.Record,   key: "01" },
  { id: "video",    label: "Video",       icon: Icon.Video,    key: "02" },
  { id: "audio",    label: "Audio",       icon: Icon.Audio,    key: "03" },
  { id: "output",   label: "Output",      icon: Icon.Output,   key: "04" },
  { id: "hotkeys",  label: "Hotkeys",     icon: Icon.Keys,     key: "05" },
  { id: "diag",     label: "Diagnostics", icon: Icon.Diag,     key: "06" },
  { id: "logs",     label: "Logs",        icon: Icon.Logs,     key: "07" },
  { id: "adv",      label: "Advanced",    icon: Icon.Advanced, key: "08" },
];

function Titlebar({ subtitle }) {
  return (
    <div className="win-titlebar">
      <BrandMark size={14}/>
      <span className="title">EXOSNAP{subtitle ? ` — ${subtitle}` : ""}</span>
      <span className="winbtn">
        <svg width="10" height="10" viewBox="0 0 10 10"><path d="M1 5h8" stroke="currentColor" strokeWidth="1"/></svg>
      </span>
      <span className="winbtn">
        <svg width="10" height="10" viewBox="0 0 10 10"><rect x="1.5" y="1.5" width="7" height="7" fill="none" stroke="currentColor" strokeWidth="1"/></svg>
      </span>
      <span className="winbtn close">
        <svg width="10" height="10" viewBox="0 0 10 10"><path d="M1.5 1.5l7 7M8.5 1.5l-7 7" stroke="currentColor" strokeWidth="1"/></svg>
      </span>
    </div>
  );
}

function Sidebar({ active, status = "ready", fps = "60.0", gpu = "RTX 4070" }) {
  return (
    <div className="sidebar">
      <div className="brand">
        <div className="brand-mark"><BrandMark size={22}/></div>
        <div className="brand-name">EXO<span className="accent">·</span>SNAP</div>
      </div>
      <ul className="nav">
        {NAV.map(n => {
          const IconC = n.icon;
          return (
            <li key={n.id} className={active === n.id ? "active" : ""}>
              <IconC/>
              <span>{n.label}</span>
              <span className="key">{n.key}</span>
            </li>
          );
        })}
      </ul>
      <div className="sidebar-foot">
        <div className="stat"><span>STATUS</span><b className={status === "live" ? "amber" : "ok"}>{status === "live" ? "● REC" : "READY"}</b></div>
        <div className="stat"><span>ENCODER</span><b>NVENC</b></div>
        <div className="stat"><span>GPU</span><b>{gpu}</b></div>
        <div className="stat"><span>VERSION</span><b>0.4.2</b></div>
      </div>
    </div>
  );
}

function ContentHead({ crumb, title, sub, right }) {
  return (
    <div className="content-head">
      <div>
        <div className="crumb">{crumb}</div>
        <h1>{title}</h1>
        {sub && <div className="sub">{sub}</div>}
      </div>
      <div className="head-right">{right}</div>
    </div>
  );
}

function SectionHead({ children, meta }) {
  return (
    <div className="section-head">
      <h2>{children}</h2>
      <div className="rule"/>
      {meta && <span className="meta">{meta}</span>}
    </div>
  );
}

function Radio({ checked, onClick, children, tag }) {
  return (
    <div className={"radio" + (checked ? " checked" : "")} onClick={onClick}>
      <span className="dot"/>
      <span className="label">{children}{tag && <span className="tag">{tag}</span>}</span>
    </div>
  );
}
function Check({ checked, onClick, children, tag }) {
  return (
    <div className={"check" + (checked ? " checked" : "")} onClick={onClick}>
      <span className="box"/>
      <span className="label">{children}{tag && <span className="tag">{tag}</span>}</span>
    </div>
  );
}
function Toggle({ on, onClick }) {
  return <div className={"toggle" + (on ? " on" : "")} onClick={onClick}/>;
}
function Pill({ kind = "", dot = true, children }) {
  return <span className={"pill " + kind}>{dot && <span className="dot"/>}{children}</span>;
}

// Audio meter — segments representing level
function Meter({ level = 0.4, segs = 18 }) {
  const lit = Math.floor(level * segs);
  return (
    <div className="meter">
      {[...Array(segs)].map((_, i) => {
        const on = i < lit;
        let cls = "seg";
        if (on) {
          cls += " on";
          if (i >= segs * 0.6 && i < segs * 0.85) cls += " mid";
          if (i >= segs * 0.85) cls += " hot";
        }
        return <span key={i} className={cls}/>;
      })}
    </div>
  );
}

Object.assign(window, {
  Icon, BrandMark, Titlebar, Sidebar, ContentHead, SectionHead,
  Radio, Check, Toggle, Pill, Meter, NAV,
});

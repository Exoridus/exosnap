# ADR 0069: Transactional Windows environment orchestration, outside the product

- Status: Accepted
- Date: 2026-08-17
- Supersedes: nothing
- Related: ADR 0066 (Live Verify control channel), ADR 0067 (shared control layer), ADR 0033 (present diagnostics)

## Context

The v0.9 release gate is largely a list of things a person does to Windows and then
watches ExoSnap cope with: set a display to 44.1 kHz, turn HDR on, drop to 60 Hz,
unplug an audio interface mid-recording. `docs/release-checklist.md` §7 is 60-odd such
items, and every one of them was manual — not because a human judgement was required,
but because nothing could set the machine up.

Two ways to fix that, and the choice between them is the whole decision.

The tempting one is to let ExoSnap do it. The control channel already exists, it is
already the thing a runner talks to, and adding `windows.setHdr` next to
`record.start` is a small diff. It is also how a recording application becomes a
Windows administration interface. Every command on that channel is reachable by
anything that can open a named pipe with the run id; the channel's whole security
argument is that its allowlist contains observations and the product's own intents and
nothing else. `registry.set` on that list ends the argument.

The other is to keep environment mutation out of the product entirely, in
infrastructure that ships to nobody.

## Decision

**Product automation and environment automation are separate trust domains.**

ExoSnap's control channel never grows a generic Windows-control surface. Specifically
absent, permanently: `windows.setHdr`, `windows.setRefreshRate`,
`windows.setDefaultAudio`, `windows.setAudioFormat`, `registry.set`, `shell.execute`.

Environment mutation lives in `tools/envctl` — `envctl_core` (pure, host-agnostic,
unit-tested on CI) plus a Win32 provider and a small `exosnap-envctl` executable. It is
test-only: never installed, never linked into `exosnap.exe` or `exosnap-updater.exe`,
no service, no daemon, no autostart. `scripts/lib/EnvironmentOrchestrator.psm1` is the
PowerShell half the release runner uses.

Four kinds of truth, and each is established by whoever actually owns it:

| Kind | Owner | How it is established |
|---|---|---|
| product | ExoSnap | the control channel's typed surfaces — automated, always |
| environment | Windows | envctl transactions, where a documented restorable mechanism exists |
| physical | the operator | they act; the runner verifies the consequence itself |
| secure | UAC | they click; the runner observes before and after |
| visual desktop | the operator's eyes | the runner prepares the state; the person judges it |

### Capability classification comes before implementation

No property is mutated before its class is decided:

```
ENV_READ             reliably readable; nothing changes it during a run
ENV_MUTATE_SAFE      documented, supported mechanism; snapshot- and restore-safe
ENV_MUTATE_TESTONLY  technically possible, not a regular product mechanism; needs a reason
ENV_HUMAN            the user must operate Windows' or a vendor's UI
PHYSICAL             a physical act — unplug a cable
SECURE               a Windows secure surface, above all UAC
UNAVAILABLE          not testable on this system
```

The class names **who may change the property during a run**, not merely whether it can
be read. A readable-but-operator-only property is `ENV_HUMAN`, not `ENV_READ`.

**Undocumented mechanisms are never a default path.** Not undocumented `IPolicyConfig`,
not registry writes, not UI macro automation of Windows Settings, not `SendKeys`, not
pixel clicking, not private vendor APIs. A scenario only automatable that way is
`ENV_HUMAN`, and the runner asks. Test convenience does not outrank system correctness:
the cost of the shortcut is an unsupported mechanism sitting on the release path, and
the benefit is that nobody has to click something once.

The consequence is a small mutable set. Of 21 classified properties, **two** are
`ENV_MUTATE_SAFE`: display HDR (`DisplayConfigSetDeviceInfo` with `SET_HDR_STATE`,
falling back to `SET_ADVANCED_COLOR_STATE`) and display refresh rate
(`ChangeDisplaySettingsExW` over a whole snapshotted `DEVMODE`). Nine are `ENV_HUMAN` —
including the audio endpoint format and the default-endpoint roles, whose only
mechanism is undocumented. Eight are `ENV_READ`, two `PHYSICAL`.

Two classifications are worth naming because guessing them would have been wrong on
this very machine:

- **ACM is not the HDR bit.** `display.main-hdr` here reports `hdr=off`, `acm=on`,
  `advanced-color-mode=wcg`. Inferring ACM from HDR would have reported "off".
- **The audio endpoint's shared-mode format is not the engine mix format.** Both are
  reported, separately and labelled, because they answer different questions and a
  conflation of the two is invisible in a passing test.

### Every mutation is a transaction

```
snapshot exact original
  -> persist recovery journal        (nothing is mutated before this is on disk)
  -> validate desired
  -> apply minimal delta             (a property already at the desired value is skipped)
  -> read back, independently
  -> verify actual == desired
  -> [ run the test ]
  -> restore exact original
  -> read back
  -> verify actual == original
  -> close
```

**A setter returning success is never evidence.** This is not a theoretical rule. The
first real transaction driven on this machine asked for 60 Hz;
`ChangeDisplaySettingsExW` accepted it and `EnumDisplaySettingsExW` then reported
**59** — the 59.94 Hz truncation. The transaction refused, rolled back, and verified
the display at its original 144 Hz. A tolerance would have called that a pass.

**Restore means "put back what this machine actually had"**, never "set the defaults".
A machine that had HDR on gets HDR on again.

**The journal is written before the first mutation**, atomically, and `applied` is the
outstanding debt rather than a history log: entries are popped as each is restored *and
verified*, so a kill at any instant leaves exactly what still has to be undone. It
carries a stable non-identifying machine hash, never a hostname or user name.

Failure states are distinct because they need distinct responses:

| State | Meaning |
|---|---|
| `Restored` | put back and verified |
| `RestoreFailed` | the restore setter succeeded and the read-back disagreed |
| `RestorePendingDeviceUnavailable` | the original device is gone; **no other device was touched** |

The last one is the reason the alias model exists at all.

### Devices are named by stable identifier, never by friendly name

Scenarios declare **aliases** — `display.main-hdr`, `audio.render.44100-test` — and the
alias profile is the only machine-specific file. It maps alias to a stable Windows
identifier: the monitor device path for displays, the MMDevice endpoint id for audio.
Friendly names are labels for human gates and nothing else.

The display key is the **monitor device path**, not the adapter LUID + target id pair.
A LUID is stable only while the adapter stays enumerated in this boot, so a journal
written before a reboot would rebind to the wrong panel. The LUID pair is carried as
display-only detail.

Two failures are reported rather than guessed at: `ambiguous_device` (more than one
device matches — nothing is chosen) and `unbound_alias` (the profile does not bind it —
the message says how). A suite that silently picks one of two matching devices is a
suite that lies about which hardware it exercised.

### Recovery is a gate, not a warning

On runner start, a dirty journal is restored and verified **before** anything else, and
a new mutating scenario is refused until it is. An unreadable journal is never reported
as clean.

What this cannot promise is instant recovery from a power loss or an OS crash. Nothing
in user space can. The guarantee is the persistent journal plus a mandatory recovery
pass — stated that way rather than as an unfalsifiable claim.

An `exosnap-envctl --guard <owner-pid>` mode waits on the owner process and recovers if
it dies unexpectedly. It is convenience on top of the mandatory guarantee, spawned by
the runner, never installed.

### Two verdicts per scenario

The product verdict and the environment-restore verdict are recorded separately. A
scenario can prove the product correct and still leave a display in the wrong mode;
that is release-relevant on its own, so a product `PASS` with a broken restore is a
failure in `junit.xml`.

## Consequences

**What this buys.** Most of release-checklist §7 becomes a runner scenario that
prepares its own environment and puts it back. The remaining manual gates are the ones
that were always genuinely manual: UAC, physical unplug/replug, desktop composition a
person has to look at, and hardware this machine does not have.

**What it costs.** A second executable to build (test-only), a machine-local alias
profile to bind once per desk, and a deliberately small mutable set. Scenarios that
would need an undocumented setter stay human — the audio format matrix in particular is
operator-set and runner-verified rather than fully automated.

**What it refuses.** ExoSnap does not become a Windows administration interface, and no
boundary is crossed with a pixel click, a registry write or a `SendKeys` macro to make
a checklist look greener than it is. A gate that would need one is reported as manual,
which is true, rather than automated, which would not be.

## Verification boundary (honest)

The transaction state machine, the journal, the alias model and the recovery gate are
covered against a fake provider that can lie about success — which is what makes the
interesting failures reachable at all, since no real display can be talked into a
read-back mismatch on demand.

The Win32 **read** path is verified live on this machine. Of the two real setters, the
refresh-rate path has now been driven end to end (apply, read-back, refusal on the
59/60 mismatch, exact restore to 144 Hz, verified). The HDR setter has not yet been run
against real hardware: changing the developer's display state is their call, and no
CTest entry is permitted to do it.

# ADR 0043: FDK-AAC license position (GPL-3.0 compatibility)

## Status

**Superseded by [ADR 0052](0052-native-ffmpeg-aac-encoder-supersedes-0043.md) — 2026-07-19.**
The maintainer reversed this decision: rather than keep FDK-AAC via the
`fdk-aac-free` fork, ExoSnap is migrating the AAC path to FFmpeg's native
(LGPL) AAC-LC encoder — ADR 0043's reserved option (c)-style exit. The decision
below is retained as the historical record; the migration and its deferred
cutover are governed by ADR 0052.

**Accepted — 2026-07-11.** The maintainer chose **option (b): switch to `fdk-aac-free`**
(the LC-only stripped fork), combined with option (a)'s corrected-comment change. The build
now fetches Wim Taymans's stripped fork
(<https://gitlab.freedesktop.org/wtaymans/fdk-aac-stripped>, branch `stripped4`, pinned at
commit `529b87452cd33d45e1d0a5066d20b64f10b38845`, FDK-AAC 2.0.2 base — the same source
Fedora ships as `fdk-aac-free` 2.0.2) instead of upstream `mstorsjo/fdk-aac`, and the
unsupported "compatible with GPL 3.0 per FSF (2017)" comment in
`third_party/CMakeLists.txt` has been replaced with an accurate, sourced position. The
sections below are the research and option analysis as drafted; they are retained unchanged
as the decision record.

## Context

ExoSnap is licensed **GPL-3.0-or-later** (`LICENSE`). It statically links the **Fraunhofer FDK
AAC Codec Library for Android** (`fdk-aac`, upstream `github.com/mstorsjo/fdk-aac`, v2.0.3) into
the shipped `exosnap.exe`. The only comment in the build file that addresses the license
relationship is:

```
# --- FDK-AAC v2.0.3 ---
# Fraunhofer FDK AAC codec. License: compatible with GPL 3.0 per FSF (2017).
```
(`third_party/CMakeLists.txt:89-90`)

This claim is not backed by a citation anywhere in the repository, and — as far as this review
was able to determine — it does not match the position that is actually attributed to the FSF
(see "FSF position" below). `THIRD_PARTY_NOTICES.md` separately notes the FDK-AAC patent caveat
("Patent licenses for AAC encoding/decoding may be required independently from the copyright
license... Patent licensing is the user's responsibility") but does not address the GPL
combination question at all.

**This is not a peripheral dependency.** `FdkAacEncoder` (`libs/recorder_core/src/fdk_aac_encoder.{h,cpp}`)
is the **only** live AAC encoder in the audio pipeline — `audio_thread.cpp` constructs it directly
and nothing else. AAC is not optional in the product: per `docs/product-spec.md` (§4, the
container/codec matrix), **MP4 offers only AAC audio** — Opus, PCM, and FLAC are explicitly
`Prohibited`/rejected for MP4 by the compatibility registry (ADR 0010, ADR 0014), independent of
this license question. AAC is also the audio codec of the built-in **Compatibility** preset
(MP4 + H.264 + AAC). There is no CMake option to omit `fdk-aac` from the build (unlike, e.g.,
`EXOSNAP_WITH_PRESENTMON`) — it is unconditionally fetched, built as a static library, and linked
into every configuration, official and self-built alike.

The legacy `MfAacEncoder` (Media Foundation AAC-LC, `libs/recorder_core/src/mf_aac_encoder.{h,cpp}`)
is still present in the tree — ADR 0038 records that the live pipeline switched from it to
FDK-AAC and that it has been dead code ever since — but **an open PR (#176, "Dead code retires
and every theme proves its QSS tokens resolve") currently proposes deleting it** (`MfAacEncoder`,
its `IAudioEncoder` adapter `MfAacAudioEncoder`, and its dedicated test binary) as unreachable
dead code. That PR is unrelated to licensing; it was not written with this question in mind. If
this ADR leads to reviving the MF path, it would need to be restored from git history after (or
instead of) that PR merges — flagged here so the two efforts do not collide silently.

## Research: what the FDK-AAC license actually says, and how others have treated it

### The license text (primary source)

The full text ships as `NOTICE` in the upstream repository:
<https://raw.githubusercontent.com/mstorsjo/fdk-aac/master/NOTICE> (also mirrored, canonicalized,
under SPDX identifier `FDK-AAC`: <https://spdx.org/licenses/FDK-AAC.html>). It is a BSD-style
license with Fraunhofer-specific additions. The clause at issue is numbered **§3** in the license
body:

> **3. NO PATENT LICENSE**
>
> NO EXPRESS OR IMPLIED LICENSES TO ANY PATENT CLAIMS, including without limitation the patents
> of Fraunhofer, ARE GRANTED BY THIS SOFTWARE LICENSE. Fraunhofer provides no warranty of patent
> non-infringement with respect to this software. You may use this FDK AAC Codec software or
> modifications thereto only for purposes that are authorized by appropriate patent licenses.

Two further clauses matter for the GPL question and are usually discussed alongside §3:

> You may not charge copyright license fees for anyone to use, copy or distribute the FDK AAC
> Codec software or your modifications thereto.
>
> ...you must make available free of charge copies of the complete source code of the FDK AAC
> Codec and your modifications thereto to recipients of copies in binary form.

### Why this collides with GPLv3

GPLv3 §7 enumerates the *only* additional restrictions a licensor may layer onto GPL-covered
code (warranty disclaimers, attribution, non-endorsement, indemnification, etc.). A restriction
outside that enumerated list is a "further restriction" under GPLv3 §10, and the license
explicitly forbids imposing one on a work conveyed under GPLv3. FDK-AAC's §3 patent
non-grant/field-of-use condition, and its no-fee/mandatory-free-source condition, are not among
the GPLv3 §7 permitted exceptions. The practical consequence — echoed identically by FFmpeg,
Debian, and Fedora below — is that a binary combining unmodified FDK-AAC with GPL-licensed code
cannot validly be licensed "as a whole" under GPLv3: the combination is not itself a normal
"System Library"/aggregation exception (GPLv3 §1) because ExoSnap links it in, statically,
on purpose, as a codec the program is specifically designed to use.

### Positions of record

- **FSF.** Per Fedora's own legal review record (Red Hat Bugzilla #1501522, "Review Request:
  fdk-aac-free"): the FDK-AAC license was reviewed and found by the FSF to be "**free, but
  GPL-incompatible**," specifically because of §3. This is a narrower and more precise
  characterization than the CMake comment's "compatible with GPL 3.0 per FSF" — the FSF's actual
  position (as reported through this review) is closer to the *opposite*: the license is
  free software, but a GPL project cannot combine with it and still convey the result under
  GPL terms. No citation in the ExoSnap tree supports the "compatible... per FSF (2017)" wording,
  and this research did not find one either.
  Source: <https://bugzilla.redhat.com/show_bug.cgi?id=1501522>

- **Debian.** `fdk-aac` (the unmodified upstream library) sits in Debian's **non-free**
  archive component, not `main`. The rationale given (see the summary at
  <https://tookmund.com/2024/02/aac-and-debian> and Debian's package tracker,
  <https://tracker.debian.org/pkg/fdk-aac>) combines the GPL-incompatibility question with a
  DFSG concern: the "no charging license fees" clause is read as discrimination against a field
  of endeavor (DFSG #6), independent of the patent clause. Because `fdk-aac` is non-free, `main`
  packages (e.g. PipeWire, for its AAC Bluetooth codec) cannot depend on it.

- **Fedora.** The Fedora Legal wiki page for FDK-AAC (<https://fedoraproject.org/wiki/Licensing/FDK-AAC>)
  records that the plain license was treated as "allowed" until **August 2022**, when it was
  **reclassified as not-allowed** specifically because of the §3 patent disclaimer, as Fedora
  adopted a more defensive posture on patent-encumbered licenses generally.

- **`fdk-aac-free` — the one carve-out both distros lean on.** Fedora has shipped
  `fdk-aac-free` since 2017 (Red Hat Bugzilla #1501522) — not a different license, but a
  **source-stripped fork** (maintained at
  <https://cgit.freedesktop.org/~wtay/fdk-aac/log/?h=fedora>, the `fedora` branch of Wim
  Taymans's tree) that removes the SBR/PS (Spectral Band Replication / Parametric Stereo, i.e.
  HE-AAC) code paths and keeps only plain **LC-AAC** encode/decode. Fedora Legal's argument,
  recorded in the same bugzilla thread, is fork-specific: with the patent-encumbered techniques
  physically removed, §3's patent non-grant has nothing live left to disclaim for what remains,
  so it is "effectively GPL-compatible for this specific implementation" even though the FSF's
  general "free, but GPL-incompatible" finding about the license *text* itself is unchanged. Both
  Red Hat Legal and FESCo approved shipping it on that basis. Debian has an `fdk-aac-free`
  package pending in its NEW queue that has stalled since 2022 (per the same sources) — i.e.
  Debian has not yet reached the same conclusion Fedora did.
  Caveat found during this research and worth stating plainly: `fdk-aac-free` reduces practical
  patent-clause risk for the LC-AAC-only case; it is **not** a universally agreed clean fix, and
  a maintainer choosing it is making the same kind of risk-tolerance call Fedora made, not
  resolving the underlying ambiguity outright.

- **How other GPL/LGPL projects handle it:**
  - **FFmpeg** gates `libfdk_aac` (and other GPL-incompatible optional components) behind
    `--enable-nonfree`, and FFmpeg's own documentation states that a binary built with
    `--enable-nonfree` (together with `--enable-gpl`) is **not redistributable** — build-for-self
    only. This is the "status quo, but honestly labeled and never shipped" pattern.
  - **OBS Studio** ships FDK-AAC support as `obs-libfdk`, a **separate, independently loaded
    plugin module** (`plugins/obs-libfdk/obs-libfdk.c`,
    <https://github.com/obsproject/obs-studio/blob/master/plugins/obs-libfdk/obs-libfdk.c>),
    not compiled into the core `obs-studio` binary. Distro packagers can and do omit the plugin
    entirely (e.g. Arch Linux's `obs-studio` package moved `libfdk-aac` from a hard dependency to
    an optional one — see the Arch packaging change referenced in
    <https://www.mail-archive.com/arch-commits@lists.archlinux.org/msg908620.html>). This is the
    "separate work, mere aggregation, user opts in" pattern — much closer to a defensible GPL
    position than static linking into the same binary, though it still does not resolve whether
    the *plugin itself*, once loaded into the same process and combined with GPL-licensed OBS
    code at runtime, is a "combined work" in the GPL sense; reasonable people differ on this, and
    it is why OBS still treats it as optional rather than declaring the question closed.

### Does `fdk-aac-free` cover what ExoSnap actually needs?

Yes, with no loss of functionality for ExoSnap specifically. `FdkAacEncoder::Init()`
(`libs/recorder_core/src/fdk_aac_encoder.cpp:41`) hardcodes `AACENC_AOT = AOT_AAC_LC` — ExoSnap
has never used HE-AAC/SBR/PS. `KNOWN_LIMITATIONS.md` already documents the shipped format as
"AAC-LC (`AAC` in the UI)." The functionality `fdk-aac-free` strips is functionality ExoSnap does
not call. A swap would plausibly be a `FetchContent` source-URL change plus a licensing-text and
`THIRD_PARTY_NOTICES.md` update, not an encoder rewrite — but see "Remaining limitations" below;
this was not verified by an actual build.

## Options

### (a) Status quo: keep static FDK-AAC, replace the comment with an accurate, sourced position

Keep shipping FDK-AAC exactly as today, but fix the CMake comment and add a documented,
citation-backed position (e.g. "ExoSnap ships FDK-AAC under the risk-tolerance precedent set by
[projects that already do this]; see ADR 0043") instead of the current unsupported "compatible...
per FSF" claim.

- **Consequence:** Does not change ExoSnap's actual legal exposure at all — only its honesty
  about that exposure. Every source above (FSF's reported position, Debian, Fedora pre-2022,
  FFmpeg's own compliance guidance) treats static-linking FDK-AAC into a GPL binary and
  redistributing it as the exact combination to avoid. This is the riskiest option precisely
  because ExoSnap already ships pre-built binaries (portable ZIP + MSI) — it is not a
  build-it-yourself tool where the FFmpeg `--enable-nonfree` "you can build it, you can't ship
  it" carve-out would even apply.

### (b) Switch to `fdk-aac-free`

Point the `FetchContent_Declare(fdk-aac …)` at the Fedora/`wtaymans` `fedora` branch
(<https://cgit.freedesktop.org/~wtay/fdk-aac/log/?h=fedora>) instead of upstream `mstorsjo/fdk-aac`.

- **Consequence:** Adopts the same risk-tolerance position Fedora Legal/FESCo already adopted
  for the identical LC-AAC-only use case, with a citable precedent. Does not fully resolve the
  FSF's general "free, but GPL-incompatible" textual finding (the license clause itself is
  reportedly unchanged; only the practical patent exposure for the stripped subset is argued
  away) — Debian has not accepted this reasoning yet (their `fdk-aac-free` package has stalled in
  NEW since 2022). Technically low-risk for ExoSnap specifically (LC-AAC-only usage, confirmed
  above) but requires validating that the `fedora` branch's CMake/build story is compatible with
  the existing `FetchContent` pattern — not verified here.

### (c) Revert to the Media Foundation AAC encoder as the live path

Media Foundation's AAC-LC MFT is a Windows **operating-system component**, not something ExoSnap
ships or links statically — no third-party redistribution question at all.

- **Consequence:** Removes the FDK-AAC license question entirely for the AAC path. Reintroduces
  a dependency ADR 0038 says the project deliberately moved away from (no stated reason found in
  that ADR or the introducing commit beyond "the live pipeline uses FDK-AAC" as an accomplished
  fact — the original rationale for the FDK switch was not documented in a form this review
  could locate). Requires either landing this decision before PR #176 merges (keeping
  `MfAacEncoder` alive and wiring it back into `audio_thread.cpp`) or resurrecting it from git
  history afterward. Also reintroduces the constraint ADR 0038 flags for Windows N/KN editions:
  MF DLLs are delay-loaded and probed today only because the *webcam* still needs them; if MF
  AAC becomes the live encode path again, AAC recording itself would become unavailable (or need
  its own fallback) on Windows N/KN hosts without the Media Feature Pack — a regression for that
  subset of users that does not exist today.

### (d) Make AAC/FDK-AAC a dynamically-loaded, optional component (OBS's model)

Split FDK-AAC out of the core `exosnap.exe` binary into a separate DLL loaded only when AAC is
actually selected, distinct from the "always statically linked" model today.

- **Consequence:** Moves ExoSnap from "static link, always present" toward the "separate
  work, opt-in" pattern OBS uses for `obs-libfdk` — a meaningfully different (and generally
  considered lower-risk) position than today's static link, though — per OBS's own continued
  treatment of it as optional rather than a settled question — this does not produce a legally
  certain answer either; it changes the shape of the risk, not the underlying license conflict.
  This is also the option with the largest engineering footprint: it would require a plugin/DLL
  boundary that does not exist anywhere else in ExoSnap's architecture today, and AAC is not an
  optional feature from the product's point of view (it is MP4's only audio codec) — making
  ExoSnap's most common export path depend on an optional component would need its own product
  conversation about what happens when the component is absent (no MP4 export at all? fall back
  to MF AAC on Windows only? forced re-encode?). None of that is scoped here.

### (e) Move MP4 to a different audio codec entirely

Not viable without contradicting existing, deliberate product decisions. `docs/product-spec.md`
and ADR 0010/0014 already evaluated and explicitly rejected Opus-in-MP4 — it is
`Prohibited` in the compatibility registry today, on **compatibility grounds** (limited
player/NLE support for Opus-in-ISOBMFF), independent of anything in this ADR. PCM-in-MP4 is
separately deferred (the `ipcm` ISO/IEC 23003-5 sample entry many players/editors reject). FLAC-
in-MP4 is out of scope for 1.0. There is no currently-sanctioned alternative MP4 audio codec in
the product; choosing this option would mean re-litigating ADR 0010/0014's compatibility findings
from scratch, not just the license question this ADR is about.

## Recommendation (advisory only — the maintainer decides)

Given that (1) AAC is not optional (MP4's only audio codec), (2) ExoSnap already ships
pre-built binaries rather than build-from-source-only, (3) ExoSnap's FDK-AAC usage is
LC-AAC-only and therefore squarely inside what `fdk-aac-free` covers, and (4) a citable
precedent already exists for treating that specific combination as acceptable (Fedora
Legal + FESCo, since 2017): **option (b), switching to `fdk-aac-free`, looks like the best
ratio of risk reduction to engineering cost**, with the accurate-comment change from option (a)
applied regardless of which option is chosen (the current "compatible... per FSF" wording should
not survive this ADR unchanged either way). Option (c) is worth keeping in reserve specifically
*because* `MfAacEncoder` still exists in the tree today — reintroducing it later remains cheap as
long as PR #176 is coordinated with whatever this ADR decides, but it reopens the Windows N/KN
question ADR 0038 closed for the webcam. Option (d) is the most legally conservative shape but is
disproportionate engineering relative to (b) unless the maintainer specifically wants to avoid
even the `fdk-aac-free` fork-specific argument.

This is a recommendation, not a decision. The maintainer should weigh their own risk tolerance —
this ADR's job is to make sure that choice is made knowingly, with sources, rather than resting on
an uncited comment.

## Open questions for the maintainer

- Is the project willing to rely on the same fork-specific "patents don't apply to what's left"
  argument Fedora Legal used, or does that feel too close to legal self-certification for a
  volunteer project?
- Should this decision block the next release, or ship with option (a)'s corrected-comment
  interim step while (b) is evaluated for real (verifying the `fedora` branch actually builds
  against ExoSnap's existing `FetchContent` + CMake pattern, which this review did not test)?
- Does PR #176 (`MfAacEncoder` removal) need to be held until this ADR resolves, in case option
  (c) is chosen?

## Consequences

The decision landed together with this ADR:

- `third_party/CMakeLists.txt` fetches
  `https://gitlab.freedesktop.org/wtaymans/fdk-aac-stripped.git` at commit
  `529b87452cd33d45e1d0a5066d20b64f10b38845` (branch `stripped4`, FDK-AAC 2.0.2 base — the
  fdk-aac-free 2.0.2 Fedora ships). This is one upstream patch release behind the
  previously pinned `mstorsjo/fdk-aac` v2.0.3; the 2.0.3 delta is decoder-side fixes with
  no encoder API change. The fork's newer `stripped5` branch (2.0.3 base) was evaluated
  first but its CMake source list is stale (it references decoder files that its
  USAC-removal commit deleted; Fedora builds it via autotools) and fails to configure —
  worth re-evaluating if upstream repairs it. The fork ships the same upstream-style
  `CMakeLists.txt` and the same `fdk-aac` target name, so the FetchContent mechanics, the
  static-link setup, and `recorder_core`'s link line are unchanged.
- The incorrect "compatible with GPL 3.0 per FSF (2017)" comment is gone; the replacement
  states the actual position (FSF: free but GPL-incompatible in general; Fedora Legal
  precedent for the LC-only stripped fork) with the Bugzilla citation.
- `FdkAacEncoder` is untouched: every API it uses (`aacEncOpen`, `aacEncoder_SetParam` with
  `AACENC_AOT`/`AACENC_SAMPLERATE`/`AACENC_CHANNELMODE`/`AACENC_BITRATE`/`AACENC_TRANSMUX`/
  `AACENC_AFTERBURNER`, `aacEncEncode`, `aacEncInfo`, `aacEncClose`) exists identically in
  the fork; the afterburner is part of the LC quantization loop
  (`libAACenc/src/adj_thr.cpp`) and survives the stripping.
- `THIRD_PARTY_NOTICES.md` and the staged `licenses/fdk-aac.txt` now describe the fork
  (the `NOTICE` license text is byte-identical between upstream and the fork).
- The shipped binary loses nothing: HE-AAC/SBR/PS were never reachable through ExoSnap's
  encoder configuration.

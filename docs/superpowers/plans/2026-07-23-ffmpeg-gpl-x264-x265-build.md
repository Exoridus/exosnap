# FFmpeg GPL Build: libx264 + libx265 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the vendored FFmpeg build (`Exoridus/exosnap-ffmpeg-build`) with the `libx264` and `libx265` software encoders in one build pass, so ExoSnap never has to touch the FFmpeg build/CI pipeline again for either of them. This is an **infrastructure-only** change: it makes `avcodec_find_encoder_by_name("libx264"/"libx265")` return a real, usable encoder in the vendored artifact. It does **not** implement `X264VideoEncoder`/a HEVC equivalent, `VideoEncoderFactory` wiring, the ADR 0007 license/patent audit gate, or any UI — that remains separate, future, already-tracked work (ADR 0007 covers x264; there is no ADR yet for a shipped software-HEVC feature, and this plan does not create one — it only makes the capability available in the binary artifact for whenever that future work happens).

**Two repositories are involved:**
- `C:\Users\User\Development\exosnap-ffmpeg-build` — the build/CI repo (remote: `https://github.com/Exoridus/exosnap-ffmpeg-build`). Tasks 1-4 and 6 work here.
- This ExoSnap worktree (`C:\Users\User\Development\exosnap\.claude\worktrees\dxgi-magnifier-cursor` at plan-writing time, but treat it as "the ExoSnap repo" — an executor may run this from a different worktree/checkout of the same repo). Tasks 5 and 7 work here.

Every task states explicitly which repo it operates in. Do not assume the working directory carries over between tasks.

**Architecture:** No ExoSnap architecture change in this plan. FFmpeg's `libavcodec` already supports wrapping external codec libraries as "encoders" (the same mechanism the existing `--enable-encoder=aac` addition in r5 used, just for FFmpeg's *native* AAC encoder rather than an external library). `libx264` and `libx265` are external C/C++ libraries with no FFmpeg-side source changes required — FFmpeg's build system cross-compiles against them via `--enable-libx264`/`--enable-libx265` once the libraries themselves are present at configure time. Both are added as static libraries linked into the existing `avcodec` shared DLL (no new DLLs to ship) — this mirrors "one shared DLL set" the way the current mux-only artifact already works, just with two more static libraries folded into `avcodec`'s link step.

**Tech Stack:** mingw-w64 cross-compilation (existing CI pattern), GNU Autotools (x264), CMake (x265), FFmpeg's own `./configure`, GitHub Actions (`ubuntu-latest`), GoogleTest (ExoSnap-side capability proof).

## Global Constraints

- **License:** confirmed with the user — the whole FFmpeg artifact moves from LGPL-2.1-or-later to **GPL-2.0-or-later** (`--enable-gpl`, no `--enable-version3`, matching x264's declared license per ADR 0007: `GPL-2.0-or-later`). ExoSnap itself is already `GPL-3.0-or-later` (see `LICENSE` at repo root) — GPL-2.0-or-later is compatible, so this does not create new obligations beyond what ExoSnap already carries as a GPL project; it does mean the FFmpeg artifact's own bundled license text and `THIRD_PARTY_NOTICES.md` entry need to change from "LGPL" to "GPL" language (Task 3, Task 7).
- **Never pin a rolling branch/tag as if it were immutable.** x264 has no numbered release tags (unlike FFmpeg); pin by capturing the resolved commit SHA at clone time into `BUILD-INFO.txt`, mirroring the existing `FFMPEG_COMMIT` capture pattern already in `build.yml`. x265 similarly (no reliable numbered tag assumed here — same commit-capture treatment).
- **x265 8-bit only in this pass.** No 10/12-bit ("main10"/"main12") multilib build. ExoSnap has no current use for higher bit-depth software HEVC; adding it later is a self-contained follow-up if a real need appears (YAGNI).
- **Tag pushes to `r*` are immutable** (`protect-release-tags` ruleset, confirmed in project memory) — Task 6 (cutting the real `r6` release tag) is a hard-to-reverse action. **Pause and get explicit user confirmation before pushing the tag**, even though earlier steps in this plan (branch pushes, workflow_dispatch runs) are freely reversible and need no such pause.
- **Verification of "does this actually work" happens on the ExoSnap side, not inside the ffmpeg-build repo's Ubuntu CI runner** — cross-compiled Windows DLLs cannot run on the Linux CI runner. This project's own prior lesson ("Minimal-Whitelist NUR über echte Remux-Tests validieren") applies again here: push to a feature branch first, get a real artifact via `workflow_dispatch`, prove it works from Windows/ExoSnap (Task 5), and only then cut the `r6` release (Task 6).
- Full build (not just `--target exosnap`) before running ExoSnap-side ctest — this project's convention for anything touching vendored FFmpeg.
- This plan does not touch `mp4_remuxer.cpp`/`.h`, container selection, or any UI — those are separate, already-identified future slices ("software-encoding", "editing-mastering-output" per project memory). Do not add them here.

---

### Task 1: Cross-compile libx264 and wire it into the FFmpeg configure

**Repo:** `C:\Users\User\Development\exosnap-ffmpeg-build`

**Files:**
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Produces: a static `/tmp/x264-install/lib/libx264.a` + `/tmp/x264-install/include/x264.h` + `/tmp/x264-install/lib/pkgconfig/x264.pc` inside the CI job, consumed by FFmpeg's own `./configure` step in this same job via `PKG_CONFIG_PATH`.

**Background — read before starting:** The current workflow (`build.yml`) clones FFmpeg at a pinned tag, then runs `./configure` with a `--disable-everything` whitelist (see the big comment block at `build.yml:81-116`), then `make`/`make install`. x264 must be cross-compiled *before* FFmpeg's configure step runs, since FFmpeg's `--enable-libx264` check needs the library already built and discoverable via `pkg-config` at that point. x264 uses a hand-rolled `./configure` (GNU-autotools-flavored, not real autotools) that already understands `--host`/`--cross-prefix` for cross-compilation — this is the same toolchain already installed in the "Install mingw-w64 cross toolchain" step (`gcc-mingw-w64-x86-64`, `nasm`), no new packages needed for x264 specifically.

- [ ] **Step 1: Add a libx264 cross-compile step, right after the "Clone FFmpeg" step**

  In `.github/workflows/build.yml`, insert this new step immediately after the existing "Clone FFmpeg at ${{ env.FFMPEG_REF }}" step (currently ending at line 79, right before the "Configure FFmpeg" step's comment block at line 81):
  ```yaml
      # -----------------------------------------------------------------------
      # 2b. Cross-compile libx264 (static) -- GPL-2.0-or-later, external to
      # FFmpeg. x264 has no numbered release tags; pin by capturing the
      # resolved commit on the `stable` branch (mirrors the FFMPEG_COMMIT
      # capture above) so the build stays fully auditable without a version
      # tag to point at.
      # -----------------------------------------------------------------------
      - name: Clone and cross-compile libx264
        run: |
          git clone --depth=1 --branch stable https://code.videolan.org/videolan/x264.git x264-src
          X264_COMMIT=$(git -C x264-src rev-parse HEAD)
          echo "X264_COMMIT=${X264_COMMIT}" >> "$GITHUB_ENV"
          echo "Cloned x264 stable @ ${X264_COMMIT}"

          cd x264-src
          ./configure \
            --host=x86_64-w64-mingw32 \
            --cross-prefix=${CROSS_PREFIX} \
            --prefix=/tmp/x264-install \
            --enable-static \
            --disable-cli \
            --disable-opencl \
            --enable-pic
          make -j$(nproc)
          make install
          echo "--- libx264 install ---"
          ls -la /tmp/x264-install/lib /tmp/x264-install/lib/pkgconfig
  ```

- [ ] **Step 2: Wire libx264 into the FFmpeg configure step**

  In `.github/workflows/build.yml`'s "Configure FFmpeg" step (currently `working-directory: ffmpeg-src`, starting at line 117), add a `env:` block right above `working-directory:`:
  ```yaml
        env:
          PKG_CONFIG_PATH: /tmp/x264-install/lib/pkgconfig
  ```
  Then, inside the `CONFIGURE_FLAGS` array (right after the existing `--enable-encoder=aac` line), add:
  ```
            --enable-gpl
            --enable-libx264
            --enable-encoder=libx264
            --extra-cflags="-I/tmp/x264-install/include"
            --extra-ldflags="-L/tmp/x264-install/lib"
  ```
  Update the big comment block above `CONFIGURE_FLAGS` (currently ending with the "r4 -> r5" paragraph at line 106-115) by appending:
  ```
      # r5 -> r6: added --enable-gpl --enable-libx264 --enable-encoder=libx264
      # (cross-compiled as a static lib in the step above). This moves the
      # artifact's license from LGPL-2.1-or-later to GPL-2.0-or-later --
      # confirmed acceptable (ExoSnap itself is GPL-3.0-or-later). No release
      # ships an X264VideoEncoder yet (ADR 0007's license/patent audit gate
      # is a separate, not-yet-done step) -- this only makes the capability
      # available in the vendored binary.
  ```

- [ ] **Step 3: Push to a feature branch and confirm the build is green**

  ```bash
  cd "C:\Users\User\Development\exosnap-ffmpeg-build"
  git checkout -b feature/gpl-x264-x265
  git add .github/workflows/build.yml
  git commit -m "Cross-compile libx264 and wire --enable-gpl --enable-libx264 into FFmpeg configure"
  git push -u origin feature/gpl-x264-x265
  gh workflow run build.yml --repo Exoridus/exosnap-ffmpeg-build --ref feature/gpl-x264-x265 --field ffmpeg_ref=n8.1.1
  ```
  Then watch it:
  ```bash
  RUN_ID=$(gh run list --repo Exoridus/exosnap-ffmpeg-build --branch feature/gpl-x264-x265 --limit 1 --json databaseId --jq '.[0].databaseId')
  gh run watch "$RUN_ID" --repo Exoridus/exosnap-ffmpeg-build
  ```
  Expected: the run completes successfully (all steps, including "Configure FFmpeg" and "Build FFmpeg", green). If `./configure` reports `ERROR: libx264 not found`, re-check the `PKG_CONFIG_PATH` env block and that `/tmp/x264-install/lib/pkgconfig/x264.pc` was actually produced in Step 1's `ls` output (re-run with `gh run view "$RUN_ID" --repo Exoridus/exosnap-ffmpeg-build --log` to inspect).

---

### Task 2: Cross-compile libx265 and wire it into the FFmpeg configure

**Repo:** `C:\Users\User\Development\exosnap-ffmpeg-build`, same branch (`feature/gpl-x264-x265`) as Task 1.

**Files:**
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Consumes: the mingw cross toolchain and `PKG_CONFIG_PATH` pattern established in Task 1.
- Produces: a static `/tmp/x265-install/lib/libx265.a` + `/tmp/x265-install/include/x265.h` + `/tmp/x265-install/lib/pkgconfig/x265.pc`.

**Background — read before starting:** x265 (unlike x264) builds via CMake, and is written in C++ — its static archive pulls in `libstdc++` symbols that must be explicitly linked into FFmpeg's final (C-linked) `avcodec` shared library, or the link step fails with undefined-reference errors for C++ runtime symbols. `cmake` is not currently in the toolchain-install step's package list (`build.yml:46-60`) — GitHub's `ubuntu-latest` runner image ships a recent `cmake` by default, but add it explicitly anyway so the dependency is auditable rather than implicit.

- [ ] **Step 1: Add `cmake` to the toolchain install list**

  In `.github/workflows/build.yml`'s "Install mingw-w64 cross toolchain and build dependencies" step (line 46-60), add `cmake` to the `apt-get install` package list:
  ```yaml
          sudo apt-get install -y --no-install-recommends \
            gcc-mingw-w64-x86-64 \
            g++-mingw-w64-x86-64 \
            binutils-mingw-w64-x86-64 \
            llvm \
            nasm \
            yasm \
            pkg-config \
            cmake \
            make \
            git \
            zip \
            unzip
  ```

- [ ] **Step 2: Add a libx265 cross-compile step, right after the libx264 step from Task 1**

  ```yaml
      # -----------------------------------------------------------------------
      # 2c. Cross-compile libx265 (static, 8-bit only) -- GPL-2.0-or-later,
      # external to FFmpeg. 10/12-bit ("main10"/"main12") multilib is
      # deliberately not built -- ExoSnap has no current use for higher
      # bit-depth software HEVC; extend later if that changes (YAGNI).
      # -----------------------------------------------------------------------
      - name: Clone and cross-compile libx265
        run: |
          git clone --depth=1 https://code.videolan.org/videolan/x265.git x265-src
          X265_COMMIT=$(git -C x265-src rev-parse HEAD)
          echo "X265_COMMIT=${X265_COMMIT}" >> "$GITHUB_ENV"
          echo "Cloned x265 default branch @ ${X265_COMMIT}"

          cat > /tmp/toolchain-mingw64.cmake <<'EOF'
          set(CMAKE_SYSTEM_NAME Windows)
          set(CMAKE_SYSTEM_PROCESSOR x86_64)
          set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
          set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
          set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
          set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
          set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
          set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
          EOF

          mkdir x265-src/build
          cd x265-src/build
          cmake ../source \
            -G "Unix Makefiles" \
            -DCMAKE_TOOLCHAIN_FILE=/tmp/toolchain-mingw64.cmake \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX=/tmp/x265-install \
            -DENABLE_SHARED=OFF \
            -DENABLE_CLI=OFF \
            -DENABLE_LIBNUMA=OFF \
            -DHIGH_BIT_DEPTH=OFF \
            -DMAIN12=OFF \
            -DENABLE_ASSEMBLY=ON
          make -j$(nproc)
          make install
          echo "--- libx265 install ---"
          ls -la /tmp/x265-install/lib /tmp/x265-install/lib/pkgconfig
  ```

- [ ] **Step 3: Wire libx265 into the FFmpeg configure step**

  Extend the `env:` block added in Task 1 Step 2:
  ```yaml
        env:
          PKG_CONFIG_PATH: /tmp/x264-install/lib/pkgconfig:/tmp/x265-install/lib/pkgconfig
  ```
  Add to `CONFIGURE_FLAGS` (right after the `libx264` lines from Task 1):
  ```
            --enable-libx265
            --enable-encoder=libx265
            --extra-cflags="-I/tmp/x265-install/include"
            --extra-ldflags="-L/tmp/x265-install/lib"
            --extra-libs="-lstdc++"
  ```
  (`--extra-libs="-lstdc++"` is needed once, covers x265's C++ runtime dependency; x264 is pure C and needs no equivalent.)

  Append to the r5->r6 comment block from Task 1 Step 2:
  ```
      # Also added --enable-libx265 --enable-encoder=libx265 (cross-compiled,
      # 8-bit only) in the same pass, plus -lstdc++ in --extra-libs since x265
      # is C++ and its static archive is linked into avcodec's C link step.
  ```

- [ ] **Step 4: Push and confirm the build is green**

  ```bash
  cd "C:\Users\User\Development\exosnap-ffmpeg-build"
  git add .github/workflows/build.yml
  git commit -m "Cross-compile libx265 and wire --enable-libx265 into FFmpeg configure"
  git push origin feature/gpl-x264-x265
  gh workflow run build.yml --repo Exoridus/exosnap-ffmpeg-build --ref feature/gpl-x264-x265 --field ffmpeg_ref=n8.1.1
  RUN_ID=$(gh run list --repo Exoridus/exosnap-ffmpeg-build --branch feature/gpl-x264-x265 --limit 1 --json databaseId --jq '.[0].databaseId')
  gh run watch "$RUN_ID" --repo Exoridus/exosnap-ffmpeg-build
  ```
  Expected: green. A likely failure mode is an undefined-reference link error mentioning C++ symbols (`operator new`, `std::__throw_*`, etc.) — if so, double-check `--extra-libs="-lstdc++"` actually landed in the configure invocation (check the logged `Configure FFmpeg` step's "Running: ./configure ..." line).

---

### Task 3: GPL license bookkeeping and README refresh

**Repo:** `C:\Users\User\Development\exosnap-ffmpeg-build`, same branch.

**Files:**
- Modify: `.github/workflows/build.yml` (archive stem, license file copied, BUILD-INFO text, release notes body)
- Modify: `README.md`

**Interfaces:** none (docs/config only).

**Background — read before starting:** The archive is currently named `ffmpeg-win64-lgpl-shared.zip` and stages `ffmpeg-src/COPYING.LGPLv2.1` as `LICENSE.md`. FFmpeg's source tree always ships `COPYING.GPLv2`/`COPYING.GPLv3`/`COPYING.LGPLv2.1`/`COPYING.LGPLv3` regardless of configure flags — with `--enable-gpl` and no `--enable-version3`, the applicable FFmpeg license is `COPYING.GPLv2` (GPL-2.0-or-later). `README.md` is also already stale independent of this change — its versioning table stops at `r3` and its component table doesn't mention the decoders/AAC-encoder additions from `r4`/`r5`. Bring it fully current in this same edit since it's the natural point to do so.

- [ ] **Step 1: Rename the archive stem from lgpl-shared to gpl-shared**

  In `.github/workflows/build.yml:30`, change:
  ```yaml
    ARCHIVE_STEM: ffmpeg-win64-lgpl-shared
  ```
  to:
  ```yaml
    ARCHIVE_STEM: ffmpeg-win64-gpl-shared
  ```
  This also flows through to the `ARCHIVE_DIR` variable inside the "Assemble release archive" step (`build.yml:283`, `"ffmpeg-${FFMPEG_REF}-win64-lgpl-shared"` → `"ffmpeg-${FFMPEG_REF}-win64-gpl-shared"`) — update that literal too.

- [ ] **Step 2: Copy the GPL license (and x264/x265's own) into the archive**

  In the "Assemble release archive" step, change:
  ```bash
          # FFmpeg LGPL license
          cp ffmpeg-src/COPYING.LGPLv2.1 "${ARCHIVE_DIR}/LICENSE.md"
  ```
  to:
  ```bash
          # FFmpeg GPL license (--enable-gpl, no --enable-version3 -> GPLv2-or-later)
          cp ffmpeg-src/COPYING.GPLv2 "${ARCHIVE_DIR}/LICENSE.md"
          # x264/x265 are statically linked into avcodec now; bundle their own
          # license texts alongside FFmpeg's.
          cp x264-src/COPYING "${ARCHIVE_DIR}/LICENSE-x264.md"
          cp x265-src/COPYING "${ARCHIVE_DIR}/LICENSE-x265.md"
  ```

- [ ] **Step 3: Update BUILD-INFO.txt generation**

  In the same step, change:
  ```bash
            printf 'Build type:           LGPL-2.1+ shared, --disable-everything whitelist\n'
  ```
  to:
  ```bash
            printf 'Build type:           GPL-2.0-or-later shared, --disable-everything whitelist\n'
            printf 'x264 commit:          %s\n' "${X264_COMMIT}"
            printf 'x265 commit:          %s\n' "${X265_COMMIT}"
  ```

- [ ] **Step 4: Update the GitHub Release notes template**

  In the "Create GitHub Release" step's `body:` block (`build.yml:382-417`), change:
  ```
              ## FFmpeg ${{ env.FFMPEG_REF }} — minimal LGPL shared build for ExoSnap

              **Upstream commit:** `${{ env.FFMPEG_COMMIT }}`
              **Build type:** LGPL-2.1+ shared (no GPL, no nonfree, `--disable-everything` whitelist)
  ```
  to:
  ```
              ## FFmpeg ${{ env.FFMPEG_REF }} — minimal GPL shared build for ExoSnap

              **Upstream commit:** `${{ env.FFMPEG_COMMIT }}`
              **x264 commit:** `${{ env.X264_COMMIT }}`
              **x265 commit:** `${{ env.X265_COMMIT }}`
              **Build type:** GPL-2.0-or-later shared (`--enable-gpl`, no `--enable-version3`; no nonfree; `--disable-everything` whitelist otherwise)
  ```
  And change the "Encoders" line in the components list to:
  ```
              Encoders: `aac` (AAC-LC — ExoSnap's native-AAC migration, ADR 0052), `libx264`, `libx265` (software H.264/HEVC; not yet wired into any ExoSnap encoder backend — see ADR 0007)
  ```
  And replace the trailing "### LGPL §4 compliance" section with:
  ```
              ### GPL compliance

              FFmpeg source at pinned tag: https://github.com/FFmpeg/FFmpeg/tree/${{ env.FFMPEG_REF }}
              x264 source at pinned commit: https://code.videolan.org/videolan/x264/-/commit/${{ env.X264_COMMIT }}
              x265 source at pinned commit: https://code.videolan.org/videolan/x265/-/commit/${{ env.X265_COMMIT }}
              Configure flags are in `BUILD-INFO.txt` inside the archive. No patches applied to any of the three.
  ```

- [ ] **Step 5: Refresh README.md**

  In `README.md`, change the opening description (line 3):
  ```
  Pinned, minimal, LGPL-only FFmpeg builds for [ExoSnap](https://github.com/Exoridus/exosnap).
  ```
  to:
  ```
  Pinned, minimal FFmpeg builds for [ExoSnap](https://github.com/Exoridus/exosnap). GPL-2.0-or-later
  since r6 (libx264/libx265); r1-r5 were LGPL-2.1-or-later only.
  ```

  Extend the versioning table (`README.md:21-25`) with the missing r4/r5/r6 rows:
  ```
  | r4          | n8.1.1              | Adds decoders: h264, hevc, av1, opus, aac, flac, pcm_s16le, pcm_s24le, pcm_s32le, pcm_f32le (Edit-page video player needs real decode; r1-r3 were mux/demux-only) |
  | r5          | n8.1.1              | Adds encoder: aac (FfmpegAacEncoder, ADR 0052) |
  | r6          | n8.1.1              | Adds `--enable-gpl`, libx264 + libx265 (cross-compiled, static) and their encoders. **License changes from LGPL-2.1-or-later to GPL-2.0-or-later as of this release.** |
  ```

  Extend the component table (`README.md:32-56`) with:
  ```
  | Encoder: `aac` | built-in to avcodec | Native AAC-LC encode (FfmpegAacEncoder, ADR 0052) |
  | Encoder: `libx264` | external, statically linked | Software H.264 encode (ADR 0007; not yet wired into an ExoSnap encoder backend) |
  | Encoder: `libx265` | external, statically linked | Software HEVC encode (no ADR yet; build-capability only) |
  | Decoders: `h264`,`hevc`,`av1`,`opus`,`aac`,`flac`,`pcm_s16le`,`pcm_s24le`,`pcm_s32le`,`pcm_f32le` | built-in to avcodec | Edit-page video player decode |
  ```
  And change the line right below the table (currently "Components explicitly **not** built: encoders, decoders, ...") to:
  ```
  Components explicitly **not** built: avfilter, avdevice, swscale, all programs (ffmpeg/ffprobe/ffplay),
  documentation, avresample, 10/12-bit x265 multilib.
  ```

  Change the "Artifact layout" section's directory name (`README.md:66`) from `ffmpeg-<ref>-win64-lgpl-shared/` to `ffmpeg-<ref>-win64-gpl-shared/`, and add the two new license files to the listing:
  ```
    LICENSE.md           # FFmpeg GPL-2.0-or-later license
    LICENSE-x264.md      # x264 GPL-2.0-or-later license
    LICENSE-x265.md      # x265 license
    BUILD-INFO.txt       # upstream commits (FFmpeg, x264, x265), configure line, toolchain versions
  ```

  Replace the "## LGPL §4 compliance" section (`README.md:110-129`) with:
  ```markdown
  ## GPL compliance

  As of r6, FFmpeg is built with `--enable-gpl` (libx264, libx265), making the combined artifact
  **GPL-2.0-or-later**. ExoSnap itself is GPL-3.0-or-later (compatible), so this is not a change in
  ExoSnap's own distribution obligations, only in how this specific artifact's license is described:

  - **Unmodified upstream**: FFmpeg, x264, and x265 are all built from unmodified upstream source.
    No patches are applied to any of them. `BUILD-INFO.txt` records the exact commit of each.
  - **Source offer**: FFmpeg source at the pinned tag (https://github.com/FFmpeg/FFmpeg), x264 and
    x265 source at their pinned commits (recorded in `BUILD-INFO.txt` and the release notes).
  - **License shipped**: `LICENSE.md` (FFmpeg), `LICENSE-x264.md`, and `LICENSE-x265.md` are all
    included in every artifact archive.
  - r1-r5 remain LGPL-2.1-or-later only (no GPL code); only r6 onward includes libx264/libx265.

  The build scripts in this repository are MIT-licensed (see `LICENSE`).
  ```

- [ ] **Step 6: Push**

  ```bash
  cd "C:\Users\User\Development\exosnap-ffmpeg-build"
  git add .github/workflows/build.yml README.md
  git commit -m "GPL license bookkeeping for libx264/libx265; refresh README for r4-r6"
  git push origin feature/gpl-x264-x265
  ```

---

### Task 4: Get a real artifact from the branch and stage it for ExoSnap-side verification

**Repo:** run from anywhere; downloads from `Exoridus/exosnap-ffmpeg-build`, stages into the ExoSnap worktree's scratch area.

**Files:** none modified — this task only produces a local artifact for Task 5 to consume.

- [ ] **Step 1: Trigger the workflow on the branch (all three tasks' commits are now on it) and wait for it**

  ```bash
  gh workflow run build.yml --repo Exoridus/exosnap-ffmpeg-build --ref feature/gpl-x264-x265 --field ffmpeg_ref=n8.1.1
  RUN_ID=$(gh run list --repo Exoridus/exosnap-ffmpeg-build --branch feature/gpl-x264-x265 --limit 1 --json databaseId --jq '.[0].databaseId')
  gh run watch "$RUN_ID" --repo Exoridus/exosnap-ffmpeg-build
  ```
  Expected: green (all of Tasks 1-3's changes together).

- [ ] **Step 2: Download the artifact**

  ```bash
  mkdir -p /tmp/exosnap-ffmpeg-r6-candidate
  gh run download "$RUN_ID" --repo Exoridus/exosnap-ffmpeg-build \
    --name "ffmpeg-win64-gpl-shared-n8.1.1" \
    --dir /tmp/exosnap-ffmpeg-r6-candidate
  ```
  This directory now contains `ffmpeg-win64-gpl-shared.zip` and `SHA256SUMS.txt` (matching the "Upload archive artifact" step's naming, `build.yml:365-372`).

- [ ] **Step 3: Extract it for local consumption**

  ```bash
  cd /tmp/exosnap-ffmpeg-r6-candidate
  unzip -q ffmpeg-win64-gpl-shared.zip
  ls -la ffmpeg-n8.1.1-win64-gpl-shared/bin ffmpeg-n8.1.1-win64-gpl-shared/lib
  ```
  Expected: `bin/` has the four DLLs (`avformat-*.dll`, `avcodec-*.dll`, `avutil-*.dll`, `swresample-*.dll`); `lib/` has the four `.lib` import libs; `LICENSE.md`, `LICENSE-x264.md`, `LICENSE-x265.md`, `BUILD-INFO.txt` are all present at the archive root.

---

### Task 5: Prove libx264/libx265 are really registered, from the ExoSnap side

**Repo:** the ExoSnap repo (this worktree or an equivalent checkout).

**Files:**
- Create: `libs/recorder_core/tests/test_ffmpeg_build_capabilities.cpp`
- Modify: `libs/recorder_core/CMakeLists.txt` (register the new test target)
- Modify (temporarily — revert at the end of this task, see Step 5): `cmake/VendorFFmpeg.cmake`

**Interfaces:**
- Consumes: `avcodec_find_encoder_by_name(const char*) -> const AVCodec*` (existing FFmpeg API, same one used by `ffmpeg_aac_encoder.cpp:66`'s `avcodec_find_encoder`).
- Produces: no new production API — this is a build-artifact capability check only (see the file's own header comment for why it deliberately does not become `X264VideoEncoder`/an encoder backend).

**Background — read before starting:** `libs/recorder_core/src/ffmpeg_aac_encoder.cpp:66-72` already establishes the pattern this task follows: look up a codec by name/ID via `avcodec_find_encoder*`, and treat a null result as a clean, structured failure. That precedent (ADR 0052) tolerates a null result gracefully because it runs against *whatever* FFmpeg build happens to be pinned at the time. This task's tests are different in intent: they should **assert hard** (not skip) that `libx264`/`libx265` are present, because the whole point of this task is to prove the *candidate* r6 build actually has them before it is ever tagged as a real release — a null result here means Tasks 1-3 need fixing, not that the test should tolerate it.

- [ ] **Step 1: Point VendorFFmpeg.cmake at the local candidate artifact (temporary)**

  In `cmake/VendorFFmpeg.cmake`, temporarily change the `FetchContent_Declare` block (lines 41-46) from the real `r5` URL to a local `file://` URL pointing at the zip downloaded in Task 4:
  ```cmake
  FetchContent_Declare(
      ffmpeg_prebuilt
      URL      "file:///C:/Users/User/AppData/Local/Temp/exosnap-ffmpeg-r6-candidate/ffmpeg-win64-gpl-shared.zip"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  ```
  (Omit `URL_HASH` for this throwaway local test — there is nothing to pin yet, this build hasn't been released.) Adjust the path if Task 4 downloaded to a different location. **Do not commit this change** — Step 5 reverts it, and the real, permanent pin happens in Task 7 against the actual tagged `r6` release.

- [ ] **Step 2: Write the capability-proof test**

  Create `libs/recorder_core/tests/test_ffmpeg_build_capabilities.cpp`:
  ```cpp
  #include <gtest/gtest.h>

  extern "C" {
  #include <libavcodec/avcodec.h>
  }

  // Proves the vendored FFmpeg build (exosnap-ffmpeg-build) actually has the
  // libx264/libx265 encoders compiled in. This is a check on the *build
  // artifact's capability*, not a test of ExoSnap production code -- there is
  // no X264VideoEncoder/HEVC equivalent wired up yet (ADR 0007 covers x264;
  // wiring either into VideoEncoderFactory/IVideoEncoder is separate, future
  // work). Unlike FfmpegAacEncoderTest's graceful-skip pattern (which
  // tolerates whatever FFmpeg build happens to be pinned), these assert hard:
  // a null result here means the vendored build is wrong, not that the test
  // should degrade.
  TEST(FfmpegBuildCapabilitiesTest, LibX264EncoderIsRegistered) {
      const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
      ASSERT_NE(codec, nullptr) << "libx264 encoder not found in the vendored FFmpeg build -- "
                                   "was --enable-libx264/--enable-encoder=libx264 set?";
  }

  TEST(FfmpegBuildCapabilitiesTest, LibX265EncoderIsRegistered) {
      const AVCodec* codec = avcodec_find_encoder_by_name("libx265");
      ASSERT_NE(codec, nullptr) << "libx265 encoder not found in the vendored FFmpeg build -- "
                                   "was --enable-libx265/--enable-encoder=libx265 set?";
  }
  ```

- [ ] **Step 3: Register the test target**

  In `libs/recorder_core/CMakeLists.txt`, add near the other small FFmpeg-only test target (`test_output_format_audio_src`, around line 214-220):
  ```cmake
  exosnap_add_gtest(
      NAME test_ffmpeg_build_capabilities
      TEST_PREFIX recorder_core.
      SOURCES tests/test_ffmpeg_build_capabilities.cpp
      LIBRARIES FFmpeg::mux
  )
  ```

- [ ] **Step 4: Full build and run**

  ```
  cmake --build build/windows-x64-debug
  ctest --test-dir build/windows-x64-debug -R "recorder_core.FfmpegBuildCapabilitiesTest" -V
  ```
  Expected: **PASS** on both tests. If either fails, go back to Task 1/Task 2 in the ffmpeg-build repo, fix the configure flags, push, re-run Task 4 to get a fresh candidate artifact, and re-point Step 1's `file://` URL at the new zip.

  Also run the full recorder_core suite once to confirm the new (GPL, larger) DLLs don't regress anything already depending on the old LGPL artifact:
  ```
  ctest --test-dir build/windows-x64-debug -R "recorder_core\." -V
  ```
  Expected: **PASS** (no regressions — nothing in this plan changes any existing muxer/demuxer/decoder/aac-encoder behavior).

- [ ] **Step 5: Revert the temporary local pin**

  ```bash
  git diff cmake/VendorFFmpeg.cmake
  git checkout -- cmake/VendorFFmpeg.cmake
  ```
  Confirm `git status` shows `cmake/VendorFFmpeg.cmake` clean again (still pinned at the real `r5` release) before proceeding to Task 6. Keep `test_ffmpeg_build_capabilities.cpp` and its CMakeLists registration — those are real, permanent additions; only the throwaway local-file pin gets reverted here (it becomes the real `r6` pin in Task 7).

---

### Task 6: Cut the real r6 release

**Repo:** `C:\Users\User\Development\exosnap-ffmpeg-build`

**⚠️ Pause here and get explicit user confirmation before Step 1.** Pushing an `r*` tag is protected by this repo's `protect-release-tags` ruleset — tags matching `r*` cannot be deleted or force-updated once pushed. Everything in Tasks 1-5 was freely reversible (branch commits, workflow_dispatch runs, a local-only cmake edit that was already reverted); this step is not.

- [ ] **Step 1 (after user confirmation): merge the feature branch and tag**

  ```bash
  cd "C:\Users\User\Development\exosnap-ffmpeg-build"
  git checkout main
  git merge --ff-only feature/gpl-x264-x265
  git push origin main
  git tag r6
  git push origin r6
  ```
  The tag push triggers `build.yml` automatically (the `push: tags: r*` trigger, `build.yml:14-16`) and creates a real GitHub Release with the archive attached (`build.yml:377-422`).

- [ ] **Step 2: Wait for the release and capture the SHA256**

  ```bash
  RUN_ID=$(gh run list --repo Exoridus/exosnap-ffmpeg-build --branch r6 --limit 1 --json databaseId --jq '.[0].databaseId')
  gh run watch "$RUN_ID" --repo Exoridus/exosnap-ffmpeg-build
  gh release download r6 --repo Exoridus/exosnap-ffmpeg-build --pattern SHA256SUMS.txt --output -
  ```
  Record the SHA256 for `ffmpeg-win64-gpl-shared.zip` from the output — Task 7 needs it.

---

### Task 7: Permanently repin ExoSnap to r6

**Repo:** the ExoSnap repo.

**Files:**
- Modify: `cmake/VendorFFmpeg.cmake`
- Modify: `THIRD_PARTY_NOTICES.md`

**Interfaces:** none new — this is the real version of the throwaway edit from Task 5 Step 1, now pointing at the actually-tagged, immutable `r6` release.

- [ ] **Step 1: Update the pin**

  In `cmake/VendorFFmpeg.cmake`, change the header comment (lines 1-32) to document r6 the same way r1-r5 are documented (append after the existing "r4 -> r5" line, `:28-29`):
  ```
  # r5 -> r6: added --enable-gpl, libx264, libx265 (cross-compiled, static,
  # 8-bit-only for x265). License changes from LGPL-2.1-or-later to
  # GPL-2.0-or-later as of this release (ExoSnap is GPL-3.0-or-later,
  # compatible). Neither encoder is wired into an ExoSnap encoder backend yet
  # (ADR 0007 covers x264's eventual X264VideoEncoder; that remains separate,
  # future work) -- this only makes avcodec_find_encoder_by_name("libx264"/
  # "libx265") return a real encoder, proven by
  # test_ffmpeg_build_capabilities.cpp.
  ```
  Also change line 17 (`# License:      LGPL-2.1-or-later (compatible with ExoSnap GPL-3.0-or-later)`) to:
  ```
  # License:      GPL-2.0-or-later (compatible with ExoSnap GPL-3.0-or-later)
  ```

  Change the version cache variable (lines 36-37):
  ```cmake
  set(EXOSNAP_FFMPEG_VERSION "r6-n8.1.1"
      CACHE STRING "Pinned exosnap-ffmpeg-build release version (informational)")
  ```

  Change the `FetchContent_Declare` block (lines 41-46) to the real release URL and the SHA256 captured in Task 6 Step 2:
  ```cmake
  FetchContent_Declare(
      ffmpeg_prebuilt
      URL      "https://github.com/Exoridus/exosnap-ffmpeg-build/releases/download/r6/ffmpeg-win64-gpl-shared.zip"
      URL_HASH "SHA256=<paste-the-hash-from-Task-6-Step-2-here>"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  ```

- [ ] **Step 2: Update THIRD_PARTY_NOTICES.md**

  In `THIRD_PARTY_NOTICES.md`, change the FFmpeg section (lines 136-154):
  ```markdown
  ### FFmpeg

  - **Version:** exosnap-ffmpeg-build release `r6` (upstream FFmpeg `n8.1.1`)
  - **Project:** https://github.com/Exoridus/exosnap-ffmpeg-build (build/packaging
    repository) / https://ffmpeg.org (upstream FFmpeg source)
  - **License:** GPL-2.0-or-later as of r6 (`--enable-gpl` for libx264/libx265; r1-r5 were
    LGPL-2.1-or-later only, no GPL code)
  - **Linkage:** dynamic (shared DLLs deployed alongside the ExoSnap binary); libx264/libx265
    are statically linked into the `avcodec` DLL
  - **Bundled license:** `licenses/ffmpeg.txt`, `licenses/ffmpeg-x264.txt`, `licenses/ffmpeg-x265.txt`
  - **DLLs deployed:** `avformat-62.dll`, `avcodec-62.dll`, `avutil-60.dll`,
    `swresample-6.dll` (avfilter/swscale/avdevice are not built by this component set and
    are excluded from the portable ZIP)
  - **Role:** Post-recording stream-copy remux of MKV → progressive MP4 (`+faststart`), plus the
    native AAC-LC encoder (ADR 0052). `libx264`/`libx265` are compiled into this build but are
    **not yet used by any ExoSnap encoder** — no `X264VideoEncoder`/HEVC equivalent exists yet
    (see ADR 0007); their presence is a build-capability enablement, tracked so the FFmpeg build
    pipeline does not need to be touched again when that feature work happens.
  - **Note:** ExoSnap is licensed GPL-3.0-or-later, which is compatible with GPL-2.0-or-later.
    No additional distribution obligations arise beyond what ExoSnap already carries as a GPL
    project. Users may replace the DLLs with compatible versions. The exosnap-ffmpeg-build
    repository builds this exact component set from unmodified upstream FFmpeg `n8.1.1`, x264,
    and x265 sources — it does not fork or patch any of them.

  ### libx264 (bundled in FFmpeg's `avcodec` DLL)

  - **License:** GPL-2.0-or-later
  - **Project:** https://code.videolan.org/videolan/x264
  - **Linkage:** static, into `avcodec-62.dll` (built by exosnap-ffmpeg-build, see FFmpeg entry above)
  - **Role:** compiled in as `avcodec_find_encoder_by_name("libx264")`'s backing encoder. Not
    currently used by any ExoSnap encoder backend (ADR 0007 covers the eventual feature work).

  ### libx265 (bundled in FFmpeg's `avcodec` DLL)

  - **License:** GPL-2.0-or-later
  - **Project:** https://code.videolan.org/videolan/x265
  - **Linkage:** static, into `avcodec-62.dll` (built by exosnap-ffmpeg-build, see FFmpeg entry above)
  - **Role:** compiled in as `avcodec_find_encoder_by_name("libx265")`'s backing encoder. Not
    currently used by any ExoSnap encoder backend; no ADR exists yet for shipping software HEVC
    as a feature — this is build-capability only.
  ```

  Note: `cmake/VendorFFmpeg.cmake`'s existing license-staging block (lines 126-145) only stages one `${_ffmpeg_root}/LICENSE.md` today. Extend it to also stage the two new license files if present, right after the existing `if(EXISTS "${_ffmpeg_license}")` block:
  ```cmake
  foreach(_extra_license LICENSE-x264.md LICENSE-x265.md)
      set(_extra_license_path "${_ffmpeg_root}/${_extra_license}")
      if(EXISTS "${_extra_license_path}")
          configure_file("${_extra_license_path}"
                         "${_exosnap_license_stage}/ffmpeg-${_extra_license}"
                         COPYONLY)
      endif()
  endforeach()
  ```
  (Placed after the existing `else()` branch's closing `endif()`, i.e. after the current line 145.)

- [ ] **Step 3: Full build and full test suite**

  ```
  cmake --build build/windows-x64-debug
  ctest --test-dir build/windows-x64-debug -R "recorder_core\." -V
  ```
  Expected: **PASS**, including `FfmpegBuildCapabilitiesTest.LibX264EncoderIsRegistered` and `LibX265EncoderIsRegistered` (Task 5), now running against the real, immutable `r6` artifact instead of the throwaway local candidate.

- [ ] **Step 4: Commit**

  ```bash
  git add cmake/VendorFFmpeg.cmake THIRD_PARTY_NOTICES.md
  git commit -m "$(cat <<'EOF'
  Repin FFmpeg to exosnap-ffmpeg-build r6 (adds libx264 + libx265, GPL-2.0-or-later)

  Build-capability only: avcodec_find_encoder_by_name("libx264"/"libx265") now
  returns a real encoder, proven by test_ffmpeg_build_capabilities.cpp. No
  encoder backend (X264VideoEncoder/HEVC equivalent) is wired up in this
  change -- that remains separate, future work per ADR 0007.
  EOF
  )"
  ```

  Note: no PR/merge instructions given here deliberately — follow this project's normal review flow for landing the ExoSnap-side change.

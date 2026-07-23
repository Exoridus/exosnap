# S1 — NVENC Advanced-Encode Capability Probe — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the app truthful, per-GPU, per-codec facts about NVENC's advanced-encode
capabilities (max B-frames, B-frame-ref-mode, Lookahead, Temporal-AQ) — the measurement
prerequisite for the SSIM/VMAF quality harness (S2/S3) and for the future Expert
B-Frames/Lookahead/Temporal-AQ settings (S5+). No encoder behavior changes; this plan only
adds *facts*, never wiring that changes what gets recorded.

**Architecture:** Mirrors the existing `chroma444` (YUV444) capability exactly, at every
layer: `NvidiaRuntimeFacts` (raw per-codec probe result) → `CapabilityCacheStore` (JSON
disk cache, keyed by adapter+driver+schema) → `CapabilitySet` (resolved,
`SupportAnnotation`-typed facts the rest of the app queries) → `AdapterEncoderCapability`
(the parallel per-adapter path that feeds the Device tab). One structural difference from
`chroma444`: these are generation-dependent NVENC features, so the **baseline is
fail-closed** (`NotImplemented`/0 for every codec when unprobed), and a real probe
*upgrades* codecs the GPU actually advertises — the inverse of `chroma444`'s
optimistic-baseline-that-gets-downgraded pattern.

**Tech Stack:** C++20, NVENC SDK 13.0 (`third_party/nvidia/nvEncodeAPI.h`), GoogleTest, Qt 6
(Device tab UI), Qt JSON (on-disk capability cache).

## Global Constraints

- No architecture/name-based GPU-generation heuristics anywhere ("Turing+" is documentation
  only, never code) — see D1 of `encoder-quality-features-spec.md`.
- Every new field defaults to the "unsupported"/fail-closed value; a codec that was never
  advertised by the GPU must never gain a positive claim.
- Pre-1.0 cache policy: schema mismatch ⇒ discard and silently re-probe, no migration code.
- This plan does **not** touch `nvenc_encoder.cpp`/`.h`, `FetchPresetConfig`, or any encoder
  behavior — it is capability data only. Do not add UI controls that let a user toggle
  B-Frames/Lookahead/Temporal-AQ (that is S5, gated on S6/S7 engine wiring per the spec's
  honesty rule).

---

## File Structure

| File | Responsibility |
|---|---|
| `libs/capability/include/capability/runtime_snapshot.h` | `NvencAdvancedEncodeFacts` struct + 3 new `NvidiaRuntimeFacts` fields (raw probe result) |
| `libs/capability/src/runtime_query.cpp` | `ProbeNvencCodecs` extended to query the 4 new NVENC caps per advertised codec |
| `app/settings/CapabilityCacheStore.cpp` | JSON (de)serialization of the 3 new fact structs, disk cache round-trip |
| `libs/capability/include/capability/capability_cache_key.h` | Schema version bump (payload shape changed) |
| `libs/capability/include/capability/capability_set.h` | `BFrameCapability` struct + 3 new `CapabilitySet` maps + 3 new `QueryXxx` accessors |
| `libs/capability/src/capability_set.cpp` | Accessor implementations |
| `libs/capability/include/capability/capability_builder.h` | `ApplyNvencAdvancedEncodeSupport` declaration |
| `libs/capability/src/capability_builder.cpp` | Fail-closed static baseline + the upgrade-on-probe refinement function |
| `libs/capability/include/capability/adapter_capability.h` | 6 new `AdapterEncoderCapability` fields (per-adapter probe result for the Device tab) |
| `libs/capability/src/adapter_capability.cpp` | `ProbeNvencGuidsOnDevice` extended to query the same 4 caps per adapter |
| `app/pages/DevicePage.cpp` | 3 new quiet fact rows (B-frames max / Lookahead / Temporal-AQ), one row per feature with per-codec chips |
| Tests: `libs/capability/tests/test_runtime_merge.cpp`, `test_adapter_capability.cpp`, `test_capability_cache_key.cpp`, `app/tests/test_capability_cache_store.cpp`, `app/tests/test_device_page.cpp` | Existing files, extended in place — no new test targets/CMake changes needed |

---

### Task 1: Raw facts — `NvidiaRuntimeFacts`, hardware probe, disk-cache round-trip

**Files:**
- Modify: `libs/capability/include/capability/runtime_snapshot.h:57-81` (`NvidiaRuntimeFacts`)
- Modify: `libs/capability/src/runtime_query.cpp:216-236` (`ProbeNvencCodecs`, inside the existing `if (funcs.nvEncGetEncodeCaps != nullptr)` block)
- Modify: `libs/capability/include/capability/capability_cache_key.h:15` (schema bump)
- Modify: `app/settings/CapabilityCacheStore.cpp:87-99` (`SnapshotToJson`) and `:131-147` (`SnapshotFromJson`)
- Test: `app/tests/test_capability_cache_store.cpp:36-64` (`MakeSnapshot`) and `:83-99` (`RoundtripWithMatchingKey`)

**Interfaces:**
- Produces: `struct NvencAdvancedEncodeFacts { int max_bframes; int bframe_ref_mode; bool lookahead; bool temporal_aq; };` at namespace scope in `runtime_snapshot.h`, plus three `NvidiaRuntimeFacts` members `nvenc_adv_h264`, `nvenc_adv_hevc`, `nvenc_adv_av1` of that type. Task 2 and Task 3 both consume these three fields directly.

- [ ] **Step 1: Write the failing cache round-trip test**

In `app/tests/test_capability_cache_store.cpp`, extend `MakeSnapshot()` (currently lines 36-64) by adding these lines right after `snap.nvidia.nvenc_yuv444_hevc = true;` (line 47):

```cpp
    snap.nvidia.nvenc_adv_h264 = {2, 1, true, true};   // max_bframes, bframe_ref_mode, lookahead, temporal_aq
    snap.nvidia.nvenc_adv_hevc = {3, 2, true, false};
    snap.nvidia.nvenc_adv_av1 = {0, 0, false, false};  // GPU advertises AV1 but with no B-frame/lookahead support
```

Then extend `RoundtripWithMatchingKey` (currently lines 83-99) by adding these assertions right after `EXPECT_TRUE(loaded->nvidia.nvenc_yuv444_hevc);` (line 98):

```cpp
    EXPECT_EQ(loaded->nvidia.nvenc_adv_h264.max_bframes, 2);
    EXPECT_EQ(loaded->nvidia.nvenc_adv_h264.bframe_ref_mode, 1);
    EXPECT_TRUE(loaded->nvidia.nvenc_adv_h264.lookahead);
    EXPECT_TRUE(loaded->nvidia.nvenc_adv_h264.temporal_aq);
    EXPECT_EQ(loaded->nvidia.nvenc_adv_hevc.max_bframes, 3);
    EXPECT_FALSE(loaded->nvidia.nvenc_adv_hevc.temporal_aq);
    EXPECT_EQ(loaded->nvidia.nvenc_adv_av1.max_bframes, 0);
    EXPECT_FALSE(loaded->nvidia.nvenc_adv_av1.lookahead);
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/windows-x64-debug --target capability_tests --config Debug` (or the project's standard test-build target — see `libs/capability/CMakeLists.txt` for the exact target name if this fails)
Expected: FAIL — compile error, `'struct exosnap::capability::NvidiaRuntimeFacts' has no member named 'nvenc_adv_h264'`

- [ ] **Step 3: Add the struct and fields to `runtime_snapshot.h`**

In `libs/capability/include/capability/runtime_snapshot.h`, insert immediately before `struct NvidiaRuntimeFacts {` (line 57):

```cpp
// Per-codec NVENC advanced-encode capability facts (B-frames, Lookahead,
// Temporal-AQ) — see docs/superpowers/plans/2026-07-23-encoder-quality-harness-s1-capability-probe.md.
// Only meaningful when NvidiaRuntimeFacts::nvenc_codec_probed is true AND the
// specific codec was advertised; every field defaults to "unsupported" so an
// unprobed or unadvertised codec never claims a generation-dependent feature.
struct NvencAdvancedEncodeFacts {
    int max_bframes = 0;
    int bframe_ref_mode = 0; // NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE: 0/1/2, SDK semantics
    bool lookahead = false;
    bool temporal_aq = false;
};

```

Then inside `struct NvidiaRuntimeFacts { ... }`, add after the existing `bool nvenc_yuv444_hevc = false;` line (currently line 80):

```cpp

    // Per-codec advanced-encode facts (B-frames, Lookahead, Temporal-AQ).
    // Populated only for codecs this GPU actually advertised (nvenc_h264/_hevc/_av1);
    // unlike nvenc_yuv444_*, AV1 gets a real probe too (NVENC AV1 B-frames use the
    // same frameIntervalP mechanic as H.264/HEVC, just without a 4:4:4 path).
    NvencAdvancedEncodeFacts nvenc_adv_h264;
    NvencAdvancedEncodeFacts nvenc_adv_hevc;
    NvencAdvancedEncodeFacts nvenc_adv_av1;
```

- [ ] **Step 4: Bump the capability cache schema version**

In `libs/capability/include/capability/capability_cache_key.h`, change line 15:

```cpp
inline constexpr int kCapabilityCacheSchemaVersion = 2;
```

(Was `1`. The comment above it, lines 11-14, already documents the bump convention — no comment change needed.)

- [ ] **Step 5: Wire JSON serialization in `CapabilityCacheStore.cpp`**

In `SnapshotToJson` (`app/settings/CapabilityCacheStore.cpp:87-99`), add after `nvidia[QStringLiteral("nvenc_yuv444_hevc")] = s.nvidia.nvenc_yuv444_hevc;` (line 99):

```cpp
    auto adv_to_json = [](const capability::NvencAdvancedEncodeFacts& adv) {
        QJsonObject obj;
        obj[QStringLiteral("max_bframes")] = adv.max_bframes;
        obj[QStringLiteral("bframe_ref_mode")] = adv.bframe_ref_mode;
        obj[QStringLiteral("lookahead")] = adv.lookahead;
        obj[QStringLiteral("temporal_aq")] = adv.temporal_aq;
        return obj;
    };
    nvidia[QStringLiteral("nvenc_adv_h264")] = adv_to_json(s.nvidia.nvenc_adv_h264);
    nvidia[QStringLiteral("nvenc_adv_hevc")] = adv_to_json(s.nvidia.nvenc_adv_hevc);
    nvidia[QStringLiteral("nvenc_adv_av1")] = adv_to_json(s.nvidia.nvenc_adv_av1);
```

In `SnapshotFromJson` (`app/settings/CapabilityCacheStore.cpp:131-147`), add after `out.nvidia.nvenc_yuv444_hevc = nvidia.value(QStringLiteral("nvenc_yuv444_hevc")).toBool(false);` (line 147):

```cpp
    auto adv_from_json = [](const QJsonObject& parent, const char* key) {
        capability::NvencAdvancedEncodeFacts adv;
        const QJsonObject obj = parent.value(QLatin1String(key)).toObject();
        adv.max_bframes = obj.value(QStringLiteral("max_bframes")).toInt(0);
        adv.bframe_ref_mode = obj.value(QStringLiteral("bframe_ref_mode")).toInt(0);
        adv.lookahead = obj.value(QStringLiteral("lookahead")).toBool(false);
        adv.temporal_aq = obj.value(QStringLiteral("temporal_aq")).toBool(false);
        return adv;
    };
    out.nvidia.nvenc_adv_h264 = adv_from_json(nvidia, "nvenc_adv_h264");
    out.nvidia.nvenc_adv_hevc = adv_from_json(nvidia, "nvenc_adv_hevc");
    out.nvidia.nvenc_adv_av1 = adv_from_json(nvidia, "nvenc_adv_av1");
```

A missing/malformed `nvenc_adv_*` object degrades to the all-zero/false default via `.toObject()` on a missing key returning an empty `QJsonObject` — matches the existing fail-closed pattern for every other field in this function, so no explicit corruption handling is needed.

- [ ] **Step 6: Run the cache round-trip test to verify it passes**

Run: `cmake --build build/windows-x64-debug --target capability_tests --config Debug && ctest --test-dir build/windows-x64-debug -R CapabilityCacheStoreTest --output-on-failure`
Expected: PASS, all `CapabilityCacheStoreTest.*` tests green including `RoundtripWithMatchingKey`.

- [ ] **Step 7: Wire the real hardware probe in `runtime_query.cpp`**

In `libs/capability/src/runtime_query.cpp`, inside `ProbeNvencCodecs` (lines 153-242), the existing YUV444 block reads:

```cpp
            if (funcs.nvEncGetEncodeCaps != nullptr) {
                auto query_yuv444 = [&funcs, encoder](const GUID& codec) -> bool {
                    NV_ENC_CAPS_PARAM capsParam{};
                    capsParam.version = NV_ENC_CAPS_PARAM_VER;
                    capsParam.capsToQuery = NV_ENC_CAPS_SUPPORT_YUV444_ENCODE;
                    int value = 0;
                    return funcs.nvEncGetEncodeCaps(encoder, codec, &capsParam, &value) == NV_ENC_SUCCESS && value != 0;
                };
                if (nvidia.nvenc_h264) {
                    nvidia.nvenc_yuv444_h264 = query_yuv444(NV_ENC_CODEC_H264_GUID);
                }
                if (nvidia.nvenc_hevc) {
                    nvidia.nvenc_yuv444_hevc = query_yuv444(NV_ENC_CODEC_HEVC_GUID);
                }
            }
```

Replace it with (adds the advanced-encode query alongside the existing YUV444 one, same `if` guard, same lambda-capture style):

```cpp
            if (funcs.nvEncGetEncodeCaps != nullptr) {
                auto query_yuv444 = [&funcs, encoder](const GUID& codec) -> bool {
                    NV_ENC_CAPS_PARAM capsParam{};
                    capsParam.version = NV_ENC_CAPS_PARAM_VER;
                    capsParam.capsToQuery = NV_ENC_CAPS_SUPPORT_YUV444_ENCODE;
                    int value = 0;
                    return funcs.nvEncGetEncodeCaps(encoder, codec, &capsParam, &value) == NV_ENC_SUCCESS && value != 0;
                };
                if (nvidia.nvenc_h264) {
                    nvidia.nvenc_yuv444_h264 = query_yuv444(NV_ENC_CODEC_H264_GUID);
                }
                if (nvidia.nvenc_hevc) {
                    nvidia.nvenc_yuv444_hevc = query_yuv444(NV_ENC_CODEC_HEVC_GUID);
                }

                auto query_cap = [&funcs, encoder](const GUID& codec, NV_ENC_CAPS cap) -> int {
                    NV_ENC_CAPS_PARAM capsParam{};
                    capsParam.version = NV_ENC_CAPS_PARAM_VER;
                    capsParam.capsToQuery = cap;
                    int value = 0;
                    return funcs.nvEncGetEncodeCaps(encoder, codec, &capsParam, &value) == NV_ENC_SUCCESS ? value : 0;
                };
                auto query_advanced = [&query_cap](const GUID& codec) -> NvencAdvancedEncodeFacts {
                    NvencAdvancedEncodeFacts adv;
                    adv.max_bframes = query_cap(codec, NV_ENC_CAPS_NUM_MAX_BFRAMES);
                    adv.bframe_ref_mode = query_cap(codec, NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE);
                    adv.lookahead = query_cap(codec, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;
                    adv.temporal_aq = query_cap(codec, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) != 0;
                    return adv;
                };
                if (nvidia.nvenc_h264) {
                    nvidia.nvenc_adv_h264 = query_advanced(NV_ENC_CODEC_H264_GUID);
                }
                if (nvidia.nvenc_hevc) {
                    nvidia.nvenc_adv_hevc = query_advanced(NV_ENC_CODEC_HEVC_GUID);
                }
                if (nvidia.nvenc_av1) {
                    nvidia.nvenc_adv_av1 = query_advanced(NV_ENC_CODEC_AV1_GUID);
                }
            }
```

**No CI test exists for this step** — same as the pre-existing `query_yuv444` block it sits beside, this is a real NVENC-session hardware call with no seam for unit testing in this file (the codebase's testable seam is one layer up: `ApplyNvencYuv444Support`/`CapabilityBuilder::BuildEffectiveCapabilities`, which is exactly what Task 2 tests). Verification for this specific block is manual, on a real NVIDIA GPU, once Task 4 (Device tab) lands and the new rows show real numbers instead of the fail-closed baseline.

- [ ] **Step 8: Full capability test suite green**

Run: `ctest --test-dir build/windows-x64-debug -R "Capability" --output-on-failure`
Expected: PASS, no regressions in any existing `Capability*`/`Nvenc*` test.

- [ ] **Step 9: Commit**

```bash
git add libs/capability/include/capability/runtime_snapshot.h \
        libs/capability/src/runtime_query.cpp \
        libs/capability/include/capability/capability_cache_key.h \
        app/settings/CapabilityCacheStore.cpp \
        app/tests/test_capability_cache_store.cpp
git commit -m "feat(capability): probe NVENC advanced-encode caps (B-frames, lookahead, temporal-AQ)"
```

---

### Task 2: `CapabilitySet` — resolved facts, fail-closed baseline, upgrade-on-probe

**Files:**
- Modify: `libs/capability/include/capability/capability_set.h:15-27` (add `BFrameCapability` before `struct CapabilitySet`), `:58-91` (add maps + accessors)
- Modify: `libs/capability/src/capability_set.cpp:167-173` (add accessor implementations)
- Modify: `libs/capability/include/capability/capability_builder.h:47-54` (add declaration)
- Modify: `libs/capability/src/capability_builder.cpp:61-71` (baseline), `:189-190` (wire the call), `:223-246` (add the function after `ApplyNvencYuv444Support`)
- Test: `libs/capability/tests/test_runtime_merge.cpp` (extend, new `TEST` blocks near the existing `NvencYuv444SupportTest` tests, lines 437-492)

**Interfaces:**
- Consumes: `NvidiaRuntimeFacts::nvenc_adv_h264/_hevc/_av1` (Task 1), `NvidiaRuntimeFacts::nvenc_codec_probed`/`nvenc_h264`/`nvenc_hevc`/`nvenc_av1` (pre-existing).
- Produces: `struct BFrameCapability { SupportAnnotation annotation; int max_bframes; int bframe_ref_mode; };` at namespace scope; `CapabilitySet::QueryBFrames(VideoCodec) const -> BFrameCapability`, `CapabilitySet::QueryLookahead(VideoCodec) const -> SupportAnnotation`, `CapabilitySet::QueryTemporalAq(VideoCodec) const -> SupportAnnotation`. Task 4 (DevicePage) does **not** consume these directly — it reads the parallel `AdapterEncoderCapability` fields from Task 3 instead, since the Device tab is per-adapter, not the global `CapabilitySet`. These accessors exist for future S5/S6/S7 consumers (Expert-setting resolver clamps) and are exercised here only by unit tests.

- [ ] **Step 1: Write the failing capability_builder tests**

In `libs/capability/tests/test_runtime_merge.cpp`, add these three tests immediately after the existing `TEST(NvencYuv444SupportTest, BuildEffective_NoYuv444_Blocks444KeepsCs420)` block (ends at line 492):

```cpp
// Advanced-encode (B-frames/Lookahead/Temporal-AQ) baseline is fail-closed:
// with no probe, every codec reports NotImplemented/0 — the inverse of the
// chroma444 baseline, because these are generation-dependent features that
// must never be assumed available.
TEST(NvencAdvancedEncodeSupportTest, StaticBaselineIsNotImplementedForEveryCodec) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::H264Nvenc).annotation));
    EXPECT_EQ(caps.QueryBFrames(VideoCodec::H264Nvenc).max_bframes, 0);
    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::Av1Nvenc).annotation));
    EXPECT_FALSE(IsSelectable(caps.QueryLookahead(VideoCodec::HevcNvenc)));
    EXPECT_FALSE(IsSelectable(caps.QueryTemporalAq(VideoCodec::HevcNvenc)));
}

// A real probe that advertises H.264 with 2 max B-frames and lookahead, but
// AV1 with zero of everything, upgrades exactly the codecs/features the GPU
// actually reported — AV1 stays at the fail-closed baseline even though the
// GPU advertised the codec at all (nvenc_av1 = true), because max_bframes = 0.
TEST(NvencAdvancedEncodeSupportTest, ProbedFacts_UpgradeOnlyWhatGpuAdvertised) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_codec_probed = true;
    facts.nvenc_h264 = true;
    facts.nvenc_av1 = true;
    facts.nvenc_adv_h264 = {2, 1, true, true};
    facts.nvenc_adv_av1 = {0, 0, false, false};

    ApplyNvencAdvancedEncodeSupport(caps, facts);

    const auto h264 = caps.QueryBFrames(VideoCodec::H264Nvenc);
    EXPECT_TRUE(IsSelectable(h264.annotation));
    EXPECT_EQ(h264.max_bframes, 2);
    EXPECT_EQ(h264.bframe_ref_mode, 1);
    EXPECT_TRUE(IsSelectable(caps.QueryLookahead(VideoCodec::H264Nvenc)));
    EXPECT_TRUE(IsSelectable(caps.QueryTemporalAq(VideoCodec::H264Nvenc)));

    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::Av1Nvenc).annotation));
    EXPECT_EQ(caps.QueryBFrames(VideoCodec::Av1Nvenc).max_bframes, 0);
}

// A codec the GPU never advertised at all (HEVC absent here) is left at the
// fail-closed baseline even though the probe ran — mirrors
// ApplyNvencYuv444Support's "not advertised -> untouched" rule.
TEST(NvencAdvancedEncodeSupportTest, CodecNotAdvertised_StaysAtBaseline) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_codec_probed = true;
    facts.nvenc_hevc = false; // this GPU does not advertise HEVC at all
    facts.nvenc_adv_hevc = {4, 2, true, true}; // must be ignored

    ApplyNvencAdvancedEncodeSupport(caps, facts);

    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::HevcNvenc).annotation));
    EXPECT_EQ(caps.QueryBFrames(VideoCodec::HevcNvenc).max_bframes, 0);
}

// Probe did not run -> the fail-closed baseline stands untouched.
TEST(NvencAdvancedEncodeSupportTest, NotProbed_KeepsFailClosedBaseline) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_codec_probed = false;
    facts.nvenc_h264 = true;
    facts.nvenc_adv_h264 = {2, 1, true, true}; // must be ignored, probe did not run

    ApplyNvencAdvancedEncodeSupport(caps, facts);

    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::H264Nvenc).annotation));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build/windows-x64-debug --target capability_tests --config Debug`
Expected: FAIL — compile error, `'class exosnap::capability::CapabilitySet' has no member named 'QueryBFrames'`

- [ ] **Step 3: Add `BFrameCapability` and the three maps/accessors to `capability_set.h`**

In `libs/capability/include/capability/capability_set.h`, insert after the `ComboKeyHash` struct (currently ends line 27, before `struct CapabilitySet {` on line 29):

```cpp

// Per-codec NVENC B-frame capability: whether/how many B-frames the GPU and
// driver support for this codec, plus the B-frame-reference-mode capability
// bit (0/1/2, SDK semantics). Fail-closed: NotImplemented/0 baseline, upgraded
// only when a real per-GPU probe confirms support — see D1 of
// encoder-quality-features-spec.md (never an architecture/name heuristic).
struct BFrameCapability {
    SupportAnnotation annotation;
    int max_bframes = 0;
    int bframe_ref_mode = 0;
};
```

Then inside `struct CapabilitySet { ... }`, add after the `chroma444` map declaration and before `combo_overrides` (currently lines 64-66):

```cpp
    // Per-codec NVENC advanced-encode capabilities (B-frames, Lookahead,
    // Temporal-AQ). Fail-closed baseline (NotImplemented/0 for every codec);
    // a real probe upgrades exactly the codecs the GPU advertised support for.
    // Not part of QueryCombo — these are Expert-setting capabilities, not
    // container/codec/audio/chroma/bitdepth combo dimensions.
    std::unordered_map<VideoCodec, BFrameCapability> bframe_capability;
    std::unordered_map<VideoCodec, SupportAnnotation> lookahead;
    std::unordered_map<VideoCodec, SupportAnnotation> temporal_aq;

```

Then add after the `QueryChroma444` declaration (currently line 85, before `QueryRateControlMode`):

```cpp
    // NVENC B-frame capability for `v`: max B-frames the GPU/driver reports
    // plus the B-ref-mode capability bit. NotImplemented/0 when unprobed or
    // when this GPU reported zero max B-frames for the codec (e.g. AV1 on
    // many current-generation NVENC parts).
    BFrameCapability QueryBFrames(VideoCodec v) const;

    // NVENC Lookahead / Temporal-AQ capability for `v`. NotImplemented when
    // unprobed or when the GPU/driver does not report the feature.
    SupportAnnotation QueryLookahead(VideoCodec v) const;
    SupportAnnotation QueryTemporalAq(VideoCodec v) const;
```

- [ ] **Step 4: Implement the accessors in `capability_set.cpp`**

In `libs/capability/src/capability_set.cpp`, add after `CapabilitySet::QueryChroma444` (currently lines 171-173):

```cpp

BFrameCapability CapabilitySet::QueryBFrames(VideoCodec v) const {
    const auto it = bframe_capability.find(v);
    if (it == bframe_capability.end()) {
        return BFrameCapability{SupportAnnotation{SupportLevel::Invalid, "no B-frame capability data for this codec"},
                                0, 0};
    }
    return it->second;
}

SupportAnnotation CapabilitySet::QueryLookahead(VideoCodec v) const {
    return LookupAnnotation(lookahead, v, "lookahead");
}

SupportAnnotation CapabilitySet::QueryTemporalAq(VideoCodec v) const {
    return LookupAnnotation(temporal_aq, v, "temporal AQ");
}
```

- [ ] **Step 5: Add the static baseline in `capability_builder.cpp`**

In `libs/capability/src/capability_builder.cpp`, inside `BuildStaticValidatedBaseline()`, add immediately after the `chroma444` block (currently lines 61-71):

```cpp

    for (VideoCodec codec : AllVideoCodecs()) {
        caps.bframe_capability.emplace(
            codec, BFrameCapability{SupportAnnotation{SupportLevel::NotImplemented,
                                                       "B-frame support not yet probed on this hardware."},
                                    0, 0});
        caps.lookahead.emplace(codec, SupportAnnotation{SupportLevel::NotImplemented,
                                                        "Lookahead support not yet probed on this hardware."});
        caps.temporal_aq.emplace(codec, SupportAnnotation{SupportLevel::NotImplemented,
                                                          "Temporal-AQ support not yet probed on this hardware."});
    }
```

(`AllVideoCodecs()` is declared in `libs/capability/include/capability/config_types.h:30-32`, already included transitively via `capability_set.h`.)

- [ ] **Step 6: Add and wire `ApplyNvencAdvancedEncodeSupport`**

In `libs/capability/include/capability/capability_builder.h`, add after the `ApplyNvencYuv444Support` declaration (currently line 54):

```cpp

// Pure refinement: when a real per-GPU NVENC probe ran (facts.nvenc_codec_probed),
// upgrade per-codec B-frame/Lookahead/Temporal-AQ support FROM the fail-closed
// NotImplemented/0 baseline TO whatever the GPU+driver actually advertised
// (NvidiaRuntimeFacts::nvenc_adv_h264/_hevc/_av1). Unlike ApplyNvencYuv444Support,
// which downgrades an optimistic baseline, this upgrades a pessimistic one — these
// are generation-dependent NVENC features (see D1: never an architecture/name
// heuristic), so an unprobed or unadvertised codec must never claim them. When the
// probe did not run, or a codec was not advertised at all, the baseline is left
// untouched. Called by BuildEffectiveCapabilities. Exposed for unit testing.
void ApplyNvencAdvancedEncodeSupport(CapabilitySet& caps, const NvidiaRuntimeFacts& facts);
```

In `libs/capability/src/capability_builder.cpp`, add the implementation after `ApplyNvencYuv444Support` (currently ends line 246):

```cpp

void ApplyNvencAdvancedEncodeSupport(CapabilitySet& caps, const NvidiaRuntimeFacts& facts) {
    if (!facts.nvenc_codec_probed) {
        return; // fail-closed baseline stands — no real probe ran
    }
    auto apply = [&caps](VideoCodec codec, bool advertised, const NvencAdvancedEncodeFacts& adv) {
        if (!advertised) {
            return; // codec not advertised at all -> leave NotImplemented/0 baseline
        }
        caps.bframe_capability[codec] = BFrameCapability{
            SupportAnnotation{adv.max_bframes > 0 ? SupportLevel::ValidUnvalidated : SupportLevel::NotImplemented,
                              adv.max_bframes > 0
                                  ? "Probed on this GPU/driver; not yet validated by ExoSnap's own encode path."
                                  : "GPU/driver reports 0 max B-frames for this codec."},
            adv.max_bframes, adv.bframe_ref_mode};
        caps.lookahead[codec] = SupportAnnotation{
            adv.lookahead ? SupportLevel::ValidUnvalidated : SupportLevel::NotImplemented,
            adv.lookahead ? "Probed on this GPU/driver; not yet validated by ExoSnap's own encode path."
                          : "GPU/driver does not report lookahead support for this codec."};
        caps.temporal_aq[codec] = SupportAnnotation{
            adv.temporal_aq ? SupportLevel::ValidUnvalidated : SupportLevel::NotImplemented,
            adv.temporal_aq ? "Probed on this GPU/driver; not yet validated by ExoSnap's own encode path."
                            : "GPU/driver does not report Temporal-AQ support for this codec."};
    };
    apply(VideoCodec::H264Nvenc, facts.nvenc_h264, facts.nvenc_adv_h264);
    apply(VideoCodec::HevcNvenc, facts.nvenc_hevc, facts.nvenc_adv_hevc);
    apply(VideoCodec::Av1Nvenc, facts.nvenc_av1, facts.nvenc_adv_av1);
}
```

Finally, in `BuildEffectiveCapabilities` (currently lines 107-193), add the call after the two existing refinement calls (lines 189-190):

```cpp
    ApplyNvencCodecSupport(caps, snapshot.nvidia);
    ApplyNvencYuv444Support(caps, snapshot.nvidia);
    ApplyNvencAdvancedEncodeSupport(caps, snapshot.nvidia);
```

- [ ] **Step 7: Run tests to verify they pass**

Run: `cmake --build build/windows-x64-debug --target capability_tests --config Debug && ctest --test-dir build/windows-x64-debug -R "NvencAdvancedEncodeSupportTest" --output-on-failure`
Expected: PASS, all 4 new tests green.

- [ ] **Step 8: Full capability suite green**

Run: `ctest --test-dir build/windows-x64-debug -R "Capability|Nvenc" --output-on-failure`
Expected: PASS, no regressions.

- [ ] **Step 9: Commit**

```bash
git add libs/capability/include/capability/capability_set.h \
        libs/capability/src/capability_set.cpp \
        libs/capability/include/capability/capability_builder.h \
        libs/capability/src/capability_builder.cpp \
        libs/capability/tests/test_runtime_merge.cpp
git commit -m "feat(capability): resolve advanced-encode facts into fail-closed CapabilitySet queries"
```

---

### Task 3: Per-adapter probe — `AdapterEncoderCapability` (Device tab data source)

**Files:**
- Modify: `libs/capability/include/capability/adapter_capability.h:13-38` (`AdapterEncoderCapability`)
- Modify: `libs/capability/src/adapter_capability.cpp:88-152` (`ProbeNvencGuidsOnDevice`)
- Test: `libs/capability/tests/test_adapter_capability.cpp` (extend near line 55)

**Interfaces:**
- Consumes: nothing from Task 1/2 — this is a separate, parallel per-adapter probe path (same NVENC-session technique, targeted at one specific adapter by LUID instead of "the first NVIDIA adapter DXGI enumerates").
- Produces: 6 new `AdapterEncoderCapability` fields: `int max_bframes_h264/_hevc/_av1`, `bool lookahead_h264/_hevc/_av1` — Task 4 (DevicePage) consumes these directly. (Two-value pairs per codec — `bframe_ref_mode` and `temporal_aq` are omitted from the per-adapter struct; see Step 3 rationale below for why the Device tab only needs 3 of the 4 facts per codec.)

- [ ] **Step 1: Write the failing test**

In `libs/capability/tests/test_adapter_capability.cpp`, add after `TEST(AdapterEncoderCapability, DefaultConstructionHasNoYuv444Support)` (currently lines 55-59):

```cpp

TEST(AdapterEncoderCapability, DefaultConstructionHasNoAdvancedEncodeSupport) {
    AdapterEncoderCapability cap;
    EXPECT_EQ(cap.max_bframes_h264, 0);
    EXPECT_EQ(cap.max_bframes_hevc, 0);
    EXPECT_EQ(cap.max_bframes_av1, 0);
    EXPECT_FALSE(cap.lookahead_h264);
    EXPECT_FALSE(cap.lookahead_hevc);
    EXPECT_FALSE(cap.lookahead_av1);
    EXPECT_FALSE(cap.temporal_aq_h264);
    EXPECT_FALSE(cap.temporal_aq_hevc);
    EXPECT_FALSE(cap.temporal_aq_av1);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build/windows-x64-debug --target capability_tests --config Debug`
Expected: FAIL — compile error, `'class exosnap::capability::AdapterEncoderCapability' has no member named 'max_bframes_h264'`

- [ ] **Step 3: Add the fields to `AdapterEncoderCapability`**

In `libs/capability/include/capability/adapter_capability.h`, add inside the struct (currently lines 13-38) after `bool yuv444_hevc = false;`:

```cpp
    // Per-codec NVENC advanced-encode capability, same semantics as the
    // system-wide NvencAdvancedEncodeFacts (runtime_snapshot.h) but scoped to
    // THIS adapter. bframe_ref_mode is intentionally omitted here — the Device
    // tab shows a max-B-frames number and on/off chips, not the raw SDK
    // ref-mode bit (that detail belongs to the Expert-setting resolver in a
    // later spec step, not this informational matrix).
    int max_bframes_h264 = 0;
    int max_bframes_hevc = 0;
    int max_bframes_av1 = 0;
    bool lookahead_h264 = false;
    bool lookahead_hevc = false;
    bool lookahead_av1 = false;
    bool temporal_aq_h264 = false;
    bool temporal_aq_hevc = false;
    bool temporal_aq_av1 = false;
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build/windows-x64-debug --target capability_tests --config Debug && ctest --test-dir build/windows-x64-debug -R "AdapterEncoderCapability.DefaultConstructionHasNoAdvancedEncodeSupport" --output-on-failure`
Expected: PASS

- [ ] **Step 5: Wire the real probe in `ProbeNvencGuidsOnDevice`**

In `libs/capability/src/adapter_capability.cpp`, inside `ProbeNvencGuidsOnDevice` (lines 88-152), the existing YUV444 block (lines 134-146) reads:

```cpp
            if (funcs.nvEncGetEncodeCaps != nullptr) {
                auto query_yuv444 = [&funcs, encoder](const GUID& codec) -> bool {
                    NV_ENC_CAPS_PARAM capsParam{};
                    capsParam.version = NV_ENC_CAPS_PARAM_VER;
                    capsParam.capsToQuery = NV_ENC_CAPS_SUPPORT_YUV444_ENCODE;
                    int value = 0;
                    return funcs.nvEncGetEncodeCaps(encoder, codec, &capsParam, &value) == NV_ENC_SUCCESS && value != 0;
                };
                if (out.h264)
                    out.yuv444_h264 = query_yuv444(NV_ENC_CODEC_H264_GUID);
                if (out.hevc)
                    out.yuv444_hevc = query_yuv444(NV_ENC_CODEC_HEVC_GUID);
            }
```

Replace it with (adds the advanced-encode query for all three codecs, including AV1 — unlike YUV444, which has no AV1 path):

```cpp
            if (funcs.nvEncGetEncodeCaps != nullptr) {
                auto query_yuv444 = [&funcs, encoder](const GUID& codec) -> bool {
                    NV_ENC_CAPS_PARAM capsParam{};
                    capsParam.version = NV_ENC_CAPS_PARAM_VER;
                    capsParam.capsToQuery = NV_ENC_CAPS_SUPPORT_YUV444_ENCODE;
                    int value = 0;
                    return funcs.nvEncGetEncodeCaps(encoder, codec, &capsParam, &value) == NV_ENC_SUCCESS && value != 0;
                };
                if (out.h264)
                    out.yuv444_h264 = query_yuv444(NV_ENC_CODEC_H264_GUID);
                if (out.hevc)
                    out.yuv444_hevc = query_yuv444(NV_ENC_CODEC_HEVC_GUID);

                auto query_cap = [&funcs, encoder](const GUID& codec, NV_ENC_CAPS cap) -> int {
                    NV_ENC_CAPS_PARAM capsParam{};
                    capsParam.version = NV_ENC_CAPS_PARAM_VER;
                    capsParam.capsToQuery = cap;
                    int value = 0;
                    return funcs.nvEncGetEncodeCaps(encoder, codec, &capsParam, &value) == NV_ENC_SUCCESS ? value : 0;
                };
                if (out.h264) {
                    out.max_bframes_h264 = query_cap(NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS_NUM_MAX_BFRAMES);
                    out.lookahead_h264 = query_cap(NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;
                    out.temporal_aq_h264 = query_cap(NV_ENC_CODEC_H264_GUID, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) != 0;
                }
                if (out.hevc) {
                    out.max_bframes_hevc = query_cap(NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_NUM_MAX_BFRAMES);
                    out.lookahead_hevc = query_cap(NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;
                    out.temporal_aq_hevc = query_cap(NV_ENC_CODEC_HEVC_GUID, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) != 0;
                }
                if (out.av1) {
                    out.max_bframes_av1 = query_cap(NV_ENC_CODEC_AV1_GUID, NV_ENC_CAPS_NUM_MAX_BFRAMES);
                    out.lookahead_av1 = query_cap(NV_ENC_CODEC_AV1_GUID, NV_ENC_CAPS_SUPPORT_LOOKAHEAD) != 0;
                    out.temporal_aq_av1 = query_cap(NV_ENC_CODEC_AV1_GUID, NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ) != 0;
                }
            }
```

**No CI test exists for this step**, same reasoning as Task 1 Step 7 — real NVENC hardware session, no seam for unit testing. Verified manually in Task 4 once the Device tab renders real numbers.

- [ ] **Step 6: Full capability suite green**

Run: `ctest --test-dir build/windows-x64-debug -R "Capability|Adapter" --output-on-failure`
Expected: PASS, no regressions.

- [ ] **Step 7: Commit**

```bash
git add libs/capability/include/capability/adapter_capability.h \
        libs/capability/src/adapter_capability.cpp \
        libs/capability/tests/test_adapter_capability.cpp
git commit -m "feat(capability): probe per-adapter NVENC advanced-encode caps for the Device tab"
```

---

### Task 4: Device tab — three new quiet fact rows

**Files:**
- Modify: `app/pages/DevicePage.cpp:127-165` (new row-builder function, alongside `make444Row`)
- Modify: `app/pages/DevicePage.cpp:680-691` (call site, inside the `if (cap.probed) { ... }` block)
- Test: `app/tests/test_device_page.cpp` (extend near line 272)

**Interfaces:**
- Consumes: `AdapterEncoderCapability::max_bframes_h264/_hevc/_av1`, `lookahead_h264/_hevc/_av1`, `temporal_aq_h264/_hevc/_av1` (Task 3), plus the pre-existing `cap.h264/.hevc/.av1` advertised-codec flags.
- Produces: three new rows in the Device tab's per-adapter feature matrix, each with `objectName("diagTableRow")` and a distinct chip `objectName` per feature (`"deviceBFramesChip"`, `"deviceLookaheadChip"`, `"deviceTemporalAqChip"`) for widget-test `findChildren` lookups.

- [ ] **Step 1: Write the failing widget test**

In `app/tests/test_device_page.cpp`, add after `MatrixShowsPerAdapter444SupportPerCodec` (currently ends around line 307, right before `MatrixOmitsChip...` continues — insert as its own new `TEST_F` after that test group, following the same style as lines 272-338):

```cpp

TEST_F(DevicePageTest, MatrixShowsPerAdapterAdvancedEncodeSupportPerCodec) {
    DevicePage page;
    page.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());

    auto nvcap = MakeProbedNvencCap(true, true, true); // h264, hevc, av1 all advertised
    nvcap.max_bframes_h264 = 3;
    nvcap.lookahead_h264 = true;
    nvcap.temporal_aq_h264 = true;
    nvcap.max_bframes_av1 = 0; // this GPU advertises AV1 but with no B-frame support
    nvcap.lookahead_av1 = false;
    page.setAdaptersForTest(
        {MakeAdapter("GeForce RTX 4070", capability::AdapterVendor::Nvidia, capability::AdapterKind::Discrete, 1),
         MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {nvcap, MakeUnwiredCap()});
    FlushDeferredDeletes();

    const auto bframe_chips = page.findChildren<QFrame*>(QStringLiteral("deviceBFramesChip"));
    ASSERT_EQ(bframe_chips.size(), 3); // one per advertised codec: h264, hevc, av1
    const auto lookahead_chips = page.findChildren<QFrame*>(QStringLiteral("deviceLookaheadChip"));
    ASSERT_EQ(lookahead_chips.size(), 3);

    bool found_h264_available = false;
    bool found_av1_unavailable = false;
    for (const QFrame* chip : bframe_chips) {
        QString name;
        for (const auto* label : chip->findChildren<QLabel*>()) {
            if (!label->text().isEmpty()) {
                name = label->text();
                break;
            }
        }
        const QString state = chip->property("chipState").toString();
        if (name == QStringLiteral("H.264") && state == QStringLiteral("available"))
            found_h264_available = true;
        if (name == QStringLiteral("AV1") && state == QStringLiteral("unavailable"))
            found_av1_unavailable = true;
    }
    EXPECT_TRUE(found_h264_available);
    EXPECT_TRUE(found_av1_unavailable);
}

// An unprobed adapter must not fabricate advanced-encode chips either.
TEST_F(DevicePageTest, UnprobedAdapterShowsNoAdvancedEncodeChips) {
    DevicePage page;
    page.setCapabilitySet(capability::CapabilityBuilder::BuildStaticValidatedBaseline());
    page.setAdaptersForTest(
        {MakeAdapter("UHD Graphics 770", capability::AdapterVendor::Intel, capability::AdapterKind::Integrated, 2)},
        {MakeUnwiredCap()});
    FlushDeferredDeletes();

    EXPECT_TRUE(page.findChildren<QFrame*>(QStringLiteral("deviceBFramesChip")).isEmpty());
    EXPECT_TRUE(page.findChildren<QFrame*>(QStringLiteral("deviceLookaheadChip")).isEmpty());
    EXPECT_TRUE(page.findChildren<QFrame*>(QStringLiteral("deviceTemporalAqChip")).isEmpty());
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build/windows-x64-debug --target exosnap_tests --config Debug && ctest --test-dir build/windows-x64-debug -R "MatrixShowsPerAdapterAdvancedEncodeSupportPerCodec|UnprobedAdapterShowsNoAdvancedEncodeChips" --output-on-failure`
Expected: FAIL — `deviceBFramesChip`/`deviceLookaheadChip`/`deviceTemporalAqChip` never appear, `ASSERT_EQ(bframe_chips.size(), 3)` fails with actual size 0.

- [ ] **Step 3: Add the row-builder functions in `DevicePage.cpp`**

In `app/pages/DevicePage.cpp`, add after `make444Row` (currently ends line 165, before the closing `} // namespace` on line 167):

```cpp

// Per-adapter NVENC B-frames-max / Lookahead / Temporal-AQ, shown as one chip
// per codec the adapter advertises (mirrors make444Row's honesty rule: a
// codec this adapter never advertised at all gets no chip, positive or
// negative — the codec-chip row above already states its absence). B-frames
// shows the max count in the chip label instead of a bare available/unavailable
// state, since "how many" is the actually useful fact for this feature.
QWidget* makeBFramesRow(bool h264_advertised, int h264_max, bool hevc_advertised, int hevc_max, bool av1_advertised,
                        int av1_max, QWidget* parent, bool first_row) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("diagTableRow"));
    row->setProperty("firstRow", first_row);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(M::kSpaceSm, M::kSpaceSm, M::kSpaceSm, M::kSpaceSm);
    row_layout->setSpacing(M::kSpaceMd);

    auto* name_label = new QLabel(QStringLiteral("B-frames (max)"), row);
    name_label->setProperty("labelRole", "body");
    name_label->setMinimumWidth(160);
    row_layout->addWidget(name_label);

    row_layout->addStretch(1);
    const QString chip = QStringLiteral("deviceBFramesChip");
    auto add_chip = [&](bool advertised, capability::VideoCodec codec, int max_bframes) {
        if (!advertised)
            return;
        const QString label = QStringLiteral("%1 (%2)").arg(ui::videoCodecLabel(codec)).arg(max_bframes);
        row_layout->addWidget(makeCodecChip(label, max_bframes > 0, row, chip));
    };
    add_chip(h264_advertised, capability::VideoCodec::H264Nvenc, h264_max);
    add_chip(hevc_advertised, capability::VideoCodec::HevcNvenc, hevc_max);
    add_chip(av1_advertised, capability::VideoCodec::Av1Nvenc, av1_max);

    return row;
}

// Shared shape for the Lookahead and Temporal-AQ rows: a plain on/off chip per
// advertised codec, same honesty rule as makeBFramesRow/make444Row.
QWidget* makeAdvancedEncodeToggleRow(const QString& label, const QString& object_name, bool h264_advertised,
                                     bool h264_on, bool hevc_advertised, bool hevc_on, bool av1_advertised,
                                     bool av1_on, QWidget* parent, bool first_row) {
    auto* row = new QWidget(parent);
    row->setObjectName(QStringLiteral("diagTableRow"));
    row->setProperty("firstRow", first_row);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(M::kSpaceSm, M::kSpaceSm, M::kSpaceSm, M::kSpaceSm);
    row_layout->setSpacing(M::kSpaceMd);

    auto* name_label = new QLabel(label, row);
    name_label->setProperty("labelRole", "body");
    name_label->setMinimumWidth(160);
    row_layout->addWidget(name_label);

    row_layout->addStretch(1);
    auto add_chip = [&](bool advertised, capability::VideoCodec codec, bool on) {
        if (!advertised)
            return;
        row_layout->addWidget(makeCodecChip(ui::videoCodecLabel(codec), on, row, object_name));
    };
    add_chip(h264_advertised, capability::VideoCodec::H264Nvenc, h264_on);
    add_chip(hevc_advertised, capability::VideoCodec::HevcNvenc, hevc_on);
    add_chip(av1_advertised, capability::VideoCodec::Av1Nvenc, av1_on);

    return row;
}
```

- [ ] **Step 4: Wire the call site**

In `app/pages/DevicePage.cpp`, replace the block ending the `if (cap.probed) { ... }` branch (currently lines 680-686):

```cpp
        // Per-adapter 8-bit 4:4:4 (YUV444) encode support, from THIS adapter's
        // probe (cap.yuv444_h264 / cap.yuv444_hevc). Shown per codec that can
        // carry 4:4:4 AND that this adapter advertises at all (cap.h264 /
        // cap.hevc); NVENC AV1 is 4:2:0 only, so it has no chip here.
        feature_rows_layout_->addWidget(make444Row(cap.h264, cap.yuv444_h264, cap.hevc, cap.yuv444_hevc,
                                                   feature_rows_layout_->parentWidget(), first_row));
        first_row = false;
```

with:

```cpp
        // Per-adapter 8-bit 4:4:4 (YUV444) encode support, from THIS adapter's
        // probe (cap.yuv444_h264 / cap.yuv444_hevc). Shown per codec that can
        // carry 4:4:4 AND that this adapter advertises at all (cap.h264 /
        // cap.hevc); NVENC AV1 is 4:2:0 only, so it has no chip here.
        feature_rows_layout_->addWidget(make444Row(cap.h264, cap.yuv444_h264, cap.hevc, cap.yuv444_hevc,
                                                   feature_rows_layout_->parentWidget(), first_row));
        first_row = false;

        // Per-adapter NVENC advanced-encode capabilities (S1 of
        // encoder-quality-features-spec.md): informational only, no Expert
        // control reads these yet (that is a later spec step, gated on the
        // engine actually applying the features).
        feature_rows_layout_->addWidget(makeBFramesRow(cap.h264, cap.max_bframes_h264, cap.hevc, cap.max_bframes_hevc,
                                                       cap.av1, cap.max_bframes_av1,
                                                       feature_rows_layout_->parentWidget(), first_row));
        first_row = false;
        feature_rows_layout_->addWidget(makeAdvancedEncodeToggleRow(
            QStringLiteral("Lookahead"), QStringLiteral("deviceLookaheadChip"), cap.h264, cap.lookahead_h264, cap.hevc,
            cap.lookahead_hevc, cap.av1, cap.lookahead_av1, feature_rows_layout_->parentWidget(), first_row));
        first_row = false;
        feature_rows_layout_->addWidget(makeAdvancedEncodeToggleRow(
            QStringLiteral("Temporal AQ"), QStringLiteral("deviceTemporalAqChip"), cap.h264, cap.temporal_aq_h264,
            cap.hevc, cap.temporal_aq_hevc, cap.av1, cap.temporal_aq_av1, feature_rows_layout_->parentWidget(),
            first_row));
        first_row = false;
```

- [ ] **Step 5: Run the new tests to verify they pass**

Run: `cmake --build build/windows-x64-debug --target exosnap_tests --config Debug && ctest --test-dir build/windows-x64-debug -R "MatrixShowsPerAdapterAdvancedEncodeSupportPerCodec|UnprobedAdapterShowsNoAdvancedEncodeChips" --output-on-failure`
Expected: PASS

- [ ] **Step 6: Full DevicePage + capability suite green**

Run: `ctest --test-dir build/windows-x64-debug -R "DevicePage|Capability" --output-on-failure`
Expected: PASS, no regressions (in particular `MatrixFeatureRowsAreLabeledSystemWideForProbedAdapter` and the existing 444-row tests must still pass unchanged).

- [ ] **Step 7: Visual proof (QSS/theme risk)**

Per CLAUDE.md, any UI change must be judged on rendered pixels, not QSS values. Run the project's `--visual-test` render harness against the Device tab with a probed NVIDIA adapter fixture to confirm the three new rows render correctly in both light/dark theme and don't clip (`wordWrap`/minimum-height risk noted in project memory) — find the exact harness invocation via the project's existing visual-test scripts/docs (e.g. `docs/development/` or `.workspace/`) rather than guessing a command here.

- [ ] **Step 8: Startup smoke check**

Since this touches a QSS-adjacent widget tree (new `objectName`s consumed by stylesheet selectors elsewhere), start the built app once to confirm it does not crash at launch (an invalid `${token}` reference crashes at startup), then close it — per CLAUDE.md's "Never drive the running application" exception for a single startup-survival check.

- [ ] **Step 9: Commit**

```bash
git add app/pages/DevicePage.cpp app/tests/test_device_page.cpp
git commit -m "feat(device-page): show per-adapter NVENC B-frames/lookahead/temporal-AQ capability"
```

---

## Test-/Verify-Plan

### CI-fähig
- Task 1: `CapabilityCacheStoreTest.*` (disk-cache round-trip of the 3 new fact structs).
- Task 2: `NvencAdvancedEncodeSupportTest.*` (4 tests: fail-closed baseline, upgrade-on-probe, unadvertised-codec-stays-baseline, not-probed-stays-baseline).
- Task 3: `AdapterEncoderCapability.DefaultConstructionHasNoAdvancedEncodeSupport`.
- Task 4: `DevicePageTest.MatrixShowsPerAdapterAdvancedEncodeSupportPerCodec`, `DevicePageTest.UnprobedAdapterShowsNoAdvancedEncodeChips`, plus the full existing `DevicePage`/`Capability` suites for regressions.

### Not CI-testable (documented, not silently skipped)
- `ProbeNvencCodecs`'s new query block (Task 1 Step 7) and `ProbeNvencGuidsOnDevice`'s new query block (Task 3 Step 5): both open a real NVENC session — no unit-test seam exists for either in this codebase today (same as the pre-existing YUV444 probe blocks they sit beside). Verified manually, once, on a real NVIDIA GPU by checking the Device tab shows real (non-zero/non-baseline) numbers after Task 4 lands.

### User-live
- None required for this plan specifically — no behavior change reaches recording. (The broader spec's user-live verify items — playback matrices, HDR+B-frames, etc. — belong to S6/S7, not S1.)

---

## Out of Scope (do not implement here)

- Any encoder wiring (`nvenc_encoder.cpp`/`.h`, `FetchPresetConfig`) — S4/S6/S7.
- Expert-setting UI controls (`ConfigPage.cpp`) for B-Frames/Lookahead/Temporal-AQ — S5, and only as hard-disabled "planned" rows per the spec's staging rule.
- `probe_encode_file` / `encoder_quality_matrix.py` — S2/S3, separate plan.
- Resolver clamp rules consuming `QueryBFrames`/`QueryLookahead`/`QueryTemporalAq` — S5, separate plan (these accessors exist now so S5 has something to call, but nothing calls them yet after this plan).

#include <gtest/gtest.h>

#include <capability/capability_builder.h>
#include <capability/capability_set.h>
#include <capability/config_types.h>
#include <capability/runtime_snapshot.h>
#include <capability/support_level.h>

#include <string>

namespace exosnap::capability {
namespace {

// Helper: build a fully-favorable synthetic snapshot.
RuntimeCapabilitySnapshot MakeFavorableSnapshot() {
    RuntimeCapabilitySnapshot snap;
    snap.nvidia.nvenc_dll_present = true;
    snap.nvidia.nvenc_api_version_valid = true;
    snap.nvidia.nvenc_api_version = 0x000D0000u; // dummy version
    snap.nvidia.adapter_name = "NVIDIA GeForce RTX 9999 (synthetic)";
    snap.mf_webcam.available = true; // S4: MF present in favorable snapshot
    snap.os.build_number = 26100u;
    snap.os.version_string = "10.0.26100";
    return snap;
}

// Shorthand for the primary M3.2 combo key.
constexpr Container kC = Container::Matroska;
constexpr VideoCodec kV = VideoCodec::Av1;
constexpr AudioCodec kA = AudioCodec::Aac;
constexpr ChromaSubsampling kCS = ChromaSubsampling::Cs420;
constexpr BitDepth kBD = BitDepth::Bit8;

// -------------------------------------------------------------------------
// TC-1: All runtime prerequisites present keeps M3.2 combo Available
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC1_AllPrerequisitesPresentKeepM32ComboAvailable) {
    const RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    // M3.2 primary combo must remain Available.
    const SupportAnnotation combo = caps.QueryCombo(kC, kV, kA, kCS, kBD);
    EXPECT_EQ(combo.level, SupportLevel::Available)
        << "M3.2 combo should be Available when NVENC and AAC are both present. reason: " << combo.reason;

    // AV1/NVENC dimension must be selectable.
    EXPECT_TRUE(IsSelectable(caps.QueryVideoCodec(VideoCodec::Av1))) << "VideoCodec::Av1 should be selectable.";

    // Aac dimension must be selectable.
    EXPECT_TRUE(IsSelectable(caps.QueryAudioCodec(AudioCodec::Aac))) << "AudioCodec::Aac should be selectable.";
}

// -------------------------------------------------------------------------
// TC-2: NVENC DLL missing blocks AV1 path
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC2_NvencDllMissingBlocksAv1Path) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_dll_present = false;
    snap.nvidia.nvenc_api_version_valid = false;

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    // AV1 dimension must not be selectable.
    const SupportAnnotation av1 = caps.QueryVideoCodec(VideoCodec::Av1);
    EXPECT_FALSE(IsSelectable(av1)) << "VideoCodec::Av1 must not be selectable when NVENC DLL is absent.";

    // M3.2 primary combo must not be selectable.
    const SupportAnnotation combo = caps.QueryCombo(kC, kV, kA, kCS, kBD);
    EXPECT_FALSE(IsSelectable(combo)) << "M3.2 combo must not be selectable when NVENC DLL is absent.";

    // Reason must mention NVENC.
    const bool mentions_nvenc =
        (av1.reason.find("NVENC") != std::string::npos) || (combo.reason.find("NVENC") != std::string::npos);
    EXPECT_TRUE(mentions_nvenc) << "Downgrade reason should mention NVENC. av1.reason='" << av1.reason
                                << "' combo.reason='" << combo.reason << "'";
}

// -------------------------------------------------------------------------
// TC-3: NVENC API version unavailable blocks AV1 path
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC3_NvencApiVersionUnavailableBlocksAv1Path) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_dll_present = true;        // DLL loads
    snap.nvidia.nvenc_api_version_valid = false; // but API version call fails

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    // AV1 dimension must not be selectable.
    const SupportAnnotation av1 = caps.QueryVideoCodec(VideoCodec::Av1);
    EXPECT_FALSE(IsSelectable(av1)) << "VideoCodec::Av1 must not be selectable when NVENC API version is invalid.";

    // M3.2 combo must not be selectable.
    const SupportAnnotation combo = caps.QueryCombo(kC, kV, kA, kCS, kBD);
    EXPECT_FALSE(IsSelectable(combo)) << "M3.2 combo must not be selectable when NVENC API version is invalid.";

    // Reason must mention NVENC (API/version).
    const bool mentions_nvenc =
        (av1.reason.find("NVENC") != std::string::npos) || (combo.reason.find("NVENC") != std::string::npos);
    EXPECT_TRUE(mentions_nvenc) << "Downgrade reason should mention NVENC/API/version. av1.reason='" << av1.reason
                                << "' combo.reason='" << combo.reason << "'";
}

// -------------------------------------------------------------------------
// TC-4/TC-5: retired. AAC used to have a Media Foundation runtime probe
// (MFTEnumEx + CLSID_AACMFTEncoder fallback) that could report AAC
// unavailable; these covered that downgrade and its CLSID fallback. Since
// ADR 0052, AAC is encoded by FFmpeg's bundled native AAC-LC encoder, which
// has no runtime-probeable failure mode — see TC-1, which now covers AAC
// staying Available unconditionally.
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// TC-6: H.264 is Available when NVENC is present (Phase 23E)
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC6_H264Available_WhenNvencPresent) {
    const RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    const SupportAnnotation h264 = caps.QueryVideoCodec(VideoCodec::H264);
    EXPECT_EQ(h264.level, SupportLevel::Available)
        << "H.264 must be Available when NVENC is present. reason: " << h264.reason;
    EXPECT_TRUE(IsSelectable(h264)) << "H.264 must be selectable when NVENC is present.";

    // MP4+H264+AAC primary combo must also be Available.
    const SupportAnnotation mp4_combo =
        caps.QueryCombo(Container::Mp4, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8);
    EXPECT_EQ(mp4_combo.level, SupportLevel::Available)
        << "MP4+H264+AAC must be Available when NVENC and AAC are present. reason: " << mp4_combo.reason;
}

TEST(RuntimeMergeTest, TC6b_H264NotImplemented_WhenNvencAbsent) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_dll_present = false;

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    const SupportAnnotation h264 = caps.QueryVideoCodec(VideoCodec::H264);
    EXPECT_EQ(h264.level, SupportLevel::NotImplemented) << "H.264 must be NotImplemented when NVENC DLL is absent.";
    EXPECT_FALSE(IsSelectable(h264));

    const SupportAnnotation mp4_combo =
        caps.QueryCombo(Container::Mp4, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8);
    EXPECT_FALSE(IsSelectable(mp4_combo)) << "MP4+H264+AAC must not be selectable when NVENC is absent.";
}

// -------------------------------------------------------------------------
// TC-7: HEVC is ValidUnvalidated (selectable with caveat) under favorable
// runtime (0.7.0: engine path implemented, not yet hardware-validated).
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC7_HevcValidUnvalidatedWhenNvencPresent) {
    const RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    const SupportAnnotation hevc = caps.QueryVideoCodec(VideoCodec::Hevc);
    EXPECT_EQ(hevc.level, SupportLevel::ValidUnvalidated)
        << "HEVC must be ValidUnvalidated when NVENC is present (implemented, not hw-validated).";
    EXPECT_TRUE(IsSelectable(hevc)) << "HEVC must be selectable (with caveat) when NVENC is present.";

    // The MKV + HEVC combo must be selectable end-to-end (registry Allowed +
    // dimension ValidUnvalidated must not be force-downgraded by the combine logic).
    const SupportAnnotation combo = caps.QueryCombo(Container::Matroska, VideoCodec::Hevc, AudioCodec::Opus,
                                                    ChromaSubsampling::Cs420, BitDepth::Bit8);
    EXPECT_EQ(combo.level, SupportLevel::ValidUnvalidated)
        << "MKV+HEVC+Opus combo must be ValidUnvalidated. reason: " << combo.reason;
    EXPECT_TRUE(IsSelectable(combo)) << "MKV+HEVC+Opus combo must be selectable.";
}

// -------------------------------------------------------------------------
// TC-7b: HEVC is downgraded to NotImplemented when NVENC is absent, exactly
// like AV1/H.264.
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC7b_HevcNotImplemented_WhenNvencAbsent) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_dll_present = false;

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    const SupportAnnotation hevc = caps.QueryVideoCodec(VideoCodec::Hevc);
    EXPECT_EQ(hevc.level, SupportLevel::NotImplemented) << "HEVC must be NotImplemented when NVENC DLL is absent.";
    EXPECT_FALSE(IsSelectable(hevc)) << "HEVC must not be selectable when NVENC is absent.";
    EXPECT_NE(hevc.reason.find("NVENC"), std::string::npos) << "HEVC downgrade reason must mention NVENC.";
}

// -------------------------------------------------------------------------
// TC-8: Runtime snapshot is preserved in the returned CapabilitySet
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC8_RuntimeSnapshotPreserved) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_api_version = 0xABCDEF01u;
    snap.os.build_number = 22621u;
    snap.os.version_string = "10.0.22621";

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    EXPECT_EQ(caps.runtime.nvidia.nvenc_dll_present, snap.nvidia.nvenc_dll_present);
    EXPECT_EQ(caps.runtime.nvidia.nvenc_api_version_valid, snap.nvidia.nvenc_api_version_valid);
    EXPECT_EQ(caps.runtime.nvidia.nvenc_api_version, snap.nvidia.nvenc_api_version);
    EXPECT_EQ(caps.runtime.nvidia.adapter_name, snap.nvidia.adapter_name);
    EXPECT_EQ(caps.runtime.os.build_number, snap.os.build_number);
    EXPECT_EQ(caps.runtime.os.version_string, snap.os.version_string);
}

// -------------------------------------------------------------------------
// TC-9: BuildFromHardwareQuery is callable — integration-style smoke test
// Does not assert machine-specific availability.
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC9_BuildFromHardwareQueryCallable) {
    // Must not throw.
    CapabilitySet caps;
    ASSERT_NO_THROW(caps = CapabilityBuilder::BuildFromHardwareQuery());

    // Returned object must be coherent: all expected dimension keys must be present.
    EXPECT_FALSE(caps.containers.empty()) << "containers map must be populated";
    EXPECT_FALSE(caps.video_codecs.empty()) << "video_codecs map must be populated";
    EXPECT_FALSE(caps.audio_codecs.empty()) << "audio_codecs map must be populated";
    EXPECT_FALSE(caps.chroma_modes.empty()) << "chroma_modes map must be populated";
    EXPECT_FALSE(caps.bit_depths.empty()) << "bit_depths map must be populated";

    // The M3.2 combo must at minimum be queryable (may or may not be Available
    // depending on whether this machine has NVENC and AAC).
    const SupportAnnotation combo = caps.QueryCombo(kC, kV, kA, kCS, kBD);
    // Just verifies we get a valid annotation without crashing.
    (void)combo;

    // H.264 availability depends on NVENC presence; just verify it's queryable.
    const SupportAnnotation h264 = caps.QueryVideoCodec(VideoCodec::H264);
    (void)h264;

    // HEVC availability is now hardware-dependent (ValidUnvalidated when NVENC is
    // present, NotImplemented when absent) — just verify it's queryable and never
    // hard-Invalid.
    const SupportAnnotation hevc = caps.QueryVideoCodec(VideoCodec::Hevc);
    EXPECT_FALSE(IsHardInvalid(hevc)) << "HEVC must be queryable (never hard-Invalid) after real hardware query.";
}

// -------------------------------------------------------------------------
// TC-10: NVENC downgrade reason is user-facing — no raw Win32 API symbols
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC10_NvencDowngradeReason_IsUserFacing) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_dll_present = false;
    snap.nvidia.nvenc_api_version_valid = false;
    snap.nvidia.failure_detail = "LoadLibraryW(nvEncodeAPI64.dll) failed, GetLastError=2";

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);
    const SupportAnnotation av1 = caps.QueryVideoCodec(VideoCodec::Av1);
    const SupportAnnotation h264 = caps.QueryVideoCodec(VideoCodec::H264);

    for (const SupportAnnotation* ann : {&av1, &h264}) {
        // Must not expose raw Win32 API names.
        EXPECT_EQ(ann->reason.find("LoadLibraryW"), std::string::npos) << "reason: " << ann->reason;
        EXPECT_EQ(ann->reason.find("GetLastError"), std::string::npos) << "reason: " << ann->reason;
        EXPECT_EQ(ann->reason.find("GetProcAddress"), std::string::npos) << "reason: " << ann->reason;
        // Must remain actionable and mention NVENC.
        EXPECT_NE(ann->reason.find("NVENC"), std::string::npos) << "reason: " << ann->reason;
        EXPECT_NE(ann->reason.find("driver"), std::string::npos) << "reason: " << ann->reason;
    }
}

// -------------------------------------------------------------------------
// TC-11: retired along with TC-4/TC-5 (see note above) — there is no AAC
// downgrade reason left to assert on. AAC's Available annotation (asserted
// in TC-1) does still mention "AAC" (BuildStaticValidatedBaseline), which
// covers the "actionable and mentions AAC" half of the old assertion.
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// S4: MF webcam capability gate tests
// -------------------------------------------------------------------------

// TC-S4-1: mf_webcam_available is true in the static validated baseline.
TEST(RuntimeMergeTest, TC_S4_1_MfWebcamAvailable_TrueInBaseline) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();
    EXPECT_TRUE(caps.mf_webcam_available) << "mf_webcam_available must be true in the static validated baseline.";
}

// TC-S4-2: mf_webcam_available propagates from the runtime snapshot (present).
TEST(RuntimeMergeTest, TC_S4_2_MfWebcamAvailable_PropagatesFromSnapshot_Present) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.mf_webcam.available = true;
    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);
    EXPECT_TRUE(caps.mf_webcam_available)
        << "mf_webcam_available must be true when snapshot.mf_webcam.available is true.";
}

// TC-S4-3: mf_webcam_available propagates from the runtime snapshot (absent / Windows-N).
TEST(RuntimeMergeTest, TC_S4_3_MfWebcamAvailable_PropagatesFromSnapshot_Absent) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.mf_webcam.available = false;
    snap.mf_webcam.failure_detail = "LoadLibraryW(mfplat.dll) failed, GetLastError=2";
    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);
    EXPECT_FALSE(caps.mf_webcam_available)
        << "mf_webcam_available must be false when snapshot.mf_webcam.available is false.";
}

// TC-S4-4: mf_webcam.available is preserved in the CapabilitySet runtime field.
TEST(RuntimeMergeTest, TC_S4_4_MfWebcamAvailable_PreservedInRuntimeField) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.mf_webcam.available = false;
    snap.mf_webcam.failure_detail = "synthetic-absent";
    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);
    EXPECT_FALSE(caps.runtime.mf_webcam.available);
    EXPECT_EQ(caps.runtime.mf_webcam.failure_detail, "synthetic-absent");
}

// TC-S4-5: BuildFromHardwareQuery includes a mf_webcam probe result (no-throw).
TEST(RuntimeMergeTest, TC_S4_5_BuildFromHardwareQuery_MfWebcamProbed) {
    CapabilitySet caps;
    ASSERT_NO_THROW(caps = CapabilityBuilder::BuildFromHardwareQuery());
    // On a normal Windows install mf_webcam_available is true; on Windows-N it
    // is false.  Either way the field must be coherent with the runtime snapshot.
    EXPECT_EQ(caps.mf_webcam_available, caps.runtime.mf_webcam.available)
        << "caps.mf_webcam_available must mirror caps.runtime.mf_webcam.available.";
}

// -------------------------------------------------------------------------
// Per-GPU NVENC codec-GUID refinement (ApplyNvencCodecSupport, pure)
// -------------------------------------------------------------------------

// Probed with AV1 unsupported (pre-Ada GPU) but HEVC/H264 supported:
// AV1 -> NotImplemented; HEVC/H264 keep their baseline annotations.
TEST(NvencCodecSupportTest, ProbedAv1Unsupported_DowngradesOnlyAv1) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_dll_present = true;
    facts.nvenc_api_version_valid = true;
    facts.nvenc_codec_probed = true;
    facts.nvenc_av1 = false; // GPU cannot do AV1
    facts.nvenc_hevc = true;
    facts.nvenc_h264 = true;

    ApplyNvencCodecSupport(caps, facts);

    EXPECT_EQ(caps.QueryVideoCodec(VideoCodec::Av1).level, SupportLevel::NotImplemented);
    EXPECT_FALSE(IsSelectable(caps.QueryVideoCodec(VideoCodec::Av1)));
    // HEVC stays ValidUnvalidated, H264 stays Available (baseline unchanged).
    EXPECT_EQ(caps.QueryVideoCodec(VideoCodec::Hevc).level, SupportLevel::ValidUnvalidated);
    EXPECT_EQ(caps.QueryVideoCodec(VideoCodec::H264).level, SupportLevel::Available);
    // The downgrade reason is user-facing and names the requirement.
    EXPECT_NE(caps.QueryVideoCodec(VideoCodec::Av1).reason.find("AV1"), std::string::npos);
}

// Probe did not run -> no change at all (graceful degrade keeps the baseline).
TEST(NvencCodecSupportTest, NotProbed_LeavesBaselineUntouched) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_dll_present = true;
    facts.nvenc_api_version_valid = true;
    facts.nvenc_codec_probed = false; // never probed
    facts.nvenc_av1 = false;          // ignored because not probed
    facts.nvenc_hevc = false;
    facts.nvenc_h264 = false;

    ApplyNvencCodecSupport(caps, facts);

    EXPECT_EQ(caps.QueryVideoCodec(VideoCodec::Av1).level, SupportLevel::Available);
    EXPECT_EQ(caps.QueryVideoCodec(VideoCodec::Hevc).level, SupportLevel::ValidUnvalidated);
    EXPECT_EQ(caps.QueryVideoCodec(VideoCodec::H264).level, SupportLevel::Available);
}

// All codecs supported -> nothing downgraded.
TEST(NvencCodecSupportTest, ProbedAllSupported_NoDowngrade) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_codec_probed = true;
    facts.nvenc_av1 = true;
    facts.nvenc_hevc = true;
    facts.nvenc_h264 = true;

    ApplyNvencCodecSupport(caps, facts);

    EXPECT_TRUE(IsSelectable(caps.QueryVideoCodec(VideoCodec::Av1)));
    EXPECT_TRUE(IsSelectable(caps.QueryVideoCodec(VideoCodec::Hevc)));
    EXPECT_TRUE(IsSelectable(caps.QueryVideoCodec(VideoCodec::H264)));
}

// 4:4:4: baseline advertises H.264/HEVC as ValidUnvalidated and AV1 as
// NotImplemented; a probe that reports HEVC-YUV444 unsupported downgrades only
// HEVC 4:4:4 while H.264 4:4:4 stays selectable.
TEST(NvencYuv444SupportTest, ProbedHevc444Unsupported_DowngradesOnlyHevc) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_codec_probed = true;
    facts.nvenc_h264 = true;
    facts.nvenc_hevc = true;
    facts.nvenc_yuv444_h264 = true;
    facts.nvenc_yuv444_hevc = false; // GPU cannot do HEVC 4:4:4

    ApplyNvencYuv444Support(caps, facts);

    EXPECT_TRUE(IsSelectable(caps.QueryChroma444(VideoCodec::H264)));
    EXPECT_FALSE(IsSelectable(caps.QueryChroma444(VideoCodec::Hevc)));
    // AV1 is never selectable for 4:4:4.
    EXPECT_FALSE(IsSelectable(caps.QueryChroma444(VideoCodec::Av1)));
}

// Probe did not run -> keep the ValidUnvalidated 4:4:4 baseline (H.264/HEVC).
TEST(NvencYuv444SupportTest, NotProbed_Keeps444Baseline) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_codec_probed = false;
    facts.nvenc_yuv444_h264 = false; // ignored because not probed
    facts.nvenc_yuv444_hevc = false;

    ApplyNvencYuv444Support(caps, facts);

    EXPECT_TRUE(IsSelectable(caps.QueryChroma444(VideoCodec::H264)));
    EXPECT_TRUE(IsSelectable(caps.QueryChroma444(VideoCodec::Hevc)));
}

// End-to-end: a probe reporting no 4:4:4 support at all makes every 4:4:4 combo
// non-selectable while 4:2:0 stays selectable.
TEST(NvencYuv444SupportTest, BuildEffective_NoYuv444_Blocks444KeepsCs420) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_codec_probed = true;
    snap.nvidia.nvenc_av1 = true;
    snap.nvidia.nvenc_hevc = true;
    snap.nvidia.nvenc_h264 = true;
    snap.nvidia.nvenc_yuv444_h264 = false;
    snap.nvidia.nvenc_yuv444_hevc = false;

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    EXPECT_FALSE(IsSelectable(
        caps.QueryCombo(Container::Mp4, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs444, BitDepth::Bit8)));
    EXPECT_FALSE(IsSelectable(caps.QueryCombo(Container::Matroska, VideoCodec::Hevc, AudioCodec::Aac,
                                              ChromaSubsampling::Cs444, BitDepth::Bit8)));
    EXPECT_TRUE(IsSelectable(
        caps.QueryCombo(Container::Mp4, VideoCodec::H264, AudioCodec::Aac, ChromaSubsampling::Cs420, BitDepth::Bit8)));
}

// Advanced-encode (B-frames/Lookahead/Temporal-AQ) baseline is fail-closed:
// with no probe, every codec reports NotImplemented/0 — the inverse of the
// chroma444 baseline, because these are generation-dependent features that
// must never be assumed available.
TEST(NvencAdvancedEncodeSupportTest, StaticBaselineIsNotImplementedForEveryCodec) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::H264).annotation));
    EXPECT_EQ(caps.QueryBFrames(VideoCodec::H264).max_bframes, 0);
    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::Av1).annotation));
    EXPECT_FALSE(IsSelectable(caps.QueryLookahead(VideoCodec::Hevc)));
    EXPECT_FALSE(IsSelectable(caps.QueryTemporalAq(VideoCodec::Hevc)));
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

    const auto h264 = caps.QueryBFrames(VideoCodec::H264);
    EXPECT_TRUE(IsSelectable(h264.annotation));
    EXPECT_EQ(h264.max_bframes, 2);
    EXPECT_EQ(h264.bframe_ref_mode, 1);
    EXPECT_TRUE(IsSelectable(caps.QueryLookahead(VideoCodec::H264)));
    EXPECT_TRUE(IsSelectable(caps.QueryTemporalAq(VideoCodec::H264)));

    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::Av1).annotation));
    EXPECT_EQ(caps.QueryBFrames(VideoCodec::Av1).max_bframes, 0);
}

// A codec the GPU never advertised at all (HEVC absent here) is left at the
// fail-closed baseline even though the probe ran — mirrors
// ApplyNvencYuv444Support's "not advertised -> untouched" rule.
TEST(NvencAdvancedEncodeSupportTest, CodecNotAdvertised_StaysAtBaseline) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_codec_probed = true;
    facts.nvenc_hevc = false;                  // this GPU does not advertise HEVC at all
    facts.nvenc_adv_hevc = {4, 2, true, true}; // must be ignored

    ApplyNvencAdvancedEncodeSupport(caps, facts);

    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::Hevc).annotation));
    EXPECT_EQ(caps.QueryBFrames(VideoCodec::Hevc).max_bframes, 0);
}

// Probe did not run -> the fail-closed baseline stands untouched.
TEST(NvencAdvancedEncodeSupportTest, NotProbed_KeepsFailClosedBaseline) {
    CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    NvidiaRuntimeFacts facts;
    facts.nvenc_codec_probed = false;
    facts.nvenc_h264 = true;
    facts.nvenc_adv_h264 = {2, 1, true, true}; // must be ignored, probe did not run

    ApplyNvencAdvancedEncodeSupport(caps, facts);

    EXPECT_FALSE(IsSelectable(caps.QueryBFrames(VideoCodec::H264).annotation));
}

// BuildEffectiveCapabilities wires the probe through end-to-end: a favorable
// snapshot that additionally reports AV1-unsupported via a real probe must yield
// AV1 NotImplemented while the M3.2 prerequisites otherwise hold.
TEST(NvencCodecSupportTest, BuildEffective_HonorsProbeFlags) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_codec_probed = true;
    snap.nvidia.nvenc_av1 = false; // GPU does not encode AV1
    snap.nvidia.nvenc_hevc = true;
    snap.nvidia.nvenc_h264 = true;

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    EXPECT_FALSE(IsSelectable(caps.QueryVideoCodec(VideoCodec::Av1)))
        << "AV1 must be downgraded when the GPU probe reports it unsupported.";
    EXPECT_TRUE(IsSelectable(caps.QueryVideoCodec(VideoCodec::Hevc)));
    EXPECT_TRUE(IsSelectable(caps.QueryVideoCodec(VideoCodec::H264)));
}

// -------------------------------------------------------------------------
// Explicit per-codec HDR10-native (10-bit / P010) capability.
// HEVC and AV1 carry HDR10; H.264 does not. This is a codec-format fact, not a
// per-GPU probe (see the inference note in capability_builder.cpp).
// -------------------------------------------------------------------------
TEST(Hdr10CapabilityTest, HevcAndAv1AreHdr10Native_H264IsNot) {
    const CapabilitySet caps = CapabilityBuilder::BuildStaticValidatedBaseline();

    EXPECT_TRUE(IsSelectable(caps.QueryHdr10Native(VideoCodec::Av1)))
        << "AV1 must be annotated HDR10-native (10-bit/P010).";
    EXPECT_TRUE(IsSelectable(caps.QueryHdr10Native(VideoCodec::Hevc)))
        << "HEVC must be annotated HDR10-native (10-bit/P010).";
    EXPECT_FALSE(IsSelectable(caps.QueryHdr10Native(VideoCodec::H264)))
        << "H.264 must NOT be annotated HDR10-native (8-bit only).";
    // The H.264 reason must be user-facing and point at the capable codecs.
    const SupportAnnotation h264 = caps.QueryHdr10Native(VideoCodec::H264);
    EXPECT_NE(h264.reason.find("HEVC"), std::string::npos) << "reason: " << h264.reason;
    EXPECT_NE(h264.reason.find("AV1"), std::string::npos) << "reason: " << h264.reason;
}

// The HDR10-native annotation is a codec-intrinsic format fact and must NOT be
// coupled to the NVENC-absence downgrade (which governs encode availability, a
// separate concern owned by QueryVideoCodec / rec.003).
TEST(Hdr10CapabilityTest, Hdr10NativeIsIndependentOfNvencPresence) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_dll_present = false;
    snap.nvidia.nvenc_api_version_valid = false;
    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    // Encode availability is gone (rec.003 territory) …
    EXPECT_FALSE(IsSelectable(caps.QueryVideoCodec(VideoCodec::Av1)));
    // … but the format-capability annotation is unchanged.
    EXPECT_TRUE(IsSelectable(caps.QueryHdr10Native(VideoCodec::Av1)));
    EXPECT_TRUE(IsSelectable(caps.QueryHdr10Native(VideoCodec::Hevc)));
    EXPECT_FALSE(IsSelectable(caps.QueryHdr10Native(VideoCodec::H264)));
}

// -------------------------------------------------------------------------
// DisplayHdrFacts primaries/luminance + Display↔Capture mapping.
// -------------------------------------------------------------------------
TEST(DisplayHdrFactsTest, PrimariesDefaultToZero) {
    const DisplayHdrFacts facts;
    EXPECT_FLOAT_EQ(facts.red_primary_x, 0.0f);
    EXPECT_FLOAT_EQ(facts.red_primary_y, 0.0f);
    EXPECT_FLOAT_EQ(facts.green_primary_x, 0.0f);
    EXPECT_FLOAT_EQ(facts.green_primary_y, 0.0f);
    EXPECT_FLOAT_EQ(facts.blue_primary_x, 0.0f);
    EXPECT_FLOAT_EQ(facts.blue_primary_y, 0.0f);
    EXPECT_FLOAT_EQ(facts.white_point_x, 0.0f);
    EXPECT_FLOAT_EQ(facts.white_point_y, 0.0f);
    EXPECT_FLOAT_EQ(facts.max_luminance_nits, 0.0f);
    EXPECT_FLOAT_EQ(facts.min_luminance_nits, 0.0f);
    EXPECT_FLOAT_EQ(facts.max_full_frame_nits, 0.0f);
    EXPECT_FALSE(facts.wide_color_enforced);
}

TEST(DisplayHdrFactsTest, FindDisplayByName_ReturnsMatchingEntryWithFacts) {
    std::vector<DisplayHdrFacts> displays;
    DisplayHdrFacts a;
    a.name = "\\\\.\\DISPLAY1";
    a.hdr_active = false;
    displays.push_back(a);
    DisplayHdrFacts b;
    b.name = "\\\\.\\DISPLAY7";
    b.hdr_active = true;
    b.bits_per_color = 10;
    b.red_primary_x = 0.680f;
    b.max_luminance_nits = 1499.0f;
    displays.push_back(b);

    const DisplayHdrFacts* found = FindDisplayByName(displays, "\\\\.\\DISPLAY7");
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(found->hdr_active);
    EXPECT_EQ(found->bits_per_color, 10u);
    EXPECT_FLOAT_EQ(found->red_primary_x, 0.680f);
    EXPECT_FLOAT_EQ(found->max_luminance_nits, 1499.0f);
}

TEST(DisplayHdrFactsTest, FindDisplayByName_ReturnsNullptrWhenAbsent) {
    std::vector<DisplayHdrFacts> displays;
    DisplayHdrFacts a;
    a.name = "\\\\.\\DISPLAY1";
    displays.push_back(a);

    EXPECT_EQ(FindDisplayByName(displays, "\\\\.\\DISPLAY9"), nullptr);
    EXPECT_EQ(FindDisplayByName({}, "\\\\.\\DISPLAY1"), nullptr);
}

} // namespace
} // namespace exosnap::capability

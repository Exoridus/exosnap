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
    snap.mf_aac.mftenum_found = true;
    snap.mf_aac.clsid_instantiable = false; // mftenum_found alone is sufficient
    snap.os.build_number = 26100u;
    snap.os.version_string = "10.0.26100";
    return snap;
}

// Shorthand for the primary M3.2 combo key.
constexpr Container kC = Container::Matroska;
constexpr VideoCodec kV = VideoCodec::Av1Nvenc;
constexpr AudioCodec kA = AudioCodec::AacMf;
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
    EXPECT_TRUE(IsSelectable(caps.QueryVideoCodec(VideoCodec::Av1Nvenc)))
        << "VideoCodec::Av1Nvenc should be selectable.";

    // AacMf dimension must be selectable.
    EXPECT_TRUE(IsSelectable(caps.QueryAudioCodec(AudioCodec::AacMf))) << "AudioCodec::AacMf should be selectable.";
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
    const SupportAnnotation av1 = caps.QueryVideoCodec(VideoCodec::Av1Nvenc);
    EXPECT_FALSE(IsSelectable(av1)) << "VideoCodec::Av1Nvenc must not be selectable when NVENC DLL is absent.";

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
    const SupportAnnotation av1 = caps.QueryVideoCodec(VideoCodec::Av1Nvenc);
    EXPECT_FALSE(IsSelectable(av1)) << "VideoCodec::Av1Nvenc must not be selectable when NVENC API version is invalid.";

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
// TC-4: AAC unavailable blocks AAC path
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC4_AacUnavailableBlocksAacPath) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.mf_aac.mftenum_found = false;
    snap.mf_aac.clsid_instantiable = false;

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    // AacMf dimension must not be selectable.
    const SupportAnnotation aac = caps.QueryAudioCodec(AudioCodec::AacMf);
    EXPECT_FALSE(IsSelectable(aac)) << "AudioCodec::AacMf must not be selectable when MF AAC is unavailable.";

    // M3.2 primary combo must not be selectable.
    const SupportAnnotation combo = caps.QueryCombo(kC, kV, kA, kCS, kBD);
    EXPECT_FALSE(IsSelectable(combo)) << "M3.2 combo must not be selectable when MF AAC is unavailable.";

    // Reason must mention AAC or Media Foundation.
    const bool mentions_aac =
        (aac.reason.find("AAC") != std::string::npos) || (aac.reason.find("Media Foundation") != std::string::npos) ||
        (combo.reason.find("AAC") != std::string::npos) || (combo.reason.find("Media Foundation") != std::string::npos);
    EXPECT_TRUE(mentions_aac) << "Downgrade reason should mention AAC or Media Foundation. aac.reason='" << aac.reason
                              << "' combo.reason='" << combo.reason << "'";
}

// -------------------------------------------------------------------------
// TC-5: Direct AAC CLSID fallback is sufficient (mirrors M2.7 discovery)
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC5_DirectAacClsidFallbackIsSufficient) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.mf_aac.mftenum_found = false;     // enumeration returns 0
    snap.mf_aac.clsid_instantiable = true; // but direct instantiation succeeds

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    // available() must return true when only clsid_instantiable is true.
    EXPECT_TRUE(snap.mf_aac.available())
        << "MfAacRuntimeFacts::available() must be true when clsid_instantiable is true.";

    // AacMf must remain selectable.
    EXPECT_TRUE(IsSelectable(caps.QueryAudioCodec(AudioCodec::AacMf)))
        << "AudioCodec::AacMf should remain selectable when CLSID fallback succeeds.";

    // M3.2 combo must remain Available.
    const SupportAnnotation combo = caps.QueryCombo(kC, kV, kA, kCS, kBD);
    EXPECT_EQ(combo.level, SupportLevel::Available)
        << "M3.2 combo should remain Available when CLSID AAC fallback succeeds. reason: " << combo.reason;
}

// -------------------------------------------------------------------------
// TC-6: H.264 is Available when NVENC is present (Phase 23E)
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC6_H264Available_WhenNvencPresent) {
    const RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    const SupportAnnotation h264 = caps.QueryVideoCodec(VideoCodec::H264Nvenc);
    EXPECT_EQ(h264.level, SupportLevel::Available)
        << "H.264 must be Available when NVENC is present. reason: " << h264.reason;
    EXPECT_TRUE(IsSelectable(h264)) << "H.264 must be selectable when NVENC is present.";

    // MP4+H264+AAC primary combo must also be Available.
    const SupportAnnotation mp4_combo = caps.QueryCombo(Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::AacMf,
                                                        ChromaSubsampling::Cs420, BitDepth::Bit8);
    EXPECT_EQ(mp4_combo.level, SupportLevel::Available)
        << "MP4+H264+AAC must be Available when NVENC and AAC are present. reason: " << mp4_combo.reason;
}

TEST(RuntimeMergeTest, TC6b_H264NotImplemented_WhenNvencAbsent) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.nvidia.nvenc_dll_present = false;

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    const SupportAnnotation h264 = caps.QueryVideoCodec(VideoCodec::H264Nvenc);
    EXPECT_EQ(h264.level, SupportLevel::NotImplemented) << "H.264 must be NotImplemented when NVENC DLL is absent.";
    EXPECT_FALSE(IsSelectable(h264));

    const SupportAnnotation mp4_combo = caps.QueryCombo(Container::Mp4, VideoCodec::H264Nvenc, AudioCodec::AacMf,
                                                        ChromaSubsampling::Cs420, BitDepth::Bit8);
    EXPECT_FALSE(IsSelectable(mp4_combo)) << "MP4+H264+AAC must not be selectable when NVENC is absent.";
}

// -------------------------------------------------------------------------
// TC-7: HEVC is ValidUnvalidated (selectable with caveat) under favorable
// runtime (0.7.0: engine path implemented, not yet hardware-validated).
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC7_HevcValidUnvalidatedWhenNvencPresent) {
    const RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);

    const SupportAnnotation hevc = caps.QueryVideoCodec(VideoCodec::HevcNvenc);
    EXPECT_EQ(hevc.level, SupportLevel::ValidUnvalidated)
        << "HEVC must be ValidUnvalidated when NVENC is present (implemented, not hw-validated).";
    EXPECT_TRUE(IsSelectable(hevc)) << "HEVC must be selectable (with caveat) when NVENC is present.";

    // The MKV + HEVC combo must be selectable end-to-end (registry Allowed +
    // dimension ValidUnvalidated must not be force-downgraded by the combine logic).
    const SupportAnnotation combo = caps.QueryCombo(Container::Matroska, VideoCodec::HevcNvenc, AudioCodec::Opus,
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

    const SupportAnnotation hevc = caps.QueryVideoCodec(VideoCodec::HevcNvenc);
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
    EXPECT_EQ(caps.runtime.mf_aac.mftenum_found, snap.mf_aac.mftenum_found);
    EXPECT_EQ(caps.runtime.mf_aac.clsid_instantiable, snap.mf_aac.clsid_instantiable);
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
    const SupportAnnotation h264 = caps.QueryVideoCodec(VideoCodec::H264Nvenc);
    (void)h264;

    // HEVC availability is now hardware-dependent (ValidUnvalidated when NVENC is
    // present, NotImplemented when absent) — just verify it's queryable and never
    // hard-Invalid.
    const SupportAnnotation hevc = caps.QueryVideoCodec(VideoCodec::HevcNvenc);
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
    const SupportAnnotation av1 = caps.QueryVideoCodec(VideoCodec::Av1Nvenc);
    const SupportAnnotation h264 = caps.QueryVideoCodec(VideoCodec::H264Nvenc);

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
// TC-11: AAC downgrade reason is user-facing — no internal COM/MF symbols
// -------------------------------------------------------------------------
TEST(RuntimeMergeTest, TC11_AacDowngradeReason_IsUserFacing) {
    RuntimeCapabilitySnapshot snap = MakeFavorableSnapshot();
    snap.mf_aac.mftenum_found = false;
    snap.mf_aac.clsid_instantiable = false;

    const CapabilitySet caps = CapabilityBuilder::BuildEffectiveCapabilities(snap);
    const SupportAnnotation aac = caps.QueryAudioCodec(AudioCodec::AacMf);

    // Must not expose internal COM/MF API identifiers.
    EXPECT_EQ(aac.reason.find("MFTEnumEx"), std::string::npos) << "reason: " << aac.reason;
    EXPECT_EQ(aac.reason.find("CLSID_AACMFTEncoder"), std::string::npos) << "reason: " << aac.reason;
    // Must be actionable and mention AAC.
    EXPECT_NE(aac.reason.find("AAC"), std::string::npos) << "reason: " << aac.reason;
}

} // namespace
} // namespace exosnap::capability

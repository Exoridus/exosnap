#include <gtest/gtest.h>

#if __has_include(<nvEncodeAPI.h>)
#include <capability/config_types.h>
#include <capability/encoder_format_capability.h>
#include <nvEncodeAPI.h>

#include "../src/nvenc_format_probe.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace exosnap::capability {
namespace {

struct ProbeSession {
    std::vector<NV_ENC_BUFFER_FORMAT> formats{NV_ENC_BUFFER_FORMAT_NV12, NV_ENC_BUFFER_FORMAT_YUV420_10BIT,
                                              NV_ENC_BUFFER_FORMAT_AYUV};
    std::vector<GUID> profiles{NV_ENC_HEVC_PROFILE_MAIN_GUID, NV_ENC_HEVC_PROFILE_MAIN10_GUID,
                               NV_ENC_HEVC_PROFILE_FREXT_GUID};
    GUID expected_codec = NV_ENC_CODEC_HEVC_GUID;
    NVENCSTATUS format_status = NV_ENC_SUCCESS;
    NVENCSTATUS profile_status = NV_ENC_SUCCESS;
    NVENCSTATUS depth_status = NV_ENC_SUCCESS;
    NVENCSTATUS chroma_status = NV_ENC_SUCCESS;
    int depth_support = 1;
    int chroma_support = 1;
    bool excessive_format_count = false;
    bool excessive_profile_count = false;
    uint32_t queries = 0;
};

ProbeSession& Session(void* encoder, const GUID& codec) {
    auto& session = *static_cast<ProbeSession*>(encoder);
    EXPECT_EQ(std::memcmp(&codec, &session.expected_codec, sizeof(GUID)), 0);
    ++session.queries;
    return session;
}

NVENCSTATUS NVENCAPI FormatCount(void* encoder, GUID codec, uint32_t* count) {
    auto& session = Session(encoder, codec);
    *count = static_cast<uint32_t>(session.formats.size());
    return session.format_status;
}

NVENCSTATUS NVENCAPI Formats(void* encoder, GUID codec, NV_ENC_BUFFER_FORMAT* formats, uint32_t capacity,
                             uint32_t* count) {
    auto& session = Session(encoder, codec);
    EXPECT_EQ(capacity, session.formats.size());
    std::copy(session.formats.begin(), session.formats.end(), formats);
    *count = capacity + (session.excessive_format_count ? 1u : 0u);
    return session.format_status;
}

NVENCSTATUS NVENCAPI ProfileCount(void* encoder, GUID codec, uint32_t* count) {
    auto& session = Session(encoder, codec);
    *count = static_cast<uint32_t>(session.profiles.size());
    return session.profile_status;
}

NVENCSTATUS NVENCAPI Profiles(void* encoder, GUID codec, GUID* profiles, uint32_t capacity, uint32_t* count) {
    auto& session = Session(encoder, codec);
    EXPECT_EQ(capacity, session.profiles.size());
    std::copy(session.profiles.begin(), session.profiles.end(), profiles);
    *count = capacity + (session.excessive_profile_count ? 1u : 0u);
    return session.profile_status;
}

NVENCSTATUS NVENCAPI Cap(void* encoder, GUID codec, NV_ENC_CAPS_PARAM* params, int* value) {
    auto& session = Session(encoder, codec);
    EXPECT_EQ(params->version, NV_ENC_CAPS_PARAM_VER);
    if (params->capsToQuery == NV_ENC_CAPS_SUPPORT_10BIT_ENCODE) {
        *value = session.depth_support;
        return session.depth_status;
    }
    EXPECT_EQ(params->capsToQuery, NV_ENC_CAPS_SUPPORT_YUV444_ENCODE);
    *value = session.chroma_support;
    return session.chroma_status;
}

NV_ENCODE_API_FUNCTION_LIST Api() {
    NV_ENCODE_API_FUNCTION_LIST api{};
    api.nvEncGetInputFormatCount = FormatCount;
    api.nvEncGetInputFormats = Formats;
    api.nvEncGetEncodeProfileGUIDCount = ProfileCount;
    api.nvEncGetEncodeProfileGUIDs = Profiles;
    api.nvEncGetEncodeCaps = Cap;
    return api;
}

TEST(NvencFormatProbe, RequiresExactInputDepthAndProfileEvidence) {
    ProbeSession session;
    const auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.nv12, EncoderFormatSupport::Supported);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Supported);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Supported);
    EXPECT_GT(session.queries, 0u);
}

TEST(NvencFormatProbe, CodecWithP010InputStillRequiresTenBitEncodingCap) {
    ProbeSession session;
    session.depth_support = 0;
    const auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.nv12, EncoderFormatSupport::Supported);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unsupported);
}

TEST(NvencFormatProbe, TenBitCapWithoutP010InputIsUnsupported) {
    ProbeSession session;
    session.formats = {NV_ENC_BUFFER_FORMAT_NV12};
    const auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unsupported);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Unsupported);
}

TEST(NvencFormatProbe, Main10AndFrextAreSeparateRequirements) {
    ProbeSession session;
    session.profiles = {NV_ENC_HEVC_PROFILE_MAIN_GUID, NV_ENC_HEVC_PROFILE_MAIN10_GUID};
    auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Supported);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Unsupported);
    session.profiles = {NV_ENC_HEVC_PROFILE_MAIN_GUID, NV_ENC_HEVC_PROFILE_FREXT_GUID};
    formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unsupported);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Supported);
}

TEST(NvencFormatProbe, FailedQueriesAreUnavailableRatherThanNegativeHardwareFacts) {
    ProbeSession session;
    session.depth_status = NV_ENC_ERR_GENERIC;
    session.chroma_status = NV_ENC_ERR_GENERIC;
    auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.nv12, EncoderFormatSupport::Supported);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unavailable);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Unavailable);
    session.format_status = NV_ENC_ERR_GENERIC;
    formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.nv12, EncoderFormatSupport::Unavailable);
}

TEST(NvencFormatProbe, MissingApiAndInvalidCountsNeverClaimSupport) {
    ProbeSession session;
    auto formats = detail::ProbeNvencFormats({}, &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.nv12, EncoderFormatSupport::Unavailable);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unavailable);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Unavailable);
    session.excessive_format_count = true;
    session.excessive_profile_count = true;
    formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.nv12, EncoderFormatSupport::Unavailable);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unavailable);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Unavailable);
}

TEST(NvencFormatProbe, FailedProfileQueryDoesNotInventAProfileOrDisableNv12) {
    ProbeSession session;
    session.profile_status = NV_ENC_ERR_GENERIC;
    const auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.nv12, EncoderFormatSupport::Supported);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unavailable);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Unavailable);
}

TEST(NvencFormatProbe, EmptySuccessfulListsAreAuthoritativeNegatives) {
    ProbeSession session;
    session.formats.clear();
    session.profiles.clear();
    const auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.nv12, EncoderFormatSupport::Unsupported);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unsupported);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Unsupported);
}

TEST(NvencFormatProbe, InvalidBooleanCapabilityValuesRemainUnavailable) {
    ProbeSession session;
    session.depth_support = -1;
    session.chroma_support = 2;
    const auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Hevc);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unavailable);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Unavailable);
}

TEST(NvencFormatProbe, Av1UsesMainProfileAndHasNoImplemented444Path) {
    ProbeSession session;
    session.expected_codec = NV_ENC_CODEC_AV1_GUID;
    session.profiles = {NV_ENC_AV1_PROFILE_MAIN_GUID};
    const auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::Av1);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Supported);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Unsupported);
}

TEST(NvencFormatProbe, H264RequiresHigh444AndDoesNotExposeUnimplementedTenBit) {
    ProbeSession session;
    session.expected_codec = NV_ENC_CODEC_H264_GUID;
    session.profiles = {NV_ENC_H264_PROFILE_HIGH_444_GUID};
    const auto formats = detail::ProbeNvencFormats(Api(), &session, session.expected_codec, VideoCodec::H264);
    EXPECT_EQ(formats.p010, EncoderFormatSupport::Unsupported);
    EXPECT_EQ(formats.ayuv, EncoderFormatSupport::Supported);
}

TEST(NvencFormatProbe, EachSessionKeepsItsOwnDeviceEvidence) {
    ProbeSession first;
    ProbeSession second;
    second.depth_support = 0;
    EXPECT_EQ(detail::ProbeNvencFormats(Api(), &first, first.expected_codec, VideoCodec::Hevc).p010,
              EncoderFormatSupport::Supported);
    EXPECT_EQ(detail::ProbeNvencFormats(Api(), &second, second.expected_codec, VideoCodec::Hevc).p010,
              EncoderFormatSupport::Unsupported);
}

} // namespace
} // namespace exosnap::capability
#else
TEST(NvencFormatProbe, VendorHeaderUnavailable) {
    GTEST_SKIP() << "NVENC SDK header is not present in this build.";
}
#endif

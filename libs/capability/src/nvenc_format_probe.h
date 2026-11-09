#pragma once

#include <capability/encoder_format_capability.h>

#include <nvEncodeAPI.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

namespace exosnap::capability::detail {

inline EncoderFormatSupport RequireFormatFacts(std::initializer_list<EncoderFormatSupport> facts) noexcept {
    // One authoritative negative is sufficient even if another query failed.
    for (const auto fact : facts)
        if (fact == EncoderFormatSupport::Unsupported)
            return fact;
    for (const auto fact : facts)
        if (fact == EncoderFormatSupport::Unavailable)
            return fact;
    for (const auto fact : facts)
        if (fact == EncoderFormatSupport::Unknown)
            return fact;
    return EncoderFormatSupport::Supported;
}

// Uses the caller's open session. No enumeration of a different adapter, new
// device/session, or global driver assumptions may enter these format facts.
inline CodecFormatCapabilities ProbeNvencFormats(const NV_ENCODE_API_FUNCTION_LIST& api, void* encoder,
                                                 const GUID& codec_guid, VideoCodec codec) {
    std::optional<std::vector<NV_ENC_BUFFER_FORMAT>> formats;
    if (api.nvEncGetInputFormatCount && api.nvEncGetInputFormats) {
        uint32_t count = 0;
        if (api.nvEncGetInputFormatCount(encoder, codec_guid, &count) == NV_ENC_SUCCESS) {
            if (count == 0) {
                formats.emplace();
            } else {
                std::vector<NV_ENC_BUFFER_FORMAT> values(count);
                uint32_t written = 0;
                if (api.nvEncGetInputFormats(encoder, codec_guid, values.data(), count, &written) == NV_ENC_SUCCESS &&
                    written <= count) {
                    values.resize(written);
                    formats = std::move(values);
                }
            }
        }
    }
    const auto has_format = [&formats](NV_ENC_BUFFER_FORMAT format) {
        if (!formats)
            return EncoderFormatSupport::Unavailable;
        return std::find(formats->begin(), formats->end(), format) != formats->end()
                   ? EncoderFormatSupport::Supported
                   : EncoderFormatSupport::Unsupported;
    };

    std::optional<std::vector<GUID>> profiles;
    if (api.nvEncGetEncodeProfileGUIDCount && api.nvEncGetEncodeProfileGUIDs) {
        uint32_t count = 0;
        if (api.nvEncGetEncodeProfileGUIDCount(encoder, codec_guid, &count) == NV_ENC_SUCCESS) {
            if (count == 0) {
                profiles.emplace();
            } else {
                std::vector<GUID> values(count);
                uint32_t written = 0;
                if (api.nvEncGetEncodeProfileGUIDs(encoder, codec_guid, values.data(), count, &written) ==
                        NV_ENC_SUCCESS &&
                    written <= count) {
                    values.resize(written);
                    profiles = std::move(values);
                }
            }
        }
    }
    const auto has_profile = [&profiles](const GUID& profile) {
        if (!profiles)
            return EncoderFormatSupport::Unavailable;
        return std::any_of(
                   profiles->begin(), profiles->end(),
                   [&profile](const GUID& candidate) { return std::memcmp(&profile, &candidate, sizeof(GUID)) == 0; })
                   ? EncoderFormatSupport::Supported
                   : EncoderFormatSupport::Unsupported;
    };
    const auto has_cap = [&api, encoder, &codec_guid](NV_ENC_CAPS cap) {
        if (!api.nvEncGetEncodeCaps)
            return EncoderFormatSupport::Unavailable;
        NV_ENC_CAPS_PARAM params{};
        params.version = NV_ENC_CAPS_PARAM_VER;
        params.capsToQuery = cap;
        int value = 0;
        if (api.nvEncGetEncodeCaps(encoder, codec_guid, &params, &value) != NV_ENC_SUCCESS)
            return EncoderFormatSupport::Unavailable;
        if (value == 0)
            return EncoderFormatSupport::Unsupported;
        return value == 1 ? EncoderFormatSupport::Supported : EncoderFormatSupport::Unavailable;
    };

    CodecFormatCapabilities result;
    result.nv12 = has_format(NV_ENC_BUFFER_FORMAT_NV12);
    result.p010 = EncoderFormatSupport::Unsupported;
    if (codec == VideoCodec::Hevc || codec == VideoCodec::Av1) {
        result.p010 = RequireFormatFacts(
            {has_format(NV_ENC_BUFFER_FORMAT_YUV420_10BIT), has_cap(NV_ENC_CAPS_SUPPORT_10BIT_ENCODE),
             has_profile(codec == VideoCodec::Hevc ? NV_ENC_HEVC_PROFILE_MAIN10_GUID : NV_ENC_AV1_PROFILE_MAIN_GUID)});
    }
    result.ayuv = EncoderFormatSupport::Unsupported;
    if (codec == VideoCodec::H264 || codec == VideoCodec::Hevc) {
        result.ayuv =
            RequireFormatFacts({has_format(NV_ENC_BUFFER_FORMAT_AYUV), has_cap(NV_ENC_CAPS_SUPPORT_YUV444_ENCODE),
                                has_profile(codec == VideoCodec::H264 ? NV_ENC_H264_PROFILE_HIGH_444_GUID
                                                                      : NV_ENC_HEVC_PROFILE_FREXT_GUID)});
    }
    return result;
}

} // namespace exosnap::capability::detail

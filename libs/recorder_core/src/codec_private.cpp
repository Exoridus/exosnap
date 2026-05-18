#include "codec_private.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace recorder_core::codec_private {

// ---------------------------------------------------------------------------
// Internal bit-reader — lifted from M2.8 probe
// ---------------------------------------------------------------------------

namespace {

struct BitReader {
    const uint8_t* data = nullptr;
    size_t total_bits = 0;
    size_t pos = 0;
    bool overflow = false;

    uint32_t f(int n) {
        if (n <= 0)
            return 0;
        if (pos + static_cast<size_t>(n) > total_bits) {
            overflow = true;
            return 0;
        }
        uint32_t val = 0;
        for (int i = 0; i < n; ++i) {
            size_t byte_idx = pos / 8;
            size_t bit_idx = 7 - (pos % 8);
            val = (val << 1) | ((data[byte_idx] >> bit_idx) & 1);
            ++pos;
        }
        return val;
    }

    uint32_t uvlc() {
        int leadingZeros = 0;
        while (true) {
            uint32_t bit = f(1);
            if (overflow)
                return 0;
            if (bit == 1)
                break;
            ++leadingZeros;
            if (leadingZeros >= 32) {
                overflow = true;
                return 0;
            }
        }
        if (leadingZeros == 0)
            return 0;
        uint32_t value = f(leadingZeros);
        if (overflow)
            return 0;
        return value + (1u << leadingZeros) - 1;
    }
};

// ---------------------------------------------------------------------------
// ParseSequenceHeaderObu — lifted from M2.8 probe
// ---------------------------------------------------------------------------

bool ParseSequenceHeaderObu(const uint8_t* obu_payload, size_t obu_size, uint32_t* out_seq_profile,
                            uint32_t* out_seq_level_idx_0, uint32_t* out_seq_tier_0, uint32_t* out_high_bitdepth,
                            uint32_t* out_twelve_bit, uint32_t* out_mono_chrome, uint32_t* out_subsampling_x,
                            uint32_t* out_subsampling_y, uint32_t* out_chroma_sample_position, char* reason,
                            size_t reason_size) {
    BitReader br;
    br.data = obu_payload;
    br.total_bits = obu_size * 8;
    br.pos = 0;
    br.overflow = false;

#define CHECK_OVERFLOW(ctx)                                                                                            \
    if (br.overflow) {                                                                                                 \
        snprintf(reason, reason_size, "BitReader overflow at " ctx);                                                   \
        return false;                                                                                                  \
    }

    uint32_t seq_profile = br.f(3);
    CHECK_OVERFLOW("seq_profile");
    uint32_t still_picture = br.f(1);
    (void)still_picture;
    CHECK_OVERFLOW("still_picture");
    uint32_t reduced_still_picture_header = br.f(1);
    CHECK_OVERFLOW("reduced_still_picture_header");

    if (reduced_still_picture_header) {
        snprintf(reason, reason_size, "reduced_still_picture_header=1 — cannot derive full color config");
        return false;
    }

    uint32_t timing_info_present_flag = br.f(1);
    CHECK_OVERFLOW("timing_info_present_flag");
    uint32_t decoder_model_info_present_flag = 0;

    if (timing_info_present_flag) {
        br.f(32);
        CHECK_OVERFLOW("num_units_in_display_tick");
        br.f(32);
        CHECK_OVERFLOW("time_scale");
        uint32_t equal_picture_interval = br.f(1);
        CHECK_OVERFLOW("equal_picture_interval");
        if (equal_picture_interval) {
            br.uvlc();
            CHECK_OVERFLOW("num_ticks_per_picture_minus_1");
        }

        decoder_model_info_present_flag = br.f(1);
        CHECK_OVERFLOW("decoder_model_info_present_flag");
        if (decoder_model_info_present_flag) {
            snprintf(reason, reason_size, "decoder_model_info_present_flag=1 — too complex to skip");
            return false;
        }
    }

    uint32_t initial_display_delay_present_flag = br.f(1);
    CHECK_OVERFLOW("initial_display_delay_present_flag");
    uint32_t operating_points_cnt_minus_1 = br.f(5);
    CHECK_OVERFLOW("operating_points_cnt_minus_1");

    uint32_t seq_level_idx[32] = {};
    uint32_t seq_tier[32] = {};

    for (uint32_t i = 0; i <= operating_points_cnt_minus_1; ++i) {
        br.f(12);
        CHECK_OVERFLOW("operating_point_idc");
        seq_level_idx[i] = br.f(5);
        CHECK_OVERFLOW("seq_level_idx");
        if (seq_level_idx[i] >= 4) {
            seq_tier[i] = br.f(1);
            CHECK_OVERFLOW("seq_tier");
        } else {
            seq_tier[i] = 0;
        }
        if (decoder_model_info_present_flag) {
            snprintf(reason, reason_size, "operating_parameters_info path not supported");
            return false;
        }
        if (initial_display_delay_present_flag) {
            uint32_t delay_present = br.f(1);
            CHECK_OVERFLOW("initial_display_delay_present_for_this_op");
            if (delay_present) {
                br.f(4);
                CHECK_OVERFLOW("initial_display_delay_minus_1");
            }
        }
    }

    uint32_t seq_level_idx_0 = seq_level_idx[0];
    uint32_t seq_tier_0 = seq_tier[0];

    uint32_t frame_width_bits_minus_1 = br.f(4);
    CHECK_OVERFLOW("frame_width_bits_minus_1");
    uint32_t frame_height_bits_minus_1 = br.f(4);
    CHECK_OVERFLOW("frame_height_bits_minus_1");
    br.f(frame_width_bits_minus_1 + 1);
    CHECK_OVERFLOW("max_frame_width_minus_1");
    br.f(frame_height_bits_minus_1 + 1);
    CHECK_OVERFLOW("max_frame_height_minus_1");

    uint32_t frame_id_numbers_present_flag = br.f(1);
    CHECK_OVERFLOW("frame_id_numbers_present_flag");
    if (frame_id_numbers_present_flag) {
        br.f(4);
        CHECK_OVERFLOW("delta_frame_id_length_minus_2");
        br.f(3);
        CHECK_OVERFLOW("additional_frame_id_length_minus_1");
    }

    br.f(1);
    CHECK_OVERFLOW("use_128x128_superblock");
    br.f(1);
    CHECK_OVERFLOW("enable_filter_intra");
    br.f(1);
    CHECK_OVERFLOW("enable_intra_edge_filter");
    br.f(1);
    CHECK_OVERFLOW("enable_interintra_compound");
    br.f(1);
    CHECK_OVERFLOW("enable_masked_compound");
    br.f(1);
    CHECK_OVERFLOW("enable_warped_motion");
    br.f(1);
    CHECK_OVERFLOW("enable_dual_filter");

    uint32_t enable_order_hint = br.f(1);
    CHECK_OVERFLOW("enable_order_hint");
    if (enable_order_hint) {
        br.f(1);
        CHECK_OVERFLOW("enable_jnt_comp");
        br.f(1);
        CHECK_OVERFLOW("enable_ref_frame_mvs");
    }

    uint32_t seq_choose_screen_content_tools = br.f(1);
    CHECK_OVERFLOW("seq_choose_screen_content_tools");
    uint32_t seq_force_screen_content_tools = 0;
    if (seq_choose_screen_content_tools) {
        seq_force_screen_content_tools = 2;
    } else {
        seq_force_screen_content_tools = br.f(1);
        CHECK_OVERFLOW("seq_force_screen_content_tools");
    }

    if (seq_force_screen_content_tools > 0) {
        uint32_t seq_choose_integer_mv = br.f(1);
        CHECK_OVERFLOW("seq_choose_integer_mv");
        if (!seq_choose_integer_mv) {
            br.f(1);
            CHECK_OVERFLOW("seq_force_integer_mv");
        }
    }

    if (enable_order_hint) {
        br.f(3);
        CHECK_OVERFLOW("order_hint_bits_minus_1");
    }

    br.f(1);
    CHECK_OVERFLOW("enable_superres");
    br.f(1);
    CHECK_OVERFLOW("enable_cdef");
    br.f(1);
    CHECK_OVERFLOW("enable_restoration");

    // color_config()
    uint32_t high_bitdepth = br.f(1);
    CHECK_OVERFLOW("high_bitdepth");
    uint32_t twelve_bit = 0;
    if (seq_profile == 2 && high_bitdepth) {
        twelve_bit = br.f(1);
        CHECK_OVERFLOW("twelve_bit");
    }
    uint32_t mono_chrome = 0;
    if (seq_profile == 1) {
        mono_chrome = 0;
    } else {
        mono_chrome = br.f(1);
        CHECK_OVERFLOW("mono_chrome");
    }

    uint32_t color_description_present_flag = br.f(1);
    CHECK_OVERFLOW("color_description_present_flag");
    uint32_t color_primaries = 2;
    uint32_t transfer_characteristics = 2;
    uint32_t matrix_coefficients = 2;
    if (color_description_present_flag) {
        color_primaries = br.f(8);
        CHECK_OVERFLOW("color_primaries");
        transfer_characteristics = br.f(8);
        CHECK_OVERFLOW("transfer_characteristics");
        matrix_coefficients = br.f(8);
        CHECK_OVERFLOW("matrix_coefficients");
    }

    uint32_t subsampling_x = 0;
    uint32_t subsampling_y = 0;
    uint32_t chroma_sample_position = 0;

    if (mono_chrome) {
        br.f(1);
        CHECK_OVERFLOW("color_range(mono)");
        subsampling_x = 1;
        subsampling_y = 1;
        chroma_sample_position = 0;
    } else if (color_primaries == 1 && transfer_characteristics == 13 && matrix_coefficients == 0) {
        subsampling_x = 0;
        subsampling_y = 0;
        chroma_sample_position = 0;
    } else {
        br.f(1);
        CHECK_OVERFLOW("color_range");
        if (seq_profile == 0) {
            subsampling_x = 1;
            subsampling_y = 1;
        } else if (seq_profile == 1) {
            subsampling_x = 0;
            subsampling_y = 0;
        } else {
            if (twelve_bit) {
                subsampling_x = 1;
                subsampling_y = 1;
            } else {
                subsampling_x = 1;
                subsampling_y = 0;
            }
        }
        if (subsampling_x && subsampling_y) {
            chroma_sample_position = br.f(2);
            CHECK_OVERFLOW("chroma_sample_position");
        }
        br.f(1);
        CHECK_OVERFLOW("separate_uv_delta_q");
    }

#undef CHECK_OVERFLOW

    *out_seq_profile = seq_profile;
    *out_seq_level_idx_0 = seq_level_idx_0;
    *out_seq_tier_0 = seq_tier_0;
    *out_high_bitdepth = high_bitdepth;
    *out_twelve_bit = twelve_bit;
    *out_mono_chrome = mono_chrome;
    *out_subsampling_x = subsampling_x;
    *out_subsampling_y = subsampling_y;
    *out_chroma_sample_position = chroma_sample_position;
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DeriveAv1CodecPrivate — public
// ---------------------------------------------------------------------------

bool DeriveAv1CodecPrivate(const uint8_t* bitstream_data, size_t bitstream_size, uint8_t out_av1_codec_private[4],
                           char* reason_buf, size_t reason_buf_size) {
    if (bitstream_data == nullptr || bitstream_size == 0) {
        snprintf(reason_buf, reason_buf_size, "empty bitstream");
        return false;
    }

    const uint8_t* bs = bitstream_data;
    size_t bsLen = bitstream_size;

    // Scan OBUs for Sequence Header (type 1)
    const uint8_t* obu_payload = nullptr;
    size_t obu_payload_size = 0;
    size_t i = 0;

    while (i < bsLen) {
        uint8_t header_byte = bs[i++];
        uint32_t obu_type = (header_byte >> 3) & 0x0F;
        uint32_t extension_flag = (header_byte >> 2) & 0x1;
        uint32_t has_size_field = (header_byte >> 1) & 0x1;

        if (extension_flag) {
            if (i >= bsLen)
                break;
            ++i;
        }

        size_t payload_size = 0;
        if (has_size_field) {
            size_t leb_shift = 0;
            for (int b = 0; b < 8; ++b) {
                if (i >= bsLen) {
                    payload_size = 0;
                    break;
                }
                uint8_t leb_byte = bs[i++];
                payload_size |= static_cast<size_t>(leb_byte & 0x7F) << leb_shift;
                leb_shift += 7;
                if ((leb_byte & 0x80) == 0)
                    break;
            }
        } else {
            payload_size = bsLen - i;
        }

        if (obu_type == 1) {
            if (i + payload_size > bsLen) {
                snprintf(reason_buf, reason_buf_size, "Sequence Header OBU payload exceeds packet boundary");
                return false;
            }
            obu_payload = bs + i;
            obu_payload_size = payload_size;
            break;
        }

        i += payload_size;
        if (i > bsLen)
            break;
    }

    if (obu_payload == nullptr) {
        snprintf(reason_buf, reason_buf_size, "Sequence Header OBU (type 1) not found in bitstream packet");
        return false;
    }

    uint32_t seq_profile = 0, seq_level_idx_0 = 0, seq_tier_0 = 0;
    uint32_t high_bitdepth = 0, twelve_bit = 0, mono_chrome = 0;
    uint32_t subsampling_x = 0, subsampling_y = 0, chroma_sample_position = 0;

    bool ok = ParseSequenceHeaderObu(obu_payload, obu_payload_size, &seq_profile, &seq_level_idx_0, &seq_tier_0,
                                     &high_bitdepth, &twelve_bit, &mono_chrome, &subsampling_x, &subsampling_y,
                                     &chroma_sample_position, reason_buf, reason_buf_size);

    if (!ok)
        return false;

    // Build AV1CodecConfigurationRecord (4 bytes)
    out_av1_codec_private[0] = 0x81; // marker=1, version=1
    out_av1_codec_private[1] = static_cast<uint8_t>((seq_profile << 5) | (seq_level_idx_0 & 0x1F));
    out_av1_codec_private[2] =
        static_cast<uint8_t>((seq_tier_0 << 7) | (high_bitdepth << 6) | (twelve_bit << 5) | (mono_chrome << 4) |
                             (subsampling_x << 3) | (subsampling_y << 2) | (chroma_sample_position & 0x03));
    out_av1_codec_private[3] = 0x00;
    return true;
}

// ---------------------------------------------------------------------------
// DeriveAacCodecPrivate — public
// ---------------------------------------------------------------------------

bool DeriveAacCodecPrivate(IMFMediaType* output_media_type, uint8_t out_aac_codec_private[2], char* reason_buf,
                           size_t reason_buf_size) {
    if (output_media_type == nullptr) {
        snprintf(reason_buf, reason_buf_size, "output_media_type is null");
        return false;
    }

    UINT32 blobSize = 0;
    HRESULT hr = output_media_type->GetBlobSize(MF_MT_USER_DATA, &blobSize);
    if (FAILED(hr) || blobSize < 14) {
        snprintf(reason_buf, reason_buf_size, "MF_MT_USER_DATA missing or too small (blobSize=%u, hr=0x%08lX)",
                 blobSize, static_cast<unsigned long>(hr));
        return false;
    }

    std::vector<BYTE> blob(blobSize);
    hr = output_media_type->GetBlob(MF_MT_USER_DATA, blob.data(), blobSize, nullptr);
    if (FAILED(hr)) {
        snprintf(reason_buf, reason_buf_size, "GetBlob(MF_MT_USER_DATA) failed 0x%08lX",
                 static_cast<unsigned long>(hr));
        return false;
    }

    // AudioSpecificConfig starts at offset 12 (after 12-byte HEAACWAVEINFO tail)
    out_aac_codec_private[0] = blob[12];
    out_aac_codec_private[1] = blob[13];
    return true;
}

} // namespace recorder_core::codec_private

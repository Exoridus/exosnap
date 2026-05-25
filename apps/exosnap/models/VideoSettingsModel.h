#pragma once

#include <recorder_core/codec_types.h>

namespace exosnap {

struct VideoSettingsModel {
    recorder_core::NvencQualityPreset quality = recorder_core::NvencQualityPreset::Balanced;
    bool cfr = true;
    bool capture_cursor = true;

    static VideoSettingsModel Defaults() {
        return {};
    }
};

} // namespace exosnap

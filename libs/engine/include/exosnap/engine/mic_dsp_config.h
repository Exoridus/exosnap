#pragma once

namespace exosnap::engine {

struct MicDspConfig {
    bool hpf_enabled = false;
    float hpf_cutoff_hz = 80.0f;
    bool gate_enabled = false;
    float gate_threshold_db = -45.0f;
    bool agc_enabled = false;
    float agc_target_db = -18.0f;
    bool rnnoise_enabled = false;

    [[nodiscard]] bool AnyEnabled() const noexcept {
        return hpf_enabled || gate_enabled || agc_enabled || rnnoise_enabled;
    }
};

} // namespace exosnap::engine

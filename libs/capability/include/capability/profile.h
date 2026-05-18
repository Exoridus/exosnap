#pragma once

#include "user_config.h"

#include <string>

namespace exosnap::capability {

struct RecorderProfile {
    std::string        name;
    std::string        description;
    UserRecorderConfig config;
    bool               is_preset = false;
    std::string        created_adapter;
    std::string        saved_at;
};

} // namespace exosnap::capability


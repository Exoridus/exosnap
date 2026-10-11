#pragma once

#include <exosnap/engine/recorder_session.h>

namespace exosnap::engine {
[[nodiscard]] std::string RuntimeConfigContentKey(const RecorderConfig& config);
} // namespace exosnap::engine

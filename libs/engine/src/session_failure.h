#pragma once

#include <exosnap/engine/error_types.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace exosnap::engine {

struct SessionFailure {
    int32_t error_code = 0;
    ErrorPhase error_phase = ErrorPhase::None;
    std::string error_detail;
};

class SessionFailureState {
  public:
    void RecordFirst(SessionFailure failure) {
        std::lock_guard lock(mutex_);
        if (!failure_) {
            failure_ = std::move(failure);
        }
    }

    [[nodiscard]] bool HasFailure() const {
        std::lock_guard lock(mutex_);
        return failure_.has_value();
    }

    [[nodiscard]] std::optional<SessionFailure> Snapshot() const {
        std::lock_guard lock(mutex_);
        return failure_;
    }

    void Reset() {
        std::lock_guard lock(mutex_);
        failure_.reset();
    }

  private:
    mutable std::mutex mutex_;
    std::optional<SessionFailure> failure_;
};

} // namespace exosnap::engine

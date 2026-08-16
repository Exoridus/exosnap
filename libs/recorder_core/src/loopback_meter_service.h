#pragma once

#include "meter_start_mode.h"

#include <atomic>
#include <functional>
#include <future>
#include <string>
#include <thread>

namespace recorder_core {

// Preflight loopback RMS meter.
// pid == 0  → system-wide render loopback (SYS)
// pid  > 0  → per-process audio loopback  (APP)
class LoopbackMeterService {
  public:
    using RmsCallback = std::function<void(float rms_linear)>;

    LoopbackMeterService();
    ~LoopbackMeterService();

    LoopbackMeterService(const LoopbackMeterService&) = delete;
    LoopbackMeterService& operator=(const LoopbackMeterService&) = delete;

    // With MeterStartMode::Deferred the return value only reports that the worker
    // thread was launched; see MeterStartMode.
    bool Start(uint32_t target_pid, RmsCallback callback, std::string& out_error,
               MeterStartMode mode = MeterStartMode::AwaitOpen);
    void Stop();
    [[nodiscard]] bool IsRunning() const;

  private:
    struct StartResult {
        bool started = false;
        std::string error;
    };

    void ThreadMain(uint32_t target_pid, std::stop_token stop_token, std::promise<StartResult> start_result_promise);

    std::jthread thread_;
    std::atomic<bool> running_{false};
    RmsCallback callback_;
};

} // namespace recorder_core

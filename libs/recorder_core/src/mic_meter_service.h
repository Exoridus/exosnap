#pragma once

#include <recorder_core/recorder_session.h>

#include <atomic>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>

namespace recorder_core {

class MicMeterService {
  public:
    using RmsCallback = std::function<void(float rms_linear)>;

    MicMeterService();
    ~MicMeterService();

    MicMeterService(const MicMeterService&) = delete;
    MicMeterService& operator=(const MicMeterService&) = delete;

    bool Start(std::optional<std::string> device_id, MicChannelMode channel_mode, RmsCallback callback,
               std::string& out_error);

    void Stop();
    [[nodiscard]] bool IsRunning() const;

  private:
    struct StartResult {
        bool started = false;
        std::string error;
    };

    void ThreadMain(std::optional<std::string> device_id, MicChannelMode channel_mode, std::stop_token stop_token,
                    std::promise<StartResult> start_result_promise);

    std::jthread thread_;
    std::atomic<bool> running_{false};
    RmsCallback callback_;
};

} // namespace recorder_core

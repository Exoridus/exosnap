#include "mic_meter_service.h"

#include <recorder_core/audio_meter.h>

#if EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC
#include "wasapi_capture_src.h"
#endif

#include <Windows.h>
#include <objbase.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace recorder_core {

MicMeterService::MicMeterService() = default;

MicMeterService::~MicMeterService() {
    Stop();
}

bool MicMeterService::Start(std::optional<std::string> device_id, MicChannelMode channel_mode, RmsCallback callback,
                            std::string& out_error) {
    out_error.clear();

    if (!callback) {
        out_error = "Mic meter callback is not set.";
        return false;
    }

#if !EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC
    out_error = "Mic meter service is unavailable in this recorder_core build.";
    return false;
#else
    if (running_.load()) {
        out_error = "Mic meter service is already running.";
        return false;
    }

    Stop();

    callback_ = std::move(callback);
    running_.store(true);

    std::promise<StartResult> start_result_promise;
    std::future<StartResult> start_result_future = start_result_promise.get_future();

    try {
        thread_ = std::jthread([this, id = std::move(device_id), mode = channel_mode,
                                promise = std::move(start_result_promise)](std::stop_token stop_token) mutable {
            ThreadMain(std::move(id), mode, stop_token, std::move(promise));
        });
    } catch (...) {
        running_.store(false);
        callback_ = {};
        out_error = "Failed to start mic meter worker thread.";
        return false;
    }

    StartResult start_result;
    try {
        start_result = start_result_future.get();
    } catch (...) {
        Stop();
        out_error = "Mic meter worker failed during startup.";
        return false;
    }
    if (!start_result.started) {
        out_error = start_result.error;
        Stop();
        return false;
    }

    return true;
#endif
}

void MicMeterService::Stop() {
    running_.store(false);
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    callback_ = {};
}

bool MicMeterService::IsRunning() const {
    return running_.load();
}

void MicMeterService::ThreadMain(std::optional<std::string> device_id, MicChannelMode channel_mode,
                                 std::stop_token stop_token, std::promise<StartResult> start_result_promise) {
#if !EXOSNAP_RECORDER_CORE_HAS_WASAPI_CAPTURE_SRC
    StartResult start_result;
    start_result.started = false;
    start_result.error = "Mic meter service is unavailable in this recorder_core build.";
    start_result_promise.set_value(std::move(start_result));
    running_.store(false);
    (void)device_id;
    (void)channel_mode;
    (void)stop_token;
    return;
#else
    StartResult start_result;
    bool start_result_sent = false;
    auto publish_start_result = [&](bool ok, std::string error) {
        if (start_result_sent) {
            return;
        }
        start_result_sent = true;
        start_result.started = ok;
        start_result.error = std::move(error);
        start_result_promise.set_value(std::move(start_result));
    };

    bool should_uninit = false;
    try {
        const HRESULT coinit_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        should_uninit = SUCCEEDED(coinit_hr);
        if (FAILED(coinit_hr) && coinit_hr != RPC_E_CHANGED_MODE) {
            publish_start_result(false, "CoInitializeEx failed for mic meter worker thread.");
            running_.store(false);
            return;
        }

        WasapiCaptureSrc capture(channel_mode, std::move(device_id));
        std::string capture_error;
        if (!capture.Init(capture_error)) {
            publish_start_result(false, capture_error.empty() ? "Failed to initialize microphone capture for meter."
                                                              : std::move(capture_error));
            capture.Shutdown();
            if (should_uninit) {
                CoUninitialize();
            }
            running_.store(false);
            return;
        }

        publish_start_result(true, {});

        while (running_.load() && !stop_token.stop_requested()) {
            if (capture.PendingFrameCount() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            RawAudioBuffer buffer;
            std::string acquire_error;
            if (!capture.AcquireBuffer(buffer, acquire_error)) {
                break;
            }

            float rms = 0.0f;
            if (!buffer.silent && buffer.bytes != nullptr && buffer.num_frames > 0) {
                const std::size_t channels = static_cast<std::size_t>(capture.Channels());
                const std::size_t sample_count = static_cast<std::size_t>(buffer.num_frames) * channels;
                if (sample_count > 0) {
                    if (capture.SampleFormat() == AudioSampleFormat::Float32) {
                        const auto* samples = reinterpret_cast<const float*>(buffer.bytes);
                        rms = ComputeRmsLinear(samples, sample_count);
                    } else if (capture.SampleFormat() == AudioSampleFormat::Int16) {
                        const auto* samples = reinterpret_cast<const int16_t*>(buffer.bytes);
                        rms = ComputeRmsFromInt16(samples, sample_count);
                    }
                }
            }

            capture.ReleaseBuffer();

            if (callback_) {
                callback_(rms);
            }
        }

        capture.Shutdown();
    } catch (...) {
        if (!start_result_sent) {
            publish_start_result(false, "Unhandled exception in mic meter worker.");
        }
    }

    if (should_uninit) {
        CoUninitialize();
    }
    running_.store(false);
#endif
}

} // namespace recorder_core

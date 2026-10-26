#pragma once

#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace exosnap {
// Callback data never borrows the capture owner, including delivery after Close().
class WebcamReadCallback final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IMFSourceReaderCallback> {
  public:
    struct Sample {
        HRESULT status = S_OK;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        // Set when the result is a failed source event rather than a read.
        std::optional<MediaEventType> event;
    };

    STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG timestamp, IMFSample* sample) override {
        std::lock_guard lock(mutex_);
        if (!closed_) {
            pending_ = Sample{status, flags, timestamp, sample};
            changed_.notify_all();
        }
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override {
        return OnReadSample(HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED), 0, 0, 0, nullptr);
    }
    // Fatal reader errors arrive through OnReadSample. A failed forwarded
    // event is kept apart so it never replaces a read result still waiting
    // to be consumed; it is delivered after that result.
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent* event) override {
        HRESULT status = S_OK;
        if (!event || FAILED(event->GetStatus(&status)) || SUCCEEDED(status))
            return S_OK;
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        std::lock_guard lock(mutex_);
        if (!closed_ && !failed_event_) {
            failed_event_ = Sample{status, MF_SOURCE_READERF_ERROR, 0, nullptr, type};
            changed_.notify_all();
        }
        return S_OK;
    }

    bool Wait(std::stop_token stop, Sample& sample) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, stop, [this] { return closed_ || pending_ || failed_event_; });
        if (closed_ || stop.stop_requested())
            return false;
        auto& next = pending_ ? pending_ : failed_event_;
        sample = std::move(*std::exchange(next, std::nullopt));
        return true;
    }
    void Close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        pending_.reset();
        failed_event_.reset();
        changed_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::optional<Sample> pending_;
    std::optional<Sample> failed_event_;
    bool closed_ = false;
};
} // namespace exosnap

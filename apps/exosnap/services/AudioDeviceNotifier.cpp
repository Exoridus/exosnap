// clang-format off
#include <windows.h>
#include <mmdeviceapi.h>
// clang-format on

#include "services/AudioDeviceNotifier.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>

#include <atomic>
#include <mutex>

#include <wrl/client.h>

#include <recorder_core/audio_input_device.h>

#include "diagnostics/AppLog.h"

using Microsoft::WRL::ComPtr;

namespace exosnap {

// ---------------------------------------------------------------------------
// AudioDeviceSnapshot
// ---------------------------------------------------------------------------

bool AudioDeviceSnapshot::operator==(const AudioDeviceSnapshot& other) const noexcept {
    if (default_input_id != other.default_input_id || default_output_id != other.default_output_id) {
        return false;
    }
    if (inputs.size() != other.inputs.size() || outputs.size() != other.outputs.size()) {
        return false;
    }
    for (int i = 0; i < inputs.size(); ++i) {
        if (inputs[i].device_id != other.inputs[i].device_id ||
            inputs[i].display_name != other.inputs[i].display_name ||
            inputs[i].is_default != other.inputs[i].is_default) {
            return false;
        }
    }
    for (int i = 0; i < outputs.size(); ++i) {
        if (outputs[i].device_id != other.outputs[i].device_id ||
            outputs[i].display_name != other.outputs[i].display_name ||
            outputs[i].is_default != other.outputs[i].is_default) {
            return false;
        }
    }
    return true;
}

bool AudioDeviceSnapshot::operator!=(const AudioDeviceSnapshot& other) const noexcept {
    return !(*this == other);
}

// ---------------------------------------------------------------------------
// AudioEndpointNotificationClient
//
// A reference-counted COM object that receives IMMNotificationClient callbacks
// from Windows on an arbitrary MTA thread and marshals them safely to the Qt
// main thread via QMetaObject::invokeMethod(Qt::QueuedConnection).
//
// Lifetime safety
// ---------------
// * active_ is set to false in AudioDeviceNotifier::stop() before
//   UnregisterEndpointNotificationCallback() is called. After Unregister
//   returns, Windows will not invoke further callbacks on new events.
//   However, a callback that was already dispatched but not yet executed on
//   the MTA thread could still be in flight.
//
// * The mutex around the active_-check + dispatch closes that race:
//   stop() holds dispatch_mutex_ while clearing active_, so any callback
//   that passes the active_ read under the mutex will successfully queue the
//   invokeMethod before stop() proceeds. After the mutex is released by stop(),
//   all further callbacks see active_ == false and return immediately.
//
// * The QPointer<AudioDeviceNotifier> is checked inside the queued lambda so
//   that if the QObject happens to be destroyed between queueing and delivery,
//   the lambda is harmlessly discarded.
// ---------------------------------------------------------------------------

class AudioEndpointNotificationClient : public IMMNotificationClient {
  public:
    explicit AudioEndpointNotificationClient(AudioDeviceNotifier* notifier) : notifier_(notifier) {
    }

    // Called by AudioDeviceNotifier::stop() to prevent further dispatches.
    // Must be called with the COM enumerator still alive (before Release/Unregister).
    void deactivate() {
        std::lock_guard<std::mutex> lock(dispatch_mutex_);
        active_ = false;
    }

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++ref_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --ref_count_;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppvObject = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) override {
        dispatch(DiscoveryReason::DeviceAdded);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) override {
        dispatch(DiscoveryReason::DeviceRemoved);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/, DWORD /*dwNewState*/) override {
        dispatch(DiscoveryReason::DeviceStateChanged);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow /*flow*/, ERole /*role*/,
                                                     LPCWSTR /*pwstrDefaultDeviceId*/) override {
        dispatch(DiscoveryReason::DefaultChanged);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/, const PROPERTYKEY /*key*/) override {
        dispatch(DiscoveryReason::PropertyChanged);
        return S_OK;
    }

  private:
    void dispatch(DiscoveryReason reason) {
        // Hold the mutex while checking active_ and queueing the invocation.
        // This prevents a race with deactivate() in stop().
        std::lock_guard<std::mutex> lock(dispatch_mutex_);
        if (!active_) {
            return;
        }
        QPointer<AudioDeviceNotifier> guard(notifier_);
        QMetaObject::invokeMethod(
            notifier_,
            [guard, reason]() {
                if (guard) {
                    guard->scheduleRefresh(reason);
                }
            },
            Qt::QueuedConnection);
    }

    QPointer<AudioDeviceNotifier> notifier_;
    std::atomic<ULONG> ref_count_{1};
    std::atomic<bool> active_{true};
    std::mutex dispatch_mutex_;
};

// ---------------------------------------------------------------------------
// ComState — holds the COM objects (private to .cpp to keep COM out of .h)
// ---------------------------------------------------------------------------

struct AudioDeviceNotifier::ComState {
    ComPtr<IMMDeviceEnumerator> enumerator;
    AudioEndpointNotificationClient* client = nullptr; // COM-AddRef owned
};

// ---------------------------------------------------------------------------
// Default enumerator (production)
// ---------------------------------------------------------------------------

static AudioDeviceSnapshot DefaultEnumerator() {
    AudioDeviceSnapshot snap;
    const auto inputs_vec = recorder_core::EnumerateAudioInputDevices();
    snap.inputs = QVector<recorder_core::AudioInputDeviceInfo>(inputs_vec.begin(), inputs_vec.end());
    const auto outputs_vec = recorder_core::EnumerateAudioOutputDevices();
    snap.outputs = QVector<recorder_core::AudioInputDeviceInfo>(outputs_vec.begin(), outputs_vec.end());

    for (const auto& d : snap.inputs) {
        if (d.is_default) {
            snap.default_input_id = d.device_id;
            break;
        }
    }
    for (const auto& d : snap.outputs) {
        if (d.is_default) {
            snap.default_output_id = d.device_id;
            break;
        }
    }
    return snap;
}

// ---------------------------------------------------------------------------
// AudioDeviceNotifier — construction / destruction
// ---------------------------------------------------------------------------

AudioDeviceNotifier::AudioDeviceNotifier(QObject* parent)
    : QObject(parent), enumerator_(&DefaultEnumerator), com_state_(std::make_unique<ComState>()) {
    debounce_timer_.setSingleShot(true);
    debounce_timer_.setInterval(200);
    connect(&debounce_timer_, &QTimer::timeout, this, [this]() { refreshNow(pending_reason_); });
}

AudioDeviceNotifier::~AudioDeviceNotifier() {
    stop();
}

// ---------------------------------------------------------------------------
// Test-support
// ---------------------------------------------------------------------------

void AudioDeviceNotifier::setEnumeratorForTest(Enumerator enumerator) {
    enumerator_ = std::move(enumerator);
    test_mode_ = true;
}

void AudioDeviceNotifier::setDebounceIntervalMsForTest(int ms) {
    debounce_timer_.setInterval(ms);
}

void AudioDeviceNotifier::simulateNativeEvent(DiscoveryReason reason) {
    // Drive the same marshaled path a real COM callback would follow:
    // post a QueuedConnection call to scheduleRefresh so that
    // debounce/coalescing/dedup logic is exercised in the test.
    QPointer<AudioDeviceNotifier> guard(this);
    QMetaObject::invokeMethod(
        this,
        [guard, reason]() {
            if (guard) {
                guard->scheduleRefresh(reason);
            }
        },
        Qt::QueuedConnection);
}

void AudioDeviceNotifier::flushPendingForTest() {
    // Process queued events until the timer fires and the refresh completes.
    // With debounce interval == 0, a single processEvents pass is enough to:
    //   1. deliver the queued scheduleRefresh call(s)
    //   2. start the 0-ms timer
    //   3. fire the timer and run refreshNow
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    if (debounce_timer_.isActive()) {
        debounce_timer_.stop();
        refreshNow(pending_reason_);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AudioDeviceNotifier::start() {
    if (started_) {
        return;
    }

    started_ = true;

    if (test_mode_) {
        // Test mode: no COM registration, just mark as started.
        return;
    }

    // Production: register IMMNotificationClient.
    // Qt has already initialized COM (STA) on the main thread; do NOT call
    // CoInitializeEx/CoUninitialize here — that would unbalance Qt's COM init.
    HRESULT hr =
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&com_state_->enumerator));
    if (FAILED(hr) || !com_state_->enumerator) {
        diagnostics::AppLog::warning(QStringLiteral("AudioDiscovery"),
                                     QStringLiteral("CoCreateInstance(MMDeviceEnumerator) failed hr=0x%1")
                                         .arg(static_cast<unsigned long>(hr), 8, 16, QChar('0')));
        return;
    }

    com_state_->client = new AudioEndpointNotificationClient(this);
    hr = com_state_->enumerator->RegisterEndpointNotificationCallback(com_state_->client);
    if (FAILED(hr)) {
        diagnostics::AppLog::warning(QStringLiteral("AudioDiscovery"),
                                     QStringLiteral("RegisterEndpointNotificationCallback failed hr=0x%1")
                                         .arg(static_cast<unsigned long>(hr), 8, 16, QChar('0')));
        com_state_->client->Release();
        com_state_->client = nullptr;
        com_state_->enumerator.Reset();
    }
}

void AudioDeviceNotifier::stop() {
    if (!started_) {
        return;
    }
    started_ = false;
    debounce_timer_.stop();

    if (com_state_->client) {
        // Deactivate first: no more dispatches will be queued after this returns.
        com_state_->client->deactivate();
        if (com_state_->enumerator) {
            com_state_->enumerator->UnregisterEndpointNotificationCallback(com_state_->client);
        }
        com_state_->client->Release();
        com_state_->client = nullptr;
    }
    com_state_->enumerator.Reset();
}

// ---------------------------------------------------------------------------
// Immediate synchronous refresh
// ---------------------------------------------------------------------------

void AudioDeviceNotifier::rescan() {
    refreshNow(DiscoveryReason::Rescan);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

AudioDeviceSnapshot AudioDeviceNotifier::currentSnapshot() const {
    return last_snapshot_;
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void AudioDeviceNotifier::scheduleRefresh(DiscoveryReason reason) {
    // Prioritise DeviceRemoved so that the UI can react to disappearing
    // devices even if a subsequent PropertyChanged overwrites the reason.
    if (!debounce_timer_.isActive() || reason == DiscoveryReason::DeviceRemoved) {
        pending_reason_ = reason;
    }
    debounce_timer_.start();
}

void AudioDeviceNotifier::refreshNow(DiscoveryReason reason) {
    const AudioDeviceSnapshot fresh = buildSnapshot();
    if (fresh == last_snapshot_) {
        return;
    }
    last_snapshot_ = fresh;

    diagnostics::AppLog::info(QStringLiteral("AudioDiscovery"),
                              QStringLiteral("Snapshot changed — inputs:%1 outputs:%2 reason:%3")
                                  .arg(fresh.inputs.size())
                                  .arg(fresh.outputs.size())
                                  .arg(QLatin1StringView(DiscoveryReasonName(reason))));

    emit snapshotChanged(last_snapshot_, reason);
}

AudioDeviceSnapshot AudioDeviceNotifier::buildSnapshot() const {
    AudioDeviceSnapshot snap = enumerator_();

    std::sort(snap.inputs.begin(), snap.inputs.end(),
              [](const recorder_core::AudioInputDeviceInfo& a, const recorder_core::AudioInputDeviceInfo& b) {
                  return a.device_id < b.device_id;
              });
    std::sort(snap.outputs.begin(), snap.outputs.end(),
              [](const recorder_core::AudioInputDeviceInfo& a, const recorder_core::AudioInputDeviceInfo& b) {
                  return a.device_id < b.device_id;
              });

    // Ensure default IDs are derived from the sorted, canonical list.
    snap.default_input_id.clear();
    snap.default_output_id.clear();
    for (const auto& d : snap.inputs) {
        if (d.is_default) {
            snap.default_input_id = d.device_id;
            break;
        }
    }
    for (const auto& d : snap.outputs) {
        if (d.is_default) {
            snap.default_output_id = d.device_id;
            break;
        }
    }

    return snap;
}

} // namespace exosnap

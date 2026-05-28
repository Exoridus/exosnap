#include "wasapi_process_loopback_src.h"

#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <propidl.h>

#include <cstdio>
#include <mutex>
#include <new>

namespace recorder_core {

namespace {

constexpr uint32_t kProcessLoopbackSampleRate = 48000;
constexpr uint32_t kProcessLoopbackChannels = 2;
constexpr REFERENCE_TIME kProcessLoopbackBufferDurationHns = 2000000LL; // 200 ms
constexpr DWORD kProcessLoopbackActivationTimeoutMs = 5000;

using PFN_ActivateAudioInterfaceAsync = HRESULT(STDAPICALLTYPE*)(LPCWSTR, REFIID, PROPVARIANT*,
                                                                 IActivateAudioInterfaceCompletionHandler*,
                                                                 IActivateAudioInterfaceAsyncOperation**);

HMODULE g_mmdevapi = nullptr;
PFN_ActivateAudioInterfaceAsync g_activate_audio_interface_async = nullptr;
std::once_flag g_mmdevapi_once;
HRESULT g_mmdevapi_load_hr = E_FAIL;

static HRESULT InvokeActivateAudioInterfaceAsync(LPCWSTR path, REFIID riid, PROPVARIANT* params,
                                                 IActivateAudioInterfaceCompletionHandler* handler,
                                                 IActivateAudioInterfaceAsyncOperation** op,
                                                 PFN_ActivateAudioInterfaceAsync pfn) noexcept {
    if (pfn == nullptr) {
        return E_POINTER;
    }

    HRESULT hr = E_FAIL;
    __try {
        hr = pfn(path, riid, params, handler, op);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = E_UNEXPECTED;
    }
    return hr;
}

HRESULT EnsureActivateAudioInterfaceAsyncLoaded(PFN_ActivateAudioInterfaceAsync& out_pfn) {
    std::call_once(g_mmdevapi_once, [] {
        g_mmdevapi = LoadLibraryW(L"mmdevapi.dll");
        if (g_mmdevapi == nullptr) {
            g_mmdevapi_load_hr = HRESULT_FROM_WIN32(GetLastError());
            return;
        }

        g_activate_audio_interface_async = reinterpret_cast<PFN_ActivateAudioInterfaceAsync>(
            GetProcAddress(g_mmdevapi, "ActivateAudioInterfaceAsync"));
        if (g_activate_audio_interface_async == nullptr) {
            g_mmdevapi_load_hr = HRESULT_FROM_WIN32(GetLastError());
            return;
        }

        g_mmdevapi_load_hr = S_OK;
    });

    out_pfn = g_activate_audio_interface_async;
    return g_mmdevapi_load_hr;
}

class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
  public:
    ActivationHandler() : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        const HRESULT ftm_hr =
            CoCreateFreeThreadedMarshaler(static_cast<IActivateAudioInterfaceCompletionHandler*>(this), &ftm_);
        if (FAILED(ftm_hr)) {
            ftm_ = nullptr;
        }
    }

    ~ActivationHandler() {
        if (event_ != nullptr) {
            CloseHandle(event_);
            event_ = nullptr;
        }
        if (activated_object_ != nullptr) {
            activated_object_->Release();
            activated_object_ = nullptr;
        }
        if (ftm_ != nullptr) {
            ftm_->Release();
            ftm_ = nullptr;
        }
    }

    ActivationHandler(const ActivationHandler&) = delete;
    ActivationHandler& operator=(const ActivationHandler&) = delete;

    bool IsAgile() const {
        return event_ != nullptr && ftm_ != nullptr;
    }

    HANDLE EventHandle() const {
        return event_;
    }

    HRESULT GetActivateCallResult() const {
        return get_activate_result_hr_;
    }

    HRESULT GetActivateResultCode() const {
        return activate_result_;
    }

    IUnknown* ActivatedObject() const {
        return activated_object_;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        IUnknown* activated = nullptr;
        if (operation == nullptr) {
            get_activate_result_hr_ = E_POINTER;
            activate_result_ = E_POINTER;
        } else {
            get_activate_result_hr_ = operation->GetActivateResult(&activate_result_, &activated);
            if (SUCCEEDED(get_activate_result_hr_)) {
                if (activated_object_ != nullptr) {
                    activated_object_->Release();
                }
                activated_object_ = activated;
            } else if (activated != nullptr) {
                activated->Release();
            }
        }

        if (event_ != nullptr) {
            SetEvent(event_);
        }
        return get_activate_result_hr_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) {
            return E_POINTER;
        }

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }

        if (ftm_ != nullptr) {
            return ftm_->QueryInterface(riid, ppv);
        }

        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG ref = InterlockedDecrement(&ref_count_);
        if (ref == 0) {
            delete this;
        }
        return static_cast<ULONG>(ref);
    }

  private:
    HANDLE event_ = nullptr;
    LONG ref_count_ = 1;
    IUnknown* ftm_ = nullptr;
    HRESULT get_activate_result_hr_ = E_FAIL;
    HRESULT activate_result_ = E_FAIL;
    IUnknown* activated_object_ = nullptr;
};

const char* ModeLabel(ProcessLoopbackMode mode) {
    switch (mode) {
    case ProcessLoopbackMode::IncludeProcessTree:
        return "APP include target process tree";
    case ProcessLoopbackMode::ExcludeProcessTree:
        return "SYS exclude target process tree";
    default:
        return "process loopback";
    }
}

PROCESS_LOOPBACK_MODE ToProcessLoopbackMode(ProcessLoopbackMode mode) {
    switch (mode) {
    case ProcessLoopbackMode::IncludeProcessTree:
        return PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    case ProcessLoopbackMode::ExcludeProcessTree:
        return PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    default:
        return PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    }
}

} // namespace

WasapiProcessLoopbackSrc::WasapiProcessLoopbackSrc(DWORD target_pid, ProcessLoopbackMode mode)
    : pid_(target_pid), mode_(mode) {
    char name[128] = {};
    snprintf(name, sizeof(name), "Process loopback (%s, pid=%lu)", ModeLabel(mode_), static_cast<unsigned long>(pid_));
    endpoint_name_ = name;
}

WasapiProcessLoopbackSrc::~WasapiProcessLoopbackSrc() {
    Shutdown();
}

bool WasapiProcessLoopbackSrc::Init(std::string& out_error) {
    Shutdown();
    out_error.clear();

    auto fail = [&](const char* context, HRESULT hr) -> bool {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s 0x%08lX", context, static_cast<unsigned long>(hr));
        out_error = buf;
        Shutdown();
        return false;
    };

    PFN_ActivateAudioInterfaceAsync activate_pfn = nullptr;
    const HRESULT load_hr = EnsureActivateAudioInterfaceAsyncLoaded(activate_pfn);
    if (FAILED(load_hr) || activate_pfn == nullptr) {
        return fail("Process loopback: failed to load ActivateAudioInterfaceAsync", load_hr);
    }

    AUDIOCLIENT_ACTIVATION_PARAMS activation_params{};
    activation_params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activation_params.ProcessLoopbackParams.TargetProcessId = pid_;
    activation_params.ProcessLoopbackParams.ProcessLoopbackMode = ToProcessLoopbackMode(mode_);

    PROPVARIANT activate_params;
    PropVariantInit(&activate_params);
    activate_params.vt = VT_BLOB;
    activate_params.blob.pBlobData = reinterpret_cast<BYTE*>(&activation_params);
    activate_params.blob.cbSize = sizeof(activation_params);

    ActivationHandler* handler = new (std::nothrow) ActivationHandler();
    if (handler == nullptr) {
        out_error = "Process loopback: failed to allocate activation handler";
        return false;
    }
    if (!handler->IsAgile()) {
        handler->Release();
        out_error = "Process loopback activation handler is not agile (FTM unavailable)";
        return false;
    }

    IActivateAudioInterfaceAsyncOperation* async_op = nullptr;
    const HRESULT activate_hr =
        InvokeActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                          &activate_params, handler, &async_op, activate_pfn);

    if (FAILED(activate_hr)) {
        if (async_op != nullptr) {
            async_op->Release();
            async_op = nullptr;
        }
        handler->Release();
        return fail("ActivateAudioInterfaceAsync(process loopback)", activate_hr);
    }

    const DWORD wait_result = WaitForSingleObject(handler->EventHandle(), kProcessLoopbackActivationTimeoutMs);
    if (wait_result != WAIT_OBJECT_0) {
        if (async_op != nullptr) {
            async_op->Release();
            async_op = nullptr;
        }
        handler->Release();
        out_error = "Process loopback activation timeout after 5000 ms";
        Shutdown();
        return false;
    }

    if (async_op != nullptr) {
        async_op->Release();
        async_op = nullptr;
    }

    const HRESULT get_activate_result_hr = handler->GetActivateCallResult();
    if (FAILED(get_activate_result_hr)) {
        handler->Release();
        return fail("Process loopback GetActivateResult failed", get_activate_result_hr);
    }

    const HRESULT activation_result = handler->GetActivateResultCode();
    if (FAILED(activation_result)) {
        handler->Release();
        return fail("Process loopback activation result failed", activation_result);
    }

    IUnknown* activated_object = handler->ActivatedObject();
    if (activated_object == nullptr) {
        handler->Release();
        out_error = "Process loopback activation returned null interface";
        Shutdown();
        return false;
    }

    HRESULT hr = activated_object->QueryInterface(IID_PPV_ARGS(&audio_client_));
    handler->Release();
    if (FAILED(hr)) {
        return fail("Process loopback QueryInterface(IAudioClient)", hr);
    }
    if (audio_client_ == nullptr) {
        out_error = "Process loopback QueryInterface succeeded but IAudioClient was null";
        Shutdown();
        return false;
    }

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = static_cast<WORD>(kProcessLoopbackChannels);
    fmt.nSamplesPerSec = kProcessLoopbackSampleRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    fmt.cbSize = 0;

    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                   kProcessLoopbackBufferDurationHns, 0, &fmt, nullptr);
    if (FAILED(hr)) {
        return fail("Process loopback IAudioClient::Initialize", hr);
    }

    hr = audio_client_->GetService(IID_PPV_ARGS(&capture_client_));
    if (FAILED(hr)) {
        return fail("Process loopback IAudioClient::GetService(IAudioCaptureClient)", hr);
    }
    if (capture_client_ == nullptr) {
        out_error = "Process loopback GetService succeeded but IAudioCaptureClient was null";
        Shutdown();
        return false;
    }

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        return fail("Process loopback IAudioClient::Start", hr);
    }

    pending_capture_error_ = false;
    pending_capture_error_msg_.clear();
    return true;
}

uint32_t WasapiProcessLoopbackSrc::PendingFrameCount() {
    if (capture_client_ == nullptr) {
        return 0;
    }

    UINT32 frames = 0;
    const HRESULT hr = capture_client_->GetNextPacketSize(&frames);
    if (FAILED(hr)) {
        char buf[220];
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            snprintf(buf, sizeof(buf),
                     "Process loopback GetNextPacketSize failed: AUDCLNT_E_DEVICE_INVALIDATED (0x%08lX)",
                     static_cast<unsigned long>(hr));
        } else {
            snprintf(buf, sizeof(buf), "Process loopback GetNextPacketSize failed 0x%08lX",
                     static_cast<unsigned long>(hr));
        }
        pending_capture_error_ = true;
        pending_capture_error_msg_ = buf;
        return 1;
    }

    pending_capture_error_ = false;
    pending_capture_error_msg_.clear();
    return frames;
}

bool WasapiProcessLoopbackSrc::AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) {
    out_buf = {};
    out_error.clear();

    if (pending_capture_error_) {
        out_error = pending_capture_error_msg_;
        pending_capture_error_ = false;
        pending_capture_error_msg_.clear();
        return false;
    }

    if (capture_client_ == nullptr) {
        out_error = "WasapiProcessLoopbackSrc::AcquireBuffer called before Init";
        return false;
    }

    if (buffer_acquired_) {
        out_error = "WasapiProcessLoopbackSrc::AcquireBuffer called while a packet is already held";
        return false;
    }

    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    HRESULT hr = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
    if (hr == AUDCLNT_S_BUFFER_EMPTY) {
        return false;
    }
    if (FAILED(hr)) {
        char buf[256];
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            snprintf(buf, sizeof(buf),
                     "Process loopback device invalidated: IAudioCaptureClient::GetBuffer failed 0x%08lX",
                     static_cast<unsigned long>(hr));
        } else {
            snprintf(buf, sizeof(buf), "Process loopback IAudioCaptureClient::GetBuffer failed 0x%08lX",
                     static_cast<unsigned long>(hr));
        }
        out_error = buf;
        return false;
    }
    if (frames == 0) {
        return false;
    }

    buffer_acquired_ = true;
    acquired_frames_ = frames;

    out_buf.bytes = reinterpret_cast<const uint8_t*>(data);
    out_buf.num_frames = frames;
    out_buf.silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    out_buf.data_discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
    return true;
}

void WasapiProcessLoopbackSrc::ReleaseBuffer() {
    if (!buffer_acquired_) {
        return;
    }

    if (capture_client_ != nullptr) {
        capture_client_->ReleaseBuffer(acquired_frames_);
    }

    buffer_acquired_ = false;
    acquired_frames_ = 0;
}

uint32_t WasapiProcessLoopbackSrc::SampleRate() const {
    return kProcessLoopbackSampleRate;
}

uint32_t WasapiProcessLoopbackSrc::Channels() const {
    return kProcessLoopbackChannels;
}

AudioSampleFormat WasapiProcessLoopbackSrc::SampleFormat() const {
    return AudioSampleFormat::Int16;
}

const std::string& WasapiProcessLoopbackSrc::EndpointName() const {
    return endpoint_name_;
}

void WasapiProcessLoopbackSrc::Shutdown() {
    ReleaseBuffer();
    pending_capture_error_ = false;
    pending_capture_error_msg_.clear();

    if (capture_client_ != nullptr) {
        capture_client_->Release();
        capture_client_ = nullptr;
    }

    if (audio_client_ != nullptr) {
        audio_client_->Stop();
        audio_client_->Release();
        audio_client_ = nullptr;
    }
}

} // namespace recorder_core

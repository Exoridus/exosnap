#include "wasapi_capture_src.h"

#include "wgc_capture.h"

#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cstring>

namespace recorder_core {

namespace {

constexpr uint32_t kRequiredSampleRate = 48000;
constexpr uint32_t kRequiredOutputChannels = 2;
constexpr REFERENCE_TIME kFallbackDevicePeriodHns = 100000; // 10 ms

enum class AcceptedSampleKind { Float32, Pcm16, Unsupported };

WORD ResolveWaveFormatTag(const WAVEFORMATEX* fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return static_cast<WORD>(ext->SubFormat.Data1);
    }
    return fmt->wFormatTag;
}

AcceptedSampleKind DetectAcceptedSampleKind(const WAVEFORMATEX* fmt) {
    const WORD tag = ResolveWaveFormatTag(fmt);
    if (tag == WAVE_FORMAT_IEEE_FLOAT && fmt->wBitsPerSample == 32) {
        return AcceptedSampleKind::Float32;
    }
    if (tag == WAVE_FORMAT_PCM && fmt->wBitsPerSample == 16) {
        return AcceptedSampleKind::Pcm16;
    }
    return AcceptedSampleKind::Unsupported;
}

} // namespace

WasapiCaptureSrc::~WasapiCaptureSrc() {
    Shutdown();
}

bool WasapiCaptureSrc::Init(std::string& out_error) {
    Shutdown();
    out_error.clear();

    WAVEFORMATEX* mixFormat = nullptr;
    WAVEFORMATEX* closestMatch = nullptr;
    IMMDeviceEnumerator* enumerator = nullptr;

    auto cleanupFormats = [&]() {
        if (closestMatch) {
            CoTaskMemFree(closestMatch);
            closestMatch = nullptr;
        }
        if (mixFormat) {
            CoTaskMemFree(mixFormat);
            mixFormat = nullptr;
        }
    };

    auto failHr = [&](const char* context, HRESULT hr) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s 0x%08lX", context, static_cast<unsigned long>(hr));
        out_error = buf;
        cleanupFormats();
        if (enumerator) {
            enumerator->Release();
            enumerator = nullptr;
        }
        Shutdown();
        return false;
    };

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        return failHr("CoCreateInstance(MMDeviceEnumerator)", hr);
    }

    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device_);
    enumerator->Release();
    enumerator = nullptr;
    if (FAILED(hr)) {
        return failHr("GetDefaultAudioEndpoint(eCapture,eConsole)", hr);
    }

    // Friendly name
    {
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR) {
                endpoint_name_ = WideToUtf8(var.pwszVal);
            }
            PropVariantClear(&var);
            props->Release();
        }
        if (endpoint_name_.empty()) {
            endpoint_name_ = "(unnamed)";
        }
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr)) {
        return failHr("Activate(IAudioClient)", hr);
    }

    hr = audio_client_->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        return failHr("GetMixFormat", hr);
    }

    WAVEFORMATEX desired = {};
    desired.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    desired.nChannels = kRequiredOutputChannels;
    desired.nSamplesPerSec = kRequiredSampleRate;
    desired.wBitsPerSample = 32;
    desired.nBlockAlign = desired.nChannels * (desired.wBitsPerSample / 8);
    desired.nAvgBytesPerSec = desired.nSamplesPerSec * desired.nBlockAlign;
    desired.cbSize = 0;

    hr = audio_client_->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &desired, &closestMatch);

    const WAVEFORMATEX* initFormat = nullptr;
    AcceptedSampleKind selectedKind = AcceptedSampleKind::Float32;
    uint32_t selectedInputChannels = 2;
    bool selectedMonoToStereo = false;

    if (hr == S_OK) {
        initFormat = &desired;
        selectedKind = AcceptedSampleKind::Float32;
        selectedInputChannels = 2;
        selectedMonoToStereo = false;
    } else if (hr == S_FALSE) {
        if (!closestMatch) {
            out_error =
                "Mic format check returned S_FALSE without closest match; cannot initialize default microphone stream.";
            cleanupFormats();
            Shutdown();
            return false;
        }

        if (closestMatch->nSamplesPerSec != kRequiredSampleRate) {
            char buf[196];
            snprintf(buf, sizeof(buf),
                     "Mic format %uHz not directly supported in Phase 4; set Windows default input format to 48kHz.",
                     closestMatch->nSamplesPerSec);
            out_error = buf;
            cleanupFormats();
            Shutdown();
            return false;
        }

        const AcceptedSampleKind closestKind = DetectAcceptedSampleKind(closestMatch);
        if (closestMatch->nChannels == 1 &&
            (closestKind == AcceptedSampleKind::Float32 || closestKind == AcceptedSampleKind::Pcm16)) {
            initFormat = closestMatch;
            selectedKind = closestKind;
            selectedInputChannels = 1;
            selectedMonoToStereo = true;
        } else if (closestMatch->nChannels == 2 && closestKind == AcceptedSampleKind::Pcm16) {
            initFormat = closestMatch;
            selectedKind = AcceptedSampleKind::Pcm16;
            selectedInputChannels = 2;
            selectedMonoToStereo = false;
        } else {
            out_error = "Mic format incompatible - Phase 4 requires 48kHz float32 or 48kHz PCM16 mono/stereo.";
            cleanupFormats();
            Shutdown();
            return false;
        }
    } else {
        return failHr("IAudioClient::IsFormatSupported(shared)", hr);
    }

    REFERENCE_TIME hnsDefaultDevicePeriod = 0;
    hr = audio_client_->GetDevicePeriod(&hnsDefaultDevicePeriod, nullptr);
    if (FAILED(hr)) {
        return failHr("IAudioClient::GetDevicePeriod", hr);
    }
    if (hnsDefaultDevicePeriod == 0) {
        hnsDefaultDevicePeriod = kFallbackDevicePeriodHns;
    }

    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsDefaultDevicePeriod, 0, initFormat, nullptr);
    if (FAILED(hr)) {
        return failHr("IAudioClient::Initialize(capture)", hr);
    }

    hr = audio_client_->GetService(IID_PPV_ARGS(&capture_client_));
    if (FAILED(hr)) {
        return failHr("GetService(IAudioCaptureClient)", hr);
    }

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        return failHr("IAudioClient::Start", hr);
    }

    cleanupFormats();

    mono_to_stereo_ = selectedMonoToStereo;
    sample_rate_ = kRequiredSampleRate;
    channels_ = kRequiredOutputChannels;
    input_channels_ = selectedInputChannels;
    sample_format_ =
        (selectedKind == AcceptedSampleKind::Pcm16) ? AudioSampleFormat::Int16 : AudioSampleFormat::Float32;
    pending_capture_error_ = false;
    pending_capture_error_msg_.clear();

    return true;
}

uint32_t WasapiCaptureSrc::PendingFrameCount() {
    if (!capture_client_) {
        return 0;
    }

    UINT32 frames = 0;
    HRESULT hr = capture_client_->GetNextPacketSize(&frames);
    if (FAILED(hr)) {
        char buf[160];
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            snprintf(buf, sizeof(buf),
                     "IAudioCaptureClient::GetNextPacketSize failed: AUDCLNT_E_DEVICE_INVALIDATED (0x%08lX)",
                     static_cast<unsigned long>(hr));
        } else {
            snprintf(buf, sizeof(buf), "IAudioCaptureClient::GetNextPacketSize failed 0x%08lX",
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

bool WasapiCaptureSrc::AcquireBuffer(RawAudioBuffer& out_buf, std::string& out_error) {
    out_buf = {};

    if (pending_capture_error_) {
        out_error = pending_capture_error_msg_;
        pending_capture_error_ = false;
        pending_capture_error_msg_.clear();
        return false;
    }

    if (!capture_client_) {
        out_error = "WasapiCaptureSrc::AcquireBuffer called before Init";
        return false;
    }

    if (buffer_acquired_) {
        out_error = "WasapiCaptureSrc::AcquireBuffer called while a packet is already held";
        return false;
    }

    BYTE* data = nullptr;
    UINT32 numFrames = 0;
    DWORD captureFlags = 0;
    UINT64 devicePos = 0;
    UINT64 qpcPos = 0;
    HRESULT hr = capture_client_->GetBuffer(&data, &numFrames, &captureFlags, &devicePos, &qpcPos);
    if (hr == AUDCLNT_S_BUFFER_EMPTY) {
        out_error.clear();
        return false;
    }
    if (FAILED(hr)) {
        char buf[160];
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            snprintf(buf, sizeof(buf), "IAudioCaptureClient::GetBuffer failed: AUDCLNT_E_DEVICE_INVALIDATED (0x%08lX)",
                     static_cast<unsigned long>(hr));
        } else {
            snprintf(buf, sizeof(buf), "IAudioCaptureClient::GetBuffer failed 0x%08lX", static_cast<unsigned long>(hr));
        }
        out_error = buf;
        return false;
    }
    if (numFrames == 0) {
        out_error.clear();
        return false;
    }

    const bool silent = (captureFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

    buffer_acquired_ = true;
    acquired_frames_ = numFrames;

    if (!mono_to_stereo_) {
        out_buf.bytes = reinterpret_cast<const uint8_t*>(data);
        out_buf.num_frames = numFrames;
        out_buf.silent = silent;
        return true;
    }

    const size_t outputSamples = static_cast<size_t>(numFrames) * kRequiredOutputChannels;
    if (sample_format_ == AudioSampleFormat::Float32) {
        mono_expand_buffer_.resize(outputSamples * sizeof(float));
        float* dst = reinterpret_cast<float*>(mono_expand_buffer_.data());
        if (silent || data == nullptr) {
            std::memset(dst, 0, mono_expand_buffer_.size());
        } else {
            const float* src = reinterpret_cast<const float*>(data);
            for (size_t i = 0; i < static_cast<size_t>(numFrames); ++i) {
                const float s = src[i];
                dst[(i * 2) + 0] = s;
                dst[(i * 2) + 1] = s;
            }
        }
    } else {
        mono_expand_buffer_.resize(outputSamples * sizeof(int16_t));
        int16_t* dst = reinterpret_cast<int16_t*>(mono_expand_buffer_.data());
        if (silent || data == nullptr) {
            std::memset(dst, 0, mono_expand_buffer_.size());
        } else {
            const int16_t* src = reinterpret_cast<const int16_t*>(data);
            for (size_t i = 0; i < static_cast<size_t>(numFrames); ++i) {
                const int16_t s = src[i];
                dst[(i * 2) + 0] = s;
                dst[(i * 2) + 1] = s;
            }
        }
    }

    out_buf.bytes = mono_expand_buffer_.data();
    out_buf.num_frames = numFrames;
    out_buf.silent = silent;
    return true;
}

void WasapiCaptureSrc::ReleaseBuffer() {
    if (!buffer_acquired_) {
        return;
    }

    if (capture_client_) {
        capture_client_->ReleaseBuffer(acquired_frames_);
    }

    buffer_acquired_ = false;
    acquired_frames_ = 0;
}

uint32_t WasapiCaptureSrc::SampleRate() const {
    return sample_rate_;
}

uint32_t WasapiCaptureSrc::Channels() const {
    return channels_;
}

AudioSampleFormat WasapiCaptureSrc::SampleFormat() const {
    return sample_format_;
}

const std::string& WasapiCaptureSrc::EndpointName() const {
    return endpoint_name_;
}

void WasapiCaptureSrc::Shutdown() {
    ReleaseBuffer();
    mono_expand_buffer_.clear();
    pending_capture_error_ = false;
    pending_capture_error_msg_.clear();

    if (capture_client_) {
        capture_client_->Release();
        capture_client_ = nullptr;
    }

    if (audio_client_) {
        audio_client_->Stop();
        audio_client_->Release();
        audio_client_ = nullptr;
    }

    if (device_) {
        device_->Release();
        device_ = nullptr;
    }

    input_channels_ = 0;
    sample_rate_ = 0;
    channels_ = 0;
    mono_to_stereo_ = false;
}

} // namespace recorder_core

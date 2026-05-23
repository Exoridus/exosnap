#include "wasapi_capture_src.h"

#include "wgc_capture.h"

#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace recorder_core {

namespace {

constexpr uint32_t kRequiredSampleRate = 48000;
constexpr uint32_t kRequiredOutputChannels = 2;
constexpr REFERENCE_TIME kFallbackDevicePeriodHns = 100000; // 10 ms
constexpr uint64_t kAutoDetectMinFrames = 4800;             // 100 ms

enum class AcceptedSampleKind { Float32, Pcm16, Unsupported };

int16_t ClampInt32ToInt16(int32_t value) {
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(value);
}

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

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring converted(static_cast<size_t>(required), L'\0');
    const int written =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), converted.data(), required);
    if (written != required) {
        return {};
    }

    return converted;
}

} // namespace

namespace mic_channel_detail {

void MapStereoFrameFloat32(MicChannelMode mode, float input_l, float input_r, float& output_l, float& output_r) {
    switch (mode) {
    case MicChannelMode::Auto:
        output_l = input_l;
        output_r = input_r;
        return;
    case MicChannelMode::PreserveStereo:
        output_l = input_l;
        output_r = input_r;
        return;
    case MicChannelMode::MonoMix: {
        const float mixed = 0.5f * (input_l + input_r);
        output_l = mixed;
        output_r = mixed;
        return;
    }
    case MicChannelMode::LeftToStereo:
        output_l = input_l;
        output_r = input_l;
        return;
    case MicChannelMode::RightToStereo:
        output_l = input_r;
        output_r = input_r;
        return;
    default:
        output_l = input_l;
        output_r = input_r;
        return;
    }
}

void MapStereoFrameInt16(MicChannelMode mode, int16_t input_l, int16_t input_r, int16_t& output_l, int16_t& output_r) {
    switch (mode) {
    case MicChannelMode::Auto:
        output_l = input_l;
        output_r = input_r;
        return;
    case MicChannelMode::PreserveStereo:
        output_l = input_l;
        output_r = input_r;
        return;
    case MicChannelMode::MonoMix: {
        const int32_t mixed = (static_cast<int32_t>(input_l) + static_cast<int32_t>(input_r)) / 2;
        const int16_t clamped = ClampInt32ToInt16(mixed);
        output_l = clamped;
        output_r = clamped;
        return;
    }
    case MicChannelMode::LeftToStereo:
        output_l = input_l;
        output_r = input_l;
        return;
    case MicChannelMode::RightToStereo:
        output_l = input_r;
        output_r = input_r;
        return;
    default:
        output_l = input_l;
        output_r = input_r;
        return;
    }
}

MicChannelMode ResolveAutoChannelMode(double rms_left, double rms_right) {
    constexpr double kSilenceFloor = 0.0025;    // about -52 dBFS
    constexpr double kDominanceRatio = 4.0;     // ~12 dB
    constexpr double kNearSilenceFactor = 0.35; // side channel effectively silent

    if (rms_left < kSilenceFloor && rms_right < kSilenceFloor) {
        return MicChannelMode::MonoMix;
    }

    if (rms_left >= kSilenceFloor && rms_right < (kSilenceFloor * kNearSilenceFactor)) {
        return MicChannelMode::LeftToStereo;
    }
    if (rms_right >= kSilenceFloor && rms_left < (kSilenceFloor * kNearSilenceFactor)) {
        return MicChannelMode::RightToStereo;
    }

    if (rms_right > 0.0 && (rms_left / rms_right) >= kDominanceRatio) {
        return MicChannelMode::LeftToStereo;
    }
    if (rms_left > 0.0 && (rms_right / rms_left) >= kDominanceRatio) {
        return MicChannelMode::RightToStereo;
    }

    return MicChannelMode::PreserveStereo;
}

void UpdateAutoModeStateFloat32(AutoModeState& state, const float* interleaved_stereo, uint32_t num_frames,
                                bool silent) {
    if (state.locked || num_frames == 0) {
        return;
    }

    if (!silent && interleaved_stereo != nullptr) {
        for (size_t i = 0; i < static_cast<size_t>(num_frames); ++i) {
            const double l = static_cast<double>(interleaved_stereo[(i * 2) + 0]);
            const double r = static_cast<double>(interleaved_stereo[(i * 2) + 1]);
            state.energy_left += (l * l);
            state.energy_right += (r * r);
        }
    }

    state.analyzed_frames += num_frames;
    if (state.analyzed_frames < kAutoDetectMinFrames) {
        return;
    }

    const double denom = static_cast<double>(std::max<uint64_t>(1u, state.analyzed_frames));
    const double rms_left = std::sqrt(state.energy_left / denom);
    const double rms_right = std::sqrt(state.energy_right / denom);
    state.resolved_mode = ResolveAutoChannelMode(rms_left, rms_right);
    state.locked = true;
}

void UpdateAutoModeStateInt16(AutoModeState& state, const int16_t* interleaved_stereo, uint32_t num_frames,
                              bool silent) {
    if (state.locked || num_frames == 0) {
        return;
    }

    if (!silent && interleaved_stereo != nullptr) {
        for (size_t i = 0; i < static_cast<size_t>(num_frames); ++i) {
            const double l = static_cast<double>(interleaved_stereo[(i * 2) + 0]) / 32768.0;
            const double r = static_cast<double>(interleaved_stereo[(i * 2) + 1]) / 32768.0;
            state.energy_left += (l * l);
            state.energy_right += (r * r);
        }
    }

    state.analyzed_frames += num_frames;
    if (state.analyzed_frames < kAutoDetectMinFrames) {
        return;
    }

    const double denom = static_cast<double>(std::max<uint64_t>(1u, state.analyzed_frames));
    const double rms_left = std::sqrt(state.energy_left / denom);
    const double rms_right = std::sqrt(state.energy_right / denom);
    state.resolved_mode = ResolveAutoChannelMode(rms_left, rms_right);
    state.locked = true;
}

MicChannelMode EffectiveAutoMode(const AutoModeState& state) {
    // Always return resolved_mode: defaults to MonoMix before lock, detected mode after.
    return state.resolved_mode;
}

} // namespace mic_channel_detail

WasapiCaptureSrc::WasapiCaptureSrc(MicChannelMode channel_mode, std::optional<std::string> device_id)
    : requested_channel_mode_(channel_mode), device_id_(std::move(device_id)) {
}

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

    if (device_id_.has_value()) {
        const std::wstring wide_device_id = Utf8ToWide(*device_id_);
        hr = wide_device_id.empty() ? E_INVALIDARG : enumerator->GetDevice(wide_device_id.c_str(), &device_);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device_);
    }
    enumerator->Release();
    enumerator = nullptr;
    if (FAILED(hr)) {
        return device_id_.has_value() ? failHr("IMMDeviceEnumerator::GetDevice", hr)
                                      : failHr("GetDefaultAudioEndpoint(eCapture,eConsole)", hr);
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
    auto_mode_locked_ = false;
    auto_resolved_mode_ = MicChannelMode::MonoMix;
    auto_detect_frames_ = 0;
    auto_energy_left_ = 0.0;
    auto_energy_right_ = 0.0;

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

    UpdateAutoModeFromBuffer(reinterpret_cast<const uint8_t*>(data), numFrames, silent);
    const MicChannelMode effective_mode = EffectiveChannelMode();

    const bool needs_mapping_buffer =
        mono_to_stereo_ || (input_channels_ == 2 && effective_mode != MicChannelMode::PreserveStereo);

    if (!needs_mapping_buffer) {
        out_buf.bytes = reinterpret_cast<const uint8_t*>(data);
        out_buf.num_frames = numFrames;
        out_buf.silent = silent;
        return true;
    }

    const size_t outputSamples = static_cast<size_t>(numFrames) * kRequiredOutputChannels;
    if (sample_format_ == AudioSampleFormat::Float32) {
        mapped_buffer_.resize(outputSamples * sizeof(float));
        float* dst = reinterpret_cast<float*>(mapped_buffer_.data());
        if (silent || data == nullptr) {
            std::memset(dst, 0, mapped_buffer_.size());
        } else {
            if (input_channels_ == 1) {
                const float* src = reinterpret_cast<const float*>(data);
                for (size_t i = 0; i < static_cast<size_t>(numFrames); ++i) {
                    const float s = src[i];
                    dst[(i * 2) + 0] = s;
                    dst[(i * 2) + 1] = s;
                }
            } else {
                const float* src = reinterpret_cast<const float*>(data);
                for (size_t i = 0; i < static_cast<size_t>(numFrames); ++i) {
                    float outL = 0.0f;
                    float outR = 0.0f;
                    mic_channel_detail::MapStereoFrameFloat32(effective_mode, src[(i * 2) + 0], src[(i * 2) + 1], outL,
                                                              outR);
                    dst[(i * 2) + 0] = outL;
                    dst[(i * 2) + 1] = outR;
                }
            }
        }
    } else {
        mapped_buffer_.resize(outputSamples * sizeof(int16_t));
        int16_t* dst = reinterpret_cast<int16_t*>(mapped_buffer_.data());
        if (silent || data == nullptr) {
            std::memset(dst, 0, mapped_buffer_.size());
        } else {
            if (input_channels_ == 1) {
                const int16_t* src = reinterpret_cast<const int16_t*>(data);
                for (size_t i = 0; i < static_cast<size_t>(numFrames); ++i) {
                    const int16_t s = src[i];
                    dst[(i * 2) + 0] = s;
                    dst[(i * 2) + 1] = s;
                }
            } else {
                const int16_t* src = reinterpret_cast<const int16_t*>(data);
                for (size_t i = 0; i < static_cast<size_t>(numFrames); ++i) {
                    int16_t outL = 0;
                    int16_t outR = 0;
                    mic_channel_detail::MapStereoFrameInt16(effective_mode, src[(i * 2) + 0], src[(i * 2) + 1], outL,
                                                            outR);
                    dst[(i * 2) + 0] = outL;
                    dst[(i * 2) + 1] = outR;
                }
            }
        }
    }

    out_buf.bytes = mapped_buffer_.data();
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

void WasapiCaptureSrc::UpdateAutoModeFromBuffer(const uint8_t* data, uint32_t num_frames, bool silent) {
    if (requested_channel_mode_ != MicChannelMode::Auto || auto_mode_locked_ || input_channels_ != 2 ||
        num_frames == 0) {
        return;
    }

    mic_channel_detail::AutoModeState state{};
    state.analyzed_frames = auto_detect_frames_;
    state.energy_left = auto_energy_left_;
    state.energy_right = auto_energy_right_;
    state.locked = auto_mode_locked_;
    state.resolved_mode = auto_resolved_mode_;

    if (sample_format_ == AudioSampleFormat::Float32) {
        mic_channel_detail::UpdateAutoModeStateFloat32(state, reinterpret_cast<const float*>(data), num_frames, silent);
    } else {
        mic_channel_detail::UpdateAutoModeStateInt16(state, reinterpret_cast<const int16_t*>(data), num_frames, silent);
    }

    auto_detect_frames_ = state.analyzed_frames;
    auto_energy_left_ = state.energy_left;
    auto_energy_right_ = state.energy_right;
    auto_mode_locked_ = state.locked;
    auto_resolved_mode_ = state.resolved_mode;
}

MicChannelMode WasapiCaptureSrc::EffectiveChannelMode() const {
    if (requested_channel_mode_ != MicChannelMode::Auto) {
        return requested_channel_mode_;
    }
    mic_channel_detail::AutoModeState state{};
    state.locked = auto_mode_locked_;
    state.resolved_mode = auto_resolved_mode_;
    return mic_channel_detail::EffectiveAutoMode(state);
}

void WasapiCaptureSrc::Shutdown() {
    ReleaseBuffer();
    mapped_buffer_.clear();
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
    auto_mode_locked_ = false;
    auto_resolved_mode_ = MicChannelMode::MonoMix;
    auto_detect_frames_ = 0;
    auto_energy_left_ = 0.0;
    auto_energy_right_ = 0.0;
}

} // namespace recorder_core

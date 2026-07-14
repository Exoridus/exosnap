#include "recorder_core/wasapi_audio_render.h"

#include "recorder_core/logging/logging.h"

#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <windows.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>
#include <vector>

namespace recorder_core {

namespace {
constexpr const char* kLogComponent = "wasapi_audio_render";
constexpr uint32_t kEngineSampleRate = 48000;
constexpr uint32_t kEngineChannels = 2;
constexpr REFERENCE_TIME kBufferDurationHns = 2000000; // 200 ms shared-mode buffer

void LogError(const char* msg) {
    logging::log(logging::LogLevel::Error, kLogComponent, msg);
}
void LogWarn(const char* msg) {
    logging::log(logging::LogLevel::Warn, kLogComponent, msg);
}

// Effective format tag of a mix format: WAVE_FORMAT_EXTENSIBLE wraps the real
// tag in SubFormat (same Data1 shortcut as wasapi_capture_src.cpp's
// ResolveWaveFormatTag -- Data1 of KSDATAFORMAT_SUBTYPE_* is the classic tag).
// Shared-mode GetMixFormat() virtually always returns WAVE_FORMAT_EXTENSIBLE,
// so testing wFormatTag directly would misclassify a plain 48k/stereo/float32
// endpoint as "needs resampling".
WORD ResolveRenderWaveFormatTag(const WAVEFORMATEX* fmt) {
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        return static_cast<WORD>(ext->SubFormat.Data1);
    }
    return fmt->wFormatTag;
}
} // namespace

WasapiAudioRenderer::WasapiAudioRenderer(uint32_t ring_capacity_frames)
    : ring_capacity_floats_(ring_capacity_frames * kEngineChannels) {
}

WasapiAudioRenderer::~WasapiAudioRenderer() {
    Shutdown();
}

bool WasapiAudioRenderer::Init(std::string& out_error) {
    Shutdown(); // tear down any previous session first

    // Defensive COM init for the caller's thread. RPC_E_CHANGED_MODE means the
    // thread already has COM in a different apartment mode -- usable as-is,
    // but NOT initialized by this call, so it must never be balanced with a
    // CoUninitialize here (that would unbalance the caller's own init).
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized_here = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        out_error = "CoInitializeEx failed";
        return false;
    }
    // On success, COM deliberately stays initialized on this thread for the
    // renderer's lifetime: the COM objects created below outlive Init(), and
    // Shutdown() may run on a different thread (CoUninitialize is
    // thread-affine, so Shutdown() cannot balance it).
    const auto fail = [&](const char* msg) {
        out_error = msg;
        Shutdown();
        if (com_initialized_here)
            CoUninitialize();
        return false;
    };

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr)
        return fail("CoCreateInstance(MMDeviceEnumerator) failed");

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    enumerator->Release();
    if (FAILED(hr) || device_ == nullptr)
        return fail("no default audio render endpoint");

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr) || audio_client_ == nullptr)
        return fail("IMMDevice::Activate(IAudioClient) failed");

    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client_->GetMixFormat(&mix_format);
    if (FAILED(hr) || mix_format == nullptr)
        return fail("IAudioClient::GetMixFormat failed");

    device_sample_rate_ = mix_format->nSamplesPerSec;
    device_channels_ = mix_format->nChannels;
    const WORD device_tag = ResolveRenderWaveFormatTag(mix_format);
    const WORD device_bits = mix_format->wBitsPerSample;

    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kBufferDurationHns, 0,
                                   mix_format, nullptr);
    CoTaskMemFree(mix_format);
    if (FAILED(hr))
        return fail("IAudioClient::Initialize failed");

    // Only float32 and 16-bit PCM device buffers are supported (the same two
    // accepted kinds as the capture side). Anything else (e.g. a hypothetical
    // 24-bit mix format) fails Init honestly instead of writing samples with
    // the wrong byte stride into the device buffer.
    const bool device_is_float32 = (device_tag == WAVE_FORMAT_IEEE_FLOAT && device_bits == 32);
    const bool device_is_pcm16 = (device_tag == WAVE_FORMAT_PCM && device_bits == 16);
    if (!device_is_float32 && !device_is_pcm16)
        return fail("unsupported render endpoint mix format (not float32 / 16-bit PCM)");
    // The render thread's silence-padding math (RenderThreadMain) needs the
    // ACTUAL bytes-per-sample of the device buffer, not an assumed float32 --
    // a PCM/int16 device would otherwise get the wrong padding stride.
    device_bytes_per_sample_ = device_is_pcm16 ? 2u : 4u;

    // Resampler: engine format (48k/stereo/float32) -> the device's actual
    // mix format, only allocated if they differ.
    if (device_sample_rate_ != kEngineSampleRate || device_channels_ != kEngineChannels || !device_is_float32) {
        const AVSampleFormat out_fmt = device_is_pcm16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLT;
        AVChannelLayout in_layout{};
        AVChannelLayout out_layout{};
        av_channel_layout_default(&in_layout, static_cast<int>(kEngineChannels));
        av_channel_layout_default(&out_layout, static_cast<int>(device_channels_));
        SwrContext* swr = nullptr;
        const int swr_ret =
            swr_alloc_set_opts2(&swr, &out_layout, out_fmt, static_cast<int>(device_sample_rate_), &in_layout,
                                AV_SAMPLE_FMT_FLT, static_cast<int>(kEngineSampleRate), 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (swr_ret < 0 || swr == nullptr || swr_init(swr) < 0) {
            if (swr != nullptr)
                swr_free(&swr);
            return fail("audio render resampler init failed");
        }
        resampler_ = swr;
    }

    hr = audio_client_->GetBufferSize(&buffer_frame_count_);
    if (FAILED(hr))
        return fail("IAudioClient::GetBufferSize failed");

    buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (buffer_event_ == nullptr)
        return fail("CreateEventW failed");
    hr = audio_client_->SetEventHandle(static_cast<HANDLE>(buffer_event_));
    if (FAILED(hr))
        return fail("IAudioClient::SetEventHandle failed");

    hr = audio_client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client_));
    if (FAILED(hr) || render_client_ == nullptr)
        return fail("IAudioClient::GetService(IAudioRenderClient) failed");

    initialized_ = true;
    return true;
}

void WasapiAudioRenderer::Start() {
    if (!initialized_ || running_.load())
        return;
    frames_rendered_.store(0);
    const HRESULT hr = audio_client_->Start();
    if (FAILED(hr)) {
        LogError("IAudioClient::Start failed -- render thread not started");
        return;
    }
    running_.store(true);
    render_thread_ = std::thread(&WasapiAudioRenderer::RenderThreadMain, this);
}

void WasapiAudioRenderer::Stop() {
    // Always wake anything blocked in PushSamples(), even if the render
    // thread was never started (Init() succeeded but Start() wasn't called
    // yet, or this is a second Stop() call) -- a caller further up the stack
    // (EditPlayerSession::Pause(), see the pacing design doc) relies on this
    // to make StopPlaybackDecode()'s join() unable to hang forever on a
    // decode thread stuck inside a full ring nothing is draining anymore.
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        stop_requested_ = true;
    }
    ring_cv_.notify_all();
    if (buffer_event_ != nullptr)
        SetEvent(static_cast<HANDLE>(buffer_event_)); // wake the render thread so it can observe running_==false
    if (render_thread_.joinable())
        render_thread_.join();
    if (initialized_ && audio_client_ != nullptr)
        audio_client_->Stop();
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        stop_requested_ = false; // reset so a subsequent Start()/PushSamples() cycle blocks normally again
        ring_.clear();           // drop whatever was left queued; a fresh Play() starts clean
    }
}

void WasapiAudioRenderer::PushSamples(const float* interleaved_stereo, uint32_t frame_count) {
    if (interleaved_stereo == nullptr || frame_count == 0)
        return;
    // Not gated on initialized_: the ring is a pure producer/consumer queue,
    // independent of whether a real device is open. In production this is
    // only ever called after Init() succeeded (EditPlayerSession only pushes
    // when HasAudioStream() is true, which requires a successful Init()), and
    // Init() always clears any pre-existing ring contents via its own
    // Shutdown() call, so nothing pushed before Init() can leak into
    // playback -- this just makes the ring's blocking/capacity behavior
    // independently unit-testable without a real WASAPI render device.
    size_t remaining = static_cast<size_t>(frame_count) * kEngineChannels;
    const float* src = interleaved_stereo;
    std::unique_lock<std::mutex> lock(ring_mutex_);
    while (remaining > 0) {
        ring_cv_.wait(lock, [&] { return stop_requested_ || ring_.size() < ring_capacity_floats_; });
        if (stop_requested_)
            return; // renderer is stopping: drop whatever's left rather than insert it
        const size_t room = ring_capacity_floats_ - ring_.size();
        const size_t take =
            std::min<size_t>(room, remaining); // explicit template arg: windows.h's min() macro (no NOMINMAX here)
        ring_.insert(ring_.end(), src, src + take);
        src += take;
        remaining -= take;
    }
}

uint64_t WasapiAudioRenderer::FramesRendered() const noexcept {
    return frames_rendered_.load();
}

uint32_t WasapiAudioRenderer::SampleRate() const noexcept {
    return device_sample_rate_;
}

void WasapiAudioRenderer::RenderThreadMain() {
    // The thread that talks to the audio engine initializes COM for itself,
    // matching the mic/loopback meter-service worker-thread convention.
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized_here = SUCCEEDED(com_hr);
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE)
        LogWarn("render thread CoInitializeEx failed");

    // Pro-audio thread priority, matching the capture side's convention of
    // treating the WASAPI event thread as latency-critical.
    DWORD task_index = 0;
    HANDLE task_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    std::vector<float> engine_buf; // pulled from ring_, engine format

    while (running_.load()) {
        const DWORD wait_ret = WaitForSingleObject(static_cast<HANDLE>(buffer_event_), 200);
        if (!running_.load())
            break;
        if (wait_ret != WAIT_OBJECT_0)
            continue;

        UINT32 padding_frames = 0;
        if (FAILED(audio_client_->GetCurrentPadding(&padding_frames)) || padding_frames > buffer_frame_count_)
            continue;
        const UINT32 available_frames = buffer_frame_count_ - padding_frames;
        if (available_frames == 0)
            continue;

        // How many engine-rate (48 kHz) frames this device-rate request needs.
        // With a resampler this must be rate-converted AND account for input
        // already buffered inside swr: pulling one engine frame per device
        // frame would under-feed every callback on a lower-rate device
        // (periodic silence gaps) and over-feed -- accumulating unboundedly in
        // swr's FIFO -- on a higher-rate one.
        size_t want_engine_frames = available_frames;
        if (resampler_ != nullptr) {
            const int64_t buffered = swr_get_delay(resampler_, static_cast<int64_t>(kEngineSampleRate));
            const int64_t needed = av_rescale_rnd(static_cast<int64_t>(available_frames), kEngineSampleRate,
                                                  static_cast<int64_t>(device_sample_rate_), AV_ROUND_UP) -
                                   std::max<int64_t>(buffered, 0);
            want_engine_frames = needed > 0 ? static_cast<size_t>(needed) : 0u;
        }

        // Pull up to want_engine_frames worth of engine-format samples out of the ring.
        const size_t want_floats = want_engine_frames * kEngineChannels;
        engine_buf.clear();
        {
            std::lock_guard<std::mutex> lock(ring_mutex_);
            // Explicit template args so windows.h's min() function-like macro
            // cannot expand (this target compiles without NOMINMAX).
            const size_t take = std::min<size_t>(want_floats, ring_.size());
            engine_buf.assign(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(take));
            ring_.erase(ring_.begin(), ring_.begin() + static_cast<std::ptrdiff_t>(take));
        }
        ring_cv_.notify_all(); // wake any PushSamples() blocked waiting for room
        const uint32_t engine_frames = static_cast<uint32_t>(engine_buf.size() / kEngineChannels);

        BYTE* device_data = nullptr;
        if (FAILED(render_client_->GetBuffer(available_frames, &device_data)))
            continue;

        if (engine_frames == 0) {
            // Nothing decoded yet (e.g. paused/starved) -- render silence rather
            // than blocking, so the device clock (and playback clock derived
            // from it) keeps advancing honestly instead of stalling.
            render_client_->ReleaseBuffer(available_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        } else if (resampler_ == nullptr) {
            // Device is exactly 48k/stereo/float32 -- straight copy.
            const size_t copy_bytes = engine_buf.size() * sizeof(float); // <= available_frames worth by construction
            memcpy(device_data, engine_buf.data(), copy_bytes);
            if (engine_frames < available_frames) {
                memset(device_data + copy_bytes, 0,
                       (static_cast<size_t>(available_frames) - engine_frames) * kEngineChannels * sizeof(float));
            }
            render_client_->ReleaseBuffer(available_frames, 0);
        } else {
            const uint8_t* in_ptr = reinterpret_cast<const uint8_t*>(engine_buf.data());
            const int produced = swr_convert(resampler_, &device_data, static_cast<int>(available_frames), &in_ptr,
                                             static_cast<int>(engine_frames));
            if (produced <= 0) {
                // Error, or all input got buffered inside swr with nothing
                // produced yet -- the buffer contents are untouched garbage, so
                // tell the engine to treat it as silence.
                render_client_->ReleaseBuffer(available_frames, AUDCLNT_BUFFERFLAGS_SILENT);
            } else {
                if (static_cast<uint32_t>(produced) < available_frames) {
                    // Pad any shortfall with silence rather than leaving garbage,
                    // using the ACTUAL device sample format's byte stride (set in
                    // Init() -- float32 or int16, whichever the resampler was
                    // configured to emit), not an assumed one.
                    const size_t device_frame_bytes = static_cast<size_t>(device_channels_) * device_bytes_per_sample_;
                    uint8_t* tail = device_data + static_cast<size_t>(produced) * device_frame_bytes;
                    const size_t tail_bytes =
                        (static_cast<size_t>(available_frames) - static_cast<size_t>(produced)) * device_frame_bytes;
                    memset(tail, 0, tail_bytes);
                }
                render_client_->ReleaseBuffer(available_frames, 0);
            }
        }

        frames_rendered_.fetch_add(available_frames);
    }

    if (task_handle != nullptr)
        AvRevertMmThreadCharacteristics(task_handle);
    if (com_initialized_here)
        CoUninitialize();
}

void WasapiAudioRenderer::Shutdown() {
    Stop();
    if (buffer_event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(buffer_event_));
        buffer_event_ = nullptr;
    }
    if (resampler_ != nullptr) {
        swr_free(&resampler_);
        resampler_ = nullptr;
    }
    if (render_client_ != nullptr) {
        render_client_->Release();
        render_client_ = nullptr;
    }
    if (audio_client_ != nullptr) {
        audio_client_->Release();
        audio_client_ = nullptr;
    }
    if (device_ != nullptr) {
        device_->Release();
        device_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(ring_mutex_);
        ring_.clear();
    }
    device_sample_rate_ = 0;
    device_channels_ = 0;
    device_bytes_per_sample_ = 4;
    buffer_frame_count_ = 0;
    initialized_ = false;
}

} // namespace recorder_core

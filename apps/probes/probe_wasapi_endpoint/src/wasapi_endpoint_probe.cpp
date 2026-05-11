#include "wasapi_endpoint_probe.h"

#include <functiondiscoverykeys_devpkey.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace wasapi_probe {

namespace {

constexpr uint32_t kDefaultDurationSec = 30;
constexpr uint32_t kPollSleepMs = 5;
constexpr double kSilenceThreshold = 0.005;

const CLSID kClsidMmDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID kIidImmDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID kIidAudioClient = __uuidof(IAudioClient);
const IID kIidAudioCaptureClient = __uuidof(IAudioCaptureClient);

std::string WideToUtf8(const wchar_t* wstr) {
    if (wstr == nullptr || wstr[0] == L'\0') {
        return "(unnamed)";
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        return "(unnamed)";
    }
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
    return result;
}

const char* FormatTagName(WORD tag) {
    switch (tag) {
    case WAVE_FORMAT_PCM:
        return "PCM";
    case WAVE_FORMAT_IEEE_FLOAT:
        return "IEEE_FLOAT";
    case WAVE_FORMAT_EXTENSIBLE:
        return "EXTENSIBLE";
    default:
        return "UNKNOWN";
    }
}

WORD ExtractGuidFormatTag(const WAVEFORMATEX* pwfx) {
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
        const auto* wfext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pwfx);
        return static_cast<WORD>(wfext->SubFormat.Data1);
    }
    return pwfx->wFormatTag;
}

} // namespace

Probe::Probe() {
    m_output.kind = "output_loopback";
    m_input.kind = "input_capture";
}

Probe::~Probe() {
    CleanupStream(m_output);
    CleanupStream(m_input);
}

void Probe::CleanupStream(Stream& st) {
    if (st.audioClient != nullptr) {
        st.audioClient->Stop();
        st.audioClient = nullptr;
    }
    st.captureClient = nullptr;
    st.device = nullptr;
}

bool Probe::InitStream(Stream& st, EDataFlow flow, bool loopback) {
    HRESULT hr = S_OK;

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    hr =
        CoCreateInstance(kClsidMmDeviceEnumerator, nullptr, CLSCTX_ALL, kIidImmDeviceEnumerator, enumerator.put_void());
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: CoCreateInstance(MMDeviceEnumerator) 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, st.device.put());
    if (FAILED(hr)) {
        const char* dir = (flow == eRender) ? "render" : "capture";
        fprintf(stderr, "[probe] FAIL: GetDefaultAudioEndpoint(%s) 0x%08lX\n", dir, static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    IPropertyStore* props = nullptr;
    hr = st.device->OpenPropertyStore(STGM_READ, &props);
    if (SUCCEEDED(hr)) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        hr = props->GetValue(PKEY_Device_FriendlyName, &varName);
        if (SUCCEEDED(hr) && varName.vt == VT_LPWSTR) {
            st.name = WideToUtf8(varName.pwszVal);
        }
        PropVariantClear(&varName);
        props->Release();
    }
    if (st.name.empty()) {
        st.name = "(unnamed)";
    }

    hr = st.device->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, st.audioClient.put_void());
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: Activate(IAudioClient) for '%s' 0x%08lX\n", st.name.c_str(),
                static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = st.audioClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || pwfx == nullptr) {
        fprintf(stderr, "[probe] FAIL: GetMixFormat for '%s' 0x%08lX\n", st.name.c_str(),
                static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        pwfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        memcpy(&st.mixFormat, pwfx, sizeof(WAVEFORMATEXTENSIBLE));
    } else {
        memcpy(&st.mixFormat, pwfx, sizeof(WAVEFORMATEX));
        st.mixFormat.Format.cbSize = 0;
    }
    CoTaskMemFree(pwfx);

    WORD actualTag = st.mixFormat.Format.wFormatTag;
    if (actualTag == WAVE_FORMAT_EXTENSIBLE) {
        actualTag = ExtractGuidFormatTag(&st.mixFormat.Format);
    }
    st.isFloat = (actualTag == WAVE_FORMAT_IEEE_FLOAT);
    st.bytesPerFrame = st.mixFormat.Format.nBlockAlign;

    DWORD streamFlags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;

    REFERENCE_TIME hnsDefaultDevicePeriod = 0;
    hr = st.audioClient->GetDevicePeriod(&hnsDefaultDevicePeriod, nullptr);
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: GetDevicePeriod for '%s' 0x%08lX\n", st.name.c_str(),
                static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = st.audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, hnsDefaultDevicePeriod, 0,
                                    &st.mixFormat.Format, nullptr);
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: Initialize for '%s' 0x%08lX\n", st.name.c_str(), static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = st.audioClient->GetBufferSize(&st.bufferFrameCount);
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: GetBufferSize for '%s' 0x%08lX\n", st.name.c_str(),
                static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = st.audioClient->GetService(kIidAudioCaptureClient, st.captureClient.put_void());
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: GetService(IAudioCaptureClient) for '%s' 0x%08lX\n", st.name.c_str(),
                static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = st.audioClient->Start();
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: Start for '%s' 0x%08lX\n", st.name.c_str(), static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    return true;
}

void Probe::ProcessStream(Stream& st) {
    if (st.captureClient == nullptr) {
        return;
    }

    while (true) {
        UINT32 numFrames = 0;
        DWORD flags = 0;
        BYTE* data = nullptr;

        HRESULT hr = st.captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) {
            break;
        }
        if (FAILED(hr)) {
            break;
        }

        st.intervalPackets++;
        st.totalPackets++;

        uint64_t totalSamples = static_cast<uint64_t>(numFrames) * st.mixFormat.Format.nChannels;

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            st.intervalSilentFrames += numFrames;
            st.totalSilentFrames += numFrames;
        } else {
            double sumSq = 0.0;
            double maxAbs = 0.0;

            if (st.isFloat) {
                const auto* samples = reinterpret_cast<const float*>(data);
                for (uint64_t i = 0; i < totalSamples; ++i) {
                    double s = static_cast<double>(samples[i]);
                    sumSq += s * s;
                    double a = std::abs(s);
                    if (a > maxAbs) {
                        maxAbs = a;
                    }
                }
            } else {
                const auto* samples = reinterpret_cast<const int16_t*>(data);
                for (uint64_t i = 0; i < totalSamples; ++i) {
                    double s = static_cast<double>(samples[i]) / 32768.0;
                    sumSq += s * s;
                    double a = std::abs(s);
                    if (a > maxAbs) {
                        maxAbs = a;
                    }
                }
            }

            st.intervalSumSq += sumSq;
            if (maxAbs > st.intervalMaxAbs) {
                st.intervalMaxAbs = maxAbs;
            }
            st.totalSumSq += sumSq;
            if (maxAbs > st.totalMaxAbs) {
                st.totalMaxAbs = maxAbs;
            }

            if (maxAbs > kSilenceThreshold) {
                st.hadNonSilent = true;
            } else {
                st.intervalSilentFrames += numFrames;
                st.totalSilentFrames += numFrames;
            }
        }

        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            st.intervalDiscontinuities++;
            st.totalDiscontinuities++;
        }

        st.intervalFrames += numFrames;
        st.totalFrames += numFrames;

        st.captureClient->ReleaseBuffer(numFrames);
    }
}

StreamSnapshot Probe::SnapshotInterval(Stream& st) {
    StreamSnapshot snap;
    snap.kind = st.kind;
    snap.sampleRate = st.mixFormat.Format.nSamplesPerSec;
    snap.channels = st.mixFormat.Format.nChannels;
    snap.packets = st.intervalPackets;
    snap.frames = st.intervalFrames;

    uint64_t totalFrames = st.intervalFrames;
    if (st.intervalSilentFrames > totalFrames) {
        st.intervalSilentFrames = totalFrames;
    }
    uint64_t nonsilentFrames = totalFrames - st.intervalSilentFrames;

    if (nonsilentFrames > 0) {
        uint64_t nonsilentSamples = nonsilentFrames * st.mixFormat.Format.nChannels;
        snap.rms = std::sqrt(st.intervalSumSq / static_cast<double>(nonsilentSamples));
    }
    snap.peak = st.intervalMaxAbs;

    if (totalFrames > 0) {
        snap.silencePct = 100.0 * static_cast<double>(st.intervalSilentFrames) / static_cast<double>(totalFrames);
    }
    snap.discontinuities = st.intervalDiscontinuities;

    return snap;
}

void Probe::ResetInterval(Stream& st) {
    st.intervalPackets = 0;
    st.intervalFrames = 0;
    st.intervalSumSq = 0.0;
    st.intervalMaxAbs = 0.0;
    st.intervalSilentFrames = 0;
    st.intervalDiscontinuities = 0;
}

void Probe::PrintMetrics(const StreamSnapshot& s) {
    if (s.frames == 0) {
        fprintf(stdout,
                "[metrics] stream=%s rate=%u channels=%u packets=%llu frames=%llu rms=%.3f peak=%.3f silence=n/a "
                "discontinuities=%llu\n",
                s.kind.c_str(), s.sampleRate, s.channels, static_cast<unsigned long long>(s.packets),
                static_cast<unsigned long long>(s.frames), s.rms, s.peak,
                static_cast<unsigned long long>(s.discontinuities));
    } else {
        fprintf(stdout,
                "[metrics] stream=%s rate=%u channels=%u packets=%llu frames=%llu rms=%.3f peak=%.3f silence=%.1f%% "
                "discontinuities=%llu\n",
                s.kind.c_str(), s.sampleRate, s.channels, static_cast<unsigned long long>(s.packets),
                static_cast<unsigned long long>(s.frames), s.rms, s.peak, s.silencePct,
                static_cast<unsigned long long>(s.discontinuities));
    }
    fflush(stdout);
}

void Probe::PrintSummary(const Stream& st) {
    uint64_t totalFrames = st.totalFrames;
    uint64_t silentFrames = st.totalSilentFrames;
    if (silentFrames > totalFrames) {
        silentFrames = totalFrames;
    }
    uint64_t nonsilentFrames = totalFrames - silentFrames;

    double avgRms = 0.0;
    if (nonsilentFrames > 0) {
        uint64_t nonsilentSamples = nonsilentFrames * st.mixFormat.Format.nChannels;
        avgRms = std::sqrt(st.totalSumSq / static_cast<double>(nonsilentSamples));
    }

    double totalSilencePct = 0.0;
    if (totalFrames > 0) {
        totalSilencePct = 100.0 * static_cast<double>(silentFrames) / static_cast<double>(totalFrames);
    }

    fprintf(stdout, "\n--- %s ---\n", st.kind.c_str());
    fprintf(stdout, "  device             : %s\n", st.name.c_str());
    fprintf(stdout, "  sample rate        : %u\n", st.mixFormat.Format.nSamplesPerSec);
    fprintf(stdout, "  channels           : %u\n", st.mixFormat.Format.nChannels);
    fprintf(stdout, "  total packets      : %llu\n", static_cast<unsigned long long>(st.totalPackets));
    fprintf(stdout, "  total frames       : %llu\n", static_cast<unsigned long long>(st.totalFrames));
    fprintf(stdout, "  avg RMS            : %.6f\n", avgRms);
    fprintf(stdout, "  max peak           : %.6f\n", st.totalMaxAbs);
    fprintf(stdout, "  silence            : %.1f %%\n", totalSilencePct);
    fprintf(stdout, "  discontinuities    : %llu\n", static_cast<unsigned long long>(st.totalDiscontinuities));
    fprintf(stdout, "  non-silent observed: %s\n", st.hadNonSilent ? "yes" : "no");
    fflush(stdout);
}

bool Probe::Start() {
    fprintf(stdout, "[probe] initializing WASAPI endpoint probe...\n");
    fflush(stdout);

    fprintf(stdout, "[probe] opening default render endpoint (output loopback)...\n");
    fflush(stdout);
    if (!InitStream(m_output, eRender, true)) {
        fprintf(stderr, "[probe] ABORT: output loopback init failed\n");
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[probe] opening default capture endpoint (input/mic)...\n");
    fflush(stdout);
    if (!InitStream(m_input, eCapture, false)) {
        fprintf(stderr, "[probe] ABORT: input capture init failed\n");
        fflush(stderr);
        return false;
    }

    WORD outTag = m_output.mixFormat.Format.wFormatTag;
    WORD inTag = m_input.mixFormat.Format.wFormatTag;
    if (outTag == WAVE_FORMAT_EXTENSIBLE) {
        outTag = ExtractGuidFormatTag(&m_output.mixFormat.Format);
    }
    if (inTag == WAVE_FORMAT_EXTENSIBLE) {
        inTag = ExtractGuidFormatTag(&m_input.mixFormat.Format);
    }

    fprintf(stdout, "\n");
    fprintf(stdout, "[probe] output loopback: %s\n", m_output.name.c_str());
    fprintf(stdout, "  format: %s, %u Hz, %u channels, %u-bit, %u bytes/frame\n",
            FormatTagName(m_output.mixFormat.Format.wFormatTag), m_output.mixFormat.Format.nSamplesPerSec,
            m_output.mixFormat.Format.nChannels, m_output.mixFormat.Format.wBitsPerSample, m_output.bytesPerFrame);
    fprintf(stdout, "  buffer frames: %u\n", m_output.bufferFrameCount);
    fflush(stdout);

    fprintf(stdout, "[probe] input capture : %s\n", m_input.name.c_str());
    fprintf(stdout, "  format: %s, %u Hz, %u channels, %u-bit, %u bytes/frame\n",
            FormatTagName(m_input.mixFormat.Format.wFormatTag), m_input.mixFormat.Format.nSamplesPerSec,
            m_input.mixFormat.Format.nChannels, m_input.mixFormat.Format.wBitsPerSample, m_input.bytesPerFrame);
    fprintf(stdout, "  buffer frames: %u\n", m_input.bufferFrameCount);
    fflush(stdout);

    fprintf(stdout, "\n[probe] both streams started — capturing for %u seconds (press 'q' to stop early)\n\n",
            kDefaultDurationSec);
    fflush(stdout);

    return true;
}

void Probe::Run(uint32_t durationSec) {
    auto startTime = std::chrono::steady_clock::now();
    auto endTime = startTime + std::chrono::seconds(durationSec);
    auto nextMetricTime = startTime + std::chrono::seconds(1);

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= endTime) {
            break;
        }

        if (GetAsyncKeyState('Q') & 0x8000) {
            fprintf(stdout, "\n[probe] user stopped early\n");
            fflush(stdout);
            break;
        }

        Sleep(kPollSleepMs);

        ProcessStream(m_output);
        ProcessStream(m_input);

        now = std::chrono::steady_clock::now();
        if (now >= nextMetricTime) {
            auto outSnap = SnapshotInterval(m_output);
            auto inSnap = SnapshotInterval(m_input);
            PrintMetrics(outSnap);
            PrintMetrics(inSnap);
            ResetInterval(m_output);
            ResetInterval(m_input);
            nextMetricTime = now + std::chrono::seconds(1);
        }
    }

    auto endActual = std::chrono::steady_clock::now();
    auto durationSecActual = std::chrono::duration<double>(endActual - startTime).count();

    fprintf(stdout, "\n=== WASAPI ENDPOINT PROBE SUMMARY ===\n");
    fprintf(stdout, "  duration (s)        : %.1f\n", durationSecActual);
    fflush(stdout);

    PrintSummary(m_output);
    PrintSummary(m_input);

    fprintf(stdout, "\n[probe] capture complete.\n");
    fflush(stdout);
}

} // namespace wasapi_probe

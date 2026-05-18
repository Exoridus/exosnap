#include "process_loopback_probe.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace process_loopback_probe {

namespace {

constexpr uint32_t kDefaultDurationSec = 30;
constexpr uint32_t kPollSleepMs = 5;
constexpr double kSilenceThreshold = 0.005;
constexpr uint32_t kActivationTimeoutMs = 5000;

const IID kIidAudioClient = __uuidof(IAudioClient);
const IID kIidAudioCaptureClient = __uuidof(IAudioCaptureClient);

typedef HRESULT(STDAPICALLTYPE* PFN_ActivateAudioInterfaceAsync)(
    LPCWSTR deviceInterfacePath, REFIID riid, PROPVARIANT* activationParams,
    IActivateAudioInterfaceCompletionHandler* completionHandler, IActivateAudioInterfaceAsyncOperation** operation);

PFN_ActivateAudioInterfaceAsync s_pfnActivateAudioInterfaceAsync = nullptr;
HMODULE s_hMmdevapi = nullptr;

HRESULT CallActivateAudioInterfaceAsyncSafe(LPCWSTR deviceInterfacePath, REFIID riid, PROPVARIANT* activationParams,
                                            IActivateAudioInterfaceCompletionHandler* completionHandler,
                                            IActivateAudioInterfaceAsyncOperation** operation) {
    HRESULT hr = E_FAIL;
    __try {
        hr =
            s_pfnActivateAudioInterfaceAsync(deviceInterfacePath, riid, activationParams, completionHandler, operation);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fflush(stdout);
        fflush(stderr);
        fprintf(stderr, "[probe] FATAL: ActivateAudioInterfaceAsync crashed (SEH 0x%08lX)\n",
                static_cast<unsigned long>(GetExceptionCode()));
        fprintf(stderr, "[probe] Process Loopback API is not supported on this OS.\n");
        fprintf(stderr, "[probe] Requires: Windows 10 Build 20348+ / Windows 11\n");
        fflush(stderr);
        hr = E_UNEXPECTED;
    }
    return hr;
}

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

std::string GetProcessName(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess == nullptr) {
        return "(unknown)";
    }
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
        CloseHandle(hProcess);
        return "(unknown)";
    }
    CloseHandle(hProcess);
    std::wstring wpath(path);
    auto pos = wpath.find_last_of(L'\\');
    if (pos != std::wstring::npos) {
        wpath = wpath.substr(pos + 1);
    }
    return WideToUtf8(wpath.c_str());
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(style & WS_CAPTION)) {
        return TRUE;
    }
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) {
        return TRUE;
    }
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner != nullptr) {
        return TRUE;
    }
    wchar_t title[256];
    int titleLen = GetWindowTextW(hwnd, title, 256);
    if (titleLen == 0) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return TRUE;
    }
    WindowInfo info;
    info.title = WideToUtf8(title);
    info.pid = pid;
    info.hwnd = hwnd;
    windows->push_back(info);
    return TRUE;
}

class ActivationHandler : public IActivateAudioInterfaceCompletionHandler {
  public:
    ActivationHandler() {
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HRESULT ftmHr =
            CoCreateFreeThreadedMarshaler(static_cast<IActivateAudioInterfaceCompletionHandler*>(this), &m_ftm);
        if (FAILED(ftmHr)) {
            m_ftm = nullptr;
        }
    }
    ~ActivationHandler() {
        if (m_event != nullptr) {
            CloseHandle(m_event);
        }
        if (m_ftm != nullptr) {
            m_ftm->Release();
        }
    }

    bool IsAgile() const {
        return m_ftm != nullptr;
    }
    ActivationHandler(const ActivationHandler&) = delete;
    ActivationHandler& operator=(const ActivationHandler&) = delete;

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT hr = operation->GetActivateResult(&m_activateResult, m_activatedObject.put());
        SetEvent(m_event);
        return hr;
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
        if (m_ftm != nullptr) {
            return m_ftm->QueryInterface(riid, ppv);
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG ref = InterlockedDecrement(&m_ref);
        if (ref == 0) {
            delete this;
        }
        return static_cast<ULONG>(ref);
    }

    HRESULT Wait(DWORD timeoutMs = kActivationTimeoutMs) {
        DWORD result = WaitForSingleObject(m_event, timeoutMs);
        if (result != WAIT_OBJECT_0) {
            return E_FAIL;
        }
        return m_activateResult;
    }

    winrt::com_ptr<IUnknown> m_activatedObject;

  private:
    HANDLE m_event = nullptr;
    LONG m_ref = 1;
    HRESULT m_activateResult = E_FAIL;
    IUnknown* m_ftm = nullptr;
};

} // namespace

Probe::Probe() {
    m_include.kind = "include_target_tree";
    m_exclude.kind = "exclude_target_tree";
}

Probe::~Probe() {
    CleanupStream(m_include);
    CleanupStream(m_exclude);
    if (m_targetProcess != nullptr) {
        CloseHandle(m_targetProcess);
    }
    if (s_hMmdevapi != nullptr) {
        FreeLibrary(s_hMmdevapi);
        s_hMmdevapi = nullptr;
        s_pfnActivateAudioInterfaceAsync = nullptr;
    }
}

void Probe::CleanupStream(Stream& st) {
    if (st.audioClient != nullptr) {
        st.audioClient->Stop();
        st.audioClient = nullptr;
    }
    st.captureClient = nullptr;
}

bool Probe::CheckTargetProcessAlive() {
    if (m_targetProcess == nullptr) {
        return false;
    }
    DWORD result = WaitForSingleObject(m_targetProcess, 0);
    if (result == WAIT_OBJECT_0) {
        m_targetExited = true;
        return false;
    }
    return result == WAIT_TIMEOUT;
}

std::vector<WindowInfo> Probe::EnumerateWindows() {
    std::vector<WindowInfo> windows;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

bool Probe::SelectTarget() {
    auto windows = EnumerateWindows();
    if (windows.empty()) {
        fprintf(stderr, "[probe] FAIL: no visible windows found\n");
        fflush(stderr);
        return false;
    }
    fprintf(stdout, "[probe] visible target windows:\n");
    for (size_t i = 0; i < windows.size(); ++i) {
        std::string procName = GetProcessName(windows[i].pid);
        fprintf(stdout, "  %zu: %s | %s [PID: %lu]\n", i + 1, procName.c_str(), windows[i].title.c_str(),
                static_cast<unsigned long>(windows[i].pid));
    }
    fprintf(stdout, "  q: quit\n");
    fprintf(stdout, "select target [1-%zu]: ", windows.size());
    fflush(stdout);

    char input[64];
    if (fgets(input, sizeof(input), stdin) == nullptr) {
        fprintf(stderr, "[probe] FAIL: input read error\n");
        fflush(stderr);
        return false;
    }
    if (input[0] == 'q' || input[0] == 'Q') {
        fprintf(stdout, "[probe] user quit\n");
        fflush(stdout);
        return false;
    }
    int idx = atoi(input);
    if (idx < 1 || idx > static_cast<int>(windows.size())) {
        fprintf(stderr, "[probe] FAIL: invalid selection\n");
        fflush(stderr);
        return false;
    }

    const auto& selected = windows[static_cast<size_t>(idx - 1)];
    m_targetPid = selected.pid;
    m_targetTitle = selected.title;

    m_targetProcess = OpenProcess(SYNCHRONIZE, FALSE, m_targetPid);
    if (m_targetProcess == nullptr) {
        fprintf(stderr, "[probe] WARN: cannot open target process handle for exit monitoring\n");
        fflush(stderr);
    }

    fprintf(stdout, "\n[probe] selected target:\n");
    fprintf(stdout, "  process: %s\n", GetProcessName(m_targetPid).c_str());
    fprintf(stdout, "  title  : %s\n", m_targetTitle.c_str());
    fprintf(stdout, "  PID    : %lu\n", static_cast<unsigned long>(m_targetPid));
    fflush(stdout);
    return true;
}

bool Probe::InitStream(Stream& st, PROCESS_LOOPBACK_MODE mode) {
    HRESULT hr = S_OK;

    if (s_pfnActivateAudioInterfaceAsync == nullptr) {
        s_hMmdevapi = LoadLibraryW(L"mmdevapi.dll");
        if (s_hMmdevapi == nullptr) {
            fprintf(stderr, "[probe] FAIL: LoadLibrary(mmdevapi.dll) failed\n");
            fprintf(stderr, "[probe] Process Loopback API not available (mmdevapi.dll load failed).\n");
            fprintf(stderr, "[probe] Requires: Windows 10 Build 20348+ / Windows 11\n");
            fflush(stderr);
            return false;
        }
        s_pfnActivateAudioInterfaceAsync = reinterpret_cast<PFN_ActivateAudioInterfaceAsync>(
            GetProcAddress(s_hMmdevapi, "ActivateAudioInterfaceAsync"));
        if (s_pfnActivateAudioInterfaceAsync == nullptr) {
            fprintf(stderr, "[probe] FAIL: GetProcAddress(ActivateAudioInterfaceAsync) failed\n");
            fprintf(stderr, "[probe] Process Loopback API not available (GetProcAddress failed).\n");
            fprintf(stderr, "[probe] Requires: Windows 10 Build 20348+ / Windows 11\n");
            fflush(stderr);
            return false;
        }
    }

    AUDIOCLIENT_ACTIVATION_PARAMS activationParams{};
    activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    activationParams.ProcessLoopbackParams.TargetProcessId = m_targetPid;
    activationParams.ProcessLoopbackParams.ProcessLoopbackMode = mode;

    PROPVARIANT activateParams;
    PropVariantInit(&activateParams);
    activateParams.vt = VT_BLOB;
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&activationParams);
    activateParams.blob.cbSize = sizeof(activationParams);

    ActivationHandler* handler = new ActivationHandler();
    if (!handler->IsAgile()) {
        fprintf(stderr, "[probe] FAIL: CoCreateFreeThreadedMarshaler failed — handler is not agile\n");
        fflush(stderr);
        handler->Release();
        return false;
    }

    winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> asyncOp;
    hr = CallActivateAudioInterfaceAsyncSafe(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, kIidAudioClient, &activateParams,
                                             handler, asyncOp.put());

    if (FAILED(hr)) {
        if (hr != E_UNEXPECTED) {
            fprintf(stderr, "[probe] FAIL: ActivateAudioInterfaceAsync 0x%08lX\n", static_cast<unsigned long>(hr));
            fprintf(stderr, "[probe] Check: handler must be agile (Free-Threaded Marshaler).\n");
            fprintf(stderr, "[probe] Check: AUDIOCLIENT_ACTIVATION_PARAMS shape matches SDK.\n");
            fprintf(stderr, "[probe] Check: device path is VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK.\n");
            fflush(stderr);
        }
        handler->Release();
        return false;
    }

    hr = handler->Wait();
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: activation did not complete (0x%08lX)\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        handler->Release();
        return false;
    }

    hr = handler->m_activatedObject->QueryInterface(kIidAudioClient, st.audioClient.put_void());
    handler->Release();

    if (FAILED(hr) || st.audioClient == nullptr) {
        fprintf(stderr, "[probe] FAIL: QI for IAudioClient 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    ZeroMemory(&st.mixFormat, sizeof(st.mixFormat));
    st.mixFormat.Format.wFormatTag = WAVE_FORMAT_PCM;
    st.mixFormat.Format.nChannels = 2;
    st.mixFormat.Format.nSamplesPerSec = 44100;
    st.mixFormat.Format.wBitsPerSample = 16;
    st.mixFormat.Format.nBlockAlign = 4;
    st.mixFormat.Format.nAvgBytesPerSec = 44100 * 4;
    st.mixFormat.Format.cbSize = 0;
    st.isFloat = false;
    st.bytesPerFrame = st.mixFormat.Format.nBlockAlign;

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    hr = st.audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 0, 0, &st.mixFormat.Format, nullptr);
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: Initialize 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = st.audioClient->GetBufferSize(&st.bufferFrameCount);
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: GetBufferSize 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = st.audioClient->GetService(kIidAudioCaptureClient, st.captureClient.put_void());
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: GetService(IAudioCaptureClient) 0x%08lX\n", static_cast<unsigned long>(hr));
        fflush(stderr);
        return false;
    }

    hr = st.audioClient->Start();
    if (FAILED(hr)) {
        fprintf(stderr, "[probe] FAIL: Start 0x%08lX\n", static_cast<unsigned long>(hr));
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

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            st.intervalSilentFrames += numFrames;
            st.totalSilentFrames += numFrames;
        } else {
            uint64_t totalSamples = static_cast<uint64_t>(numFrames) * st.mixFormat.Format.nChannels;
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

StreamSnapshot Probe::SnapshotInterval(Stream& st, DWORD pid) {
    StreamSnapshot snap;
    snap.kind = st.kind;
    snap.pid = pid;
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
                "[metrics] stream=%s pid=%lu rate=%u channels=%u packets=%llu frames=%llu rms=%.3f "
                "peak=%.3f silence=n/a discontinuities=%llu\n",
                s.kind.c_str(), static_cast<unsigned long>(s.pid), s.sampleRate, s.channels,
                static_cast<unsigned long long>(s.packets), static_cast<unsigned long long>(s.frames), s.rms, s.peak,
                static_cast<unsigned long long>(s.discontinuities));
    } else {
        fprintf(stdout,
                "[metrics] stream=%s pid=%lu rate=%u channels=%u packets=%llu frames=%llu rms=%.3f "
                "peak=%.3f silence=%.1f%% discontinuities=%llu\n",
                s.kind.c_str(), static_cast<unsigned long>(s.pid), s.sampleRate, s.channels,
                static_cast<unsigned long long>(s.packets), static_cast<unsigned long long>(s.frames), s.rms, s.peak,
                s.silencePct, static_cast<unsigned long long>(s.discontinuities));
    }
    fflush(stdout);
}

void Probe::PrintSummary(const Stream& st, DWORD pid) {
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
    fprintf(stdout, "  target PID         : %lu\n", static_cast<unsigned long>(pid));
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
    fprintf(stdout, "[probe] Windows Process Loopback probe\n");
    fprintf(stdout, "[probe] requires: Windows 10 Build 20348+ / Windows 11 with Process Loopback API\n");
    fflush(stdout);

    if (!SelectTarget()) {
        return false;
    }

    fprintf(stdout, "\n[probe] activating INCLUDE target process tree...\n");
    fflush(stdout);
    if (!InitStream(m_include, PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE)) {
        fprintf(stderr, "[probe] ABORT: include_target_tree init failed\n");
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "[probe] activating EXCLUDE target process tree...\n");
    fflush(stdout);
    if (!InitStream(m_exclude, PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE)) {
        fprintf(stderr, "[probe] ABORT: exclude_target_tree init failed\n");
        fflush(stderr);
        return false;
    }

    fprintf(stdout, "\n");
    fprintf(stdout, "[probe] include_target_tree (APP):\n");
    fprintf(stdout, "  probe capture format: PCM, %u Hz, %u channels, %u-bit\n",
            m_include.mixFormat.Format.nSamplesPerSec, m_include.mixFormat.Format.nChannels,
            m_include.mixFormat.Format.wBitsPerSample);
    fprintf(stdout, "  buffer frames: %u\n", m_include.bufferFrameCount);
    fflush(stdout);

    fprintf(stdout, "[probe] exclude_target_tree (SYS):\n");
    fprintf(stdout, "  probe capture format: PCM, %u Hz, %u channels, %u-bit\n",
            m_exclude.mixFormat.Format.nSamplesPerSec, m_exclude.mixFormat.Format.nChannels,
            m_exclude.mixFormat.Format.wBitsPerSample);
    fprintf(stdout, "  buffer frames: %u\n", m_exclude.bufferFrameCount);
    fflush(stdout);

    fprintf(stdout,
            "\n[probe] both process loopback streams started — capturing for %u seconds (press 'q' to stop early)\n\n",
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

        ProcessStream(m_include);
        ProcessStream(m_exclude);

        if (!CheckTargetProcessAlive()) {
            fprintf(stdout, "\n[probe] target process exited\n");
            fflush(stdout);
            break;
        }
        now = std::chrono::steady_clock::now();
        if (now >= nextMetricTime) {
            auto incSnap = SnapshotInterval(m_include, m_targetPid);
            auto excSnap = SnapshotInterval(m_exclude, m_targetPid);
            PrintMetrics(incSnap);
            PrintMetrics(excSnap);
            ResetInterval(m_include);
            ResetInterval(m_exclude);
            nextMetricTime = now + std::chrono::seconds(1);
        }
    }

    auto endActual = std::chrono::steady_clock::now();
    auto durationSecActual = std::chrono::duration<double>(endActual - startTime).count();

    fprintf(stdout, "\n=== PROCESS LOOPBACK PROBE SUMMARY ===\n");
    fprintf(stdout, "  duration (s)        : %.1f\n", durationSecActual);
    fprintf(stdout, "  target process loss : %s\n", m_targetExited ? "yes" : "no");
    fflush(stdout);

    PrintSummary(m_include, m_targetPid);
    PrintSummary(m_exclude, m_targetPid);

    fprintf(stdout, "\n[probe] capture complete.\n");
    fflush(stdout);
}

} // namespace process_loopback_probe

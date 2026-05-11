#pragma once

#include <windows.h>
#include <Audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

#include <winrt/base.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace process_loopback_probe {

struct StreamSnapshot {
    std::string kind;
    DWORD pid = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint64_t packets = 0;
    uint64_t frames = 0;
    double rms = 0.0;
    double peak = 0.0;
    double silencePct = 0.0;
    uint64_t discontinuities = 0;
};

struct WindowInfo {
    std::string title;
    DWORD pid;
    HWND hwnd;
};

class Probe {
  public:
    Probe();
    ~Probe();

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    bool Start();
    void Run(uint32_t durationSec);

  private:
    struct Stream {
        std::string kind;
        winrt::com_ptr<IAudioClient> audioClient;
        winrt::com_ptr<IAudioCaptureClient> captureClient;
        WAVEFORMATEXTENSIBLE mixFormat{};
        uint32_t bufferFrameCount = 0;
        bool isFloat = false;
        uint16_t bytesPerFrame = 0;

        uint64_t intervalPackets = 0;
        uint64_t intervalFrames = 0;
        double intervalSumSq = 0.0;
        double intervalMaxAbs = 0.0;
        uint64_t intervalSilentFrames = 0;
        uint64_t intervalDiscontinuities = 0;

        uint64_t totalPackets = 0;
        uint64_t totalFrames = 0;
        double totalSumSq = 0.0;
        double totalMaxAbs = 0.0;
        uint64_t totalSilentFrames = 0;
        uint64_t totalDiscontinuities = 0;
        bool hadNonSilent = false;
    };

    DWORD m_targetPid = 0;
    std::string m_targetTitle;
    HANDLE m_targetProcess = nullptr;
    Stream m_include;
    Stream m_exclude;
    bool m_targetExited = false;

    std::vector<WindowInfo> EnumerateWindows();
    bool SelectTarget();
    bool InitStream(Stream& st, PROCESS_LOOPBACK_MODE mode);
    void ProcessStream(Stream& st);
    static StreamSnapshot SnapshotInterval(Stream& st, DWORD pid);
    void ResetInterval(Stream& st);
    void PrintMetrics(const StreamSnapshot& s);
    static void PrintSummary(const Stream& st, DWORD pid);
    void CleanupStream(Stream& st);
    bool CheckTargetProcessAlive();
};

} // namespace process_loopback_probe

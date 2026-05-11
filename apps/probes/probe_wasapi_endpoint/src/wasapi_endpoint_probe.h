#pragma once

#include <Audioclient.h>
#include <mmdeviceapi.h>

#include <winrt/base.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace wasapi_probe {

struct StreamSnapshot {
    std::string kind;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint64_t packets = 0;
    uint64_t frames = 0;
    double rms = 0.0;
    double peak = 0.0;
    double silencePct = 0.0;
    uint64_t discontinuities = 0;
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
        std::string name;
        std::string kind;
        winrt::com_ptr<IMMDevice> device;
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

    Stream m_output;
    Stream m_input;

    bool InitStream(Stream& st, EDataFlow flow, bool loopback);
    void ProcessStream(Stream& st);
    static StreamSnapshot SnapshotInterval(Stream& st);
    void ResetInterval(Stream& st);
    void PrintMetrics(const StreamSnapshot& s);
    void PrintSummary(const Stream& st);
    void CleanupStream(Stream& st);
};

} // namespace wasapi_probe

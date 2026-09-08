// probe_gpup_nvenc -- does NVENC work through a Hyper-V GPU partition?
//
// A partitioned adapter is not the host adapter. The guest runs driver files copied
// from the host rather than an installed driver package, presents no display of its
// own, and reaches the encoder through a paravirtualised device. Any of that can be
// wrong in a way that leaves DXGI enumeration working and encoding not, which is the
// worst shape of failure to discover from a release gate: the gate reports a product
// defect and the product is fine.
//
// So this asks the smallest question that has to be true before a capture-and-encode
// gate may run in the guest at all -- open a session on the first adapter, read what
// the encoder says it can do, and push 60 frames through it -- and answers in JSON, on
// stdout, exit 0 for yes.
//
// It runs on the host too, and should: the host answer is the reference the guest
// answer is compared against.

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include "nvEncodeAPI.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kWidth = 1920;
constexpr uint32_t kHeight = 1080;
constexpr uint32_t kFrameRateNum = 60;
constexpr uint32_t kFrameRateDen = 1;
constexpr int kFrameCount = 60;

struct CodecEntry {
    const char* name;
    GUID guid;
};

struct CapEntry {
    const char* name;
    NV_ENC_CAPS cap;
};

const CapEntry kCaps[] = {
    {"maxWidth", NV_ENC_CAPS_WIDTH_MAX},
    {"maxHeight", NV_ENC_CAPS_HEIGHT_MAX},
    {"maxBFrames", NV_ENC_CAPS_NUM_MAX_BFRAMES},
    {"encoderEngines", NV_ENC_CAPS_NUM_ENCODER_ENGINES},
    {"yuv444", NV_ENC_CAPS_SUPPORT_YUV444_ENCODE},
    {"tenBit", NV_ENC_CAPS_SUPPORT_10BIT_ENCODE},
    {"lossless", NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE},
    {"lookahead", NV_ENC_CAPS_SUPPORT_LOOKAHEAD},
    {"temporalAq", NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ},
};

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string ToUtf8(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

const char* StatusName(NVENCSTATUS status) {
    switch (status) {
    case NV_ENC_SUCCESS:
        return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE:
        return "NV_ENC_ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE:
        return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE:
        return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE:
        return "NV_ENC_ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST:
        return "NV_ENC_ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PARAM:
        return "NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL:
        return "NV_ENC_ERR_INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY:
        return "NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_UNSUPPORTED_PARAM:
        return "NV_ENC_ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_INVALID_VERSION:
        return "NV_ENC_ERR_INVALID_VERSION";
    case NV_ENC_ERR_NEED_MORE_INPUT:
        return "NV_ENC_ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY:
        return "NV_ENC_ERR_ENCODER_BUSY";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY:
        return "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY";
    case NV_ENC_ERR_UNIMPLEMENTED:
        return "NV_ENC_ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_GENERIC:
        return "NV_ENC_ERR_GENERIC";
    default:
        return "NV_ENC_ERR_UNKNOWN";
    }
}

int Fail(const char* stage, const std::string& reason) {
    printf("{\"ok\":false,\"stage\":\"%s\",\"error\":\"%s\"}\n", stage, JsonEscape(reason).c_str());
    fflush(stdout);
    return 1;
}

class Probe {
  public:
    ~Probe() {
        if (encoder_ != nullptr) {
            if (input_ != nullptr) {
                functions_.nvEncDestroyInputBuffer(encoder_, input_);
            }
            if (bitstream_ != nullptr) {
                functions_.nvEncDestroyBitstreamBuffer(encoder_, bitstream_);
            }
            functions_.nvEncDestroyEncoder(encoder_);
        }
        if (context_ != nullptr) {
            context_->Release();
        }
        if (device_ != nullptr) {
            device_->Release();
        }
        if (library_ != nullptr) {
            FreeLibrary(library_);
        }
    }

    int Run();

  private:
    bool OpenAdapter(std::string& error);
    bool OpenSession(std::string& error);
    void ReportCodecs(std::string& json);
    bool Encode(std::string& json, std::string& error);

    HMODULE library_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST functions_{};
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    void* encoder_ = nullptr;
    NV_ENC_INPUT_PTR input_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_ = nullptr;
    std::string adapterJson_;
};

bool Probe::OpenAdapter(std::string& error) {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
        error = "CreateDXGIFactory1 failed";
        return false;
    }

    // The FIRST adapter, deliberately, and not the first NVIDIA one: inside the guest
    // there is exactly one adapter and it is the partition. A probe that searched for
    // vendor 0x10DE would silently answer about a different device on a host with a
    // second GPU, and the answer is meant to be about the device the encoder gates use.
    IDXGIAdapter1* adapter = nullptr;
    hr = factory->EnumAdapters1(0, &adapter);
    factory->Release();
    if (FAILED(hr) || adapter == nullptr) {
        error = "no DXGI adapter at index 0";
        return false;
    }

    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device_,
                           nullptr, &context_);
    adapter->Release();
    if (FAILED(hr) || device_ == nullptr) {
        char buf[96];
        snprintf(buf, sizeof(buf), "D3D11CreateDevice failed 0x%08lX", static_cast<unsigned long>(hr));
        error = buf;
        return false;
    }

    char buffer[512];
    snprintf(buffer, sizeof(buffer),
             "{\"index\":0,\"description\":\"%s\",\"vendorId\":\"0x%04X\",\"deviceId\":\"0x%04X\","
             "\"dedicatedVideoMemoryMB\":%llu,\"software\":%s}",
             JsonEscape(ToUtf8(desc.Description)).c_str(), static_cast<unsigned>(desc.VendorId),
             static_cast<unsigned>(desc.DeviceId),
             static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024ull * 1024ull)),
             ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) ? "true" : "false");
    adapterJson_ = buffer;
    return true;
}

bool Probe::OpenSession(std::string& error) {
    // Loaded by name from the system directory. In the guest this file arrived as a
    // copy of the host's, and a missing one is the single most likely way the driver
    // staging step went wrong.
    library_ = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (library_ == nullptr) {
        error = "nvEncodeAPI64.dll did not load; the host driver libraries are not in System32";
        return false;
    }

    using CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto create = reinterpret_cast<CreateInstance>(GetProcAddress(library_, "NvEncodeAPICreateInstance"));
    if (create == nullptr) {
        error = "NvEncodeAPICreateInstance is not exported";
        return false;
    }

    functions_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS status = create(&functions_);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("NvEncodeAPICreateInstance: ") + StatusName(status);
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = device_;
    params.apiVersion = NVENCAPI_VERSION;

    status = functions_.nvEncOpenEncodeSessionEx(&params, &encoder_);
    if (status != NV_ENC_SUCCESS || encoder_ == nullptr) {
        error = std::string("nvEncOpenEncodeSessionEx: ") + StatusName(status);
        return false;
    }
    return true;
}

void Probe::ReportCodecs(std::string& json) {
    const CodecEntry known[] = {
        {"h264", NV_ENC_CODEC_H264_GUID},
        {"hevc", NV_ENC_CODEC_HEVC_GUID},
        {"av1", NV_ENC_CODEC_AV1_GUID},
    };

    uint32_t count = 0;
    std::vector<GUID> guids;
    if (functions_.nvEncGetEncodeGUIDCount(encoder_, &count) == NV_ENC_SUCCESS && count > 0) {
        guids.resize(count);
        uint32_t got = 0;
        if (functions_.nvEncGetEncodeGUIDs(encoder_, guids.data(), count, &got) == NV_ENC_SUCCESS) {
            guids.resize(got);
        } else {
            guids.clear();
        }
    }

    json += "[";
    bool first = true;
    for (const CodecEntry& codec : known) {
        bool supported = false;
        for (const GUID& guid : guids) {
            if (IsEqualGUID(guid, codec.guid) != 0) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            continue;
        }
        if (!first) {
            json += ",";
        }
        first = false;
        json += "{\"codec\":\"";
        json += codec.name;
        json += "\",\"caps\":{";
        bool firstCap = true;
        for (const CapEntry& entry : kCaps) {
            NV_ENC_CAPS_PARAM param{};
            param.version = NV_ENC_CAPS_PARAM_VER;
            param.capsToQuery = entry.cap;
            int value = 0;
            if (functions_.nvEncGetEncodeCaps(encoder_, codec.guid, &param, &value) != NV_ENC_SUCCESS) {
                continue;
            }
            if (!firstCap) {
                json += ",";
            }
            firstCap = false;
            char buffer[96];
            snprintf(buffer, sizeof(buffer), "\"%s\":%d", entry.name, value);
            json += buffer;
        }
        json += "}}";
    }
    json += "]";
}

bool Probe::Encode(std::string& json, std::string& error) {
    NV_ENC_PRESET_CONFIG preset{};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    NVENCSTATUS status = functions_.nvEncGetEncodePresetConfigEx(
        encoder_, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID, NV_ENC_TUNING_INFO_LOW_LATENCY, &preset);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("nvEncGetEncodePresetConfigEx: ") + StatusName(status);
        return false;
    }

    NV_ENC_CONFIG config{};
    std::memcpy(&config, &preset.presetCfg, sizeof(NV_ENC_CONFIG));
    config.version = NV_ENC_CONFIG_VER;

    NV_ENC_INITIALIZE_PARAMS init{};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_H264_GUID;
    init.presetGUID = NV_ENC_PRESET_P4_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init.encodeWidth = kWidth;
    init.encodeHeight = kHeight;
    init.darWidth = kWidth;
    init.darHeight = kHeight;
    init.maxEncodeWidth = kWidth;
    init.maxEncodeHeight = kHeight;
    init.frameRateNum = kFrameRateNum;
    init.frameRateDen = kFrameRateDen;
    init.enablePTD = 1;
    init.encodeConfig = &config;

    status = functions_.nvEncInitializeEncoder(encoder_, &init);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("nvEncInitializeEncoder: ") + StatusName(status);
        return false;
    }

    NV_ENC_CREATE_INPUT_BUFFER inputParams{};
    inputParams.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    inputParams.width = kWidth;
    inputParams.height = kHeight;
    inputParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
    status = functions_.nvEncCreateInputBuffer(encoder_, &inputParams);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("nvEncCreateInputBuffer: ") + StatusName(status);
        return false;
    }
    input_ = inputParams.inputBuffer;

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamParams{};
    bitstreamParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    status = functions_.nvEncCreateBitstreamBuffer(encoder_, &bitstreamParams);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("nvEncCreateBitstreamBuffer: ") + StatusName(status);
        return false;
    }
    bitstream_ = bitstreamParams.bitstreamBuffer;

    int packets = 0;
    uint64_t bytes = 0;
    const auto started = std::chrono::steady_clock::now();

    auto drain = [&]() -> bool {
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = bitstream_;
        lock.doNotWait = 0;
        if (functions_.nvEncLockBitstream(encoder_, &lock) != NV_ENC_SUCCESS) {
            return false;
        }
        packets++;
        bytes += lock.bitstreamSizeInBytes;
        return functions_.nvEncUnlockBitstream(encoder_, bitstream_) == NV_ENC_SUCCESS;
    };

    for (int frame = 0; frame < kFrameCount; ++frame) {
        NV_ENC_LOCK_INPUT_BUFFER lockInput{};
        lockInput.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lockInput.inputBuffer = input_;
        status = functions_.nvEncLockInputBuffer(encoder_, &lockInput);
        if (status != NV_ENC_SUCCESS) {
            error = std::string("nvEncLockInputBuffer: ") + StatusName(status);
            return false;
        }

        // A moving luma ramp rather than flat grey. A constant frame lets the encoder
        // emit near-empty pictures after the first one, which would make "60 frames
        // encoded" true and meaningless.
        auto* plane = static_cast<uint8_t*>(lockInput.bufferDataPtr);
        const uint32_t pitch = lockInput.pitch;
        for (uint32_t row = 0; row < kHeight; ++row) {
            std::memset(plane + static_cast<size_t>(row) * pitch,
                        static_cast<int>((row + static_cast<uint32_t>(frame) * 4u) & 0xFFu), pitch);
        }
        std::memset(plane + static_cast<size_t>(pitch) * kHeight, 128, static_cast<size_t>(pitch) * (kHeight / 2));

        status = functions_.nvEncUnlockInputBuffer(encoder_, input_);
        if (status != NV_ENC_SUCCESS) {
            error = std::string("nvEncUnlockInputBuffer: ") + StatusName(status);
            return false;
        }

        NV_ENC_PIC_PARAMS pic{};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputWidth = kWidth;
        pic.inputHeight = kHeight;
        pic.inputPitch = pitch;
        pic.inputBuffer = input_;
        pic.outputBitstream = bitstream_;
        pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = static_cast<uint64_t>(frame);

        status = functions_.nvEncEncodePicture(encoder_, &pic);
        if (status == NV_ENC_SUCCESS) {
            if (!drain()) {
                error = "nvEncLockBitstream failed on an accepted picture";
                return false;
            }
        } else if (status != NV_ENC_ERR_NEED_MORE_INPUT) {
            error = std::string("nvEncEncodePicture: ") + StatusName(status);
            return false;
        }
    }

    NV_ENC_PIC_PARAMS eos{};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    status = functions_.nvEncEncodePicture(encoder_, &eos);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("EOS nvEncEncodePicture: ") + StatusName(status);
        return false;
    }
    while (packets < kFrameCount && drain()) {
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

    if (packets == 0 || bytes == 0) {
        error = "the encoder accepted every frame and produced no bitstream";
        return false;
    }

    char buffer[320];
    snprintf(buffer, sizeof(buffer),
             "{\"codec\":\"h264\",\"width\":%u,\"height\":%u,\"framesSubmitted\":%d,\"packets\":%d,"
             "\"bytes\":%llu,\"elapsedMs\":%lld}",
             kWidth, kHeight, kFrameCount, packets, static_cast<unsigned long long>(bytes),
             static_cast<long long>(elapsed));
    json = buffer;
    return true;
}

int Probe::Run() {
    std::string error;
    if (!OpenAdapter(error)) {
        return Fail("adapter", error);
    }
    if (!OpenSession(error)) {
        return Fail("session", error);
    }

    std::string codecs;
    ReportCodecs(codecs);

    std::string encode;
    if (!Encode(encode, error)) {
        return Fail("encode", error);
    }

    printf("{\"ok\":true,\"adapter\":%s,\"apiVersion\":\"%u.%u\",\"codecs\":%s,\"encode\":%s}\n",
           adapterJson_.c_str(), static_cast<unsigned>(NVENCAPI_MAJOR_VERSION),
           static_cast<unsigned>(NVENCAPI_MINOR_VERSION), codecs.c_str(), encode.c_str());
    fflush(stdout);
    return 0;
}

} // namespace

int main() {
    Probe probe;
    return probe.Run();
}

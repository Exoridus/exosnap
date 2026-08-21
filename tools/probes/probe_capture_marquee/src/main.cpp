// Marquee capture probe.
//
// Captures ONE monitor through DXGI Desktop Duplication and Windows Graphics
// Capture at the same time, from one process, and records a per-frame mean
// luma of a fixed monitor-local rectangle for each backend. Purpose: decide
// whether a shell visual that is missing from individual captured frames (an
// Explorer drag-selection rectangle) is missing from one backend, from both, or
// from neither.
//
// The two backends run on separate D3D11 devices and separate threads so that
// neither serialises the other; they observe the same desktop at the same time,
// which a sequential A/B run cannot guarantee.
//
// Output is a CSV of samples; no judgement is made here.

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    int display = 1;       // \\.\DISPLAY<n>
    double seconds = 12.0; // capture duration
    double delay = 0.0;    // lead-in before capture starts, for the operator to get into position
    int rect_x = -1;       // monitor-local; -1 => centred
    int rect_y = -1;
    int rect_w = 800;
    int rect_h = 600;
    std::string out = "capture-marquee.csv";
    bool run_od = true;
    bool run_wgc = true;
};

struct Sample {
    double t_ms = 0.0;     // probe clock, ms since start
    uint64_t src_ts = 0;   // OD: LastPresentTime QPC. WGC: SystemRelativeTime (100ns).
    uint64_t mouse_ts = 0; // OD only: LastMouseUpdateTime QPC.
    uint32_t accumulated = 0;
    uint32_t meta_bytes = 0;
    uint32_t dirty_rects = 0;
    double mean = 0.0;
};

int64_t QpcFreq() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
}

int64_t QpcNow() {
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

// Rec.709 luma on a 0..255 scale, subsampled by 2 in both axes.
double MeanLuma(const D3D11_MAPPED_SUBRESOURCE& map, UINT width, UINT height, DXGI_FORMAT format) {
    const auto* base = static_cast<const uint8_t*>(map.pData);
    double sum = 0.0;
    uint64_t count = 0;

    for (UINT y = 0; y < height; y += 2) {
        const uint8_t* row = base + static_cast<size_t>(y) * map.RowPitch;
        for (UINT x = 0; x < width; x += 2) {
            double r = 0.0;
            double g = 0.0;
            double b = 0.0;
            switch (format) {
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8X8_UNORM: {
                const uint8_t* px = row + static_cast<size_t>(x) * 4;
                b = px[0];
                g = px[1];
                r = px[2];
                break;
            }
            case DXGI_FORMAT_R8G8B8A8_UNORM: {
                const uint8_t* px = row + static_cast<size_t>(x) * 4;
                r = px[0];
                g = px[1];
                b = px[2];
                break;
            }
            case DXGI_FORMAT_R10G10B10A2_UNORM: {
                uint32_t v = 0;
                std::memcpy(&v, row + static_cast<size_t>(x) * 4, sizeof(v));
                r = static_cast<double>(v & 0x3FFu) * 255.0 / 1023.0;
                g = static_cast<double>((v >> 10) & 0x3FFu) * 255.0 / 1023.0;
                b = static_cast<double>((v >> 20) & 0x3FFu) * 255.0 / 1023.0;
                break;
            }
            default:
                return -1.0;
            }
            sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
            ++count;
        }
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

// Staging texture matching the sampled rectangle, created once per backend from
// the first frame's actual format (the desktop surface format is negotiated, not
// declared: a 10 bpc desktop hands out R10G10B10A2 where the mode desc says BGRA8).
struct Sampler {
    winrt::com_ptr<ID3D11Texture2D> staging;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;

    bool Ensure(ID3D11Device* device, ID3D11Texture2D* source, UINT w, UINT h) {
        D3D11_TEXTURE2D_DESC src{};
        source->GetDesc(&src);
        if (staging && src.Format == format)
            return true;
        staging = nullptr;
        format = src.Format;
        width = w;
        height = h;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = src.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.put()));
    }
};

// Copy the sampled rectangle out of a capture texture and average it.
// Returns -1.0 when the rectangle does not fit or the format is unsupported.
double SampleRect(ID3D11Device* device, ID3D11DeviceContext* context, Sampler& sampler, ID3D11Texture2D* source,
                  const Options& opt, int rx, int ry) {
    D3D11_TEXTURE2D_DESC src{};
    source->GetDesc(&src);
    if (rx < 0 || ry < 0 || static_cast<UINT>(rx + opt.rect_w) > src.Width ||
        static_cast<UINT>(ry + opt.rect_h) > src.Height)
        return -1.0;
    if (!sampler.Ensure(device, source, static_cast<UINT>(opt.rect_w), static_cast<UINT>(opt.rect_h)))
        return -1.0;

    D3D11_BOX box{};
    box.left = static_cast<UINT>(rx);
    box.top = static_cast<UINT>(ry);
    box.front = 0;
    box.right = static_cast<UINT>(rx + opt.rect_w);
    box.bottom = static_cast<UINT>(ry + opt.rect_h);
    box.back = 1;
    context->CopySubresourceRegion(sampler.staging.get(), 0, 0, 0, 0, source, 0, &box);

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context->Map(sampler.staging.get(), 0, D3D11_MAP_READ, 0, &map)))
        return -1.0;
    const double mean = MeanLuma(map, sampler.width, sampler.height, sampler.format);
    context->Unmap(sampler.staging.get(), 0);
    return mean;
}

bool FindOutput(int display, winrt::com_ptr<IDXGIAdapter1>& out_adapter, winrt::com_ptr<IDXGIOutput1>& out_output,
                DXGI_OUTPUT_DESC& out_desc) {
    wchar_t wanted[32];
    swprintf_s(wanted, L"\\\\.\\DISPLAY%d", display);

    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
        return false;

    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        winrt::com_ptr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop && wcscmp(desc.DeviceName, wanted) == 0) {
                out_adapter = adapter;
                out_output = output.as<IDXGIOutput1>();
                out_desc = desc;
                return true;
            }
            output = nullptr;
        }
        adapter = nullptr;
    }
    return false;
}

bool CreateDeviceOn(IDXGIAdapter1* adapter, winrt::com_ptr<ID3D11Device>& out_device,
                    winrt::com_ptr<ID3D11DeviceContext>& out_context) {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    return SUCCEEDED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, out_device.put(), nullptr,
                                       out_context.put()));
}

void RunOd(const Options& opt, IDXGIAdapter1* adapter, IDXGIOutput1* output, int rx, int ry, int64_t t0, double qpc_ms,
           std::vector<Sample>& out, std::atomic<bool>& fail) {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    if (!CreateDeviceOn(adapter, device, context)) {
        fprintf(stderr, "[od] device creation failed\n");
        fail = true;
        return;
    }

    winrt::com_ptr<IDXGIOutputDuplication> dupl;
    const HRESULT hr = output->DuplicateOutput(device.get(), dupl.put());
    if (FAILED(hr)) {
        fprintf(stderr, "[od] DuplicateOutput failed 0x%08X\n", static_cast<unsigned>(hr));
        fail = true;
        return;
    }

    Sampler sampler;
    std::vector<uint8_t> metadata;
    const int64_t deadline = t0 + static_cast<int64_t>(opt.seconds * 1000.0 / qpc_ms);

    while (QpcNow() < deadline) {
        DXGI_OUTDUPL_FRAME_INFO info{};
        winrt::com_ptr<IDXGIResource> resource;
        const HRESULT acq = dupl->AcquireNextFrame(8, &info, resource.put());
        if (acq == DXGI_ERROR_WAIT_TIMEOUT)
            continue;
        if (FAILED(acq)) {
            fprintf(stderr, "[od] AcquireNextFrame failed 0x%08X\n", static_cast<unsigned>(acq));
            fail = true;
            return;
        }

        Sample s;
        s.t_ms = static_cast<double>(QpcNow() - t0) * qpc_ms;
        s.src_ts = static_cast<uint64_t>(info.LastPresentTime.QuadPart);
        s.mouse_ts = static_cast<uint64_t>(info.LastMouseUpdateTime.QuadPart);
        s.accumulated = info.AccumulatedFrames;
        s.meta_bytes = info.TotalMetadataBufferSize;

        if (info.TotalMetadataBufferSize > 0) {
            metadata.resize(info.TotalMetadataBufferSize);
            UINT used = 0;
            if (SUCCEEDED(dupl->GetFrameDirtyRects(info.TotalMetadataBufferSize,
                                                   reinterpret_cast<RECT*>(metadata.data()), &used)))
                s.dirty_rects = used / static_cast<UINT>(sizeof(RECT));
        }

        auto texture = resource.as<ID3D11Texture2D>();
        s.mean = SampleRect(device.get(), context.get(), sampler, texture.get(), opt, rx, ry);
        out.push_back(s);

        texture = nullptr;
        resource = nullptr;
        dupl->ReleaseFrame();
    }
}

void RunWgc(const Options& opt, IDXGIAdapter1* adapter, HMONITOR monitor, int rx, int ry, int64_t t0, double qpc_ms,
            std::vector<Sample>& out, std::atomic<bool>& fail) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    if (!CreateDeviceOn(adapter, device, context)) {
        fprintf(stderr, "[wgc] device creation failed\n");
        fail = true;
        return;
    }

    namespace capture = winrt::Windows::Graphics::Capture;
    capture::GraphicsCaptureItem item{nullptr};
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};

    try {
        auto interop = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(
            interop->CreateForMonitor(monitor, winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item)));

        winrt::com_ptr<IDXGIDevice> dxgi = device.as<IDXGIDevice>();
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
        auto winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        pool = capture::Direct3D11CaptureFramePool::Create(
            winrtDevice, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, item.Size());
        session = pool.CreateCaptureSession(item);
        // Match the OD path, which never composites the pointer, and keep the
        // capture border off the sampled desktop where the OS allows it.
        session.IsCursorCaptureEnabled(false);
        try {
            session.IsBorderRequired(false);
        } catch (const winrt::hresult_error&) {
            fprintf(stdout, "[wgc] border could not be disabled (older OS or policy)\n");
        }
        session.StartCapture();
    } catch (const winrt::hresult_error& e) {
        fprintf(stderr, "[wgc] setup failed 0x%08X\n", static_cast<unsigned>(e.code().value));
        fail = true;
        return;
    }

    Sampler sampler;
    const int64_t deadline = t0 + static_cast<int64_t>(opt.seconds * 1000.0 / qpc_ms);

    while (QpcNow() < deadline) {
        auto frame = pool.TryGetNextFrame();
        if (frame == nullptr) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        Sample s;
        s.t_ms = static_cast<double>(QpcNow() - t0) * qpc_ms;
        s.src_ts = static_cast<uint64_t>(frame.SystemRelativeTime().count());

        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        if (SUCCEEDED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void())))
            s.mean = SampleRect(device.get(), context.get(), sampler, texture.get(), opt, rx, ry);
        else
            s.mean = -1.0;

        out.push_back(s);
        texture = nullptr;
        frame.Close();
    }

    session.Close();
    pool.Close();
}

bool ParseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char*& value) {
            if (i + 1 >= argc)
                return false;
            value = argv[++i];
            return true;
        };
        const char* value = nullptr;
        if (arg == "--display" && next(value)) {
            opt.display = atoi(value);
        } else if (arg == "--seconds" && next(value)) {
            opt.seconds = atof(value);
        } else if (arg == "--delay" && next(value)) {
            opt.delay = atof(value);
        } else if (arg == "--rect" && next(value)) {
            if (sscanf_s(value, "%d,%d,%d,%d", &opt.rect_x, &opt.rect_y, &opt.rect_w, &opt.rect_h) != 4) {
                fprintf(stderr, "--rect expects x,y,w,h\n");
                return false;
            }
        } else if (arg == "--size" && next(value)) {
            if (sscanf_s(value, "%d,%d", &opt.rect_w, &opt.rect_h) != 2) {
                fprintf(stderr, "--size expects w,h\n");
                return false;
            }
        } else if (arg == "--out" && next(value)) {
            opt.out = value;
        } else if (arg == "--no-od") {
            opt.run_od = false;
        } else if (arg == "--no-wgc") {
            opt.run_wgc = false;
        } else {
            fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

void WriteCsv(const Options& opt, const std::vector<Sample>& od, const std::vector<Sample>& wgc) {
    FILE* f = nullptr;
    if (fopen_s(&f, opt.out.c_str(), "w") != 0 || f == nullptr) {
        fprintf(stderr, "cannot write %s\n", opt.out.c_str());
        return;
    }
    fprintf(f, "source,t_ms,src_ts,mouse_ts,accumulated,meta_bytes,dirty_rects,mean\n");
    auto dump = [&](const char* name, const std::vector<Sample>& samples) {
        for (const Sample& s : samples)
            fprintf(f, "%s,%.3f,%llu,%llu,%u,%u,%u,%.4f\n", name, s.t_ms, static_cast<unsigned long long>(s.src_ts),
                    static_cast<unsigned long long>(s.mouse_ts), s.accumulated, s.meta_bytes, s.dirty_rects, s.mean);
    };
    dump("od", od);
    dump("wgc", wgc);
    fclose(f);
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt))
        return 2;

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    winrt::com_ptr<IDXGIAdapter1> adapter;
    winrt::com_ptr<IDXGIOutput1> output;
    DXGI_OUTPUT_DESC outputDesc{};
    if (!FindOutput(opt.display, adapter, output, outputDesc)) {
        fprintf(stderr, "display %d not found\n", opt.display);
        return 1;
    }

    const int monitorW = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    const int monitorH = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
    const int rx = opt.rect_x >= 0 ? opt.rect_x : (monitorW - opt.rect_w) / 2;
    const int ry = opt.rect_y >= 0 ? opt.rect_y : (monitorH - opt.rect_h) / 2;

    fprintf(stdout, "[probe] display %d %dx%d, sampling rect %d,%d %dx%d for %.1f s\n", opt.display, monitorW, monitorH,
            rx, ry, opt.rect_w, opt.rect_h, opt.seconds);
    fflush(stdout);

    for (int remaining = static_cast<int>(opt.delay); remaining > 0; --remaining) {
        fprintf(stdout, "[probe] starting in %d s\n", remaining);
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    const double qpc_ms = 1000.0 / static_cast<double>(QpcFreq());
    const int64_t t0 = QpcNow();
    fprintf(stdout, "[probe] capturing\n");
    fflush(stdout);

    std::vector<Sample> odSamples;
    std::vector<Sample> wgcSamples;
    odSamples.reserve(4096);
    wgcSamples.reserve(4096);
    std::atomic<bool> odFail{false};
    std::atomic<bool> wgcFail{false};

    std::thread odThread;
    std::thread wgcThread;
    if (opt.run_od)
        odThread = std::thread(RunOd, std::cref(opt), adapter.get(), output.get(), rx, ry, t0, qpc_ms,
                               std::ref(odSamples), std::ref(odFail));
    if (opt.run_wgc)
        wgcThread = std::thread(RunWgc, std::cref(opt), adapter.get(), outputDesc.Monitor, rx, ry, t0, qpc_ms,
                                std::ref(wgcSamples), std::ref(wgcFail));
    if (odThread.joinable())
        odThread.join();
    if (wgcThread.joinable())
        wgcThread.join();

    fprintf(stdout, "[probe] od frames=%zu (fail=%d), wgc frames=%zu (fail=%d)\n", odSamples.size(),
            odFail.load() ? 1 : 0, wgcSamples.size(), wgcFail.load() ? 1 : 0);
    WriteCsv(opt, odSamples, wgcSamples);
    fprintf(stdout, "[probe] wrote %s\n", opt.out.c_str());
    return (odFail || wgcFail) ? 1 : 0;
}

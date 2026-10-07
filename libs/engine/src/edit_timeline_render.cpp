#include <exosnap/engine/codec_types.h>
#include <exosnap/engine/edit_timeline_compositor.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <exosnap/engine/encoder_device.h>
#include <exosnap/engine/gpu_surface_inventory.h>
#include <exosnap/engine/interfaces/IAudioEncoder.h>
#include <exosnap/engine/interfaces/IVideoEncoder.h>
#include <exosnap/engine/interfaces/VideoEncoderFactory.h>
#include <exosnap/engine/mp4_remuxer.h>
#include <exosnap/engine/packet_types.h>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/video_processor_color.h>

#include "annexb_to_avcc.h"
#include "annexb_to_hvcc.h"
#include "codec_private.h"
#include "matroska_stream_writer.h"
#include "pcm_audio_encoder.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ratio>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <winrt/base.h>

namespace exosnap::engine {
namespace {
bool Check(HRESULT status, const char* operation, std::string& error) {
    if (SUCCEEDED(status))
        return true;
    error = std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<uint32_t>(status)) + ").";
    return false;
}

struct RenderDevice {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    uint32_t vendor = 0;
    bool Open(const RecorderConfig& config, std::string& error) {
        winrt::com_ptr<IDXGIFactory1> factory;
        if (!Check(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()), "Create DXGI factory", error))
            return false;
        winrt::com_ptr<IDXGIAdapter1> selected;
        for (UINT index = 0;; ++index) {
            winrt::com_ptr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, adapter.put()) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                continue;
            const auto& preference = config.encoder_device;
            const bool explicit_device = preference.mode == EncoderDevicePreference::Mode::Explicit;
            if (explicit_device && (preference.device.vendor_id != description.VendorId ||
                                    preference.device.device_id != description.DeviceId ||
                                    preference.device.subsystem_id != description.SubSysId))
                continue;
            if (!explicit_device && ImplementedEncoderBackendForVendorId(description.VendorId) == EncoderBackend::None)
                continue;
            if (selected && explicit_device) {
                error = "The selected encoder adapter is ambiguous.";
                return false;
            }
            selected = std::move(adapter);
            vendor = description.VendorId;
            if (!explicit_device)
                break;
        }
        if (!selected) {
            error = "No supported encoder adapter is available for timeline rendering.";
            return false;
        }
        const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        return Check(D3D11CreateDevice(selected.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                       D3D11_SDK_VERSION, device.put(), nullptr, context.put()),
                     "Create timeline D3D11 device", error);
    }
};

struct EncodeSurfaces {
    winrt::com_ptr<ID3D11VideoDevice> device;
    winrt::com_ptr<ID3D11VideoContext1> context;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> enumerator;
    winrt::com_ptr<ID3D11VideoProcessor> processor;
    winrt::com_ptr<ID3D11VideoProcessorInputView> input;
    std::vector<winrt::com_ptr<ID3D11Texture2D>> textures;
    std::vector<winrt::com_ptr<ID3D11VideoProcessorOutputView>> outputs;

    bool Init(RenderDevice& gpu, IVideoEncoder& encoder, ID3D11Texture2D* source, const EditRenderPlan& plan,
              bool full_range, std::string& error) {
        device = gpu.device.try_as<ID3D11VideoDevice>();
        context = gpu.context.try_as<ID3D11VideoContext1>();
        if (!device || !context) {
            error = "Timeline encoding requires explicit D3D11 VideoContext1 color conversion.";
            return false;
        }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate = {static_cast<UINT>(plan.fps_num), static_cast<UINT>(plan.fps_den)};
        content.OutputFrameRate = content.InputFrameRate;
        content.InputWidth = content.OutputWidth = plan.output.width;
        content.InputHeight = content.OutputHeight = plan.output.height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (!Check(device->CreateVideoProcessorEnumerator(&content, enumerator.put()),
                   "Create timeline video enumerator", error) ||
            !Check(device->CreateVideoProcessor(enumerator.get(), 0, processor.put()),
                   "Create timeline video processor", error))
            return false;
        ConfigureSdrVideoProcessorColor(context.get(), processor.get(), full_range);
        context->VideoProcessorSetStreamAutoProcessingMode(processor.get(), 0, FALSE);
        context->VideoProcessorSetStreamFrameFormat(processor.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        const RECT rect{0, 0, static_cast<LONG>(plan.output.width), static_cast<LONG>(plan.output.height)};
        context->VideoProcessorSetStreamSourceRect(processor.get(), 0, TRUE, &rect);
        context->VideoProcessorSetStreamDestRect(processor.get(), 0, TRUE, &rect);
        context->VideoProcessorSetOutputTargetRect(processor.get(), TRUE, &rect);
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (!Check(device->CreateVideoProcessorInputView(source, enumerator.get(), &input_desc, input.put()),
                   "Create timeline input view", error))
            return false;
        if (encoder.SlotCount() <= 0 || encoder.SlotCount() > 128) {
            error = "Encoder returned an invalid input surface count.";
            return false;
        }
        textures.resize(encoder.SlotCount());
        outputs.resize(encoder.SlotCount());
        for (int32_t slot = 0; slot < encoder.SlotCount(); ++slot) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = plan.output.width;
            desc.Height = plan.output.height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_NV12;
            desc.SampleDesc.Count = 1;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
            output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (!Check(CreateTrackedTexture2D(gpu.device.get(), &desc, nullptr, textures[slot].put(),
                                              GpuSurfaceOwner::EncoderInputs),
                       "Create timeline encoder surface", error) ||
                !Check(device->CreateVideoProcessorOutputView(textures[slot].get(), enumerator.get(), &output_desc,
                                                              outputs[slot].put()),
                       "Create timeline output view", error) ||
                !encoder.RegisterSlotTexture(slot, textures[slot].get(), error))
                return false;
        }
        return true;
    }
    bool Convert(int32_t slot, std::string& error) {
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input.get();
        return Check(context->VideoProcessorBlt(processor.get(), outputs[slot].get(), 0, 1, &stream),
                     "Convert timeline encoder input", error);
    }
};

bool VideoPrivate(VideoCodec codec, const std::vector<uint8_t>& header, MatroskaStreamConfig& mux, std::string& error) {
    if (codec == VideoCodec::H264) {
        mux.video_codec_id = "V_MPEG4/ISO/AVC";
        std::vector<uint8_t> parameters;
        if (annexb::ExtractH264SpsAndPps(header.data(), header.size(), parameters) &&
            annexb::BuildAvccFromAnnexBSpsAndPps(parameters, mux.video_codec_private))
            return true;
    } else if (codec == VideoCodec::Hevc) {
        mux.video_codec_id = "V_MPEGH/ISO/HEVC";
        std::vector<uint8_t> parameters;
        if (annexb::ExtractHevcVpsSpsPps(header.data(), header.size(), parameters) &&
            annexb::BuildHvccFromAnnexBVpsSpsPps(parameters, mux.video_codec_private))
            return true;
    } else {
        mux.video_codec_id = "V_AV1";
        char reason[256]{};
        if (codec_private::DeriveAv1CodecPrivate(header.data(), header.size(), mux.video_codec_private, reason,
                                                 sizeof(reason)))
            return true;
    }
    error = "Encoder did not supply usable video codec private data.";
    return false;
}
} // namespace

RemuxResult RenderEditTimeline(const TimelineRenderSnapshot& snapshot, const EditRenderPlan& plan,
                               RecorderConfig config, const std::filesystem::path& output, bool to_mp4,
                               RemuxProgressCallback progress, TimelineRenderMeasurements* measurements) {
    const auto started = std::chrono::steady_clock::now();
    TimelineRenderMeasurements stats;
    std::string error;
    if (plan.path != EditExportPath::Render || to_mp4)
        return RemuxResult::Fail(-1, plan.reason.empty() ? "This render output profile is unsupported." : plan.reason);
    if (std::filesystem::exists(output))
        return RemuxResult::Fail(-1, "Render staging path already exists.");
    const auto run = [&]() -> bool {
        if (!progress(0)) {
            error = "Export cancelled.";
            return false;
        }
        RenderDevice gpu;
        if (!gpu.Open(config, error))
            return false;
        config.container = Container::Matroska;
        config.audio_codec = AudioCodec::Pcm;
        config.bit_depth = BitDepth::Bit8;
        config.chroma = ChromaSubsampling::Cs420;
        config.color = ColorMetadata::Sdr709();
        config.frame_rate_num = plan.fps_num;
        config.frame_rate_den = plan.fps_den;
        EditTimelineCompositor compositor;
        if (!compositor.Init(gpu.device.get(), gpu.context.get(), plan.output, error))
            return false;
        // Encoder must be destroyed before its registered surfaces and device.
        EncodeSurfaces surfaces;
        auto encoder = VideoEncoderFactory{}.CreateForAdapter(gpu.vendor, config, error);
        if (!encoder)
            return false;
        encoder->SetCodec(config.video_codec);
        encoder->SetBitDepth(config.bit_depth);
        encoder->SetChroma(config.chroma);
        encoder->SetColor(config.color);
        encoder->SetCq(config.cq);
        encoder->SetRateControl(config.rate_control_mode, config.target_bitrate_kbps);
        encoder->SetKeyframeIntervalSecs(config.keyframe_interval_secs);
        encoder->SetConstantFrameRate(true);
        if (!encoder->Open(gpu.device.get(), error) ||
            !encoder->Configure(plan.output.width, plan.output.height, plan.fps_num, plan.fps_den, error) ||
            !surfaces.Init(gpu, *encoder, compositor.Result(), plan, false, error))
            return false;
        const uint64_t frames =
            (snapshot.timeline.duration_us * static_cast<uint64_t>(plan.fps_num) + 1'000'000ULL * plan.fps_den - 1) /
            (1'000'000ULL * plan.fps_den);
        const uint64_t duration_ns = frames * 1'000'000'000ULL * plan.fps_den / plan.fps_num;
        const uint64_t audio_count = (duration_ns * 48000 + 500'000'000ULL) / 1'000'000'000ULL;
        MatroskaStreamConfig mux;
        const auto utf8 = output.u8string();
        mux.output_path.assign(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        mux.encode_width = plan.output.width;
        mux.encode_height = plan.output.height;
        mux.frame_rate_num = plan.fps_num;
        mux.frame_rate_den = plan.fps_den;
        mux.timeline_duration_ns = duration_ns;
        mux.color = config.color;
        const bool audio = std::any_of(snapshot.timeline.clips.begin(), snapshot.timeline.clips.end(),
                                       [](const auto& clip) { return clip.audio; });
        mux.audio_track_count = audio ? 1 : 0;
        mux.audio_codec = StreamAudioCodec::Pcm;
        mux.audio_bit_depth = 24;
        if (!VideoPrivate(config.video_codec, encoder->SequenceHeader(), mux, error))
            return false;
        MatroskaStreamWriter writer;
        if (!writer.Open(mux)) {
            error = writer.error();
            return false;
        }
        auto pcm_encoder = std::make_unique<PcmAudioEncoder>();
        pcm_encoder->SetBitDepth(mux.audio_bit_depth);
        std::unique_ptr<IAudioEncoder> audio_encoder = std::move(pcm_encoder);
        if (audio && !audio_encoder->Init(48000, 2, error))
            return false;
        EditTimelineReader reader;
        if (!reader.Open(snapshot, plan, error))
            return false;
        std::vector<EncodedVideoPacket> packets;
        const auto mux_video = [&]() {
            for (auto& packet : packets) {
                std::vector<uint8_t> bytes;
                bool converted = true;
                if (config.video_codec == VideoCodec::H264)
                    converted = annexb::ConvertAnnexBToAvcc(packet.bytes.data(), packet.bytes.size(), bytes);
                else if (config.video_codec == VideoCodec::Hevc)
                    converted = annexb::ConvertAnnexBToHevcSample(packet.bytes.data(), packet.bytes.size(), bytes);
                else
                    bytes = std::move(packet.bytes);
                if (!converted) {
                    error = "Cannot package encoded video sample.";
                    return false;
                }
                if (!writer.Push({packet.pts_ns, 1, packet.keyframe, std::move(bytes), packet.dts_ns})) {
                    error = writer.error();
                    return false;
                }
            }
            packets.clear();
            return true;
        };
        uint64_t audio_position = 0;
        uint64_t accumulated = 0;
        std::vector<float> samples;
        std::vector<EncodedAudioPacket> audio_packets;
        for (uint64_t frame_index = 0; frame_index < frames; ++frame_index) {
            if (!progress(static_cast<float>(frame_index) / static_cast<float>(frames))) {
                error = "Export cancelled.";
                return false;
            }
            TimelineVideoFrame frame;
            const uint64_t pts = frame_index * 1'000'000'000ULL * plan.fps_den / plan.fps_num;
            if (!reader.VideoAt(static_cast<int64_t>(pts / 1000), frame, error) ||
                !compositor.Compose(frame.layers, error))
                return false;
            int32_t slot = encoder->AcquireFreeSlot();
            const auto waiting = std::chrono::steady_clock::now();
            while (slot < 0) {
                if (!encoder->ReapCompleted(packets, error, 10) || !mux_video())
                    return false;
                if (!progress(static_cast<float>(frame_index) / static_cast<float>(frames))) {
                    error = "Export cancelled.";
                    return false;
                }
                if (std::chrono::steady_clock::now() - waiting > std::chrono::seconds(10)) {
                    error = "Encoder stopped returning reusable input slots.";
                    return false;
                }
                slot = encoder->AcquireFreeSlot();
            }
            if (!surfaces.Convert(slot, error)) {
                encoder->ReleaseSlot(slot);
                return false;
            }
            if (!encoder->EncodeFrame(slot, pts, plan.output.width, plan.output.height, packets, error) || !mux_video())
                return false;
            ++stats.video_frames;
            stats.peak_encoder_backlog = std::max(stats.peak_encoder_backlog, encoder->PendingFrames());
            const uint64_t through =
                frame_index + 1 == frames
                    ? audio_count
                    : std::min(audio_count, (frame_index + 1) * 48000ULL * plan.fps_den / plan.fps_num);
            while (audio && audio_position < through) {
                const auto count = static_cast<uint32_t>(std::min<uint64_t>(960, through - audio_position));
                if (!reader.AudioAt(static_cast<int64_t>(audio_position), count, samples, error))
                    return false;
                audio_encoder->FeedFloat32(samples.data(), samples.size(), audio_position * 1'000'000'000ULL / 48000,
                                           accumulated, 48000, 2, audio_packets);
                for (auto& packet : audio_packets)
                    if (!writer.Push({packet.pts_ns, 2, true, std::move(packet.bytes), {}})) {
                        error = writer.error();
                        return false;
                    }
                audio_packets.clear();
                audio_position += count;
            }
            if (!encoder->ReapCompleted(packets, error) || !mux_video())
                return false;
        }
        if (!encoder->Flush(packets, error) || !mux_video() || encoder->PendingFrames() != 0)
            return false;
        if (audio) {
            audio_encoder->Flush(audio_packets);
            for (auto& packet : audio_packets)
                if (!writer.Push({packet.pts_ns, 2, true, std::move(packet.bytes), {}})) {
                    error = writer.error();
                    return false;
                }
            audio_encoder->Shutdown();
        }
        if (!writer.Finalize()) {
            error = writer.error();
            return false;
        }
        stats.audio_frames = audio_position;
        stats.peak_decoders = reader.PeakDecoders();
        stats.compositor_texture_creations = compositor.TextureCreations();
        return progress(1);
    };
    const bool success = run();
    stats.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (measurements)
        *measurements = stats;
    if (!success) {
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        return RemuxResult::Fail(-1, error.empty() ? "Export cancelled." : error);
    }
    return RemuxResult::Ok();
}

} // namespace exosnap::engine

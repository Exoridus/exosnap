#include "audio_runtime_graph.h"
#include "audio_tee.h"
#include "logical_audio_source.h"
#include "qpc_100ns.h"
#include "shared_audio_capture.h"
#include "track_audio_mixer.h"
#include "wasapi_capture_src.h"
#include "wasapi_loopback.h"
#include "wasapi_loopback_src.h"
#include "wasapi_process_loopback_src.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <exosnap/engine/recording_media_types.h>
#include <exosnap/engine/session_stats.h>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace exosnap::engine {
namespace {
uint64_t NowNs() {
    LARGE_INTEGER ticks{}, frequency{};
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return QpcTicksTo100ns(static_cast<uint64_t>(ticks.QuadPart), static_cast<uint64_t>(frequency.QuadPart)) * 100;
}
SharedAudioCaptureRequest NativeRequest(const ResolvedAudioSource& source) {
    SharedAudioCaptureRequest request;
    request.key = *source.capture_key;
    request.admission.sample_rate = 48000;
    request.admission.channels = 2;
    request.admission.maximum_packet_frames = 9600;
    const auto key = request.key;
    if (key.kind == AudioBindingKind::MicrophoneEndpoint) {
        request.describe_native = [key](AudioCaptureDescriptor& descriptor, std::string& error) {
            return DescribeNativeMicrophoneFormat(key.endpoint_id, descriptor, error);
        };
        request.native_factory = [key] {
            return std::make_unique<WasapiCaptureSrc>(MicChannelMode::PreserveStereo, key.endpoint_id, true);
        };
    } else if (key.kind == AudioBindingKind::PlaybackEndpoint) {
        request.describe_native = [key](AudioCaptureDescriptor& descriptor, std::string& error) {
            WasapiNativeFormat format;
            if (!DescribeNativePlaybackFormat(key.endpoint_id, format, error))
                return false;
            descriptor.sample_rate = format.sample_rate;
            descriptor.channels = format.channels;
            descriptor.maximum_packet_frames = format.sample_rate / 5 + 256;
            return true;
        };
        request.native_factory = [key] { return std::make_unique<WasapiLoopbackSrc>(key.endpoint_id, true); };
    } else {
        request.native_factory = [key] {
            return std::make_unique<WasapiProcessLoopbackSrc>(key.process->process_id,
                                                              key.kind == AudioBindingKind::LegacyExcludeTarget
                                                                  ? ProcessLoopbackMode::ExcludeProcessTree
                                                                  : ProcessLoopbackMode::IncludeProcessTree,
                                                              key.process->creation_time);
        };
    }
    return request;
}
} // namespace

AudioRuntimeGraph::AudioRuntimeGraph() : drained_event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!drained_event_)
        throw std::runtime_error("Cannot create audio graph drain event");
}
AudioRuntimeGraph::~AudioRuntimeGraph() {
    RequestStop(NowNs());
    CloseHandle(static_cast<HANDLE>(drained_event_));
}
std::shared_ptr<AudioRuntimeGraph> AudioRuntimeGraph::Prepare(const ResolvedAudioPlan& plan,
                                                              std::shared_ptr<SharedAudioCaptureRegistry> registry,
                                                              uint64_t start, uint32_t rate, uint32_t channels,
                                                              bool correction, std::string& error,
                                                              RequestFactory factory) {
    error.clear();
    if (!registry || plan.tracks.empty() || plan.tracks.size() > kMaxAudioTracks ||
        plan.sources.size() > kMaxAudioSources || !rate || !channels) {
        error = "Resolved audio graph exceeds its admitted topology or format";
        return {};
    }
    std::map<AudioSourceId, const ResolvedAudioSource*> sources;
    for (const auto& source : plan.sources) {
        if (source.spec.id == AudioSourceId{} || !sources.emplace(source.spec.id, &source).second) {
            error = "Resolved audio graph has an invalid or duplicate source identity";
            return {};
        }
    }
    std::set<AudioTrackId> track_ids;
    std::set<AudioRouteId> route_ids;
    std::set<AudioSourceId> used;
    for (size_t i = 0; i < plan.tracks.size(); ++i) {
        const auto& track = plan.tracks[i];
        if (track.id == AudioTrackId{} || !track_ids.insert(track.id).second || track.mux_index.value != i ||
            track.routes.empty() || track.fixed_average_divisor != track.routes.size()) {
            error = "Resolved audio tracks require unique identities and dense stream indices";
            return {};
        }
        std::set<AudioSourceId> track_sources;
        for (const auto& route : track.routes) {
            if (!sources.contains(route.source_id) || route.id == AudioRouteId{} ||
                !route_ids.insert(route.id).second || !track_sources.insert(route.source_id).second ||
                !std::isfinite(route.gain_linear) || route.gain_linear < 0) {
                error = "Resolved route identity, source or gain is invalid";
                return {};
            }
            used.insert(route.source_id);
        }
    }
    if (route_ids.size() > kMaxAudioRoutes) {
        error = "Resolved audio route budget exceeded";
        return {};
    }
    auto graph = std::shared_ptr<AudioRuntimeGraph>(new AudioRuntimeGraph);
    graph->registry_ = std::move(registry);
    std::vector<SharedAudioCaptureRequest> requests;
    std::vector<AudioSourceId> captured_ids;
    for (const auto& id : used) {
        const auto& source = *sources.at(id);
        if (!source.spec.participation || source.silence_for_this_start)
            continue;
        if (source.availability != AudioSourceAvailability::Available || !source.capture_key) {
            error = "Resolved audio source is unavailable without explicit silent-start admission";
            return {};
        }
        const auto& key = *source.capture_key;
        if (key.kind != AudioBindingKind::MicrophoneEndpoint && key.kind != AudioBindingKind::PlaybackEndpoint &&
            (!key.process || !key.process->process_id || !key.process->creation_time)) {
            error = "Process audio requires a verified process instance";
            return {};
        }
        auto request = factory ? factory(source) : NativeRequest(source);
        request.correction_enabled = correction;
        requests.push_back(std::move(request));
        captured_ids.push_back(id);
    }
    auto admit = [&](std::span<const AudioCaptureDescriptor> physical_descriptors, std::string& admission_error) {
        if (physical_descriptors.size() != captured_ids.size()) {
            admission_error = "Physical audio admission has an invalid descriptor census";
            return false;
        }
        std::map<AudioSourceId, LogicalAudioSource*> logical;
        for (size_t i = 0; i < physical_descriptors.size(); ++i) {
            const auto& descriptor = physical_descriptors[i];
            const auto& source = *sources.at(captured_ids[i]);
            auto reservation = graph->registry_->ReservePayload(
                LogicalAudioSource::RequiredPayloadBytes(source, descriptor), admission_error);
            if (!reservation)
                return false;
            auto processor = LogicalAudioSource::Create(source, descriptor, admission_error);
            if (!processor)
                return false;
            logical[captured_ids[i]] = processor.get();
            graph->nodes_.push_back({nullptr, std::move(reservation), std::move(processor)});
        }
        for (const auto& track : plan.tracks) {
            std::vector<std::unique_ptr<IAudioCaptureSource>> inputs;
            std::vector<AudioCaptureDescriptor> descriptors;
            for (const auto& route : track.routes) {
                const auto found = logical.find(route.source_id);
                if (found == logical.end()) {
                    inputs.push_back(nullptr);
                    AudioCaptureDescriptor silent;
                    silent.sample_rate = rate;
                    silent.channels = channels;
                    silent.maximum_packet_frames = rate / 100;
                    descriptors.push_back(silent);
                    continue;
                }
                auto attached = found->second->AttachReaders(1, start, admission_error);
                if (attached.size() != 1)
                    return false;
                descriptors.push_back(found->second->Descriptor());
                inputs.push_back(MakeLogicalAudioReader(std::move(attached.front())));
            }
            auto reservation = graph->registry_->ReservePayload(
                TrackAudioMixer::RequiredPayloadBytes(rate, channels, descriptors), admission_error);
            if (!reservation)
                return false;
            auto mixer = std::make_unique<TrackAudioMixer>(track, std::move(inputs), rate, channels,
                                                           std::function<uint64_t()>{}, start);
            graph->track_telemetry_.push_back(mixer->Telemetry());
            graph->tracks_.push_back({std::move(reservation), std::move(mixer)});
        }
        return true;
    };
    auto readers = graph->registry_->AttachReaders(requests, start, error, admit);
    if (readers.size() != requests.size() || graph->tracks_.size() != plan.tracks.size())
        return {};
    for (size_t i = 0; i < readers.size(); ++i)
        graph->nodes_[i].physical = std::move(readers[i]);
    return graph;
}
std::vector<std::unique_ptr<IAudioCaptureSource>> AudioRuntimeGraph::TakeTracks() {
    std::vector<std::unique_ptr<IAudioCaptureSource>> result;
    for (auto& track : tracks_)
        result.push_back(std::make_unique<AudioRuntimeTrackSource>(shared_from_this(), std::move(track.reservation),
                                                                   std::move(track.mixer)));
    tracks_.clear();
    return result;
}
void AudioRuntimeGraph::Start(std::function<void(std::string)> on_error) {
    if (started_.exchange(true))
        return;
    on_error_ = std::move(on_error);
    std::thread([self = shared_from_this()] { self->Run(); }).detach();
}
void AudioRuntimeGraph::RequestStop(uint64_t end) noexcept {
    uint64_t unset = 0;
    end_qpc_ns_.compare_exchange_strong(unset, end ? end : NowNs());
}
bool AudioRuntimeGraph::WaitDrained(uint32_t timeout, std::string& error) {
    if (WaitForSingleObject(static_cast<HANDLE>(drained_event_), timeout) != WAIT_OBJECT_0) {
        error = "Audio graph drain is stalled";
        return false;
    }
    std::lock_guard lock(error_mutex_);
    error = error_;
    return error.empty();
}
void AudioRuntimeGraph::Run() {
    auto fail = [this](std::string error) { Fail(std::move(error)); };
    bool sealed = false;
    try {
        for (;;) {
            if (!sealed && end_qpc_ns_.load()) {
                std::vector<IAudioCaptureSource*> readers;
                for (auto& node : nodes_)
                    readers.push_back(node.physical.get());
                for (const auto& end : BatchSealAudioReadersEnd(readers, end_qpc_ns_.load())) {
                    if (end.stalled)
                        fail("Physical audio reader could not seal its finite endpoint");
                }
                sealed = true;
            }
            bool work = false, all_ended = sealed;
            for (auto& node : nodes_) {
                for (uint32_t count = 0; count < 256 && node.physical->PendingFrameCount(); ++count) {
                    RawAudioBuffer raw;
                    AudioBlockView block;
                    std::string error;
                    if (!node.physical->AcquireBuffer(raw, error)) {
                        if (!error.empty())
                            fail(error);
                        break;
                    }
                    const bool processed =
                        LastSharedAudioBlock(*node.physical, block) && node.logical->Process(block, error);
                    node.physical->ReleaseBuffer();
                    work = true;
                    if (!processed) {
                        fail(error.empty() ? "Shared audio block is unavailable" : error);
                        break;
                    }
                }
                if (sealed && !AudioReaderEnded(*node.physical))
                    all_ended = false;
            }
            if (all_ended)
                break;
            if (!work)
                Sleep(1);
        }
        for (auto& node : nodes_) {
            std::string error;
            if (!node.logical->Drain(error))
                fail(error);
            node.logical->Stop();
            node.physical->Shutdown();
        }
    } catch (const std::exception& exception) {
        fail(exception.what());
        for (auto& node : nodes_)
            node.logical->Stop();
    }
    SetEvent(static_cast<HANDLE>(drained_event_));
}
void AudioRuntimeGraph::Fail(std::string error) {
    bool first = false;
    {
        std::lock_guard lock(error_mutex_);
        if (error_.empty()) {
            error_ = error;
            first = true;
        }
    }
    RequestStop(NowNs());
    if (first && on_error_)
        on_error_(std::move(error));
}
bool AudioRuntimeGraph::SetSourceMuted(const AudioSourceId& id, bool muted) {
    for (auto& node : nodes_)
        if (node.logical->Meter().source_id == id) {
            node.logical->SetMuted(muted);
            return true;
        }
    return false;
}
std::vector<LogicalAudioMeter> AudioRuntimeGraph::SourceMeters() const {
    std::vector<LogicalAudioMeter> result;
    for (const auto& node : nodes_)
        result.push_back(node.logical->Meter());
    return result;
}
std::vector<TrackMeterSnapshot> AudioRuntimeGraph::TrackMeters() const {
    std::vector<TrackMeterSnapshot> result;
    for (const auto& telemetry : track_telemetry_) {
        std::lock_guard lock(telemetry->mutex);
        result.push_back(telemetry->snapshot);
    }
    return result;
}
std::vector<PhysicalMeterSnapshot> AudioRuntimeGraph::PhysicalMeters() const {
    return registry_->MeterSnapshots();
}
AudioRuntimeTrackSource::AudioRuntimeTrackSource(std::shared_ptr<AudioRuntimeGraph> graph,
                                                 std::shared_ptr<void> reservation,
                                                 std::unique_ptr<TrackAudioMixer> mixer)
    : graph_(std::move(graph)), reservation_(std::move(reservation)), mixer_(std::move(mixer)) {
}
AudioRuntimeTrackSource::~AudioRuntimeTrackSource() = default;
bool AudioRuntimeTrackSource::Init(std::string& error) {
    return mixer_->Init(error);
}
bool AudioRuntimeTrackSource::Reinit(std::string& error) {
    return mixer_->Reinit(error);
}
uint32_t AudioRuntimeTrackSource::PendingFrameCount() {
    const auto count = mixer_->PendingFrameCount();
    if (const auto error = mixer_->Failure(); !error.empty())
        graph_->Fail(error);
    return count;
}
bool AudioRuntimeTrackSource::AcquireBuffer(RawAudioBuffer& buffer, std::string& error) {
    return mixer_->AcquireBuffer(buffer, error);
}
void AudioRuntimeTrackSource::ReleaseBuffer() {
    mixer_->ReleaseBuffer();
}
uint32_t AudioRuntimeTrackSource::SampleRate() const {
    return mixer_->SampleRate();
}
uint32_t AudioRuntimeTrackSource::Channels() const {
    return mixer_->Channels();
}
AudioSampleFormat AudioRuntimeTrackSource::SampleFormat() const {
    return AudioSampleFormat::Float32;
}
const std::string& AudioRuntimeTrackSource::EndpointName() const {
    return mixer_->EndpointName();
}
bool AudioRuntimeTrackSource::ClockCorrected() const {
    return true;
}
uint64_t AudioRuntimeTrackSource::LastBufferQpcNs() const {
    return mixer_->LastBufferQpcNs();
}
uint32_t AudioRuntimeTrackSource::CaptureSourceCount() const {
    return mixer_->CaptureSourceCount();
}
uint32_t AudioRuntimeTrackSource::DegradedSourceCount() const {
    return mixer_->DegradedSourceCount();
}
bool AudioRuntimeTrackSource::BeginDrain(uint64_t end, std::string& error) {
    graph_->RequestStop(end);
    return graph_->WaitDrained(1500, error) && mixer_->BeginDrain(graph_->end_qpc_ns_.load(), error);
}
void AudioRuntimeTrackSource::Shutdown() {
    mixer_->Shutdown();
}
} // namespace exosnap::engine

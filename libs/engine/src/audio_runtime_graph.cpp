#include "audio_runtime_graph.h"
#include "audio_tee.h"
#include "logical_audio_source.h"
#include "shared_audio_capture.h"
#include "track_audio_mixer.h"
#include "wasapi_capture_src.h"
#include "wasapi_loopback.h"
#include "wasapi_loopback_src.h"
#include "wasapi_process_loopback_src.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <exosnap/engine/audio_input_device.h>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/audio_track_model.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <exosnap/engine/logging/logging.h>
#include <exosnap/engine/qpc_100ns.h>
#include <exosnap/engine/recording_media_types.h>
#include <exosnap/engine/session_stats.h>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace exosnap::engine {
namespace {
constexpr uint64_t kDefaultRetryMinNs = 250000000;
constexpr uint64_t kDefaultRetryMaxNs = 2000000000;
std::atomic<uint32_t> active_default_reopens{0};

struct DefaultReopenSlot {
    ~DefaultReopenSlot() {
        active_default_reopens.fetch_sub(1, std::memory_order_relaxed);
    }
};

uint64_t NowNs() {
    LARGE_INTEGER ticks{}, frequency{};
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    return QpcTicksTo100ns(static_cast<uint64_t>(ticks.QuadPart), static_cast<uint64_t>(frequency.QuadPart)) * 100;
}
std::string CurrentDefaultEndpoint(AudioBindingKind kind, AudioEndpointRole role) {
    const auto role_index = static_cast<size_t>(role);
    const auto endpoints =
        kind == AudioBindingKind::MicrophoneEndpoint ? EnumerateAudioInputDevices() : EnumerateAudioOutputDevices();
    for (const auto& endpoint : endpoints) {
        if (role_index < endpoint.is_default_by_role.size() && endpoint.is_default_by_role[role_index])
            return endpoint.device_id;
    }
    return {};
}
bool FitsRecoveryAdmission(const ResolvedAudioSource& source, const AudioCaptureDescriptor& admission,
                           const AudioCaptureDescriptor& incoming, uint32_t rate, uint32_t channels) {
    if (incoming.sample_rate > admission.sample_rate || incoming.channels > admission.channels ||
        incoming.maximum_packet_frames > admission.maximum_packet_frames ||
        incoming.sample_format != admission.sample_format || incoming.clock_corrected != admission.clock_corrected)
        return false;
    const auto old_output = LogicalAudioSource::ProjectOutputDescriptor(source, admission);
    const auto new_output = LogicalAudioSource::ProjectOutputDescriptor(source, incoming);
    return TrackAudioMixer::RequiredPayloadBytes(rate, channels, std::span(&new_output, 1)) <=
           TrackAudioMixer::RequiredPayloadBytes(rate, channels, std::span(&old_output, 1));
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
                                                              RequestFactory factory, DefaultEndpointLookup lookup) {
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
    if (!factory) {
        factory = NativeRequest;
        if (!lookup)
            lookup = CurrentDefaultEndpoint;
    }
    graph->lookup_default_ = std::move(lookup);
    graph->request_factory_ = std::move(factory);
    graph->output_rate_ = rate;
    graph->output_channels_ = channels;
    graph->correction_enabled_ = correction;
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
        auto request = graph->request_factory_(source);
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
        std::map<AudioSourceId, std::shared_ptr<std::atomic<uint64_t>>> completion;
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
            auto frontier = std::make_shared<std::atomic<uint64_t>>(start);
            completion[captured_ids[i]] = frontier;
            Node node{nullptr, std::move(reservation), std::move(processor), std::move(frontier), std::nullopt};
            const auto kind = source.spec.binding.kind;
            if (graph->lookup_default_ && source.spec.binding.follow_default &&
                (kind == AudioBindingKind::PlaybackEndpoint || kind == AudioBindingKind::MicrophoneEndpoint)) {
                node.default_recovery.emplace();
                node.default_recovery->source = source;
                node.default_recovery->admission = descriptor;
            }
            graph->nodes_.push_back(std::move(node));
        }
        for (const auto& track : plan.tracks) {
            std::vector<std::unique_ptr<IAudioCaptureSource>> inputs;
            std::vector<AudioCaptureDescriptor> descriptors;
            std::vector<std::shared_ptr<std::atomic<uint64_t>>> frontiers;
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
                frontiers.push_back(completion.at(route.source_id));
                inputs.push_back(MakeLogicalAudioReader(std::move(attached.front())));
            }
            auto reservation = graph->registry_->ReservePayload(
                TrackAudioMixer::RequiredPayloadBytes(rate, channels, descriptors), admission_error);
            if (!reservation)
                return false;
            auto mixer = std::make_unique<TrackAudioMixer>(
                track, std::move(inputs), rate, channels, std::function<uint64_t()>{}, start,
                [frontiers = std::move(frontiers)] {
                    uint64_t completed = NowNs();
                    for (const auto& frontier : frontiers)
                        completed = (std::min)(completed, frontier->load(std::memory_order_acquire));
                    return completed;
                });
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
    default_recovery_stop_.request_stop();
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
void AudioRuntimeGraph::PollDefaultRecovery(Node& node, bool allow_reopen) {
    if (!node.default_recovery)
        return;
    auto& recovery = *node.default_recovery;
    const auto loss = SharedAudioReaderLossGeneration(*node.physical);
    if (loss != recovery.loss_generation && loss) {
        // Another loss must not authorize packets queued during unresolved recovery.
        if (!recovery.waiting)
            recovery.mute_after_generation = loss;
        recovery.loss_generation = loss;
        recovery.waiting = true;
        recovery.next_retry_qpc = 0;
        recovery.retry_delay_ns = kDefaultRetryMinNs;
    }
    if (!allow_reopen || !recovery.waiting)
        return;

    if (recovery.pending.valid()) {
        if (recovery.pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready ||
            node.physical->PendingFrameCount())
            return;
        auto result = recovery.pending.get();
        if (recovery.pending_generation != recovery.loss_generation)
            return;
        if (result.reader) {
            const auto cutoff =
                (std::max)(node.completed_through->load(std::memory_order_acquire), recovery.last_packet_end_qpc);
            AdvanceSharedAudioReaderStart(*result.reader, cutoff);
            const auto previous = node.logical->Descriptor();
            recovery.source_generation_offset = previous.source_generation;
            recovery.format_generation_offset = previous.format_generation;
            recovery.handoff_qpc = cutoff;
            node.physical = std::move(result.reader);
            recovery.source.capture_key->endpoint_id = std::move(result.endpoint);
            recovery.loss_generation = 0;
            recovery.reported_failure = false;
            recovery.waiting = false;
        } else if (result.endpoint == recovery.source.capture_key->endpoint_id &&
                   !node.physical->DegradedSourceCount()) {
            recovery.reported_failure = false;
            recovery.waiting = false;
        } else {
            if (!recovery.reported_failure) {
                logging::log(logging::LogLevel::Warn, "audio", "Current role-default endpoint could not be reopened");
                recovery.reported_failure = true;
            }
            recovery.next_retry_qpc = NowNs() + recovery.retry_delay_ns;
            recovery.retry_delay_ns = (std::min)(recovery.retry_delay_ns * 2, kDefaultRetryMaxNs);
        }
        return;
    }
    if (NowNs() < recovery.next_retry_qpc)
        return;
    const auto stop = default_recovery_stop_.get_token();
    if (stop.stop_requested())
        return;
    if (active_default_reopens.fetch_add(1, std::memory_order_relaxed) >= kMaxAudioSources) {
        active_default_reopens.fetch_sub(1, std::memory_order_relaxed);
        recovery.next_retry_qpc = NowNs() + kDefaultRetryMinNs;
        return;
    }

    recovery.pending_generation = recovery.loss_generation;
    try {
        std::promise<ReopenedDefault> result;
        recovery.pending = result.get_future();
        std::thread([result = std::move(result), stop, registry = registry_, lookup = lookup_default_,
                     factory = request_factory_, source = recovery.source, admission = recovery.admission,
                     rate = output_rate_, channels = output_channels_, correction = correction_enabled_]() mutable {
            const DefaultReopenSlot slot;
            ReopenedDefault reopened;
            try {
                const auto& binding = source.spec.binding;
                if (!stop.stop_requested())
                    reopened.endpoint = lookup(binding.kind, binding.endpoint_role);
                if (!stop.stop_requested() && !reopened.endpoint.empty() &&
                    reopened.endpoint != source.capture_key->endpoint_id) {
                    source.capture_key->endpoint_id = reopened.endpoint;
                    auto request = factory(source);
                    request.correction_enabled = correction;
                    std::string error;
                    auto readers = registry->AttachReaders(
                        {request}, 0, error,
                        [&](std::span<const AudioCaptureDescriptor> descriptors, std::string& detail) {
                            if (FitsRecoveryAdmission(source, admission, descriptors.front(), rate, channels))
                                return true;
                            detail = "Role-default endpoint exceeds the admitted audio format; reprepare required";
                            return false;
                        });
                    if (!readers.empty() && !stop.stop_requested()) {
                        // Native activation may block while Windows changes the default role.
                        if (lookup(binding.kind, binding.endpoint_role) == reopened.endpoint && !stop.stop_requested())
                            reopened.reader = std::move(readers.front());
                    }
                }
            } catch (const std::exception&) {
                reopened = {};
            }
            result.set_value(std::move(reopened));
        }).detach();
    } catch (const std::exception&) {
        active_default_reopens.fetch_sub(1, std::memory_order_relaxed);
        recovery.pending = {};
        recovery.next_retry_qpc = NowNs() + kDefaultRetryMinNs;
    }
}

void AudioRuntimeGraph::PrepareDefaultRouteBlock(Node& node, AudioBlockView& block) {
    if (!node.default_recovery)
        return;
    auto& recovery = *node.default_recovery;
    const auto loss = SharedAudioReaderLossGeneration(*node.physical);
    const auto mute_after = recovery.waiting ? recovery.mute_after_generation : loss;
    if (loss && (loss != recovery.loss_generation || recovery.waiting) &&
        block.descriptor.source_generation > mute_after) {
        // A shared owner can reopen its fixed endpoint before the role lookup finishes.
        block.samples = {};
        block.metadata.silence = true;
        block.metadata.synthetic_gap = true;
        block.metadata.device_timing_valid = false;
        block.metadata.native_sample_position_valid = false;
    }
    block.descriptor.source_generation += recovery.source_generation_offset;
    block.descriptor.format_generation += recovery.format_generation_offset;
    if (recovery.handoff_qpc) {
        const auto elapsed = (std::max)(block.metadata.output_qpc_ns, *recovery.handoff_qpc) - *recovery.handoff_qpc;
        const auto rate = block.descriptor.sample_rate;
        const uint64_t frames =
            elapsed / 1000000000ULL * rate + ((elapsed % 1000000000ULL) * rate + 500000000ULL) / 1000000000ULL;
        block.metadata.device_gap_frames =
            static_cast<uint32_t>((std::min)(frames, uint64_t{(std::numeric_limits<uint32_t>::max)()}));
        block.reader_overflow_frames = 0;
        block.metadata.discontinuity = true;
        recovery.handoff_qpc.reset();
    }
    recovery.last_packet_end_qpc =
        block.metadata.output_qpc_ns + DeviceFramesToNs(block.metadata.frames, block.descriptor.sample_rate);
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
                PollDefaultRecovery(node, !sealed);
                // Snapshot before draining: a newer frontier can include packets not processed yet.
                const uint64_t completed = SharedAudioReaderCompletedThrough(*node.physical);
                for (uint32_t count = 0; count < 256 && node.physical->PendingFrameCount(); ++count) {
                    RawAudioBuffer raw;
                    AudioBlockView block;
                    std::string error;
                    if (!node.physical->AcquireBuffer(raw, error)) {
                        if (!error.empty())
                            fail(error);
                        break;
                    }
                    const bool shared = LastSharedAudioBlock(*node.physical, block);
                    if (shared)
                        PrepareDefaultRouteBlock(node, block);
                    const bool processed = shared && node.logical->Process(block, error);
                    node.physical->ReleaseBuffer();
                    work = true;
                    if (!processed) {
                        fail(error.empty() ? "Shared audio block is unavailable" : error);
                        break;
                    }
                }
                if (!node.physical->PendingFrameCount()) {
                    const auto previous = node.completed_through->load(std::memory_order_relaxed);
                    node.completed_through->store((std::max)(previous, node.logical->CompletedThrough(completed)),
                                                  std::memory_order_release);
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

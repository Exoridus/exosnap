#include "shared_audio_capture.h"
#include "audio_tee.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/audio_routing_model.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <exosnap/engine/recording_media_types.h>
#include <exosnap/engine/session_stats.h>
#include <functional>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <vector>

#include "audio_clock_drift.h"
#include "clock_slaving.h"
#include "output_format_audio_src.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <objbase.h>
#include <windows.h>
#endif

namespace exosnap::engine {

namespace {
struct PhysicalProcessingTimer {
    PhysicalAudioStatistics& statistics;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    bool measured = false;
    ~PhysicalProcessingTimer() {
        if (!measured)
            return;
        statistics.processing_time_available = true;
        statistics.processing_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
};
} // namespace

struct PhysicalAudioClockSource::Impl {
    Impl(std::unique_ptr<IAudioCaptureSource> native, AudioCaptureDescriptor envelope, bool enabled)
        : conversion(std::move(native), 0, 0), admission(envelope), descriptor(envelope), correction_enabled(enabled) {
    }
    OutputFormatAudioSrc conversion;
    AudioCaptureDescriptor admission;
    AudioCaptureDescriptor descriptor;
    AudioClockDriftEstimator estimator;
    ClockSlavingController controller{5.0};
    PhysicalAudioStatistics statistics;
    AudioDeviceTiming timing;
    bool timing_valid = false;
    bool correction_enabled = true;
    bool initialized = false;
    bool observation_active = false;
    double applied_origin_ms = 0;
    uint64_t observation_origin_qpc_ns = 0;
    uint64_t output_qpc_ns = 0;
    uint64_t next_output_qpc_ns = 0;

    bool Admit(std::string& error) {
        if (conversion.SampleRate() > admission.sample_rate || conversion.Channels() > admission.channels) {
            error = "Native audio format " + std::to_string(conversion.SampleRate()) + " Hz / " +
                    std::to_string(conversion.Channels()) + " channels exceeds admitted " +
                    std::to_string(admission.sample_rate) + " Hz / " + std::to_string(admission.channels) +
                    " channels; reprepare required";
            conversion.Shutdown();
            return false;
        }
        descriptor.sample_rate = conversion.SampleRate();
        descriptor.channels = conversion.Channels();
        descriptor.sample_format = AudioSampleFormat::Float32;
        descriptor.clock_corrected = true;
        initialized = true;
        ++statistics.capture_opens;
        return true;
    }
    void ResetObservation() {
        estimator.Reset();
        controller = ClockSlavingController(5.0);
        applied_origin_ms = conversion.AppliedCompensationMs();
        observation_active = false;
        timing_valid = false;
        statistics.clock_quality = PhysicalClockQuality::Unknown;
        statistics.compensation_ppm = 0;
        conversion.SetCompensationPpm(0);
    }
};

PhysicalAudioClockSource::PhysicalAudioClockSource(std::unique_ptr<IAudioCaptureSource> native,
                                                   AudioCaptureDescriptor admission, bool correction_enabled)
    : impl_(std::make_unique<Impl>(std::move(native), admission, correction_enabled)) {
}
PhysicalAudioClockSource::~PhysicalAudioClockSource() = default;
bool PhysicalAudioClockSource::Init(std::string& error) {
    const auto& admission = impl_->admission;
    const size_t budget = AudioTee::RequiredPayloadBytes(admission);
    if (!budget || budget > kAudioTeePayloadBytes) {
        error = "Native audio payload admission exceeds per-group budget";
        return false;
    }
    if (!impl_->conversion.SetBufferSampleLimit(
            static_cast<size_t>(admission.maximum_packet_frames) * admission.channels, error) ||
        !impl_->conversion.Init(error))
        return false;
    impl_->ResetObservation();
    return impl_->Admit(error);
}
bool PhysicalAudioClockSource::Reinit(std::string& error) {
    impl_->initialized = false;
    if (!impl_->conversion.Reinit(error))
        return false;
    impl_->ResetObservation();
    if (!impl_->Admit(error))
        return false;
    ++impl_->descriptor.source_generation;
    ++impl_->descriptor.format_generation;
    ++impl_->statistics.reinit_count;
    return true;
}
uint32_t PhysicalAudioClockSource::PendingFrameCount() {
    return impl_->conversion.PendingFrameCount();
}
bool PhysicalAudioClockSource::AcquireBuffer(RawAudioBuffer& buffer, std::string& error) {
    PhysicalProcessingTimer timer{impl_->statistics};
    if (!impl_->conversion.AcquireBuffer(buffer, error))
        return false;
    timer.measured = true;
    impl_->output_qpc_ns = impl_->conversion.LastBufferQpcNs();
    impl_->next_output_qpc_ns = impl_->output_qpc_ns + DeviceFramesToNs(buffer.num_frames, SampleRate());
    ++impl_->statistics.input_blocks;
    impl_->statistics.input_frames += impl_->conversion.LastInputFrames();
    impl_->statistics.output_frames += buffer.num_frames;
    impl_->statistics.applied_compensation_ms = impl_->conversion.AppliedCompensationMs();
    AudioDeviceTiming timing;
    const bool valid = impl_->conversion.LastBufferDeviceTiming(timing) && timing.qpc_position_ns != 0;
    if (!valid || (impl_->timing_valid && (timing.qpc_position_ns <= impl_->timing.qpc_position_ns ||
                                           timing.device_position_ns < impl_->timing.device_position_ns))) {
        impl_->ResetObservation();
        impl_->statistics.clock_quality = valid ? PhysicalClockQuality::Degraded : PhysicalClockQuality::Unknown;
        return true;
    }
    impl_->timing = timing;
    impl_->timing_valid = true;
    if (!impl_->observation_active) {
        impl_->applied_origin_ms = impl_->conversion.AppliedCompensationMs();
        impl_->observation_active = true;
        impl_->observation_origin_qpc_ns = timing.qpc_position_ns;
    }
    impl_->estimator.AddObservation(timing.device_position_ns, timing.qpc_position_ns);
    const double drift = impl_->estimator.DriftMs();
    const double applied = impl_->conversion.AppliedCompensationMs() - impl_->applied_origin_ms;
    const double elapsed_s = static_cast<double>(timing.qpc_position_ns - impl_->observation_origin_qpc_ns) / 1e9;
    if (std::abs(drift) > PlausibleDriftBoundMs(elapsed_s)) {
        impl_->ResetObservation();
        impl_->statistics.clock_quality = PhysicalClockQuality::Degraded;
        return true;
    }
    impl_->statistics.drift_ms = drift;
    impl_->statistics.residual_ms = drift - applied;
    impl_->statistics.clock_quality = PhysicalClockQuality::Measured;
    if (impl_->correction_enabled && impl_->controller.Update(drift, applied, timing.qpc_position_ns)) {
        impl_->conversion.SetCompensationPpm(impl_->controller.Ppm());
        impl_->statistics.compensation_ppm = impl_->controller.Ppm();
        ++impl_->statistics.compensation_updates;
    }
    if (impl_->controller.MeasurementFaulted()) {
        impl_->ResetObservation();
        impl_->statistics.clock_quality = PhysicalClockQuality::Degraded;
    }
    return true;
}
void PhysicalAudioClockSource::ReleaseBuffer() {
    impl_->conversion.ReleaseBuffer();
}
uint32_t PhysicalAudioClockSource::SampleRate() const {
    return impl_->conversion.SampleRate();
}
uint32_t PhysicalAudioClockSource::Channels() const {
    return impl_->conversion.Channels();
}
AudioSampleFormat PhysicalAudioClockSource::SampleFormat() const {
    return AudioSampleFormat::Float32;
}
const std::string& PhysicalAudioClockSource::EndpointName() const {
    return impl_->conversion.EndpointName();
}
bool PhysicalAudioClockSource::LastBufferDeviceTiming(AudioDeviceTiming& timing) const {
    timing = impl_->timing;
    return impl_->timing_valid;
}
uint64_t PhysicalAudioClockSource::LastBufferQpcNs() const {
    return impl_->output_qpc_ns;
}
void* PhysicalAudioClockSource::BufferReadyEvent() const {
    return impl_->conversion.BufferReadyEvent();
}
int32_t PhysicalAudioClockSource::LastCaptureHresult() const {
    return impl_->conversion.LastCaptureHresult();
}
int32_t PhysicalAudioClockSource::LastInitHresult() const {
    return impl_->conversion.LastInitHresult();
}
bool PhysicalAudioClockSource::ClockCorrected() const {
    return true;
}
void PhysicalAudioClockSource::Shutdown() {
    impl_->conversion.Shutdown();
    impl_->initialized = false;
}
PhysicalAudioStatistics PhysicalAudioClockSource::Statistics() const {
    return impl_->statistics;
}
AudioCaptureDescriptor PhysicalAudioClockSource::Descriptor() const {
    auto descriptor = impl_->descriptor;
    descriptor.clock_quality = impl_->statistics.clock_quality;
    return descriptor;
}
uint64_t PhysicalAudioClockSource::BufferedFrames() const {
    return impl_->conversion.BufferedFrames();
}
uint32_t PhysicalAudioClockSource::DrainResampler(RawAudioBuffer& buffer, int64_t* undrained_frames) {
    int64_t remaining = 0;
    const auto frames = impl_->conversion.DrainResampler(buffer, &remaining);
    impl_->statistics.output_frames += frames;
    impl_->statistics.undrained_frames = static_cast<uint64_t>((std::max)(remaining, int64_t{0}));
    impl_->statistics.applied_compensation_ms = impl_->conversion.AppliedCompensationMs();
    impl_->output_qpc_ns = impl_->next_output_qpc_ns;
    impl_->next_output_qpc_ns += DeviceFramesToNs(frames, SampleRate());
    impl_->timing_valid = false;
    if (undrained_frames)
        *undrained_frames = remaining;
    return frames;
}

namespace {
using SteadyClock = std::chrono::steady_clock;

bool SameAdmission(const AudioCaptureDescriptor& left, const AudioCaptureDescriptor& right) {
    return left.sample_rate == right.sample_rate && left.channels == right.channels &&
           left.maximum_packet_frames == right.maximum_packet_frames && left.clock_id == right.clock_id &&
           left.source_generation == right.source_generation && left.format_generation == right.format_generation &&
           left.sample_format == right.sample_format;
}

struct RuntimePayloadBudget {
    explicit RuntimePayloadBudget(size_t bytes) : capacity(bytes) {
    }
    mutable std::mutex mutex;
    size_t capacity = 0;
    size_t reserved = 0;
    uint32_t owner_count = 0;
    std::atomic<uint64_t> capture_opens = 0;
    bool ReserveOwner(std::string& error) {
        std::lock_guard lock(mutex);
        if (owner_count == kMaxPhysicalAudioCaptures) {
            error = "Audio runtime physical owner limit exceeded, including retained stalled owners";
            return false;
        }
        ++owner_count;
        return true;
    }
    bool Reserve(size_t bytes, std::string& error) {
        std::lock_guard lock(mutex);
        if (!bytes || bytes > kAudioTeePayloadBytes || bytes > capacity - reserved) {
            error = "Audio runtime payload budget exceeded: reserved " + std::to_string(reserved) +
                    " bytes + requested " + std::to_string(bytes) + " bytes, budget " + std::to_string(capacity) +
                    " bytes";
            return false;
        }
        reserved += bytes;
        return true;
    }
    void Release(size_t bytes) {
        std::lock_guard lock(mutex);
        reserved -= bytes;
    }
};

struct PayloadReservation {
    PayloadReservation(std::shared_ptr<RuntimePayloadBudget> owner, size_t reserved_bytes)
        : budget(std::move(owner)), bytes(reserved_bytes) {
    }
    ~PayloadReservation() {
        budget->Release(bytes);
    }
    PayloadReservation(const PayloadReservation&) = delete;
    PayloadReservation& operator=(const PayloadReservation&) = delete;
    std::shared_ptr<RuntimePayloadBudget> budget;
    size_t bytes;
};

struct PendingAudioSeal {
    std::future<AudioReaderEnd> completion;
    std::shared_ptr<AudioTeeReader> reader;
    uint64_t end_qpc_ns = 0;

    AudioReaderEnd Wait(SteadyClock::time_point deadline) {
        if (completion.valid() && completion.wait_until(deadline) == std::future_status::ready)
            return completion.get();
        auto end = reader ? reader->SealEnd(end_qpc_ns) : AudioReaderEnd{};
        end.stalled = true;
        return end;
    }
};

class PhysicalOwner : public std::enable_shared_from_this<PhysicalOwner> {
  public:
    PhysicalOwner(SharedAudioCaptureRequest request, std::shared_ptr<RuntimePayloadBudget> budget)
        : request_(std::move(request)), configured_admission_(request_.admission), budget_(std::move(budget)) {
    }
    ~PhysicalOwner() {
        if (reserved_bytes_)
            budget_->Release(reserved_bytes_);
        if (owner_reserved_) {
            std::lock_guard lock(budget_->mutex);
            --budget_->owner_count;
        }
    }
    bool Prepare(std::string& error) {
        if (!budget_->ReserveOwner(error))
            return false;
        owner_reserved_ = true;
        auto result = prepared_.get_future();
        auto self = shared_from_this();
        std::thread([self = std::move(self)] { self->Run(); }).detach();
        if (result.wait_for(request_.preparation_timeout) != std::future_status::ready) {
            stopping_ = true;
            condition_.notify_all();
            error = "Physical audio preparation stalled; native owner resources remain retained until cleanup";
            return false;
        }
        error = result.get();
        return error.empty();
    }
    bool CommitInit(std::string& error, bool require_exact_format) {
        auto result = startup_.get_future();
        require_exact_format_ = require_exact_format;
        initialization_requested_ = true;
        condition_.notify_all();
        if (result.wait_for(request_.preparation_timeout) != std::future_status::ready) {
            stopping_ = true;
            condition_.notify_all();
            error = "Physical audio initialization stalled; native owner resources remain retained until cleanup";
            return false;
        }
        error = result.get();
        return error.empty();
    }
    void Arm() {
        armed_ = true;
        condition_.notify_all();
    }
    std::shared_ptr<AudioTee> Tee() const {
        return tee_;
    }
    AudioCaptureDescriptor PreparedDescriptor() const {
        std::lock_guard lock(mutex_);
        return prepared_descriptor_;
    }
    bool Compatible(const SharedAudioCaptureRequest& request) const {
        return SameAdmission(configured_admission_, request.admission) &&
               request_.correction_enabled == request.correction_enabled;
    }
    std::vector<std::shared_ptr<AudioTeeReader>> Attach(uint32_t count, uint64_t qpc_ns, std::string& error) {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            error = "Physical audio owner is stopping; retry after cleanup";
            return {};
        }
        return tee_->AttachReaders(count, qpc_ns, error);
    }
    void StopIfUnused() {
        std::lock_guard lock(mutex_);
        if (!ready_ || tee_->ReaderCount() == 0) {
            stopping_ = true;
            condition_.notify_all();
        }
    }
    const std::string& Name() const {
        return endpoint_name_;
    }
    PhysicalAudioStatistics Statistics() const {
        std::lock_guard lock(mutex_);
        return statistics_;
    }
    PhysicalMeterSnapshot MeterSnapshot() const {
        std::lock_guard lock(mutex_);
        PhysicalMeterSnapshot result;
        result.clock_id = prepared_descriptor_.clock_id;
        result.reader_count = tee_ ? tee_->ReaderCount() : 0;
        result.available = ready_ && !degraded_ && !stopping_;
        switch (statistics_.clock_quality) {
        case PhysicalClockQuality::Measured:
            result.clock_quality = AudioMeterClockQuality::Measured;
            break;
        case PhysicalClockQuality::Degraded:
            result.clock_quality = AudioMeterClockQuality::Degraded;
            break;
        default:
            result.clock_quality = AudioMeterClockQuality::Unknown;
            break;
        }
        result.capture_opens = statistics_.capture_opens;
        result.input_blocks = statistics_.input_blocks;
        result.input_frames = statistics_.input_frames;
        result.output_frames = statistics_.output_frames;
        result.reinit_count = statistics_.reinit_count;
        result.drift_ms = statistics_.drift_ms;
        result.residual_ms = statistics_.residual_ms;
        result.compensation_ms = statistics_.applied_compensation_ms;
        result.compensation_ppm = statistics_.compensation_ppm;
        result.processing_time_available = statistics_.processing_time_available;
        result.processing_ms = statistics_.processing_ms;
        return result;
    }
    void RequestReinit(uint64_t generation) {
        if (generation == generation_) {
            uint64_t empty = 0;
            reinit_requested_.compare_exchange_strong(empty, generation);
            condition_.notify_all();
        }
    }
    bool Degraded() const {
        return degraded_;
    }
    int32_t CaptureHresult() const {
        return capture_hresult_;
    }
    int32_t InitHresult() const {
        return init_hresult_;
    }
    std::string Error() const {
        std::lock_guard lock(mutex_);
        return error_;
    }
    PendingAudioSeal QueueSeal(std::shared_ptr<AudioTeeReader> reader, uint64_t end_qpc_ns,
                               SteadyClock::time_point deadline) {
        auto promise = std::make_shared<std::promise<AudioReaderEnd>>();
        PendingAudioSeal result{promise->get_future(), reader, end_qpc_ns};
        {
            std::lock_guard lock(mutex_);
            if (seals_.size() == kMaxAudioTeeReaders) {
                auto end = reader->SealEnd(end_qpc_ns);
                end.stalled = true;
                promise->set_value(end);
                return result;
            }
            seals_.push_back({reader, end_qpc_ns, deadline, promise});
        }
        condition_.notify_all();
        return result;
    }

  private:
    struct SealRequest {
        std::weak_ptr<AudioTeeReader> reader;
        uint64_t qpc_ns = 0;
        SteadyClock::time_point deadline;
        std::shared_ptr<std::promise<AudioReaderEnd>> promise;
    };
    SharedAudioCaptureRequest request_;
    const AudioCaptureDescriptor configured_admission_;
    std::shared_ptr<RuntimePayloadBudget> budget_;
    std::promise<std::string> prepared_;
    std::promise<std::string> startup_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::shared_ptr<AudioTee> tee_;
    std::unique_ptr<PhysicalAudioClockSource> source_;
    std::vector<SealRequest> seals_;
    PhysicalAudioStatistics statistics_;
    AudioCaptureDescriptor prepared_descriptor_;
    std::string endpoint_name_;
    std::string error_;
    std::atomic<bool> stopping_ = false;
    std::atomic<bool> armed_ = false;
    std::atomic<bool> initialization_requested_ = false;
    std::atomic<bool> require_exact_format_ = false;
    std::atomic<bool> ready_ = false;
    std::atomic<bool> degraded_ = false;
    std::atomic<uint64_t> reinit_requested_ = 0;
    std::atomic<uint64_t> generation_ = 1;
    std::atomic<int32_t> capture_hresult_ = 0;
    std::atomic<int32_t> init_hresult_ = 0;
    size_t reserved_bytes_ = 0;
    bool owner_reserved_ = false;
    uint64_t last_output_end_qpc_ = 0;
    bool next_discontinuity_ = false;

    void UpdateStatistics() {
        std::lock_guard lock(mutex_);
        statistics_ = source_->Statistics();
        prepared_descriptor_ = source_->Descriptor();
    }
    void SetError(const std::string& error) {
        std::lock_guard lock(mutex_);
        error_ = error;
    }
    void CompleteSeals(bool force = false) {
        std::lock_guard lock(mutex_);
        for (auto iterator = seals_.begin(); iterator != seals_.end();) {
            const bool timeout = SteadyClock::now() >= iterator->deadline;
            if (!force && !timeout && last_output_end_qpc_ < iterator->qpc_ns) {
                ++iterator;
                continue;
            }
            const auto reader = iterator->reader.lock();
            if (!reader) {
                iterator->promise->set_value({});
                iterator = seals_.erase(iterator);
                continue;
            }
            auto end = reader->SealEnd(iterator->qpc_ns);
            end.stalled = (timeout || force) && last_output_end_qpc_ < iterator->qpc_ns;
            if (end.stalled && source_)
                end.undrained_frames = source_->BufferedFrames();
            iterator->promise->set_value(end);
            iterator = seals_.erase(iterator);
        }
    }
    bool Publish(RawAudioBuffer buffer, std::string& error) {
        if (!buffer.num_frames)
            return true;
        AudioBlockMetadata metadata;
        metadata.frames = buffer.num_frames;
        metadata.device_gap_frames = buffer.gap_frames;
        metadata.silence = buffer.silent;
        metadata.discontinuity = buffer.data_discontinuity || next_discontinuity_;
        metadata.output_qpc_ns = source_->LastBufferQpcNs();
        metadata.device_timing_valid = source_->LastBufferDeviceTiming(metadata.device_timing);
        metadata.filter_latency_ns = DeviceFramesToNs(source_->BufferedFrames(), source_->SampleRate());
        if (metadata.device_timing_valid) {
            metadata.capture_qpc_ns = metadata.device_timing.qpc_position_ns;
            metadata.native_sample_position = metadata.device_timing.device_position_frames;
            metadata.native_sample_position_valid = metadata.device_timing.device_position_frames_valid;
        }
        const size_t samples = static_cast<size_t>(buffer.num_frames) * source_->Channels();
        const auto pcm = buffer.bytes ? std::span<const float>(reinterpret_cast<const float*>(buffer.bytes), samples)
                                      : std::span<const float>();
        const bool result = tee_->SetDescriptor(source_->Descriptor(), error) && tee_->Publish(pcm, metadata, error);
        if (result) {
            next_discontinuity_ = false;
            last_output_end_qpc_ = metadata.output_qpc_ns + DeviceFramesToNs(buffer.num_frames, source_->SampleRate());
        }
        return result;
    }
    void WaitForWork(std::chrono::milliseconds duration) {
#ifdef _WIN32
        const auto event = source_ && !degraded_ ? static_cast<HANDLE>(source_->BufferReadyEvent()) : nullptr;
        if (event) {
            WaitForSingleObject(event, static_cast<DWORD>((std::min)(duration.count(), int64_t{10})));
            return;
        }
#endif
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, duration, [this] { return stopping_ || reinit_requested_; });
    }
    void Run();
};

void PhysicalOwner::Run() {
#ifdef _WIN32
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    bool prepared_sent = false;
    bool startup_sent = false;
    try {
        std::string error;
        if (request_.describe_native && !request_.describe_native(request_.admission, error)) {
            prepared_.set_value(error.empty() ? "Native audio format description failed" : error);
            prepared_sent = true;
        } else if (stopping_) {
            prepared_.set_value("Physical audio preparation was cancelled");
            prepared_sent = true;
        } else if (!budget_->Reserve(AudioTee::RequiredPayloadBytes(request_.admission), error)) {
            prepared_.set_value(error);
            prepared_sent = true;
        } else {
            reserved_bytes_ = AudioTee::RequiredPayloadBytes(request_.admission);
            if (request_.admission.clock_id == PhysicalClockId{})
                request_.admission.clock_id = NewId<PhysicalClockId>();
            request_.admission.sample_format = AudioSampleFormat::Float32;
            request_.admission.clock_corrected = true;
            request_.admission.clock_quality = AudioClockQuality::Unknown;
            tee_ = AudioTee::Create(request_.admission, error);
            {
                std::lock_guard lock(mutex_);
                prepared_descriptor_ = request_.admission;
            }
            if (!tee_ && error.empty())
                error = "Physical audio tee preparation failed";
            prepared_.set_value(error);
            prepared_sent = true;
            if (error.empty()) {
                {
                    std::unique_lock lock(mutex_);
                    condition_.wait(lock, [this] { return stopping_ || initialization_requested_; });
                }
                if (stopping_)
                    error = "Physical audio preparation was cancelled";
            }
            if (error.empty()) {
                auto native = request_.native_factory();
                if (!native) {
                    error = "Native audio source factory returned null";
                } else if (stopping_) {
                    error = "Physical audio preparation was cancelled";
                } else {
                    source_ = std::make_unique<PhysicalAudioClockSource>(std::move(native), request_.admission,
                                                                         request_.correction_enabled);
                    if (!source_->Init(error)) {
                        if (error.empty())
                            error = "Native audio source initialization failed";
                    } else {
                        ++budget_->capture_opens;
                        const auto actual = source_->Descriptor();
                        if (require_exact_format_ && (actual.sample_rate != request_.admission.sample_rate ||
                                                      actual.channels != request_.admission.channels)) {
                            error = "Native audio format changed during preparation; reprepare required";
                        } else {
                            endpoint_name_ = source_->EndpointName();
                            generation_ = actual.source_generation;
                            if (tee_->SetDescriptor(actual, error))
                                UpdateStatistics();
                        }
                    }
                }
            }
            ready_ = error.empty();
            startup_.set_value(error);
            startup_sent = true;
            if (error.empty()) {
                {
                    std::unique_lock lock(mutex_);
                    condition_.wait(lock, [this] { return stopping_ || armed_; });
                }
                std::chrono::milliseconds backoff(250);
                auto next_retry = SteadyClock::now();
                while (!stopping_) {
                    const uint64_t requested_generation = reinit_requested_.exchange(0);
                    if (requested_generation && requested_generation == generation_ && !degraded_) {
                        degraded_ = true;
                        next_retry = SteadyClock::now();
                        backoff = std::chrono::milliseconds(250);
                    }
                    if (degraded_) {
                        if (SteadyClock::now() >= next_retry) {
                            if (source_->Reinit(error) && tee_->SetDescriptor(source_->Descriptor(), error)) {
                                ++budget_->capture_opens;
                                degraded_ = false;
                                capture_hresult_ = 0;
                                init_hresult_ = 0;
                                next_discontinuity_ = true;
                                generation_ = source_->Descriptor().source_generation;
                                backoff = std::chrono::milliseconds(250);
                                UpdateStatistics();
                                SetError({});
                            } else {
                                SetError(error);
                                init_hresult_ = source_->LastInitHresult();
                                next_retry = SteadyClock::now() + backoff;
                                backoff = (std::min)(backoff * 2, std::chrono::milliseconds(2000));
                            }
                        }
                        CompleteSeals();
                        WaitForWork(std::chrono::milliseconds(10));
                        continue;
                    }
                    const uint32_t pending = source_->PendingFrameCount();
                    if (pending) {
                        RawAudioBuffer buffer;
                        error.clear();
                        if (source_->AcquireBuffer(buffer, error)) {
                            const bool published = Publish(buffer, error);
                            source_->ReleaseBuffer();
                            UpdateStatistics();
                            if (!published) {
                                SetError(error);
                                degraded_ = true;
                                next_retry = SteadyClock::now() + std::chrono::milliseconds(250);
                                backoff = std::chrono::milliseconds(500);
                            }
                        } else if (!error.empty()) {
                            SetError(error);
                            capture_hresult_ = source_->LastCaptureHresult();
                            degraded_ = true;
                            next_retry = SteadyClock::now() + std::chrono::milliseconds(250);
                            backoff = std::chrono::milliseconds(500);
                        }
                    }
                    CompleteSeals();
                    if (!pending)
                        WaitForWork(std::chrono::milliseconds(10));
                }
            }
        }
    } catch (const std::exception& exception) {
        degraded_ = true;
        if (!prepared_sent)
            prepared_.set_value(exception.what());
        if (!startup_sent)
            startup_.set_value(exception.what());
    }
    if (tee_) {
        if (source_ && ready_ && !degraded_) {
            RawAudioBuffer tail;
            std::string error;
            source_->DrainResampler(tail);
            if (tail.num_frames)
                Publish(tail, error);
            UpdateStatistics();
        }
        CompleteSeals(true);
        tee_->Stop();
    }
    if (source_) {
        source_->Shutdown();
        source_.reset();
    }
#ifdef _WIN32
    if (SUCCEEDED(com_result))
        CoUninitialize();
#endif
}

class SharedAudioReader final : public IAudioCaptureSource {
  public:
    SharedAudioReader(std::shared_ptr<PhysicalOwner> owner, std::shared_ptr<AudioTeeReader> reader)
        : owner_(std::move(owner)), reader_(std::move(reader)), endpoint_name_(owner_->Name()),
          observed_generation_(reader_->Descriptor().source_generation) {
    }
    ~SharedAudioReader() override {
        Shutdown();
    }
    bool Init(std::string& error) override {
        error.clear();
        return reader_ != nullptr;
    }
    bool Reinit(std::string& error) override {
        error.clear();
        if (!owner_)
            return false;
        owner_->RequestReinit(observed_generation_);
        return true;
    }
    uint32_t PendingFrameCount() override {
        return reader_ ? reader_->PendingFrames() : 0;
    }
    bool AcquireBuffer(RawAudioBuffer& buffer, std::string& error) override {
        buffer = {};
        if (!reader_ || !reader_->Acquire(block_, error))
            return false;
        observed_generation_ = block_.descriptor.source_generation;
        buffer.bytes = reinterpret_cast<const uint8_t*>(block_.samples.data());
        buffer.num_frames = block_.metadata.frames;
        buffer.silent = block_.metadata.silence;
        buffer.data_discontinuity = block_.metadata.discontinuity;
        buffer.gap_frames = static_cast<uint32_t>(
            (std::min)(static_cast<uint64_t>(block_.metadata.device_gap_frames) + block_.reader_overflow_frames,
                       static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())));
        return true;
    }
    void ReleaseBuffer() override {
        if (reader_)
            reader_->Release();
        block_ = {};
    }
    uint32_t SampleRate() const override {
        return reader_ ? reader_->Descriptor().sample_rate : 0;
    }
    uint32_t Channels() const override {
        return reader_ ? reader_->Descriptor().channels : 0;
    }
    AudioSampleFormat SampleFormat() const override {
        return AudioSampleFormat::Float32;
    }
    const std::string& EndpointName() const override {
        return endpoint_name_;
    }
    bool LastBufferDeviceTiming(AudioDeviceTiming& timing) const override {
        timing = block_.metadata.device_timing;
        return block_.metadata.device_timing_valid;
    }
    uint64_t LastBufferQpcNs() const override {
        return block_.metadata.output_qpc_ns;
    }
    void* BufferReadyEvent() const override {
        return reader_ ? reader_->ReadyEvent() : nullptr;
    }
    int32_t LastCaptureHresult() const override {
        return owner_ ? owner_->CaptureHresult() : 0;
    }
    int32_t LastInitHresult() const override {
        return owner_ ? owner_->InitHresult() : 0;
    }
    uint32_t DegradedSourceCount() const override {
        return owner_ && owner_->Degraded() ? 1 : 0;
    }
    uint32_t DegradedSourceIndexMask() const override {
        return DegradedSourceCount();
    }
    bool ClockCorrected() const override {
        return true;
    }
    void Shutdown() override {
        ReleaseBuffer();
        reader_.reset();
        if (owner_)
            owner_->StopIfUnused();
        owner_.reset();
    }
    AudioReaderEnd Seal(uint64_t qpc_ns) {
        const auto deadline = SteadyClock::now() + std::chrono::milliseconds(500);
        return owner_ && reader_ ? owner_->QueueSeal(reader_, qpc_ns, deadline).Wait(deadline) : AudioReaderEnd{};
    }
    PendingAudioSeal QueueSeal(uint64_t qpc_ns, SteadyClock::time_point deadline) {
        return owner_ && reader_ ? owner_->QueueSeal(reader_, qpc_ns, deadline) : PendingAudioSeal{};
    }
    bool Ended() const {
        return !reader_ || reader_->Ended();
    }
    bool LastBlock(AudioBlockView& block) const {
        block = block_;
        return !block_.samples.empty();
    }
    std::optional<AudioCaptureDescriptor> Descriptor() const {
        return reader_ ? std::optional<AudioCaptureDescriptor>{reader_->Descriptor()} : std::nullopt;
    }
    std::string Error() const {
        return owner_ ? owner_->Error() : std::string{};
    }

  private:
    std::shared_ptr<PhysicalOwner> owner_;
    std::shared_ptr<AudioTeeReader> reader_;
    AudioBlockView block_;
    std::string endpoint_name_;
    uint64_t observed_generation_ = 1;
};
} // namespace

struct SharedAudioCaptureRegistry::Impl {
    explicit Impl(size_t bytes)
        : budget(std::make_shared<RuntimePayloadBudget>((std::min)(bytes, kAudioRuntimePayloadBytes))) {
    }
    mutable std::mutex mutex;
    std::shared_ptr<RuntimePayloadBudget> budget;
    std::map<PhysicalCaptureKey, std::weak_ptr<PhysicalOwner>> owners;
};

SharedAudioCaptureRegistry::SharedAudioCaptureRegistry(size_t payload_budget)
    : impl_(std::make_unique<Impl>(payload_budget)) {
}
SharedAudioCaptureRegistry::~SharedAudioCaptureRegistry() = default;
std::vector<std::unique_ptr<IAudioCaptureSource>> SharedAudioCaptureRegistry::AttachReaders(
    const std::vector<SharedAudioCaptureRequest>& requests, uint64_t start_qpc_ns, std::string& error,
    std::function<bool(std::span<const AudioCaptureDescriptor>, std::string&)> admit) {
    error.clear();
    std::lock_guard lock(impl_->mutex);
    struct Group {
        SharedAudioCaptureRequest request;
        std::shared_ptr<PhysicalOwner> owner;
        std::vector<size_t> positions;
        bool needs_init = false;
        ~Group() {
            if (owner)
                owner->StopIfUnused();
        }
    };
    std::map<PhysicalCaptureKey, Group> groups;
    std::vector<std::unique_ptr<IAudioCaptureSource>> result(requests.size());
    std::vector<AudioCaptureDescriptor> descriptors(requests.size());
    for (auto iterator = impl_->owners.begin(); iterator != impl_->owners.end();) {
        if (iterator->second.expired())
            iterator = impl_->owners.erase(iterator);
        else
            ++iterator;
    }
    size_t new_owners = 0;
    for (size_t index = 0; index < requests.size(); ++index) {
        auto key = requests[index].key;
        if (key.kind == AudioBindingKind::CaptureTargetProcess)
            key.kind = AudioBindingKind::IndependentProcess;
        if (key.process)
            key.process->identity_hint.clear();
        auto [iterator, inserted] = groups.try_emplace(key);
        if (inserted)
            iterator->second.request = requests[index];
        else if (!SameAdmission(iterator->second.request.admission, requests[index].admission) ||
                 iterator->second.request.correction_enabled != requests[index].correction_enabled) {
            error = "Conflicting admission or clock correction policy for one physical audio capture";
            return {};
        }
        iterator->second.positions.push_back(index);
    }
    for (auto& [key, group] : groups) {
        const auto existing = impl_->owners.find(key);
        group.owner = existing == impl_->owners.end() ? nullptr : existing->second.lock();
        if (group.owner && !group.owner->Compatible(group.request)) {
            error = "Requested admission or clock correction policy conflicts with the existing physical audio owner";
            return {};
        }
        if (!group.owner) {
            if (impl_->owners.size() + new_owners >= kMaxPhysicalAudioCaptures) {
                error = "Audio runtime physical capture limit exceeded";
                return {};
            }
            ++new_owners;
            if (!group.request.native_factory) {
                error = "Native audio source factory is missing";
                return {};
            }
            group.owner = std::make_shared<PhysicalOwner>(group.request, impl_->budget);
            group.needs_init = true;
            if (!group.owner->Prepare(error))
                return {};
        }
        const auto descriptor = group.owner->PreparedDescriptor();
        for (const auto index : group.positions)
            descriptors[index] = descriptor;
    }
    if (admit) {
        try {
            if (!admit(descriptors, error)) {
                if (error.empty())
                    error = "Audio graph payload admission failed";
                return {};
            }
        } catch (const std::exception& exception) {
            error = exception.what();
            return {};
        }
        error.clear();
    }
    for (auto& [key, group] : groups) {
        (void)key;
        if (group.needs_init && !group.owner->CommitInit(error, static_cast<bool>(admit)))
            return {};
    }
    for (auto& [key, group] : groups) {
        (void)key;
        auto readers = group.owner->Attach(static_cast<uint32_t>(group.positions.size()), start_qpc_ns, error);
        if (readers.size() != group.positions.size())
            return {};
        for (size_t index = 0; index < readers.size(); ++index)
            result[group.positions[index]] = std::make_unique<SharedAudioReader>(group.owner, readers[index]);
    }
    for (auto& [key, group] : groups) {
        impl_->owners[key] = group.owner;
        group.owner->Arm();
    }
    return result;
}
std::shared_ptr<void> SharedAudioCaptureRegistry::ReservePayload(size_t bytes, std::string& error) {
    error.clear();
    auto budget = impl_->budget;
    if (!budget->Reserve(bytes, error))
        return {};
    try {
        return std::make_shared<PayloadReservation>(budget, bytes);
    } catch (const std::bad_alloc&) {
        budget->Release(bytes);
        error = "Audio payload reservation metadata allocation failed";
        return {};
    }
}
size_t SharedAudioCaptureRegistry::ReservedPayloadBytes() const {
    std::lock_guard lock(impl_->budget->mutex);
    return impl_->budget->reserved;
}
PhysicalAudioStatistics SharedAudioCaptureRegistry::Statistics() const {
    std::lock_guard lock(impl_->mutex);
    PhysicalAudioStatistics result;
    result.capture_opens = impl_->budget->capture_opens;
    for (const auto& [key, weak] : impl_->owners) {
        (void)key;
        if (const auto owner = weak.lock()) {
            const auto statistics = owner->Statistics();
            result.input_blocks += statistics.input_blocks;
            result.input_frames += statistics.input_frames;
            result.output_frames += statistics.output_frames;
            result.compensation_updates += statistics.compensation_updates;
        }
    }
    return result;
}
std::vector<PhysicalMeterSnapshot> SharedAudioCaptureRegistry::MeterSnapshots() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<PhysicalMeterSnapshot> result;
    result.reserve(impl_->owners.size());
    for (const auto& [key, weak] : impl_->owners) {
        (void)key;
        if (const auto owner = weak.lock())
            result.push_back(owner->MeterSnapshot());
    }
    return result;
}
AudioReaderEnd SealAudioReaderEnd(IAudioCaptureSource& reader, uint64_t end_qpc_ns) {
    const auto adapter = dynamic_cast<SharedAudioReader*>(&reader);
    return adapter ? adapter->Seal(end_qpc_ns) : AudioReaderEnd{};
}
std::vector<AudioReaderEnd> BatchSealAudioReadersEnd(std::span<IAudioCaptureSource* const> readers, uint64_t end_qpc_ns,
                                                     std::chrono::milliseconds timeout) {
    const auto deadline = SteadyClock::now() + (std::max)(timeout, std::chrono::milliseconds(0));
    std::vector<PendingAudioSeal> pending;
    pending.reserve(readers.size());
    for (auto* reader : readers) {
        auto* adapter = dynamic_cast<SharedAudioReader*>(reader);
        pending.push_back(adapter ? adapter->QueueSeal(end_qpc_ns, deadline) : PendingAudioSeal{});
    }
    std::vector<AudioReaderEnd> result;
    result.reserve(pending.size());
    for (auto& seal : pending)
        result.push_back(seal.Wait(deadline));
    return result;
}
bool AudioReaderEnded(const IAudioCaptureSource& reader) {
    const auto adapter = dynamic_cast<const SharedAudioReader*>(&reader);
    return adapter && adapter->Ended();
}
bool LastSharedAudioBlock(const IAudioCaptureSource& reader, AudioBlockView& block) {
    const auto adapter = dynamic_cast<const SharedAudioReader*>(&reader);
    block = {};
    return adapter && adapter->LastBlock(block);
}
std::string SharedAudioCaptureError(const IAudioCaptureSource& reader) {
    const auto adapter = dynamic_cast<const SharedAudioReader*>(&reader);
    return adapter ? adapter->Error() : std::string{};
}
std::optional<AudioCaptureDescriptor> SharedAudioReaderDescriptor(const IAudioCaptureSource& reader) {
    const auto* adapter = dynamic_cast<const SharedAudioReader*>(&reader);
    return adapter ? adapter->Descriptor() : std::nullopt;
}
} // namespace exosnap::engine

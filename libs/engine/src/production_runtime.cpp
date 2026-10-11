#include "production_runtime.h"
#include "file_output.h"
#include "qpc_100ns.h"
#include "session_internal.h"
#include "video_thread.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/recorder_session.h>
#include <exosnap/engine/recording_media_types.h>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace exosnap::engine {
namespace {
EngineError RuntimeError(EngineErrorCode code, const char* context) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::Runtime;
    error.context = context;
    return error;
}
bool Compatible(const RecorderConfig& left, const RecorderConfig& right) {
    return left.target.kind == right.target.kind && left.target.native_id == right.target.native_id &&
           ResolveCaptureBackend(left) == ResolveCaptureBackend(right) && left.hdr_mode == right.hdr_mode;
}
} // namespace

struct RuntimeFrameLease::Slot {
    enum class Phase { Free, Reserved, GPUWriting, Published };
    Phase phase = Phase::Free;
    std::shared_ptr<void> texture;
    RuntimeCompletionToken completion;
    std::atomic<bool> gpu_complete{true};
    uint64_t sequence = 0;
    RuntimeQpc100ns qpc;
    RuntimeFrameGeometry geometry;
};

RuntimeFrameLease::RuntimeFrameLease(std::shared_ptr<const Slot> slot) : slot_(std::move(slot)) {
}
void* RuntimeFrameLease::texture() const noexcept {
    return slot_ ? slot_->texture.get() : nullptr;
}
uint64_t RuntimeFrameLease::sequence() const noexcept {
    return slot_ ? slot_->sequence : 0;
}
RuntimeQpc100ns RuntimeFrameLease::capture_qpc() const noexcept {
    return slot_ ? slot_->qpc : RuntimeQpc100ns{};
}
const RuntimeFrameGeometry& RuntimeFrameLease::geometry() const {
    if (!slot_)
        throw std::logic_error("Empty frame lease");
    return slot_->geometry;
}
RuntimeCompletionToken RuntimeFrameLease::completion() const noexcept {
    return slot_ ? slot_->completion : RuntimeCompletionToken{};
}
bool RuntimeFrameLease::completed() const noexcept {
    return slot_ && slot_->gpu_complete.load(std::memory_order_acquire);
}

RuntimeFramePool::RuntimeFramePool(size_t slot_count, uint64_t domain, GpuDeviceGeneration generation)
    : count_(slot_count), domain_(domain), generation_(generation) {
    if (slot_count == 0 || slot_count > 64 || domain == 0 || generation.value == 0)
        throw std::invalid_argument("Invalid GPU export pool budget or identity");
    for (size_t index = 0; index < slot_count; ++index)
        current_.push_back(std::make_shared<RuntimeFrameLease::Slot>());
}
void RuntimeFramePool::Collect() {
    for (auto& slot : current_) {
        if (slot->phase == RuntimeFrameLease::Slot::Phase::Published && slot.use_count() == 1 &&
            slot->gpu_complete.load(std::memory_order_acquire)) {
            slot->phase = RuntimeFrameLease::Slot::Phase::Free;
            slot->texture.reset();
        }
    }
    std::erase_if(retired_, [](const auto& slot) {
        return slot.use_count() == 1 && slot->gpu_complete.load(std::memory_order_acquire);
    });
}
std::optional<size_t> RuntimeFramePool::Reserve() {
    Collect();
    for (size_t index = 0; index < current_.size(); ++index) {
        auto& slot = current_[index];
        if (slot->phase == RuntimeFrameLease::Slot::Phase::Free) {
            slot->phase = RuntimeFrameLease::Slot::Phase::Reserved;
            return index;
        }
    }
    return std::nullopt;
}
Outcome<RuntimeCompletionToken> RuntimeFramePool::BeginWrite(size_t index, std::shared_ptr<void> texture) {
    if (index >= current_.size() || !texture || current_[index]->phase != RuntimeFrameLease::Slot::Phase::Reserved)
        return RuntimeError(EngineErrorCode::InvalidConfig, "GPU slot is not reserved");
    if (next_serial_ == (std::numeric_limits<uint64_t>::max)())
        return RuntimeError(EngineErrorCode::ResourceBudgetExceeded, "GPU submit serial exhausted");
    auto& slot = current_[index];
    slot->texture = std::move(texture);
    slot->completion = {domain_, generation_, ++next_serial_};
    slot->gpu_complete.store(false, std::memory_order_relaxed);
    slot->phase = RuntimeFrameLease::Slot::Phase::GPUWriting;
    return slot->completion;
}
Outcome<RuntimeFrameLease> RuntimeFramePool::Publish(size_t index, uint64_t sequence, RuntimeQpc100ns qpc,
                                                     RuntimeFrameGeometry geometry) {
    if (index >= current_.size() || current_[index]->phase != RuntimeFrameLease::Slot::Phase::GPUWriting)
        return RuntimeError(EngineErrorCode::InvalidConfig, "GPU frame was not submitted");
    auto& slot = current_[index];
    slot->sequence = sequence;
    slot->qpc = qpc;
    slot->geometry = std::move(geometry);
    slot->phase = RuntimeFrameLease::Slot::Phase::Published;
    return RuntimeFrameLease{slot};
}
void RuntimeFramePool::CancelReserved(size_t index) noexcept {
    if (index < current_.size() && current_[index]->phase == RuntimeFrameLease::Slot::Phase::Reserved)
        current_[index]->phase = RuntimeFrameLease::Slot::Phase::Free;
}
bool RuntimeFramePool::Complete(const RuntimeCompletionToken& token) noexcept {
    if (token.domain != domain_ || token.submit_serial == 0)
        return false;
    for (auto* slots : {&current_, &retired_}) {
        for (auto& slot : *slots) {
            if (slot->completion == token) {
                slot->gpu_complete.store(true, std::memory_order_release);
                return true;
            }
        }
    }
    return false;
}
bool RuntimeFramePool::RotateGeneration(GpuDeviceGeneration generation) {
    Collect();
    if (generation.value <= generation_.value || !retired_.empty())
        return false;
    std::vector<std::shared_ptr<RuntimeFrameLease::Slot>> next;
    for (size_t index = 0; index < count_; ++index)
        next.push_back(std::make_shared<RuntimeFrameLease::Slot>());
    for (auto& slot : current_) {
        if (slot->phase != RuntimeFrameLease::Slot::Phase::Free &&
            slot->phase != RuntimeFrameLease::Slot::Phase::Reserved)
            retired_.push_back(std::move(slot));
    }
    current_ = std::move(next);
    generation_ = generation;
    return true;
}
size_t RuntimeFramePool::retired_slots() const noexcept {
    return retired_.size();
}
bool RuntimeFramePool::idle() const noexcept {
    for (auto* slots : {&current_, &retired_})
        for (const auto& slot : *slots)
            if (!slot->gpu_complete.load(std::memory_order_acquire))
                return false;
    return true;
}
bool RuntimeFramePool::reclaimable() const noexcept {
    for (auto* slots : {&current_, &retired_})
        for (const auto& slot : *slots)
            if (slot.use_count() != 1 || !slot->gpu_complete.load(std::memory_order_acquire))
                return false;
    return true;
}

std::optional<RuntimeFrameLease> RawVideoReader::TakeLatest() {
    std::lock_guard lock(mutex_);
    if (!latest_ || !latest_->completed())
        return std::nullopt;
    return std::exchange(latest_, std::nullopt);
}
void RawVideoReader::Close() noexcept {
    closed_.store(true, std::memory_order_release);
    std::lock_guard lock(mutex_);
    latest_.reset();
}
bool RawVideoReader::closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}
uint64_t RawVideoReader::frames_published() const noexcept {
    return frames_.load(std::memory_order_acquire);
}
bool RawVideoReader::TryPublish(RuntimeFrameLease frame) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || closed())
        return false;
    latest_ = std::move(frame);
    frames_.fetch_add(1, std::memory_order_release);
    return true;
}

ProductionRuntime::ProductionRuntime() = default;
ProductionRuntime::~ProductionRuntime() {
    RequestShutdown();
}
Outcome<std::shared_ptr<FileOutput>> ProductionRuntime::PrepareOutput(std::shared_ptr<SessionState> state,
                                                                      RuntimeOutputCommand command) {
    if (!state || command.command_id == CommandId{} || command.output_generation.value == 0 ||
        command.content_key.empty() || command.content_key.size() > 4096)
        return RuntimeError(EngineErrorCode::InvalidConfig, "Runtime command identity or intent is invalid");
    std::lock_guard lock(mutex_);
    for (const auto& previous : commands_) {
        if (previous.command.command_id != command.command_id)
            continue;
        if (previous.command.content_key != command.content_key ||
            previous.command.expected_revision != command.expected_revision ||
            previous.command.output_generation != command.output_generation)
            return RuntimeError(EngineErrorCode::InvalidConfig, "Command identity was reused with different content");
        if (auto output = previous.output.lock())
            return output;
        return RuntimeError(EngineErrorCode::StaleRevision, "Command outcome is no longer retained");
    }
    if (command.expected_revision != revision_)
        return RuntimeError(EngineErrorCode::StaleRevision, "Runtime configuration revision changed");
    if (stopping_.load(std::memory_order_acquire))
        return RuntimeError(EngineErrorCode::Cancelled, "Runtime is shutting down");
    if (command.output_generation.value <= latest_generation_.value)
        return RuntimeError(EngineErrorCode::StaleRevision, "Output generation is stale");
    if (auto active = active_.lock(); active && active->phase() != FileOutputPhase::Closed)
        return RuntimeError(EngineErrorCode::ResourceBudgetExceeded, "A file output is already attached");
    if (capture_config_ && !Compatible(*capture_config_, state->config))
        return RuntimeError(EngineErrorCode::UnsupportedFormat, "Capture binding differs from the retained runtime");
    auto output = std::make_shared<FileOutput>(state, state->config, command.output_generation);
    latest_generation_ = command.output_generation;
    if (commands_.size() == 16)
        commands_.pop_front();
    commands_.push_back({std::move(command), output});
    return output;
}
Outcome<bool> ProductionRuntime::AttachOutput(std::shared_ptr<FileOutput> output) {
    std::lock_guard lock(mutex_);
    if (!output || stopping_.load(std::memory_order_acquire) || output->generation() != latest_generation_ || pending_)
        return RuntimeError(EngineErrorCode::Cancelled, "Output attachment was superseded or cancelled");
    if (auto active = active_.lock(); active && active->phase() != FileOutputPhase::Closed)
        return RuntimeError(EngineErrorCode::ResourceBudgetExceeded, "Runtime output reader is occupied");
    if (capture_config_ && !Compatible(*capture_config_, output->config()))
        return RuntimeError(EngineErrorCode::UnsupportedFormat, "Capture attachment is incompatible");
    if (!output->LockState() || !output->Attach())
        return RuntimeError(EngineErrorCode::Cancelled, "Output stopped before attachment");
    if (!capture_config_)
        capture_config_ = output->config();
    pending_ = output;
    active_ = output;
    return true;
}
std::shared_ptr<FileOutput> ProductionRuntime::TakePendingOutput() {
    std::lock_guard lock(mutex_);
    return std::exchange(pending_, nullptr);
}
bool ProductionRuntime::HasPendingOutput() const {
    std::lock_guard lock(mutex_);
    return !!pending_;
}
void ProductionRuntime::OutputDetached(const std::shared_ptr<FileOutput>& output) {
    std::lock_guard lock(mutex_);
    if (active_.lock() == output)
        active_.reset();
}
std::shared_ptr<FileOutput> ProductionRuntime::active_output() const {
    std::lock_guard lock(mutex_);
    return active_.lock();
}
Outcome<std::shared_ptr<RawVideoReader>> ProductionRuntime::OpenRawReader(const RecorderConfig& config) {
    std::lock_guard lock(mutex_);
    if (stopping_.load(std::memory_order_acquire))
        return RuntimeError(EngineErrorCode::Cancelled, "Runtime is shutting down");
    if (capture_config_ && !Compatible(*capture_config_, config))
        return RuntimeError(EngineErrorCode::UnsupportedFormat, "Raw reader capture binding is incompatible");
    std::erase_if(raw_readers_, [](const auto& reader) {
        const auto value = reader.lock();
        return !value || value->closed();
    });
    if (raw_readers_.size() >= 4)
        return RuntimeError(EngineErrorCode::ResourceBudgetExceeded, "Raw reader connection budget exceeded");
    if (!capture_config_)
        capture_config_ = config;
    auto reader = std::make_shared<RawVideoReader>();
    raw_readers_.push_back(reader);
    raw_reader_count_.store(raw_readers_.size(), std::memory_order_release);
    return reader;
}
std::vector<std::shared_ptr<RawVideoReader>> ProductionRuntime::RawReaders() {
    if (raw_reader_count_.load(std::memory_order_acquire) == 0)
        return {};
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<RawVideoReader>> result;
    std::erase_if(raw_readers_, [&](const auto& weak) {
        auto reader = weak.lock();
        if (!reader || reader->closed())
            return true;
        result.push_back(std::move(reader));
        return false;
    });
    raw_reader_count_.store(result.size(), std::memory_order_release);
    return result;
}
bool ProductionRuntime::HasRawReaders() {
    return !RawReaders().empty();
}
std::shared_ptr<VideoThread> ProductionRuntime::AcquireVideoWorker(std::shared_ptr<SessionState> state) {
    std::lock_guard lock(mutex_);
    if (!video_worker_)
        video_worker_ = std::make_shared<VideoThread>(std::move(state));
    return video_worker_;
}
bool ProductionRuntime::VideoWorkerMayExit(VideoThread* worker) noexcept {
    std::lock_guard lock(mutex_);
    std::erase_if(raw_readers_, [](const auto& weak) {
        auto reader = weak.lock();
        return !reader || reader->closed();
    });
    raw_reader_count_.store(raw_readers_.size(), std::memory_order_release);
    if (pending_ || (!stopping_.load(std::memory_order_acquire) && !raw_readers_.empty()))
        return false;
    if (video_worker_.get() == worker)
        video_worker_.reset();
    if (raw_readers_.empty())
        capture_config_.reset();
    return true;
}
void ProductionRuntime::VideoWorkerExited(VideoThread* worker) noexcept {
    std::lock_guard lock(mutex_);
    if (video_worker_.get() == worker)
        video_worker_.reset();
}
void ProductionRuntime::RequestShutdown() noexcept {
    stopping_.store(true, std::memory_order_release);
    std::shared_ptr<FileOutput> active, pending;
    {
        std::lock_guard lock(mutex_);
        active = active_.lock();
        pending = pending_;
    }
    LARGE_INTEGER ticks{}, frequency{};
    QueryPerformanceCounter(&ticks);
    QueryPerformanceFrequency(&frequency);
    const RuntimeQpc100ns endpoint{
        QpcTicksTo100ns(static_cast<uint64_t>(ticks.QuadPart), static_cast<uint64_t>(frequency.QuadPart))};
    for (auto& output : {active, pending}) {
        if (!output)
            continue;
        output->RequestStop(endpoint);
        if (auto state = output->LockState())
            state->RequestCleanStop();
    }
    for (auto& reader : RawReaders())
        reader->Close();
}
bool ProductionRuntime::stopping() const noexcept {
    return stopping_.load(std::memory_order_acquire);
}
ConfigRevision ProductionRuntime::revision() const noexcept {
    return revision_;
}
void ProductionRuntime::SetAudioRegistry(std::shared_ptr<SharedAudioCaptureRegistry> registry) {
    std::lock_guard lock(mutex_);
    audio_registry_ = std::move(registry);
}
std::shared_ptr<SharedAudioCaptureRegistry> ProductionRuntime::audio_registry() const {
    std::lock_guard lock(mutex_);
    return audio_registry_;
}
void ProductionRuntime::CountRawExport(uint64_t bytes) noexcept {
    raw_export_copies_.fetch_add(1, std::memory_order_relaxed);
    raw_export_bytes_.fetch_add(bytes, std::memory_order_relaxed);
}
uint64_t ProductionRuntime::raw_export_copies() const noexcept {
    return raw_export_copies_.load(std::memory_order_acquire);
}
uint64_t ProductionRuntime::raw_export_bytes() const noexcept {
    return raw_export_bytes_.load(std::memory_order_acquire);
}
void ProductionRuntime::MarkGpuStalled() noexcept {
    gpu_stalled_.store(true, std::memory_order_release);
}
bool ProductionRuntime::gpu_stalled() const noexcept {
    return gpu_stalled_.load(std::memory_order_acquire);
}

} // namespace exosnap::engine

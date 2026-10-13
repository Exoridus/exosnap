#include "audio_tee.h"
#include <cstddef>
#include <cstdint>
#include <exosnap/engine/audio_limits.h>
#include <exosnap/engine/interfaces/IAudioCaptureSource.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <algorithm>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace exosnap::engine {
namespace {
constexpr uint64_t kUnsealed = (std::numeric_limits<uint64_t>::max)();

uint32_t FramesBefore(uint64_t boundary, uint64_t origin, uint32_t rate, uint32_t frames) {
    if (boundary <= origin)
        return 0;
    const uint64_t duration = DeviceFramesToNs(frames, rate);
    if (boundary - origin >= duration)
        return frames;
    return static_cast<uint32_t>(((boundary - origin) * rate + 999'999'999ULL) / 1'000'000'000ULL);
}

struct StoredBlock {
    AudioCaptureDescriptor descriptor;
    AudioBlockMetadata metadata;
    uint64_t payload_position = 0;
    uint64_t media_end_ns = 0;
};

struct ReaderSlot {
    bool attached = false;
    bool leased = false;
    uint64_t cursor = 0;
    uint64_t start_qpc_ns = 0;
    uint64_t end_sequence = kUnsealed;
    uint64_t end_qpc_ns = kUnsealed;
    uint64_t overflow_frames = 0;
    uint64_t wake_sequence = 0;
    std::vector<float> lease;
    AudioBlockView held;
#ifdef _WIN32
    HANDLE event = nullptr;
#endif
};
} // namespace

struct AudioTeeState {
    mutable std::mutex mutex;
    AudioCaptureDescriptor admission;
    AudioCaptureDescriptor descriptor;
    std::vector<float> ring;
    std::vector<StoredBlock> blocks;
    std::vector<ReaderSlot> readers;
    uint64_t next_sequence = 0;
    uint64_t oldest_sequence = 0;
    uint64_t reader_overflow_frames = 0;
    uint64_t payload_position = 0;
    uint64_t media_position_ns = 0;
    size_t reserved_payload_bytes = 0;
    bool stopped = false;

    ~AudioTeeState() {
#ifdef _WIN32
        for (const auto& reader : readers)
            if (reader.event)
                CloseHandle(reader.event);
#endif
    }

    StoredBlock& Block(uint64_t sequence) {
        return blocks[static_cast<size_t>(sequence % blocks.size())];
    }
    const StoredBlock& Block(uint64_t sequence) const {
        return blocks[static_cast<size_t>(sequence % blocks.size())];
    }
    void Wake(ReaderSlot& reader) {
        ++reader.wake_sequence;
#ifdef _WIN32
        SetEvent(reader.event);
#endif
    }
    std::pair<uint32_t, uint32_t> Range(const ReaderSlot& reader, const StoredBlock& block) const {
        const auto& metadata = block.metadata;
        const uint32_t begin = reader.start_qpc_ns ? FramesBefore(reader.start_qpc_ns, metadata.output_qpc_ns,
                                                                  block.descriptor.sample_rate, metadata.frames)
                                                   : 0;
        const uint32_t end = reader.end_qpc_ns == kUnsealed
                                 ? metadata.frames
                                 : FramesBefore(reader.end_qpc_ns, metadata.output_qpc_ns, block.descriptor.sample_rate,
                                                metadata.frames);
        return {begin, (std::max)(begin, end)};
    }
    bool Find(ReaderSlot& reader) {
        while (reader.cursor < next_sequence && reader.cursor < reader.end_sequence) {
            const auto& block = Block(reader.cursor);
            const auto [begin, end] = Range(reader, block);
            if (end > begin)
                return true;
            ++reader.cursor;
        }
#ifdef _WIN32
        if (!stopped && reader.end_sequence == kUnsealed)
            ResetEvent(reader.event);
#endif
        return false;
    }
    void Evict() {
        const auto& block = Block(oldest_sequence);
        for (auto& reader : readers) {
            if (reader.attached && reader.cursor <= oldest_sequence) {
                if (oldest_sequence < reader.end_sequence) {
                    const auto [begin, end] = Range(reader, block);
                    reader.overflow_frames += end - begin;
                    reader_overflow_frames += end - begin;
                }
                reader.cursor = oldest_sequence + 1;
            }
        }
        ++oldest_sequence;
    }
};

AudioTee::AudioTee(std::shared_ptr<AudioTeeState> state) : state_(std::move(state)) {
}

size_t AudioCorrectionPayloadBytes(const AudioCaptureDescriptor& admission) {
    // Two external scratch buffers and a conservative bound for Swr's input,
    // output and filter storage are reserved before the native stream starts.
    const uint64_t frames = 6ULL * admission.maximum_packet_frames + 1024;
    const size_t limit = (std::numeric_limits<size_t>::max)();
    if (admission.channels && frames > limit / sizeof(float) / admission.channels)
        return limit;
    return static_cast<size_t>(frames * admission.channels * sizeof(float));
}

size_t AudioTee::RequiredPayloadBytes(const AudioCaptureDescriptor& admission, uint32_t maximum_readers) {
    if (!admission.sample_rate || !admission.channels || !admission.maximum_packet_frames || !maximum_readers ||
        maximum_readers > kMaxAudioTeeReaders)
        return 0;
    const uint64_t frames = static_cast<uint64_t>(admission.sample_rate) * 2 +
                            static_cast<uint64_t>(maximum_readers) * admission.maximum_packet_frames;
    const size_t limit = (std::numeric_limits<size_t>::max)();
    if (frames > limit / sizeof(float) / admission.channels)
        return limit;
    const auto correction = AudioCorrectionPayloadBytes(admission);
    const auto bytes = static_cast<size_t>(frames * admission.channels * sizeof(float));
    return correction > limit - bytes ? limit : bytes + correction;
}

std::shared_ptr<AudioTee> AudioTee::Create(const AudioCaptureDescriptor& admission, std::string& error,
                                           uint32_t maximum_readers) {
    error.clear();
    const size_t payload = RequiredPayloadBytes(admission, maximum_readers);
    if (!payload || payload > kAudioTeePayloadBytes || admission.maximum_packet_frames > admission.sample_rate * 2ULL) {
        error = "Audio tee payload budget exceeded or invalid format: required " + std::to_string(payload) +
                " bytes, budget " + std::to_string(kAudioTeePayloadBytes) + " bytes";
        return {};
    }
    try {
        auto state = std::make_shared<AudioTeeState>();
        state->admission = state->descriptor = admission;
        state->reserved_payload_bytes = payload;
        const size_t ring_frames = static_cast<size_t>(admission.sample_rate) * 2;
        state->ring.resize(ring_frames * admission.channels);
        state->blocks.resize(ring_frames);
        state->readers.resize(maximum_readers);
        for (auto& reader : state->readers) {
            reader.lease.resize(static_cast<size_t>(admission.maximum_packet_frames) * admission.channels);
#ifdef _WIN32
            reader.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!reader.event) {
                error = "Audio tee reader event creation failed";
                return {};
            }
#endif
        }
        return std::shared_ptr<AudioTee>(new AudioTee(std::move(state)));
    } catch (const std::bad_alloc&) {
        error = "Audio tee preallocation failed";
        return {};
    }
}

std::vector<std::shared_ptr<AudioTeeReader>> AudioTee::AttachReaders(uint32_t count, uint64_t start_qpc_ns,
                                                                     std::string& error) {
    error.clear();
    std::unique_lock lock(state_->mutex);
    size_t free = 0;
    for (const auto& reader : state_->readers)
        free += !reader.attached;
    if (state_->stopped || !count || count > free) {
        error = state_->stopped ? "Audio tee is stopped" : "Audio tee reader capacity exceeded";
        return {};
    }
    if (start_qpc_ns && state_->oldest_sequence && state_->oldest_sequence < state_->next_sequence &&
        start_qpc_ns < state_->Block(state_->oldest_sequence).metadata.output_qpc_ns) {
        error = "Requested audio start prefix has left the two-second horizon";
        return {};
    }
    std::vector<std::shared_ptr<AudioTeeReader>> result;
    try {
        result.reserve(count);
        for (uint32_t index = 0; index < state_->readers.size() && result.size() < count; ++index) {
            if (!state_->readers[index].attached)
                result.push_back(std::shared_ptr<AudioTeeReader>(new AudioTeeReader(state_, index)));
        }
    } catch (const std::bad_alloc&) {
        error = "Audio reader transaction allocation failed";
        return {};
    }
    for (const auto& adapter : result) {
        auto& reader = state_->readers[adapter->index_];
        reader.attached = true;
        reader.leased = false;
        reader.cursor = start_qpc_ns ? state_->oldest_sequence : state_->next_sequence;
        reader.start_qpc_ns = start_qpc_ns;
        reader.end_sequence = reader.end_qpc_ns = kUnsealed;
        reader.overflow_frames = 0;
        reader.held = {};
        adapter->committed_ = true;
        state_->Wake(reader);
    }
    return result;
}

bool AudioTee::Publish(std::span<const float> samples, AudioBlockMetadata metadata, std::string& error) {
    error.clear();
    std::lock_guard lock(state_->mutex);
    const size_t sample_count = static_cast<size_t>(metadata.frames) * state_->descriptor.channels;
    if (state_->stopped || !metadata.frames || metadata.frames > state_->admission.maximum_packet_frames ||
        (!metadata.silence && samples.size() != sample_count) || (!samples.empty() && samples.size() != sample_count)) {
        error = "Audio tee publish exceeds its admitted format or stopped state";
        return false;
    }
    const uint64_t new_end_qpc =
        metadata.output_qpc_ns + DeviceFramesToNs(metadata.frames, state_->descriptor.sample_rate);
    const uint64_t new_media_end = state_->media_position_ns +
                                   DeviceFramesToNs(metadata.frames + static_cast<uint64_t>(metadata.device_gap_frames),
                                                    state_->descriptor.sample_rate);
    while (state_->oldest_sequence < state_->next_sequence) {
        const auto& old = state_->Block(state_->oldest_sequence);
        const uint64_t old_end =
            old.metadata.output_qpc_ns + DeviceFramesToNs(old.metadata.frames, old.descriptor.sample_rate);
        const bool horizon = new_media_end - old.media_end_ns >= kAudioTeeHorizonNs ||
                             (metadata.output_qpc_ns && old.metadata.output_qpc_ns && new_end_qpc >= old_end &&
                              new_end_qpc - old_end >= kAudioTeeHorizonNs);
        if (!horizon && state_->payload_position + sample_count - old.payload_position <= state_->ring.size() &&
            state_->next_sequence - state_->oldest_sequence < state_->blocks.size())
            break;
        state_->Evict();
    }
    const size_t offset = static_cast<size_t>(state_->payload_position % state_->ring.size());
    const size_t first = (std::min)(sample_count, state_->ring.size() - offset);
    if (samples.empty()) {
        std::fill_n(state_->ring.data() + offset, first, 0.0f);
        std::fill_n(state_->ring.data(), sample_count - first, 0.0f);
    } else {
        std::copy_n(samples.data(), first, state_->ring.data() + offset);
        std::copy_n(samples.data() + first, sample_count - first, state_->ring.data());
    }
    metadata.sequence = state_->next_sequence;
    state_->Block(state_->next_sequence) = {state_->descriptor, metadata, state_->payload_position, new_media_end};
    ++state_->next_sequence;
    state_->payload_position += sample_count;
    state_->media_position_ns = new_media_end;
    for (auto& reader : state_->readers)
        if (reader.attached)
            state_->Wake(reader);
    return true;
}

bool AudioTee::SetDescriptor(const AudioCaptureDescriptor& descriptor, std::string& error) {
    error.clear();
    std::lock_guard lock(state_->mutex);
    if (state_->stopped || !descriptor.sample_rate || !descriptor.channels ||
        descriptor.sample_rate > state_->admission.sample_rate || descriptor.channels > state_->admission.channels ||
        descriptor.maximum_packet_frames > state_->admission.maximum_packet_frames) {
        error = "New native format exceeds admitted audio payload envelope; reprepare required";
        return false;
    }
    state_->descriptor = descriptor;
    for (auto& reader : state_->readers)
        if (reader.attached)
            state_->Wake(reader);
    return true;
}

void AudioTee::Stop() {
    std::lock_guard lock(state_->mutex);
    state_->stopped = true;
    for (auto& reader : state_->readers)
        if (reader.attached) {
            reader.end_sequence = (std::min)(reader.end_sequence, state_->next_sequence);
            state_->Wake(reader);
        }
}
size_t AudioTee::ReservedPayloadBytes() const {
    return state_->reserved_payload_bytes;
}
size_t AudioTee::MetadataBytes() const {
    return state_->blocks.size() * sizeof(StoredBlock);
}
uint32_t AudioTee::ReaderCount() const {
    std::lock_guard lock(state_->mutex);
    uint32_t count = 0;
    for (const auto& reader : state_->readers)
        count += reader.attached;
    return count;
}
uint64_t AudioTee::ReaderOverflowFrames() const {
    std::lock_guard lock(state_->mutex);
    return state_->reader_overflow_frames;
}

AudioTeeReader::AudioTeeReader(std::shared_ptr<AudioTeeState> state, uint32_t index)
    : state_(std::move(state)), index_(index) {
}
AudioTeeReader::~AudioTeeReader() {
    if (!committed_)
        return;
    std::lock_guard lock(state_->mutex);
    state_->readers[index_].attached = false;
    state_->readers[index_].leased = false;
}
bool AudioTeeReader::Acquire(AudioBlockView& block, std::string& error) {
    block = {};
    error.clear();
    std::lock_guard lock(state_->mutex);
    auto& reader = state_->readers[index_];
    if (reader.leased) {
        error = "Audio reader already holds a lease";
        return false;
    }
    if (!state_->Find(reader))
        return false;
    const auto& stored = state_->Block(reader.cursor);
    const auto [begin, end] = state_->Range(reader, stored);
    const size_t count = static_cast<size_t>(end - begin) * stored.descriptor.channels;
    const size_t offset = static_cast<size_t>(
        (stored.payload_position + static_cast<size_t>(begin) * stored.descriptor.channels) % state_->ring.size());
    const size_t first = (std::min)(count, state_->ring.size() - offset);
    std::copy_n(state_->ring.data() + offset, first, reader.lease.data());
    std::copy_n(state_->ring.data(), count - first, reader.lease.data() + first);
    reader.held = {stored.descriptor, stored.metadata, {reader.lease.data(), count}, reader.overflow_frames};
    reader.held.metadata.frames = end - begin;
    reader.held.metadata.output_qpc_ns += DeviceFramesToNs(begin, stored.descriptor.sample_rate);
    reader.held.metadata.native_sample_position += begin;
    if (begin && stored.metadata.filter_latency_ns)
        reader.held.metadata.native_sample_position_valid = false;
    reader.overflow_frames = 0;
    reader.leased = true;
    ++reader.cursor;
    block = reader.held;
    return true;
}
void AudioTeeReader::Release() {
    std::lock_guard lock(state_->mutex);
    auto& reader = state_->readers[index_];
    reader.leased = false;
    reader.held = {};
}
uint32_t AudioTeeReader::PendingFrames() const {
    std::lock_guard lock(state_->mutex);
    auto& reader = state_->readers[index_];
    if (reader.leased || !state_->Find(reader))
        return 0;
    const auto [begin, end] = state_->Range(reader, state_->Block(reader.cursor));
    return end - begin;
}
AudioCaptureDescriptor AudioTeeReader::Descriptor() const {
    std::lock_guard lock(state_->mutex);
    auto& reader = state_->readers[index_];
    if (reader.leased)
        return reader.held.descriptor;
    if (state_->Find(reader))
        return state_->Block(reader.cursor).descriptor;
    return state_->descriptor;
}
AudioReaderEnd AudioTeeReader::SealEnd(uint64_t end_qpc_ns) {
    std::lock_guard lock(state_->mutex);
    auto& reader = state_->readers[index_];
    reader.end_sequence = (std::min)(reader.end_sequence, state_->next_sequence);
    reader.end_qpc_ns = (std::min)(reader.end_qpc_ns, end_qpc_ns);
    state_->Wake(reader);
    return {reader.end_sequence, reader.end_qpc_ns};
}
bool AudioTeeReader::Ended() const {
    std::lock_guard lock(state_->mutex);
    auto& reader = state_->readers[index_];
    return !reader.leased && reader.end_sequence != kUnsealed && !state_->Find(reader);
}
void* AudioTeeReader::ReadyEvent() const {
#ifdef _WIN32
    return state_->readers[index_].event;
#else
    return nullptr;
#endif
}
uint64_t AudioTeeReader::WakeSequence() const {
    std::lock_guard lock(state_->mutex);
    return state_->readers[index_].wake_sequence;
}
} // namespace exosnap::engine

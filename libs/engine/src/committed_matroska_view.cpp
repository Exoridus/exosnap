#include "committed_matroska_view.h"
#include <exosnap/engine/recording_media_types.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include <windows.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace exosnap::engine {
namespace {

EngineError ReadError(EngineErrorCode code, const char* context, DWORD native = 0) {
    EngineError error;
    error.code = code;
    error.owner = EngineModule::Clip;
    error.context = context;
    if (native != 0)
        error.win32_code = native;
    return error;
}

} // namespace

struct CommittedMatroskaView::State {
    HANDLE file = INVALID_HANDLE_VALUE;
    std::shared_ptr<const RecordingMediaDescriptor> descriptor;
    std::vector<ClusterExtent> extents;
    std::vector<uint64_t> starts;
    uint64_t length = 0;
    uint64_t cursor = 0;
    std::atomic<bool> cancelled{false};
    AVIOContext* avio = nullptr;
    std::optional<EngineError> error;

    ~State() {
        if (avio) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }
};

CommittedMatroskaView::CommittedMatroskaView(std::unique_ptr<State> state) : state_(std::move(state)) {
}
CommittedMatroskaView::~CommittedMatroskaView() = default;

Outcome<std::unique_ptr<CommittedMatroskaView>>
CommittedMatroskaView::Open(std::shared_ptr<const RecordingMediaDescriptor> descriptor,
                            std::vector<ClusterExtent> extents, uint64_t readable_end) {
    if (!descriptor || descriptor->immutable_header.empty() || extents.empty())
        return ReadError(EngineErrorCode::InvalidConfig, "Committed view needs a header and closed extents");
    if (extents.size() > 4096)
        return ReadError(EngineErrorCode::ResourceBudgetExceeded, "Committed view requires a bounded cluster page");
    auto state = std::make_unique<State>();
    state->descriptor = std::move(descriptor);
    state->length = state->descriptor->immutable_header.size();
    uint64_t previous_end = state->descriptor->immutable_header.size();
    for (const auto& extent : extents) {
        if (extent.file_begin < previous_end || extent.file_end <= extent.file_begin ||
            extent.file_end > readable_end ||
            extent.file_end - extent.file_begin >
                static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) - state->length)
            return ReadError(EngineErrorCode::SourceIntegrityFailure, "Invalid committed cluster extent");
        state->starts.push_back(state->length);
        state->length += extent.file_end - extent.file_begin;
        previous_end = extent.file_end;
    }
    state->extents = std::move(extents);
    const auto path = std::filesystem::path(state->descriptor->path);
    state->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (state->file == INVALID_HANDLE_VALUE)
        return ReadError(EngineErrorCode::UnavailableSource, "Cannot open committed source", GetLastError());
    FILE_ID_INFO identity{};
    if (!GetFileInformationByHandleEx(state->file, FileIdInfo, &identity, sizeof(identity)))
        return ReadError(EngineErrorCode::SourceIntegrityFailure, "Cannot identify committed source", GetLastError());
    FileIdentity actual;
    actual.volume_serial = identity.VolumeSerialNumber;
    std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier), actual.file_id.begin());
    if (actual != state->descriptor->file_identity)
        return ReadError(EngineErrorCode::SourceIntegrityFailure, "Committed source file identity changed");
    auto view = std::unique_ptr<CommittedMatroskaView>(new CommittedMatroskaView(std::move(state)));
    auto* buffer = static_cast<uint8_t*>(av_malloc(32768));
    if (!buffer)
        return ReadError(EngineErrorCode::ResourceBudgetExceeded, "Cannot allocate committed AVIO buffer");
    view->state_->avio = avio_alloc_context(buffer, 32768, 0, view.get(), ReadPacket, nullptr, Seek);
    if (!view->state_->avio) {
        av_free(buffer);
        return ReadError(EngineErrorCode::ResourceBudgetExceeded, "Cannot allocate committed AVIO context");
    }
    view->state_->avio->seekable = AVIO_SEEKABLE_NORMAL;
    return view;
}

Outcome<size_t> CommittedMatroskaView::ReadAt(uint64_t offset, std::span<uint8_t> destination) {
    if (state_->cancelled.load(std::memory_order_acquire))
        return ReadError(EngineErrorCode::Cancelled, "Committed read cancelled");
    if (offset > state_->length)
        return ReadError(EngineErrorCode::SourceIntegrityFailure, "Read offset exceeds committed view");
    const size_t requested =
        static_cast<size_t>((std::min)(static_cast<uint64_t>(destination.size()), state_->length - offset));
    size_t completed = 0;
    while (completed < requested) {
        const uint64_t position = offset + completed;
        if (position < state_->descriptor->immutable_header.size()) {
            const size_t count = (std::min)(requested - completed, state_->descriptor->immutable_header.size() -
                                                                       static_cast<size_t>(position));
            std::memcpy(destination.data() + completed, state_->descriptor->immutable_header.data() + position, count);
            completed += count;
            continue;
        }
        const size_t index = static_cast<size_t>(
            std::upper_bound(state_->starts.begin(), state_->starts.end(), position) - state_->starts.begin() - 1);
        const auto& extent = state_->extents[index];
        const uint64_t within = position - state_->starts[index];
        const DWORD count = static_cast<DWORD>(
            (std::min)({static_cast<uint64_t>(requested - completed), extent.file_end - extent.file_begin - within,
                        static_cast<uint64_t>((std::numeric_limits<DWORD>::max)())}));
        const uint64_t absolute = extent.file_begin + within;
        OVERLAPPED operation{};
        operation.Offset = static_cast<DWORD>(absolute);
        operation.OffsetHigh = static_cast<DWORD>(absolute >> 32);
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent)
            return ReadError(EngineErrorCode::UnavailableSource, "Cannot create source I/O event", GetLastError());
        DWORD received = 0;
        BOOL ok = ReadFile(state_->file, destination.data() + completed, count, &received, &operation);
        DWORD native = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && native == ERROR_IO_PENDING) {
            while (WaitForSingleObject(operation.hEvent, 50) == WAIT_TIMEOUT) {
                if (state_->cancelled.load(std::memory_order_acquire))
                    CancelIoEx(state_->file, &operation);
            }
            // The operation must finish before its stack OVERLAPPED and destination can be released.
            ok = GetOverlappedResult(state_->file, &operation, &received, TRUE);
            native = ok ? ERROR_SUCCESS : GetLastError();
        }
        CloseHandle(operation.hEvent);
        if (state_->cancelled.load(std::memory_order_acquire))
            return ReadError(EngineErrorCode::Cancelled, "Committed read cancelled", native);
        if (!ok || received != count) {
            state_->error =
                ReadError(EngineErrorCode::SourceIntegrityFailure, "Short read inside committed extent", native);
            return *state_->error;
        }
        completed += received;
    }
    return completed;
}

void CommittedMatroskaView::Cancel() noexcept {
    state_->cancelled.store(true, std::memory_order_release);
    CancelIoEx(state_->file, nullptr);
}
uint64_t CommittedMatroskaView::size() const noexcept {
    return state_->length;
}
AVIOContext* CommittedMatroskaView::avio() const noexcept {
    return state_->avio;
}
const std::optional<EngineError>& CommittedMatroskaView::last_error() const noexcept {
    return state_->error;
}

int CommittedMatroskaView::ReadPacket(void* opaque, uint8_t* buffer, int count) {
    auto& view = *static_cast<CommittedMatroskaView*>(opaque);
    auto result = view.ReadAt(view.state_->cursor, {buffer, static_cast<size_t>(count)});
    if (!result.succeeded()) {
        view.state_->error = *result.error();
        return result.error()->code == EngineErrorCode::Cancelled ? AVERROR_EXIT : AVERROR(EIO);
    }
    view.state_->cursor += *result.value();
    return *result.value() == 0 ? AVERROR_EOF : static_cast<int>(*result.value());
}

int64_t CommittedMatroskaView::Seek(void* opaque, int64_t offset, int whence) {
    auto& state = *static_cast<CommittedMatroskaView*>(opaque)->state_;
    if (whence == AVSEEK_SIZE)
        return static_cast<int64_t>(state.length);
    whence &= ~AVSEEK_FORCE;
    const int64_t base = whence == SEEK_SET   ? 0
                         : whence == SEEK_CUR ? static_cast<int64_t>(state.cursor)
                         : whence == SEEK_END ? static_cast<int64_t>(state.length)
                                              : -1;
    if (base < 0 || offset < -base || offset > static_cast<int64_t>(state.length) - base)
        return AVERROR(EINVAL);
    state.cursor = static_cast<uint64_t>(base + offset);
    return static_cast<int64_t>(state.cursor);
}

} // namespace exosnap::engine

// The real ETW backend: the only place in the application that names PresentMon.
//
// Everything here is a call into Windows or into the vendored consumer. There is no
// decision, no accumulation and no attribution in this file -- those live in
// PresentMonEtwSession, which is compiled identically for the product and for the
// tests. Keeping the line exactly there is what makes the session testable at all.

#include "diagnostics/PresentTraceBackend.h"

#ifdef EXOSNAP_HAS_PRESENTMON

#define WIN32_LEAN_AND_MEAN
// clang-format off
// Order matters: evntrace.h depends on TRACEHANDLE et al. from windows.h.
#include <windows.h>
#include <evntrace.h>
// clang-format on

// Vendored PresentMon PresentData (pinned v1.10.0):
#include "PresentMonTraceConsumer.hpp"
#include "TraceSession.hpp"

namespace exosnap::diagnostics {
namespace {

constexpr wchar_t kSessionName[] = L"ExoSnapPresentMon";

class PresentMonTraceBackend final : public IPresentTraceBackend {
  public:
    PresentTraceOpenResult Open() override {
        // Ownership is checked BEFORE opening, and nothing is stopped by name. A
        // session already carrying this name may belong to another ExoSnap
        // instance, a test collector or a crashed run; tearing it down would
        // break whichever of those owns it. A stale session therefore has to be
        // cleaned up deliberately and is reported as a conflict until then.
        {
            std::vector<wchar_t> buffer(sizeof(EVENT_TRACE_PROPERTIES) + (sizeof(kSessionName) + 1) * sizeof(wchar_t));
            auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
            properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size() * sizeof(wchar_t));
            properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            const ULONG query = ::ControlTraceW(0, kSessionName, properties, EVENT_TRACE_CONTROL_QUERY);
            if (query == ERROR_SUCCESS)
                return PresentTraceOpenResult::SessionConflict;
        }
        // realtime: etlPath=nullptr; no WinMR: mrConsumer=nullptr.
        const ULONG status = session_.Start(&consumer_, nullptr, nullptr, kSessionName);
        switch (status) {
        case ERROR_SUCCESS:
            return PresentTraceOpenResult::Opened;
        case ERROR_ACCESS_DENIED:
            return PresentTraceOpenResult::AccessDenied;
        case ERROR_ALREADY_EXISTS:
            return PresentTraceOpenResult::SessionConflict;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_OPERATION:
            return PresentTraceOpenResult::NotSupported;
        default:
            return PresentTraceOpenResult::Failed;
        }
    }

    void Consume() override {
        // Blocks, routing events into consumer_ (TraceSession installed the callback),
        // until Close() -> CloseTrace unblocks it.
        ::ProcessTrace(&session_.mTraceHandle, 1, nullptr, nullptr);
    }

    void Close() override {
        session_.Stop();
    }

    int64_t TimestampFrequency() const override {
        return session_.mTimestampFrequency.QuadPart;
    }

    std::vector<TracePresentEvent> Drain() override {
        std::vector<std::shared_ptr<PresentEvent>> presents;
        // DequeuePresentEvents is thread-safe against the consumer thread; that is why
        // the drain can run on the reader side at all.
        consumer_.DequeuePresentEvents(presents);
        std::vector<TracePresentEvent> decoded;
        decoded.reserve(presents.size());
        for (const auto& present : presents) {
            if (!present)
                continue;
            TracePresentEvent event;
            event.process_id = present->ProcessId;
            event.hwnd = present->Hwnd;
            for (const auto& dependent : present->DependentPresents) {
                if (dependent)
                    event.related_process_ids.push_back(dependent->ProcessId);
            }
            event.present_qpc = present->PresentStartTime;
            event.present_mode_code = static_cast<int>(present->PresentMode);
            event.sync_interval = present->SyncInterval;
            event.tearing_flag = present->SupportsTearing;
            event.discarded = present->FinalState == PresentResult::Discarded;
            decoded.push_back(event);
        }
        return decoded;
    }

  private:
    PMTraceConsumer consumer_; // default ctor: mTrackDisplay=true, mTrackGPU/Input=false
    TraceSession session_;     // PresentMon's session helper (open + enable + OpenTrace)
};

} // namespace

std::shared_ptr<IPresentTraceBackend> MakePresentTraceBackend() {
    return std::make_shared<PresentMonTraceBackend>();
}

} // namespace exosnap::diagnostics

#else // !EXOSNAP_HAS_PRESENTMON

namespace exosnap::diagnostics {

// No vendored consumer in this build. The session treats a null backend exactly as it
// treats an ETW session that refused to open: Start() is false and everything reports
// unavailable. One code path, not two.
std::shared_ptr<IPresentTraceBackend> MakePresentTraceBackend() {
    return nullptr;
}

} // namespace exosnap::diagnostics

#endif

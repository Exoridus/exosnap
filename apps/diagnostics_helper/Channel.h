#pragma once

#include "Handle.h"
#include "Protocol.h"

namespace exosnap::diagnostics::ipc {

// The pipe must be message-mode and overlapped. Retirement waits for the OS to
// release the OVERLAPPED and record buffer, even when cancellation races I/O.
inline bool Transfer(HANDLE pipe, HANDLE lifetime, Record& record, bool write, DWORD timeout) {
    if (WaitForSingleObject(lifetime, 0) != WAIT_TIMEOUT)
        return false;
    Handle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!done)
        return false;
    OVERLAPPED operation{};
    operation.hEvent = done.get();
    DWORD bytes = 0;
    BOOL ok = write ? WriteFile(pipe, &record, sizeof(record), &bytes, &operation)
                    : ReadFile(pipe, &record, sizeof(record), &bytes, &operation);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING)
            return false;
        HANDLE waits[] = {lifetime, done.get()};
        if (WaitForMultipleObjects(2, waits, FALSE, timeout) != WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &bytes, TRUE);
            return false;
        }
        ok = GetOverlappedResult(pipe, &operation, &bytes, FALSE);
    }
    return ok && Valid(record, bytes);
}

} // namespace exosnap::diagnostics::ipc

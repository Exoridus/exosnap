#pragma once

#include "../viewmodels/RecordViewModel.h"

#include <string>

namespace exosnap::diagnostics {

struct UiErrorMessage {
    std::wstring title;
    std::wstring message;
    std::wstring action_hint;
};

[[nodiscard]] UiErrorMessage MapErrorToUserMessage(const UiRecordingResult& result);

} // namespace exosnap::diagnostics

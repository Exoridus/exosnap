#include "error_message.h"

#include <array>
#include <string_view>
#include <utility>

namespace exosnap::diagnostics {
namespace {

bool Contains(std::wstring_view haystack, std::wstring_view needle) {
    return haystack.find(needle) != std::wstring_view::npos;
}

bool ContainsAny(std::wstring_view haystack, const std::array<std::wstring_view, 2>& needles) {
    for (const auto needle : needles) {
        if (Contains(haystack, needle)) {
            return true;
        }
    }
    return false;
}

bool ContainsAny(std::wstring_view haystack, const std::array<std::wstring_view, 3>& needles) {
    for (const auto needle : needles) {
        if (Contains(haystack, needle)) {
            return true;
        }
    }
    return false;
}

UiErrorMessage MakeMessage(std::wstring title, std::wstring message, std::wstring action_hint) {
    return UiErrorMessage{
        std::move(title),
        std::move(message),
        std::move(action_hint),
    };
}

} // namespace

UiErrorMessage MapErrorToUserMessage(const UiRecordingResult& result) {
    if (result.succeeded) {
        return MakeMessage(L"Recording complete", {}, {});
    }

    const std::wstring_view phase = result.error_phase;
    const std::wstring_view detail = result.error_detail;

    if (Contains(detail, L"PID unavailable")) {
        return MakeMessage(L"Window closed", L"The selected window was closed before recording started.",
                           L"Reselect the window and try again.");
    }

    if (phase == L"Video Capture" && Contains(detail, L"handle invalid")) {
        return MakeMessage(L"Window closed", L"The selected window was closed before recording started.",
                           L"Reselect the window and try again.");
    }

    if (Contains(detail, L"too small for NV12") || Contains(detail, L"WGC source size invalid")) {
        return MakeMessage(L"Window too small", L"The window is too small to record.",
                           L"Resize or restore the window, then try again.");
    }

    if (phase == L"Prepare" && Contains(detail, L"NVENC open")) {
        return MakeMessage(L"Encoder unavailable", L"The NVIDIA hardware encoder could not be opened.",
                           L"Check GPU drivers. NVENC requires a supported NVIDIA GPU.");
    }

    if (phase == L"Prepare" && Contains(detail, L"NVENC AV1/NV12")) {
        return MakeMessage(L"Codec unsupported", L"This GPU does not support AV1 NVENC encoding.",
                           L"Update GPU drivers or check hardware requirements.");
    }

    if ((phase == L"Video Encoder" || phase == L"Prepare") &&
        ContainsAny(detail, std::array<std::wstring_view, 3>{L"NVENC init", L"NVENC encode", L"NVENC register"})) {
        return MakeMessage(L"Encoder error", L"The video encoder failed to initialize.",
                           L"Check available GPU memory and driver health.");
    }

    if (phase == L"Audio Capture" &&
        ContainsAny(detail, std::array<std::wstring_view, 2>{L"GetDevice", L"IMMDeviceEnumerator"})) {
        return MakeMessage(L"Microphone not found",
                           L"The selected microphone could not be found. It may have been disconnected.",
                           L"Refresh the device list, reconnect the microphone, or select a different input.");
    }

    if (phase == L"Audio Capture" && Contains(detail, L"process loopback")) {
        return MakeMessage(L"App audio unavailable", L"The application audio capture path could not be activated.",
                           L"Try reselecting the window. The target process may have no audio session.");
    }

    if (phase == L"Audio Capture") {
        return MakeMessage(L"Audio capture failed", L"An audio capture source failed to initialize.",
                           L"Check audio device connections.");
    }

    if (phase == L"Mux" && Contains(detail, L"Failed to open output file")) {
        return MakeMessage(L"Output write failed", L"The output file could not be opened for writing.",
                           L"Check available disk space and folder write permissions.");
    }

    if (phase == L"Mux" || phase == L"Finalize") {
        return MakeMessage(L"Write error", L"A file write error occurred while saving the recording.",
                           L"Check disk space.");
    }

    if (Contains(detail, L"output directory")) {
        return MakeMessage(L"Output folder error", L"The recording folder could not be created.",
                           L"Check folder permissions.");
    }

    if (phase == L"Shutdown") {
        return MakeMessage(L"Shutdown timeout",
                           L"Recording stopped but cleanup timed out. The file may still be complete.",
                           L"Check the log for further details.");
    }

    return MakeMessage(L"Recording failed", L"An unexpected error occurred during recording.",
                       L"Check the diagnostic log for details.");
}

} // namespace exosnap::diagnostics

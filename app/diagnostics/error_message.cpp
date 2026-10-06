#include "error_message.h"
#include <QCoreApplication>
#include <QString>
#include <capability/translatable.h>

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

bool ContainsAny(std::wstring_view haystack, const std::array<std::wstring_view, 4>& needles) {
    for (const auto needle : needles) {
        if (Contains(haystack, needle)) {
            return true;
        }
    }
    return false;
}

UiErrorMessage MakeMessage(const std::wstring& title, const std::wstring& message, const std::wstring& action_hint) {
    return UiErrorMessage{
        QCoreApplication::translate("RecordingErrors", QString::fromStdWString(title).toUtf8().constData())
            .toStdWString(),
        QCoreApplication::translate("RecordingErrors", QString::fromStdWString(message).toUtf8().constData())
            .toStdWString(),
        QCoreApplication::translate("RecordingErrors", QString::fromStdWString(action_hint).toUtf8().constData())
            .toStdWString(),
    };
}

} // namespace

UiErrorMessage MapErrorToUserMessage(const UiRecordingResult& result) {
    if (result.succeeded) {
        return MakeMessage(EXOSNAP_TRANSLATABLE("RecordingErrors", L"Recording complete"), {}, {});
    }

    const std::wstring_view phase = result.error_phase;
    const std::wstring_view detail = result.error_detail;

    if (Contains(detail, L"PID unavailable")) {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Window closed"),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"The selected window was closed before recording started."),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Reselect the window and try again."));
    }

    if (phase == L"Video Capture" && Contains(detail, L"handle invalid")) {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Window closed"),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"The selected window was closed before recording started."),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Reselect the window and try again."));
    }

    if (Contains(detail, L"too small for NV12") || Contains(detail, L"WGC source size invalid")) {
        return MakeMessage(EXOSNAP_TRANSLATABLE("RecordingErrors", L"Window too small"),
                           EXOSNAP_TRANSLATABLE("RecordingErrors", L"The window is too small to record."),
                           EXOSNAP_TRANSLATABLE("RecordingErrors", L"Resize or restore the window, then try again."));
    }

    if (phase == L"Prepare" && Contains(detail, L"NVENC open")) {
        // NV_ENC_ERR_OUT_OF_MEMORY on session open is how the driver reports that
        // its session budget is spent: another NVENC client (OBS, NVIDIA Instant
        // Replay, Discord streaming) holds it. A driver reinstall does nothing here.
        if (Contains(detail, L"NV_ENC_ERR_OUT_OF_MEMORY")) {
            return MakeMessage(
                EXOSNAP_TRANSLATABLE("RecordingErrors", L"Encoder is in use by another application"),
                EXOSNAP_TRANSLATABLE("RecordingErrors",
                                     L"The NVIDIA hardware encoder has no free session for this recording."),
                EXOSNAP_TRANSLATABLE("RecordingErrors",
                                     L"Close other applications that use NVENC (OBS Studio, NVIDIA Instant Replay / "
                                     L"ShadowPlay, Discord streaming, Xbox Game Bar capture), then start again."));
        }
        return MakeMessage(EXOSNAP_TRANSLATABLE("RecordingErrors", L"Encoder unavailable"),
                           EXOSNAP_TRANSLATABLE("RecordingErrors", L"The NVIDIA hardware encoder could not be opened."),
                           EXOSNAP_TRANSLATABLE("RecordingErrors",
                                                L"Make sure the display you record is driven by the NVIDIA GPU and "
                                                L"the driver is current. NVENC requires a supported NVIDIA GPU."));
    }

    if (phase == L"Prepare" && Contains(detail, L"NVENC AV1/NV12")) {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Codec unsupported"),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"This GPU does not support AV1 NVENC encoding."),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Update GPU drivers or check hardware requirements."));
    }

    if ((phase == L"Video Encoder" || phase == L"Prepare") &&
        ContainsAny(detail, std::array<std::wstring_view, 3>{L"NVENC init", L"NVENC encode", L"NVENC register"})) {
        return MakeMessage(EXOSNAP_TRANSLATABLE("RecordingErrors", L"Encoder error"),
                           EXOSNAP_TRANSLATABLE("RecordingErrors", L"The video encoder failed to initialize."),
                           EXOSNAP_TRANSLATABLE("RecordingErrors", L"Check available GPU memory and driver health."));
    }

    if (phase == L"Audio Capture" &&
        ContainsAny(detail, std::array<std::wstring_view, 2>{L"GetDevice", L"IMMDeviceEnumerator"})) {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Microphone not found"),
            EXOSNAP_TRANSLATABLE("RecordingErrors",
                                 L"The selected microphone could not be found. It may have been disconnected."),
            EXOSNAP_TRANSLATABLE("RecordingErrors",
                                 L"Refresh the device list, reconnect the microphone, or select a different input."));
    }

    if (phase == L"Audio Capture" && Contains(detail, L"process loopback")) {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"App audio unavailable"),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"The application audio capture path could not be activated."),
            EXOSNAP_TRANSLATABLE("RecordingErrors",
                                 L"Try reselecting the window. The target process may have no audio session."));
    }

    if (phase == L"Audio Capture") {
        return MakeMessage(EXOSNAP_TRANSLATABLE("RecordingErrors", L"Audio capture failed"),
                           EXOSNAP_TRANSLATABLE("RecordingErrors", L"An audio capture source failed to initialize."),
                           EXOSNAP_TRANSLATABLE("RecordingErrors", L"Check audio device connections."));
    }

    if (phase == L"Mux" && Contains(detail, L"Failed to open output file")) {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Output write failed"),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"The output file could not be opened for writing."),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Check available disk space and folder write permissions."));
    }

    if (phase == L"Mux" || phase == L"Finalize") {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Write error"),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"A file write error occurred while saving the recording."),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Check disk space."));
    }

    if (Contains(detail, L"unique output filename")) {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Filename collision"),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Could not create a unique filename in the output folder."),
            EXOSNAP_TRANSLATABLE(
                "RecordingErrors",
                L"Empty the output folder, change the filename pattern, or choose a different folder."));
    }

    if (ContainsAny(detail, std::array<std::wstring_view, 4>{L"output directory", L"output folder", L"Output folder",
                                                             L"Output directory"})) {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Output folder error"),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"The recording folder could not be created or written to."),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Check folder permissions and available disk space."));
    }

    if (phase == L"Shutdown") {
        return MakeMessage(
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Shutdown timeout"),
            EXOSNAP_TRANSLATABLE("RecordingErrors",
                                 L"Recording stopped but cleanup timed out. The file may still be complete."),
            EXOSNAP_TRANSLATABLE("RecordingErrors", L"Check the log for further details."));
    }

    return MakeMessage(EXOSNAP_TRANSLATABLE("RecordingErrors", L"Recording failed"),
                       EXOSNAP_TRANSLATABLE("RecordingErrors", L"An unexpected error occurred during recording."),
                       EXOSNAP_TRANSLATABLE("RecordingErrors", L"Check the diagnostic log for details."));
}

} // namespace exosnap::diagnostics

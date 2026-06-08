#include <gtest/gtest.h>

#include <utility>

#include "diagnostics/error_message.h"

namespace exosnap {
namespace {

UiRecordingResult MakeFailure(std::wstring phase, std::wstring detail) {
    UiRecordingResult result;
    result.succeeded = false;
    result.error_phase = std::move(phase);
    result.error_detail = std::move(detail);
    return result;
}

TEST(ErrorMessageTest, MapErrorToUserMessage_Success) {
    UiRecordingResult result;
    result.succeeded = true;

    const auto msg = diagnostics::MapErrorToUserMessage(result);

    EXPECT_EQ(msg.title, L"Recording complete");
    EXPECT_TRUE(msg.message.empty());
    EXPECT_TRUE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_PidUnavailable_WindowClosed) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Prepare", L"PID unavailable for target"));

    EXPECT_EQ(msg.title, L"Window closed");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_VideoHandleInvalid_WindowClosed) {
    const auto msg =
        diagnostics::MapErrorToUserMessage(MakeFailure(L"Video Capture", L"window handle invalid at activation"));

    EXPECT_EQ(msg.title, L"Window closed");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_WindowTooSmall) {
    const auto msg =
        diagnostics::MapErrorToUserMessage(MakeFailure(L"Prepare", L"WGC source size invalid for capture path"));

    EXPECT_EQ(msg.title, L"Window too small");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_NvencOpen_EncoderUnavailable) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Prepare", L"NVENC open failed"));

    EXPECT_EQ(msg.title, L"Encoder unavailable");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_NvencAv1Unsupported_CodecUnsupported) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Prepare", L"NVENC AV1/NV12 not supported"));

    EXPECT_EQ(msg.title, L"Codec unsupported");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_NvencInit_EncoderError) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Video Encoder", L"NVENC init failed"));

    EXPECT_EQ(msg.title, L"Encoder error");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_MicrophoneGetDevice_NotFound) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Audio Capture", L"GetDevice returned E_FAIL"));

    EXPECT_EQ(msg.title, L"Microphone not found");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_ProcessLoopback_AppAudioUnavailable) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Audio Capture", L"process loopback failed"));

    EXPECT_EQ(msg.title, L"App audio unavailable");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_GenericAudioCapture) {
    const auto msg =
        diagnostics::MapErrorToUserMessage(MakeFailure(L"Audio Capture", L"unexpected audio initialization failure"));

    EXPECT_EQ(msg.title, L"Audio capture failed");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_OutputFileOpen_OutputWriteFailed) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Mux", L"Failed to open output file for writing"));

    EXPECT_EQ(msg.title, L"Output write failed");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_Finalize_WriteError) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Finalize", L"Finalize write failed"));

    EXPECT_EQ(msg.title, L"Write error");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_ShutdownTimeout) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Shutdown", L"timed out waiting for stop"));

    EXPECT_EQ(msg.title, L"Shutdown timeout");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_UnknownFallback) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Prepare", L"unknown failure"));

    EXPECT_EQ(msg.title, L"Recording failed");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_OutputFolderInvalid) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Prepare", L"Output folder path is invalid."));

    EXPECT_EQ(msg.title, L"Output folder error");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_OutputFolderNotWritable) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Prepare", L"Output folder is not writable."));

    EXPECT_EQ(msg.title, L"Output folder error");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_OutputFolderCreationFailed) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(L"Prepare", L"Failed to create output folder."));

    EXPECT_EQ(msg.title, L"Output folder error");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_OutputDirectoryCreateFailed) {
    const auto msg = diagnostics::MapErrorToUserMessage(
        MakeFailure(L"Prepare", L"Failed to create output directory: permission denied"));

    EXPECT_EQ(msg.title, L"Output folder error");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_UniqueFilenameExhausted) {
    const auto msg = diagnostics::MapErrorToUserMessage(MakeFailure(
        L"Prepare", L"Could not create a unique output filename. Change the filename pattern or output folder."));

    EXPECT_EQ(msg.title, L"Filename collision");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

TEST(ErrorMessageTest, MapErrorToUserMessage_MuxOpenFailed) {
    const auto msg =
        diagnostics::MapErrorToUserMessage(MakeFailure(L"Mux", L"Failed to open output file: permission denied"));

    EXPECT_EQ(msg.title, L"Output write failed");
    EXPECT_FALSE(msg.message.empty());
    EXPECT_FALSE(msg.action_hint.empty());
}

} // namespace
} // namespace exosnap

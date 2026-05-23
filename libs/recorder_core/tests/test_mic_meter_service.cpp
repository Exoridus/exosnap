#include <gtest/gtest.h>

#include "mic_meter_service.h"

namespace recorder_core {
namespace {

TEST(MicMeterServiceTest, StopWithoutStart_IsSafe) {
    MicMeterService service;
    EXPECT_FALSE(service.IsRunning());
    service.Stop();
    EXPECT_FALSE(service.IsRunning());
}

TEST(MicMeterServiceTest, InvalidDeviceIdStart_FailsAndKeepsStoppedState) {
    MicMeterService service;

    std::string error;
    const bool started = service.Start(
        std::optional<std::string>("__exosnap_invalid_mic_device_id__"), MicChannelMode::Auto,
        [](float) {
            // No-op
        },
        error);

    EXPECT_FALSE(started);
    EXPECT_FALSE(service.IsRunning());
    EXPECT_FALSE(error.empty());
}

} // namespace
} // namespace recorder_core

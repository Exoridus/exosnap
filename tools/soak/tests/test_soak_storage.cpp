#include "soak_storage.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

namespace {

exosnap::soak::StorageWatchConfig Config() {
    return {600.0, 60.0, 300, 500, 100, false};
}

exosnap::soak::StorageObservation Observation(double time, uint64_t free = 100000, uint64_t bytes = 0) {
    return {time, free, 100000, bytes};
}

} // namespace

TEST(SoakStorage, SafeStartupAndGrowthContinueWithProjection) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0), std::nullopt);
    ASSERT_TRUE(first.stop_reason.empty()) << first.stop_reason;
    const auto next = exosnap::soak::CheckStorage(Config(), Observation(60, 99400, 600), first);
    EXPECT_TRUE(next.stop_reason.empty()) << next.stop_reason;
    EXPECT_EQ(next.growth_bytes, 600u);
    EXPECT_DOUBLE_EQ(next.average_bytes_per_second, 10.0);
    EXPECT_DOUBLE_EQ(next.peak_bytes_per_second, 10.0);
    EXPECT_EQ(next.projected_remaining_bytes, 13200u);
}

TEST(SoakStorage, ReserveIncludesFinalizationAndStopsAtTheBoundary) {
    EXPECT_FALSE(exosnap::soak::CheckStorage(Config(), Observation(0, 400), std::nullopt).stop_reason.empty());
    EXPECT_TRUE(exosnap::soak::CheckStorage(Config(), Observation(0, 401), std::nullopt).stop_reason.empty());
}

TEST(SoakStorage, ProjectedWritesStopBeforeTheReserveIsReached) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0), std::nullopt);
    const auto next = exosnap::soak::CheckStorage(Config(), Observation(60, 13000, 600), first);
    EXPECT_NE(next.stop_reason.find("projected"), std::string::npos);
}

TEST(SoakStorage, BitrateIncreaseUsesThePeakNotOnlyTheLifetimeMean) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0), std::nullopt);
    const auto next = exosnap::soak::CheckStorage(Config(), Observation(60, 99400, 600), first);
    const auto peak = exosnap::soak::CheckStorage(Config(), Observation(120, 20000, 1800), next);
    EXPECT_DOUBLE_EQ(peak.average_bytes_per_second, 15.0);
    EXPECT_DOUBLE_EQ(peak.peak_bytes_per_second, 20.0);
    EXPECT_GE(peak.projected_remaining_bytes, 24000u);
    EXPECT_FALSE(peak.stop_reason.empty());
}

TEST(SoakStorage, SystemReserveIsCheckedWhenOutputIsOnAnotherVolume) {
    auto observation = Observation(0);
    observation.system_free_bytes = 600;
    EXPECT_FALSE(exosnap::soak::CheckStorage(Config(), observation, std::nullopt).stop_reason.empty());
}

TEST(SoakStorage, MissingStorageOrFileObservationCannotContinue) {
    for (int missing = 0; missing < 3; ++missing) {
        auto observation = Observation(0);
        if (missing == 0)
            observation.output_free_bytes.reset();
        else if (missing == 1)
            observation.system_free_bytes.reset();
        else
            observation.file_bytes.reset();
        EXPECT_FALSE(exosnap::soak::CheckStorage(Config(), observation, std::nullopt).stop_reason.empty()) << missing;
    }
}

TEST(SoakStorage, ADisappearingOrTruncatedOutputStops) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0, 100000, 1000), std::nullopt);
    EXPECT_FALSE(exosnap::soak::CheckStorage(Config(), Observation(60, 100000, 0), first).stop_reason.empty());
}

TEST(SoakStorage, InvalidTimeAndOverflowingProjectionStopSafely) {
    auto config = Config();
    config.target_seconds = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(exosnap::soak::CheckStorage(config, Observation(0), std::nullopt).stop_reason.empty());
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0), std::nullopt);
    EXPECT_FALSE(exosnap::soak::CheckStorage(Config(), Observation(0), first).stop_reason.empty());
    const auto huge = exosnap::soak::CheckStorage(
        Config(), Observation(1, std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max()), first);
    EXPECT_EQ(huge.projected_remaining_bytes, std::numeric_limits<uint64_t>::max());
    EXPECT_FALSE(huge.stop_reason.empty());
}

TEST(SoakStorage, CompetingOutputVolumeWritesAreIncludedInTheProjection) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0), std::nullopt);
    const auto next = exosnap::soak::CheckStorage(Config(), Observation(60, 10000), first);
    EXPECT_FALSE(next.stop_reason.empty());
}

TEST(SoakStorage, CompetingSystemVolumeWritesCannotConsumeTheSystemReserveUnnoticed) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0), std::nullopt);
    auto observation = Observation(60);
    observation.system_free_bytes = 10000;
    const auto next = exosnap::soak::CheckStorage(Config(), observation, first);
    EXPECT_FALSE(next.stop_reason.empty());
}

TEST(SoakStorage, AcceptedNonDividingDiagnosticIntervalStillWakesAtTheStorageDeadline) {
    auto config = Config();
    const auto last = Observation(0);
    const double diagnostic_interval = 59.0;
    const double first_wait = std::min(diagnostic_interval, exosnap::soak::StoragePollDelay(config, last, 0));
    EXPECT_DOUBLE_EQ(first_wait, 59.0);
    const double second_wait = std::min(diagnostic_interval, exosnap::soak::StoragePollDelay(config, last, first_wait));
    EXPECT_DOUBLE_EQ(second_wait, 1.0);
    EXPECT_DOUBLE_EQ(first_wait + second_wait, config.poll_seconds);
    EXPECT_DOUBLE_EQ(exosnap::soak::StoragePollDelay(config, last, 65.0), 0.0);
}

TEST(SoakStorage, TransientCompetingWritesRetainTheirPeakOnlyForStopResponseHeadroom) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0), std::nullopt);
    const auto burst = exosnap::soak::CheckStorage(Config(), Observation(60, 88000, 600), first);
    EXPECT_TRUE(burst.stop_reason.empty()) << burst.stop_reason;
    EXPECT_DOUBLE_EQ(burst.output_consumption_peak_bytes_per_second, 200.0);
    EXPECT_EQ(burst.projected_remaining_bytes, 58800u);
    const auto recovered = exosnap::soak::CheckStorage(Config(), Observation(120, 100000, 1200), burst);
    EXPECT_TRUE(recovered.stop_reason.empty()) << recovered.stop_reason;
    EXPECT_DOUBLE_EQ(recovered.output_consumption_peak_bytes_per_second, 200.0);
    EXPECT_EQ(recovered.projected_remaining_bytes, 57600u);
}

TEST(SoakStorage, MediaGrowthIsProjectedForTheEntireRemainingDurationEvenWhenFreeSpaceRecovers) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0), std::nullopt);
    const auto next = exosnap::soak::CheckStorage(Config(), Observation(60, 100000, 6000), first);
    EXPECT_EQ(next.projected_remaining_bytes, 132000u);
    EXPECT_FALSE(next.stop_reason.empty());
}

TEST(SoakStorage, SustainedCompetingWritesStopWithPollDrainAndFinalizationSpaceStillAvailable) {
    auto config = Config();
    config.target_seconds = 3600;
    auto check = exosnap::soak::CheckStorage(config, Observation(0, 1000000), std::nullopt);
    for (int minute = 1; minute <= 12; ++minute) {
        check = exosnap::soak::CheckStorage(config, Observation(minute * 60, 1000000 - minute * 60000, minute * 600),
                                            check);
        if (minute < 12)
            ASSERT_TRUE(check.stop_reason.empty()) << minute << ": " << check.stop_reason;
    }
    EXPECT_FALSE(check.stop_reason.empty());
    EXPECT_GT(*check.observation.output_free_bytes,
              config.output_reserve_bytes + config.finalization_reserve_bytes + 240000u);
}

TEST(SoakStorage, SeparateSystemVolumeProjectsOnlyItsCompetingWritesDuringStopResponse) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0), std::nullopt);
    auto observation = Observation(60, 99400, 600);
    observation.system_free_bytes = 88000;
    const auto next = exosnap::soak::CheckStorage(Config(), observation, first);
    EXPECT_TRUE(next.stop_reason.empty()) << next.stop_reason;
    EXPECT_EQ(next.projected_remaining_bytes, 13200u);
    EXPECT_EQ(next.system_projected_remaining_bytes, 48000u);
}

TEST(SoakStorage, SharedSystemVolumeRetainsTheMediaRateEvenWhenVolumeFreeSpaceRecovers) {
    auto config = Config();
    config.output_is_system_volume = true;
    const auto first = exosnap::soak::CheckStorage(config, Observation(0), std::nullopt);
    const auto next = exosnap::soak::CheckStorage(config, Observation(60, 100000, 600), first);
    EXPECT_EQ(next.projected_remaining_bytes, 13200u);
    EXPECT_EQ(next.system_projected_remaining_bytes, next.projected_remaining_bytes);
}

TEST(SoakStorage, ProjectedWritesAndReservesRequireOneByteOfAdditionalSpace) {
    const auto first = exosnap::soak::CheckStorage(Config(), Observation(0, 1000), std::nullopt);
    EXPECT_FALSE(exosnap::soak::CheckStorage(Config(), Observation(60, 13600, 600), first).stop_reason.empty());
    EXPECT_TRUE(exosnap::soak::CheckStorage(Config(), Observation(60, 13601, 600), first).stop_reason.empty());
}

TEST(SoakStorage, CombinedProjectionSaturatesBeforeConvertingToBytes) {
    auto config = Config();
    config.target_seconds = 3;
    config.poll_seconds = 1;
    const auto maximum = std::numeric_limits<uint64_t>::max();
    auto previous = exosnap::soak::CheckStorage(config, Observation(0, maximum), std::nullopt);
    const auto rate = static_cast<double>(maximum) / 124.0;
    previous.peak_bytes_per_second = rate;
    previous.output_consumption_peak_bytes_per_second = rate;
    const auto next = exosnap::soak::CheckStorage(config, Observation(1, maximum), previous);
    EXPECT_EQ(next.projected_remaining_bytes, maximum);
    EXPECT_FALSE(next.stop_reason.empty());
}

#include <gtest/gtest.h>

#include "clapper_schedule.h"

namespace exosnap::soak {
namespace {

TEST(ClapperScheduleTest, KeepsLegacyTwoMarkerTiming) {
    ClapperSchedule schedule;
    std::string error;
    ASSERT_TRUE(BuildClapperSchedule(120, 2, 0, 0, schedule, error)) << error;
    EXPECT_EQ(schedule.marker_seconds, (std::vector<std::int64_t>{0, 120}));
}

TEST(ClapperScheduleTest, BuildsTwoAndThreeHourAcceptanceSchedules) {
    ClapperSchedule two_hours;
    ClapperSchedule three_hours;
    std::string error;
    ASSERT_TRUE(BuildClapperSchedule(7200, 3, 10, 10, two_hours, error)) << error;
    ASSERT_TRUE(BuildClapperSchedule(10800, 3, 10, 10, three_hours, error)) << error;
    EXPECT_EQ(two_hours.marker_seconds, (std::vector<std::int64_t>{10, 3600, 7190}));
    EXPECT_EQ(three_hours.marker_seconds, (std::vector<std::int64_t>{10, 5400, 10790}));
}

TEST(ClapperScheduleTest, RejectsInvalidSchedules) {
    ClapperSchedule schedule;
    std::string error;
    EXPECT_FALSE(BuildClapperSchedule(0, 3, 0, 0, schedule, error));
    EXPECT_FALSE(BuildClapperSchedule(-1, 3, 0, 0, schedule, error));
    EXPECT_FALSE(BuildClapperSchedule(10, 1, 0, 0, schedule, error));
    EXPECT_FALSE(BuildClapperSchedule(10, 4, 0, 0, schedule, error));
    EXPECT_FALSE(BuildClapperSchedule(20, 3, 10, 10, schedule, error));
    EXPECT_FALSE(BuildClapperSchedule(20, 3, 11, 0, schedule, error));
}

TEST(ClapperScheduleTest, ParsesPositiveDurationsFailClosed) {
    std::int64_t parsed = 0;
    std::string error;
    EXPECT_TRUE(ParsePositiveInt64("7200", parsed, error));
    EXPECT_EQ(parsed, 7200);
    EXPECT_TRUE(ParsePositiveInt64("10800", parsed, error));
    EXPECT_EQ(parsed, 10800);

    for (const char* invalid : {"", "0", "-1", "12s", "1.5", "999999999999999999999999"}) {
        error.clear();
        EXPECT_FALSE(ParsePositiveInt64(invalid, parsed, error)) << invalid;
        EXPECT_FALSE(error.empty());
    }
}

} // namespace
} // namespace exosnap::soak

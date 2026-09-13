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

TEST(ClapperScheduleTest, SpreadsMoreThanThreeMarkersEvenly) {
    // The drift fit's precision comes from the marker count, not from the length
    // of the run, so a budget three markers cannot carry is reached by asking for
    // more of them. They have to be spread: markers bunched at one end carry less
    // than the same number across the span.
    ClapperSchedule schedule;
    std::string error;
    ASSERT_TRUE(BuildClapperSchedule(800, 9, 0, 0, schedule, error)) << error;
    EXPECT_EQ(schedule.marker_seconds,
              (std::vector<std::int64_t>{0, 100, 200, 300, 400, 500, 600, 700, 800}));
}

TEST(ClapperScheduleTest, RejectsInvalidSchedules) {
    ClapperSchedule schedule;
    std::string error;
    EXPECT_FALSE(BuildClapperSchedule(0, 3, 0, 0, schedule, error));
    EXPECT_FALSE(BuildClapperSchedule(-1, 3, 0, 0, schedule, error));
    EXPECT_FALSE(BuildClapperSchedule(10, 1, 0, 0, schedule, error));
    EXPECT_FALSE(BuildClapperSchedule(20, 3, 10, 10, schedule, error));
    // More markers than the span has whole seconds: two would land on the same
    // second, and a marker pair the analysis cannot separate is worse than one
    // fewer marker.
    EXPECT_FALSE(BuildClapperSchedule(4, 6, 0, 0, schedule, error));
}

TEST(ClapperScheduleTest, PlacesInnerMarkersInsideTheMargins) {
    // The middle marker used to be total/2 regardless of the margins, so a large
    // start margin put it BEFORE the first marker and the schedule was rejected
    // as unordered -- a valid request reported as a bad one. Inner markers are
    // spaced across the measurable span, so the same request now succeeds.
    ClapperSchedule schedule;
    std::string error;
    ASSERT_TRUE(BuildClapperSchedule(20, 3, 11, 0, schedule, error)) << error;
    EXPECT_EQ(schedule.marker_seconds, (std::vector<std::int64_t>{11, 15, 20}));
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

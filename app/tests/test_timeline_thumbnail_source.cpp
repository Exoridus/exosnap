#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QImage>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "services/TimelineThumbnailSource.h"

namespace exosnap {
namespace {

// QImage scaling needs a QGuiApplication for its image-format plumbing on some
// platforms; one per process is enough (each CTest entry is one binary).
QGuiApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "timeline_thumbnail_source_tests";
    static char* argv[] = {app_name, nullptr};
    static QGuiApplication app(argc, argv);
    return &app;
}

class TimelineThumbnailSourceTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// A decoded frame whose buffer reports its own lifetime, so a test can prove
// the tile loop is not sitting on the frames it has already scaled.
recorder_core::DecodedVideoFrame MakeFrame(int64_t pts_us, uint32_t width, uint32_t height,
                                           std::atomic<int>* live_buffers) {
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    auto* raw = new uint8_t[bytes];
    for (size_t i = 0; i < bytes; ++i)
        raw[i] = static_cast<uint8_t>(i & 0xFF);
    if (live_buffers != nullptr)
        live_buffers->fetch_add(1);
    recorder_core::DecodedVideoFrame frame;
    frame.pts_us = pts_us;
    frame.width = width;
    frame.height = height;
    frame.stride_bytes = width * 4;
    frame.bgra = std::shared_ptr<const uint8_t[]>(raw, [live_buffers](const uint8_t* p) {
        delete[] p;
        if (live_buffers != nullptr)
            live_buffers->fetch_sub(1);
    });
    return frame;
}

std::vector<int64_t> KeyframesEveryMs(int64_t step_ms, int count) {
    std::vector<int64_t> out;
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        out.push_back(step_ms * 1000 * i);
    return out;
}

} // namespace

// ---- Strip layout ----

TEST_F(TimelineThumbnailSourceTest, TileWidthFollowsTheRecordingsAspectRatio) {
    // 40 px row, 16:9 recording -> ~71 px wide, the width the design assumes.
    EXPECT_EQ(TimelineTileWidth(40, 2560, 1440), 71);
    // Portrait is narrower, ultrawide wider, at the same row height.
    EXPECT_LT(TimelineTileWidth(40, 1080, 1920), TimelineTileWidth(40, 2560, 1440));
    EXPECT_GT(TimelineTileWidth(40, 2560, 1080), TimelineTileWidth(40, 2560, 1440));
    // An unreported frame size falls back to 16:9, not to a square.
    EXPECT_EQ(TimelineTileWidth(40, 0, 0), TimelineTileWidth(40, 1920, 1080));
}

TEST_F(TimelineThumbnailSourceTest, TileCountFollowsTheAspectRatioAtEqualWidth) {
    constexpr int kRowH = 40;
    constexpr int kTrackW = 800;
    const int portrait = TimelineTileCount(kTrackW, TimelineTileWidth(kRowH, 1080, 1920));
    const int wide = TimelineTileCount(kTrackW, TimelineTileWidth(kRowH, 2560, 1440));
    const int ultrawide = TimelineTileCount(kTrackW, TimelineTileWidth(kRowH, 2560, 1080));

    EXPECT_GT(portrait, wide);
    EXPECT_GT(wide, ultrawide);
    // A partial last tile is never counted.
    EXPECT_LE(wide * TimelineTileWidth(kRowH, 2560, 1440), kTrackW);
}

TEST_F(TimelineThumbnailSourceTest, TilePositionsSpreadEvenlyAcrossTheClip) {
    // Keyframes every 250 ms: fine enough that snapping does not move a tile.
    const std::vector<int64_t> keyframes = KeyframesEveryMs(250, 41);

    const std::vector<qint64> five = TimelineTileTimesMs(5, 10000, keyframes);
    ASSERT_EQ(five.size(), 5u);
    EXPECT_EQ(five[0], 0);
    EXPECT_EQ(five[1], 2000);
    EXPECT_EQ(five[2], 4000);
    EXPECT_EQ(five[3], 6000);
    EXPECT_EQ(five[4], 8000);

    // A wider track means more, closer-spaced tiles over the same clip.
    const std::vector<qint64> ten = TimelineTileTimesMs(10, 10000, keyframes);
    ASSERT_EQ(ten.size(), 10u);
    EXPECT_EQ(ten[0], 0);
    EXPECT_EQ(ten[1], 1000);
    EXPECT_EQ(ten[9], 9000);
}

TEST_F(TimelineThumbnailSourceTest, TilePositionsSnapToTheNearestKeyframe) {
    // Keyframes every 4 s: the nominal positions of a 4-tile strip over 10 s
    // (0, 2500, 5000, 7500) all land between two of them.
    const std::vector<int64_t> keyframes = {0, 4'000'000, 8'000'000};
    const std::vector<qint64> times = TimelineTileTimesMs(4, 10000, keyframes);

    // 2500 -> 4000 (nearest), 5000 -> 4000 (merged into the previous tile),
    // 7500 -> 8000. Two tiles of the same frame would claim detail the clip
    // does not have, so the duplicate collapses.
    ASSERT_EQ(times.size(), 3u);
    EXPECT_EQ(times[0], 0);
    EXPECT_EQ(times[1], 4000);
    EXPECT_EQ(times[2], 8000);
}

TEST_F(TimelineThumbnailSourceTest, NoTilesWithoutATrackOrADuration) {
    EXPECT_TRUE(TimelineTileTimesMs(0, 10000, {}).empty());
    EXPECT_TRUE(TimelineTileTimesMs(8, 0, {}).empty());
    EXPECT_EQ(TimelineTileCount(0, 71), 0);
    EXPECT_EQ(TimelineTileCount(800, 0), 0);
}

// ---- Tile generation ----

TEST_F(TimelineThumbnailSourceTest, EveryTileIsScaledToTheRowHeight) {
    constexpr int kRowH = 40;
    std::vector<TimelineThumbnail> tiles;
    std::atomic<bool> cancelled{false};

    GenerateTimelineTiles(
        {0, 1000, 2000}, kRowH,
        [](int64_t target_us) { return std::optional(MakeFrame(target_us, 320, 180, nullptr)); },
        [&tiles](TimelineThumbnail&& tile) { tiles.push_back(std::move(tile)); }, cancelled);

    ASSERT_EQ(tiles.size(), 3u);
    for (size_t i = 0; i < tiles.size(); ++i) {
        EXPECT_EQ(tiles[i].image.height(), kRowH);
        // 16:9 source at 40 px -> 71 px, i.e. the aspect survived the scale.
        EXPECT_EQ(tiles[i].image.width(), 71);
        EXPECT_EQ(tiles[i].time_ms, static_cast<qint64>(i) * 1000);
    }
}

TEST_F(TimelineThumbnailSourceTest, AFailedDecodeLeavesTheRemainingTilesAlone) {
    std::vector<TimelineThumbnail> tiles;
    std::atomic<bool> cancelled{false};

    GenerateTimelineTiles(
        {0, 1000, 2000, 3000}, 40,
        [](int64_t target_us) -> std::optional<recorder_core::DecodedVideoFrame> {
            if (target_us == 1'000'000)
                return std::nullopt; // this position carries nothing decodable
            return MakeFrame(target_us, 320, 180, nullptr);
        },
        [&tiles](TimelineThumbnail&& tile) { tiles.push_back(std::move(tile)); }, cancelled);

    // The failed position simply has no tile; the run does not stop there.
    ASSERT_EQ(tiles.size(), 3u);
    EXPECT_EQ(tiles[0].time_ms, 0);
    EXPECT_EQ(tiles[1].time_ms, 2000);
    EXPECT_EQ(tiles[2].time_ms, 3000);
}

TEST_F(TimelineThumbnailSourceTest, FullSizeFramesAreReleasedBeforeTheNextDecode) {
    // At 1440p a retained full frame is ~15 MB, so a strip that holds all of
    // them costs more than the player itself. Prove none survive its tile.
    std::atomic<int> live_buffers{0};
    int peak_live = 0;
    int tiles = 0;
    std::atomic<bool> cancelled{false};

    GenerateTimelineTiles(
        {0, 1000, 2000, 3000, 4000, 5000}, 40,
        [&live_buffers, &peak_live](int64_t target_us) {
            peak_live = std::max(peak_live, live_buffers.load());
            return std::optional(MakeFrame(target_us, 640, 360, &live_buffers));
        },
        [&live_buffers, &tiles](TimelineThumbnail&& tile) {
            ++tiles;
            // The frame this tile came from is already gone by the time the
            // tile is handed over.
            EXPECT_EQ(live_buffers.load(), 0);
            EXPECT_FALSE(tile.image.isNull());
        },
        cancelled);

    EXPECT_EQ(tiles, 6);
    EXPECT_EQ(live_buffers.load(), 0);
    // No decode ever started while a previous frame was still held.
    EXPECT_EQ(peak_live, 0);
}

TEST_F(TimelineThumbnailSourceTest, ACancelledRunStopsDecoding) {
    // A resize storm must not be able to stack runs: an in-flight one has to
    // give up as soon as its successor is queued.
    std::atomic<bool> cancelled{false};
    int decodes = 0;
    std::vector<TimelineThumbnail> tiles;

    GenerateTimelineTiles(
        {0, 1000, 2000, 3000, 4000}, 40,
        [&decodes, &cancelled](int64_t target_us) {
            ++decodes;
            if (decodes == 2)
                cancelled.store(true);
            return std::optional(MakeFrame(target_us, 320, 180, nullptr));
        },
        [&tiles](TimelineThumbnail&& tile) { tiles.push_back(std::move(tile)); }, cancelled);

    EXPECT_EQ(decodes, 2);
    EXPECT_EQ(tiles.size(), 2u);
}

TEST_F(TimelineThumbnailSourceTest, WrappingADecodedFrameDoesNotCopyItsBuffer) {
    std::atomic<int> live_buffers{0};
    {
        const recorder_core::DecodedVideoFrame frame = MakeFrame(0, 64, 36, &live_buffers);
        ASSERT_EQ(live_buffers.load(), 1);
        {
            const QImage wrapped = WrapDecodedFrame(frame);
            EXPECT_EQ(wrapped.width(), 64);
            EXPECT_EQ(wrapped.height(), 36);
            // The wrapper points straight at the decoder's own allocation.
            EXPECT_EQ(wrapped.constBits(), frame.bgra.get());
        }
        EXPECT_EQ(live_buffers.load(), 1); // the frame still owns it
    }
    EXPECT_EQ(live_buffers.load(), 0);
}

// A QImage cannot be built on geometry it has no way to describe, and a null
// QImage never runs its cleanup hook. Allocating the keep-alive first therefore
// stranded both the wrapper and the decoder's whole frame buffer -- ~33 MB at
// 4K, once per unusable frame, for the life of the process.
TEST_F(TimelineThumbnailSourceTest, AnUnusableFrameGeometryStrandsNothing) {
    std::atomic<int> live_buffers{0};

    const auto expect_released = [&live_buffers](recorder_core::DecodedVideoFrame frame) {
        {
            const QImage wrapped = WrapDecodedFrame(frame);
            EXPECT_TRUE(wrapped.isNull());
        }
        // Only the frame itself still holds the buffer; the wrapper kept nothing.
        EXPECT_EQ(live_buffers.load(), 1);
    };

    {
        recorder_core::DecodedVideoFrame zero_width = MakeFrame(0, 0, 36, &live_buffers);
        zero_width.stride_bytes = 0;
        expect_released(std::move(zero_width));
    }
    EXPECT_EQ(live_buffers.load(), 0);

    {
        recorder_core::DecodedVideoFrame zero_height = MakeFrame(0, 64, 0, &live_buffers);
        expect_released(std::move(zero_height));
    }
    EXPECT_EQ(live_buffers.load(), 0);

    {
        // A stride that cannot hold one row: QImage refuses it, and so must the
        // wrapper -- accepting it would read past the allocation.
        recorder_core::DecodedVideoFrame short_stride = MakeFrame(0, 64, 36, &live_buffers);
        short_stride.stride_bytes = 64 * 4 - 4;
        expect_released(std::move(short_stride));
    }
    EXPECT_EQ(live_buffers.load(), 0);

    {
        recorder_core::DecodedVideoFrame no_buffer = MakeFrame(0, 64, 36, &live_buffers);
        no_buffer.bgra.reset();
        {
            const QImage wrapped = WrapDecodedFrame(no_buffer);
            EXPECT_TRUE(wrapped.isNull());
        }
    }
    EXPECT_EQ(live_buffers.load(), 0);
}

// The clip must be handed back to the user when Edit closes: as long as the
// worker's engine holds the container open, Windows refuses to delete or rename
// the recording. Needs real media, so it runs against an untracked fixture at the
// path below and skips where there is none. Create one with
// `ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60:duration=6 -c:v libx264
// -g 120 -pix_fmt yuv420p <that path>`.
// Measured while writing this: the copy cannot be deleted while the clip is
// open, and can be immediately after the close is processed.
TEST_F(TimelineThumbnailSourceTest, ClosingTheClipReleasesTheFileHandle) {
    const std::filesystem::path fixture =
        std::filesystem::path(EXOSNAP_SOURCE_DIR) / ".workspace" / "test-fixtures" / "edit_handle_probe.mkv";
    if (!std::filesystem::exists(fixture))
        GTEST_SKIP() << "fixture not present on this host: " << fixture.string();

    // A copy, so a failed run cannot cost the fixture itself.
    const std::filesystem::path probe = std::filesystem::temp_directory_path() / "exosnap_edit_handle_probe.mkv";
    std::error_code ec;
    std::filesystem::remove(probe, ec);
    ASSERT_TRUE(std::filesystem::copy_file(fixture, probe, std::filesystem::copy_options::overwrite_existing, ec))
        << ec.message();

    {
        TimelineThumbnailSource source;
        source.openClip(QString::fromStdString(probe.string()), {0, 2'000'000});
        for (int i = 0; i < 400 && source.videoWidth() == 0; ++i) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ASSERT_GT(source.videoWidth(), 0) << "the fixture did not open";

        source.closeClip();
        bool released = false;
        for (int i = 0; i < 400 && !released; ++i) {
            QCoreApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            std::filesystem::remove(probe, ec);
            released = !std::filesystem::exists(probe);
        }
        // Still inside the source's lifetime: the destructor must not be what
        // releases the file.
        EXPECT_TRUE(released);
    }
    std::filesystem::remove(probe, ec);
}

// ---- Worker ----

TEST_F(TimelineThumbnailSourceTest, AClipThatCannotBeOpenedYieldsNoTiles) {
    TimelineThumbnailSource source;
    source.openClip(QStringLiteral("C:\\this\\path\\does\\not\\exist.mkv"), {0, 1'000'000});
    const quint64 run = source.requestTiles({0, 1000}, 40);
    EXPECT_GT(run, 0u);

    // The worker must simply report an unopened clip and stay idle -- no tiles,
    // no crash, and a destructor that joins cleanly.
    QCoreApplication::processEvents();
    EXPECT_EQ(source.videoWidth(), 0);
    EXPECT_EQ(source.videoHeight(), 0);
}

TEST_F(TimelineThumbnailSourceTest, ClosingTheClipIsIdempotentAndForgetsTheFrameSize) {
    // Closing the Edit surface has to hand the recording back to the user: as
    // long as the worker's engine holds the container open, the file cannot be
    // moved, renamed or deleted.
    TimelineThumbnailSource source;
    source.openClip(QStringLiteral("C:\\this\\path\\does\\not\\exist.mkv"), {0, 1'000'000});
    source.requestTiles({0, 1000}, 40);

    source.closeClip();
    source.closeClip(); // a second close is a no-op, not a second teardown

    QCoreApplication::processEvents();
    EXPECT_EQ(source.videoWidth(), 0);
    EXPECT_EQ(source.videoHeight(), 0);
}

TEST_F(TimelineThumbnailSourceTest, EachRequestGetsItsOwnRunId) {
    TimelineThumbnailSource source;
    const quint64 first = source.requestTiles({0}, 40);
    const quint64 second = source.requestTiles({0, 1000}, 40);
    EXPECT_NE(first, second);
    source.cancel();
}

} // namespace exosnap

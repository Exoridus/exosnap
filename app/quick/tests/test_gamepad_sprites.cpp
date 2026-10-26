#include "GamepadOverlay.h"
#include "GamepadSprites.h"

#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>

using namespace exosnap::quick;

namespace exosnap::quick {

class GamepadOverlayTest : public ::testing::Test {
  protected:
    static void Seed(GamepadOverlay& overlay, std::shared_ptr<const GamepadSpriteSet> sprites) {
        overlay.settings_.enabled = true;
        overlay.sprites_ = std::move(sprites);
        ++overlay.revision_;
    }
    static void Poll(GamepadOverlay& overlay) {
        overlay.Poll();
    }
    static void Rebuild(GamepadOverlay& overlay) {
        overlay.Rebuild();
    }
    static void WaitRaster(GamepadOverlay& overlay) {
        overlay.raster_pool_.waitForDone();
    }
};

TEST_F(GamepadOverlayTest, RecorderSnapshotDoesNotConsumePreviewNotification) {
    int argc = 1;
    char name[] = "gamepad-tests";
    char* argv[]{name, nullptr};
    QCoreApplication application(argc, argv);
    QString error;
    auto sprites =
        RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/dualsense.svg"), 480, false, false, error);
    ASSERT_TRUE(sprites) << error.toStdString();
    GamepadOverlay overlay;
    Seed(overlay, std::move(sprites));
    int notifications = 0;
    QObject::connect(&overlay, &GamepadOverlay::frameChanged, [&]() { ++notifications; });
    Poll(overlay);
    ASSERT_EQ(notifications, 1);
    overlay.setContentSize(QSize(800, 800));
    ASSERT_TRUE(overlay.Snapshot(0));
    Poll(overlay);
    EXPECT_EQ(notifications, 2);
}

TEST_F(GamepadOverlayTest, ArtworkReplacementWaitsForGuiDelivery) {
    int argc = 1;
    char name[] = "gamepad-tests";
    char* argv[]{name, nullptr};
    QCoreApplication application(argc, argv);
    QString error;
    const auto sprites =
        RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/dualsense.svg"), 480, false, false, error);
    ASSERT_TRUE(sprites) << error.toStdString();
    GamepadOverlay overlay;
    Seed(overlay, sprites);
    Rebuild(overlay);
    ASSERT_TRUE(overlay.Snapshot(0));
    EXPECT_EQ(overlay.Snapshot(0)->atlas, sprites->atlas);
    WaitRaster(overlay);
    QCoreApplication::processEvents();
    EXPECT_NE(overlay.Snapshot(0)->atlas, sprites->atlas);
}

TEST_F(GamepadOverlayTest, DisableDiscardsPendingArtworkDelivery) {
    int argc = 1;
    char name[] = "gamepad-tests";
    char* argv[]{name, nullptr};
    QCoreApplication application(argc, argv);
    QString error;
    const auto sprites =
        RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/dualsense.svg"), 480, false, false, error);
    ASSERT_TRUE(sprites) << error.toStdString();
    GamepadOverlay overlay;
    Seed(overlay, sprites);
    int notifications = 0;
    QObject::connect(&overlay, &GamepadOverlay::frameChanged, [&]() { ++notifications; });
    Rebuild(overlay);
    overlay.setEnabled(false);
    WaitRaster(overlay);
    QCoreApplication::processEvents();
    EXPECT_EQ(notifications, 1);
    EXPECT_FALSE(overlay.Snapshot(0));
}

} // namespace exosnap::quick

TEST(GamepadSprites, RasterizesStraightAlphaWithTransparentGutters) {
    QString error;
    const auto sprites =
        RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/dualsense.svg"), 480, false, false, error);
    ASSERT_NE(sprites, nullptr) << error.toStdString();
    ASSERT_TRUE(sprites->atlas->HasCompletePayload());
    bool has_soft_white = false;
    for (size_t i = 0; i < sprites->atlas->pixels.size(); i += 4) {
        const auto* pixel = sprites->atlas->pixels.data() + i;
        if (pixel[3] > 16 && pixel[3] < 200 && pixel[0] > 240 && pixel[1] > 240 && pixel[2] > 240)
            has_soft_white = true;
    }
    EXPECT_TRUE(has_soft_white);
    for (const auto& part : sprites->parts) {
        EXPECT_GT(part.idle.x, 0);
        EXPECT_GT(part.idle.y, 0);
        const size_t corner = (static_cast<size_t>(part.idle.y - 1) * sprites->atlas->width + part.idle.x - 1) * 4;
        EXPECT_EQ(sprites->atlas->pixels[corner + 3], 0);
    }
}

TEST(GamepadSprites, InputChangesQuadsWithoutChangingAtlas) {
    QString error;
    const auto sprites =
        RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/dualsense.svg"), 480, false, false, error);
    ASSERT_NE(sprites, nullptr) << error.toStdString();
    exosnap::gamepad::GamepadState pressed;
    pressed.buttons = 4U | 0x1000U;
    pressed.left_x = 1.0F;
    pressed.left_y = 1.0F;
    pressed.right_trigger = 0.5F;
    const auto idle = GamepadLayer(*sprites, {}, {0, 0, 1, 1}, 1);
    const auto active = GamepadLayer(*sprites, pressed, {0, 0, 1, 1}, 1);
    EXPECT_EQ(idle->atlas, active->atlas);
    EXPECT_NE(idle->quads, active->quads);
    const auto again = GamepadLayer(*sprites, pressed, {0, 0, 1, 1}, 1);
    EXPECT_EQ(active->quads, again->quads);
}

TEST(GamepadSprites, AuxiliaryButtonsHighlightTheirActualControls) {
    QString error;
    const auto sprites =
        RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/dualsense.svg"), 480, false, false, error);
    ASSERT_NE(sprites, nullptr) << error.toStdString();
    const auto idle = GamepadLayer(*sprites, {}, {0, 0, 1, 1}, 1);
    exosnap::gamepad::GamepadState state;
    state.home = true;
    state.touchpad = true;
    const auto pressed = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
    size_t changed = 0;
    ASSERT_EQ(idle->quads.size(), pressed->quads.size());
    for (size_t i = 0; i < idle->quads.size(); ++i)
        if (idle->quads[i] != pressed->quads[i])
            ++changed;
    EXPECT_EQ(changed, 2U);
}

TEST(GamepadSprites, StickTravelIsRadialAndTriggerFillKeepsItsScale) {
    QString error;
    const auto sprites =
        RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/dualsense.svg"), 480, false, false, error);
    ASSERT_NE(sprites, nullptr) << error.toStdString();
    exosnap::gamepad::GamepadState state;
    state.left_x = 1;
    state.left_y = 1;
    state.right_trigger = 0.5F;
    const auto active = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
    const auto stick = std::find_if(sprites->parts.begin(), sprites->parts.end(),
                                    [](const auto& part) { return part.motion == PadMotion::LeftStick; });
    ASSERT_NE(stick, sprites->parts.end());
    const auto index = static_cast<size_t>(std::distance(sprites->parts.begin(), stick));
    const auto& moved = active->quads[index].destination;
    EXPECT_NEAR(std::hypot((moved.x - stick->bounds.x) * 480, (moved.y - stick->bounds.y) * 320), 8, 0.01);
    EXPECT_LT(moved.y, stick->bounds.y);
    ASSERT_EQ(active->quads.size(), sprites->parts.size() + 1);
    const auto trigger = std::find_if(sprites->parts.begin(), sprites->parts.end(),
                                      [](const auto& part) { return part.motion == PadMotion::RightTrigger; });
    ASSERT_NE(trigger, sprites->parts.end());
    const auto& fill = active->quads[static_cast<size_t>(std::distance(sprites->parts.begin(), trigger)) + 1];
    EXPECT_NEAR(static_cast<float>(fill.source.h) / trigger->pressed.h, 0.5F, 1.0F / trigger->pressed.h);
    EXPECT_NEAR(fill.destination.h / trigger->bounds.h, static_cast<float>(fill.source.h) / trigger->pressed.h, 0.001F);
}

TEST(GamepadSprites, AllArtworkRendersForVisualReview) {
    const QString output = qEnvironmentVariable("EXOSNAP_GAMEPAD_VISUAL_OUTPUT");
    for (const auto& name : {QStringLiteral("dualsense"), QStringLiteral("xbox_series"), QStringLiteral("generic")}) {
        for (const bool glass : {false, true}) {
            QString error;
            const auto sprites =
                RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/") + name + QStringLiteral(".svg"), 960,
                                 glass, false, error);
            ASSERT_NE(sprites, nullptr) << error.toStdString();
            if (output.isEmpty())
                continue;
            QImage sheet(1440, 640, QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&sheet);
            const QImage atlas(sprites->atlas->pixels.data(), sprites->atlas->width, sprites->atlas->height,
                               sprites->atlas->width * 4, QImage::Format_ARGB32);
            for (int column = 0; column < 3; ++column) {
                painter.fillRect(column * 480, 0, 480, 640,
                                 column == 0   ? QColor(20, 25, 32)
                                 : column == 1 ? QColor(100, 105, 112)
                                               : QColor(230, 232, 236));
                for (int row = 0; row < 2; ++row) {
                    exosnap::gamepad::GamepadState state;
                    if (row == 1) {
                        state.buttons = 0x3FFF;
                        state.left_x = -1;
                        state.right_y = 1;
                        state.left_trigger = 0.5F;
                        state.right_trigger = 1;
                    }
                    const auto layer = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
                    for (const auto& quad : layer->quads) {
                        const auto& s = quad.source;
                        const auto& d = quad.destination;
                        painter.setOpacity(quad.opacity);
                        painter.drawImage(QRectF(column * 480 + d.x * 480, row * 320 + d.y * 320, d.w * 480, d.h * 320),
                                          atlas, QRectF(s.x, s.y, s.w, s.h));
                    }
                }
            }
            painter.end();
            ASSERT_TRUE(QDir().mkpath(output));
            EXPECT_TRUE(sheet.save(output + QLatin1Char('/') + name +
                                   (glass ? QStringLiteral("-glass.png") : QStringLiteral("-lines.png"))));
        }
    }
}

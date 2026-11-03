#include "GamepadArtwork.h"
#include "GamepadOverlay.h"
#include "GamepadSprites.h"
#include "models/GamepadOverlaySettings.h"

#include <exosnap/engine/overlay_sprites.h>
#include <gamepad/gamepad.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPainter>
#include <QSize>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <gtest/gtest.h>
#include <iterator>
#include <memory>
#include <utility>

using namespace exosnap::quick;

namespace {
QImage MaskedTile(const exosnap::engine::SpriteLayerSnapshot& layer, const exosnap::engine::SpriteQuad& quad) {
    const QImage atlas(layer.atlas->pixels.data(), layer.atlas->width, layer.atlas->height, layer.atlas->width * 4,
                       QImage::Format_ARGB32);
    const auto& source = quad.source;
    QImage tile =
        atlas.copy(source.x, source.y, source.w, source.h).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
    for (const auto& mask : quad.masks) {
        if (mask.source.w <= 0 || mask.source.h <= 0)
            continue;
        const auto& d = quad.destination;
        const auto& m = mask.destination;
        painter.drawImage(QRectF((m.x - d.x) / d.w * source.w, (m.y - d.y) / d.h * source.h, m.w / d.w * source.w,
                                 m.h / d.h * source.h),
                          atlas, QRectF(mask.source.x, mask.source.y, mask.source.w, mask.source.h));
    }
    painter.end();
    return tile;
}
} // namespace

TEST(GamepadSprites, MovingKnockoutClearsOnlyTheBodyAtTheCurrentCapPosition) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QFile source(directory.filePath(QStringLiteral("source.svg")));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 480 320"><g id="body"><rect width="480" height="320" fill="white"/></g><g id="stick-l-cap"><g transform="translate(240 160)"><circle id="cap" cx="0" cy="0" r="20" stroke="white" fill="none"/></g></g></svg>)svg");
    source.close();
    QFile mapping(directory.filePath(QStringLiteral("mapping.json")));
    ASSERT_TRUE(mapping.open(QIODevice::WriteOnly));
    mapping.write(
        R"({"schemaVersion":1,"source":"source.svg","layers":[{"id":"body","role":"body"},{"id":"stick-l-cap","role":"stick","pressElement":"cap","knockout":"cap","binding":{"type":"stick","clickMask":4096},"motion":{"travelRadius":30}}]})");
    mapping.close();
    QString error;
    const auto sprites = RasterizeGamepad(mapping.fileName(), 480, false, false, error);
    ASSERT_TRUE(sprites) << error.toStdString();
    for (const QPointF position : {QPointF(0, 0), QPointF(1, 0), QPointF(1, 1)}) {
        exosnap::gamepad::GamepadState state;
        state.left_x = static_cast<float>(position.x());
        state.left_y = static_cast<float>(position.y());
        const auto layer = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
        EXPECT_EQ(layer->atlas, sprites->atlas);
        const auto& body = layer->quads.front();
        const auto& cap = layer->quads[1].destination;
        const auto tile = MaskedTile(*layer, body);
        const auto alpha = [&](float x, float y) {
            const int px = qRound((x - body.destination.x) / body.destination.w * body.source.w);
            const int py = qRound((y - body.destination.y) / body.destination.h * body.source.h);
            return qAlpha(tile.pixel(px, py));
        };
        // Body pixels under the moving cap must be transparent.
        EXPECT_EQ(alpha(cap.x + cap.w / 2, cap.y + cap.h / 2), 0);
        EXPECT_GT(alpha(0.1F, 0.5F), 0);
        EXPECT_TRUE(layer->atlas->HasCompletePayload());
        state.buttons = 4096;
        const auto clicked = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 0.5F);
        EXPECT_EQ(clicked->atlas, layer->atlas);
        EXPECT_NE(clicked->quads, layer->quads);
    }
}

TEST(GamepadSprites, MappedCatalogProducesVisiblePressedTilesAndCompleteSnapshots) {
    const QDir root(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers"));
    QFile catalog(root.filePath(QStringLiteral("catalog.json")));
    ASSERT_TRUE(catalog.open(QIODevice::ReadOnly));
    const auto controllers = QJsonDocument::fromJson(catalog.readAll()).object().value("controllers").toArray();
    for (const auto& value : controllers) {
        const auto entry = value.toObject();
        SCOPED_TRACE(entry.value("id").toString().toStdString());
        QString error;
        const auto sprites =
            RasterizeGamepad(root.filePath(entry.value("mapping").toString()), 480, false, false, error);
        ASSERT_TRUE(sprites) << error.toStdString();
        const auto blue_pixels = [&](const exosnap::engine::SpriteAtlasRect& rect) {
            int count = 0;
            for (int y = rect.y; y < rect.y + rect.h; ++y) {
                for (int x = rect.x; x < rect.x + rect.w; ++x) {
                    const auto* pixel =
                        &sprites->atlas->pixels[(static_cast<size_t>(y) * sprites->atlas->width + x) * 4];
                    if (pixel[0] > pixel[2] + 15 && pixel[1] > pixel[2] + 5 && pixel[3] > 15)
                        ++count;
                }
            }
            return count;
        };
        for (size_t index = 0; index < sprites->parts.size(); ++index) {
            const auto& part = sprites->parts[index];
            if (index != 0)
                EXPECT_TRUE(part.canonical || part.button || part.auxiliary != PadAuxiliary::None ||
                            part.motion != PadMotion::None);
            if (part.canonical || part.button || part.auxiliary != PadAuxiliary::None ||
                part.motion != PadMotion::None) {
                EXPECT_EQ(blue_pixels(part.idle), 0);
                EXPECT_GT(blue_pixels(part.pressed), 0);
            }
        }
        exosnap::gamepad::GamepadState state;
        const auto idle = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
        state.buttons = 0x3FFF;
        state.left_x = 1;
        state.right_trigger = 0.5F;
        state.home = true;
        const auto pressed = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
        EXPECT_TRUE(pressed->atlas->HasCompletePayload());
        EXPECT_NE(idle->quads, pressed->quads);
    }
}

TEST(GamepadSprites, CatalogRasterSizesAndMovingMaskCost) {
    QFile report;
    const auto output = qEnvironmentVariable("EXOSNAP_GAMEPAD_VISUAL_OUTPUT");
    if (!output.isEmpty()) {
        ASSERT_TRUE(QDir().mkpath(output));
        report.setFileName(output + QStringLiteral("/mask-cost.csv"));
        ASSERT_TRUE(report.open(QIODevice::WriteOnly));
        report.write("layout,width,atlas_bytes,snapshot_mean_us\n");
    }
    for (const auto& id : exosnap::kGamepadOverlayArtworkIds) {
        for (const int width : {240, 480, 960}) {
            SCOPED_TRACE(std::string(id) + ":" + std::to_string(width));
            QString error;
            const auto sprites = RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers/") +
                                                      QString::fromLatin1(id.data()) + QStringLiteral("/mapping.json"),
                                                  width, false, false, error);
            ASSERT_TRUE(sprites) << error.toStdString();
            for (const auto& part : sprites->parts) {
                for (const auto& rect : {part.idle, part.pressed}) {
                    EXPECT_GE(rect.x, 0);
                    EXPECT_GE(rect.y, 0);
                    EXPECT_LE(rect.x + rect.w, sprites->atlas->width);
                    EXPECT_LE(rect.y + rect.h, sprites->atlas->height);
                }
            }
            QElapsedTimer timer;
            timer.start();
            for (int step = 0; step < 30; ++step) {
                exosnap::gamepad::GamepadState state;
                state.left_x = std::sin(step * 0.3F);
                state.left_y = std::cos(step * 0.3F);
                state.right_x = -state.left_x;
                state.right_y = -state.left_y;
                const auto layer = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
                EXPECT_TRUE(layer->atlas->HasCompletePayload());
            }
            if (report.isOpen())
                report.write(QByteArray(id.data(), static_cast<qsizetype>(id.size())) + ',' +
                             QByteArray::number(width) + ',' + QByteArray::number(sprites->atlas->pixels.size()) + ',' +
                             QByteArray::number(timer.nsecsElapsed() / 30000.0, 'f', 2) + '\n');
        }
    }
}

TEST(GamepadSprites, MappedResourcesExposeEveryPersistableLayout) {
    GamepadOverlay overlay;
    EXPECT_EQ(overlay.layouts().size(), exosnap::kGamepadOverlayArtworkIds.size() + 1);
    for (const auto& option : overlay.layouts())
        EXPECT_TRUE(option.toMap().value(QStringLiteral("selectable")).toBool());
    for (int index = 0; index < static_cast<int>(exosnap::kGamepadOverlayArtworkIds.size()); ++index) {
        exosnap::GamepadOverlaySettings settings;
        settings.layout = index;
        EXPECT_EQ(exosnap::SanitizeGamepadOverlaySettings(settings).layout, index);
        QString error;
        const auto sprites = RasterizeGamepad(
            QStringLiteral(":/gamepad/controllers/") +
                QString::fromLatin1(exosnap::kGamepadOverlayArtworkIds[index].data()) + QStringLiteral("/mapping.json"),
            240, false, false, error);
        ASSERT_TRUE(sprites) << error.toStdString();
    }
}

TEST(GamepadSprites, Steam2026PersistedLayoutUsesGenericResourcesWithoutQualifyingExtraInputs) {
    using namespace exosnap::gamepad;
    ASSERT_EQ(exosnap::kGamepadOverlayArtworkIds[15], "steam_controller_2026");
    const QString root = QStringLiteral(":/gamepad/controllers/");
    QFile generic(root + "generic/source.svg");
    QFile fallback(root + "steam_controller_2026/source.svg");
    ASSERT_TRUE(generic.open(QIODevice::ReadOnly));
    ASSERT_TRUE(fallback.open(QIODevice::ReadOnly));
    EXPECT_TRUE(fallback.readAll() == generic.readAll());
    GamepadOverlay overlay;
    EXPECT_EQ(overlay.layouts()[16].toMap().value("label").toString(),
              QStringLiteral("Steam Controller (2026, generic artwork)"));
    QString error;
    const auto sprites = RasterizeGamepad(root + "steam_controller_2026/mapping.json", 240, false, false, error);
    ASSERT_TRUE(sprites) << error.toStdString();
    GamepadState state;
    state.controls_normalized = true;
    const auto idle = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
    for (const auto control : {Control::Misc1, Control::LeftPaddle1, Control::LeftPaddle2, Control::RightPaddle1,
                               Control::RightPaddle2, Control::Touchpad})
        state.controls[static_cast<size_t>(control)] = {ControlStatus::Active, 1, 0};
    EXPECT_EQ(GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1)->quads, idle->quads);
    for (const auto control :
         {Control::South, Control::East, Control::West, Control::North, Control::Back, Control::Guide, Control::Start,
          Control::LeftShoulder, Control::RightShoulder, Control::DpadUp, Control::DpadDown, Control::DpadLeft,
          Control::DpadRight, Control::LeftStick, Control::RightStick, Control::LeftTrigger, Control::RightTrigger}) {
        const auto part = std::find_if(sprites->parts.begin(), sprites->parts.end(),
                                       [=](const auto& part) { return part.control == control; });
        EXPECT_NE(part, sprites->parts.end()) << static_cast<int>(control);
    }
    for (const auto control : {Control::LeftStickClick, Control::RightStickClick}) {
        const auto part = std::find_if(sprites->parts.begin(), sprites->parts.end(),
                                       [=](const auto& part) { return part.click_control == control; });
        EXPECT_NE(part, sprites->parts.end());
    }
}

TEST(GamepadSprites, MappedPressureStaysInsideShouldersAndDpadDirection) {
    for (const auto* name : {"xbox_series", "xbox_one", "generic"}) {
        SCOPED_TRACE(name);
        QString error;
        const auto sprites = RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers/") +
                                                  QString::fromLatin1(name) + QStringLiteral("/mapping.json"),
                                              480, false, false, error);
        ASSERT_TRUE(sprites) << error.toStdString();
        for (const auto& part : sprites->parts) {
            if (part.control != exosnap::gamepad::Control::LeftShoulder &&
                part.control != exosnap::gamepad::Control::RightShoulder &&
                part.control != exosnap::gamepad::Control::DpadUp)
                continue;
            int outside = 0;
            for (int y = 0; y < part.pressed.h; ++y) {
                for (int x = 0; x < part.pressed.w; ++x) {
                    const float world_x = part.bounds.x + part.bounds.w * (x + 0.5F) / part.pressed.w;
                    const bool excluded = part.control == exosnap::gamepad::Control::DpadUp
                                              ? y > part.pressed.h * 0.6F
                                              : world_x > 0.42F && world_x < 0.58F;
                    if (!excluded)
                        continue;
                    const auto* pixel =
                        &sprites->atlas->pixels[(static_cast<size_t>(part.pressed.y + y) * sprites->atlas->width +
                                                 part.pressed.x + x) *
                                                4];
                    if (pixel[0] > pixel[2] + 15 && pixel[1] > pixel[2] + 5 && pixel[3] > 15)
                        ++outside;
                }
            }
            EXPECT_EQ(outside, 0) << part.button;
        }
    }
}

TEST(GamepadArtworkSources, ExportedLayersMatchSourceWithoutBakingStickCapsIntoBody) {
    const QDir root(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers"));
    QFile catalog(root.filePath(QStringLiteral("catalog.json")));
    ASSERT_TRUE(catalog.open(QIODevice::ReadOnly));
    const auto controllers = QJsonDocument::fromJson(catalog.readAll()).object().value("controllers").toArray();
    ASSERT_EQ(controllers.size(), 20);
    for (const auto& controller : controllers) {
        const auto entry = controller.toObject();
        const QDir directory(root.filePath(entry.value("id").toString()));
        SCOPED_TRACE(directory.path().toStdString());
        QFile file(directory.filePath(QStringLiteral("mapping.json")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const auto mapping = QJsonDocument::fromJson(file.readAll()).object();
        ASSERT_EQ(mapping.value("schemaVersion").toInt(), 1);
        EXPECT_EQ(mapping.value("appearance").toObject().value("detailWidth").toInt(), 50);
        EXPECT_EQ(mapping.value("appearance").toObject().value("detailOpacity").toInt(), 40);
        const auto box = mapping.value("viewBox").toArray();
        ASSERT_EQ(box.size(), 4);
        ASSERT_GT(box[2].toDouble(), 0);
        ASSERT_GT(box[3].toDouble(), 0);
        const QSize size(480, qRound(480 * box[3].toDouble() / box[2].toDouble()));
        QImage expected(size, QImage::Format_ARGB32_Premultiplied);
        QImage composed(size, QImage::Format_ARGB32_Premultiplied);
        expected.fill(Qt::transparent);
        composed.fill(Qt::transparent);
        QSvgRenderer source(directory.filePath(mapping.value("source").toString()));
        ASSERT_TRUE(source.isValid());
        {
            QPainter painter(&expected);
            source.render(&painter, QRectF(QPointF(), size));
        }
        const auto layers = mapping.value("layers").toArray();
        ASSERT_FALSE(layers.isEmpty());
        ASSERT_EQ(layers[0].toObject().value("role").toString(), QStringLiteral("body"));
        {
            QPainter painter(&composed);
            for (const auto& value : layers) {
                const auto layer = value.toObject();
                const auto bounds = layer.value("bounds").toObject();
                EXPECT_GT(bounds.value("width").toDouble(), 0);
                EXPECT_GT(bounds.value("height").toDouble(), 0);
                EXPECT_GE(bounds.value("x").toDouble(), box[0].toDouble() - 1);
                EXPECT_GE(bounds.value("y").toDouble(), box[1].toDouble() - 1);
                EXPECT_LE(bounds.value("x").toDouble() + bounds.value("width").toDouble(),
                          box[0].toDouble() + box[2].toDouble() + 1);
                EXPECT_LE(bounds.value("y").toDouble() + bounds.value("height").toDouble(),
                          box[1].toDouble() + box[3].toDouble() + 1);
                QSvgRenderer element(directory.filePath(layer.value("file").toString()));
                ASSERT_TRUE(element.isValid()) << layer.value("id").toString().toStdString();
                if (layer.value("role").toString() == QStringLiteral("body")) {
                    EXPECT_FALSE(element.elementExists(QStringLiteral("stick-l-cap")));
                    EXPECT_FALSE(element.elementExists(QStringLiteral("stick-r-cap")));
                } else if (layer.value("role").toString() == QStringLiteral("stick")) {
                    EXPECT_TRUE(element.elementExists(layer.value("id").toString()));
                    const auto motion = layer.value("motion").toObject();
                    EXPECT_EQ(motion.value("center").toArray().size(), 2);
                    EXPECT_GT(motion.value("travelRadius").toDouble(), 0);
                }
                element.render(&painter, QRectF(QPointF(), size));
            }
        }
        double alpha_difference = 0;
        double source_alpha = 0;
        int non_white_pixels = 0;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const auto alpha = qAlpha(expected.pixel(x, y));
                const auto pixel = expected.pixel(x, y);
                if (qRed(pixel) != alpha || qGreen(pixel) != alpha || qBlue(pixel) != alpha)
                    ++non_white_pixels;
                source_alpha += alpha;
                alpha_difference += std::abs(alpha - qAlpha(composed.pixel(x, y)));
            }
        }
        const double pixels = static_cast<double>(size.width()) * size.height();
        EXPECT_EQ(non_white_pixels, 0);
        EXPECT_GT(source_alpha / pixels, 1);
        EXPECT_LT(alpha_difference / pixels, 0.6);
    }
}

TEST(GamepadArtworkSources, HorizontalJoyConsUseRailButtonsAsShoulders) {
    for (const auto* name : {"joycon_left_horizontal", "joycon_right_horizontal"}) {
        QFile file(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers/") + QString::fromLatin1(name) +
                   QStringLiteral("/mapping.json"));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const auto mapping = QJsonDocument::fromJson(file.readAll()).object();
        EXPECT_EQ(mapping.value("orientation").toString(), QStringLiteral("horizontal"));
        bool found_sl = false;
        bool found_sr = false;
        bool found_stick = false;
        for (const auto& value : mapping.value("layers").toArray()) {
            const auto layer = value.toObject();
            const auto binding = layer.value("binding").toObject();
            const auto id = layer.value("id").toString();
            if (id == QStringLiteral("btn-sl")) {
                EXPECT_EQ(binding.value("mask").toInt(), 1024);
                found_sl = true;
            } else if (id == QStringLiteral("btn-sr")) {
                EXPECT_EQ(binding.value("mask").toInt(), 2048);
                found_sr = true;
            } else if (layer.value("role").toString() == QStringLiteral("stick")) {
                EXPECT_EQ(binding.value("axes").toArray()[0].toString(), QStringLiteral("left_x"));
                EXPECT_EQ(binding.value("clickMask").toInt(), 4096);
                found_stick = true;
            }
        }
        EXPECT_TRUE(found_sl);
        EXPECT_TRUE(found_sr);
        EXPECT_TRUE(found_stick);
    }
}

namespace exosnap::quick {

class GamepadOverlayTest : public ::testing::Test {
  protected:
    static gamepad::GamepadState DisplayState(gamepad::GamepadState state) {
        return GamepadOverlay::DisplayState(std::move(state));
    }
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

TEST_F(GamepadOverlayTest, HiddenOutputRetainsDimmedPreviewAndInputUpdates) {
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
    ASSERT_TRUE(overlay.Snapshot(0));
    int notifications = 0;
    QObject::connect(&overlay, &GamepadOverlay::frameChanged, [&]() { ++notifications; });
    overlay.setOutputVisible(false);
    EXPECT_TRUE(overlay.enabled());
    EXPECT_FALSE(overlay.Snapshot(0));
    ASSERT_TRUE(overlay.PreviewSnapshot(0));
    ASSERT_FALSE(overlay.PreviewSnapshot(0)->quads.empty());
    EXPECT_FLOAT_EQ(overlay.PreviewSnapshot(0)->quads.front().opacity, 0.35F);
    Poll(overlay);
    EXPECT_GT(notifications, 0);
    overlay.setOutputVisible(true);
    EXPECT_TRUE(overlay.Snapshot(0));
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
    for (const auto& id : exosnap::kGamepadOverlayArtworkIds) {
        const QString name = QString::fromLatin1(id.data());
        for (const bool glass : {false}) {
            QString error;
            const auto sprites = RasterizeGamepad(QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers/") + name +
                                                      QStringLiteral("/mapping.json"),
                                                  960, glass, false, error);
            ASSERT_NE(sprites, nullptr) << error.toStdString();
            if (output.isEmpty())
                continue;
            QImage sheet(1440, 640, QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&sheet);
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
                        state.home = true;
                        state.share = true;
                        state.touchpad = true;
                    }
                    const auto layer = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
                    const QImage atlas(layer->atlas->pixels.data(), layer->atlas->width, layer->atlas->height,
                                       layer->atlas->width * 4, QImage::Format_ARGB32);
                    for (const auto& quad : layer->quads) {
                        const auto& d = quad.destination;
                        painter.setOpacity(quad.opacity);
                        painter.drawImage(QRectF(column * 480 + d.x * 480, row * 320 + d.y * 320, d.w * 480, d.h * 320),
                                          MaskedTile(*layer, quad));
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

TEST(GamepadSprites, ClippedTranslatedBodyUsesItsOwnMaskRegion) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QFile source(directory.filePath(QStringLiteral("source.svg")));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write(
        R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 480 320"><defs><clipPath id="cut" clipPathUnits="userSpaceOnUse"><path clip-rule="evenodd" d="M600 100h80v80h-80z M620 120h20v20h-20z"/></clipPath></defs><g id="body"><rect x="40" y="40" width="80" height="80" fill="white"/><g transform="translate(-400)"><g clip-path="url(#cut)"><rect x="600" y="100" width="160" height="80" fill="white"/></g></g></g></svg>)svg");
    source.close();
    QFile mapping(directory.filePath(QStringLiteral("mapping.json")));
    ASSERT_TRUE(mapping.open(QIODevice::WriteOnly));
    mapping.write(R"({"schemaVersion":1,"source":"source.svg","layers":[{"id":"body","role":"body"}]})");
    mapping.close();
    QString error;
    const auto sprites = RasterizeGamepad(mapping.fileName(), 480, false, false, error);
    ASSERT_TRUE(sprites) << error.toStdString();
    const auto layer = GamepadLayer(*sprites, {}, {0, 0, 1, 1}, 1);
    ASSERT_EQ(layer->quads.size(), 1u);
    QImage image(480, 320, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const QImage atlas(layer->atlas->pixels.data(), layer->atlas->width, layer->atlas->height, layer->atlas->width * 4,
                       QImage::Format_ARGB32);
    const auto& quad = layer->quads.front();
    {
        QPainter painter(&image);
        painter.drawImage(QRectF(quad.destination.x * 480, quad.destination.y * 320, quad.destination.w * 480,
                                 quad.destination.h * 320),
                          atlas, QRectF(quad.source.x, quad.source.y, quad.source.w, quad.source.h));
    }
    EXPECT_GT(qAlpha(image.pixel(86, 83)), 240);
    EXPECT_GT(qAlpha(image.pixel(259, 160)), 240);
    EXPECT_EQ(qAlpha(image.pixel(230, 131)), 0);
    EXPECT_EQ(qAlpha(image.pixel(317, 141)), 0);
}

TEST(GamepadSprites, CanonicalFaceControlsFollowPhysicalArtwork) {
    using namespace exosnap::gamepad;
    struct Example {
        const char* layout;
        const char* id;
        Control control;
    };
    for (const auto& example :
         {Example{"xbox_series", "btn-a", Control::South}, Example{"dualsense", "btn-cross", Control::South},
          Example{"switch_pro", "btn-b", Control::South}, Example{"generic", "btn-a", Control::South},
          Example{"gamecube", "btn-x", Control::East}, Example{"gamecube", "btn-b", Control::West},
          Example{"gamecube", "btn-r1", Control::RightShoulder}}) {
        SCOPED_TRACE(example.layout);
        QString error;
        const auto path = QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers/") + example.layout + "/mapping.json";
        const auto artwork = PrepareMappedGamepadArtwork(path, error);
        ASSERT_FALSE(artwork.svg.isEmpty()) << error.toStdString();
        EXPECT_EQ(artwork.controls.value(example.id).control, example.control);
        const auto sprites = RasterizeGamepad(path, 240, false, false, error);
        ASSERT_TRUE(sprites) << error.toStdString();
        GamepadState state;
        state.controls_normalized = true;
        state.controls[static_cast<size_t>(example.control)] = {ControlStatus::Active, 1, 0};
        const auto layer = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
        for (size_t i = 1; i < sprites->parts.size(); ++i) {
            const auto& part = sprites->parts[i];
            EXPECT_EQ(layer->quads[i].source, part.control == example.control ? part.pressed : part.idle);
        }
    }
}

TEST(GamepadSprites, GameCubeMovesBothSticksWithoutInventingStickClicks) {
    using namespace exosnap::gamepad;
    QString error;
    const auto sprites = RasterizeGamepad(
        QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers/gamecube/mapping.json"), 480, false, false, error);
    ASSERT_TRUE(sprites) << error.toStdString();
    GamepadState state;
    state.controls_normalized = true;
    state.controls[static_cast<size_t>(Control::LeftStick)] = {ControlStatus::Active, -1, 1};
    state.controls[static_cast<size_t>(Control::RightStick)] = {ControlStatus::Active, 1, -1};
    state.controls[static_cast<size_t>(Control::LeftStickClick)] = {ControlStatus::Active, 1, 0};
    state.controls[static_cast<size_t>(Control::RightStickClick)] = {ControlStatus::Active, 1, 0};
    const auto layer = GamepadLayer(*sprites, state, {0.1F, 0.2F, 0.6F, 0.5F}, 0.7F);
    ASSERT_EQ(layer->atlas, sprites->atlas);
    ASSERT_GT(layer->quads.front().masks[0].source.w, 0);
    ASSERT_GT(layer->quads.front().masks[1].source.w, 0);
    for (size_t i = 1; i < sprites->parts.size(); ++i) {
        const auto& part = sprites->parts[i];
        if (part.motion != PadMotion::LeftStick && part.motion != PadMotion::RightStick)
            continue;
        EXPECT_EQ(part.click_control, Control::Count);
        EXPECT_EQ(layer->quads[i].source, part.idle);
        EXPECT_NE(layer->quads[i].destination.x, 0.1F + part.bounds.x * 0.6F);
    }
    state.controls[static_cast<size_t>(Control::Misc3)] = {ControlStatus::Active, 1, 0};
    const auto clicked = GamepadLayer(*sprites, state, {0.1F, 0.2F, 0.6F, 0.5F}, 0.7F);
    EXPECT_NE(layer->quads, clicked->quads);
}
namespace {
class CanonicalArtworkTest : public ::testing::Test {
  protected:
    QTemporaryDir directory;
    QJsonObject mapping;
    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        const QString root = QStringLiteral(EXOSNAP_GAMEPAD_ASSET_DIR "/controllers/xbox_series/");
        ASSERT_TRUE(QFile::copy(root + "source.svg", directory.filePath("source.svg")));
        QFile file(root + "mapping.json");
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        mapping = QJsonDocument::fromJson(file.readAll()).object();
    }
    std::shared_ptr<const GamepadSpriteSet> Rasterize(QString& error) {
        QFile file(directory.filePath("mapping.json"));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return {};
        file.write(QJsonDocument(mapping).toJson());
        file.close();
        return RasterizeGamepad(file.fileName(), 480, false, false, error);
    }
};
} // namespace

TEST_F(CanonicalArtworkTest, CanonicalOnlyLayersDoNotRequireProviderBindings) {
    auto layers = mapping.value("layers").toArray();
    for (int i = 0; i < layers.size(); ++i) {
        auto layer = layers[i].toObject();
        if (!layer.value("binding").isNull())
            layer.remove("binding");
        layers[i] = layer;
    }
    mapping.insert("layers", layers);
    QString error;
    const auto sprites = Rasterize(error);
    ASSERT_TRUE(sprites) << error.toStdString();
    using namespace exosnap::gamepad;
    for (const auto control : {Control::South, Control::DpadUp, Control::LeftStick, Control::LeftTrigger}) {
        const auto part = std::find_if(sprites->parts.begin(), sprites->parts.end(),
                                       [=](const auto& part) { return part.control == control; });
        EXPECT_NE(part, sprites->parts.end());
    }
}

TEST_F(CanonicalArtworkTest, ExplicitNullBindingRemainsInertWithCanonicalInput) {
    auto layers = mapping.value("layers").toArray();
    for (int i = 0; i < layers.size(); ++i) {
        auto layer = layers[i].toObject();
        if (layer.value("control") == "south")
            layer.insert("binding", QJsonValue::Null);
        layers[i] = layer;
    }
    mapping.insert("layers", layers);
    QString error;
    const auto sprites = Rasterize(error);
    ASSERT_TRUE(sprites) << error.toStdString();
    exosnap::gamepad::GamepadState state;
    state.controls_normalized = true;
    const auto idle = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
    state.controls[static_cast<size_t>(exosnap::gamepad::Control::South)] = {exosnap::gamepad::ControlStatus::Active, 1,
                                                                             0};
    const auto pressed = GamepadLayer(*sprites, state, {0, 0, 1, 1}, 1);
    EXPECT_EQ(idle->quads, pressed->quads);
}

TEST_F(CanonicalArtworkTest, UnknownCanonicalControlRejectsMapping) {
    auto layers = mapping.value("layers").toArray();
    auto layer = layers[1].toObject();
    layer.insert("control", "button-number-42");
    layers[1] = layer;
    mapping.insert("layers", layers);
    QString error;
    EXPECT_FALSE(Rasterize(error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(CanonicalArtworkTest, PressColorChangesFillPixels) {
    auto layers = mapping.value("layers").toArray();
    for (int i = 0; i < layers.size(); ++i) {
        auto layer = layers[i].toObject();
        if (layer.value("control") == "south")
            layer.insert("pressColor", "#ff8a3d");
        layers[i] = layer;
    }
    mapping.insert("layers", layers);
    QString error;
    const auto sprites = Rasterize(error);
    ASSERT_TRUE(sprites) << error.toStdString();
    const auto part = std::find_if(sprites->parts.begin(), sprites->parts.end(),
                                   [](const auto& part) { return part.control == exosnap::gamepad::Control::South; });
    ASSERT_NE(part, sprites->parts.end());
    int orange = 0;
    for (int y = part->pressed.y; y < part->pressed.y + part->pressed.h; ++y)
        for (int x = part->pressed.x; x < part->pressed.x + part->pressed.w; ++x) {
            const auto* pixel = &sprites->atlas->pixels[(static_cast<size_t>(y) * sprites->atlas->width + x) * 4];
            if (pixel[2] > pixel[1] + 30 && pixel[1] > pixel[0] + 30 && pixel[3] > 15)
                ++orange;
        }
    EXPECT_GT(orange, 0);
}

namespace exosnap::quick {
TEST_F(GamepadOverlayTest, CanonicalDeadzoneCoalescesWithoutChangingCapturedInput) {
    using namespace exosnap::gamepad;
    GamepadState first;
    first.controls_normalized = true;
    first.controls[static_cast<size_t>(Control::LeftStick)] = {ControlStatus::Active, 0.02F, -0.03F};
    first.left_x = 0.02F;
    first.left_y = -0.03F;
    auto second = first;
    second.controls[static_cast<size_t>(Control::LeftStick)] = {ControlStatus::Active, 0.01F, 0.02F};
    second.left_x = 0.01F;
    second.left_y = 0.02F;
    EXPECT_EQ(DisplayState(first), DisplayState(second));
    EXPECT_EQ(ReadControl(first, Control::LeftStick).x, 0.02F);
    second.controls[static_cast<size_t>(Control::LeftStick)] = {ControlStatus::Active, 0.5F, 0};
    EXPECT_NE(DisplayState(first), DisplayState(second));
    second.controls[static_cast<size_t>(Control::LeftStick)] = {ControlStatus::Unavailable, 0, 0};
    EXPECT_EQ(ReadControl(DisplayState(second), Control::LeftStick).status, ControlStatus::Unavailable);
}
} // namespace exosnap::quick
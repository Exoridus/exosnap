#include "GamepadSprites.h"
#include "GamepadArtwork.h"

#include <exosnap/engine/overlay_sprites.h>
#include <gamepad/gamepad.h>

#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QSvgRenderer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace exosnap::quick {
namespace {

std::atomic<uint64_t> atlas_generation{0};

struct Element {
    const char* id = nullptr;
    uint32_t button = 0;
    PadMotion motion = PadMotion::None;
    PadAuxiliary auxiliary = PadAuxiliary::None;
};

constexpr Element kElements[] = {
    {"btn-menu", 1},
    {"btn-options", 1},
    {"btn-view", 2},
    {"btn-create", 2},
    {"btn-a", 4},
    {"btn-cross", 4},
    {"btn-b", 8},
    {"btn-circle", 8},
    {"btn-x", 16},
    {"btn-square", 16},
    {"btn-y", 32},
    {"btn-triangle", 32},
    {"btn-up", 64},
    {"btn-down", 128},
    {"btn-left", 256},
    {"btn-right", 512},
    {"btn-l1", 1024},
    {"btn-r1", 2048},
    {"stick-l-cap", 4096, PadMotion::LeftStick},
    {"stick-r-cap", 8192, PadMotion::RightStick},
    {"trigger-l2", 0, PadMotion::LeftTrigger},
    {"trigger-r2", 0, PadMotion::RightTrigger},
    {"btn-home", 0, PadMotion::None, PadAuxiliary::Home},
    {"btn-share", 0, PadMotion::None, PadAuxiliary::Share},
    {"btn-mute", 0},
    {"btn-touchpad", 0, PadMotion::None, PadAuxiliary::Touchpad},
};

struct Tile {
    QImage pixels;
    engine::SpriteAtlasRect rect;
};

QImage RasterizeElement(QSvgRenderer& renderer, const QString& id, const QRectF& bounds, double scale, int padding,
                        const QRectF& geometry = {}) {
    const int w = std::max(1, static_cast<int>(std::ceil(bounds.width() * scale)));
    const int h = std::max(1, static_cast<int>(std::ceil(bounds.height() * scale)));
    QImage image(w + padding * 2, h + padding * 2, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto target =
            geometry.isEmpty() || geometry == bounds
                ? QRectF(padding, padding, w, h)
                : QRectF(padding + (geometry.x() - bounds.x()) * scale, padding + (geometry.y() - bounds.y()) * scale,
                         geometry.width() * scale, geometry.height() * scale);
        renderer.render(&painter, id, target);
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    const QImage original = image;
    // Straight-alpha bilinear sampling needs neighbouring RGB even where alpha
    // is zero, otherwise a scaled white contour acquires a dark fringe.
    for (int y = 1; y < image.height() - 1; ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 1; x < image.width() - 1; ++x) {
            if (qAlpha(row[x]) != 0)
                continue;
            for (const QPoint offset : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)}) {
                const QRgb neighbour = original.pixel(x + offset.x(), y + offset.y());
                if (qAlpha(neighbour) != 0) {
                    row[x] = neighbour & 0x00FFFFFFU;
                    break;
                }
            }
        }
    }
    return image;
}

} // namespace

std::shared_ptr<const GamepadSpriteSet> RasterizeGamepad(const QString& path, int width, bool, bool, QString& error) {
    MappedGamepadArtwork mapped;
    QSvgRenderer renderer;
    if (path.endsWith(QStringLiteral(".json"))) {
        mapped = PrepareMappedGamepadArtwork(path, error);
        if (mapped.svg.isEmpty())
            return nullptr;
        renderer.load(mapped.svg);
    } else {
        renderer.load(path);
    }
    if (!renderer.isValid() || renderer.viewBoxF() != QRectF(0, 0, 480, 320) || width < 96 || width > 2048) {
        error = QCoreApplication::translate("GamepadArtwork", "Invalid gamepad artwork or raster size.");
        return nullptr;
    }
    const double scale = static_cast<double>(width) / 480;
    const int padding = 2;
    auto result = std::make_shared<GamepadSpriteSet>();
    auto atlas = std::make_shared<engine::SpriteAtlas>();
    atlas->width = std::max(512, width + padding * 2 + 8);
    std::vector<Tile> tiles;
    int x = 1;
    int y = 1;
    int row_height = 0;
    auto add = [&](const QString& id, QRectF bounds) {
        Tile tile;
        const auto geometry = renderer.transformForElement(id).mapRect(renderer.boundsOnElement(id));
        tile.pixels = RasterizeElement(renderer, id, bounds, scale, padding, geometry);
        atlas->width = std::max(atlas->width, tile.pixels.width() + 2);
        if (x + tile.pixels.width() + 1 > atlas->width) {
            x = 1;
            y += row_height + 2;
            row_height = 0;
        }
        tile.rect = {x, y, tile.pixels.width(), tile.pixels.height()};
        x += tile.pixels.width() + 2;
        row_height = std::max(row_height, tile.pixels.height());
        const auto rect = tile.rect;
        tiles.push_back(std::move(tile));
        return rect;
    };
    auto add_part = [&](const QString& id, uint32_t button, PadMotion motion, bool body) {
        if (!renderer.elementExists(id))
            return;
        const auto geometry = renderer.transformForElement(id).mapRect(renderer.boundsOnElement(id));
        const QRectF bounds = body && !mapped.svg.isEmpty() ? geometry.intersected(renderer.viewBoxF()) : geometry;
        GamepadSpritePart part;
        part.button = button;
        part.motion = motion;
        if (mapped.stick_travel.contains(id)) {
            const auto travel = mapped.stick_travel.value(id);
            part.travel_x = static_cast<float>(travel.x());
            part.travel_y = static_cast<float>(travel.y());
        }
        part.idle = add(id, bounds);
        part.bounds = {static_cast<float>((bounds.x() - padding / scale) / 480),
                       static_cast<float>((bounds.y() - padding / scale) / 320),
                       static_cast<float>(part.idle.w / scale / 480), static_cast<float>(part.idle.h / scale / 320)};
        part.pressed = body ? part.idle : add(id + QStringLiteral("-on"), bounds);
        const auto knockout_id = id + QStringLiteral("-knockout");
        if (renderer.elementExists(knockout_id)) {
            const auto mask_bounds =
                renderer.transformForElement(knockout_id).mapRect(renderer.boundsOnElement(knockout_id));
            part.knockout = add(knockout_id, mask_bounds);
            part.knockout_bounds = {static_cast<float>((mask_bounds.x() - padding / scale) / 480),
                                    static_cast<float>((mask_bounds.y() - padding / scale) / 320),
                                    static_cast<float>(part.knockout.w / scale / 480),
                                    static_cast<float>(part.knockout.h / scale / 320)};
        }
        result->parts.push_back(part);
    };
    add_part(QStringLiteral("body"), 0, PadMotion::None, true);
    if (!mapped.svg.isEmpty()) {
        for (const auto& id : mapped.interactive_ids) {
            const auto binding = mapped.controls.value(id);
            if (!binding.live)
                continue;
            using gamepad::Control;
            const auto motion = binding.control == Control::LeftStick      ? PadMotion::LeftStick
                                : binding.control == Control::RightStick   ? PadMotion::RightStick
                                : binding.control == Control::LeftTrigger  ? PadMotion::LeftTrigger
                                : binding.control == Control::RightTrigger ? PadMotion::RightTrigger
                                                                           : PadMotion::None;
            add_part(id, 0, motion, false);
            auto& part = result->parts.back();
            part.control = binding.control;
            part.click_control = binding.click_control;
            part.canonical = true;
        }
    } else {
        for (const auto& element : kElements)
            if (renderer.elementExists(QString::fromLatin1(element.id))) {
                add_part(QString::fromLatin1(element.id), element.button, element.motion, false);
                result->parts.back().auxiliary = element.auxiliary;
            }
    }
    atlas->height = y + row_height + 1;
    if (atlas->width > 4096 || atlas->height > 4096) {
        error = QCoreApplication::translate("GamepadArtwork", "Gamepad atlas exceeds the supported texture size.");
        return nullptr;
    }
    atlas->pixels.resize(static_cast<size_t>(atlas->width) * atlas->height * 4);
    for (const auto& tile : tiles) {
        for (int line = 0; line < tile.rect.h; ++line) {
            const size_t destination = (static_cast<size_t>(tile.rect.y + line) * atlas->width + tile.rect.x) * 4;
            std::memcpy(atlas->pixels.data() + destination, tile.pixels.constScanLine(line),
                        static_cast<size_t>(tile.rect.w) * 4);
        }
    }
    atlas->generation = ++atlas_generation;
    result->atlas = std::move(atlas);
    return result;
}

std::shared_ptr<const engine::SpriteLayerSnapshot> GamepadLayer(const GamepadSpriteSet& sprites,
                                                                const gamepad::GamepadState& input,
                                                                engine::SpriteContentRect placement, float opacity) {
    auto layer = std::make_shared<engine::SpriteLayerSnapshot>();
    if (!engine::PrepareGamepadLayer(sprites, *layer).succeeded())
        return nullptr;
    engine::FillGamepadLayer(sprites, input, placement, opacity, *layer);
    return layer;
}

} // namespace exosnap::quick

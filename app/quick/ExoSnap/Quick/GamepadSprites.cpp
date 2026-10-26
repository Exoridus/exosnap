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
                        bool glow, bool shadow) {
    const int w = std::max(1, static_cast<int>(std::ceil(bounds.width() * scale)));
    const int h = std::max(1, static_cast<int>(std::ceil(bounds.height() * scale)));
    QImage image(w + padding * 2, h + padding * 2, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, id, QRectF(padding, padding, w, h));
    }
    if (shadow) {
        QImage silhouette = image;
        QPainter tint(&silhouette);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(silhouette.rect(), QColor(0, 0, 0, 170));
        tint.end();
        QPainter painter(&image);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                painter.setOpacity(static_cast<double>((x == 0 ? 2 : 1) * (y == 0 ? 2 : 1)) / 16);
                painter.drawImage(QPoint(x, y + 1), silhouette);
            }
        }
    }
    if (glow) {
        QImage halo = image;
        QPainter tint(&halo);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(halo.rect(), QColor(56, 189, 248, 22));
        tint.end();
        QPainter painter(&image);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
        for (const QPoint offset : {QPoint(-3, 0), QPoint(3, 0), QPoint(0, -3), QPoint(0, 3), QPoint(-2, -2),
                                    QPoint(2, -2), QPoint(-2, 2), QPoint(2, 2)})
            painter.drawImage(offset, halo);
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

engine::SpriteContentRect Placed(engine::SpriteContentRect local, engine::SpriteContentRect placement) {
    return {placement.x + local.x * placement.w, placement.y + local.y * placement.h, local.w * placement.w,
            local.h * placement.h};
}

void MoveStick(engine::SpriteContentRect& bounds, float x, float y, const GamepadSpritePart& part) {
    const float length = std::hypot(x, y);
    if (length <= 0.08F)
        return;
    const float travel = std::min(1.0F, (length - 0.08F) / 0.92F);
    bounds.x += x / length * travel * part.travel_x;
    bounds.y -= y / length * travel * part.travel_y;
}

} // namespace

std::shared_ptr<const GamepadSpriteSet> RasterizeGamepad(const QString& path, int width, bool glass, bool glow,
                                                         QString& error) {
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
    const int padding = glow ? 8 : 2;
    auto result = std::make_shared<GamepadSpriteSet>();
    auto atlas = std::make_shared<engine::SpriteAtlas>();
    atlas->width = std::max(512, width + padding * 2 + 8);
    std::vector<Tile> tiles;
    int x = 1;
    int y = 1;
    int row_height = 0;
    auto add = [&](const QString& id, QRectF bounds, bool pressed) {
        Tile tile;
        tile.pixels = RasterizeElement(renderer, id, bounds, scale, padding, glow && pressed, !mapped.svg.isEmpty());
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
        const QRectF bounds = renderer.transformForElement(id).mapRect(renderer.boundsOnElement(id));
        GamepadSpritePart part;
        part.button = button;
        part.motion = motion;
        if (mapped.stick_travel.contains(id)) {
            const auto travel = mapped.stick_travel.value(id);
            part.travel_x = static_cast<float>(travel.x());
            part.travel_y = static_cast<float>(travel.y());
        }
        part.idle = add(id, bounds, false);
        part.bounds = {static_cast<float>((bounds.x() - padding / scale) / 480),
                       static_cast<float>((bounds.y() - padding / scale) / 320),
                       static_cast<float>(part.idle.w / scale / 480), static_cast<float>(part.idle.h / scale / 320)};
        part.pressed = body ? part.idle : add(id + QStringLiteral("-on"), bounds, true);
        result->parts.push_back(part);
    };
    add_part(glass ? QStringLiteral("body-glass") : QStringLiteral("body"), 0, PadMotion::None, true);
    for (const auto& element : kElements)
        if (renderer.elementExists(QString::fromLatin1(element.id))) {
            add_part(QString::fromLatin1(element.id), element.button, element.motion, false);
            result->parts.back().auxiliary = element.auxiliary;
        }
    atlas->height = y + row_height + 1;
    if (atlas->height > 4096) {
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
    static std::atomic<uint64_t> generation{0};
    atlas->generation = ++generation;
    result->atlas = std::move(atlas);
    return result;
}

std::shared_ptr<const engine::SpriteLayerSnapshot> GamepadLayer(const GamepadSpriteSet& sprites,
                                                                const gamepad::GamepadState& input,
                                                                engine::SpriteContentRect placement, float opacity) {
    const auto state = gamepad::Normalized(input);
    auto layer = std::make_shared<engine::SpriteLayerSnapshot>();
    layer->atlas = sprites.atlas;
    layer->quads.reserve(sprites.parts.size() + 2);
    for (const auto& part : sprites.parts) {
        auto bounds = part.bounds;
        if (part.motion == PadMotion::LeftStick)
            MoveStick(bounds, state.left_x, state.left_y, part);
        if (part.motion == PadMotion::RightStick)
            MoveStick(bounds, state.right_x, state.right_y, part);
        const bool pressed = (part.button & state.buttons) != 0 ||
                             (part.auxiliary == PadAuxiliary::Home && state.home.value_or(false)) ||
                             (part.auxiliary == PadAuxiliary::Share && state.share.value_or(false)) ||
                             (part.auxiliary == PadAuxiliary::Touchpad && state.touchpad.value_or(false));
        layer->quads.push_back({pressed ? part.pressed : part.idle, Placed(bounds, placement), opacity});
        const float trigger = part.motion == PadMotion::LeftTrigger    ? state.left_trigger
                              : part.motion == PadMotion::RightTrigger ? state.right_trigger
                                                                       : 0;
        if (trigger > 0) {
            auto source = part.pressed;
            const int fill = static_cast<int>(std::round(source.h * trigger));
            if (fill <= 0)
                continue;
            const float fraction = static_cast<float>(source.h - fill) / source.h;
            source.y += source.h - fill;
            source.h = fill;
            bounds.y += bounds.h * fraction;
            bounds.h *= 1 - fraction;
            layer->quads.push_back({source, Placed(bounds, placement), opacity});
        }
    }
    return layer;
}

} // namespace exosnap::quick

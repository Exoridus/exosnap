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
            const auto mask = RasterizeElement(renderer, knockout_id, mask_bounds, scale, padding);
            part.knockout_width = mask.width();
            part.knockout_height = mask.height();
            part.knockout.reserve(static_cast<size_t>(mask.width()) * mask.height());
            for (int row = 0; row < mask.height(); ++row)
                for (int column = 0; column < mask.width(); ++column)
                    part.knockout.push_back(static_cast<uint8_t>(qAlpha(mask.pixel(column, row))));
            part.knockout_bounds = {static_cast<float>((mask_bounds.x() - padding / scale) / 480),
                                    static_cast<float>((mask_bounds.y() - padding / scale) / 320),
                                    static_cast<float>(mask.width() / scale / 480),
                                    static_cast<float>(mask.height() / scale / 320)};
        }
        result->parts.push_back(part);
    };
    add_part(QStringLiteral("body"), 0, PadMotion::None, true);
    for (const auto& element : kElements)
        if (renderer.elementExists(QString::fromLatin1(element.id)) &&
            (mapped.svg.isEmpty() || mapped.interactive_ids.contains(QString::fromLatin1(element.id)))) {
            add_part(QString::fromLatin1(element.id), element.button, element.motion, false);
            result->parts.back().auxiliary = element.auxiliary;
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
    const auto state = gamepad::Normalized(input);
    auto layer = std::make_shared<engine::SpriteLayerSnapshot>();
    layer->atlas = sprites.atlas;
    if (!sprites.parts.empty() && std::any_of(sprites.parts.begin(), sprites.parts.end(),
                                              [](const auto& part) { return !part.knockout.empty(); })) {
        const std::scoped_lock lock(sprites.mask_mutex);
        const std::array position{state.left_x, state.left_y, state.right_x, state.right_y};
        if (!sprites.masked_atlas || sprites.masked_position != position) {
            auto atlas = std::make_shared<engine::SpriteAtlas>(*sprites.atlas);
            const auto& body = sprites.parts.front();
            for (const auto& part : sprites.parts) {
                if (part.knockout.empty())
                    continue;
                auto bounds = part.knockout_bounds;
                if (part.motion == PadMotion::LeftStick)
                    MoveStick(bounds, state.left_x, state.left_y, part);
                if (part.motion == PadMotion::RightStick)
                    MoveStick(bounds, state.right_x, state.right_y, part);
                const float left = (bounds.x - body.bounds.x) / body.bounds.w * body.idle.w;
                const float top = (bounds.y - body.bounds.y) / body.bounds.h * body.idle.h;
                const auto sample = [&](int x, int y) -> float {
                    if (x < 0 || y < 0 || x >= part.knockout_width || y >= part.knockout_height)
                        return 0;
                    return part.knockout[static_cast<size_t>(y) * part.knockout_width + x] / 255.0F;
                };
                for (int y = std::max(0, static_cast<int>(std::floor(top)));
                     y < std::min(body.idle.h, static_cast<int>(std::ceil(top + part.knockout_height))); ++y) {
                    for (int x = std::max(0, static_cast<int>(std::floor(left)));
                         x < std::min(body.idle.w, static_cast<int>(std::ceil(left + part.knockout_width))); ++x) {
                        auto& alpha =
                            atlas->pixels[(static_cast<size_t>(body.idle.y + y) * atlas->width + body.idle.x + x) * 4 +
                                          3];
                        if (alpha == 0)
                            continue;
                        const float u = x - left, v = y - top;
                        const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
                        const float fx = u - ix, fy = v - iy;
                        const float coverage = (sample(ix, iy) * (1 - fx) + sample(ix + 1, iy) * fx) * (1 - fy) +
                                               (sample(ix, iy + 1) * (1 - fx) + sample(ix + 1, iy + 1) * fx) * fy;
                        // Destination-out changes alpha only in a straight-alpha atlas.
                        alpha = static_cast<uint8_t>(std::lround(alpha * (1 - coverage)));
                    }
                }
            }
            atlas->generation = ++atlas_generation;
            sprites.masked_atlas = std::move(atlas);
            sprites.masked_position = position;
        }
        layer->atlas = sprites.masked_atlas;
    }
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

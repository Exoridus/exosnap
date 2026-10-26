#include "GamepadArtwork.h"

#include <QCoreApplication>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRectF>
#include <QRegularExpression>
#include <QStringList>
#include <QSvgRenderer>
#include <QTransform>

#include <algorithm>
#include <cmath>

namespace exosnap::quick {
namespace {

QDomElement Find(QDomElement node, const QString& id) {
    if (node.attribute(QStringLiteral("id")) == id)
        return node;
    for (auto child = node.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        const auto found = Find(child, id);
        if (!found.isNull())
            return found;
    }
    return {};
}

void Style(QDomElement node, const QString& key, const QString& value) {
    QStringList declarations;
    for (const auto& declaration : node.attribute(QStringLiteral("style")).split(';', Qt::SkipEmptyParts)) {
        if (declaration.section(':', 0, 0).trimmed() != key)
            declarations.push_back(declaration.trimmed());
    }
    declarations.push_back(key + ':' + value);
    node.setAttribute(QStringLiteral("style"), declarations.join(';'));
}

void StripIds(QDomElement node) {
    node.removeAttribute(QStringLiteral("id"));
    for (auto child = node.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
        StripIds(child);
}

void Identify(QDomElement node, int& sequence) {
    if (!node.hasAttribute(QStringLiteral("id")))
        node.setAttribute(QStringLiteral("id"), QStringLiteral("mapped-graphic-%1").arg(++sequence));
    for (auto child = node.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
        Identify(child, sequence);
}

QDomElement LargestGraphic(QDomElement node, const QSvgRenderer& renderer) {
    QDomElement largest;
    double area = -1;
    const auto visit = [&](auto&& self, QDomElement current) -> void {
        const auto tag = current.tagName();
        if (tag == QStringLiteral("path") || tag == QStringLiteral("ellipse") || tag == QStringLiteral("circle") ||
            tag == QStringLiteral("rect") || tag == QStringLiteral("polygon")) {
            const auto id = current.attribute(QStringLiteral("id"));
            const auto bounds = renderer.transformForElement(id).mapRect(renderer.boundsOnElement(id));
            const double candidate = bounds.width() * bounds.height();
            if (candidate > area) {
                largest = current;
                area = candidate;
            }
        }
        for (auto child = current.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
            self(self, child);
    };
    visit(visit, node);
    return largest;
}

QString FirstContour(QString path) {
    static const QRegularExpression move(QStringLiteral("[Mm]"));
    const auto next = move.match(path, 1);
    if (next.hasMatch())
        path.truncate(next.capturedStart());
    return path;
}

QString ButtonId(int mask) {
    switch (mask) {
    case 1:
        return QStringLiteral("btn-menu");
    case 2:
        return QStringLiteral("btn-view");
    case 4:
        return QStringLiteral("btn-a");
    case 8:
        return QStringLiteral("btn-b");
    case 16:
        return QStringLiteral("btn-x");
    case 32:
        return QStringLiteral("btn-y");
    case 64:
        return QStringLiteral("btn-up");
    case 128:
        return QStringLiteral("btn-down");
    case 256:
        return QStringLiteral("btn-left");
    case 512:
        return QStringLiteral("btn-right");
    case 1024:
        return QStringLiteral("btn-l1");
    case 2048:
        return QStringLiteral("btn-r1");
    default:
        return {};
    }
}

QString BindingId(const QJsonObject& binding) {
    const auto type = binding.value("type").toString();
    if (type == QStringLiteral("button"))
        return ButtonId(binding.value("mask").toInt());
    if (type == QStringLiteral("stick"))
        return binding.value("clickMask").toInt() == 4096 ? QStringLiteral("stick-l-cap")
                                                          : QStringLiteral("stick-r-cap");
    if (type == QStringLiteral("axis"))
        return binding.value("field").toString() == QStringLiteral("left_trigger") ? QStringLiteral("trigger-l2")
                                                                                   : QStringLiteral("trigger-r2");
    if (type == QStringLiteral("auxiliary")) {
        const auto field = binding.value("field").toString();
        if (field == QStringLiteral("home"))
            return QStringLiteral("btn-home");
        if (field == QStringLiteral("share"))
            return QStringLiteral("btn-share");
        if (field == QStringLiteral("touchpad"))
            return QStringLiteral("btn-touchpad");
    }
    return {};
}

void Clip(QDomDocument& document, QDomElement defs, QDomElement element, const QString& id,
          const QJsonObject& rectangle) {
    if (rectangle.isEmpty())
        return;
    auto clip = document.createElement(QStringLiteral("clipPath"));
    clip.setAttribute(QStringLiteral("id"), id);
    clip.setAttribute(QStringLiteral("clipPathUnits"), QStringLiteral("userSpaceOnUse"));
    auto rect = document.createElement(QStringLiteral("rect"));
    for (const auto& key :
         {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height")})
        rect.setAttribute(key, rectangle.value(key).toDouble());
    clip.appendChild(rect);
    defs.appendChild(clip);
    element.setAttribute(QStringLiteral("clip-path"), QStringLiteral("url(#%1)").arg(id));
}

void RasterizerMasks(QDomElement root, const QRectF& view) {
    // Qt SVG ignores clipPath. Equivalent white user-space masks preserve the
    // source geometry's clipping when rasterizing individual atlas elements.
    const auto clips = root.elementsByTagName(QStringLiteral("clipPath"));
    for (int index = clips.size() - 1; index >= 0; --index) {
        auto mask = clips.at(index).toElement();
        mask.setTagName(QStringLiteral("mask"));
        mask.removeAttribute(QStringLiteral("clipPathUnits"));
        mask.setAttribute(QStringLiteral("maskUnits"), QStringLiteral("userSpaceOnUse"));
        mask.setAttribute(QStringLiteral("maskContentUnits"), QStringLiteral("userSpaceOnUse"));
        mask.setAttribute(QStringLiteral("x"), view.x());
        mask.setAttribute(QStringLiteral("y"), view.y());
        mask.setAttribute(QStringLiteral("width"), view.width());
        mask.setAttribute(QStringLiteral("height"), view.height());
        const auto paint = [&](auto&& self, QDomElement node) -> void {
            Style(node, QStringLiteral("fill"), QStringLiteral("#ffffff"));
            Style(node, QStringLiteral("fill-opacity"), QStringLiteral("1"));
            Style(node, QStringLiteral("stroke"), QStringLiteral("none"));
            for (auto child = node.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
                self(self, child);
        };
        for (auto child = mask.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
            paint(paint, child);
    }
    const auto references = [&](auto&& self, QDomElement node) -> void {
        if (node.hasAttribute(QStringLiteral("clip-path"))) {
            node.setAttribute(QStringLiteral("mask"), node.attribute(QStringLiteral("clip-path")));
            node.removeAttribute(QStringLiteral("clip-path"));
        }
        for (auto child = node.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
            self(self, child);
    };
    references(references, root);
}

} // namespace

MappedGamepadArtwork PrepareMappedGamepadArtwork(const QString& mapping_path, QString& error) {
    MappedGamepadArtwork result;
    const auto fail = [&]() {
        error = QCoreApplication::translate("GamepadArtwork", "Invalid controller source or mapping.");
        return MappedGamepadArtwork{};
    };
    QFile mapping_file(mapping_path);
    if (!mapping_file.open(QIODevice::ReadOnly))
        return fail();
    const auto mapping = QJsonDocument::fromJson(mapping_file.readAll()).object();
    if (mapping.value("schemaVersion").toInt() != 1 ||
        mapping.value("source").toString() != QStringLiteral("source.svg"))
        return fail();
    QFile source(QFileInfo(mapping_path).dir().filePath(QStringLiteral("source.svg")));
    if (!source.open(QIODevice::ReadOnly))
        return fail();
    QDomDocument document;
    if (!document.setContent(source.readAll()))
        return fail();
    auto root = document.documentElement();
    int sequence = 0;
    Identify(root, sequence);
    QSvgRenderer renderer(document.toByteArray(-1));
    const auto view = renderer.viewBoxF();
    if (!renderer.isValid() || !std::isfinite(view.x()) || !std::isfinite(view.y()) || !std::isfinite(view.width()) ||
        !std::isfinite(view.height()) || view.width() <= 0 || view.height() <= 0)
        return fail();
    const double scale = std::min(480.0 / view.width(), 320.0 / view.height()) * 0.96;
    auto defs = root.firstChildElement(QStringLiteral("defs"));
    if (defs.isNull()) {
        defs = document.createElement(QStringLiteral("defs"));
        root.insertBefore(defs, root.firstChild());
    }
    QHash<QString, QDomElement> parts;
    for (auto child = root.firstChildElement(QStringLiteral("g")); !child.isNull();) {
        const auto next = child.nextSiblingElement(QStringLiteral("g"));
        parts.insert(child.attribute(QStringLiteral("id")), child);
        root.removeChild(child);
        child = next;
    }
    auto body = parts.value(QStringLiteral("body"));
    if (body.isNull())
        return fail();
    auto canvas = document.createElement(QStringLiteral("g"));
    canvas.setAttribute(QStringLiteral("transform"),
                        QStringLiteral("translate(%1 %2) scale(%3)")
                            .arg((480 - view.width() * scale) / 2 - view.x() * scale, 0, 'g', 16)
                            .arg((320 - view.height() * scale) / 2 - view.y() * scale, 0, 'g', 16)
                            .arg(scale, 0, 'g', 16));
    root.appendChild(canvas);
    root.setAttribute(QStringLiteral("viewBox"), QStringLiteral("0 0 480 320"));
    root.setAttribute(QStringLiteral("width"), 480);
    root.setAttribute(QStringLiteral("height"), 320);
    canvas.appendChild(body);

    const auto add = [&](QDomElement source_part, const QJsonObject& layer, const QString& id, const QString& region) {
        if (id.isEmpty())
            return false;
        const auto explicit_shape = layer.value("pressElement").toString();
        const auto shape =
            explicit_shape.isEmpty() ? LargestGraphic(source_part, renderer) : Find(source_part, explicit_shape);
        if (shape.isNull())
            return false;
        auto idle = source_part.cloneNode(true).toElement();
        auto pressed = source_part.cloneNode(true).toElement();
        if (layer.value("pressMode").toString() == QStringLiteral("tint")) {
            Style(Find(pressed, shape.attribute(QStringLiteral("id"))), QStringLiteral("fill"),
                  QStringLiteral("#4cb8ff"));
        } else {
            auto fill = shape.cloneNode(true).toElement();
            StripIds(fill);
            if (fill.tagName() == QStringLiteral("path"))
                fill.setAttribute(QStringLiteral("d"), FirstContour(fill.attribute(QStringLiteral("d"))));
            Style(fill, QStringLiteral("fill"), QStringLiteral("#4cb8ff"));
            Style(fill, QStringLiteral("fill-opacity"), QStringLiteral("0.36"));
            Style(fill, QStringLiteral("stroke"), QStringLiteral("none"));
            auto fill_group = document.createElement(QStringLiteral("g"));
            fill_group.appendChild(fill);
            Clip(document, defs, fill_group, QStringLiteral("pressure-%1").arg(id),
                 layer.value("pressClip").toObject());
            pressed.insertBefore(fill_group, pressed.firstChild());
        }
        StripIds(idle);
        StripIds(pressed);
        idle.setAttribute(QStringLiteral("id"), id);
        pressed.setAttribute(QStringLiteral("id"), id + QStringLiteral("-on"));
        if (!region.isEmpty()) {
            auto clip = document.createElement(QStringLiteral("clipPath"));
            clip.setAttribute(QStringLiteral("id"), QStringLiteral("region-%1").arg(id));
            clip.setAttribute(QStringLiteral("clipPathUnits"), QStringLiteral("userSpaceOnUse"));
            auto polygon = document.createElement(QStringLiteral("polygon"));
            polygon.setAttribute(QStringLiteral("points"), region);
            clip.appendChild(polygon);
            defs.appendChild(clip);
            for (auto element : {idle, pressed})
                element.setAttribute(QStringLiteral("clip-path"), QStringLiteral("url(#region-%1)").arg(id));
        }
        canvas.appendChild(idle);
        canvas.appendChild(pressed);
        return true;
    };

    for (const auto& value : mapping.value("layers").toArray()) {
        const auto layer = value.toObject();
        const auto source_id = layer.value("id").toString();
        if (source_id == QStringLiteral("body") || !layer.value("visible").toBool(true))
            continue;
        const auto part = parts.value(source_id);
        if (part.isNull())
            return fail();
        const auto binding = layer.value("binding").toObject();
        if (binding.value("type").toString() == QStringLiteral("directional-pad")) {
            const auto b = layer.value("bounds").toObject();
            const double x = b.value("x").toDouble(), y = b.value("y").toDouble();
            const double w = b.value("width").toDouble(), h = b.value("height").toDouble();
            const auto point = [](double x, double y) {
                return QStringLiteral("%1,%2").arg(x, 0, 'g', 16).arg(y, 0, 'g', 16);
            };
            const QString center = point(x + w / 2, y + h / 2);
            const QString corners[]{point(x, y), point(x + w, y), point(x + w, y + h), point(x, y + h)};
            const QString directions[]{QStringLiteral("up"), QStringLiteral("right"), QStringLiteral("down"),
                                       QStringLiteral("left")};
            for (int i = 0; i < 4; ++i)
                if (!add(part, layer, ButtonId(binding.value(directions[i]).toInt()),
                         center + ' ' + corners[i] + ' ' + corners[(i + 1) % 4]))
                    return fail();
            continue;
        }
        const auto id = BindingId(binding);
        if (id.isEmpty()) {
            body.appendChild(part);
            continue;
        }
        if (!add(part, layer, id, {}))
            return fail();
        if (binding.value("type").toString() == QStringLiteral("stick")) {
            const double travel = layer.value("motion").toObject().value("travelRadius").toDouble();
            if (!std::isfinite(travel) || travel <= 0 || travel > view.width())
                return fail();
            result.stick_travel.insert(id, QPointF(travel * scale / 480, travel * scale / 320));
        }
    }
    auto glass = body.cloneNode(true).toElement();
    const auto outer = LargestGraphic(body, renderer);
    if (!outer.isNull()) {
        auto surface = outer.cloneNode(true).toElement();
        if (surface.tagName() == QStringLiteral("path"))
            surface.setAttribute(QStringLiteral("d"), FirstContour(surface.attribute(QStringLiteral("d"))));
        Style(surface, QStringLiteral("fill"), QStringLiteral("#ffffff"));
        Style(surface, QStringLiteral("fill-opacity"), QStringLiteral("0.025"));
        Style(surface, QStringLiteral("stroke"), QStringLiteral("none"));
        glass.insertBefore(surface, glass.firstChild());
    }
    StripIds(glass);
    glass.setAttribute(QStringLiteral("id"), QStringLiteral("body-glass"));
    canvas.insertAfter(glass, body);
    RasterizerMasks(root, view);
    result.svg = document.toByteArray(-1);
    return result;
}

} // namespace exosnap::quick

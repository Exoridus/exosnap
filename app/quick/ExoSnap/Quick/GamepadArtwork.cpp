#include "GamepadArtwork.h"

#include <QColor>
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
QColor PressColor(const QJsonObject& layer) {
    return QColor(layer.value("pressColor").toString(QStringLiteral("#4cb8ff")));
}

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

gamepad::Control BindingControl(const QJsonObject& binding) {
    using gamepad::Control;
    const auto type = binding.value("type").toString();
    if (type == QStringLiteral("button"))
        return gamepad::ControlFromLegacyButton(binding.value("mask").toInt());
    if (type == QStringLiteral("stick")) {
        if (!binding.contains("axes")) {
            if (binding.value("clickMask").toInt() == 4096)
                return Control::LeftStick;
            if (binding.value("clickMask").toInt() == 8192)
                return Control::RightStick;
        }
        const auto axes = binding.value("axes").toArray();
        if (axes == QJsonArray{QStringLiteral("left_x"), QStringLiteral("left_y")})
            return Control::LeftStick;
        if (axes == QJsonArray{QStringLiteral("right_x"), QStringLiteral("right_y")})
            return Control::RightStick;
        return Control::Count;
    }
    const auto field = binding.value("field").toString();
    if (type == QStringLiteral("axis")) {
        if (field == QStringLiteral("left_trigger"))
            return Control::LeftTrigger;
        if (field == QStringLiteral("right_trigger"))
            return Control::RightTrigger;
    }
    if (type == QStringLiteral("auxiliary")) {
        if (field == QStringLiteral("home"))
            return Control::Guide;
        if (field == QStringLiteral("share"))
            return Control::Misc1;
        if (field == QStringLiteral("touchpad"))
            return Control::Touchpad;
    }
    return Control::Count;
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

void RasterizerMasks(QDomElement root) {
    // Qt SVG ignores clipPath. Equivalent white user-space masks preserve the
    // source geometry's clipping when rasterizing individual atlas elements.
    const auto clips = root.elementsByTagName(QStringLiteral("clipPath"));
    for (int index = clips.size() - 1; index >= 0; --index) {
        auto mask = clips.at(index).toElement();
        mask.setTagName(QStringLiteral("mask"));
        mask.removeAttribute(QStringLiteral("clipPathUnits"));
        mask.setAttribute(QStringLiteral("maskUnits"), QStringLiteral("objectBoundingBox"));
        mask.setAttribute(QStringLiteral("maskContentUnits"), QStringLiteral("userSpaceOnUse"));
        const auto paint = [&](auto&& self, QDomElement node) -> void {
            Style(node, QStringLiteral("fill"), QStringLiteral("#ffffff"));
            Style(node, QStringLiteral("fill-opacity"), QStringLiteral("1"));
            Style(node, QStringLiteral("stroke"), QStringLiteral("none"));
            if (node.hasAttribute(QStringLiteral("clip-rule")))
                Style(node, QStringLiteral("fill-rule"), node.attribute(QStringLiteral("clip-rule")));
            for (auto child = node.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
                self(self, child);
        };
        paint(paint, mask);
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
        const QColor press_color = PressColor(layer);
        if (!press_color.isValid())
            return false;
        const auto explicit_shape = layer.value("pressElement").toString();
        const auto shape =
            explicit_shape.isEmpty() ? LargestGraphic(source_part, renderer) : Find(source_part, explicit_shape);
        if (shape.isNull())
            return false;
        auto idle = source_part.cloneNode(true).toElement();
        auto pressed = source_part.cloneNode(true).toElement();
        if (layer.value("pressMode").toString() == QStringLiteral("tint")) {
            Style(Find(pressed, shape.attribute(QStringLiteral("id"))), QStringLiteral("fill"), press_color.name());
        } else {
            auto fill = shape.cloneNode(true).toElement();
            StripIds(fill);
            if (fill.tagName() == QStringLiteral("path"))
                fill.setAttribute(QStringLiteral("d"), FirstContour(fill.attribute(QStringLiteral("d"))));
            Style(fill, QStringLiteral("fill"), press_color.name());
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
        result.interactive_ids.push_back(id);
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
        const bool explicitly_unbound = layer.contains("binding") && layer.value("binding").isNull();
        if (!explicitly_unbound &&
            (binding.value("type").toString() == QStringLiteral("directional-pad") ||
             (layer.value("role").toString() == QStringLiteral("directional-pad") && layer.contains("controls")))) {
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
            for (int i = 0; i < 4; ++i) {
                const auto control = gamepad::ControlFromLegacyButton(binding.value(directions[i]).toInt());
                const auto controls = layer.value("controls").toObject();
                const auto canonical =
                    controls.contains(directions[i])
                        ? gamepad::ControlFromName(controls.value(directions[i]).toString().toStdString())
                        : control;
                const auto id = source_id + '-' + directions[i];
                if (canonical == gamepad::Control::Count ||
                    !add(part, layer, id, center + ' ' + corners[i] + ' ' + corners[(i + 1) % 4]))
                    return fail();
                result.controls.insert(id, {canonical, gamepad::Control::Count, true, PressColor(layer)});
            }
            continue;
        }
        const auto control = layer.contains("control")
                                 ? gamepad::ControlFromName(layer.value("control").toString().toStdString())
                                 : BindingControl(binding);
        if (layer.contains("control") && control == gamepad::Control::Count)
            return fail();
        if (explicitly_unbound || control == gamepad::Control::Count) {
            body.appendChild(part);
            continue;
        }
        const auto id = source_id;
        const auto click = layer.contains("clickControl")
                               ? gamepad::ControlFromName(layer.value("clickControl").toString().toStdString())
                               : gamepad::ControlFromLegacyButton(binding.value("clickMask").toInt());
        if (layer.contains("clickControl") && !layer.value("clickControl").isNull() && click == gamepad::Control::Count)
            return fail();
        if (!add(part, layer, id, {}))
            return fail();
        result.controls.insert(id, {control, click, true, PressColor(layer)});
        if (control == gamepad::Control::LeftStick || control == gamepad::Control::RightStick) {
            const auto knockout = layer.value("knockout").toString();
            if (!knockout.isEmpty()) {
                auto shape = Find(part, knockout);
                if (shape.isNull())
                    return fail();
                auto mask = shape.cloneNode(true).toElement();
                if (mask.tagName() == QStringLiteral("path"))
                    mask.setAttribute(QStringLiteral("d"), FirstContour(mask.attribute(QStringLiteral("d"))));
                Style(mask, QStringLiteral("fill"), QStringLiteral("#ffffff"));
                Style(mask, QStringLiteral("fill-opacity"), QStringLiteral("1"));
                Style(mask, QStringLiteral("opacity"), QStringLiteral("1"));
                Style(mask, QStringLiteral("stroke"), QStringLiteral("none"));
                for (auto parent = shape.parentNode().toElement(); !parent.isNull() && parent != part;
                     parent = parent.parentNode().toElement()) {
                    auto wrapper = parent.cloneNode(false).toElement();
                    wrapper.appendChild(mask);
                    mask = wrapper;
                }
                StripIds(mask);
                mask.setAttribute(QStringLiteral("id"), id + QStringLiteral("-knockout"));
                canvas.appendChild(mask);
            }
            const double travel = layer.value("motion").toObject().value("travelRadius").toDouble();
            if (!std::isfinite(travel) || travel <= 0 || travel > view.width())
                return fail();
            result.stick_travel.insert(id, QPointF(travel * scale / 480, travel * scale / 320));
        }
    }
    RasterizerMasks(root);
    result.svg = document.toByteArray(-1);
    return result;
}

} // namespace exosnap::quick

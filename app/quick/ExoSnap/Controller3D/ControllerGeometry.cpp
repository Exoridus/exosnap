#include "ControllerGeometry.h"

#include <QVector2D>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace exosnap::quick {
namespace {
struct Vertex {
    QVector3D position;
    QVector3D normal;
};
using Profile = std::vector<QVector2D>;
constexpr int kSegments = 64;

Profile StickProfile(ControllerGeometry::Part part, const StickDimensions& d) {
    const float top = d.shaft_height + d.cap_height;
    Profile p;
    switch (part) {
    case ControllerGeometry::Stem:
        p = {{0, 0}, {d.boot_radius, 0}};
        for (int i = 1; i <= 16; ++i) {
            const float a = std::acos(d.shaft_radius / d.boot_radius) * i / 16;
            p.emplace_back(d.boot_radius * std::cos(a), d.boot_radius * std::sin(a));
        }
        p.emplace_back(d.shaft_radius, d.shaft_height + .35F * d.cap_height);
        p.emplace_back(0, d.shaft_height + .35F * d.cap_height);
        break;
    case ControllerGeometry::Rim:
        p = {{d.shaft_radius, d.shaft_height}};
        for (int i = 0; i <= 12; ++i) {
            const float a = -std::numbers::pi_v<float> / 2 + i * std::numbers::pi_v<float> / 12;
            p.emplace_back(d.cap_radius * (.8F + .2F * std::cos(a)),
                           d.shaft_height + d.cap_height * (.5F + .5F * std::sin(a)));
        }
        p.emplace_back(d.ring_inner_radius, top - d.ring_thickness);
        p.emplace_back(d.ring_inner_radius, d.shaft_height);
        p.emplace_back(d.shaft_radius, d.shaft_height);
        break;
    case ControllerGeometry::Cap:
        p = {{0, d.shaft_height + .3F * d.cap_height},
             {d.ring_inner_radius, d.shaft_height + .3F * d.cap_height},
             {d.ring_inner_radius, top - d.ring_thickness}};
        for (int i = 1; i <= 16; ++i) {
            const float t = 1 - i / 16.0F;
            p.emplace_back(d.ring_inner_radius * t, top - d.ring_thickness - .24F * d.cap_height * (1 - t * t));
        }
        break;
    case ControllerGeometry::Well: {
        const auto inner = [&](float y) {
            return std::sqrt(std::max(0.0F, d.boot_radius * d.boot_radius - y * y)) +
                   (d.well_radius - d.boot_radius) * .2F;
        };
        p = {{inner(d.well_height - d.well_depth), d.well_height - d.well_depth},
             {d.well_radius, d.well_height - d.well_depth},
             {d.well_radius, d.well_height - .1F * d.well_depth},
             {.99F * d.well_radius, d.well_height},
             {inner(d.well_height), d.well_height}};
        for (int i = 1; i <= 12; ++i) {
            const float y = d.well_height - d.well_depth * i / 12;
            p.emplace_back(inner(y), y);
        }
        break;
    }
    case ControllerGeometry::Shell:
        break;
    }
    return p;
}

void Lathe(const Profile& profile, std::vector<Vertex>& vertices, std::vector<quint32>& indices) {
    for (size_t j = 1; j < profile.size(); ++j) {
        const QVector2D tangent = (profile[j] - profile[j - 1]).normalized();
        const auto first = static_cast<quint32>(vertices.size());
        for (int i = 0; i <= kSegments; ++i) {
            const float angle = 2 * std::numbers::pi_v<float> * i / kSegments;
            const float s = std::sin(angle), c = std::cos(angle);
            const QVector3D normal(tangent.y() * s, -tangent.x(), tangent.y() * c);
            for (const auto& point : {profile[j - 1], profile[j]})
                vertices.push_back({{point.x() * s, point.y(), point.x() * c}, normal});
            if (i < kSegments) {
                const auto a = first + static_cast<quint32>(2 * i);
                indices.insert(indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
            }
        }
    }
}

void BuildShell(std::vector<Vertex>& vertices, std::vector<quint32>& indices) {
    // The rearward fan center keeps triangles inside the concave grip outline.
    const Profile outline{{0, -.53F},     {.48F, -.53F},  {.70F, -.47F},  {.84F, -.31F}, {.92F, -.08F}, {1.0F, .43F},
                          {.97F, .60F},   {.86F, .69F},   {.75F, .65F},   {.50F, .29F},  {.30F, .23F},  {0, .23F},
                          {-.30F, .23F},  {-.50F, .29F},  {-.75F, .65F},  {-.86F, .69F}, {-.97F, .60F}, {-1.0F, .43F},
                          {-.92F, -.08F}, {-.84F, -.31F}, {-.70F, -.47F}, {-.48F, -.53F}};
    const auto triangle = [&](QVector3D a, QVector3D b, QVector3D c) {
        const auto n = QVector3D::crossProduct(b - a, c - a).normalized();
        const auto base = static_cast<quint32>(vertices.size());
        vertices.insert(vertices.end(), {{a, n}, {b, n}, {c, n}});
        indices.insert(indices.end(), {base, base + 1, base + 2});
    };
    for (size_t i = 0; i < outline.size(); ++i) {
        const auto a = outline[i], b = outline[(i + 1) % outline.size()];
        const QVector3D at(a.x() * .96F, .23F, a.y() * .96F);
        const QVector3D bt(b.x() * .96F, .23F, b.y() * .96F);
        const QVector3D am(a.x(), .13F, a.y()), bm(b.x(), .13F, b.y());
        const QVector3D ab(a.x() * .91F, -.08F, a.y() * .91F);
        const QVector3D bb(b.x() * .91F, -.08F, b.y() * .91F);
        triangle({0, .23F, -.46F}, bt, at);
        triangle(at, bt, am);
        triangle(am, bt, bm);
        triangle(am, bm, ab);
        triangle(ab, bm, bb);
        triangle({0, -.08F, -.46F}, ab, bb);
    }
}
} // namespace

ControllerGeometry::ControllerGeometry(QQuick3DObject* parent) : QQuick3DGeometry(parent) {
    Rebuild();
}
void ControllerGeometry::setPart(Part part) {
    if (part_ == part || part < Stem || part > Shell)
        return;
    part_ = part;
    Rebuild();
}
bool ControllerGeometry::SetDimensions(const StickDimensions& dimensions) {
    if (!ValidStickDimensions(dimensions))
        return false;
    if (dimensions_ != dimensions) {
        dimensions_ = dimensions;
        Rebuild();
    }
    return true;
}
void ControllerGeometry::Rebuild() {
    if (!ValidStickDimensions(dimensions_))
        return;
    std::vector<Vertex> vertices;
    std::vector<quint32> indices;
    if (part_ == Shell)
        BuildShell(vertices, indices);
    else
        Lathe(StickProfile(part_, dimensions_), vertices, indices);
    QVector3D minimum = vertices.front().position, maximum = minimum;
    for (const auto& vertex : vertices) {
        for (int i = 0; i < 3; ++i) {
            minimum[i] = std::min(minimum[i], vertex.position[i]);
            maximum[i] = std::max(maximum[i], vertex.position[i]);
        }
    }
    clear();
    setStride(sizeof(Vertex));
    setPrimitiveType(PrimitiveType::Triangles);
    addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
    addAttribute(Attribute::NormalSemantic, sizeof(QVector3D), Attribute::F32Type);
    addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
    setVertexData(QByteArray(reinterpret_cast<const char*>(vertices.data()),
                             static_cast<qsizetype>(vertices.size() * sizeof(Vertex))));
    setIndexData(QByteArray(reinterpret_cast<const char*>(indices.data()),
                            static_cast<qsizetype>(indices.size() * sizeof(quint32))));
    setBounds(minimum, maximum);
    ++generation_;
    update();
    emit geometryChanged();
}
} // namespace exosnap::quick

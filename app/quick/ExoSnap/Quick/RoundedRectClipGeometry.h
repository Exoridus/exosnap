#pragma once

// Rounded-rectangle mask geometry for a QSGClipNode.
//
// Both scene-graph items that present a GPU surface -- the Record preview and the
// editor player -- clip to the same rounded rect, and both had their own copy of
// this. Both copies emitted QSGGeometry::DrawTriangleFan, which the Qt Quick
// batch renderer does not support: qsg_topology() has cases only for Points,
// Lines, LineStrip, Triangles and TriangleStrip, so a fan hits the default arm,
// logs "Primitive topology 0x6 not supported" once per clip-batch rebuild, and
// then silently builds the pipeline as a triangle LIST.
//
// That fallback is not cosmetic. A 38-vertex fan consumed as a list becomes 12
// unrelated triangles with the last two vertices discarded, so the stencil mask
// degenerates into slivers instead of the intended rounded rect -- and
// updateClipState installs no scissor fallback for a non-rectangular clip.
// Emitting the triangles explicitly is the fix, and having one copy of it is
// what stops the two items drifting apart again.

#include <QRectF>
#include <QSGGeometry>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace exosnap::quick {

// Eight segments per corner: at the radii the product uses (10 px) that is well
// past the point where more segments are visible.
inline constexpr int kClipSegmentsPerCorner = 8;
inline constexpr int kClipBoundaryVertices = 4 * (kClipSegmentsPerCorner + 1);
// One triangle per boundary edge, fanned from the centre, three vertices each.
inline constexpr int kClipTriangleVertices = 3 * kClipBoundaryVertices;

// Fills `geometry` with a rounded-rect mask for `rect` as an explicit triangle
// list. `bounded_radius` must already be clamped to half the shorter side; the
// caller decides the degenerate cases (a zero radius is a rectangular clip and
// should never reach here).
inline void BuildRoundedRectClipGeometry(QSGGeometry* geometry, const QRectF& rect, qreal bounded_radius) {
    geometry->allocate(kClipTriangleVertices);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    QSGGeometry::Point2D* vertices = geometry->vertexDataAsPoint2D();

    const QPointF corner_centers[] = {
        {rect.right() - bounded_radius, rect.top() + bounded_radius},
        {rect.right() - bounded_radius, rect.bottom() - bounded_radius},
        {rect.left() + bounded_radius, rect.bottom() - bounded_radius},
        {rect.left() + bounded_radius, rect.top() + bounded_radius},
    };

    QSGGeometry::Point2D boundary[kClipBoundaryVertices];
    int index = 0;
    for (int corner = 0; corner < 4; ++corner) {
        const qreal start_degrees = -90.0 + 90.0 * corner;
        for (int segment = 0; segment <= kClipSegmentsPerCorner; ++segment) {
            const qreal radians = qDegreesToRadians(start_degrees + 90.0 * segment / kClipSegmentsPerCorner);
            boundary[index++].set(static_cast<float>(corner_centers[corner].x() + std::cos(radians) * bounded_radius),
                                  static_cast<float>(corner_centers[corner].y() + std::sin(radians) * bounded_radius));
        }
    }

    QSGGeometry::Point2D center;
    center.set(static_cast<float>(rect.center().x()), static_cast<float>(rect.center().y()));

    int vertex = 0;
    for (int edge = 0; edge < kClipBoundaryVertices; ++edge) {
        vertices[vertex++] = center;
        vertices[vertex++] = boundary[edge];
        // Wraps on the last edge, which is what closes the ring.
        vertices[vertex++] = boundary[(edge + 1) % kClipBoundaryVertices];
    }
}

} // namespace exosnap::quick

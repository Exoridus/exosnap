#include "ControllerGeometry.h"
#include "ControllerState.h"

namespace exosnap::quick {

ControllerGeometry::ControllerGeometry(QQuick3DObject* parent) : QQuick3DGeometry(parent) {
}

void ControllerGeometry::setController(ControllerState* controller) {
    if (controller_ == controller)
        return;
    controller_ = controller;
    Upload();
    emit sourceChanged();
}

void ControllerGeometry::setMesh(int mesh) {
    if (mesh_ == mesh)
        return;
    mesh_ = mesh;
    Upload();
    emit sourceChanged();
}

void ControllerGeometry::Upload() {
    clear();
    const auto& meshes = controller_ ? controller_->Model().meshes : std::vector<ControllerModelMesh>{};
    if (mesh_ >= 0 && static_cast<size_t>(mesh_) < meshes.size()) {
        const auto& source = meshes[static_cast<size_t>(mesh_)];
        setStride(6 * sizeof(float));
        setPrimitiveType(PrimitiveType::Triangles);
        addAttribute(Attribute::PositionSemantic, 0, Attribute::F32Type);
        addAttribute(Attribute::NormalSemantic, 3 * sizeof(float), Attribute::F32Type);
        addAttribute(Attribute::IndexSemantic, 0, Attribute::U32Type);
        setVertexData(source.vertices);
        setIndexData(source.indices);
        setBounds(source.minimum, source.maximum);
    }
    update();
}

} // namespace exosnap::quick

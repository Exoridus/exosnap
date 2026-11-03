import QtQuick
import QtQuick3D

Model {
    source: "#Sphere"
    position: Qt.vector3d(0.15, 0.05, -0.1)
    scale: Qt.vector3d(0.001, 0.001, 0.001)
    materials: PrincipledMaterial {
        lighting: PrincipledMaterial.NoLighting
        baseColor: "#ff00ff"
    }
}

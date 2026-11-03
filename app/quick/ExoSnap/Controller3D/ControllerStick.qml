import QtQuick
import QtQuick3D
import ExoSnap.Controller3D

Node {
    id: stick
    required property quaternion tilt
    required property real click
    required property ControllerGeometry stemGeometry
    required property ControllerGeometry rimGeometry
    required property ControllerGeometry capGeometry
    required property ControllerGeometry wellGeometry
    required property Material idleMaterial
    required property Material capMaterial

    Model {
        geometry: stick.wellGeometry
        materials: [stick.idleMaterial]
    }
    Node {
        rotation: stick.tilt
        Node {
            y: -stick.click
            Model {
                geometry: stick.stemGeometry
                materials: [stick.idleMaterial]
            }
            Model {
                geometry: stick.rimGeometry
                materials: [stick.capMaterial]
            }
            Model {
                geometry: stick.capGeometry
                materials: [stick.capMaterial]
            }
        }
    }
}

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick3D
import ExoSnap.Controller3D

View3D {
    id: controllerScene
    required property ControllerState controller
    property bool ghost: false
    property bool light: false
    property bool closeup: false
    property vector3d viewRotation: Qt.vector3d(0, 0, 0)

    environment: SceneEnvironment {
        backgroundMode: SceneEnvironment.Transparent
        antialiasingMode: SceneEnvironment.MSAA
        antialiasingQuality: SceneEnvironment.High
        depthTestEnabled: true
        depthPrePassEnabled: true
    }
    camera: pilotCamera
    OrthographicCamera {
        id: pilotCamera
        position: controllerScene.closeup ? Qt.vector3d(0, 368, 188) : Qt.vector3d(0, 340, 220)
        eulerRotation.x: -57.1
        clipNear: 1
        clipFar: 1000
        horizontalMagnification: Math.max(1, controllerScene.width) / (controllerScene.closeup ? 95 : (controllerScene.viewRotation.y === 0 ? 235 : 330))
        verticalMagnification: horizontalMagnification
    }
    DirectionalLight {
        eulerRotation: Qt.vector3d(-65, -25, 0)
        brightness: 1.2
        ambientColor: "#747f90"
    }
    DirectionalLight {
        eulerRotation: Qt.vector3d(-30, 150, 0)
        brightness: 0.45
    }
    ControllerGeometry { id: shell; part: ControllerGeometry.Shell }
    ControllerGeometry { id: stem; part: ControllerGeometry.Stem }
    ControllerGeometry { id: rim; part: ControllerGeometry.Rim }
    ControllerGeometry { id: cap; part: ControllerGeometry.Cap }
    ControllerGeometry { id: well; part: ControllerGeometry.Well }

    PrincipledMaterial {
        id: idle
        baseColor: controllerScene.ghost ? (controllerScene.light ? "#394d62" : "#aebed0") : "#242b35"
        roughness: 0.72
    }
    PrincipledMaterial {
        id: leftCap
        baseColor: controllerScene.controller.leftClick > 0 ? "#239fe8" : (controllerScene.ghost ? "#8299b3" : "#454d5b")
        roughness: 0.65
    }
    PrincipledMaterial {
        id: rightCap
        baseColor: controllerScene.controller.rightClick > 0 ? "#239fe8" : (controllerScene.ghost ? "#8299b3" : "#454d5b")
        roughness: 0.65
    }

    Node {
        objectName: "controllerRig"
        scale: Qt.vector3d(100, 100, 100)
        eulerRotation: controllerScene.viewRotation
        Model {
            objectName: "controllerShell"
            geometry: shell
            materials: PrincipledMaterial {
                baseColor: controllerScene.ghost ? (controllerScene.light ? "#405a76" : "#b3cee9") : "#e4e8ee"
                opacity: controllerScene.ghost ? 0.22 : 1
                roughness: 0.8
                depthDrawMode: Material.AlwaysDepthDraw
            }
        }
        ControllerStick {
            position: Qt.vector3d(-0.32, 0.2, -0.02)
            tilt: controllerScene.controller.leftRotation
            click: controllerScene.controller.leftClick
            stemGeometry: stem
            rimGeometry: rim
            capGeometry: cap
            wellGeometry: well
            idleMaterial: idle
            capMaterial: leftCap
        }
        ControllerStick {
            position: Qt.vector3d(0.32, 0.2, -0.02)
            tilt: controllerScene.controller.rightRotation
            click: controllerScene.controller.rightClick
            stemGeometry: stem
            rimGeometry: rim
            capGeometry: cap
            wellGeometry: well
            idleMaterial: idle
            capMaterial: rightCap
        }
        Repeater3D {
            model: controllerScene.controller.buttons
            delegate: Node {
                required property var modelData
                id: button
                position: modelData.position
                rotation: modelData.rotation
                Model {
                    source: button.modelData.round ? "#Cylinder" : "#Cube"
                    scale: button.modelData.size
                    z: button.modelData.hinged ? -0.08 : 0
                    materials: PrincipledMaterial {
                        baseColor: button.modelData.amount > 0 ? "#239fe8" : (controllerScene.ghost ? "#8299b3" : "#343d4b")
                        roughness: 0.65
                    }
                }
            }
        }
    }
}

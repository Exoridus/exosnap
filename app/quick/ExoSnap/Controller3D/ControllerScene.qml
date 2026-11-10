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

    readonly property vector3d viewDirection: Qt.vector3d(0, Math.sin(-controller.cameraPitch * Math.PI / 180),
                                                          Math.cos(controller.cameraPitch * Math.PI / 180))
    readonly property color ghostFill: light ? "#2f4459" : "#c4d4e6"
    readonly property color ghostRim: light ? "#1d3b5a" : "#e8f2ff"
    readonly property color ghostIdle: light ? "#41566c" : "#8ea3bb"

    environment: SceneEnvironment {
        backgroundMode: SceneEnvironment.Transparent
        antialiasingMode: SceneEnvironment.MSAA
        antialiasingQuality: SceneEnvironment.High
        depthTestEnabled: true
        depthPrePassEnabled: true
    }
    camera: fitCamera
    OrthographicCamera {
        id: fitCamera
        readonly property real distance: 1000
        position: (controllerScene.closeup ? controllerScene.controller.closeupCenter
                                           : controllerScene.controller.center)
                      .times(controllerScene.controller.sceneScale).plus(controllerScene.viewDirection.times(distance))
        eulerRotation.x: controllerScene.controller.cameraPitch
        clipNear: 1
        clipFar: 2 * distance
        horizontalMagnification: (controllerScene.closeup ? 3 : 0.92) * Math.min(
            Math.max(1, controllerScene.width) / (controllerScene.controller.fitWidth * controllerScene.controller.sceneScale),
            Math.max(1, controllerScene.height) / (controllerScene.controller.fitHeight * controllerScene.controller.sceneScale))
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

    Node {
        objectName: "controllerRig"
        scale: Qt.vector3d(controllerScene.controller.sceneScale, controllerScene.controller.sceneScale,
                           controllerScene.controller.sceneScale)
        pivot: controllerScene.controller.center
        eulerRotation: controllerScene.viewRotation
        Repeater3D {
            model: controllerScene.controller.parts
            delegate: Model {
                id: part
                required property int mesh
                required property vector3d partPosition
                required property quaternion partRotation
                required property real partScale
                required property color baseColor
                required property color pressColor
                required property bool active
                required property bool housing

                objectName: housing ? "controllerShell" : "controllerControl"
                geometry: ControllerGeometry {
                    controller: controllerScene.controller
                    mesh: part.mesh
                }
                position: partPosition
                rotation: partRotation
                scale: Qt.vector3d(partScale, partScale, partScale)
                materials: [part.active ? activeMaterial
                                        : controllerScene.ghost ? (part.housing ? shellMaterial : ghostMaterial)
                                                                : normalMaterial]

                PrincipledMaterial {
                    id: normalMaterial
                    baseColor: part.baseColor
                    roughness: 0.68
                    metalness: 0
                }
                PrincipledMaterial {
                    id: activeMaterial
                    baseColor: part.pressColor
                    emissiveFactor: Qt.vector3d(0.25, 0.25, 0.25)
                    roughness: 0.5
                    metalness: 0
                }
                // Ghost controls stay closed and opaque so they hide the contours behind them.
                PrincipledMaterial {
                    id: ghostMaterial
                    baseColor: controllerScene.ghostIdle
                    roughness: 0.65
                    metalness: 0
                }
                CustomMaterial {
                    id: shellMaterial
                    property color fillColor: controllerScene.ghostFill
                    property color rimColor: controllerScene.ghostRim
                    shadingMode: CustomMaterial.Shaded
                    // Custom materials blend only when asked; alpha stays premultiplied-compatible for the host.
                    sourceBlend: CustomMaterial.SrcAlpha
                    destinationBlend: CustomMaterial.OneMinusSrcAlpha
                    sourceAlphaBlend: CustomMaterial.One
                    destinationAlphaBlend: CustomMaterial.OneMinusSrcAlpha
                    cullMode: Material.BackFaceCulling
                    depthDrawMode: Material.AlwaysDepthDraw
                    fragmentShader: "ghost_shell.frag"
                }
            }
        }
    }
}

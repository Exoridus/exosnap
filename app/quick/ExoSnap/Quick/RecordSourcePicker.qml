pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Popup {
    id: root

    required property RecordViewModelAdapter recordViewModel

    parent: Overlay.overlay
    width: Math.min(680, parent ? parent.width - 48 : 680)
    height: Math.min(560, parent ? parent.height - 48 : 560)
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: ExoTheme.spacingXl

    background: Rectangle {
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: ExoTheme.spacingLg

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: qsTr("Choose capture source")
                textFormat: Text.PlainText
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontPageTitle
                    weight: Font.DemiBold
                }
            }

            ExoButton {
                text: qsTr("Close")
                quiet: true
                onClicked: root.close()
            }
        }

        Label {
            text: qsTr("Displays")
            textFormat: Text.PlainText
            color: ExoTheme.textMuted
            font.family: ExoTheme.monoFamily
            font.pixelSize: ExoTheme.fontEyebrow
        }

        Flow {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            Repeater {
                model: root.recordViewModel.displayTargetOptions

                delegate: Component {
                    Column {
                        id: displayDelegate

                        required property string label
                        required property int targetIndex

                        width: 198
                        spacing: ExoTheme.spacingXs

                        ExoButton {
                            width: parent.width
                            text: displayDelegate.label
                            selectable: true
                            selected: root.recordViewModel.selectedTargetIndex === displayDelegate.targetIndex
                                      && root.recordViewModel.captureMode === 0
                            onClicked: {
                                root.recordViewModel.requestSelectTarget(displayDelegate.targetIndex, 0)
                                root.close()
                            }
                        }

                        ExoButton {
                            width: parent.width
                            text: qsTr("Region on %1").arg(displayDelegate.label)
                            quiet: true
                            selectable: true
                            selected: root.recordViewModel.selectedTargetIndex === displayDelegate.targetIndex
                                      && root.recordViewModel.captureMode === 2
                            onClicked: {
                                root.recordViewModel.requestSelectTarget(displayDelegate.targetIndex, 2)
                                root.close()
                            }
                        }
                    }
                }
            }
        }

        Label {
            text: qsTr("Application windows")
            textFormat: Text.PlainText
            color: ExoTheme.textMuted
            font.family: ExoTheme.monoFamily
            font.pixelSize: ExoTheme.fontEyebrow
        }

        ListView {
            id: windowList

            model: root.recordViewModel.windowTargetOptions
            spacing: ExoTheme.spacingXs
            boundsBehavior: Flickable.StopAtBounds
            Layout.fillWidth: true
            Layout.fillHeight: true

            delegate: Component {
                ExoButton {
                    id: windowDelegate

                    required property string label
                    required property int targetIndex

                    width: ListView.view.width
                    text: windowDelegate.label
                    quiet: true
                    selectable: true
                    selected: root.recordViewModel.selectedTargetIndex === windowDelegate.targetIndex
                              && root.recordViewModel.captureMode === 1
                    onClicked: {
                        root.recordViewModel.requestSelectTarget(windowDelegate.targetIndex, 1)
                        root.close()
                    }
                }
            }
        }
    }
}

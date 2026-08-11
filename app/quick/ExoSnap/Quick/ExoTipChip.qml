pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The ONE quiet chip that Tier-3 optimisations bundle into ("better, but it runs").
// Never a warning colour: an optimisation is not a problem, and colouring it as one
// is exactly the alarmism the diagnostics ethos rules out.
Rectangle {
    id: root

    required property var tips
    // Simple view keeps the bundle collapsed; Expert lists them open.
    property bool defaultOpen: false
    property bool expanded: root.defaultOpen

    signal applyFixRequested(string fixId)
    signal assistedFixRequested(string fixId)

    visible: root.tips.length > 0
    implicitHeight: column.implicitHeight + 2 * ExoTheme.spacingMd
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusMd

    onDefaultOpenChanged: root.expanded = root.defaultOpen

    ColumnLayout {
        id: column

        spacing: ExoTheme.spacingSm
        anchors {
            fill: parent
            topMargin: ExoTheme.spacingMd
            bottomMargin: ExoTheme.spacingMd
            leftMargin: ExoTheme.spacingLg
            rightMargin: ExoTheme.spacingLg
        }

        AbstractButton {
            id: header

            implicitHeight: 20
            hoverEnabled: true
            Layout.fillWidth: true
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Tips")
            onClicked: root.expanded = !root.expanded

            contentItem: RowLayout {
                spacing: ExoTheme.spacingSm

                Label {
                    text: root.expanded ? "▾" : "▸"
                    textFormat: Text.PlainText
                    color: ExoTheme.textMuted
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 11
                    }
                }

                Label {
                    text: root.tips.length === 1 ? qsTr("1 tip") : qsTr("%1 tips").arg(root.tips.length)
                    textFormat: Text.PlainText
                    color: ExoTheme.accent
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 12
                        weight: Font.DemiBold
                    }
                }

                Label {
                    text: qsTr("optional improvements")
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: ExoTheme.textMuted
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 11
                    }
                }
            }
        }

        Repeater {
            model: root.expanded ? root.tips : []

            RowLayout {
                id: tipRow

                required property var modelData

                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                Label {
                    text: tipRow.modelData.summary ?? ""
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: ExoTheme.textSecondary
                    Layout.fillWidth: true
                    Layout.minimumHeight: 15
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 12
                    }
                }

                ExoButton {
                    text: (tipRow.modelData.fixSafety === 1 ? (tipRow.modelData.fixLabel ?? "") + " →"
                                                            : (tipRow.modelData.fixLabel ?? ""))
                    visible: (tipRow.modelData.hasFix ?? false) && tipRow.modelData.fixSafety !== 2
                    quiet: true
                    Layout.alignment: Qt.AlignVCenter
                    onClicked: {
                        if (tipRow.modelData.fixSafety === 0) {
                            root.applyFixRequested(tipRow.modelData.fixId);
                        } else {
                            root.assistedFixRequested(tipRow.modelData.fixId);
                        }
                    }
                }
            }
        }
    }
}

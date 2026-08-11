pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One worst-first entry card: severity glyph + title + mono id chip + optional
// "Elev" lock badge, the summary, the FixAction control for its safety class, and
// a collapsed L2/L3 evidence disclosure (Measured → Why → Log excerpt).
//
// The card decides nothing: tone, cap, fix safety and evidence presence all arrive
// as already-resolved roles from diagnostics::BuildTopIssues.
Rectangle {
    id: root

    required property string issueId
    required property string tone
    required property string title
    required property string summary
    property string why: ""
    property string measured: ""
    property string logExcerpt: ""
    property bool needsElevation: false
    property bool hasEvidence: false
    property bool hasFix: false
    property string fixId: ""
    property string fixLabel: ""
    // 0 = Auto (button + confirm), 1 = Assisted (navigates), 2 = External (label only).
    property int fixSafety: 0

    signal applyFixRequested(string fixId)
    signal assistedFixRequested(string fixId)

    readonly property color toneColor: root.tone === "blocker" ? ExoTheme.error
                                     : root.tone === "notice" ? ExoTheme.warning
                                     : ExoTheme.success

    implicitHeight: column.implicitHeight + 2 * ExoTheme.spacingMd
    color: root.tone === "blocker" ? ExoTheme.errorSurface
         : root.tone === "notice" ? ExoTheme.warningSurface
         : ExoTheme.surface
    border.width: 1
    border.color: root.toneColor
    radius: ExoTheme.radiusLg

    Accessible.role: Accessible.StaticText
    Accessible.name: root.title

    ColumnLayout {
        id: column

        spacing: ExoTheme.spacingXs
        anchors {
            fill: parent
            topMargin: ExoTheme.spacingMd
            bottomMargin: ExoTheme.spacingMd
            leftMargin: ExoTheme.spacingLg
            rightMargin: ExoTheme.spacingLg
        }

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            Label {
                text: root.tone === "blocker" ? "✗" : root.tone === "notice" ? "⚠" : "✓"
                textFormat: Text.PlainText
                color: root.toneColor
                Layout.alignment: Qt.AlignTop
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: 13
                }
            }

            Label {
                text: root.title
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: ExoTheme.text
                Layout.fillWidth: true
                Layout.minimumHeight: 16
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: 13
                    weight: Font.DemiBold
                }
            }

            ExoBadge {
                text: root.issueId
                mono: true
                visible: root.issueId !== ""
                Layout.alignment: Qt.AlignTop
            }

            ExoBadge {
                text: qsTr("Elev")
                tone: "notice"
                visible: root.needsElevation
                Layout.alignment: Qt.AlignTop
                ToolTip.text: qsTr("Measured from the elevated present-path baseline (PresentMon / DPC-ISR).")
                ToolTip.visible: elevHover.hovered
                ToolTip.delay: 400

                HoverHandler {
                    id: elevHover
                }
            }
        }

        Label {
            text: root.summary
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.summary !== ""
            color: ExoTheme.textSecondary
            Layout.fillWidth: true
            Layout.minimumHeight: 15
            font {
                family: ExoTheme.sansFamily
                pixelSize: 12
            }
        }

        ExoButton {
            text: root.fixSafety === 1 ? root.fixLabel + " →" : root.fixLabel
            visible: root.hasFix && root.fixSafety !== 2
            quiet: true
            Layout.alignment: Qt.AlignLeft
            Layout.topMargin: ExoTheme.spacingXs
            onClicked: {
                if (root.fixSafety === 0) {
                    root.applyFixRequested(root.fixId);
                } else {
                    root.assistedFixRequested(root.fixId);
                }
            }
        }

        // External fixes cannot be performed by the app (a driver install, say), so
        // they are stated and given no control at all.
        Label {
            text: root.fixLabel
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.hasFix && root.fixSafety === 2
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: 11
            }
        }

        ExoDisclosure {
            title: qsTr("Evidence")
            visible: root.hasEvidence
            Layout.fillWidth: true
            Layout.topMargin: ExoTheme.spacingXs

            body: Component {
                ColumnLayout {
                    spacing: ExoTheme.spacingXs

                    Repeater {
                        model: [
                            { "label": qsTr("Measured"), "value": root.measured },
                            { "label": qsTr("Why"), "value": root.why },
                            { "label": qsTr("Log excerpt"), "value": root.logExcerpt }
                        ]

                        ColumnLayout {
                            id: evidenceRow

                            required property var modelData

                            spacing: 2
                            visible: (evidenceRow.modelData.value ?? "").trim() !== ""
                            Layout.fillWidth: true

                            Label {
                                text: evidenceRow.modelData.label
                                textFormat: Text.PlainText
                                color: ExoTheme.textMuted
                                Layout.fillWidth: true
                                font {
                                    family: ExoTheme.monoFamily
                                    pixelSize: 9
                                    letterSpacing: 1
                                    weight: Font.DemiBold
                                }
                            }

                            Label {
                                text: evidenceRow.modelData.value
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                color: ExoTheme.textSecondary
                                Layout.fillWidth: true
                                font {
                                    family: ExoTheme.monoFamily
                                    pixelSize: 11
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

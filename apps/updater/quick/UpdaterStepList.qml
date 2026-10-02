import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ExoSnap.Updater

// The five fixed canon phases, top to bottom. The labels, the state keys and the
// tag words all come from the adapter, which builds them from the same
// UpdaterController state the Widgets checklist was rendered from.
Column {
    id: root

    required property var rows
    // The colour a failed row carries: warning for an Amber terminal, error for
    // Red, success for the Green soft-success. Presentation only -- the
    // controller's own StepStatus is untouched.
    required property string failTone

    spacing: 0

    function toneColor(tone) {
        switch (tone) {
        case "success":
            return ExoTheme.success;
        case "warning":
            return ExoTheme.warning;
        case "error":
            return ExoTheme.error;
        default:
            return ExoTheme.accent;
        }
    }

    Repeater {
        model: root.rows

        delegate: Item {
            id: row

            required property var modelData
            required property int index

            width: root.width
            height: 39

            readonly property string status: row.modelData.status
            readonly property bool manual: row.modelData.manual
            readonly property color failColor: root.toneColor(root.failTone === "accent" ? "warning" : root.failTone)

            Accessible.role: Accessible.ListItem
            Accessible.name: row.modelData.accessible

            Rectangle {
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                }
                height: 1
                visible: row.index > 0
                color: ExoTheme.line
            }

            Item {
                id: glyphSlot

                anchors {
                    left: parent.left
                    leftMargin: ExoTheme.spacingLg
                    verticalCenter: parent.verticalCenter
                }
                width: 22
                height: 22

                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    visible: row.status === "done" || row.status === "failed"
                    color: {
                        if (row.status === "done")
                            return ExoTheme.accent;
                        if (row.manual)
                            return "transparent";
                        return Qt.tint(ExoTheme.surface, Qt.rgba(row.failColor.r, row.failColor.g,
                                                                row.failColor.b, 0.13));
                    }
                    border.width: row.status === "failed" && !row.manual ? 1 : row.manual ? 2 : 0
                    border.color: row.manual ? row.failColor : row.failColor
                }

                ExoGlyph {
                    anchors.centerIn: parent
                    width: 12
                    height: 12
                    visible: row.status === "done" || (row.status === "failed" && !row.manual)
                    kind: row.status === "done" ? ExoGlyph.Check : ExoGlyph.Close
                    color: row.status === "done" ? ExoTheme.accentInk : row.failColor
                }

                UpdaterGlyph {
                    anchors.fill: parent
                    visible: row.status === "working"
                    glyph: "spinner"
                    color: ExoTheme.accent
                }

                Rectangle {
                    anchors.centerIn: parent
                    visible: row.status === "queued"
                    width: 22
                    height: 22
                    radius: width / 2
                    color: "transparent"
                    border.width: 1
                    border.color: ExoTheme.lineStrong

                    Rectangle {
                        anchors.centerIn: parent
                        width: 5
                        height: 5
                        radius: width / 2
                        color: ExoTheme.textDim
                    }
                }
            }

            Label {
                objectName: "updaterStepLabel"

                anchors {
                    left: glyphSlot.right
                    leftMargin: ExoTheme.spacingMd
                    right: tagLabel.left
                    rightMargin: ExoTheme.spacingSm
                    verticalCenter: parent.verticalCenter
                }
                text: row.modelData.label
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: row.status === "queued" ? ExoTheme.textDim : ExoTheme.text
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                    weight: row.status === "working" || row.status === "failed" ? Font.DemiBold : Font.Medium
                }
                ToolTip.text: row.modelData.accessible
                ToolTip.visible: stepHover.hovered && labelTruncated
                ToolTip.delay: 400

                readonly property bool labelTruncated: contentWidth > width
            }

            HoverHandler {
                id: stepHover
            }

            Label {
                id: tagLabel

                objectName: "updaterStepTag"

                anchors {
                    right: parent.right
                    rightMargin: ExoTheme.spacingLg
                    verticalCenter: parent.verticalCenter
                }
                text: row.modelData.tag
                textFormat: Text.PlainText
                color: {
                    switch (row.status) {
                    case "done":
                        return ExoTheme.textMuted;
                    case "working":
                        return ExoTheme.accent;
                    case "failed":
                        return row.failColor;
                    default:
                        return ExoTheme.textDim;
                    }
                }
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontCaption
                    weight: Font.Medium
                }
            }
        }
    }
}

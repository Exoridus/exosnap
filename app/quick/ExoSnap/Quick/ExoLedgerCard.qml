pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One session-ledger entry (spec section 2/3): a 44 px amber-outlined row while
// collapsed, or the full evidence card once expanded. The amber outline marks
// "measured this session" either way; the ground fills amber only while the
// check is firing right now (`active`) -- a settled entry keeps the plain
// surface ground even expanded, same as the row it collapses back to.
Rectangle {
    id: root

    required property string entryId
    required property string title
    required property string summary
    required property bool active
    required property int count
    // Bare, already-localized fragments -- e.g. "15:52:40", "40 s ago", "13.9 ms",
    // "16.67 ms budget", "14 s". The card supplies the surrounding words ("first",
    // "worst", the multiplication sign); no clock or duration arithmetic happens
    // here.
    required property string firstSeenText
    required property string lastSeenText
    required property string worstText
    required property string budgetText
    required property string totalActiveText
    property string logExcerpt: ""
    // [{ startMs: int, text: string }], time already formatted by the caller.
    property var occurrences: []
    property bool expanded: false

    signal showInLogRequested(string entryId)
    signal openAtRequested(int startMs)

    property bool _logExpanded: false

    readonly property string ledgerLine: {
        const parts = [root.count + "× " + qsTr("observed")];
        if (root.firstSeenText !== "")
            parts.push(qsTr("first %1").arg(root.firstSeenText));
        if (root.active)
            parts.push(qsTr("active for %1").arg(root.totalActiveText));
        else if (root.lastSeenText !== "")
            parts.push(qsTr("last %1").arg(root.lastSeenText));
        if (root.worstText !== "")
            parts.push(qsTr("worst %1 / %2").arg(root.worstText).arg(root.budgetText));
        return parts.join(" · ");
    }

    implicitWidth: 400
    implicitHeight: root.expanded ? column.implicitHeight + 2 * ExoTheme.spacingMd : 44
    color: root.active ? ExoTheme.warningSurface : ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.warning
    radius: ExoTheme.radiusLg
    clip: !root.expanded

    Accessible.role: Accessible.StaticText
    Accessible.name: ExoTheme.advisoryToneName("caution") + ". " + root.title

    // Collapsed: one clickable row. Expanded: only the header toggles back, so
    // the occurrence links and the log-excerpt disclosure below stay reachable.
    MouseArea {
        anchors.fill: parent
        enabled: !root.expanded
        onClicked: root.expanded = true
    }

    ColumnLayout {
        id: column

        spacing: ExoTheme.spacingXs
        anchors {
            fill: parent
            topMargin: root.expanded ? ExoTheme.spacingMd : 0
            bottomMargin: root.expanded ? ExoTheme.spacingMd : 0
            leftMargin: ExoTheme.spacingLg
            rightMargin: ExoTheme.spacingLg
        }

        RowLayout {
            id: header

            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            ExoGlyph {
                kind: ExoGlyph.Warning
                color: ExoTheme.warningText
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
            }

            Label {
                text: root.title
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                    weight: Font.DemiBold
                }
            }

            Label {
                text: root.ledgerLine
                textFormat: Text.PlainText
                visible: !root.expanded
                elide: Text.ElideRight
                color: ExoTheme.textMuted
                Layout.alignment: Qt.AlignVCenter
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }

            ExoBadge {
                text: qsTr("now")
                tone: "notice"
                visible: root.active
                Layout.alignment: Qt.AlignVCenter
            }

            ExoBadge {
                text: qsTr("quiet")
                tone: "pass"
                visible: !root.active && !root.expanded
                Layout.alignment: Qt.AlignVCenter
            }

            ExoBadge {
                text: root.entryId
                mono: true
                Layout.alignment: Qt.AlignVCenter
            }

            // The header alone toggles collapse; MouseArea above only opens it.
            TapHandler {
                enabled: root.expanded
                onTapped: root.expanded = false
            }
        }

        Label {
            text: root.ledgerLine
            textFormat: Text.PlainText
            visible: root.expanded
            wrapMode: Text.WordWrap
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontCaption
            }
        }

        Label {
            text: root.summary
            textFormat: Text.PlainText
            visible: root.expanded && root.summary !== ""
            wrapMode: Text.WordWrap
            color: ExoTheme.textSecondary
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        RowLayout {
            spacing: ExoTheme.spacingSm
            visible: root.expanded && root.occurrences.length > 0
            Layout.fillWidth: true

            Repeater {
                model: root.occurrences

                Text {
                    id: occurrenceLink

                    required property var modelData

                    objectName: "ledgerOccurrenceLink"
                    text: occurrenceLink.modelData.text
                    textFormat: Text.PlainText
                    color: ExoTheme.accent
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: ExoTheme.fontCaption
                        underline: true
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }

                    TapHandler {
                        onTapped: root.openAtRequested(occurrenceLink.modelData.startMs)
                    }
                }
            }
        }

        RowLayout {
            spacing: ExoTheme.spacingSm
            visible: root.expanded && root.logExcerpt !== ""
            Layout.fillWidth: true
            Layout.topMargin: ExoTheme.spacingXs

            ExoChevron {
                direction: root._logExpanded ? 0 : -90
                tone: ExoTheme.textMuted
                Layout.preferredWidth: 10

                Behavior on rotation {
                    NumberAnimation { duration: ExoTheme.animMedium; easing.type: Easing.OutCubic }
                }
            }

            Label {
                text: qsTr("Log excerpt")
                textFormat: Text.PlainText
                color: ExoTheme.textSecondary
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                    weight: Font.DemiBold
                }

                TapHandler {
                    onTapped: root._logExpanded = !root._logExpanded
                }
            }

            Item { Layout.fillWidth: true }

            Text {
                text: qsTr("Show in log →")
                textFormat: Text.PlainText
                color: ExoTheme.accent
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                    weight: Font.DemiBold
                }

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }

                TapHandler {
                    onTapped: root.showInLogRequested(root.entryId)
                }
            }
        }

        Label {
            text: root.logExcerpt
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.expanded && root._logExpanded && root.logExcerpt !== ""
            color: ExoTheme.textSecondary
            Layout.fillWidth: true
            Layout.leftMargin: ExoTheme.spacingLg
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontCaption
            }
        }
    }
}

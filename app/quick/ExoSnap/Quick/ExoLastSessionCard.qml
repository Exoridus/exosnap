pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The After-Stop surface's "Last session" section (spec section 5): what was
// recorded, the four headline facts, the session timeline, the frozen ledger
// worst-first with the first entry expanded, and the follow-up actions.
// Replaces the old "Last session" readiness tile.
ColumnLayout {
    id: root

    // The adapter's session summary map:
    //   headerText: string, e.g. "Recording saved · 2 problems observed" --
    //     already pluralized, so QML never counts problems itself.
    //   fileName: string
    //   durationMs: int
    //   facts: [{ label, value, valueTone: "ok"|"warn"|"critical"|"neutral", sub }],
    //          exactly 4 -- Frames dropped, Achieved fps, A/V drift, File.
    //   marks: ExoSessionTimeline.marks
    //   ledgerEntries: ExoLedgerCard's own props per entry, worst first.
    required property var session
    // 4 on a regular page, 2 once the column is too narrow for four -- the
    // caller decides, the same way DiagnosticsPage decides tileColumns.
    property int columns: 4

    signal showInFolderRequested()
    signal openEditRequested()
    signal openEditAtRequested(int positionMs)
    signal viewLogRequested()
    signal showInLogRequested(string entryId)

    function _toneColor(tone: string): color {
        return tone === "ok" ? ExoTheme.success
             : tone === "warn" ? ExoTheme.warning
             : tone === "critical" ? ExoTheme.error
             : ExoTheme.text;
    }

    spacing: ExoTheme.spacingMd

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: card.implicitHeight
        color: ExoTheme.surface
        border.width: 1
        border.color: ExoTheme.line
        radius: ExoTheme.radiusLg

        ColumnLayout {
            id: card

            spacing: ExoTheme.spacingMd
            anchors {
                fill: parent
                margins: ExoTheme.cardPadding
            }

            RowLayout {
                Layout.fillWidth: true

                Label {
                    objectName: "lastSessionHeader"
                    text: root.session.headerText
                    textFormat: Text.PlainText
                    color: ExoTheme.text
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontSectionTitle
                        weight: Font.DemiBold
                    }
                }

                Label {
                    text: root.session.fileName
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: ExoTheme.textMuted
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: ExoTheme.fontCaption
                    }
                }
            }

            GridLayout {
                objectName: "lastSessionFacts"
                columns: root.columns
                rowSpacing: ExoTheme.spacingSm
                columnSpacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                Repeater {
                    model: root.session.facts

                    Rectangle {
                        id: fact

                        required property var modelData

                        Layout.fillWidth: true
                        Layout.preferredHeight: factColumn.implicitHeight + 2 * ExoTheme.spacingSm
                        color: ExoTheme.surfaceRaised
                        border.width: 1
                        border.color: ExoTheme.line
                        radius: ExoTheme.radiusMd

                        ColumnLayout {
                            id: factColumn

                            spacing: 2
                            anchors {
                                fill: parent
                                margins: ExoTheme.spacingSm
                            }

                            Label {
                                text: fact.modelData.label.toUpperCase()
                                textFormat: Text.PlainText
                                color: ExoTheme.textMuted
                                Layout.fillWidth: true
                                font {
                                    family: ExoTheme.monoFamily
                                    pixelSize: ExoTheme.fontEyebrow
                                    letterSpacing: 1
                                    weight: Font.DemiBold
                                }
                            }

                            Label {
                                text: fact.modelData.value
                                textFormat: Text.PlainText
                                color: root._toneColor(fact.modelData.valueTone)
                                Layout.fillWidth: true
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontSectionTitle
                                    weight: Font.DemiBold
                                }
                            }

                            Label {
                                text: fact.modelData.sub
                                textFormat: Text.PlainText
                                visible: fact.modelData.sub !== ""
                                color: ExoTheme.textDim
                                Layout.fillWidth: true
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontCaption
                                }
                            }
                        }
                    }
                }
            }

            ExoSessionTimeline {
                durationMs: root.session.durationMs
                marks: root.session.marks
                Layout.fillWidth: true

                onOpenAtRequested: (positionMs) => root.openEditAtRequested(positionMs)
            }

            ColumnLayout {
                objectName: "lastSessionLedger"
                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                Repeater {
                    model: root.session.ledgerEntries

                    ExoLedgerCard {
                        id: ledgerDelegate

                        required property var modelData
                        required property int index

                        Layout.fillWidth: true
                        entryId: ledgerDelegate.modelData.entryId
                        title: ledgerDelegate.modelData.title
                        summary: ledgerDelegate.modelData.summary
                        active: ledgerDelegate.modelData.active
                        count: ledgerDelegate.modelData.count
                        firstSeenText: ledgerDelegate.modelData.firstSeenText
                        lastSeenText: ledgerDelegate.modelData.lastSeenText
                        worstText: ledgerDelegate.modelData.worstText
                        budgetText: ledgerDelegate.modelData.budgetText
                        totalActiveText: ledgerDelegate.modelData.totalActiveText
                        logExcerpt: ledgerDelegate.modelData.logExcerpt ?? ""
                        occurrences: ledgerDelegate.modelData.occurrences ?? []
                        // Worst-first order (rule 3): one problem, its card
                        // already open; the rest are rows the reader can expand.
                        expanded: ledgerDelegate.index === 0

                        onShowInLogRequested: (entryId) => root.showInLogRequested(entryId)
                        onOpenAtRequested: (startMs) => root.openEditAtRequested(startMs)
                    }
                }
            }

            RowLayout {
                objectName: "lastSessionActions"
                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                ExoButton {
                    text: qsTr("Show in folder")
                    onClicked: root.showInFolderRequested()
                }

                ExoButton {
                    text: qsTr("Open in Edit")
                    quiet: true
                    onClicked: root.openEditRequested()
                }

                ExoButton {
                    text: qsTr("View log")
                    quiet: true
                    onClicked: root.viewLogRequested()
                }

                Item { Layout.fillWidth: true }
            }
        }
    }
}

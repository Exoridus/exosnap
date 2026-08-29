pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The previous session did not shut down normally (ADR 0017). Shown once at the
// next launch, and only when the persisted policy is "ask every time".
//
// This is a consent surface: the two committing actions are equally weighted and
// neither is pre-focused, the remember tick changes nothing until one of them is
// pressed, and dismissing commits nothing at all. All of that is enforced in
// CrashReportAdapter — the QML only reports which button was pressed.
ExoOverlayCard {
    id: root

    required property CrashReportAdapter crash

    objectName: "quickCrashReportOverlay"
    subtitle: qsTr("PROBLEM REPORT")
    // Warning, not error: the crash already happened and nothing is broken right
    // now — the surface asks a consent question about a session that has ended.
    // Escalating it to "error" would overstate what the user has to do about it.
    severity: "warning"
    title: qsTr("The previous session did not shut down normally")
    hint: root.crash.availabilityText
    onDismissed: root.crash.dismiss()

    // A crash mid-recording left a recovery candidate behind. Said here because
    // "did not shut down normally" otherwise reads as "your recording is gone".
    ExoNotice {
        text: qsTr("Your interrupted recording data is available for recovery.")
        tone: "success"
        visible: root.crash.recordingWasActive
        Layout.fillWidth: true
    }

    Label {
        text: qsTr("WHAT HAPPENED")
        textFormat: Text.PlainText
        color: ExoTheme.textDim
        Layout.fillWidth: true
        font {
            family: ExoTheme.monoFamily
            pixelSize: ExoTheme.fontEyebrow
            letterSpacing: 0.6
        }
    }

    ExoKeyValueTable {
        rows: root.crash.summaryRows
        labelColumnWidth: 110
        Layout.fillWidth: true
    }

    ExoDisclosure {
        // Named so a --visual-test capture can photograph the expanded body; the
        // disclosure is otherwise only reachable by clicking, and the harness
        // synthesises no input.
        objectName: "crashIncludedDisclosure"
        title: qsTr("What is included in this report?")
        subtitle: qsTr("Includes a native crash dump when available and limited app diagnostics. Recordings are never included.")
        Layout.fillWidth: true

        body: ColumnLayout {
            spacing: ExoTheme.spacingSm

            Repeater {
                model: [
                    { heading: qsTr("INCLUDED"), items: root.crash.includedItems, tone: ExoTheme.success,
                      glyph: ExoGlyph.Check },
                    { heading: qsTr("NOT INCLUDED"), items: root.crash.excludedItems, tone: ExoTheme.error,
                      glyph: ExoGlyph.Close }
                ]

                delegate: ColumnLayout {
                    id: group

                    required property var modelData

                    spacing: 2
                    Layout.fillWidth: true

                    RowLayout {
                        spacing: ExoTheme.spacingSm
                        Layout.fillWidth: true

                        ExoGlyph {
                            kind: group.modelData.glyph
                            color: group.modelData.tone
                            Layout.preferredWidth: 12
                            Layout.preferredHeight: 12
                        }

                        Label {
                            text: group.modelData.heading
                            textFormat: Text.PlainText
                            color: group.modelData.tone
                            Layout.fillWidth: true
                            font {
                                family: ExoTheme.monoFamily
                                pixelSize: ExoTheme.fontEyebrow
                                letterSpacing: 0.6
                            }
                        }
                    }

                    Repeater {
                        model: group.modelData.items

                        delegate: Label {
                            required property string modelData

                            text: "· " + modelData
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            color: ExoTheme.textMuted
                            Layout.fillWidth: true
                            font {
                                family: ExoTheme.sansFamily
                                pixelSize: ExoTheme.fontCaption
                            }
                        }
                    }
                }
            }

            Label {
                text: root.crash.channelNote
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: ExoTheme.textDim
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }
        }
    }

    // In the card's persistent strip, not in the scrolling body: this is the
    // standing part of the decision, and at the 860x700 minimum window an
    // expanded "What is included in this report?" used to scroll it out of sight
    // while both committing buttons stayed visible. What the report contains may
    // scroll; whether the answer is remembered may not.
    persistent: [
        ExoCheckBox {
            objectName: "crashRememberChoice"
            text: qsTr("Remember this choice for future crashes")
            checked: root.crash.rememberChoice
            onToggled: root.crash.rememberChoice = checked
        },
        Label {
            text: qsTr("Send report will enable automatic reports. Don't send will stop future report prompts.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.crash.rememberChoice
            color: ExoTheme.textDim
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }
    ]

    actions: [
        ExoButton {
            objectName: "crashOpenFolderButton"
            text: qsTr("Open crash folder")
            leadingGlyph: ExoGlyph.Folder
            enabled: root.crash.crashFolderAvailable
            onClicked: root.crash.openCrashFolder()
        },
        Item {
            Layout.fillWidth: true
        },
        // Neither action is pre-focused: a stray Return must not answer a
        // consent question on the user's behalf, in either direction.
        ExoButton {
            objectName: "crashDontSendButton"
            text: qsTr("Don't send")
            onClicked: root.crash.dontSend()
        },
        ExoButton {
            objectName: "crashSendButton"
            text: qsTr("Send report")
            leadingGlyph: ExoGlyph.Send
            tone: "primary"
            onClicked: root.crash.sendReport()
        }
    ]
}

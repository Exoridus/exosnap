pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Startup recovery (ADR-0014/ADR-0015). Shown when the previous run left an
// unfinalized recording behind.
//
// Everything here is presentation: which rows exist, which actions a row may
// offer, whether a delete has been confirmed and what any of them do all come
// from RecoveryAdapter. The delegate never touches a manifest, a path or a file.
ExoOverlayCard {
    id: root

    required property RecoveryAdapter recovery

    objectName: "quickRecoveryOverlay"
    subtitle: qsTr("Recovery")
    title: qsTr("Recover interrupted recordings")
    hint: qsTr("These recordings were interrupted before they could be saved. Finish saves the recording as originally configured. Continue resumes recording from where you left off. Or decide later — entries stay for the next launch.")
    // A running repair must stay visible, so the escape hatch closes with it.
    dismissOnEscape: !root.recovery.busy
    onDismissed: root.recovery.dismiss()

    Repeater {
        model: root.recovery.model

        delegate: ColumnLayout {
            id: candidate

            required property int index
            required property string displayName
            required property string meta
            required property bool canContinue
            required property string status
            required property bool statusIsError
            required property bool busy
            required property real progress
            required property bool armedDiscard

            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            Rectangle {
                color: ExoTheme.line
                Layout.fillWidth: true
                Layout.preferredHeight: 1
            }

            RowLayout {
                spacing: ExoTheme.spacingMd
                Layout.fillWidth: true

                Label {
                    text: candidate.displayName
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: ExoTheme.text
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 13
                        weight: Font.DemiBold
                    }
                }

                Label {
                    text: candidate.meta
                    textFormat: Text.PlainText
                    color: ExoTheme.textDim
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: 11
                        letterSpacing: 0.3
                    }
                }
            }

            // ---- Idle actions ----
            RowLayout {
                spacing: ExoTheme.spacingSm
                visible: !candidate.busy
                Layout.fillWidth: true

                ExoButton {
                    objectName: "recoveryFinishButton"
                    text: qsTr("Finish")
                    tone: "primary"
                    enabled: !root.recovery.busy
                    Accessible.name: qsTr("Finish saving %1").arg(candidate.displayName)
                    onClicked: root.recovery.finish(candidate.index)
                }

                ExoButton {
                    objectName: "recoveryContinueButton"
                    text: qsTr("Continue")
                    visible: candidate.canContinue
                    enabled: !root.recovery.busy
                    Accessible.name: qsTr("Continue recording into %1").arg(candidate.displayName)
                    onClicked: root.recovery.continueSession(candidate.index)
                }

                Label {
                    text: candidate.status
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    visible: candidate.status !== ""
                    color: candidate.statusIsError ? ExoTheme.error : ExoTheme.textMuted
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 12
                    }
                }

                Item {
                    Layout.fillWidth: candidate.status === ""
                }

                // Two-step, and the second step is the one the adapter accepts:
                // the armed state lives in C++, so this button only ever reports
                // it back.
                ExoButton {
                    objectName: "recoveryDeleteButton"
                    text: candidate.armedDiscard ? qsTr("Confirm delete") : qsTr("Delete")
                    tone: "destructive"
                    enabled: !root.recovery.busy
                    Accessible.name: candidate.armedDiscard
                                     ? qsTr("Confirm deleting %1").arg(candidate.displayName)
                                     : qsTr("Delete %1").arg(candidate.displayName)
                    onClicked: {
                        if (candidate.armedDiscard)
                            root.recovery.discard(candidate.index);
                        else
                            root.recovery.armDiscard(candidate.index);
                    }
                }
            }

            // ---- Running Finish ----
            RowLayout {
                spacing: ExoTheme.spacingMd
                visible: candidate.busy
                Layout.fillWidth: true

                ExoProgressBar {
                    value: candidate.progress
                    Layout.fillWidth: true
                }

                ExoButton {
                    text: qsTr("Cancel")
                    quiet: true
                    onClicked: root.recovery.cancelAction()
                }
            }
        }
    }

    actions: [
        Item {
            Layout.fillWidth: true
        },
        ExoButton {
            objectName: "recoveryDecideLaterButton"
            text: qsTr("Decide later")
            quiet: true
            enabled: !root.recovery.busy
            onClicked: root.recovery.dismiss()
        }
    ]
}

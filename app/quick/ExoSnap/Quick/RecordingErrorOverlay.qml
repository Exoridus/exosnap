// Bound: the disclosure body is a nested Component, and it reads the adapter
// through this file's own root id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A recording attempt failed (RECORDING-ERROR-MODAL-R1). Shown for every failure
// except the disk-space auto-stop, which carries its own actionable notification.
//
// Which rows exist, what the failure is called and whether it can be reported at
// all are all decided in C++; this renders them.
ExoOverlayCard {
    id: root

    required property RecordingErrorAdapter error

    objectName: "quickRecordingErrorOverlay"
    subtitle: qsTr("Recording")
    title: root.error.title
    hint: root.error.summary
    onDismissed: root.error.dismiss()

    // The plain-language failure and the next step are the title and the hint,
    // which the card puts above this. The phase/code/codec table is what a
    // support conversation needs, not what tells the user what happened — so it
    // is one click away rather than the first thing under the sentence. It is
    // never removed: a failure nobody can diagnose is worse than a busy card.
    ExoDisclosure {
        title: qsTr("Technical details")
        Layout.fillWidth: true

        body: ExoKeyValueTable {
            rows: root.error.detailRows
            labelColumnWidth: 82
        }
    }

    // Says exactly what leaves the machine, next to the button that would send
    // it — not in a policy page the user will not read.
    RowLayout {
        spacing: ExoTheme.spacingSm
        visible: root.error.canSendReport
        Layout.fillWidth: true

        Label {
            text: qsTr("Sends the error phase, code, and codec/container only — never file paths, folder names, or recording content.")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.textDim
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: 11
            }
        }
    }

    actions: [
        ExoButton {
            objectName: "recordingErrorSendButton"
            text: qsTr("Send report")
            tone: "primary"
            visible: root.error.canSendReport
            onClicked: root.error.sendReport()
        },
        ExoButton {
            objectName: "recordingErrorLogsButton"
            text: qsTr("Open logs")
            onClicked: root.error.openLogs()
        },
        Item {
            Layout.fillWidth: true
        },
        ExoButton {
            objectName: "recordingErrorCloseButton"
            text: qsTr("Close")
            quiet: true
            focus: true
            onClicked: root.error.dismiss()
        }
    ]
}

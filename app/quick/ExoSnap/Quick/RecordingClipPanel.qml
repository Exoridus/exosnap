pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    required property RecordingClipAdapter clipAdapter

    width: 380
    padding: ExoTheme.spacingMd
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    contentItem: ColumnLayout {
        spacing: ExoTheme.spacingSm

        Label {
            text: qsTr("Save clip")
            font.pixelSize: ExoTheme.fontSectionTitle
            font.weight: Font.DemiBold
            Layout.fillWidth: true
        }
        Label {
            text: root.clipAdapter.unavailableReason
            visible: !root.clipAdapter.canRequest && text.length > 0
            wrapMode: Text.Wrap
            color: ExoTheme.warningText
            Layout.fillWidth: true
        }
        Flow {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true
            Repeater {
                model: root.clipAdapter.durationOptions
                Button {
                    required property var modelData
                    text: modelData.label
                    enabled: root.clipAdapter.canRequest
                    onClicked: root.clipAdapter.requestPlan(modelData.seconds)
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: customSeconds
                placeholderText: qsTr("Custom seconds")
                inputMethodHints: Qt.ImhDigitsOnly
                validator: RegularExpressionValidator { regularExpression: /^[1-9][0-9]*$/ }
                Layout.fillWidth: true
            }
            Button {
                text: qsTr("Plan clip")
                enabled: root.clipAdapter.canRequest && customSeconds.acceptableInput
                onClicked: root.clipAdapter.requestPlan(customSeconds.text)
            }
        }
        Label {
            text: qsTr("The recording continues while clips are saved.")
            color: ExoTheme.textSecondary
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(320, jobList.implicitHeight)
            visible: root.clipAdapter.jobs.length > 0
            ColumnLayout {
                id: jobList
                width: root.width - 2 * root.padding
                spacing: ExoTheme.spacingMd
                Repeater {
                    model: root.clipAdapter.jobs
                    ColumnLayout {
                        id: jobRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: ExoTheme.spacingSm
                        Label {
                            text: jobRow.modelData.phaseText
                            font.weight: Font.DemiBold
                            Layout.fillWidth: true
                        }
                        Label {
                            text: qsTr("Requested %1–%2").arg(jobRow.modelData.requestedBegin).arg(jobRow.modelData.requestedEnd)
                            visible: jobRow.modelData.canConfirm
                            Layout.fillWidth: true
                        }
                        Label {
                            text: qsTr("Available %1–%2").arg(jobRow.modelData.actualBegin).arg(jobRow.modelData.actualEnd)
                            visible: jobRow.modelData.canConfirm
                            Layout.fillWidth: true
                        }
                        Label {
                            text: qsTr("MKV stream copy. The range follows safe GOP boundaries.")
                            visible: jobRow.modelData.canConfirm
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                        Label {
                            text: jobRow.modelData.reviewReason || ""
                            visible: jobRow.modelData.canConfirm && text.length > 0
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                        Label {
                            text: qsTr("Tracks: %1").arg(jobRow.modelData.trackNames.join(", "))
                            visible: jobRow.modelData.canConfirm && jobRow.modelData.trackNames.length > 0
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                        Label {
                            text: qsTr("Read %1 bytes · wrote %2 bytes")
                                .arg(jobRow.modelData.sourceBytesRead).arg(jobRow.modelData.stagingBytesWritten)
                            visible: !jobRow.modelData.canConfirm && !jobRow.modelData.canOpen
                            color: ExoTheme.textSecondary
                            Layout.fillWidth: true
                        }
                        Label {
                            text: jobRow.modelData.errorSummary
                            visible: text.length > 0
                            color: ExoTheme.errorText
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                        Label {
                            text: jobRow.modelData.errorDetail
                            visible: text.length > 0
                            color: ExoTheme.textSecondary
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Button {
                                text: qsTr("Save this range")
                                visible: jobRow.modelData.canConfirm
                                onClicked: root.clipAdapter.confirmPlan(jobRow.modelData.jobId, jobRow.modelData.generation)
                            }
                            Button {
                                text: qsTr("Cancel clip")
                                visible: jobRow.modelData.canCancel
                                onClicked: root.clipAdapter.cancelJob(jobRow.modelData.jobId, jobRow.modelData.generation)
                            }
                            Button {
                                text: qsTr("Edit saved clip")
                                visible: jobRow.modelData.canOpen
                                onClicked: root.clipAdapter.openCompleted(jobRow.modelData.assetId)
                            }
                        }
                    }
                }
            }
        }
    }
}

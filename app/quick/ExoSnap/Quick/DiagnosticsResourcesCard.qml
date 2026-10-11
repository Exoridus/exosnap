pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    required property var resourceSnapshot

    readonly property var audioResourceSnapshot: root.resourceSnapshot || ({})
    readonly property var audioResourceGroups: [
        {title: qsTr("Physical capture"), rows: root.audioResourceSnapshot.physical || []},
        {title: qsTr("Logical sources"), rows: root.audioResourceSnapshot.sources || []},
        {title: qsTr("Track outputs"), rows: root.audioResourceSnapshot.tracks || []},
        {title: qsTr("Clip jobs"), rows: root.audioResourceSnapshot.clipJobs || []}
    ]

    component ResourceMetric: ColumnLayout {
        id: resourceMetric

        required property var metric

        objectName: "diagnosticsAudioResourceMetric"
        spacing: ExoTheme.spacingXs
        Layout.fillWidth: true

        RowLayout {
            spacing: ExoTheme.spacingMd
            Layout.fillWidth: true

            Label {
                text: resourceMetric.metric.label || ""
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: ExoTheme.textSecondary
                Layout.preferredWidth: Math.min(180, root.width * 0.3)
                Layout.alignment: Qt.AlignTop
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                }
            }

            Label {
                objectName: "diagnosticsAudioResourceValue"
                text: resourceMetric.metric.available ? (resourceMetric.metric.value || qsTr("n/a")) : qsTr("n/a")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: resourceMetric.metric.available ? ExoTheme.text : ExoTheme.textMuted
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.alignment: Qt.AlignTop
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }
        }

        Label {
            objectName: "diagnosticsAudioResourceReason"
            text: resourceMetric.metric.reason || ""
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            visible: !resourceMetric.metric.available && text !== ""
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }
    }

    objectName: "diagnosticsAudioResources"
    spacing: ExoTheme.spacingSm
    visible: root.audioResourceGroups.some(function (group) { return group.rows.length > 0; })
             || (root.audioResourceSnapshot.unavailable || []).length > 0
    Layout.fillWidth: true

    DiagnosticsSectionHeader {
        title: qsTr("AUDIO RESOURCES")
        Layout.fillWidth: true
    }

    Label {
        text: qsTr("Elapsed processing time is cumulative per owner. Shared capture and processing work is counted once, not once per route.")
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: ExoTheme.textMuted
        Layout.fillWidth: true
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontCaption
        }
    }

    Repeater {
        model: root.audioResourceGroups

        ColumnLayout {
            id: resourceGroup
            required property var modelData

            objectName: "diagnosticsAudioResourceGroup"

            spacing: ExoTheme.spacingSm
            visible: resourceGroup.modelData.rows.length > 0
            Layout.fillWidth: true

            Label {
                objectName: "diagnosticsAudioResourceCategory"
                text: resourceGroup.modelData.title
                textFormat: Text.PlainText
                color: ExoTheme.textSecondary
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                    weight: Font.DemiBold
                }
            }

            Repeater {
                model: resourceGroup.modelData.rows

                Rectangle {
                    id: resourceRow
                    required property var modelData

                    objectName: "diagnosticsAudioResourceRow"
                    implicitHeight: resourceContent.implicitHeight + 2 * ExoTheme.spacingMd
                    color: ExoTheme.surface
                    border.width: 1
                    border.color: ExoTheme.line
                    radius: ExoTheme.radiusMd
                    Layout.fillWidth: true

                    ColumnLayout {
                        id: resourceContent
                        anchors {
                            fill: parent
                            margins: ExoTheme.spacingMd
                        }
                        spacing: ExoTheme.spacingSm

                        Label {
                            objectName: "diagnosticsAudioResourceIdentity"
                            text: resourceRow.modelData.identity || ""
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            color: ExoTheme.text
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            font {
                                family: ExoTheme.monoFamily
                                pixelSize: ExoTheme.fontCaption
                                weight: Font.DemiBold
                            }
                        }

                        Repeater {
                            model: resourceRow.modelData.metrics || []

                            ResourceMetric {
                                id: metricDelegate
                                required property var modelData
                                metric: metricDelegate.modelData
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        visible: (root.audioResourceSnapshot.unavailable || []).length > 0
        implicitHeight: unavailableContent.implicitHeight + 2 * ExoTheme.spacingMd
        color: ExoTheme.surface
        border.width: 1
        border.color: ExoTheme.line
        radius: ExoTheme.radiusMd
        Layout.fillWidth: true

        ColumnLayout {
            id: unavailableContent
            anchors {
                fill: parent
                margins: ExoTheme.spacingMd
            }
            spacing: ExoTheme.spacingSm

            Label {
                text: qsTr("Unavailable measurements")
                textFormat: Text.PlainText
                color: ExoTheme.textSecondary
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                    weight: Font.DemiBold
                }
            }

            Repeater {
                model: root.audioResourceSnapshot.unavailable || []

                ResourceMetric {
                    id: unavailableMetricDelegate
                    required property var modelData
                    metric: unavailableMetricDelegate.modelData
                }
            }
        }
    }
}

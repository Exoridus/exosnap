pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ExoCard {
    id: root

    required property AudioRoutingAdapter routing
    required property var track
    property bool detailsOpen: false
    readonly property var meter: routing.trackMeters[track.id] || {available: false}

    title: track.name
    subtitle: track.empty ? qsTr("Empty track") : qsTr("Track %1").arg(track.position)

    function invoke(action) {
        if (root.routing.readOnly)
            return false;
        return action(root.routing.revision);
    }

    RowLayout {
        Layout.fillWidth: true
        ExoTextField {
            id: nameField
            Layout.fillWidth: true
            Layout.minimumWidth: 100
            value: root.track.name
            Accessible.name: qsTr("Track name")
            onCommitted: value => root.invoke(revision => root.routing.setTrackName(root.track.id, value, revision))
            Keys.onEscapePressed: event => {
                nameField.text = root.track.name;
                nameField.focus = false;
                event.accepted = true;
            }
        }
        ExoButton {
            text: qsTr("Move up")
            Accessible.name: qsTr("Move %1 up").arg(root.track.name)
            enabled: !root.routing.readOnly && root.track.position > 1
            onClicked: root.routing.reorderTrackById(root.track.id, -1, root.routing.revision)
        }
        ExoButton {
            text: qsTr("Move down")
            Accessible.name: qsTr("Move %1 down").arg(root.track.name)
            enabled: !root.routing.readOnly && root.track.position < root.routing.tracks.length
            onClicked: root.routing.reorderTrackById(root.track.id, 1, root.routing.revision)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        ExoCheckBox {
            text: qsTr("Record")
            checked: root.track.recordEnabled
            Accessible.name: qsTr("Record track %1").arg(root.track.name)
            enabled: !root.routing.readOnly
            onToggledByUser: value => root.routing.setTrackRecordEnabled(root.track.id, value, root.routing.revision)
        }
        ExoCheckBox {
            text: qsTr("Default monitor")
            checked: root.track.defaultMonitored
            Accessible.name: qsTr("Monitor track %1 by default").arg(root.track.name)
            enabled: !root.routing.readOnly
            onToggledByUser: value => {
                const ids = Array.from(root.routing.defaultMonitorTrackIds);
                const index = ids.indexOf(root.track.id);
                if (value && index < 0)
                    ids.push(root.track.id);
                else if (!value && index >= 0)
                    ids.splice(index, 1);
                root.routing.setDefaultMonitorSelection(ids, root.routing.revision);
            }
        }
        ComboBox {
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            Accessible.name: qsTr("Role for %1").arg(root.track.name)
            model: [qsTr("Standard"), qsTr("Stem"), qsTr("Mix")]
            currentIndex: root.track.role
            enabled: !root.routing.readOnly
            onActivated: index => root.routing.setTrackRole(root.track.id, index, root.routing.revision)
        }
        ExoButton {
            text: root.detailsOpen ? qsTr("Hide details") : qsTr("Track details")
            Accessible.name: qsTr("Toggle details for %1").arg(root.track.name)
            onClicked: root.detailsOpen = !root.detailsOpen
        }
    }

    Label {
        Layout.fillWidth: true
        visible: !root.meter.available
        text: qsTr("Track level: Unavailable")
        wrapMode: Text.WordWrap
        color: ExoTheme.textMuted
        Accessible.name: text
    }
    Label {
        Layout.fillWidth: true
        visible: root.meter.available
        text: qsTr("Track level: %1 dBFS").arg(Number(root.meter.rmsDbfs).toFixed(1))
        color: ExoTheme.textMuted
    }

    Flow {
        Layout.fillWidth: true
        spacing: ExoTheme.spacingXs
        Repeater {
            model: root.track.routes
            delegate: Rectangle {
                required property var modelData
                color: ExoTheme.surfaceRaised
                radius: ExoTheme.radiusPill
                implicitWidth: routeRow.implicitWidth + 16
                implicitHeight: routeRow.implicitHeight + 8
                RowLayout {
                    id: routeRow
                    anchors.centerIn: parent
                    Label {
                        text: modelData.name
                        wrapMode: Text.Wrap
                        color: ExoTheme.textMuted
                    }
                    ExoButton {
                        text: qsTr("Remove")
                        Accessible.name: qsTr("Remove %1 from %2").arg(modelData.name).arg(root.track.name)
                        enabled: !root.routing.readOnly
                        onClicked: root.routing.removeRoute(modelData.id, root.routing.revision)
                    }
                }
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: root.detailsOpen
        Repeater {
            model: root.track.routes
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                Label {
                    text: qsTr("%1 gain: %2 dB").arg(modelData.name).arg(Number(modelData.gainDb).toFixed(1))
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    color: ExoTheme.textMuted
                }
                ExoButton {
                    text: qsTr("-1 dB")
                    Accessible.name: qsTr("Lower gain for %1").arg(modelData.name)
                    enabled: !root.routing.readOnly && modelData.gainDb > -60
                    onClicked: root.routing.setRouteGain(modelData.id, Math.max(-60, modelData.gainDb - 1), root.routing.revision)
                }
                ExoButton {
                    text: qsTr("+1 dB")
                    Accessible.name: qsTr("Raise gain for %1").arg(modelData.name)
                    enabled: !root.routing.readOnly && modelData.gainDb < 24
                    onClicked: root.routing.setRouteGain(modelData.id, Math.min(24, modelData.gainDb + 1), root.routing.revision)
                }
                ExoCheckBox {
                    text: qsTr("Mute route")
                    checked: modelData.muted
                    Accessible.name: qsTr("Mute %1 on %2").arg(modelData.name).arg(root.track.name)
                    enabled: !root.routing.readOnly
                    onToggledByUser: value => root.routing.setRouteMute(modelData.id, value, root.routing.revision)
                }
            }
        }
    }
}

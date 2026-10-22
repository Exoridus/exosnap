pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The audio mix as a context of the right panel: one row per audio track with
// the same track fields Details edits. Mute and gain are document edits that
// export honours; solo is monitoring only. There is no level meter here because
// the editor has no measured signal to show.
ExoScrollView {
    id: root
    objectName: "editMixPanel"
    required property EditSessionAdapter session
    required property Item canvasFocusItem
    readonly property var audioTracks: session.tracks.filter(track => !track.video)
    contentWidth: availableWidth
    clip: true
    rightPadding: ExoTheme.spacingMd
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    function focusFirst(): void {
        const row = rows.itemAt(0);
        if (row)
            row.muteButton.forceActiveFocus(Qt.TabFocusReason);
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: ExoTheme.spacingMd
        Label {
            objectName: "editMixEmpty"
            Layout.fillWidth: true
            visible: root.audioTracks.length === 0
            text: qsTr("This timeline has no audio tracks.")
            wrapMode: Text.Wrap
            color: ExoTheme.textSecondary
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontSecondary
        }
        Repeater {
            id: rows
            model: root.audioTracks
            delegate: ColumnLayout {
                id: row
                required property var modelData
                property alias muteButton: mute
                objectName: "editMixRow" + modelData.id
                Layout.fillWidth: true
                spacing: ExoTheme.spacingXs
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.typeName + " " + row.modelData.name
                        elide: Text.ElideRight
                        color: row.modelData.enabled ? ExoTheme.text : ExoTheme.textSecondary
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: ExoTheme.fontBody
                    }
                    EditActionButton {
                        id: mute
                        objectName: "editMixMute" + row.modelData.id
                        implicitWidth: 26
                        implicitHeight: 26
                        text: qsTr("Mute %1").arg(row.modelData.name)
                        glyph: ExoGlyph.Speaker
                        checkable: true
                        checked: !row.modelData.enabled
                        enabled: !row.modelData.locked
                        disabledReason: qsTr("The track is locked.")
                        onClicked: {
                            root.session.setTrackEnabled(row.modelData.id, !row.modelData.enabled);
                            checked = Qt.binding(() => !row.modelData.enabled);
                        }
                    }
                    EditActionButton {
                        objectName: "editMixSolo" + row.modelData.id
                        implicitWidth: 26
                        implicitHeight: 26
                        text: qsTr("Solo %1 (monitoring only)").arg(row.modelData.name)
                        glyph: ExoGlyph.Dot
                        checkable: true
                        checked: row.modelData.solo
                        onClicked: {
                            root.session.setTrackSolo(row.modelData.id, !row.modelData.solo);
                            checked = Qt.binding(() => row.modelData.solo);
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: ExoTheme.spacingSm
                    ExoSlider {
                        id: fader
                        objectName: "editMixFader" + row.modelData.id
                        Layout.fillWidth: true
                        from: -60
                        to: 12
                        stepSize: 0.1
                        value: row.modelData.gainDb
                        enabled: !row.modelData.locked
                        Accessible.name: qsTr("%1 gain").arg(row.modelData.name)
                        // One gesture, one edit: the document changes on release,
                        // and Escape during the drag restores the stored value.
                        property bool cancelled: false
                        onPressedChanged: {
                            if (pressed) {
                                cancelled = false;
                                return;
                            }
                            if (!cancelled && Math.abs(value - row.modelData.gainDb) >= 0.05)
                                root.session.setTrackGain(row.modelData.id, Math.round(value * 10) / 10);
                            value = Qt.binding(() => row.modelData.gainDb);
                        }
                        onMoved: if (cancelled) value = row.modelData.gainDb
                        Keys.onEscapePressed: event => {
                            event.accepted = pressed;
                            cancelled = pressed;
                            value = Qt.binding(() => row.modelData.gainDb);
                        }
                        Keys.onReleased: event => {
                            if ((event.key === Qt.Key_Left || event.key === Qt.Key_Right) && !event.isAutoRepeat
                                    && Math.abs(value - row.modelData.gainDb) >= 0.05)
                                root.session.setTrackGain(row.modelData.id, Math.round(value * 10) / 10);
                        }
                    }
                    EditGainField {
                        objectName: "editMixGain" + row.modelData.id
                        Layout.preferredWidth: 88
                        session: root.session
                        canvasFocusItem: root.canvasFocusItem
                        targetId: row.modelData.id
                        track: true
                        gainDb: row.modelData.gainDb
                        enabled: !row.modelData.locked
                        Accessible.name: qsTr("%1 gain in decibels").arg(row.modelData.name)
                        applyValue: (value, target) => root.session.setTrackGain(target, value)
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: ExoTheme.spacingXs
                    implicitHeight: 1
                    color: ExoTheme.line
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: root.audioTracks.length > 0
            text: qsTr("Mute and gain are part of the edit and apply to export. Solo only changes what you hear.")
            wrapMode: Text.Wrap
            color: ExoTheme.textMuted
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontCaption
        }
    }
}

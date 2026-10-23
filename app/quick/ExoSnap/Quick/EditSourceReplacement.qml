pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    required property EditSessionAdapter session
    readonly property var draft: session.sourceReplacement
    objectName: "editSourceReplacement"
    title: qsTr("Relink source: %1").arg(draft.name || "")
    modal: true
    focus: true
    padding: ExoTheme.spacingLg
    palette.window: ExoTheme.surfaceRaised
    palette.windowText: ExoTheme.text
    palette.text: ExoTheme.text
    palette.buttonText: ExoTheme.text
    Overlay.modal: Rectangle { color: ExoTheme.overlayScrim }
    background: Rectangle {
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusLg
    }
    header: Label {
        text: root.title
        textFormat: Text.PlainText
        color: ExoTheme.text
        leftPadding: ExoTheme.spacingLg
        rightPadding: ExoTheme.spacingLg
        topPadding: ExoTheme.spacingLg
        font.family: ExoTheme.sansFamily
        font.pixelSize: ExoTheme.fontSectionTitle
        font.weight: Font.DemiBold
    }
    width: Math.min(parent.width - 32, 620)
    height: Math.min(parent.height - 32, implicitHeight)
    anchors.centerIn: parent
    function sourceTime(value: var): string {
        if (value === undefined || value === null)
            return qsTr("Unavailable");
        const seconds = root.session.formatTimeValue(value, 1);
        return seconds.length > 0 ? qsTr("%1 s").arg(seconds) : qsTr("Unavailable");
    }
    onClosed: session.cancelSourceReplacement()
    Component.onDestruction: if (session) session.cancelSourceReplacement()
    Component.onCompleted: if (draft.active) open()
    Connections {
        target: root.session
        function onSourceReplacementChanged(): void {
            if (root.draft.active && !root.visible)
                root.open();
            else if (!root.draft.active && root.visible)
                root.close();
        }
    }
    contentItem: ExoScrollView {
        id: reviewScroll
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        clip: true
        implicitHeight: Math.min(440, content.implicitHeight)
        ColumnLayout {
            id: content
            width: reviewScroll.availableWidth
            spacing: ExoTheme.spacingMd
            Label {
                Layout.fillWidth: true
                text: qsTr("Every clip using this source will change together. Clip positions, groups and edits are preserved.")
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: ExoTheme.textSecondary
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Current: %1\nReplacement: %2").arg(root.draft.oldPath || "").arg(root.draft.path || "")
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                color: ExoTheme.text
            }
            Label {
                objectName: "editSourceReplacementIdentity"
                Layout.fillWidth: true
                text: qsTr("Expected SHA-256: %1\nObserved SHA-256: %2")
                    .arg(root.draft.expectedFingerprint || qsTr("Unavailable"))
                    .arg(root.draft.observedFingerprint || qsTr("Awaiting verification"))
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                font.pixelSize: ExoTheme.fontCaption
                color: ExoTheme.textSecondary
            }
            Label {
                Layout.fillWidth: true
                visible: root.draft.durationUs !== undefined && root.draft.durationUs !== null
                text: qsTr("Replacement duration: %1 · Required source end: %2")
                    .arg(root.sourceTime(root.draft.durationUs))
                    .arg(root.sourceTime(root.draft.requiredSourceEndUs))
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: ExoTheme.textSecondary
            }
            ExoCheckBox {
                objectName: "editSourceReplacementAdopt"
                Layout.fillWidth: true
                text: qsTr("Adopt changed content and establish its new identity")
                Accessible.name: text
                onActiveFocusChanged: if (activeFocus) reviewScroll.revealItem(this)
                checked: root.draft.adoptChangedBytes ?? false
                onToggledByUser: adopted => root.session.beginSourceReplacement(root.draft.assetId, root.draft.url, adopted)
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Affected clips: %1").arg((root.draft.affectedClips || []).length)
                color: ExoTheme.text
            }
            ListView {
                objectName: "editSourceReplacementClips"
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(120, count * 38)
                clip: true
                reuseItems: true
                model: root.visible ? root.draft.affectedClips || [] : []
                ScrollBar.vertical: ExoScrollBar {}
                delegate: Label {
                    required property var modelData
                    width: ListView.view.width
                    height: 38
                    text: qsTr("Clip %1 · %2 · %3–%4").arg(modelData.id).arg(modelData.track)
                        .arg(root.sourceTime(modelData.startUs)).arg(root.sourceTime(modelData.endUs))
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    verticalAlignment: Text.AlignVCenter
                    color: ExoTheme.textSecondary
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.draft.audioChoiceVisible ?? false
                text: qsTr("Choose the replacement audio streams")
                color: ExoTheme.text
            }
            Repeater {
                model: root.visible && root.draft.audioChoiceVisible ? (root.draft.tracks || []).length : 0
                ExoCheckBox {
                    required property int index
                    readonly property var row: (root.draft.tracks || [])[index] || ({})
                    objectName: "editSourceReplacementTrack-" + row.id
                    Layout.fillWidth: true
                    text: qsTr("%1 · %2 · %3 channels").arg(row.name || "").arg(row.role || qsTr("Audio")).arg(row.channels ?? 0)
                    Accessible.name: text
                    onActiveFocusChanged: if (activeFocus) reviewScroll.revealItem(this)
                    checked: row.selected ?? false
                    enabled: row.enabled ?? false
                    onToggledByUser: selected => root.session.setSourceReplacementTrack(row.id, selected)
                }
            }
            ExoCheckBox {
                objectName: "editSourceReplacementOverlap"
                Layout.fillWidth: true
                visible: (root.draft.audioChoiceVisible ?? false) && (root.draft.overlapRequired ?? false)
                text: qsTr("Include the named streams together despite overlapping content")
                Accessible.name: text
                onActiveFocusChanged: if (activeFocus) reviewScroll.revealItem(this)
                checked: root.draft.overlapConfirmed ?? false
                enabled: !root.draft.busy
                onToggledByUser: selected => root.session.setSourceReplacementOverlapConfirmed(selected)
            }
            Label {
                objectName: "editSourceReplacementReason"
                Layout.fillWidth: true
                text: root.draft.reason || ""
                textFormat: Text.PlainText
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: ExoTheme.warning
                Accessible.name: text
            }
            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: root.draft.busy ?? false
                visible: running
                Accessible.name: qsTr("Checking replacement source")
            }
        }
    }
    footer: DialogButtonBox {
        ExoButton {
            text: qsTr("Cancel")
            Accessible.name: text
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.close()
        }
        ExoButton {
            objectName: "editSourceReplacementReviewAgain"
            text: qsTr("Review again")
            Accessible.name: text
            enabled: (root.draft.active ?? false) && !root.draft.busy
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.session.beginSourceReplacement(root.draft.assetId, root.draft.url, root.draft.adoptChangedBytes)
        }
        ExoButton {
            objectName: "editSourceReplacementApply"
            text: qsTr("Apply")
            Accessible.name: text
            tone: "primary"
            enabled: root.draft.canApply ?? false
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.session.applySourceReplacement()
        }
    }
}

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    required property string formatText
    required property string sourceText
    required property int sourceGlyph
    required property bool sourceEnabled
    property bool overlayEnabled: true
    readonly property real controlsBottom: previewChrome.y + previewChrome.height
    property bool metricsVisible: false
    property string dropsText: ""
    property string driftText: ""
    property string sizeText: ""
    signal sourceRequested()
    signal overlayRequested()

    RowLayout {
        id: previewChrome
        z: 10
        spacing: ExoTheme.spacingSm
        readonly property real contentWidth: Math.max(0, width - addOverlayButton.width - 3 * spacing)
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            margins: ExoTheme.spacingMd
        }
        Rectangle {
            Layout.preferredWidth: Math.min(contractText.implicitWidth + 2 * ExoTheme.spacingMd,
                                           previewChrome.contentWidth * 0.52)
            Layout.preferredHeight: ExoTheme.controlHeightCompact
            radius: ExoTheme.radiusSm
            color: ExoTheme.overlaySurface
            Label {
                id: contractText
                objectName: "previewContractText"
                anchors.fill: parent
                anchors.leftMargin: ExoTheme.spacingMd
                anchors.rightMargin: ExoTheme.spacingMd
                text: root.formatText
                textFormat: Text.PlainText
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                color: ExoTheme.overlayInk
                font.family: ExoTheme.monoFamily
                font.pixelSize: ExoTheme.fontCaption
                Accessible.name: qsTr("Recording format: %1").arg(text)
            }
        }
        Item { Layout.fillWidth: true }
        Button {
            id: previewSourceButton
            objectName: "previewSourceButton"
            text: root.sourceText
            enabled: root.sourceEnabled
            focusPolicy: Qt.StrongFocus
            Layout.preferredWidth: Math.min(implicitWidth, previewChrome.contentWidth * 0.43)
            Layout.minimumWidth: 0
            Layout.preferredHeight: ExoTheme.controlHeightCompact
            leftPadding: ExoTheme.spacingMd
            rightPadding: ExoTheme.spacingMd
            Accessible.name: text
            Accessible.description: enabled ? qsTr("Open Source Picker") : qsTr("Source is locked while recording")
            onClicked: root.sourceRequested()
            background: Rectangle {
                radius: ExoTheme.radiusSm
                color: previewSourceButton.hovered ? ExoTheme.overlaySurfaceRaised : ExoTheme.overlaySurface
                border.width: previewSourceButton.activeFocus ? 2 : 1
                border.color: previewSourceButton.activeFocus ? ExoTheme.overlayAccent : ExoTheme.overlayLine
            }
            contentItem: RowLayout {
                spacing: ExoTheme.spacingSm
                ExoGlyph {
                    kind: root.sourceGlyph
                    color: ExoTheme.overlayInk
                    implicitWidth: 16
                    implicitHeight: 16
                    Accessible.ignored: true
                }
                Label {
                    text: previewSourceButton.text
                    textFormat: Text.PlainText
                    color: ExoTheme.overlayInk
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    font.pixelSize: ExoTheme.fontCaption
                }
            }
            ToolTip.visible: hovered
            ToolTip.text: text
        }
        Button {
            id: addOverlayButton
            objectName: "previewAddOverlayButton"
            text: qsTr("Add overlay")
            enabled: root.overlayEnabled
            Layout.preferredWidth: ExoTheme.controlHeightCompact
            Layout.preferredHeight: ExoTheme.controlHeightCompact
            Accessible.name: text
            onClicked: root.overlayRequested()
            background: Rectangle {
                radius: ExoTheme.radiusSm
                color: addOverlayButton.hovered ? ExoTheme.overlaySurfaceRaised : ExoTheme.overlaySurface
                border.width: addOverlayButton.activeFocus ? 2 : 1
                border.color: addOverlayButton.activeFocus ? ExoTheme.overlayAccent : ExoTheme.overlayLine
            }
            contentItem: ExoGlyph {
                kind: ExoGlyph.Plus
                color: ExoTheme.overlayInk
                width: 18
                height: 18
                Accessible.ignored: true
            }
            ToolTip.visible: hovered
            ToolTip.text: text
        }
    }

    Rectangle {
        id: liveMetrics
        objectName: "previewLiveMetrics"

        readonly property real maxWidth: Math.max(0, parent.width - 2 * ExoTheme.spacingLg)

        width: Math.min(metricsFlow.childrenRect.width + 2 * ExoTheme.spacingMd, liveMetrics.maxWidth)
        height: metricsFlow.childrenRect.height + 2 * ExoTheme.spacingSm
        // Captured content needs fixed-dark ink and surfaces in both appearances.
        color: ExoTheme.overlaySurface
        radius: ExoTheme.radiusSm
        visible: root.metricsVisible
        anchors {
            left: parent.left
            bottom: parent.bottom
            margins: ExoTheme.spacingMd
        }

        Flow {
            id: metricsFlow

            // Measured off the preview rather than off the ground
            // above, which is what keeps the two from feeding into
            // each other.
            width: Math.max(0, liveMetrics.maxWidth - 2 * ExoTheme.spacingMd)
            spacing: ExoTheme.spacingMd
            anchors {
                top: parent.top
                left: parent.left
                topMargin: ExoTheme.spacingSm
                leftMargin: ExoTheme.spacingMd
            }

            Label {
                text: qsTr("%1 Drops").arg(root.dropsText)
                textFormat: Text.PlainText
                color: ExoTheme.overlayInk
                font.family: ExoTheme.monoFamily
                font.pixelSize: ExoTheme.fontCaption
            }
            Label {
                text: qsTr("A/V %1").arg(root.driftText)
                textFormat: Text.PlainText
                color: ExoTheme.overlayInk
                font.family: ExoTheme.monoFamily
                font.pixelSize: ExoTheme.fontCaption
            }

            Label {
                text: root.sizeText
                textFormat: Text.PlainText
                color: ExoTheme.overlayInk
                font.family: ExoTheme.monoFamily
                font.pixelSize: ExoTheme.fontCaption
            }
        }
    }
}

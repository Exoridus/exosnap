import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

RowLayout {
    id: root
    required property var indicators
    spacing: ExoTheme.spacingSm
    Repeater {
        model: root.indicators
        delegate: Item {
            id: confidenceIndicator
            objectName: "confidence-" + modelData.key
            required property var modelData
            implicitWidth: 20
            implicitHeight: 24
            Accessible.role: Accessible.StaticText
            Accessible.name: modelData.description
            ExoGlyph {
                anchors.centerIn: parent
                kind: confidenceIndicator.modelData.key === "speaker" ? ExoGlyph.Speaker
                      : confidenceIndicator.modelData.key === "microphone" ? ExoGlyph.Mic : ExoGlyph.Webcam
                color: confidenceIndicator.modelData.tone === "error" ? ExoTheme.errorText
                       : confidenceIndicator.modelData.tone === "warning" ? ExoTheme.warningText
                       : confidenceIndicator.modelData.included ? ExoTheme.successText : ExoTheme.textDim
                implicitWidth: 18
                implicitHeight: 18
                Accessible.ignored: true
            }
            Rectangle {
                visible: !confidenceIndicator.modelData.included
                anchors.centerIn: parent
                width: 20
                height: 1
                rotation: -45
                color: ExoTheme.textDim
            }
            HoverHandler { id: confidenceHover }
            ToolTip.visible: confidenceHover.hovered
            ToolTip.text: confidenceIndicator.modelData.description
        }
    }
}

import QtQuick
import QtQuick.Controls.Basic

CheckBox {
    id: root

    signal toggledByUser(bool value)

    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    // QCR-506. The control was exactly its 18 px indicator tall, which is a
    // small target for a pointer and a very small one for anything less steady
    // than one. The three padding lines below take the interactive height to
    // 24 px WITHOUT enlarging the drawn box: the indicator stays 18 px and
    // stays vertically centred (its `y` is already derived from `topPadding`
    // and `availableHeight`), so nothing in the shipped layouts moves except
    // 3 px of air above and below.
    //
    // Width needs no such padding in practice — every shipped use carries a
    // label, and an AbstractButton's whole extent is its target, so the row is
    // already far wider than 24 px. The floor is stated anyway, for the
    // labelless case a future call site could introduce.
    padding: 0
    topPadding: 3
    bottomPadding: 3
    implicitWidth: Math.max(24, contentItem.implicitWidth + root.leftPadding + root.rightPadding)

    indicator: Rectangle {
        x: root.leftPadding
        y: root.topPadding + (root.availableHeight - height) / 2
        implicitWidth: 18
        implicitHeight: 18
        color: !root.enabled ? ExoTheme.surface
                             : root.checked ? (root.hovered ? ExoTheme.hoverTint(ExoTheme.accent) : ExoTheme.accent)
                                            : (root.hovered ? ExoTheme.hoverTint(ExoTheme.surfaceHover) : ExoTheme.surfaceHover)
        border.width: 1
        border.color: root.checked ? ExoTheme.accent : ExoTheme.lineStrong
        radius: ExoTheme.radiusSm - 4

        ExoGlyph {
            kind: ExoGlyph.Check
            visible: root.checked
            anchors.centerIn: parent
            color: root.enabled ? ExoTheme.accentInk : ExoTheme.textDim
            strokeWidth: 2
            width: 12
            height: 12
        }
    }

    // The focus ring goes around the whole interactive unit — the indicator AND
    // its label — rather than around the 18 px box alone: clicking the label
    // toggles the control, so the label is part of what has focus.
    background: Rectangle {
        color: "transparent"
        border.width: root.visualFocus ? ExoTheme.focusRingWidth : 0
        border.color: ExoTheme.text
        radius: ExoTheme.radiusXs
    }

    contentItem: Label {
        text: root.text
        textFormat: Text.PlainText
        verticalAlignment: Text.AlignVCenter
        visible: root.text !== ""
        color: root.enabled ? ExoTheme.textSecondary : ExoTheme.textDim
        leftPadding: root.indicator.width + ExoTheme.spacingXs
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontSecondary
        }
    }

    onClicked: root.toggledByUser(root.checked)
}

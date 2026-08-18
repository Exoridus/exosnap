import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The shared two-button confirm. Its first use was the mandatory prompt in front
// of every Auto FixAction — a config change the app makes on the user's behalf,
// so it is never silent — and the close guards reuse it, which is why the button
// labels are properties rather than literals: a guard has to name the actual
// consequence ("Cancel save and close"), not a generic "Apply".
Dialog {
    id: root

    property string bodyText: ""
    property string proceedText: qsTr("Apply")
    property string cancelText: qsTr("Cancel")
    // Which button carries focus when the dialog opens. Guards whose proceeding
    // option destroys work (stopping a recording, cancelling a save) must not
    // let a stray Return key trigger it.
    property bool defaultIsCancel: true

    modal: true
    // Without this the popup never takes active focus, and everything the dialog
    // claims about the keyboard stops being true: CloseOnEscape is documented to
    // need activeFocus, so Escape reaches the window instead of the guard, and
    // neither button below can hold the focus `defaultIsCancel` assigns. The
    // shell behind the modal keeps answering Tab and Return.
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(420, (parent?.width ?? 420) - 2 * ExoTheme.spacingXl)
    padding: ExoTheme.spacingLg
    closePolicy: Popup.CloseOnEscape

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
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontSectionTitle
            weight: Font.DemiBold
        }
    }

    contentItem: Label {
        text: root.bodyText
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        color: ExoTheme.textSecondary
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontSecondary
        }
    }

    footer: RowLayout {
        spacing: ExoTheme.spacingSm

        Item {
            Layout.fillWidth: true
        }

        ExoButton {
            text: root.cancelText
            quiet: true
            focus: root.defaultIsCancel
            onClicked: root.reject()
        }

        ExoButton {
            text: root.proceedText
            focus: !root.defaultIsCancel
            Layout.rightMargin: ExoTheme.spacingLg
            Layout.bottomMargin: ExoTheme.spacingLg
            onClicked: root.accept()
        }
    }
}

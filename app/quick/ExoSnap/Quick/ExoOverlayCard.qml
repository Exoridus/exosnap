import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The shared in-window overlay: scrim + centred card with the ExoSnap chrome
// bar, a title, an optional hint, a scrollable body and an action row.
//
// Three surfaces carry exactly this shape in the Widgets product — RecoveryOverlay,
// RecordingErrorOverlay and CrashReportOverlay — and each rebuilt it by hand from
// inline QSS. They are consolidated here because the semantics genuinely match,
// not because the pixels happen to: an app-modal, in-window, non-OS surface that
// reports something the user must resolve before continuing.
//
// What it deliberately does NOT own: which actions exist, what they do, or when
// the surface appears. Those belong to the calling surface and, above it, to C++.
//
// Responsive by construction: the card is capped rather than fixed, so at the
// 860x700 minimum window it uses the available width minus a margin, and its body
// scrolls instead of pushing the action row off the bottom.
Item {
    id: root

    required property string title
    property string subtitle: ""
    property string hint: ""
    // Escape and a backdrop click both mean "leave this for now". Surfaces where
    // that is not a safe answer (an unrecoverable recording error the user has to
    // acknowledge) set this false and offer an explicit action instead.
    property bool dismissOnEscape: true
    property int maxCardWidth: 620

    // The scrollable body between hint and actions — the default slot, because
    // that is what a caller writes most of.
    default property alias body: bodyColumn.data
    // The action row. Fill it with ExoButtons; the card supplies alignment.
    property alias actions: actionRow.data

    signal dismissed()

    // Blocks every click that would otherwise reach the page underneath: the
    // surfaces using this card all report state the user must resolve, so acting
    // on the shell behind them is never the intent.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onClicked: {
            if (root.dismissOnEscape)
                root.dismissed();
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.alpha(ExoTheme.background, 0.78)
    }

    Keys.onEscapePressed: function (event) {
        if (root.dismissOnEscape) {
            root.dismissed();
            event.accepted = true;
        }
    }

    Rectangle {
        id: card

        // Swallows clicks so the backdrop MouseArea above cannot dismiss the
        // surface when the user simply clicks inside the card.
        anchors.centerIn: parent
        width: Math.min(root.maxCardWidth, root.width - 2 * ExoTheme.spacingXl)
        height: Math.min(layout.implicitHeight, root.height - 2 * ExoTheme.spacingXl)
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusLg
        clip: true

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }

        ColumnLayout {
            id: layout

            anchors.fill: parent
            spacing: 0

            // ---- Chrome bar ----
            Rectangle {
                color: ExoTheme.surface
                Layout.fillWidth: true
                Layout.preferredHeight: 38

                RowLayout {
                    spacing: ExoTheme.spacingSm
                    anchors {
                        fill: parent
                        leftMargin: ExoTheme.spacingLg
                        rightMargin: ExoTheme.spacingMd
                    }

                    Image {
                        source: "qrc:/brand/exosnap-logo.svg"
                        sourceSize.width: 15
                        sourceSize.height: 15
                        Layout.preferredWidth: 15
                        Layout.preferredHeight: 15
                    }

                    Label {
                        text: qsTr("ExoSnap")
                        textFormat: Text.PlainText
                        color: ExoTheme.text
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: 12
                            weight: Font.DemiBold
                        }
                    }

                    Rectangle {
                        color: ExoTheme.line
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 13
                        visible: root.subtitle !== ""
                    }

                    Label {
                        text: root.subtitle
                        textFormat: Text.PlainText
                        visible: root.subtitle !== ""
                        color: ExoTheme.textMuted
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: 11
                            letterSpacing: 0.3
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                    }
                }

                Rectangle {
                    height: 1
                    color: ExoTheme.line
                    anchors {
                        right: parent.right
                        bottom: parent.bottom
                        left: parent.left
                    }
                }
            }

            // ---- Title + hint ----
            ColumnLayout {
                spacing: ExoTheme.spacingXs
                Layout.fillWidth: true
                Layout.leftMargin: ExoTheme.spacingXl
                Layout.rightMargin: ExoTheme.spacingXl
                Layout.topMargin: ExoTheme.spacingLg

                Label {
                    text: root.title
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: ExoTheme.text
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 16
                        weight: Font.DemiBold
                    }
                }

                Label {
                    text: root.hint
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    visible: root.hint !== ""
                    color: ExoTheme.textMuted
                    Layout.fillWidth: true
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: 12
                    }
                }
            }

            // ---- Body ----
            // fillHeight rather than a fixed height: the card is capped at the
            // window height, so whatever is left after chrome/title/actions is
            // what the body gets, and it scrolls inside that.
            ExoScrollView {
                id: bodyScroll

                // Word-wrapped content inside a width-capped card: the content
                // width feeds its own height, so the gutters are reserved
                // unconditionally to cut that cycle (see ExoScrollView's note).
                reserveScrollBarGutters: true
                contentWidth: availableWidth
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: ExoTheme.spacingLg
                Layout.bottomMargin: ExoTheme.spacingLg
                Layout.leftMargin: ExoTheme.spacingXl
                Layout.rightMargin: ExoTheme.spacingXl

                ColumnLayout {
                    id: bodyColumn

                    spacing: ExoTheme.spacingMd
                    width: bodyScroll.availableWidth
                }
            }

            // ---- Actions ----
            RowLayout {
                id: actionRow

                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true
                Layout.leftMargin: ExoTheme.spacingXl
                Layout.rightMargin: ExoTheme.spacingXl
                Layout.bottomMargin: ExoTheme.spacingLg
            }
        }
    }
}

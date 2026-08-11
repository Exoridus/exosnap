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
    // "none" | "info" | "warning" | "error" — how serious this surface is, drawn
    // as the hero block left of the title. "none" keeps the plain heading, which
    // is what a surface that merely asks a question wants.
    property string severity: "none"

    readonly property color _severityColor: root.severity === "error" ? ExoTheme.error
                                          : root.severity === "warning" ? ExoTheme.warning
                                          : ExoTheme.accent
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

                    // The same mark-plus-wordmark pair the shell's title bar
                    // draws, at the same rung. Every runtime surface the user
                    // meets after something went wrong — the recording error,
                    // the crash consent, the recovery prompt — sits on this card,
                    // and all three were spelling the product "ExoSnap" in 12 px
                    // body text next to a 15 px mark. That is the exact thing the
                    // shell stopped doing; a surface that appears when the
                    // application is in trouble is the last one that should look
                    // like a different application.
                    ExoBrandMark {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                    }

                    Row {
                        Layout.alignment: Qt.AlignVCenter

                        Label {
                            text: qsTr("exo")
                            textFormat: Text.PlainText
                            color: ExoTheme.text
                            font {
                                family: ExoTheme.sansFamily
                                pixelSize: ExoTheme.fontBrand
                                weight: Font.DemiBold
                            }
                        }

                        Label {
                            text: qsTr("snap")
                            textFormat: Text.PlainText
                            color: ExoTheme.accent
                            font {
                                family: ExoTheme.sansFamily
                                pixelSize: ExoTheme.fontBrand
                                weight: Font.DemiBold
                            }
                        }
                    }

                    Rectangle {
                        color: ExoTheme.line
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 14
                        Layout.alignment: Qt.AlignVCenter
                        visible: root.subtitle !== ""
                    }

                    Label {
                        text: root.subtitle
                        textFormat: Text.PlainText
                        visible: root.subtitle !== ""
                        color: ExoTheme.textMuted
                        Layout.alignment: Qt.AlignVCenter
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: ExoTheme.fontCaption
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

            // ---- Severity hero + title + hint ----
            //
            // The hero is what tells the user, before they read a word, whether
            // this surface is a failure, a caution or a routine question. Without
            // it every one of these surfaces was a card with a heading in it, and
            // "Recording could not start" carried exactly as much visual weight as
            // a settings section title.
            RowLayout {
                spacing: ExoTheme.spacingLg
                Layout.fillWidth: true
                Layout.leftMargin: ExoTheme.spacingXl
                Layout.rightMargin: ExoTheme.spacingXl
                Layout.topMargin: ExoTheme.spacingLg

                Rectangle {
                    color: root.severity === "error" ? ExoTheme.errorSurface
                         : root.severity === "warning" ? ExoTheme.warningSurface : ExoTheme.surfaceRaised
                    border.width: 1
                    border.color: root._severityColor
                    radius: ExoTheme.radiusMd
                    visible: root.severity !== "none"
                    Layout.preferredWidth: 44
                    Layout.preferredHeight: 44
                    Layout.alignment: Qt.AlignTop

                    ExoGlyph {
                        anchors.centerIn: parent
                        kind: root.severity === "error" ? ExoGlyph.Close
                              : root.severity === "warning" ? ExoGlyph.Warning : ExoGlyph.Info
                        color: root._severityColor
                        strokeWidth: 2
                        width: 22
                        height: 22
                    }
                }

                ColumnLayout {
                    spacing: ExoTheme.spacingXs
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter

                    Label {
                        text: root.title
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: ExoTheme.text
                        Layout.fillWidth: true
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontPageTitle
                            weight: Font.DemiBold
                        }
                    }

                    Label {
                        text: root.hint
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        visible: root.hint !== ""
                        color: ExoTheme.textSecondary
                        Layout.fillWidth: true
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontSecondary
                        }
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

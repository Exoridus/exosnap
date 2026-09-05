import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Chevron collapsible: a header button plus a body that only exists while open.
// The body is a Loader so a collapsed section (self-test rows, the config table,
// an issue card's evidence) costs nothing until it is opened.
ColumnLayout {
    id: root

    required property string title
    property string subtitle: ""
    property bool expanded: false
    property alias body: bodyLoader.sourceComponent
    // An optional slot at the header row's right edge (a "Run again" button,
    // a one-line mono summary next to a button -- ExoReferenceRow builds
    // exactly that). Empty for every disclosure that predates this, so it
    // draws nothing and costs nothing there.
    property alias trailing: trailingLoader.sourceComponent

    spacing: ExoTheme.spacingXs

    AbstractButton {
        id: header

        implicitHeight: 28
        hoverEnabled: true
        // QCR-503. A bare AbstractButton does not take focus, so this header —
        // the only way to open the section — was mouse-only: not in the tab
        // order, and Enter/Space did nothing. `focusPolicy` covers both, and
        // AbstractButton's own Space/Enter handling then reaches onClicked.
        focusPolicy: Qt.StrongFocus
        Layout.fillWidth: true
        Accessible.role: Accessible.Button
        Accessible.name: root.title
        // Whether the section is open is the whole state of this control, and
        // the chevron that shows it is a drawn shape. Qt Quick's Accessible
        // attached type has no expanded/collapsed property, so the supported
        // representation of a two-state button in this stack is checkable +
        // checked — which is what a screen reader reads back as pressed/not
        // pressed rather than leaving the state unsaid.
        Accessible.checkable: true
        Accessible.checked: root.expanded
        Accessible.focusable: true
        Accessible.focused: header.activeFocus
        Accessible.onPressAction: header.toggle()
        Accessible.onToggleAction: header.toggle()
        onClicked: header.toggle()

        function toggle(): void {
            root.expanded = !root.expanded;
        }

        // Only the header is a control; the body it opens is content.
        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }

        background: Rectangle {
            color: header.hovered ? ExoTheme.surfaceHover : "transparent"
            // The shared focus treatment: the `text` rung as a hairline, the
            // control's own radius. Same as ExoButton and ExoNavTab, so a
            // keyboard user sees one focus language across the frontend.
            border.width: header.visualFocus ? ExoTheme.focusRingWidth : 0
            border.color: ExoTheme.text
            radius: ExoTheme.radiusSm
        }

        contentItem: RowLayout {
            spacing: ExoTheme.spacingSm

            ExoChevron {
                direction: root.expanded ? 0 : -90
                tone: header.hovered ? ExoTheme.text : ExoTheme.textMuted
                Layout.preferredWidth: 12
                Layout.alignment: Qt.AlignVCenter

                Behavior on rotation {
                    NumberAnimation {
                        duration: ExoTheme.animMedium
                        easing.type: Easing.OutCubic
                    }
                }
            }

            Label {
                text: root.title
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.textSecondary
                Layout.fillWidth: true
                // Capped so a trailing slot (a summary, a button) gets the rest
                // of the row instead of splitting it evenly with a short title.
                // Every title in this frontend is well under this on its own.
                Layout.maximumWidth: 240
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                    weight: Font.DemiBold
                }
            }

            Loader {
                id: trailingLoader

                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
            }
        }
    }

    Label {
        text: root.subtitle
        textFormat: Text.PlainText
        wrapMode: Text.WordWrap
        visible: root.expanded && root.subtitle !== ""
        color: ExoTheme.textMuted
        Layout.fillWidth: true
        Layout.leftMargin: ExoTheme.spacingLg
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontCaption
        }
    }

    Loader {
        id: bodyLoader

        active: root.expanded
        visible: root.expanded
        Layout.fillWidth: true
        Layout.leftMargin: ExoTheme.spacingLg
    }
}

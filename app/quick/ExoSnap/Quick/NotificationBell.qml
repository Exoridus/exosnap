import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

// Title-bar bell. Opens/closes the hub and paints the worst-unread-severity
// dot (docs/product-spec.md §9: "The bell's unread dot carries urgency, not a
// count... The exact number is deliberately not shown"). unreadCount is
// therefore never rendered as visible text here — it only decides whether the
// dot is drawn, and feeds the accessible name for screen readers.
Item {
    id: root

    required property NotificationsAdapter notifications

    implicitWidth: ExoTheme.controlHeight
    implicitHeight: ExoTheme.controlHeight

    // Three steps, not four: an unread SUCCESS is not urgent, so the bell paints
    // it the same as info rather than green (product-spec §9 — the dot carries
    // urgency). Folding it to "info" here keeps the one-step-down decision at the
    // bell and leaves the colour table itself in ExoTheme, unduplicated.
    readonly property color dotColor: ExoTheme.advisoryTone(
        root.notifications.worstUnreadTone === "success" ? "info" : root.notifications.worstUnreadTone)

    Accessible.role: Accessible.Button
    Accessible.name: root.notifications.unreadCount > 0
                     ? qsTr("Notifications, %1 unread").arg(root.notifications.unreadCount)
                     : qsTr("Notifications")
    Accessible.onPressAction: root.notifications.toggleHub()
    activeFocusOnTab: true
    // QCR-503. `activeFocusOnTab` put the bell in the tab order; nothing acted
    // on a key once it was there, so a keyboard user could reach the hub's
    // opener and not open it. Space, and only Space: this is a plain Item with
    // a hand-written handler rather than an AbstractButton, so it is the one
    // place in the frontend that could quietly grow a second activation key —
    // and the product's contract is that a focused control answers to Space
    // (product-spec §10.1).
    Keys.onSpacePressed: event => {
        root.notifications.toggleHub();
        event.accepted = true;
    }

    ToolTip.text: qsTr("Notifications")
    ToolTip.visible: hoverHandler.hovered
    ToolTip.delay: 500

    Rectangle {
        anchors.fill: parent
        radius: ExoTheme.radiusSm
        color: hoverHandler.hovered || root.notifications.hubOpen ? ExoTheme.surfaceHover : "transparent"
        border.width: root.activeFocus ? ExoTheme.focusRingWidth : 0
        border.color: ExoTheme.text
    }

    // The bell, drawn as one continuous outline.
    //
    // It used to be three stacked Rectangles — a rounded dome, a separate base
    // bar and a detached dot. Enlarged next to the Widgets reference that reads
    // as a lamp with a crumb underneath rather than as a bell: the dome's corner
    // radii never met the bar, and the clapper floated clear of the body. A
    // Shape draws the silhouette the glyph is supposed to have, and the clapper
    // stays attached to it.
    Shape {
        id: glyph

        anchors.centerIn: parent
        width: 16
        height: 16
        preferredRendererType: Shape.CurveRenderer

        readonly property color ink: ExoTheme.textSecondary

        ShapePath {
            strokeColor: glyph.ink
            strokeWidth: 1.4
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin

            // Body: shoulders rising from the rim, a crown across the top, and
            // the flare back down to the rim on the other side.
            startX: 2.2
            startY: 11.4
            PathLine { x: 3.6; y: 11.4 }
            PathLine { x: 3.6; y: 7.4 }
            PathCubic {
                control1X: 3.6; control1Y: 4.5
                control2X: 5.6; control2Y: 2.6
                x: 8; y: 2.6
            }
            PathCubic {
                control1X: 10.4; control1Y: 2.6
                control2X: 12.4; control2Y: 4.5
                x: 12.4; y: 7.4
            }
            PathLine { x: 12.4; y: 11.4 }
            PathLine { x: 13.8; y: 11.4 }
        }

        // Rim: one straight rule the body's two shoulders both land on.
        ShapePath {
            strokeColor: glyph.ink
            strokeWidth: 1.4
            capStyle: ShapePath.RoundCap
            startX: 2.2
            startY: 11.4
            PathLine { x: 13.8; y: 11.4 }
        }

        // Clapper: touching the rim, not floating below it.
        ShapePath {
            strokeColor: glyph.ink
            strokeWidth: 1.4
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            startX: 6.6
            startY: 11.9
            PathCubic {
                control1X: 6.9; control1Y: 13.4
                control2X: 9.1; control2Y: 13.4
                x: 9.4; y: 11.9
            }
        }

        // The little stem on the crown — the detail that stops the silhouette
        // reading as a dome.
        ShapePath {
            strokeColor: glyph.ink
            strokeWidth: 1.4
            capStyle: ShapePath.RoundCap
            startX: 8
            startY: 2.6
            PathLine { x: 8; y: 1.4 }
        }
    }

    Rectangle {
        visible: root.notifications.unreadCount > 0
        width: 7
        height: 7
        radius: 3.5
        color: root.dotColor
        border.width: 1.5
        border.color: ExoTheme.surface
        anchors {
            top: parent.top
            right: parent.right
            topMargin: 7
            rightMargin: 7
        }
    }

    HoverHandler {
        id: hoverHandler
    }

    TapHandler {
        onTapped: root.notifications.toggleHub()
    }
}

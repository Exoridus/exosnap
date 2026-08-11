import QtQuick
import QtQuick.Controls

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

    readonly property color dotColor: root.notifications.worstUnreadTone === "error" ? ExoTheme.error
                                     : root.notifications.worstUnreadTone === "caution" ? ExoTheme.warning
                                     : ExoTheme.accent

    Accessible.role: Accessible.Button
    Accessible.name: root.notifications.unreadCount > 0
                     ? qsTr("Notifications, %1 unread").arg(root.notifications.unreadCount)
                     : qsTr("Notifications")
    Accessible.onPressAction: root.notifications.toggleHub()
    activeFocusOnTab: true

    ToolTip.text: qsTr("Notifications")
    ToolTip.visible: hoverHandler.hovered
    ToolTip.delay: 500

    Rectangle {
        anchors.fill: parent
        radius: ExoTheme.radiusSm
        color: hoverHandler.hovered || root.notifications.hubOpen ? ExoTheme.surfaceHover : "transparent"
    }

    // Minimal bell glyph: a rounded dome outline, a base line, and a clapper
    // dot — built from Rectangles rather than a Shape/SVG asset, matching the
    // plain-glyph weight ("✗ ⚠ ✓") the rest of this frontend uses for icons
    // that have no existing asset.
    Item {
        id: glyph

        anchors.centerIn: parent
        width: 14
        height: 14

        Rectangle {
            id: dome

            width: 12
            height: 9
            radius: 6
            bottomLeftRadius: 1
            bottomRightRadius: 1
            color: "transparent"
            border.width: 1.4
            border.color: ExoTheme.textSecondary
            anchors {
                horizontalCenter: parent.horizontalCenter
                top: parent.top
            }
        }

        Rectangle {
            width: glyph.width
            height: 1.4
            color: ExoTheme.textSecondary
            anchors {
                horizontalCenter: parent.horizontalCenter
                top: dome.bottom
                topMargin: 1
            }
        }

        Rectangle {
            width: 3.5
            height: 3.5
            radius: 1.75
            color: ExoTheme.textSecondary
            anchors {
                horizontalCenter: parent.horizontalCenter
                top: parent.top
                topMargin: 11
            }
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
            topMargin: 5
            rightMargin: 5
        }
    }

    HoverHandler {
        id: hoverHandler
    }

    TapHandler {
        onTapped: root.notifications.toggleHub()
    }
}

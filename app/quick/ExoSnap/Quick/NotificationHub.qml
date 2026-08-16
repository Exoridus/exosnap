pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The bell's hub popup: the persistent notification record (product-spec §9
// — "the hub is the record: every notification lands there, persists until
// dismissed, and keeps its action"). Instantiate with `parent` set to the
// NotificationBell item it belongs to (same y/x-relative-to-parent anchoring
// ExoSelect's own dropdown popup uses) and `notifications` bound to the same
// NotificationsAdapter instance the bell reads:
//
//   NotificationHub {
//       parent: bell
//       notifications: notificationsAdapter
//   }
//
// `visible` mirrors notifications.hubOpen rather than being driven by open()/
// close() calls directly, so the adapter stays the single source of truth for
// whether the hub is open — a click on the bell, an Escape key, or a
// click-outside must all agree on that one boolean.
//
// The entry list is a Repeater over a ColumnLayout, not a ListView: the hub
// is a handful to a few dozen entries, never the thousands ExoLogView's
// ListView exists to virtualise, and the same Repeater-over-a-model pattern
// already covers DiagnosticsPage's issue cards. It also avoids nesting one
// Flickable (ListView) inside another (ExoScrollView), which has no
// precedent anywhere else in this frontend.
Popup {
    id: root

    required property NotificationsAdapter notifications

    y: (root.parent ? root.parent.height : 0) + ExoTheme.spacingXs
    x: root.parent ? root.parent.width - root.width : 0
    width: 380
    height: 460
    padding: 0
    modal: false
    focus: true
    // OutsideParent, not Outside: the parent IS the bell that toggles this
    // popup. With CloseOnPressOutside the two fought over one click — the press
    // landed outside the popup, so the overlay closed it and cleared `hubOpen`,
    // and then the release reached the bell's TapHandler, whose toggleHub() saw
    // a closed hub and opened it straight back up. The hub could not be closed
    // by the control that opened it. Excluding the parent leaves that click to
    // the bell alone, which is the only thing that ever needed to own it; a
    // press anywhere else still closes the hub.
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    visible: root.notifications.hubOpen

    onClosed: root.notifications.closeHub()

    background: Rectangle {
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusLg
    }

    contentItem: ColumnLayout {
        id: column

        // On the contentItem, not on the Popup: Popup derives from QtObject, so
        // an Accessible attachment there is silently inert — the screen reader
        // announced nothing at all. qmllint flags it as Quick.attached-property-type.
        Accessible.role: Accessible.Pane
        Accessible.name: qsTr("Notifications")

        // Popup does not auto-bind a custom contentItem's size the way it
        // does its default one; implicitWidth/Height here is what makes this
        // ColumnLayout actually fill the popup's available area.
        implicitWidth: root.availableWidth
        implicitHeight: root.availableHeight
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: ExoTheme.spacingLg
            Layout.rightMargin: ExoTheme.spacingLg
            Layout.topMargin: ExoTheme.spacingMd
            Layout.bottomMargin: ExoTheme.spacingMd
            spacing: ExoTheme.spacingSm

            Label {
                text: qsTr("Notifications")
                textFormat: Text.PlainText
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSectionTitle
                    weight: Font.DemiBold
                }
            }

            Label {
                id: markAllReadLabel

                text: qsTr("Mark all read")
                textFormat: Text.PlainText
                visible: root.notifications.hasEntries
                color: markAllReadHover.hovered ? ExoTheme.text : ExoTheme.accent
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                    weight: Font.Medium
                }

                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Mark all read")

                HoverHandler {
                    id: markAllReadHover
                }

                TapHandler {
                    onTapped: root.notifications.markAllRead()
                }
            }

            Label {
                id: clearAllLabel

                text: qsTr("Clear all")
                textFormat: Text.PlainText
                visible: root.notifications.hasEntries
                color: clearAllHover.hovered ? ExoTheme.text : ExoTheme.textMuted
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                    weight: Font.Medium
                }

                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Clear all notifications")

                HoverHandler {
                    id: clearAllHover
                }

                TapHandler {
                    onTapped: root.notifications.dismissAll()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: ExoTheme.line
        }

        ColumnLayout {
            visible: !root.notifications.hasEntries
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: ExoTheme.spacingXl
            spacing: ExoTheme.spacingXs

            Item {
                Layout.fillHeight: true
            }

            Label {
                text: qsTr("You’re all caught up")
                textFormat: Text.PlainText
                color: ExoTheme.text
                Layout.alignment: Qt.AlignHCenter
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                    weight: Font.DemiBold
                }
            }

            Label {
                text: qsTr("Advisories about settings, updates and disk space land here.")
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: ExoTheme.textMuted
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }

            Item {
                Layout.fillHeight: true
            }
        }

        ExoScrollView {
            id: scroll

            visible: root.notifications.hasEntries
            contentWidth: availableWidth
            clip: true
            Layout.fillWidth: true
            Layout.fillHeight: true

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Notification history")

            ColumnLayout {
                width: scroll.availableWidth
                spacing: 0

                Repeater {
                    model: root.notifications.model

                    Rectangle {
                        id: entryDelegate

                        required property int index
                        required property string title
                        required property string body
                        required property string tone
                        required property string timestampText
                        required property bool unread
                        required property var actions

                        readonly property color toneColor: entryDelegate.tone === "success" ? ExoTheme.success
                                                          : entryDelegate.tone === "caution" ? ExoTheme.warning
                                                          : entryDelegate.tone === "error" ? ExoTheme.error
                                                          : ExoTheme.accent

                        Layout.fillWidth: true
                        implicitHeight: entryColumn.implicitHeight + 2 * ExoTheme.spacingMd
                        color: entryDelegate.unread ? ExoTheme.surfaceHover : "transparent"

                        Rectangle {
                            anchors {
                                top: parent.top
                                left: parent.left
                                right: parent.right
                            }
                            height: 1
                            color: ExoTheme.line
                            visible: entryDelegate.index > 0
                        }

                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: entryDelegate.toneColor
                            anchors {
                                left: parent.left
                                leftMargin: ExoTheme.spacingLg
                                top: parent.top
                                topMargin: ExoTheme.spacingMd + 3
                            }
                        }

                        ColumnLayout {
                            id: entryColumn

                            spacing: ExoTheme.spacingXs
                            anchors {
                                left: parent.left
                                right: parent.right
                                top: parent.top
                                leftMargin: ExoTheme.spacingLg + 8 + ExoTheme.spacingSm
                                rightMargin: ExoTheme.spacingLg
                                topMargin: ExoTheme.spacingMd
                                bottomMargin: ExoTheme.spacingMd
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: ExoTheme.spacingSm

                                Label {
                                    text: entryDelegate.title
                                    textFormat: Text.PlainText
                                    wrapMode: Text.WordWrap
                                    color: ExoTheme.text
                                    Layout.fillWidth: true
                                    font {
                                        family: ExoTheme.sansFamily
                                        pixelSize: ExoTheme.fontSecondary
                                        weight: entryDelegate.unread ? Font.DemiBold : Font.Medium
                                    }
                                }

                                Label {
                                    text: entryDelegate.timestampText
                                    textFormat: Text.PlainText
                                    color: ExoTheme.textDim
                                    font {
                                        family: ExoTheme.sansFamily
                                        pixelSize: ExoTheme.fontCaption
                                    }
                                }

                                ExoGlyph {
                                    kind: ExoGlyph.Close
                                    color: dismissHover.hovered ? ExoTheme.text : ExoTheme.textMuted
                                    Layout.preferredWidth: 12
                                    Layout.preferredHeight: 12

                                    Accessible.role: Accessible.Button
                                    Accessible.name: qsTr("Dismiss")

                                    HoverHandler {
                                        id: dismissHover
                                    }

                                    TapHandler {
                                        onTapped: root.notifications.dismissEntry(entryDelegate.index)
                                    }
                                }
                            }

                            Label {
                                text: entryDelegate.body
                                textFormat: Text.PlainText
                                wrapMode: Text.WordWrap
                                visible: entryDelegate.body !== ""
                                color: ExoTheme.textSecondary
                                Layout.fillWidth: true
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontCaption
                                }
                            }

                            RowLayout {
                                visible: entryDelegate.actions.length > 0
                                Layout.fillWidth: true
                                Layout.topMargin: ExoTheme.spacingXs
                                spacing: ExoTheme.spacingSm

                                Repeater {
                                    model: entryDelegate.actions

                                    ExoButton {
                                        id: actionButton

                                        required property var modelData

                                        text: actionButton.modelData.label
                                        quiet: true
                                        onClicked: root.notifications.triggerAction(entryDelegate.index, actionButton.modelData.action)
                                    }
                                }

                                Item {
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

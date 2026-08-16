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
// The entry list is a ListView, and the hub scroller IS that ListView.
//
// It used to be a Repeater inside an ExoScrollView, on the argument that the hub
// holds a handful to a few dozen entries. That argument does not survive the
// model: NotificationEntryModel::recordEvent() collapses only five event types
// onto a stable key and gives everything else its own permanent `evt-<sequence>`
// row, with no cap and no eviction — so `Saved`, `FramesDropped`,
// `CaptureActionFailed` and friends accumulate for the whole process lifetime,
// and `CaptureActionFailed` is input-driven, so the growth rate is not bounded by
// how much the user records. A Repeater instantiated every one of those the
// moment the hub opened: a Rectangle, a ColumnLayout, three wrapping Labels, an
// ExoGlyph, a HoverHandler, a TapHandler and a nested Repeater, per entry.
//
// The original objection — do not nest a Flickable inside another Flickable,
// which has no precedent in this frontend — is respected rather than overruled:
// the ExoScrollView is gone, so there is exactly one Flickable here, the same
// shape ExoLogView uses for its own bounded history.
//
// RETENTION IS DELIBERATELY UNCHANGED. `docs/product-spec.md` §9 says the hub is
// the record and that entries persist until dismissed; capping the model would
// be a product decision about that sentence, not a performance fix, and it is
// tracked as one. Virtualisation needs no such decision: the model may hold a
// thousand rows and only the visible ones cost a delegate.
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

            // QCR-503. Both of these were a Label carrying an Accessible.Button
            // role and a TapHandler: they claimed to be buttons to a screen
            // reader while being unreachable by Tab and inert to Enter/Space.
            // An AbstractButton around the same Label keeps the presentation
            // exactly as it was (a word, no chrome) and makes the claim true.
            AbstractButton {
                id: markAllReadButton

                objectName: "hubMarkAllRead"
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                visible: root.notifications.hasEntries
                padding: ExoTheme.spacingXs
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Mark all read")
                onClicked: root.notifications.markAllRead()

                background: Rectangle {
                    color: "transparent"
                    border.width: markAllReadButton.visualFocus ? ExoTheme.focusRingWidth : 0
                    border.color: ExoTheme.text
                    radius: ExoTheme.radiusXs
                }

                contentItem: Label {
                    text: qsTr("Mark all read")
                    textFormat: Text.PlainText
                    color: markAllReadButton.hovered ? ExoTheme.text : ExoTheme.accent
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontSecondary
                        weight: Font.Medium
                    }
                }
            }

            AbstractButton {
                id: clearAllButton

                objectName: "hubClearAll"
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                visible: root.notifications.hasEntries
                padding: ExoTheme.spacingXs
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Clear all notifications")
                onClicked: root.notifications.dismissAll()

                background: Rectangle {
                    color: "transparent"
                    border.width: clearAllButton.visualFocus ? ExoTheme.focusRingWidth : 0
                    border.color: ExoTheme.text
                    radius: ExoTheme.radiusXs
                }

                contentItem: Label {
                    text: qsTr("Clear all")
                    textFormat: Text.PlainText
                    color: clearAllButton.hovered ? ExoTheme.text : ExoTheme.textMuted
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontSecondary
                        weight: Font.Medium
                    }
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

        ListView {
            id: entryList

            objectName: "hubEntryList"

            visible: root.notifications.hasEntries
            clip: true
            model: root.notifications.model
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds
            Layout.fillWidth: true
            Layout.fillHeight: true

            Accessible.role: Accessible.List
            Accessible.name: qsTr("Notification history")

            ScrollBar.vertical: ExoScrollBar {}

            delegate: Rectangle {
                id: entryDelegate

                required property int index
                required property string title
                required property string body
                required property string tone
                required property string timestampText
                required property bool unread
                required property var actions

                // QCR-513. The entry used to carry its severity as an 8 px
                // coloured dot and nothing else — no shape, no word, so a user
                // who cannot separate those hues read four different advisories
                // as one, and a screen reader read the severity as nothing at
                // all. The glyph and the spoken name come off the same tone
                // table the rest of the frontend uses; this is the notification
                // hub joining that vocabulary, not a new one beside it.
                readonly property color toneColor: ExoTheme.advisoryToneText(entryDelegate.tone)
                readonly property string toneName: ExoTheme.advisoryToneName(entryDelegate.tone)
                // Same four meanings, same four glyphs the readiness tiles, the
                // issue cards and ExoNotice use. Kept beside the surface that
                // draws it for the reason ExoTheme's own comment gives.
                readonly property int toneGlyph: entryDelegate.tone === "success" ? ExoGlyph.Check
                                               : entryDelegate.tone === "caution" ? ExoGlyph.Warning
                                               : entryDelegate.tone === "error" ? ExoGlyph.Close
                                               : ExoGlyph.Info

                width: ListView.view.width
                height: entryColumn.implicitHeight + 2 * ExoTheme.spacingMd
                color: entryDelegate.unread ? ExoTheme.surfaceHover : "transparent"

                // The whole entry announces itself once, severity first. The
                // dismiss button and the action buttons inside keep their own
                // names; this is the row they belong to.
                Accessible.role: Accessible.ListItem
                Accessible.name: entryDelegate.toneName + ". " + entryDelegate.title
                Accessible.description: entryDelegate.body

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

                ExoGlyph {
                    id: toneGlyph

                    kind: entryDelegate.toneGlyph
                    color: entryDelegate.toneColor
                    width: 14
                    height: 14
                    anchors {
                        left: parent.left
                        leftMargin: ExoTheme.spacingLg
                        top: parent.top
                        // Optically centred on the title's cap height: the 14 px
                        // glyph is shorter than the title's line box, so sharing
                        // the column's top margin would sit it visibly high.
                        topMargin: ExoTheme.spacingMd + 2
                    }
                }

                ColumnLayout {
                    id: entryColumn

                    spacing: ExoTheme.spacingXs
                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                        leftMargin: ExoTheme.spacingLg + toneGlyph.width + ExoTheme.spacingSm
                        rightMargin: ExoTheme.spacingLg
                        topMargin: ExoTheme.spacingMd
                        // No bottomMargin: without an `anchors.bottom` it would be
                        // inert. The bottom padding is the `2 *` in the delegate's
                        // own height above, which is the one place that decides it.
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

                        // Same as the two header actions, plus the hit
                        // target QCR-506 asks of a control this small:
                        // the glyph stays 12 px, the button around it
                        // is 24.
                        AbstractButton {
                            id: dismissButton

                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 24
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Dismiss")
                            onClicked: root.notifications.dismissEntry(entryDelegate.index)

                            background: Rectangle {
                                color: "transparent"
                                border.width: dismissButton.visualFocus ? ExoTheme.focusRingWidth : 0
                                border.color: ExoTheme.text
                                radius: ExoTheme.radiusXs
                            }

                            ExoGlyph {
                                anchors.centerIn: parent
                                kind: ExoGlyph.Close
                                color: dismissButton.hovered ? ExoTheme.text : ExoTheme.textMuted
                                width: 12
                                height: 12
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

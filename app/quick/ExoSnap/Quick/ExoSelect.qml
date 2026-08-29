import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Dropdown over an adapter-built option list. Each entry carries the
// capability owner's own `selectable` verdict and `reason`, so an unavailable
// option stays visible and explains itself instead of silently disappearing.
ComboBox {
    id: root

    required property var options
    required property var value

    // Shown instead of blank text when nothing in `options` matches `value` --
    // an empty list (nothing detected) and a value with no matching entry
    // (nothing pinned yet) both land on resolvedIndex === -1, and the control
    // must always display something rather than going empty.
    property string placeholderText: qsTr("(none selected)")

    // ComboBox.indexOfValue() is a plain call, so a binding on it never
    // re-evaluates when `options` is replaced -- the adapter rebuilds its option
    // lists on every edit, which would leave the control permanently blank.
    // Resolving the index over both inputs keeps the binding correct.
    readonly property int resolvedIndex: {
        const entries = root.options;
        if (entries === undefined || entries === null) {
            return -1;
        }
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i].value === root.value) {
                return i;
            }
        }
        return -1;
    }

    signal valueActivated(var value)

    implicitHeight: ExoTheme.controlHeight
    model: root.options
    textRole: "label"
    valueRole: "value"
    currentIndex: root.resolvedIndex
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    HoverHandler {
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
    }

    contentItem: Label {
        text: root.currentIndex === -1 ? root.placeholderText : root.displayText
        textFormat: Text.PlainText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        // A locked select still states its current value on a readable rung:
        // the chevron and the control's fill carry "not changeable" on their
        // own. Only the empty selection stays on the disabled rung, because
        // there is no value to read there.
        color: root.currentIndex === -1 ? ExoTheme.textDim
             : root.enabled ? ExoTheme.text : ExoTheme.textSecondary
        leftPadding: ExoTheme.spacingMd
        rightPadding: root.indicator.width + ExoTheme.spacingMd
        font {
            family: ExoTheme.sansFamily
            pixelSize: ExoTheme.fontBody
            italic: root.currentIndex === -1
        }
    }

    // The Basic style's own indicator is a DOUBLE arrow, which reads as a spinner
    // rather than a list. One chevron, pointing the way the popup opens, and
    // flipped while it is open so the control says what dismissing it will do.
    indicator: ExoChevron {
        x: root.width - width - ExoTheme.spacingMd
        y: root.topPadding + (root.availableHeight - height) / 2
        direction: root.popup.visible ? 180 : 0
        tone: !root.enabled ? ExoTheme.textDim
                            : root.popup.visible || root.hovered ? ExoTheme.text : ExoTheme.textMuted

        Behavior on rotation {
            NumberAnimation {
                duration: ExoTheme.animFast
                easing.type: Easing.OutCubic
            }
        }
    }

    background: Rectangle {
        color: !root.enabled ? ExoTheme.surface
                             : root.popup.visible || root.hovered ? ExoTheme.hoverTint(ExoTheme.surfaceRaised)
                                                                  : ExoTheme.surfaceRaised
        border.width: 1
        border.color: root.visualFocus || root.popup.visible ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
    }

    delegate: ItemDelegate {
        id: optionDelegate

        required property var model

        width: ListView.view.width
        height: ExoTheme.controlHeight
        enabled: optionDelegate.model.selectable
        highlighted: optionDelegate.ListView.isCurrentItem

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }

        contentItem: RowLayout {
            spacing: ExoTheme.spacingSm

            Label {
                text: optionDelegate.model.label
                textFormat: Text.PlainText
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                color: optionDelegate.enabled ? ExoTheme.text : ExoTheme.textDim
                leftPadding: ExoTheme.spacingSm
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                }
            }

            // Why this one cannot be chosen, stated in the row. It was a
            // ToolTip on a DISABLED delegate — and Qt delivers no hover events
            // to a disabled item, so it never appeared for any input at all.
            // The row it explains is the only place with the room for it.
            Label {
                text: optionDelegate.model.reason ?? ""
                textFormat: Text.PlainText
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
                visible: !optionDelegate.enabled && text !== ""
                color: ExoTheme.textDim
                rightPadding: ExoTheme.spacingSm
                Layout.maximumWidth: optionDelegate.width / 2
                Layout.minimumWidth: 0
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }
        }

        background: Rectangle {
            color: ExoTheme.surfaceHover
            visible: optionDelegate.highlighted || optionDelegate.hovered
            opacity: optionDelegate.highlighted ? 1.0 : 0.55
        }

        // QCR-509. The reason also rides on the accessible description, for the
        // enabled and the disabled case alike.
        Accessible.description: optionDelegate.model.reason ?? ""
        // The tooltip stays for an ENABLED option — an option can carry a
        // reason without being unavailable, and hover works there. It is the
        // DISABLED case the row above had to take over, because Qt delivers no
        // hover events to a disabled item at all.
        ToolTip.visible: optionDelegate.hovered && (optionDelegate.model.reason ?? "") !== ""
        ToolTip.text: optionDelegate.model.reason ?? ""
    }

    popup: Popup {
        y: root.height
        width: root.width
        implicitHeight: Math.min(contentItem.implicitHeight, 280)
        padding: 1

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.delegateModel
            currentIndex: root.highlightedIndex

            ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            color: ExoTheme.surfaceRaised
            border.width: 1
            border.color: ExoTheme.lineStrong
            radius: ExoTheme.radiusSm
        }
    }

    onActivated: index => root.valueActivated(root.valueAt(index))
}

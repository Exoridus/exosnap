import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Label + hint on the left, one control on the right. `stacked` is driven by the
// hosting page rather than this row's own width so the column count can never
// feed back into the layout pass that determines it.
GridLayout {
    id: root

    required property string label
    // A row carries exactly ONE explanation mode:
    //   none      -- label and control, and nothing else to say;
    //   `hint`    -- one visible line that adds to the decision. One line: if the
    //                explanation needs to wrap it is not a hint, it is an `info`;
    //   `info`    -- a focusable trigger opening a paragraph beside the row.
    // Setting both is a design error, not a richer row, and the check below fails
    // loudly rather than rendering two greys that summarise each other.
    property string hint: ""
    property string info: ""
    property string warning: ""
    property bool stacked: false
    property int controlWidth: 220

    Component.onCompleted: {
        if (root.hint !== "" && root.info !== "") {
            console.error("ExoSettingRow '" + root.label
                          + "': hint and info are mutually exclusive -- pick the one-line hint or the popover.");
        }
    }

    default property alias control: controlHost.data

    // A blocked row dims as a whole. The lock is applied per CONTROL across this
    // product (`enabled: !settings.controlsLocked`), so a row whose control is
    // greyed out kept a full-strength label beside it and read as an ordinary
    // setting the user had simply failed to notice was inert. Read off the first
    // hosted control: rows with two controls (a resolution and a frame rate) gate
    // both on the same condition.
    // Layouts are recursed into, controls are not. A row whose slot holds a
    // RowLayout of a resolution and a frame rate has to see both; descending into
    // a control's own internals would reach things like a Select's unselectable
    // option delegates and dim the row for them.
    function _slotState(host) {
        let total = 0;
        let live = 0;
        const kids = host.children;
        for (let i = 0; i < kids.length; ++i) {
            const child = kids[i];
            if (!child)
                continue;
            if (String(child).indexOf("Layout") !== -1) {
                const inner = root._slotState(child);
                total += inner.total;
                live += inner.live;
                continue;
            }
            // A CONTROL, not everything in the slot. `hoverEnabled` is what every
            // Qt Quick Control carries and no plain Item does, which is the
            // cheapest honest way to tell the switch apart from the level meter
            // and the unit label sitting beside it -- both of which stay enabled
            // whatever the control next to them is doing.
            if (child.hoverEnabled === undefined)
                continue;
            total += 1;
            if (child.enabled !== false)
                live += 1;
        }
        return { total: total, live: live };
    }

    // Dim only when EVERY control in the slot is blocked. One live control is
    // enough to keep the label at full strength: the Microphone row's slot holds
    // a switch beside a "Mix into previous track" box that gates on something
    // else entirely, and dimming its label because the box was inert said the
    // switch could not be used either.
    readonly property bool controlEnabled: {
        const state = root._slotState(controlHost);
        return state.total === 0 || state.live > 0;
    }

    columns: root.stacked ? 1 : 2
    columnSpacing: ExoTheme.spacingLg
    rowSpacing: ExoTheme.spacingXs

    ColumnLayout {
        spacing: 0
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignVCenter

        RowLayout {
            spacing: ExoTheme.spacingXs
            Layout.fillWidth: true

            // The trigger belongs against the label it explains, so the label only
            // claims the row's width when there is no trigger to sit beside it.
            Label {
                text: root.label
                textFormat: Text.PlainText
                wrapMode: root.info === "" ? Text.WordWrap : Text.NoWrap
                elide: root.info === "" ? Text.ElideNone : Text.ElideRight
                color: root.controlEnabled ? ExoTheme.text : ExoTheme.textDim
                Layout.fillWidth: root.info === ""
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                }
            }

            ExoInfoButton {
                subject: root.label
                body: root.info
                visible: root.info !== ""
                Layout.alignment: Qt.AlignVCenter
            }

            Item {
                Layout.fillWidth: root.info !== ""
            }
        }

        Label {
            text: root.hint
            textFormat: Text.PlainText
            elide: Text.ElideRight
            visible: root.hint !== ""
            color: root.controlEnabled ? ExoTheme.textMuted : ExoTheme.textDim
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }

        Label {
            text: root.warning
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.warning !== ""
            color: ExoTheme.warningText
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }
    }

    ColumnLayout {
        id: controlHost

        spacing: ExoTheme.spacingXs
        Layout.fillWidth: root.stacked
        Layout.preferredWidth: root.stacked ? -1 : root.controlWidth
        Layout.alignment: root.stacked ? Qt.AlignLeft : Qt.AlignRight | Qt.AlignVCenter
    }
}

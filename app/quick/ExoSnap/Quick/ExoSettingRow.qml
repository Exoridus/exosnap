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
    // A resolved value the control does not itself show, such as the quantizer a
    // CQ point maps to on the selected codec. It occupies the hint's line and is
    // NOT an explanation mode: a row may carry a readout beside an `info`, because
    // the two cannot restate each other -- one is what the setting means, the
    // other is what it currently resolves to.
    property string readout: ""
    // A caution about the current choice -- a capability limit, a side effect.
    // Shown as a warning mark beside the label that opens the text, never as a
    // wrapping line under the row, so it moves nothing on the page.
    property string warning: ""
    property bool stacked: false
    property int controlWidth: ExoTheme.controlSlot

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
    // enough to keep the label at full strength: a slot holding two controls that
    // gate on different conditions would otherwise dim its whole label for the
    // one that happens to be inert, saying the other could not be used either.
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
                wrapMode: root.info === "" && root.warning === "" ? Text.WordWrap : Text.NoWrap
                elide: root.info === "" && root.warning === "" ? Text.ElideNone : Text.ElideRight
                // The label keeps its text rung when the row is locked. What a
                // running recording takes away is the ability to CHANGE the
                // setting, not the user's need to read which setting it is, and
                // a whole page of labels on the disabled rung answers nothing.
                // Only the interaction chrome and the secondary hint recede.
                color: ExoTheme.text
                Layout.fillWidth: root.info === "" && root.warning === ""
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
                Layout.leftMargin: ExoTheme.spacingXs
                // The label's line box carries descender space below the
                // letters, so its geometric centre sits under the text's visual
                // centre; lifting the glyph by that offset centres it on the word.
                Layout.bottomMargin: 2
            }

            // Created only for a row that has a caution: almost none do, and an
            // instantiated-but-hidden trigger per row costs a popup each.
            Loader {
                active: root.warning !== ""
                visible: active
                Layout.alignment: Qt.AlignVCenter
                Layout.leftMargin: root.info !== "" ? 0 : ExoTheme.spacingXs
                Layout.bottomMargin: 2

                sourceComponent: ExoInfoButton {
                    subject: root.label
                    body: root.warning
                    caution: true
                }
            }

            Item {
                Layout.fillWidth: root.info !== "" || root.warning !== ""
            }
        }

        Label {
            objectName: "secondaryLine"
            text: root.hint !== "" ? root.hint : root.readout
            textFormat: Text.PlainText
            elide: Text.ElideRight
            visible: root.hint !== "" || root.readout !== ""
            color: root.controlEnabled ? ExoTheme.textMuted : ExoTheme.textDim
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
        // A control wider than the slot takes the room from the label column,
        // which elides its hint; with only a preferred width it was drawn across
        // the label instead.
        Layout.minimumWidth: root.stacked ? -1 : controlHost.implicitWidth
        Layout.alignment: root.stacked ? Qt.AlignLeft : Qt.AlignRight | Qt.AlignVCenter
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One audio source as a block, not as a settings row.
//
// A source carries four statements -- included or not, which source it is, what
// it is reading, and which track it lands on -- and four statements do not fit
// in one control slot. The header spends the width on three FIXED columns, and
// the level gets a line of its own underneath at the card's full width, where a
// meter is wide enough to be read as a measurement.
//
// Track resolution itself stays engine-side; this block only submits the two
// editable flags.
ColumnLayout {
    id: root

    required property string label
    required property bool sourceEnabled
    required property bool separateTrack
    required property bool locked
    required property real meterLevel
    required property bool stacked
    // The same reading as the meter, in dBFS. Negative infinity means the source
    // produced nothing at all, which is not the same statement as a level at the
    // bottom of the scale and is not printed as one.
    required property real meterDb
    property string hint: ""
    // False only for a source row that renders as the topmost VISIBLE row:
    // "mix into previous track" is meaningless when there is no visible row above
    // it. The column keeps its width regardless -- see below.
    property bool showMixOption: true
    // Whole-dB gain applied to this source in the mix; hidden when the source
    // carries its own gain elsewhere (the microphone).
    property bool showGain: false
    property int gainDb: 0

    signal sourceToggled(bool value)
    signal separateToggled(bool value)
    signal gainCommitted(int value)

    readonly property bool _live: root.sourceEnabled && !root.locked

    spacing: ExoTheme.spacingSm

    RowLayout {
        spacing: ExoTheme.spacingMd
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.sourceEnabled
            enabled: !root.locked
            Accessible.name: qsTr("Enable %1").arg(root.label)
            Layout.alignment: Qt.AlignTop
            onToggledByUser: value => root.sourceToggled(value)
        }

        ColumnLayout {
            spacing: 0
            Layout.fillWidth: true

            Label {
                text: root.label
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                }
            }

            // The hint is where the MEANING lives. "System audio" records
            // everything on a display target and everything except one process on
            // a window target; the label stays put across that change so the row
            // keeps its identity, and this line says which of the two it is.
            Label {
                text: root.hint
                textFormat: Text.PlainText
                elide: Text.ElideRight
                visible: root.hint !== ""
                color: root._live ? ExoTheme.textMuted : ExoTheme.textDim
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }
        }

        ExoNumberField {
            visible: root.showGain
            from: -60
            to: 24
            suffix: qsTr("dB")
            value: root.gainDb
            enabled: root._live
            Layout.preferredWidth: 96
            Layout.alignment: Qt.AlignTop
            Accessible.name: qsTr("%1 gain").arg(root.label)
            onValueCommitted: value => root.gainCommitted(value)
        }

        // Column two: the reading. Fixed width and tabular figures, so every row
        // on the card shares one right edge whatever it is showing.
        Label {
            text: !root._live ? "—"
                : root.meterDb === Number.NEGATIVE_INFINITY ? qsTr("-∞ dB")
                : qsTr("%1 dB").arg(root.meterDb.toFixed(1))
            textFormat: Text.PlainText
            horizontalAlignment: Text.AlignRight
            color: root._live ? ExoTheme.textSecondary : ExoTheme.textDim
            Layout.preferredWidth: 68
            Layout.alignment: Qt.AlignTop
            Accessible.name: qsTr("%1 level").arg(root.label)
            font {
                family: ExoTheme.monoFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        // Column three: the track assignment. It keeps its width even for a row
        // that cannot offer it -- `visible` would let the reading column slide
        // sideways depending on which rows happen to be shown, and a placeholder
        // dash in an empty slot is a value that means nothing.
        ExoCheckBox {
            text: qsTr("Mix into previous track")
            checked: !root.separateTrack
            // Not focusable while it is holding the column open: an invisible
            // control in the tab order is a stop the eye cannot account for.
            enabled: root.showMixOption && !root.locked && root.sourceEnabled
            opacity: root.showMixOption ? 1 : 0
            visible: !root.stacked || root.showMixOption
            Layout.preferredWidth: 172
            Layout.alignment: Qt.AlignTop
            Accessible.ignored: !root.showMixOption
            Accessible.name: qsTr("Mix %1 into the previous track").arg(root.label)
            onToggledByUser: value => {
                if (root.showMixOption)
                    root.separateToggled(!value);
            }
        }
    }

    // No meter for a source that is off. A greyed bar the width of the card
    // states nothing and takes exactly as much of the eye as the live one beside
    // it; the row collapsing to its header is the honest shape of "not recording
    // this".
    ColumnLayout {
        spacing: ExoTheme.spacingXs
        visible: root._live
        Layout.fillWidth: true

        ExoLevelMeter {
            level: root.meterLevel
            active: root._live
            // Twice the dock's count: this meter has a whole card to itself, and
            // the extra width buys resolution rather than wider cells.
            segmentCount: 32
            implicitHeight: 12
            Layout.fillWidth: true
        }

        ExoLevelMeterScale {
            active: root._live
            Layout.fillWidth: true
        }
    }
}

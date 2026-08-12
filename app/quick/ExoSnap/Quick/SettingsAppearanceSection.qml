pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Two independent choices, two controls.
//
// It used to be one dropdown of four complete themes, which meant picking a
// hue also picked a set of neutrals: "Dark · Indigo" was the only way to get an
// indigo accent, and it came with a different background than "Dark · Default".
// Splitting the axes makes each row answer one question.
ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Appearance")

    ExoSettingRow {
        label: qsTr("Appearance")
        hint: qsTr("Surface palette. Dark is the shipped default.")
        stacked: root.stacked
        Layout.fillWidth: true

        // Segmented rather than a dropdown: with exactly two mutually exclusive
        // values, a list that has to be opened to see the other one is a worse
        // control than two visible words.
        ExoSegmentedControl {
            options: root.settings.appearanceOptions.map(option => option.label)
            currentIndex: Math.max(0, root.settings.appearanceOptions
                                        .findIndex(option => option.value === root.settings.appearanceId))
            Accessible.name: qsTr("Appearance")
            onSelected: index => root.settings.appearanceId = root.settings.appearanceOptions[index].value
        }
    }

    ExoSettingRow {
        label: qsTr("Accent")
        hint: qsTr("Highlight colour for selection, active controls and the primary action. It never changes what recording, caution or ready look like.")
        stacked: root.stacked
        Layout.fillWidth: true

        Row {
            spacing: ExoTheme.spacingSm

            Repeater {
                model: root.settings.accentOptions

                // A swatch, not a colour name in a list: the whole point of this
                // row is the colour, and each entry is drawn in the value it will
                // actually take in the CURRENT appearance — the adapter resolves
                // that, so a light-mode swatch never shows its dark-mode value.
                delegate: AbstractButton {
                    id: swatch

                    required property var modelData

                    readonly property bool current: root.settings.accentId === swatch.modelData.value

                    width: ExoTheme.controlHeight
                    height: ExoTheme.controlHeight
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: swatch.modelData.label
                    Accessible.description: swatch.modelData.reason
                    Accessible.checked: swatch.current
                    onClicked: root.settings.accentId = swatch.modelData.value

                    ToolTip.visible: swatch.hovered
                    ToolTip.delay: 400
                    ToolTip.text: swatch.modelData.label

                    // The ring is the selection, and it is drawn in the theme's
                    // own line/text colours rather than in the accent: a swatch
                    // outlined in its own colour is invisible as a selection, and
                    // the two light accents that sit closest to the surface need
                    // the strongest ring, not the weakest.
                    background: Rectangle {
                        anchors.fill: parent
                        color: "transparent"
                        border.width: swatch.current ? 2 : 1
                        border.color: swatch.current ? ExoTheme.text
                                      : swatch.visualFocus ? ExoTheme.text
                                      : swatch.hovered ? ExoTheme.lineStrong : ExoTheme.line
                        radius: height / 2
                    }

                    Rectangle {
                        anchors.centerIn: parent
                        width: swatch.current ? parent.width - 10 : parent.width - 8
                        height: width
                        color: swatch.modelData.swatch
                        radius: height / 2
                    }
                }
            }
        }
    }
}

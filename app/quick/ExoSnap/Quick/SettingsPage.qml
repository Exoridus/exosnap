import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property SettingsAdapter settings

    // Card columns come from the shared width classes; the row stacking decision
    // is made per COLUMN, because that is the width a label/control pair actually
    // gets. Below the stacking threshold the pair goes one over the other, which
    // is what keeps the 860x700 minimum window usable instead of clipped.
    //
    // Settings stops at two columns even on a wide screen: a third would break a
    // sequential configuration into pieces nobody reads in order.
    readonly property int cardColumns: ExoTheme.columnsFor(root.width, 2)
    readonly property bool stackedRows: ExoTheme.stackRows(root.width / root.cardColumns)

    objectName: "quickSettingsPage"

    // ── The sections themselves, instantiated exactly once ───────────────────
    //
    // Two compositions below claim them through LayoutItemProxy, which is what
    // QtQuick.Layouts provides for responsive layouts whose HIERARCHY differs
    // between sizes. The alternative — one copy of every section per breakpoint —
    // is two page implementations that drift apart.
    //
    // They start invisible; the controlling proxy makes its target visible.
    SettingsPresetBar {
        id: presetSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsFormatSection {
        id: formatSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsQualitySection {
        id: qualitySection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsAudioSection {
        id: audioSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsOutputSection {
        id: outputSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsWebcamSection {
        id: webcamSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsOverlaysSection {
        id: overlaysSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsPresenceSection {
        id: presenceSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsHotkeysSection {
        id: hotkeysSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsUpdatesSection {
        id: updatesSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsAppearanceSection {
        id: appearanceSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsDeveloperSection {
        id: developerSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    ColumnLayout {
        spacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            margins: ExoTheme.pagePadding
        }

        RowLayout {
            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true
            Layout.bottomMargin: ExoTheme.spacingXs

            Label {
                text: qsTr("Settings")
                textFormat: Text.PlainText
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontPageTitle
                    weight: Font.DemiBold
                }
            }

            Label {
                text: qsTr("Expert mode")
                textFormat: Text.PlainText
                color: root.settings.expertMode ? ExoTheme.accent : ExoTheme.textSecondary
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                }
            }

            ExoSwitch {
                checked: root.settings.expertMode
                Accessible.name: qsTr("Expert mode")
                onToggledByUser: value => root.settings.expertMode = value
            }
        }

        ExoNotice {
            text: qsTr("Recording is in progress — settings that change the output format are locked until it stops.")
            tone: "info"
            visible: root.settings.controlsLocked
            Layout.fillWidth: true
        }

        ExoScrollView {
            id: scroll

            contentWidth: availableWidth
            clip: true
            // The vertical scroll bar is an overlay, so `availableWidth` still
            // spans underneath it. Reserving the gutter keeps the right column's
            // controls from being covered. The page always scrolls, so this is
            // not a conditional binding (which would feed back into wrap height).
            rightPadding: ExoTheme.spacingLg
            Layout.fillWidth: true
            Layout.fillHeight: true

            // One content item for the ScrollView; which composition fills it is
            // decided below.
            Item {
                id: compositionHost

                width: scroll.availableWidth
                implicitHeight: root.cardColumns === 1 ? oneColumn.implicitHeight
                                                       : twoColumn.implicitHeight

                // ── Narrow: one column, canonical section order ──────────────
                ColumnLayout {
                    id: oneColumn

                    width: compositionHost.width
                    spacing: ExoTheme.sectionGap
                    visible: root.cardColumns === 1

                    LayoutItemProxy { target: presetSection }
                    LayoutItemProxy { target: formatSection }
                    LayoutItemProxy { target: qualitySection }
                    LayoutItemProxy { target: audioSection }
                    LayoutItemProxy { target: outputSection }
                    LayoutItemProxy { target: webcamSection }
                    LayoutItemProxy { target: overlaysSection }
                    LayoutItemProxy { target: presenceSection }
                    LayoutItemProxy { target: hotkeysSection }
                    LayoutItemProxy { target: updatesSection }
                    LayoutItemProxy { target: appearanceSection }
                    LayoutItemProxy { target: developerSection }
                }

                // ── Wide: two INDEPENDENT columns ────────────────────────────
                //
                // Independent, not a two-column grid: a grid aligns its rows, so a
                // short card beside a tall one leaves the difference as dead space.
                //
                // The split follows the canonical order rather than balancing
                // heights — left is the recording chain (format → quality → audio
                // → output), right is everything around it — so reading order stays
                // top-to-bottom within a column.
                ColumnLayout {
                    id: twoColumn

                    width: compositionHost.width
                    spacing: ExoTheme.sectionGap
                    visible: root.cardColumns > 1

                    LayoutItemProxy { target: presetSection }

                    RowLayout {
                        spacing: ExoTheme.sectionGap
                        Layout.fillWidth: true

                        ColumnLayout {
                            spacing: ExoTheme.sectionGap
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.alignment: Qt.AlignTop

                            LayoutItemProxy { target: formatSection }
                            LayoutItemProxy { target: qualitySection }
                            LayoutItemProxy { target: audioSection }
                            LayoutItemProxy { target: outputSection }

                            Item {
                                Layout.fillHeight: true
                            }
                        }

                        ColumnLayout {
                            spacing: ExoTheme.sectionGap
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.alignment: Qt.AlignTop

                            LayoutItemProxy { target: webcamSection }
                            LayoutItemProxy { target: overlaysSection }
                            LayoutItemProxy { target: presenceSection }
                            LayoutItemProxy { target: hotkeysSection }
                            LayoutItemProxy { target: updatesSection }
                            LayoutItemProxy { target: appearanceSection }
                            LayoutItemProxy { target: developerSection }

                            Item {
                                Layout.fillHeight: true
                            }
                        }
                    }
                }
            }
        }
    }
}

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property SettingsAdapter settings

    // How wide the configuration itself is allowed to get, independent of the
    // window. Uncapped, a 1600 px window put a setting's label at the far left
    // and the control that changes it at the far right with half a screen of
    // nothing between them — technically responsive, unusable to scan.
    //
    // `contentBox` is what the scroll view actually offers (page padding on both
    // sides, minus the reserved scrollbar gutter on the right), so the page title
    // above the scroll view and the cards inside it centre on the SAME axis
    // rather than on two that differ by the gutter.
    readonly property real contentBox: Math.max(0, root.width - 2 * ExoTheme.pagePadding - ExoTheme.spacingLg)
    readonly property real contentWidth: Math.min(root.contentBox, ExoTheme.contentMaxWidth)
    readonly property real sideInset: Math.max(0, (root.contentBox - root.contentWidth) / 2)

    // Card columns come from the shared width classes; the row stacking decision
    // is made per COLUMN, because that is the width a label/control pair actually
    // gets. Below the stacking threshold the pair goes one over the other, which
    // is what keeps the 860x700 minimum window usable instead of clipped.
    //
    // Measured against the CONTENT width, not the window: the column count has to
    // answer "does a second column of settings fit", and after the cap above the
    // window no longer answers that question.
    //
    // Settings stops at two columns even on a wide screen: a third would break a
    // sequential configuration into pieces nobody reads in order.
    readonly property int cardColumns: ExoTheme.columnsFor(root.contentWidth, 2)
    readonly property bool stackedRows: ExoTheme.stackRows(root.contentWidth / root.cardColumns)

    objectName: "quickSettingsPage"

    // Harness-only, same class of hook as --record-visual-menu: scrolls to the
    // end of the page so a --visual-test capture can photograph the sections
    // below the fold. Appearance is the last card on the page and there is no
    // window height on any real display that reaches it — the window is clamped
    // to the screen work area long before the content ends.
    function scrollToBottom(): void {
        scrollToBottomTimer.restart();
    }

    // ── Automation targets (protocol 2 ui.reveal) ────────────────────────────
    //
    // Stable product names on the left, this document's items on the right. The
    // QML ids below are document-local and must never appear on the wire: a
    // client that could name an id would be naming an implementation detail, and
    // the twelve sections are a product decision (CLAUDE.md) that happens to be
    // spelled with ids here.
    //
    // The set is closed. An unknown name is an error, never a silent no-op —
    // that is exactly the trap --settings-visual-bottom fell into, where a
    // findChild() against nullptr did nothing while every capture claimed to
    // show the end of the page.
    readonly property var automationTargets: ({
        "preset": presetSection,
        "format": formatSection,
        "quality": qualitySection,
        "audio": audioSourcesSection,
        "microphone": microphoneSection,
        "audio-encoding": audioEncodingSection,
        "output": outputSection,
        "webcam": webcamSection,
        "overlays": overlaysSection,
        "presence": presenceSection,
        "hotkeys": hotkeysSection,
        "updates": updatesSection,
        "appearance": appearanceSection,
        "developer": developerSection
    })

    // -1 = no such target, 0 = a real target that did not end up in the
    // viewport, 1 = revealed. Three answers rather than two, because "you asked
    // for something that does not exist" and "what you asked for did not
    // happen" are different findings and the protocol reports them as different
    // errors.
    function revealAutomationTarget(name: string): int {
        const section = root.automationTargets[name];
        if (section === undefined || section === null)
            return -1;
        return scroll.revealItem(section) ? 1 : 0;
    }

    function scrollAutomationHome(): bool {
        return scroll.scrollToHome();
    }

    function scrollAutomationEnd(): bool {
        return scroll.scrollToEnd();
    }

    function focusOutputDestination(): void {
        outputFocusTimer.remainingAttempts = 8;
        outputFocusTimer.restart();
    }

    // The card is the unit a deep link arrives at. Output is the one destination
    // that also focuses a field, because its action exists so the user can retype
    // a folder; the others only have to be found.
    function sectionForFocusTarget(target: int): var {
        switch (target) {
        case SettingsAdapter.OutputDestination:
            return outputSection;
        case SettingsAdapter.AudioSources:
            return audioSourcesSection;
        case SettingsAdapter.Format:
            return formatSection;
        case SettingsAdapter.Webcam:
            return webcamSection;
        case SettingsAdapter.Presence:
            return presenceSection;
        case SettingsAdapter.Appearance:
            return appearanceSection;
        case SettingsAdapter.Hotkeys:
            return hotkeysSection;
        case SettingsAdapter.Updates:
            return updatesSection;
        }
        return null;
    }

    // The mark says "this one" for a beat. Without it a jump into a card that was
    // already on screen scrolls nothing and looks like nothing happened.
    function landOnSection(section: var): void {
        if (!section)
            return;
        scroll.revealItem(section);
        section.land();
    }

    // The Settings page is loaded lazily, and its proxy-based layout settles
    // over several frames. A single reveal can therefore target pre-layout
    // geometry and be reset before the destination field receives focus.
    Timer {
        id: outputFocusTimer

        property int remainingAttempts: 0

        interval: 150
        repeat: true
        onTriggered: {
            scroll.revealItem(outputSection);
            outputSection.focusDestination();
            remainingAttempts -= 1;
            if (remainingAttempts <= 0)
                stop();
        }
    }

    // Harness-only: the page owns the sections, so a --visual-dialog request
    // lands here and is forwarded to whichever one holds the dialog.
    function openHarnessDialog(name: string): bool {
        return presetSection.openHarnessDialog(name);
    }

    // Repeating, not one-shot: the request arrives during startup, when the
    // column layout has not settled and contentHeight is still 0, and anything
    // that rebuilds the content afterwards (the harness applying an appearance,
    // a card hydrating) puts the view back at the top. Keeping it pinned until
    // the capture fires is the only version of this that is not a race. It runs
    // only in a harness process, which exits as soon as it has its screenshot.
    Timer {
        id: scrollToBottomTimer

        interval: 250
        repeat: true
        onTriggered: scroll.contentItem.contentY = Math.max(0, scroll.contentHeight - scroll.height)
    }

    Connections {
        target: root.settings

        function onSettingsFocusRequested(target: int): void {
            root.landOnSection(root.sectionForFocusTarget(target));
            // Focus AFTER the reveal: focusing first scrolls the field into view
            // on its own terms and undoes the position the reveal just set.
            if (target === SettingsAdapter.OutputDestination)
                root.focusOutputDestination();
        }
    }

    Component.onCompleted: {
        if (root.settings.folderValidation.length > 0)
            root.focusOutputDestination();
    }

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

    SettingsAudioSourcesSection {
        id: audioSourcesSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsMicrophoneSection {
        id: microphoneSection

        settings: root.settings
        stacked: root.stackedRows
        visible: false
        Layout.fillWidth: true
    }

    SettingsAudioEncodingSection {
        id: audioEncodingSection

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

        ExoNotice {
            text: qsTr("Recording is in progress — settings that change the output format are locked until it stops.")
            tone: "info"
            visible: root.settings.controlsLocked
            Layout.fillWidth: true
            Layout.leftMargin: root.sideInset
            Layout.rightMargin: root.sideInset + ExoTheme.spacingLg
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

                // Capped and centred, not stretched. `sideInset` is shared with
                // the page title above so the two line up exactly.
                x: root.sideInset
                width: root.contentWidth
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
                    LayoutItemProxy { target: audioSourcesSection }
                    LayoutItemProxy { target: microphoneSection }
                    LayoutItemProxy { target: outputSection }
                    LayoutItemProxy { target: presenceSection }
                    LayoutItemProxy { target: hotkeysSection }
                    LayoutItemProxy { target: appearanceSection }
                    LayoutItemProxy { target: qualitySection }
                    LayoutItemProxy { target: audioEncodingSection }
                    LayoutItemProxy { target: webcamSection }
                    LayoutItemProxy { target: overlaysSection }
                    LayoutItemProxy { target: updatesSection }
                    LayoutItemProxy { target: developerSection }
                }

                // ── Wide: two INDEPENDENT columns ────────────────────────────
                //
                // Independent, not a two-column grid: a grid aligns its rows, so a
                // short card beside a tall one leaves the difference as dead space.
                //
                // The split follows the canonical order rather than balancing
                // heights — left is the recording chain (format → quality → audio
                // → output) plus notifications & presence, right is everything
                // else around it — so reading order stays top-to-bottom within a
                // column. Presence sits in the left column purely to keep the two
                // columns' endpoints reasonably close; it is not part of the
                // recording chain itself.
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
                            LayoutItemProxy { target: audioSourcesSection }
                            LayoutItemProxy { target: microphoneSection }
                            LayoutItemProxy { target: outputSection }
                            LayoutItemProxy { target: presenceSection }
                            LayoutItemProxy { target: hotkeysSection }
                            LayoutItemProxy { target: appearanceSection }

                            Item {
                                Layout.fillHeight: true
                            }
                        }

                        ColumnLayout {
                            spacing: ExoTheme.sectionGap
                            Layout.fillWidth: true
                            Layout.preferredWidth: 1
                            Layout.alignment: Qt.AlignTop

                            LayoutItemProxy { target: qualitySection }
                            LayoutItemProxy { target: audioEncodingSection }
                            LayoutItemProxy { target: webcamSection }
                            LayoutItemProxy { target: overlaysSection }
                            LayoutItemProxy { target: updatesSection }
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

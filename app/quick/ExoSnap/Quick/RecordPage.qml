pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property RecordViewModelAdapter recordViewModel
    property GamepadOverlay gamepadOverlay: null
    required property RecordingClipAdapter clipAdapter
    required property RecordPreviewAdapter previewAdapter
    required property RecordingWorkspaceState workspaceState
    required property SettingsAdapter settings
    // The shell adapter, for the two directions the source picker now has to
    // travel: it publishes whether the picker is on screen (nothing outside this
    // document could observe that before), and it carries the open/close request
    // the control channel sends, so automation opens the real picker instead of
    // bypassing the surface the way record.selectTarget does.
    required property ShellAdapter shell
    property bool active: false
    property bool benchmarkInteractionActive: false
    property bool showMetricsOverlay: false
    property int pendingProfileTarget: -1
    focus: true
    Keys.onEscapePressed: event => {
        event.accepted = root.workspaceState.closeTopPanel();
    }

    objectName: "quickRecordPage"
    ExoButton {
        id: sourcesButton
        visible: root.workspaceState.sourceState === "Closed"
        z: 3
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: ExoTheme.spacingSm
        text: qsTr("Sources")
        Accessible.name: qsTr("Open recording sources")
        onClicked: root.workspaceState.toggleSources()
    }

    ExoButton {
        id: profileButton
        visible: root.workspaceState.profileState === "Closed"
        z: 3
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: ExoTheme.spacingSm
        text: qsTr("Profile")
        Accessible.name: qsTr("Open recording profile")
        onClicked: root.workspaceState.toggleProfile()
    }

    Binding {
        target: root.previewAdapter
        property: "active"
        value: root.active
    }

    Binding {
        target: root.recordViewModel
        property: "active"
        value: root.active
    }

    // What the toolbar's glyph says the capture target is. The kind used to be
    // spelled out as an eyebrow ("SCREEN") beside a name that already said
    // "Display 1"; one icon carries the same fact without a second text run, and
    // the accessible name below still states it in words.
    readonly property int sourceGlyph: root.recordViewModel.sourceKindText === "WINDOW" ? ExoGlyph.AppWindow
                                       : root.recordViewModel.sourceKindText === "REGION" ? ExoGlyph.Region
                                       : ExoGlyph.Display

    ColumnLayout {
        spacing: ExoTheme.spacingLg
        anchors {
            fill: parent
            margins: ExoTheme.spacingLg
        }

        // ── Preview Surface ──────────────────────────────────────────────────
        Item {
            id: previewHost
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 280

            RecordingWorkspaceSurface {
                id: workspaceSurface
                anchors.fill: parent
                workspaceState: root.workspaceState

                Rectangle {
                    id: previewSurface

                    readonly property real sourceAspect: {
                        const sourceWidth = root.previewAdapter.sourceSize.width
                                            * root.recordViewModel.normalizedSourceRect.width
                        const sourceHeight = root.previewAdapter.sourceSize.height
                                             * root.recordViewModel.normalizedSourceRect.height
                        return sourceHeight > 0 ? sourceWidth / sourceHeight : 16 / 9
                    }
                    readonly property real frameWidth: Math.max(0, Math.min(width - 2, (height - 2) * previewSurface.sourceAspect))

                    anchors.fill: parent
                    color: "#08080A"
                    border.width: 1
                    border.color: ExoTheme.overlayLine
                    radius: ExoTheme.radiusLg

                    // ── The frame ────────────────────────────────────────────────
                    //
                    // No fill of its own: the surface behind it already is the black
                    // stage. This is the coordinate space the video and everything
                    // drawn over it share, and it is what the webcam overlay measures
                    // its normalized rect against.
                    Item {
                        id: previewStage

                        width: previewSurface.frameWidth
                        height: previewSurface.frameWidth / previewSurface.sourceAspect
                        anchors.centerIn: parent

                        ExoPreviewItem {
                            objectName: "quickPreviewItem"
                            previewAdapter: root.previewAdapter
                            gamepadOverlay: root.gamepadOverlay
                            function updateGamepadSize(): void {
                                if (root.gamepadOverlay)
                                    root.gamepadOverlay.setContentSize(Qt.size(sourceSize.width * normalizedSourceRect.width,
                                                                             sourceSize.height * normalizedSourceRect.height))
                            }
                            onSourceSizeChanged: updateGamepadSize()
                            onNormalizedSourceRectChanged: updateGamepadSize()
                            normalizedSourceRect: root.recordViewModel.normalizedSourceRect
                            cornerRadius: 0
                            anchors.fill: parent
                            visible: !root.previewAdapter.harnessTestCard && root.previewAdapter.previewFrameRate > 0
                            // The camera is composited into the preview texture by the
                            // recording's own compositor; the overlay below is only its
                            // editing chrome. Bound to the draft so a drag moves the
                            // real picture, not a stand-in.
                            webcamVisible: webcamOverlay.visible && webcamOverlay.idlePreview
                            webcamRect: webcamOverlay.draftRect
                        }

                        Label {
                            visible: root.previewAdapter.previewFrameRate === 0 && root.recordViewModel.selectedTargetAvailable
                            text: qsTr("Preview off")
                            color: ExoTheme.overlayInkMuted
                            anchors.centerIn: parent
                        }

                        PreviewTestCard {
                            objectName: "quickPreviewTestCard"
                            visible: root.previewAdapter.harnessTestCard && root.previewAdapter.frameReady && root.previewAdapter.previewFrameRate > 0
                            radius: 0
                            fontFamily: ExoTheme.sansFamily
                            anchors.fill: parent
                        }

                        // Harness seam (--visual-test record-brand): the stage
                        // carrying the brand alone, at a size the empty state has no
                        // room for. Unreachable in a product run on purpose -- what
                        // the page owes a user with no source is the instruction,
                        // not a logo over it.
                        Row {
                            spacing: ExoTheme.spacingMd
                            visible: root.recordViewModel.harnessBrandStage
                            anchors.centerIn: parent

                            ExoBrandMark {
                                width: 112
                                height: 112
                                anchors.verticalCenter: parent.verticalCenter
                                Accessible.ignored: true
                            }

                            ExoBrandWordmark {
                                typePixelSize: 72
                                width: implicitWidth
                                height: implicitHeight
                                anchors.verticalCenter: parent.verticalCenter
                                Accessible.role: Accessible.StaticText
                                Accessible.name: "exosnap"
                            }
                        }

                        Column {
                            spacing: ExoTheme.spacingMd
                            visible: !root.recordViewModel.selectedTargetAvailable
                                     && !root.recordViewModel.harnessBrandStage
                            anchors.centerIn: parent

                            Label {
                                text: qsTr("Choose what to record")
                                color: ExoTheme.overlayInk
                                anchors.horizontalCenter: parent.horizontalCenter
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontPageTitle
                                    weight: Font.DemiBold
                                }
                            }

                            Label {
                                text: qsTr("Select a screen, window, or region to continue.")
                                color: ExoTheme.overlayInkMuted
                                anchors.horizontalCenter: parent.horizontalCenter
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontBody
                                }
                            }

                            ExoButton {
                                text: qsTr("Choose source")
                                leadingGlyph: ExoGlyph.Layers
                                tone: "primary"
                                enabled: root.recordViewModel.canSelectSource
                                anchors.horizontalCenter: parent.horizontalCenter
                                onClicked: root.openSourcePicker()
                            }
                        }

                        RecordWebcamOverlay {
                            id: webcamOverlay

                            stage: previewStage
                            visible: root.active && root.previewAdapter.previewFrameRate > 0 && root.recordViewModel.webcamEnabled
                            idlePreview: !root.recordViewModel.recording && !root.recordViewModel.paused
                                         && !root.recordViewModel.preparing && !root.recordViewModel.finalizing
                            modelRect: root.recordViewModel.webcamOverlayRect
                            editable: root.recordViewModel.webcamOverlayEditable
                            error: root.recordViewModel.webcamError
                            onRectRequested: rect => root.recordViewModel.requestWebcamOverlayRect(rect)
                        }

                        RecordWebcamOverlay {
                            id: gamepadPlacement
                            stage: previewStage
                            overlayName: qsTr("Gamepad overlay")
                            visible: root.gamepadOverlay !== null && root.gamepadOverlay.enabled
                            idlePreview: false
                            modelRect: root.gamepadOverlay ? root.gamepadOverlay.placement : Qt.rect(0, 0, 1, 1)
                            editable: root.recordViewModel.webcamOverlayEditable
                            onDraftRectChanged: {
                                if (root.gamepadOverlay && editable)
                                    root.gamepadOverlay.placement = draftRect
                            }
                            onRectRequested: rect => {
                                if (root.gamepadOverlay)
                                    root.gamepadOverlay.placement = rect
                            }
                        }

                        GamepadOverlayControls {
                            visible: root.gamepadOverlay !== null
                            overlay: root.gamepadOverlay
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: ExoTheme.spacingMd
                        }

                        RecordPreviewChrome {
                            parent: previewSurface
                            anchors.fill: parent
                            z: 10
                            formatText: root.recordViewModel.formatText
                            sourceText: root.recordViewModel.sourceButtonText
                            sourceGlyph: root.sourceGlyph
                            sourceEnabled: root.recordViewModel.canSelectSource
                            metricsVisible: (root.recordViewModel.recording || root.recordViewModel.paused) && root.recordViewModel.liveStatsAvailable
                            dropsText: root.recordViewModel.droppedFramesText
                            driftText: root.recordViewModel.driftText
                            sizeText: root.recordViewModel.outputSizeText
                            onSourceRequested: root.openSourcePicker()
                        }

                        // Harness surface, so it is not built in an ordinary run.
                        // `visible: false` was not enough: an instantiated Item
                        // evaluates its bindings regardless, so five preview-metrics
                        // properties stayed bound to an adapter that republishes them
                        // four times a second — for a panel nobody can see.
                        Loader {
                            active: root.benchmarkInteractionActive
                            anchors {
                                fill: parent
                                topMargin: ExoTheme.spacingLg
                                rightMargin: ExoTheme.spacingLg
                                bottomMargin: ExoTheme.spacingLg
                                leftMargin: ExoTheme.spacingLg
                            }

                            sourceComponent: PreviewMetricsOverlay {
                                expanded: root.showMetricsOverlay
                                frameReady: root.previewAdapter.frameReady
                                presentationRate: root.previewAdapter.presentationRate
                                sourceDeliveryRate: root.previewAdapter.sourceDeliveryRate
                                frameTimeP95Ms: root.previewAdapter.frameTimeP95Ms
                                frameTimeP99Ms: root.previewAdapter.frameTimeP99Ms
                                // Fixed-dark surface, fixed-dark ink: `surfaceColor`
                                // is a literal near-black in both appearances, so the
                                // three colours over it resolve against the Dark
                                // appearance rather than the application's.
                                accentColor: ExoTheme.overlayAccent
                                surfaceColor: "#E6151517"
                                textColor: ExoTheme.overlayInk
                                secondaryTextColor: ExoTheme.overlayInkSecondary
                                sansFamily: ExoTheme.sansFamily
                                monoFamily: ExoTheme.monoFamily
                                onToggled: expanded => root.showMetricsOverlay = expanded
                            }
                        }

                        Label {
                            // Only a real preview error. The blocked/failed fallback used
                            // to put `capabilityText` here — the configuration summary,
                            // which reads "Ready: MKV · AV1 NVENC · Opus · 60 fps". On a
                            // failed run that is amber text across the middle of the
                            // preview saying "Ready", and it repeats what the strip above
                            // the preview already states. Why the run failed belongs to
                            // the recording-error surface, which owns it.
                            text: root.previewAdapter.errorText
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            // Centred on the black stage, so the fixed-dark rung.
                            color: ExoTheme.overlayWarning
                            // Never alongside the placeholder. Both are centred on the
                            // same stage, so two visible at once overlap -- which is what
                            // the empty state used to do until the adapter stopped
                            // reporting it as an error. Belt and braces: a future
                            // producer of errorText cannot bring the overlap back.
                            visible: text.length > 0 && !root.recordViewModel.regionSelectionNeeded
                                     && root.recordViewModel.selectedTargetAvailable
                            width: Math.max(0, Math.min(440, parent.width - 48))
                            anchors.centerIn: parent
                            font.family: ExoTheme.sansFamily
                            font.pixelSize: ExoTheme.fontBody
                        }

                        Rectangle {
                            id: benchmarkActivity

                            width: 8
                            height: 8
                            // Also on the stage.
                            color: ExoTheme.overlayWarning
                            radius: 4
                            visible: root.benchmarkInteractionActive
                            anchors {
                                right: parent.right
                                bottom: parent.bottom
                                margins: ExoTheme.spacingLg
                            }

                            SequentialAnimation {
                                running: root.benchmarkInteractionActive && root.active
                                loops: Animation.Infinite
                                ScaleAnimator { target: benchmarkActivity; from: 1.0; to: 1.8; duration: 280 }
                                ScaleAnimator { target: benchmarkActivity; from: 1.8; to: 1.0; duration: 280 }
                            }
                        }
                    }
                }
            }

            Loader {
                id: sourcePanelLoader
                objectName: "recordingSourcePanel"
                z: 20
                active: root.workspaceState.sourceState !== "Closed"
                width: root.workspaceState.sourceWidth
                height: previewHost.height
                anchors.left: parent.left
                anchors.top: parent.top
                sourceComponent: RecordingSourcePanel {
                    recordViewModel: root.recordViewModel
                    workspaceState: root.workspaceState
                    settings: root.settings
                    onChooseSource: root.openSourcePicker()
                    onOpenProfile: target => root.openProfile(target)
                }
            }

            Loader {
                id: profilePanelLoader
                objectName: "recordingProfileInspector"
                z: 21
                active: root.workspaceState.profileState !== "Closed"
                width: root.workspaceState.profileWidth
                height: previewHost.height
                anchors.right: parent.right
                anchors.top: parent.top
                sourceComponent: RecordingProfileInspector {
                    settings: root.settings
                    workspaceState: root.workspaceState
                }
                onLoaded: root.focusPendingProfileTarget()
            }

            ExoNotice {
                z: 30
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: ExoTheme.spacingMd
                width: Math.min(parent.width - 2 * ExoTheme.spacingMd, 440)
                visible: root.workspaceState.notice.length > 0
                text: root.workspaceState.notice
                tone: "info"
            }
        }

        // One Preview Surface, 16 px, the transport dock: nothing sits between
        // them in any state. A finished recording is reached through its Saved
        // notification and Edit History, not through a row on this page.
        RecordTransportDock {
            recordViewModel: root.recordViewModel
            clipAdapter: root.clipAdapter
            routing: root.settings.audioRouting
            Layout.fillWidth: true
        }
    }

    // Built on first open, not at page load. A Popup constructs its contentItem
    // the moment the Popup itself is constructed — opening it later is only a
    // visibility change — so the whole picker, including the window list and its
    // delegates, used to be built while the Record page was being laid out. That
    // was a measured ~280-293 ms synchronous GUI-thread stall in all three
    // profiler traces, and in the auto-record trace it landed at t = 20.9 s,
    // i.e. DURING a running recording, because a targetOptionsChanged re-ran the
    // binding. The Loader moves the whole cost behind the one gesture that needs
    // it. Same idiom as AppShell's four overlay Loaders.
    //
    // Resident once loaded: the picker's own state is only the selection, which
    // lives in the view model, but rebuilding it on every open would pay the
    // construction cost again for no gain.
    Loader {
        id: sourcePickerLoader

        active: false

        sourceComponent: RecordSourcePicker {
            recordViewModel: root.recordViewModel
            hostPage: root
        }
    }

    function openSourcePicker(): void {
        sourcePickerLoader.active = true;
        // Loader is synchronous by default, so the item exists on the next line.
        // Cast because Loader.item is typed QObject — without it qmllint cannot
        // see Popup::open() and reports missing-property.
        const picker = sourcePickerLoader.item as RecordSourcePicker;
        if (picker)
            picker.open();
    }

    function openProfile(target: int): void {
        root.pendingProfileTarget = target;
        root.workspaceState.openProfile();
        root.focusPendingProfileTarget();
    }

    function focusPendingProfileTarget(): void {
        if (root.pendingProfileTarget < 0 || profilePanelLoader.status !== Loader.Ready)
            return;
        const target = root.pendingProfileTarget;
        Qt.callLater(() => {
            if (profilePanelLoader.status === Loader.Ready)
                (profilePanelLoader.item as RecordingProfileInspector).focusTarget(target);
        });
        root.pendingProfileTarget = -1;
    }

    function prepareRecordingPanelVisual(name: string): bool {
        if (["sources", "sources-pinned", "profile-pinned", "panels-pinned"].indexOf(name) < 0)
            return false;
        const sources = name !== "profile-pinned";
        const profile = name === "profile-pinned" || name === "panels-pinned";
        if (!sources)
            root.workspaceState.closeSources();
        if (!profile)
            root.workspaceState.closeProfile();
        if (sources) {
            root.workspaceState.openSources();
            if (name === "sources" && root.workspaceState.sourceState === "Pinned")
                root.workspaceState.toggleSourcePin();
            else if (name !== "sources" && root.workspaceState.sourceState !== "Pinned"
                     && root.workspaceState.canPinSources)
                root.workspaceState.pinSources();
        }
        if (profile) {
            root.workspaceState.openProfile();
            if (root.workspaceState.profileState !== "Pinned" && root.workspaceState.canPinProfile)
                root.workspaceState.pinProfile();
        }
        const sourceItem = sourcePanelLoader.item as Item;
        const profileItem = profilePanelLoader.item as Item;
        return (!sources || (root.workspaceState.sourceState !== "Closed" && sourcePanelLoader.status === Loader.Ready
                            && sourceItem && sourceItem.visible && sourceItem.width > 0 && sourceItem.height > 0))
                && (!profile || (root.workspaceState.profileState !== "Closed" && profilePanelLoader.status === Loader.Ready
                                 && profileItem && profileItem.visible && profileItem.width > 0 && profileItem.height > 0));
    }

    function prepareProfileVisual(expanded: bool): bool {
        root.workspaceState.openProfile();
        const profile = profilePanelLoader.status === Loader.Ready
                        ? profilePanelLoader.item as RecordingProfileInspector : null;
        if (!profile || !profile.visible || profile.width <= 0 || profile.height <= 0)
            return false;
        profile.setAutomationDetailsExpanded(expanded);
        return true;
    }

    function revealProfileTarget(name: string): int {
        root.workspaceState.openProfile();
        const profile = profilePanelLoader.status === Loader.Ready
                        ? profilePanelLoader.item as RecordingProfileInspector : null;
        return profile ? profile.revealAutomationTarget(name) : 0;
    }

    function openHarnessDialog(name: string): bool {
        root.workspaceState.openProfile();
        const profile = profilePanelLoader.status === Loader.Ready
                        ? profilePanelLoader.item as RecordingProfileInspector : null;
        return profile ? profile.openHarnessDialog(name) : false;
    }

    Connections {
        target: root.workspaceState
        function onPanelClosed(panel: string): void {
            Qt.callLater(() => {
                if (root.workspaceState.activePanel === "sources" && sourcePanelLoader.status === Loader.Ready)
                    (sourcePanelLoader.item as RecordingSourcePanel).forceActiveFocus(Qt.OtherFocusReason);
                else if (root.workspaceState.activePanel === "profile" && profilePanelLoader.status === Loader.Ready)
                    (profilePanelLoader.item as RecordingProfileInspector).forceActiveFocus(Qt.OtherFocusReason);
                else if (panel === "sources")
                    sourcesButton.forceActiveFocus(Qt.OtherFocusReason);
                else
                    profileButton.forceActiveFocus(Qt.OtherFocusReason);
            });
        }
    }

    // The counterpart, for a control-channel close. A picker that was never
    // built is already closed, which is why this is not an error.
    function closeSourcePicker(): void {
        const picker = sourcePickerLoader.item as RecordSourcePicker;
        if (picker)
            picker.close();
    }

    // Whether the picker is on screen, published to C++. `opened` rather than
    // `visible`: it is false for the whole exit transition, and a state the
    // channel reports must not be true while the surface is on its way out.
    Binding {
        target: root.shell
        property: "sourcePickerOpen"
        value: sourcePickerLoader.item !== null && (sourcePickerLoader.item as RecordSourcePicker).opened
    }

    Connections {
        target: root.shell

        function onSourcePickerRequested(open: bool): void {
            if (open)
                root.openSourcePicker();
            else
                root.closeSourcePicker();
        }
    }
}

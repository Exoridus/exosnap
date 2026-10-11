pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property RecordViewModelAdapter recordViewModel
    required property RecordPreviewAdapter previewAdapter
    // The shell adapter, for the two directions the source picker now has to
    // travel: it publishes whether the picker is on screen (nothing outside this
    // document could observe that before), and it carries the open/close request
    // the control channel sends, so automation opens the real picker instead of
    // bypassing the surface the way record.selectTarget does.
    required property ShellAdapter shell
    property bool active: false
    property bool benchmarkInteractionActive: false
    property bool showMetricsOverlay: false

    objectName: "quickRecordPage"

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
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 280

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
                anchors.centerIn: parent

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

        // One Preview Surface, 16 px, the transport dock: nothing sits between
        // them in any state. A finished recording is reached through its Saved
        // notification and Edit History, not through a row on this page.
        RecordTransportDock {
            recordViewModel: root.recordViewModel
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

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property RecordViewModelAdapter recordViewModel
    required property RecordPreviewAdapter previewAdapter
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

    // One rhythm for the page: the shared page inset (24) to every window edge,
    // one scale step less (16) between the two bands. It used to be 12 between
    // the bands with a 12 px top inset against 24 everywhere else, so the strip
    // sat closer to the title bar than the transport did to the bottom edge and
    // the page did not share the inset every other page uses.
    ColumnLayout {
        spacing: ExoTheme.spacingLg
        anchors {
            fill: parent
            topMargin: ExoTheme.pagePadding
            rightMargin: ExoTheme.pagePadding
            bottomMargin: ExoTheme.pagePadding
            leftMargin: ExoTheme.pagePadding
        }

        ExoNotice {
            text: root.recordViewModel.noticeText
            // The banner used to render every notice in the default warning
            // tone, so a saved recording arrived in caution amber. The tone now
            // comes from the same place the sentence does.
            tone: root.recordViewModel.noticeTone
            dismissible: true
            visible: text.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            onDismissed: root.recordViewModel.clearNotice()
        }

        // ── Preview Surface ──────────────────────────────────────────────────
        //
        // One surface, two parts: a compact toolbar that says what is being
        // captured and what it will be written as, and the live frame below it.
        // They used to be two: a full-width context card across the top of the
        // page and, under it, the preview. Two rounded rectangles a scale step
        // apart said the same thing twice — the card named the source, the
        // preview showed it — and the card's own height plus the gap to the
        // preview cost the page's subject about 70 px of stage for a row of text
        // that never changes while recording.
        //
        // The toolbar is chrome ON the stage, not a second page header: it
        // recedes (secondary text rung, muted ink, one hairline divider) and it
        // shares the stage's border, radius and width, so the preview reads as
        // the page's one object.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 280

            Rectangle {
                id: previewSurface

                // 38 px: two rungs under the shell's 40 px title band, so the
                // two never read as a pair of title bars, and tall enough for a
                // 30 px compact button with air either side.
                readonly property int toolbarHeight: 38

                readonly property real sourceAspect: {
                    const sourceWidth = root.previewAdapter.sourceSize.width
                                        * root.recordViewModel.normalizedSourceRect.width
                    const sourceHeight = root.previewAdapter.sourceSize.height
                                         * root.recordViewModel.normalizedSourceRect.height
                    return sourceHeight > 0 ? sourceWidth / sourceHeight : 16 / 9
                }
                // Idle is the state the page spends most of its life in, and it
                // is the one that does NOT need to shout: a hairline on the
                // container rung (`line`, what every card uses) rather than the
                // control rung. The four states that mean something keep their
                // full-strength tone, so the ring reads as a signal instead of
                // as permanent chrome.
                readonly property color stateColor: root.recordViewModel.stateTone === "recording" ? ExoTheme.error
                                                    : root.recordViewModel.stateTone === "warning" ? ExoTheme.warning
                                                    : root.recordViewModel.stateTone === "error" ? ExoTheme.error
                                                    : root.recordViewModel.stateTone === "success" ? ExoTheme.success
                                                    : ExoTheme.line

                // The video is still fitted to the source's aspect ratio and the
                // toolbar rides on top of it, so the edge of the frame is still
                // the edge of the recording — there is no letterboxing inside
                // the stage, only chrome above it.
                readonly property real stageHeight: Math.max(0, parent.height - previewSurface.toolbarHeight)
                readonly property real frameWidth: Math.min(parent.width,
                                                            previewSurface.stageHeight * previewSurface.sourceAspect)

                width: previewSurface.frameWidth
                height: previewSurface.frameWidth / previewSurface.sourceAspect + previewSurface.toolbarHeight
                // The surface IS the stage: black under the frame, with the
                // toolbar painting its own ground over the top of it. One
                // rectangle rather than two means the rounded bottom corners
                // are the frame's own, so nothing has to be clipped to them.
                color: "#08080A"
                border.width: 1
                border.color: previewSurface.stateColor
                // The largest surface in the product sits on the largest radius
                // in the scale, the same rung cards use. At radiusMd it read as
                // a scaled-up control rather than as the page's stage.
                radius: ExoTheme.radiusLg
                anchors.centerIn: parent

                // ── Preview toolbar ──────────────────────────────────────────
                Item {
                    id: previewToolbar

                    height: previewSurface.toolbarHeight
                    anchors {
                        top: parent.top
                        right: parent.right
                        left: parent.left
                        margins: 1
                    }

                    // Two rectangles for one ground: the rounded one supplies
                    // the surface's top corners, the square one fills back down
                    // to the divider. A single rounded rectangle would round its
                    // bottom corners away from the divider and show the black
                    // stage through them.
                    Rectangle {
                        color: ExoTheme.surface
                        radius: ExoTheme.radiusLg
                        anchors.fill: parent
                    }

                    Rectangle {
                        height: ExoTheme.radiusLg
                        color: ExoTheme.surface
                        anchors {
                            right: parent.right
                            bottom: parent.bottom
                            left: parent.left
                        }
                    }

                    RowLayout {
                        spacing: ExoTheme.spacingSm
                        anchors {
                            fill: parent
                            rightMargin: ExoTheme.spacingSm
                            leftMargin: ExoTheme.spacingMd
                        }

                        ExoGlyph {
                            kind: root.sourceGlyph
                            color: ExoTheme.textMuted
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth: 16
                            implicitHeight: 16
                        }

                        // On the body rung, not the section-title rung it used
                        // to sit on in the context card. This is chrome now, and
                        // the thing it labels — the frame right below it — is
                        // what the user actually reads.
                        Label {
                            text: root.recordViewModel.sourceName
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: ExoTheme.text
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            Accessible.name: qsTr("%1: %2").arg(root.recordViewModel.sourceKindText)
                                                           .arg(root.recordViewModel.sourceName)
                            font {
                                family: ExoTheme.sansFamily
                                pixelSize: ExoTheme.fontBody
                                weight: Font.DemiBold
                            }
                        }

                        ExoBadge {
                            text: qsTr("LOCKED")
                            tone: "notice"
                            visible: !root.recordViewModel.canSelectSource
                            Layout.alignment: Qt.AlignVCenter
                        }

                        // One understated run, not a badge per property. At the
                        // 860 px minimum window it is the first thing to give
                        // up room, because the source identity and the way back
                        // to the picker are what the toolbar exists for.
                        Label {
                            text: root.recordViewModel.formatText
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignRight
                            // `textMuted`, not `textDim`. This is live secondary
                            // metadata a user reads, so it belongs on a text rung
                            // — `textDim` is the disabled/decorative rung and
                            // lands at roughly 3:1 on this surface, under the
                            // 4.5:1 the contrast gate holds text to.
                            color: ExoTheme.textMuted
                            Layout.maximumWidth: 320
                            Layout.minimumWidth: 0
                            Layout.alignment: Qt.AlignVCenter
                            font {
                                family: ExoTheme.monoFamily
                                pixelSize: ExoTheme.fontCaption
                            }
                        }

                        ExoButton {
                            id: changeSourceButton

                            text: qsTr("Change source")
                            // Quiet and compact: the way back to the picker must
                            // stay obvious without competing with the Record
                            // pill at the other end of the page.
                            quiet: true
                            compact: true
                            enabled: root.recordViewModel.canSelectSource
                            Layout.alignment: Qt.AlignVCenter
                            onClicked: sourcePicker.open()
                        }
                    }

                    // The one internal division. A second border or a filled
                    // toolbar band would turn the surface back into two cards.
                    Rectangle {
                        height: 1
                        color: ExoTheme.line
                        anchors {
                            right: parent.right
                            bottom: parent.bottom
                            left: parent.left
                        }
                    }
                }

                // ── The frame ────────────────────────────────────────────────
                //
                // No fill of its own: the surface behind it already is the black
                // stage. This is the coordinate space the video and everything
                // drawn over it share, and it is what the webcam overlay measures
                // its normalized rect against.
                Item {
                    id: previewStage

                    anchors {
                        top: previewToolbar.bottom
                        right: parent.right
                        bottom: parent.bottom
                        left: parent.left
                        rightMargin: 1
                        bottomMargin: 1
                        leftMargin: 1
                    }

                    ExoPreviewItem {
                        id: previewItem

                        objectName: "quickPreviewItem"
                        previewAdapter: root.previewAdapter
                        normalizedSourceRect: root.recordViewModel.normalizedSourceRect
                        cornerRadius: ExoTheme.radiusLg
                        // Square where it meets the divider, rounded where it meets
                        // the surface's own bottom corners.
                        topCornerRadius: 0
                        anchors.fill: parent
                    }

                    FocusScope {
                        id: webcamOverlay

                        property rect draftRect: Qt.rect(0, 0, 1, 1)
                        readonly property bool idlePreview: !root.recordViewModel.recording
                                                            && !root.recordViewModel.paused
                                                            && !root.recordViewModel.preparing
                                                            && !root.recordViewModel.finalizing

                        x: parent.width * draftRect.x
                        y: parent.height * draftRect.y
                        width: parent.width * draftRect.width
                        height: parent.height * draftRect.height
                        visible: root.active && root.recordViewModel.webcamEnabled
                                 && (!idlePreview || root.recordViewModel.webcamFrameSource.length > 0)
                        activeFocusOnTab: root.recordViewModel.webcamOverlayEditable
                        Accessible.name: qsTr("Webcam overlay")
                        Accessible.description: qsTr("Drag to move. Use arrow keys to move and Shift plus arrow keys to resize.")
                        Keys.onPressed: event => {
                            if (!root.recordViewModel.webcamOverlayEditable)
                                return
                            const step = Boolean(event.modifiers & Qt.ControlModifier) ? 0.01 : 0.02
                            const resize = Boolean(event.modifiers & Qt.ShiftModifier)
                            let x = webcamOverlay.draftRect.x
                            let y = webcamOverlay.draftRect.y
                            let w = webcamOverlay.draftRect.width
                            let h = webcamOverlay.draftRect.height
                            if (event.key === Qt.Key_Left)
                                resize ? w = Math.max(0.05, w - step) : x = Math.max(0, x - step)
                            else if (event.key === Qt.Key_Right)
                                resize ? w = Math.min(1 - x, w + step) : x = Math.min(1 - w, x + step)
                            else if (event.key === Qt.Key_Up)
                                resize ? h = Math.max(0.05, h - step) : y = Math.max(0, y - step)
                            else if (event.key === Qt.Key_Down)
                                resize ? h = Math.min(1 - y, h + step) : y = Math.min(1 - h, y + step)
                            else
                                return
                            const nextRect = Qt.rect(x, y, w, h)
                            root.recordViewModel.requestWebcamOverlayRect(nextRect)
                            event.accepted = true
                        }

                        Rectangle {
                            color: ExoTheme.surface
                            radius: ExoTheme.radiusSm
                            visible: webcamOverlay.idlePreview
                            anchors.fill: parent
                        }

                        Binding {
                            target: webcamOverlay
                            property: "draftRect"
                            value: root.recordViewModel.webcamOverlayRect
                            when: !webcamDrag.pressed && !webcamResize.pressed
                            restoreMode: Binding.RestoreNone
                        }

                        Image {
                            id: webcamImage

                            anchors.fill: parent
                            source: root.active && webcamOverlay.idlePreview
                                    ? root.recordViewModel.webcamFrameSource : ""
                            sourceSize: Qt.size(320, 180)
                            cache: false
                            asynchronous: true
                            fillMode: Image.PreserveAspectCrop
                            mirror: root.recordViewModel.webcamMirror
                            opacity: root.recordViewModel.webcamOpacity
                            visible: webcamOverlay.idlePreview
                        }

                        Binding {
                            target: webcamImage
                            property: "sourceSize"
                            value: Qt.size(Math.max(1, Math.ceil(webcamImage.width / 32) * 32),
                                           Math.max(1, Math.ceil(webcamImage.height / 32) * 32))
                            when: !webcamResize.pressed
                            restoreMode: Binding.RestoreNone
                        }

                        Label {
                            text: qsTr("Webcam preview unavailable")
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            color: ExoTheme.warning
                            visible: webcamImage.status === Image.Error
                            anchors.fill: parent
                            font.family: ExoTheme.sansFamily
                            font.pixelSize: ExoTheme.fontCaption
                        }

                        Rectangle {
                            color: Qt.rgba(0, 0, 0, 0)
                            border.width: 1
                            border.color: root.recordViewModel.webcamError ? ExoTheme.error : ExoTheme.lineStrong
                            radius: ExoTheme.radiusSm
                            visible: root.recordViewModel.webcamError || webcamOverlay.activeFocus
                                     || webcamDrag.containsMouse || webcamResize.containsMouse
                            anchors.fill: parent
                        }

                        MouseArea {
                            id: webcamDrag

                            property point pressPoint
                            property rect pressRect

                            anchors.fill: parent
                            enabled: root.recordViewModel.webcamOverlayEditable
                            hoverEnabled: true
                            cursorShape: enabled ? Qt.SizeAllCursor : Qt.ArrowCursor
                            onPressed: mouse => {
                                webcamOverlay.forceActiveFocus()
                                pressPoint = Qt.point(mouse.x, mouse.y)
                                pressRect = webcamOverlay.draftRect
                            }
                            onPositionChanged: mouse => {
                                if (!pressed)
                                    return
                                const dx = (mouse.x - pressPoint.x) / Math.max(1, previewStage.width)
                                const dy = (mouse.y - pressPoint.y) / Math.max(1, previewStage.height)
                                webcamOverlay.draftRect = Qt.rect(
                                            Math.max(0, Math.min(1 - pressRect.width, pressRect.x + dx)),
                                            Math.max(0, Math.min(1 - pressRect.height, pressRect.y + dy)),
                                            pressRect.width, pressRect.height)
                            }
                            onReleased: root.recordViewModel.requestWebcamOverlayRect(webcamOverlay.draftRect)
                        }

                        Rectangle {
                            width: 14
                            height: 14
                            color: ExoTheme.accent
                            radius: 3
                            visible: root.recordViewModel.webcamOverlayEditable
                                     && (webcamDrag.containsMouse || webcamResize.containsMouse || webcamResize.pressed)
                            anchors {
                                right: parent.right
                                bottom: parent.bottom
                                margins: 3
                            }

                            MouseArea {
                                id: webcamResize

                                property point pressPoint
                                property rect pressRect

                                anchors.fill: parent
                                enabled: root.recordViewModel.webcamOverlayEditable
                                hoverEnabled: true
                                cursorShape: Qt.SizeFDiagCursor
                                onPressed: mouse => {
                                    mouse.accepted = true
                                    pressPoint = Qt.point(mouse.x, mouse.y)
                                    pressRect = webcamOverlay.draftRect
                                }
                                onPositionChanged: mouse => {
                                    if (!pressed)
                                        return
                                    const dw = (mouse.x - pressPoint.x) / Math.max(1, previewStage.width)
                                    const dh = (mouse.y - pressPoint.y) / Math.max(1, previewStage.height)
                                    const delta = Math.max(dw, dh)
                                    const maxDelta = Math.min(1 - pressRect.x - pressRect.width,
                                                              1 - pressRect.y - pressRect.height)
                                    const minDelta = 0.05 - Math.min(pressRect.width, pressRect.height)
                                    const clamped = Math.max(minDelta, Math.min(maxDelta, delta))
                                    webcamOverlay.draftRect = Qt.rect(pressRect.x, pressRect.y,
                                                                      pressRect.width + clamped,
                                                                      pressRect.height + clamped)
                                }
                                onReleased: mouse => {
                                    mouse.accepted = true
                                    root.recordViewModel.requestWebcamOverlayRect(webcamOverlay.draftRect)
                                }
                            }
                        }
                    }

                    ExoStatusPill {
                        id: statusChip

                        text: root.recordViewModel.stateText
                        tone: root.recordViewModel.stateTone
                        onSurface: true
                        width: Math.min(implicitWidth, Math.max(0, parent.width - 2 * ExoTheme.spacingLg))
                        anchors {
                            top: parent.top
                            left: parent.left
                            margins: ExoTheme.spacingLg
                        }
                    }

                    // The source name used to be repeated here, over the preview. The
                    // strip directly above states it already, and a second copy cost
                    // preview area to say the same thing twice.

                    // The live readout, on the SAME ground the status pill in the
                    // opposite corner already uses. It used to be four bare outlined
                    // strings sitting straight on the video: legible, but it read as
                    // debug text burned into the frame rather than as a readout the
                    // product puts there, and it was the one thing on the preview
                    // that did not share the pill's language.
                    Rectangle {
                        id: liveMetrics

                        readonly property real maxWidth: Math.max(0, parent.width - 2 * ExoTheme.spacingLg)

                        width: Math.min(metricsFlow.childrenRect.width + 2 * ExoTheme.spacingMd, liveMetrics.maxWidth)
                        height: metricsFlow.childrenRect.height + 2 * ExoTheme.spacingSm
                        color: Qt.rgba(0, 0, 0, 0.72)
                        radius: ExoTheme.radiusSm
                        visible: root.recordViewModel.recording || root.recordViewModel.paused
                        anchors {
                            left: parent.left
                            bottom: parent.bottom
                            margins: ExoTheme.spacingLg
                        }

                        Flow {
                            id: metricsFlow

                            // Measured off the preview rather than off the ground
                            // above, which is what keeps the two from feeding into
                            // each other.
                            width: Math.max(0, liveMetrics.maxWidth - 2 * ExoTheme.spacingMd)
                            spacing: ExoTheme.spacingMd
                            anchors {
                                top: parent.top
                                left: parent.left
                                topMargin: ExoTheme.spacingSm
                                leftMargin: ExoTheme.spacingMd
                            }

                            Label {
                                text: qsTr("BITRATE %1").arg(root.recordViewModel.bitrateText)
                                textFormat: Text.PlainText
                                color: ExoTheme.text
                                font.family: ExoTheme.monoFamily
                                font.pixelSize: ExoTheme.fontEyebrow
                            }
                            Label {
                                text: qsTr("DROP %1").arg(root.recordViewModel.droppedFramesText)
                                textFormat: Text.PlainText
                                color: ExoTheme.text
                                font.family: ExoTheme.monoFamily
                                font.pixelSize: ExoTheme.fontEyebrow
                            }
                            Label {
                                text: qsTr("DRIFT %1").arg(root.recordViewModel.driftText)
                                textFormat: Text.PlainText
                                color: ExoTheme.text
                                font.family: ExoTheme.monoFamily
                                font.pixelSize: ExoTheme.fontEyebrow
                            }

                            Label {
                                text: qsTr("SIZE %1").arg(root.recordViewModel.outputSizeText)
                                textFormat: Text.PlainText
                                color: ExoTheme.text
                                font.family: ExoTheme.monoFamily
                                font.pixelSize: ExoTheme.fontEyebrow
                            }
                        }
                    }

                    PreviewMetricsOverlay {
                        expanded: root.showMetricsOverlay
                        frameReady: root.previewAdapter.frameReady
                        presentationRate: root.previewAdapter.presentationRate
                        sourceDeliveryRate: root.previewAdapter.sourceDeliveryRate
                        frameTimeP95Ms: root.previewAdapter.frameTimeP95Ms
                        frameTimeP99Ms: root.previewAdapter.frameTimeP99Ms
                        accentColor: ExoTheme.accent
                        surfaceColor: "#E6151517"
                        textColor: ExoTheme.text
                        secondaryTextColor: ExoTheme.textSecondary
                        sansFamily: ExoTheme.sansFamily
                        monoFamily: ExoTheme.monoFamily
                        visible: root.benchmarkInteractionActive
                        onToggled: expanded => root.showMetricsOverlay = expanded
                        anchors {
                            fill: parent
                            topMargin: ExoTheme.spacingLg
                            rightMargin: ExoTheme.spacingLg
                            bottomMargin: ExoTheme.spacingLg
                            leftMargin: ExoTheme.spacingLg
                        }
                    }

                    RegionSelectionOverlay {
                        recordViewModel: root.recordViewModel
                        visible: root.recordViewModel.regionSelectionNeeded
                        anchors.fill: parent
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
                        color: ExoTheme.warning
                        visible: text.length > 0 && !root.recordViewModel.regionSelectionNeeded
                        width: Math.max(0, Math.min(440, parent.width - 48))
                        anchors.centerIn: parent
                        font.family: ExoTheme.sansFamily
                        font.pixelSize: ExoTheme.fontBody
                    }

                    Rectangle {
                        id: benchmarkActivity

                        width: 8
                        height: 8
                        color: ExoTheme.warning
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

        // The entry into the Edit surface (ADR 0022) lives IN the transport dock,
        // as that state's one recommended action — see RecordTransportDock. It
        // used to be a detached button in a row of its own between the Preview
        // Surface and the dock, which broke the page's composition (one Preview
        // Surface → 16 px → transport dock) and left the primary action of the
        // Completed state floating in the gap between the two.
        RecordTransportDock {
            recordViewModel: root.recordViewModel
            Layout.fillWidth: true
        }
    }

    RecordSourcePicker {
        id: sourcePicker
        recordViewModel: root.recordViewModel
    }
}

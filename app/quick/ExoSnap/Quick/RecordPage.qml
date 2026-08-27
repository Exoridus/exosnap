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

        // UNRESOLVED conditions only: a source that is gone, a region too small
        // to record, a settings write that failed. Confirmations belong in a
        // toast -- this is the page's fill-height column, so anything that
        // appears here pushes the Preview Surface down, and a message the user
        // cannot act on has no business doing that. Both the recording-saved and
        // the frame-saved confirmations used to be here.
        ExoNotice {
            text: root.recordViewModel.noticeText
            // Still bound rather than fixed to a warning tone: an error and a
            // caution do not read alike, and the tone comes from the same place
            // the sentence does.
            tone: root.recordViewModel.noticeTone
            dismissible: true
            visible: text.length > 0
            Layout.fillWidth: true
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
                // Structural, never semantic. This border used to take the
                // state's colour — red while recording, amber while paused or
                // counting down, green once a recording had saved — which made
                // the largest object on the page into a ~1000 px status light
                // repeating what the status pill in its own corner, the shell's
                // pill and the transport's one recommended action all already
                // said. A surface border says "this is one bounded surface"; it
                // is not a place to encode state.
                border.color: ExoTheme.line
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
                    //
                    // The radius is the surface's MINUS the 1 px inset this item
                    // sits at. Two concentric rounded rects are only concentric
                    // when their radii differ by the inset; drawn at the same
                    // radius the inner corner cuts across the outer arc and
                    // leaves a sliver of black stage inside the border -- read as
                    // a mismatched corner, and most visible in Light where the
                    // toolbar ground is near-white against it.
                    readonly property int cornerRadius: ExoTheme.radiusLg - 1

                    Rectangle {
                        color: ExoTheme.surface
                        radius: previewToolbar.cornerRadius
                        anchors.fill: parent
                    }

                    Rectangle {
                        height: previewToolbar.cornerRadius
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

                        // Neutral, not `notice`. The capture setup is locked for
                        // the duration of a recording BY DESIGN — changing the
                        // source mid-file is not a thing the product allows, so
                        // there is nothing here for the user to attend to and
                        // caution amber promised otherwise. It stays as a quiet
                        // statement of fact beside the disabled action it
                        // explains.
                        ExoBadge {
                            text: qsTr("LOCKED")
                            tone: "neutral"
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
                            text: qsTr("Change source")
                            // Compact, but NOT quiet. A quiet button carries no
                            // chrome at rest, and at the end of a row that is
                            // otherwise "Display 1 · 60 CFR · AV1 · Opus · MKV"
                            // it read as one more piece of metadata: the only
                            // thing on the toolbar the user can press looked
                            // exactly like the four things they cannot. One
                            // control boundary on the raised control fill is
                            // enough to say "press me" — the compact rung keeps
                            // it well below the Record pill at the other end of
                            // the page.
                            compact: true
                            enabled: root.recordViewModel.canSelectSource
                            Layout.alignment: Qt.AlignVCenter
                            onClicked: root.openSourcePicker()
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
                            // sourceSize is not set here: the Binding below owns it
                            // and is active from construction, so a literal would be
                            // overwritten before the first frame and read as a
                            // decode resolution that nothing honours.
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
                            // The theme rung, not the overlay one: this label
                            // only ever appears in the idle preview, where the
                            // panel behind it is `ExoTheme.surface`.
                            color: ExoTheme.warningText
                            visible: webcamImage.status === Image.Error
                            anchors.fill: parent
                            font.family: ExoTheme.sansFamily
                            font.pixelSize: ExoTheme.fontCaption
                        }

                        Rectangle {
                            id: webcamFrameChrome

                            color: Qt.rgba(0, 0, 0, 0)
                            border.width: 1
                            border.color: root.recordViewModel.webcamError ? ExoTheme.error : ExoTheme.lineStrong
                            radius: ExoTheme.radiusSm
                            // `pressed` as well as `containsMouse`: a drag that
                            // leaves the frame's own bounds is still a drag, and
                            // chrome that blinks out mid-gesture reads as a dropped
                            // grab.
                            visible: root.recordViewModel.webcamError || webcamOverlay.activeFocus
                                     || webcamDrag.containsMouse || webcamResize.containsMouse
                                     || webcamDrag.pressed || webcamResize.pressed
                            anchors.fill: parent
                        }

                        // The four corners of the editable frame. What was here was a
                        // single filled square at the bottom right, which reads as a
                        // stray box rather than as a frame with corners; brackets
                        // state all four without covering the picture.
                        Repeater {
                            model: 4

                            delegate: Item {
                                id: webcamCorner

                                required property int index

                                readonly property bool atRight: webcamCorner.index === 1 || webcamCorner.index === 2
                                readonly property bool atBottom: webcamCorner.index >= 2
                                // Bottom right is the resize grip, so it is the one
                                // corner that answers the pointer.
                                readonly property bool grip: webcamCorner.index === 2
                                readonly property color ink:
                                    (webcamCorner.grip && (webcamResize.containsMouse || webcamResize.pressed))
                                    ? ExoTheme.accent
                                    : (root.recordViewModel.webcamError ? ExoTheme.error : ExoTheme.lineStrong)

                                width: 14
                                height: 14
                                visible: webcamFrameChrome.visible
                                x: webcamCorner.atRight ? parent.width - width : 0
                                y: webcamCorner.atBottom ? parent.height - height : 0

                                Rectangle {
                                    width: parent.width
                                    height: 2
                                    color: webcamCorner.ink
                                    y: webcamCorner.atBottom ? parent.height - height : 0
                                }

                                Rectangle {
                                    width: 2
                                    height: parent.height
                                    color: webcamCorner.ink
                                    x: webcamCorner.atRight ? parent.width - width : 0
                                }
                            }
                        }

                        MouseArea {
                            id: webcamDrag

                            property point pressPoint
                            property rect pressRect

                            anchors.fill: parent
                            enabled: root.recordViewModel.webcamOverlayEditable
                            hoverEnabled: true
                            cursorShape: enabled ? Qt.SizeAllCursor : Qt.ArrowCursor
                            // Both points live in previewStage coordinates, never in
                            // this MouseArea's: the overlay MOVES as draftRect
                            // changes, so a delta measured against the item itself
                            // shrinks by exactly the amount already applied. The drag
                            // then crawls behind the pointer and stalls once it
                            // catches up, without ever losing the grab -- which is
                            // what makes it look like lag rather than like a bug.
                            onPressed: mouse => {
                                webcamOverlay.forceActiveFocus()
                                pressPoint = webcamDrag.mapToItem(previewStage, mouse.x, mouse.y)
                                pressRect = webcamOverlay.draftRect
                            }
                            onPositionChanged: mouse => {
                                if (!pressed)
                                    return
                                const at = webcamDrag.mapToItem(previewStage, mouse.x, mouse.y)
                                const dx = (at.x - pressPoint.x) / Math.max(1, previewStage.width)
                                const dy = (at.y - pressPoint.y) / Math.max(1, previewStage.height)
                                webcamOverlay.draftRect = Qt.rect(
                                            Math.max(0, Math.min(1 - pressRect.width, pressRect.x + dx)),
                                            Math.max(0, Math.min(1 - pressRect.height, pressRect.y + dy)),
                                            pressRect.width, pressRect.height)
                            }
                            onReleased: root.recordViewModel.requestWebcamOverlayRect(webcamOverlay.draftRect)
                        }

                        Item {
                            // Hit area only. The bottom-right bracket above is what
                            // the eye reads, and it takes the accent while this is
                            // hovered or held.
                            width: 14
                            height: 14
                            anchors {
                                right: parent.right
                                bottom: parent.bottom
                            }

                            MouseArea {
                                id: webcamResize

                                property point pressPoint
                                property rect pressRect

                                anchors.fill: parent
                                enabled: root.recordViewModel.webcamOverlayEditable
                                hoverEnabled: true
                                cursorShape: Qt.SizeFDiagCursor
                                // previewStage coordinates for the same reason the
                                // drag uses them: the grip is anchored to a frame
                                // that grows under it.
                                onPressed: mouse => {
                                    mouse.accepted = true
                                    pressPoint = webcamResize.mapToItem(previewStage, mouse.x, mouse.y)
                                    pressRect = webcamOverlay.draftRect
                                }
                                onPositionChanged: mouse => {
                                    if (!pressed)
                                        return
                                    const at = webcamResize.mapToItem(previewStage, mouse.x, mouse.y)
                                    const dw = (at.x - pressPoint.x) / Math.max(1, previewStage.width)
                                    const dh = (at.y - pressPoint.y) / Math.max(1, previewStage.height)
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
                        // Near-black in BOTH appearances, like the status pill
                        // opposite: what is behind it is the captured frame. So
                        // the labels below take the `overlay*` ink rungs —
                        // `ExoTheme.text` measured 1.20:1 here in Light.
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
                                color: ExoTheme.overlayInk
                                font.family: ExoTheme.monoFamily
                                font.pixelSize: ExoTheme.fontEyebrow
                            }
                            Label {
                                text: qsTr("DROP %1").arg(root.recordViewModel.droppedFramesText)
                                textFormat: Text.PlainText
                                color: ExoTheme.overlayInk
                                font.family: ExoTheme.monoFamily
                                font.pixelSize: ExoTheme.fontEyebrow
                            }
                            Label {
                                text: qsTr("DRIFT %1").arg(root.recordViewModel.driftText)
                                textFormat: Text.PlainText
                                color: ExoTheme.overlayInk
                                font.family: ExoTheme.monoFamily
                                font.pixelSize: ExoTheme.fontEyebrow
                            }

                            Label {
                                text: qsTr("SIZE %1").arg(root.recordViewModel.outputSizeText)
                                textFormat: Text.PlainText
                                color: ExoTheme.overlayInk
                                font.family: ExoTheme.monoFamily
                                font.pixelSize: ExoTheme.fontEyebrow
                            }
                        }
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
                        // Centred on the black stage, so the fixed-dark rung.
                        color: ExoTheme.overlayWarning
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

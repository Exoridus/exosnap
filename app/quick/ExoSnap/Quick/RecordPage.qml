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

    // One rhythm for the page: the shared page inset (24) to every window edge,
    // one scale step less (16) between the three bands. It used to be 12 between
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

        // Context band. Record is the one page with no page title, so this bar IS
        // the page's identity: what is being captured, whether that choice is
        // locked, and what it will be written as.
        //
        // It used to be a bare row of text across the bare page. At 1600 px that
        // left the source name and the format summary a thousand pixels apart
        // with nothing saying they belonged to one statement, and its 9/10 px
        // mono read as debug output beside 14 px body text everywhere else. A
        // quiet surface — contrast only, no border, per the same rule ExoCard
        // follows — ties the two ends into one band without making it a second
        // toolbar.
        Rectangle {
            id: contextBar

            implicitHeight: ExoTheme.controlHeight + 2 * ExoTheme.spacingSm
            color: ExoTheme.surface
            radius: ExoTheme.radiusMd
            Layout.fillWidth: true

            RowLayout {
                spacing: ExoTheme.spacingSm
                anchors {
                    fill: parent
                    topMargin: ExoTheme.spacingSm
                    rightMargin: ExoTheme.spacingSm
                    bottomMargin: ExoTheme.spacingSm
                    leftMargin: ExoTheme.spacingLg
                }

                Label {
                    text: root.recordViewModel.sourceKindText
                    textFormat: Text.PlainText
                    color: ExoTheme.textDim
                    Layout.alignment: Qt.AlignVCenter
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: ExoTheme.fontCaption
                        letterSpacing: 1
                        weight: Font.DemiBold
                    }
                }

                // On the section-title rung, not the body rung: with no page
                // title above it this is the largest piece of text on the page
                // outside the timer, and it is what the user checks before
                // pressing Record.
                Label {
                    text: root.recordViewModel.sourceName
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: ExoTheme.text
                    Layout.alignment: Qt.AlignVCenter
                    font {
                        family: ExoTheme.sansFamily
                        pixelSize: ExoTheme.fontSectionTitle
                        weight: Font.DemiBold
                    }
                }

                ExoBadge {
                    text: qsTr("LOCKED")
                    tone: "notice"
                    visible: !root.recordViewModel.canSelectSource
                    Layout.alignment: Qt.AlignVCenter
                }

                Item {
                    Layout.fillWidth: true
                }

                Label {
                    text: root.recordViewModel.formatText
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                    color: ExoTheme.textMuted
                    Layout.maximumWidth: 360
                    Layout.alignment: Qt.AlignVCenter
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: ExoTheme.fontCaption
                    }
                }

                ExoButton {
                    id: changeSourceButton

                    text: qsTr("Change source")
                    enabled: root.recordViewModel.canSelectSource
                    Layout.leftMargin: ExoTheme.spacingMd - ExoTheme.spacingSm
                    onClicked: sourcePicker.open()
                }
            }
        }

        ExoNotice {
            text: root.recordViewModel.noticeText
            dismissible: true
            visible: text.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            onDismissed: root.recordViewModel.clearNotice()
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 280

            Rectangle {
                id: fittedPreview

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

                width: Math.min(parent.width, parent.height * fittedPreview.sourceAspect)
                height: width / fittedPreview.sourceAspect
                color: "#08080A"
                border.width: 1
                border.color: fittedPreview.stateColor
                // The largest surface in the product sits on the largest radius
                // in the scale, the same rung cards use. At radiusMd it read as
                // a scaled-up control rather than as the page's stage.
                radius: ExoTheme.radiusLg
                anchors.centerIn: parent

                ExoPreviewItem {
                    id: previewItem

                    objectName: "quickPreviewItem"
                    previewAdapter: root.previewAdapter
                    normalizedSourceRect: root.recordViewModel.normalizedSourceRect
                    cornerRadius: ExoTheme.radiusLg
                    anchors {
                        fill: parent
                        margins: 1
                    }
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
                            const dx = (mouse.x - pressPoint.x) / Math.max(1, fittedPreview.width)
                            const dy = (mouse.y - pressPoint.y) / Math.max(1, fittedPreview.height)
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
                                const dw = (mouse.x - pressPoint.x) / Math.max(1, fittedPreview.width)
                                const dh = (mouse.y - pressPoint.y) / Math.max(1, fittedPreview.height)
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

        // Production entry into the Edit surface (ADR 0022). Hidden rather than
        // disabled when the recording cannot be edited at all (split recording,
        // missing file, failed run) — a permanently dead button next to a
        // successful result reads as a defect.
        //
        // The row's OWN visibility is the Edit button's, so an invisible row
        // contributes neither height nor column spacing. It used to also carry
        // `recordViewModel.resultText`, which appeared and disappeared with the
        // engine state and re-scaled the live preview underneath it every time —
        // a failed run shrank the preview to make room for a sentence saying it
        // had failed. What went wrong is stated inside the preview and, when it
        // is serious, on the recording-error surface; neither needed this line.
        RowLayout {
            spacing: ExoTheme.spacingSm
            visible: root.recordViewModel.canOpenEditor
            Layout.fillWidth: true

            Item {
                Layout.fillWidth: true
            }

            ExoButton {
                text: qsTr("Edit")
                tone: "primary"
                onClicked: root.recordViewModel.requestOpenEditor()
            }
        }

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

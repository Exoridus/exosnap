import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ExoSnap.Updater

// The standalone updater window, rendered as a pure function of the existing
// UpdaterController state through UpdaterViewAdapter. Every decision this window
// displays -- the eyebrow wording, the step tags, the state panel copy, which
// actions exist, whether close is blocked or asks for a confirmation -- was made
// before this document runs. QML lays the projection out and routes button
// presses back through the adapter's command signals.
//
// Structure and geometry keep the Widgets window's recognisable composition:
// a 56 px title bar with the brand and the stable "Updater" role label, the
// version transition, the ring, the status line, the five fixed phases, one
// fixed 110 px state panel and one fixed 36 px action row.
Window {
    id: window

    required property UpdaterViewAdapter adapter

    // The window's role label is the same word in EVERY state. What the run IS
    // gets said by the content eyebrow; a title bar that renames itself under
    // the user is the instability this window exists to avoid.
    title: qsTr("ExoSnap Updater")
    flags: Qt.Window | Qt.FramelessWindowHint
    color: ExoTheme.background
    // Reference geometry, and a fixed 520x680 like the Widgets window.
    width: 520
    height: 680
    minimumWidth: 520
    maximumWidth: 520
    minimumHeight: 680
    maximumHeight: 680
    // The updater process shows the window itself, after its placement policy
    // ran -- and the harness captures it explicitly.
    visible: false

    // The X, Alt+F4, the taskbar close and a native WM_CLOSE all arrive here.
    // The adapter applies the same policy the Widgets closeEvent applied:
    // blocked during Install/Verify/Launch, a confirmation while a run is
    // cancellable, plain close when nothing is in flight.
    onClosing: function (close) {
        close.accepted = window.adapter.requestClose();
    }

    readonly property color panelToneColor: {
        switch (window.adapter.panelTone) {
        case "success":
            return ExoTheme.success;
        case "warning":
            return ExoTheme.warning;
        case "error":
            return ExoTheme.error;
        case "neutral":
            return ExoTheme.textMuted;
        default:
            return ExoTheme.accent;
        }
    }

    readonly property color ringToneColor: {
        switch (window.adapter.ringTone) {
        case "success":
            return ExoTheme.success;
        case "warning":
            return ExoTheme.warning;
        case "error":
            return ExoTheme.error;
        case "neutral":
            return ExoTheme.textMuted;
        default:
            return ExoTheme.accent;
        }
    }

    function toneSurface(color, alpha) {
        return Qt.tint(ExoTheme.surface, Qt.rgba(color.r, color.g, color.b, alpha));
    }

    function glyphKind(key) {
        switch (key) {
        case "check":
            return ExoGlyph.Check;
        case "cross":
            return ExoGlyph.Close;
        case "warning":
            return ExoGlyph.Warning;
        case "dot":
            return ExoGlyph.Dot;
        case "layers":
            return ExoGlyph.Layers;
        case "close":
            return ExoGlyph.Close;
        default:
            return ExoGlyph.Invalid;
        }
    }

    Rectangle {
        anchors.fill: parent
        color: ExoTheme.background
        border.width: 1
        border.color: ExoTheme.lineStrong

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ── Title bar ────────────────────────────────────────────────────
            Item {
                id: titleBar

                objectName: "updaterTitleBar"
                Layout.fillWidth: true
                Layout.preferredHeight: 56

                Rectangle {
                    anchors.fill: parent
                    color: ExoTheme.surface
                }

                Rectangle {
                    anchors {
                        left: parent.left
                        right: parent.right
                        bottom: parent.bottom
                    }
                    height: 1
                    color: ExoTheme.line
                }

                RowLayout {
                    anchors {
                        fill: parent
                        leftMargin: ExoTheme.spacingLg
                        rightMargin: 0
                    }
                    spacing: ExoTheme.spacingSm

                    // The wordmark as two runs, the same reading the Widgets
                    // header carried: the brand in ink and accent.
                    Item {
                        objectName: "updaterWordmark"

                        implicitWidth: wordmarkRow.implicitWidth
                        implicitHeight: wordmarkRow.implicitHeight
                        Layout.alignment: Qt.AlignVCenter

                        Row {
                            id: wordmarkRow

                            Label {
                                text: "exo"
                                textFormat: Text.PlainText
                                color: ExoTheme.text
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontSectionTitle
                                    weight: Font.DemiBold
                                }
                            }

                            Label {
                                text: "snap"
                                textFormat: Text.PlainText
                                color: ExoTheme.accent
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontSectionTitle
                                    weight: Font.DemiBold
                                }
                            }
                        }
                    }

                    Label {
                        objectName: "updaterRoleLabel"
                        text: "Updater"
                        textFormat: Text.PlainText
                        color: ExoTheme.textMuted
                        Layout.alignment: Qt.AlignVCenter
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontSecondary
                            weight: Font.Medium
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    AbstractButton {
                        id: minimizeButton

                        objectName: "updaterMinimizeButton"
                        implicitWidth: 46
                        implicitHeight: 56
                        hoverEnabled: true
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Minimize updater")
                        onClicked: window.showMinimized()

                        background: Rectangle {
                            color: minimizeButton.hovered ? ExoTheme.surfaceHover : "transparent"
                        }

                        contentItem: ExoGlyph {
                            anchors.centerIn: parent
                            kind: ExoGlyph.Minus
                            color: ExoTheme.textSecondary
                            width: 14
                            height: 14
                        }
                    }

                    AbstractButton {
                        id: closeButton

                        objectName: "updaterCloseButton"
                        implicitWidth: 46
                        implicitHeight: 56
                        hoverEnabled: true
                        activeFocusOnTab: true
                        enabled: window.adapter.closeEnabled
                        Accessible.role: Accessible.Button
                        Accessible.name: window.adapter.closeAccessibleName
                        onClicked: window.adapter.requestClose()

                        ToolTip.text: window.adapter.closeTooltip
                        ToolTip.visible: closeButton.hovered
                        ToolTip.delay: 400

                        background: Rectangle {
                            color: closeButton.hovered && closeButton.enabled ? ExoTheme.errorSurface : "transparent"
                        }

                        contentItem: ExoGlyph {
                            anchors.centerIn: parent
                            kind: ExoGlyph.Close
                            color: closeButton.enabled ? closeButton.hovered ? ExoTheme.error : ExoTheme.textSecondary
                                                       : ExoTheme.textDim
                            width: 14
                            height: 14
                        }
                    }
                }

                DragHandler {
                    target: null
                    acceptedButtons: Qt.LeftButton
                    onActiveChanged: {
                        if (active)
                            window.startSystemMove();
                    }
                }
            }

            // ── Content ──────────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: ExoTheme.spacingXl
                Layout.topMargin: 18
                Layout.bottomMargin: 18
                spacing: 0

                // Version transition
                Column {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 9

                    Label {
                        objectName: "updaterEyebrow"
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: window.adapter.eyebrow
                        textFormat: Text.PlainText
                        color: ExoTheme.textDim
                        font {
                            family: ExoTheme.monoFamily
                            pixelSize: ExoTheme.fontEyebrow
                            weight: Font.Medium
                            letterSpacing: 1
                        }
                    }

                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 8

                        Rectangle {
                            objectName: "updaterFromVersionPill"
                            implicitWidth: Math.min(fromLabel.implicitWidth + 2 * ExoTheme.spacingMd, 210)
                            implicitHeight: 30
                            radius: ExoTheme.radiusSm
                            color: ExoTheme.surfaceRaised
                            border.width: 1
                            border.color: window.adapter.failedTerminal ? ExoTheme.lineStrong : ExoTheme.line

                            Label {
                                id: fromLabel
                                anchors.centerIn: parent
                                width: Math.min(implicitWidth, parent.width - 2 * ExoTheme.spacingMd)
                                text: window.adapter.fromVersion
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: window.adapter.failedTerminal ? ExoTheme.text : ExoTheme.textMuted
                                font {
                                    family: ExoTheme.monoFamily
                                    pixelSize: ExoTheme.fontBody
                                    weight: window.adapter.failedTerminal ? Font.DemiBold : Font.Medium
                                }

                                ToolTip.text: window.adapter.fromVersion
                                ToolTip.visible: fromHover.hovered
                                ToolTip.delay: 400
                                Accessible.role: Accessible.StaticText
                                Accessible.name: window.adapter.fromVersion
                            }

                            HoverHandler {
                                id: fromHover
                            }
                        }

                        ExoGlyph {
                            id: transitionChevron

                            anchors.verticalCenter: parent.verticalCenter
                            visible: window.adapter.hasTarget
                            kind: ExoGlyph.ArrowRight
                            color: window.adapter.failedTerminal ? ExoTheme.textDim : ExoTheme.accent
                            width: 14
                            height: 14
                        }

                        Rectangle {
                            objectName: "updaterToVersionPill"
                            visible: window.adapter.hasTarget
                            implicitWidth: Math.min(toLabel.implicitWidth + 2 * ExoTheme.spacingMd, 210)
                            implicitHeight: 30
                            radius: ExoTheme.radiusSm
                            color: window.adapter.failedTerminal ? ExoTheme.surfaceRaised
                                                                 : toneSurface(ExoTheme.accent, 0.14)
                            border.width: 1
                            border.color: window.adapter.failedTerminal ? ExoTheme.line
                                                                        : Qt.tint(ExoTheme.accent, Qt.rgba(0, 0, 0, 0.4))

                            Label {
                                id: toLabel
                                anchors.centerIn: parent
                                width: Math.min(implicitWidth, parent.width - 2 * ExoTheme.spacingMd)
                                text: window.adapter.toVersion
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: window.adapter.failedTerminal ? ExoTheme.textDim : ExoTheme.text
                                font {
                                    family: ExoTheme.monoFamily
                                    pixelSize: ExoTheme.fontBody
                                    weight: window.adapter.failedTerminal ? Font.Medium : Font.DemiBold
                                }

                                ToolTip.text: window.adapter.toVersion
                                ToolTip.visible: toHover.hovered
                                ToolTip.delay: 400
                                Accessible.role: Accessible.StaticText
                                Accessible.name: window.adapter.toVersion
                            }

                            HoverHandler {
                                id: toHover
                            }
                        }
                    }
                }

                Item {
                    Layout.preferredHeight: 12
                }

                UpdaterRing {
                    objectName: "updaterRing"
                    Layout.alignment: Qt.AlignHCenter
                    value: window.adapter.ring
                    indeterminate: window.adapter.indeterminate
                    tone: window.adapter.ringTone
                    glyph: window.adapter.ringGlyph
                    percent: window.adapter.ringPercent
                    description: window.adapter.ringDescription
                }

                Item {
                    Layout.preferredHeight: 8
                }

                // Status line
                Row {
                    objectName: "updaterStatusRow"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredHeight: 20
                    spacing: 8
                    visible: window.adapter.statusTextVisible

                    UpdaterGlyph {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: window.adapter.statusGlyph === "spinner"
                        glyph: "spinner"
                        color: window.ringToneColor
                        width: 16
                        height: 16
                    }

                    ExoGlyph {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: window.adapter.statusGlyph !== "" && window.adapter.statusGlyph !== "spinner"
                        kind: window.glyphKind(window.adapter.statusGlyph)
                        color: window.ringToneColor
                        width: 16
                        height: 16
                    }

                    Label {
                        id: statusLabel

                        objectName: "updaterStatusText"
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, 420)
                        text: window.adapter.statusHeadline !== "" ? window.adapter.statusHeadline
                                                                   : window.adapter.statusLine
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: ExoTheme.text
                        font {
                            family: window.adapter.statusHeadline !== "" ? ExoTheme.sansFamily : ExoTheme.monoFamily
                            pixelSize: window.adapter.statusHeadline !== "" ? ExoTheme.fontBody : ExoTheme.fontSecondary
                            weight: window.adapter.statusHeadline !== "" ? Font.DemiBold : Font.Medium
                        }

                        ToolTip.text: text
                        ToolTip.visible: statusHover.hovered && contentWidth > width
                        ToolTip.delay: 400
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }

                    HoverHandler {
                        id: statusHover
                    }
                }

                Item {
                    Layout.preferredHeight: 14
                }

                UpdaterStepList {
                    objectName: "updaterStepList"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 196
                    rows: window.adapter.stepRows
                    failTone: window.adapter.ringTone
                }

                Item {
                    Layout.preferredHeight: 14
                }

                // State / result panel. One geometry in every state.
                Rectangle {
                    objectName: window.adapter.panelKind === "result" ? "updaterResultCard" : "updaterWorkingPanel"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 110
                    radius: ExoTheme.radiusMd
                    color: window.toneSurface(window.panelToneColor, 0.13)
                    border.width: 1
                    border.color: window.toneSurface(window.panelToneColor, 0.44)

                    ColumnLayout {
                        anchors {
                            fill: parent
                            margins: 15
                            topMargin: 12
                            bottomMargin: 12
                        }
                        spacing: 7

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 9

                            ExoGlyph {
                                Layout.alignment: Qt.AlignVCenter
                                visible: window.adapter.panelGlyph !== "download" && window.adapter.panelGlyph !== "shield"
                                         && window.adapter.panelGlyph !== "spinner"
                                kind: window.glyphKind(window.adapter.panelGlyph)
                                color: window.panelToneColor
                                width: 16
                                height: 16
                            }

                            UpdaterGlyph {
                                Layout.alignment: Qt.AlignVCenter
                                visible: window.adapter.panelGlyph === "download" || window.adapter.panelGlyph === "shield"
                                         || window.adapter.panelGlyph === "spinner"
                                glyph: window.adapter.panelGlyph
                                color: window.panelToneColor
                                width: 16
                                height: 16
                            }

                            Label {
                                id: panelTitleLabel

                                objectName: window.adapter.panelKind === "result" ? "updaterResultHeadline" : "updaterWorkingTitle"
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                text: window.adapter.panelTitle
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: ExoTheme.text
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontSecondary
                                    weight: Font.DemiBold
                                }

                                ToolTip.text: text
                                ToolTip.visible: panelHover.hovered && contentWidth > width
                                ToolTip.delay: 400
                                Accessible.role: Accessible.StaticText
                                Accessible.name: text
                            }
                        }

                        Label {
                            id: panelDetailLabel

                            objectName: window.adapter.panelKind === "result" ? "updaterResultDetail" : "updaterWorkingDetail"
                            Layout.fillWidth: true
                            text: window.adapter.panelDetail
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: ExoTheme.textSecondary
                            font {
                                family: ExoTheme.sansFamily
                                pixelSize: ExoTheme.fontCaption
                            }

                            ToolTip.text: text
                            ToolTip.visible: panelHover.hovered && contentWidth > width
                            ToolTip.delay: 400
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            visible: window.adapter.panelSafety !== ""

                            UpdaterGlyph {
                                Layout.alignment: Qt.AlignVCenter
                                glyph: "shield"
                                color: ExoTheme.success
                                width: 14
                                height: 14
                            }

                            Label {
                                id: safetyLabel

                                objectName: window.adapter.panelKind === "result" ? "updaterSafetyText" : "updaterWorkingSafety"
                                Layout.fillWidth: true
                                text: window.adapter.panelSafety
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: ExoTheme.text
                                font {
                                    family: ExoTheme.sansFamily
                                    pixelSize: ExoTheme.fontCaption
                                    weight: Font.Medium
                                }

                                ToolTip.text: text
                                ToolTip.visible: panelHover.hovered && contentWidth > width
                                ToolTip.delay: 400
                                Accessible.role: Accessible.StaticText
                                Accessible.name: text
                            }
                        }

                        Item {
                            Layout.fillHeight: true
                        }
                    }

                    HoverHandler {
                        id: panelHover
                    }
                }

                Item {
                    Layout.preferredHeight: 10
                }

                // Action row. Fixed 36 px in every state.
                RowLayout {
                    objectName: "updaterActionRow"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    spacing: ExoTheme.spacingSm

                    Label {
                        objectName: "updaterActionHint"
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        text: window.adapter.hint
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: ExoTheme.textDim
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontCaption
                        }
                    }

                    ExoButton {
                        objectName: "updaterSecondaryAction"
                        visible: window.adapter.secondaryAction !== ""
                        text: window.adapter.secondaryAction
                        onClicked: window.adapter.activateAction(text)
                    }

                    ExoButton {
                        objectName: "updaterPrimaryAction"
                        visible: window.adapter.primaryAction !== ""
                        text: window.adapter.primaryAction
                        tone: "primary"
                        onClicked: window.adapter.activateAction(text)
                    }

                    ExoButton {
                        objectName: "updaterCloseAction"
                        visible: window.adapter.closeActionVisible
                        enabled: window.adapter.closeActionEnabled
                        text: window.adapter.closeActionLabel
                        onClicked: window.adapter.activateAction(text)
                    }
                }
            }
        }

        // In-window confirmation: cancellation stays visually inside the
        // updater instead of opening a second, differently styled native dialog.
        Rectangle {
            id: cancelOverlay

            objectName: "updaterCancelOverlay"
            anchors.fill: parent
            visible: window.adapter.cancelConfirmationVisible
            color: Qt.rgba(0, 0, 0, 0.88)

            onVisibleChanged: {
                if (visible)
                    keepUpdatingButton.forceActiveFocus();
            }

            Rectangle {
                id: cancelDialog

                objectName: "updaterCancelDialog"
                anchors.centerIn: parent
                width: 390
                implicitHeight: dialogColumn.implicitHeight + 36
                radius: ExoTheme.radiusLg
                color: ExoTheme.surfaceRaised
                border.width: 1
                border.color: ExoTheme.lineStrong

                ColumnLayout {
                    id: dialogColumn

                    anchors {
                        left: parent.left
                        right: parent.right
                        verticalCenter: parent.verticalCenter
                        margins: 20
                    }
                    spacing: 9

                    Label {
                        objectName: "updaterCancelTitle"
                        Layout.fillWidth: true
                        text: qsTr("Cancel this update?")
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: ExoTheme.text
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontSectionTitle
                            weight: Font.DemiBold
                        }
                    }

                    Label {
                        objectName: "updaterCancelBody"
                        Layout.fillWidth: true
                        text: qsTr("Download and preparation progress will be discarded. "
                                   + "Your installed ExoSnap version remains unchanged.")
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: ExoTheme.textSecondary
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontSecondary
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 9
                        spacing: ExoTheme.spacingSm

                        Item {
                            Layout.fillWidth: true
                        }

                        ExoButton {
                            objectName: "updaterConfirmCancelButton"
                            text: qsTr("Cancel update")
                            tone: "destructive"
                            onClicked: window.adapter.confirmCancelAndClose()
                        }

                        ExoButton {
                            id: keepUpdatingButton

                            objectName: "updaterKeepUpdatingButton"
                            text: qsTr("Keep updating")
                            tone: "primary"
                            onClicked: window.adapter.dismissCancelConfirmation()
                        }
                    }
                }
            }
        }
    }
}

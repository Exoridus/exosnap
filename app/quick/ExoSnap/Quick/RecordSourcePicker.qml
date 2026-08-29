pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Popup {
    id: root

    required property RecordViewModelAdapter recordViewModel

    // The pending choice is seeded from the view model when the picker opens
    // and is committed by the footer action, Enter or a double click. Browsing
    // is not committing: the capture keeps its current source until then.
    property int pendingTargetIndex: -1
    property int pendingCaptureMode: 0
    property string pendingPresetKey: ""

    property int currentTab: 0
    property string filterQuery: ""

    readonly property var regionPresetRows: recordViewModel.regionPresetOptions

    readonly property string windowsCountText: windowRows.length === 1 ? qsTr("1 window")
                                                                       : qsTr("%1 windows").arg(windowRows.length)

    // filteredTargetOptions is an invokable, so the binding cannot see its
    // dependencies; reading the target list into the result is what re-derives
    // the rows on a target refresh as well as on a query edit.
    readonly property var windowRows: {
        const all = recordViewModel.targetOptions
        return all.length >= 0 ? recordViewModel.filteredTargetOptions("window", filterQuery) : []
    }

    // Two columns while the popup is wide, one when the window narrows.
    function columnsForWidth(availableWidth: real): int {
        return availableWidth >= 520 ? 2 : 1
    }
    readonly property int pickerColumns: columnsForWidth(width - 2 * padding)

    parent: Overlay.overlay
    width: Math.min(680, parent ? parent.width - 48 : 680)
    height: Math.min(560, parent ? parent.height - 48 : 560)
    anchors.centerIn: parent
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: ExoTheme.spacingXl

    onAboutToShow: {
        pendingTargetIndex = recordViewModel.selectedTargetIndex
        pendingCaptureMode = recordViewModel.captureMode
        pendingPresetKey = ""
        filterQuery = ""
        windowSearch.text = ""
        // Opening refreshes the cached stills of the unselected targets; the
        // selected target stays on its live preview instead.
        recordViewModel.requestTargetStillRefresh()
    }

    function commit(): void {
        if (currentTab === 2) {
            const anchor = regionAnchorRow()
            if (!anchor)
                return
            recordViewModel.requestSelectTarget(anchor.targetIndex, 2)
            recordViewModel.requestRegionPreset(pendingPresetKey)
        } else {
            recordViewModel.requestSelectTarget(pendingTargetIndex, pendingCaptureMode)
        }
        close()
    }

    function confirmCard(targetIndex: int, captureMode: int): void {
        pendingTargetIndex = targetIndex
        pendingCaptureMode = captureMode
        commit()
    }

    function commitPreset(key: string): void {
        pendingPresetKey = key
        commit()
    }

    // The display a Region-tab rectangle anchors on: the display the user has
    // pending, else the one already capturing, else the first display.
    function regionAnchorRow(): var {
        const rows = recordViewModel.displayTargetOptions
        let fallback = null
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (row.targetIndex === pendingTargetIndex)
                return row
            if (fallback === null && row.targetIndex === recordViewModel.selectedTargetIndex)
                fallback = row
        }
        return fallback !== null ? fallback : (rows.length > 0 ? rows[0] : null)
    }

    background: Rectangle {
        color: ExoTheme.surfaceRaised
        border.width: 1
        border.color: ExoTheme.lineStrong
        radius: ExoTheme.radiusLg
    }

    contentItem: ColumnLayout {
        spacing: ExoTheme.spacingLg

        RowLayout {
            Layout.fillWidth: true

            Label {
                text: qsTr("Choose capture source")
                textFormat: Text.PlainText
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontPageTitle
                    weight: Font.DemiBold
                }
            }

            ExoButton {
                id: refreshButton

                objectName: "refreshButton"
                text: qsTr("Refresh previews")
                quiet: true
                compact: true
                onClicked: root.recordViewModel.requestTargetStillRefresh()
                ToolTip.visible: hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Refresh the cached preview stills")
                Accessible.name: qsTr("Refresh previews")
            }
        }

        ExoSegmentedControl {
            id: tabsControl

            objectName: "tabs"
            options: [qsTr("Displays"), qsTr("Windows"), qsTr("Region")]
            currentIndex: root.currentTab
            onSelected: index => root.currentTab = index
        }

        Item {
            id: displaysPage

            objectName: "displaysPage"
            visible: root.currentTab === 0
            Layout.fillWidth: true
            Layout.fillHeight: true

            GridView {
                id: displaysGrid

                objectName: "displaysGrid"
                anchors.fill: parent
                clip: true
                model: root.recordViewModel.displayTargetOptions
                // GridView has no spacing: the gap lives in the cell math and
                // the delegate draws its card one gap short of the cell.
                readonly property real cardGap: ExoTheme.spacingSm
                cellWidth: Math.floor((width - cardGap * (root.pickerColumns - 1)) / root.pickerColumns)
                cellHeight: 148
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ExoScrollBar {
                }

                delegate: TargetCard {
                    captureMode: 0
                    width: GridView.view.cellWidth - GridView.view.cardGap
                    height: GridView.view.cellHeight - GridView.view.cardGap
                }
            }
        }

        Item {
            id: windowsPage

            objectName: "windowsPage"
            visible: root.currentTab === 1
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                spacing: ExoTheme.spacingMd

                RowLayout {
                    Layout.fillWidth: true

                    ExoSearchField {
                        id: windowSearch

                        objectName: "windowSearch"
                        placeholderText: qsTr("Search windows")
                        Layout.fillWidth: true
                        onSearchEdited: query => root.filterQuery = query
                    }

                    Label {
                        objectName: "windowsCount"

                        text: root.windowsCountText
                        textFormat: Text.PlainText
                        color: ExoTheme.textSecondary
                        font.family: ExoTheme.monoFamily
                        font.pixelSize: ExoTheme.fontSecondary
                    }
                }

                GridView {
                    id: windowsGrid

                    objectName: "windowsGrid"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.windowRows
                    readonly property real cardGap: ExoTheme.spacingSm
                    cellWidth: Math.floor((width - cardGap * (root.pickerColumns - 1)) / root.pickerColumns)
                    cellHeight: 148
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ExoScrollBar {
                        objectName: "windowsScrollBar"
                    }

                    delegate: TargetCard {
                        captureMode: 1
                        width: GridView.view.cellWidth - GridView.view.cardGap
                        height: GridView.view.cellHeight - GridView.view.cardGap
                    }
                }
            }
        }

        Item {
            id: regionPage

            objectName: "regionPage"
            visible: root.currentTab === 2
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                spacing: ExoTheme.spacingMd

                Label {
                    objectName: "regionCaption"

                    text: {
                        const anchor = root.regionAnchorRow()
                        return anchor ? qsTr("A preset starts an editable rectangle on %1.").arg(anchor.regionLabel)
                                      : qsTr("Select a display first.")
                    }
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    color: ExoTheme.textSecondary
                    Layout.fillWidth: true
                    font.family: ExoTheme.sansFamily
                    font.pixelSize: ExoTheme.fontSecondary
                }

                Flow {
                    spacing: ExoTheme.spacingSm
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Repeater {
                        model: root.regionPresetRows

                        delegate: PresetCard {
                            width: Math.min(190, (regionPage.width - 2 * ExoTheme.spacingSm) / 3)
                            height: 128
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Item {
                Layout.fillWidth: true
            }

            ExoButton {
                objectName: "cancelButton"

                text: qsTr("Cancel")
                quiet: true
                onClicked: root.close()
            }

            ExoButton {
                objectName: "confirmButton"

                text: qsTr("Use source")
                tone: "primary"
                enabled: root.currentTab === 2 ? root.pendingPresetKey !== "" : root.pendingTargetIndex >= 0
                onClicked: root.commit()
            }
        }
    }

    component TargetCard: Rectangle {
        id: card

        // A QVariantList model exposes one role, modelData, whose map keys do
        // not initialize per-key required properties -- so the row arrives as
        // modelData and the typed surface is derived from it.
        required property var modelData

        readonly property int targetIndex: card.modelData.targetIndex
        readonly property string identity: card.modelData.identity
        readonly property string label: card.modelData.label
        readonly property string kind: card.modelData.kind
        readonly property string thumbnailState: card.modelData.thumbnailState
        readonly property string thumbnailSource: card.modelData.thumbnailSource

        property int captureMode: 0
        readonly property bool pending: root.pendingTargetIndex === card.targetIndex
                                        && root.pendingCaptureMode === card.captureMode

        objectName: "targetCard-" + card.identity
        color: card.pending ? ExoTheme.surfaceHover : ExoTheme.surface
        border.width: card.pending ? 2 : 1
        border.color: card.pending ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: card.label

        Keys.onReturnPressed: event => {
            root.confirmCard(card.targetIndex, card.captureMode)
            event.accepted = true
        }
        Keys.onEnterPressed: event => {
            root.confirmCard(card.targetIndex, card.captureMode)
            event.accepted = true
        }
        Keys.onSpacePressed: event => {
            root.pendingTargetIndex = card.targetIndex
            root.pendingCaptureMode = card.captureMode
            event.accepted = true
        }

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                root.pendingTargetIndex = card.targetIndex
                root.pendingCaptureMode = card.captureMode
            }
            onDoubleClicked: root.confirmCard(card.targetIndex, card.captureMode)
        }

        Rectangle {
            id: thumbnail

            anchors {
                top: parent.top
                left: parent.left
                right: parent.right
                margins: ExoTheme.spacingSm
            }
            height: 84
            color: ExoTheme.surfaceRaised
            radius: ExoTheme.radiusXs

            Image {
                anchors.fill: parent
                source: card.thumbnailSource
                visible: card.thumbnailState === "ready"
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                sourceSize: Qt.size(width, height)
            }

            ExoGlyph {
                anchors.centerIn: parent
                kind: card.kind === "window" ? ExoGlyph.AppWindow : ExoGlyph.Display
                visible: card.thumbnailState !== "ready"
                color: ExoTheme.textDim
                width: 22
                height: 22
            }

            ExoGlyph {
                anchors {
                    top: parent.top
                    right: parent.right
                    margins: ExoTheme.spacingXs
                }
                kind: ExoGlyph.Check
                visible: card.pending
                color: ExoTheme.accent
                width: 16
                height: 16
            }
        }

        Label {
            anchors {
                top: thumbnail.bottom
                left: parent.left
                right: parent.right
                margins: ExoTheme.spacingSm
            }
            text: card.label
            textFormat: Text.PlainText
            elide: Text.ElideRight
            maximumLineCount: 2
            wrapMode: Text.Wrap
            color: card.pending ? ExoTheme.text : ExoTheme.textSecondary
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontSecondary
        }
    }

    component PresetCard: Rectangle {
        id: presetCard

        // Same modelData shape as TargetCard: the row is a QVariantMap entry.
        required property var modelData

        readonly property string key: presetCard.modelData.key
        readonly property string label: presetCard.modelData.label
        readonly property real aspect: presetCard.modelData.aspect
        readonly property bool draw: presetCard.modelData.draw

        readonly property bool pendingPreset: root.pendingPresetKey === presetCard.key
        readonly property bool emphasized: presetCard.draw

        objectName: "presetCard-" + presetCard.key
        color: presetCard.pendingPreset ? ExoTheme.surfaceHover : ExoTheme.surface
        border.width: presetCard.pendingPreset ? 2 : 1
        border.color: presetCard.pendingPreset ? ExoTheme.accent
                     : presetCard.emphasized ? ExoTheme.accent : ExoTheme.line
        radius: ExoTheme.radiusSm
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: presetCard.label

        Keys.onReturnPressed: event => {
            root.commitPreset(presetCard.key)
            event.accepted = true
        }
        Keys.onEnterPressed: event => {
            root.commitPreset(presetCard.key)
            event.accepted = true
        }
        Keys.onSpacePressed: event => {
            root.pendingPresetKey = presetCard.key
            event.accepted = true
        }

        HoverHandler {
            cursorShape: Qt.PointingHandCursor
        }

        MouseArea {
            anchors.fill: parent
            onClicked: root.pendingPresetKey = presetCard.key
            onDoubleClicked: root.commitPreset(presetCard.key)
        }

        ExoGlyph {
            id: drawGlyph

            anchors.centerIn: parent
            kind: ExoGlyph.Region
            visible: presetCard.draw
            color: presetCard.pendingPreset ? ExoTheme.accent : ExoTheme.textSecondary
            width: 30
            height: 30
        }

        Rectangle {
            id: previewShape

            visible: !presetCard.draw
            readonly property real maxW: 88
            readonly property real maxH: 56
            readonly property real fitW: presetCard.aspect >= 1.0 ? maxW : Math.min(maxW, maxH * presetCard.aspect)
            readonly property real fitH: presetCard.aspect >= 1.0 ? Math.min(maxH, maxW / presetCard.aspect) : maxH

            width: Math.max(10, fitW)
            height: Math.max(10, fitH)
            radius: 2
            color: "#00000000"
            border.width: 2
            border.color: presetCard.pendingPreset ? ExoTheme.accent : ExoTheme.textDim
            anchors.centerIn: parent
            anchors.verticalCenterOffset: -10
        }

        Label {
            text: presetCard.label
            textFormat: Text.PlainText
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            color: presetCard.pendingPreset || presetCard.emphasized ? ExoTheme.text : ExoTheme.textSecondary
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontSecondary
            font.weight: presetCard.emphasized ? Font.DemiBold : Font.Medium
            anchors {
                bottom: parent.bottom
                left: parent.left
                right: parent.right
                margins: ExoTheme.spacingSm
            }
        }
    }
}

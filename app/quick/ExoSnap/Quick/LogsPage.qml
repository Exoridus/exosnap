import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Logs nav area: the raw event stream behind the diagnostics, plus the startup
// trace so start-up regressions are visible instead of buried in log lines.
//
// The history is diagnostics::AppLog's bounded deque, surfaced once through
// LogEntryModel and filtered by a proxy — the view holds no copy of its own.
Item {
    id: root

    required property LogsAdapter logs

    objectName: "quickLogsPage"

    onVisibleChanged: {
        if (root.visible) {
            // first-paint / preview-live land after this page is built.
            root.logs.refreshStartupTrace();
        }
    }

    FileDialog {
        id: exportDialog

        title: qsTr("Export Log")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Text files (*.txt)"), qsTr("All files (*)")]
        defaultSuffix: "txt"
        onAccepted: root.logs.exportToUrl(exportDialog.selectedFile)
    }

    FileDialog {
        id: bundleDialog

        title: qsTr("Save support bundle")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Zip archives (*.zip)")]
        defaultSuffix: "zip"
        onAccepted: root.logs.createSupportBundle(bundleDialog.selectedFile)
    }

    ColumnLayout {
        spacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            margins: ExoTheme.pagePadding
        }

        // The page title on the same rung as Settings, Device and Diagnostics.
        // It was a 10 px mono kicker with a hairline — the SECTION header
        // component, used as a page header — so the one nav destination whose
        // content is deliberately full-bleed also had the smallest identity of
        // the six. The startup trace further down keeps that component, which is
        // what it is for.
        RowLayout {
            spacing: ExoTheme.spacingMd
            Layout.fillWidth: true
            Layout.bottomMargin: ExoTheme.spacingXs

            Label {
                text: qsTr("Logs")
                textFormat: Text.PlainText
                color: ExoTheme.text
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontPageTitle
                    weight: Font.DemiBold
                }
            }

            // "Showing 23 of 23 entries · All · no search" — the page's own
            // subtitle, and it used to sit in a band of its own between the
            // toolbar and the list. One line of muted text does not need a band.
            Label {
                text: root.logs.statusText
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: ExoTheme.textMuted
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignBaseline
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontSecondary
                }
            }
        }

        // Toolbar. Left: severity segments, search, auto-scroll. Right: the three
        // file actions. The two clusters wrap independently so the narrow window
        // never squeezes the search field to nothing.
        GridLayout {
            columns: root.width >= 900 ? 2 : 1
            columnSpacing: ExoTheme.spacingLg
            rowSpacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            RowLayout {
                spacing: ExoTheme.spacingSm
                Layout.fillWidth: true

                ExoSegmentedControl {
                    options: [qsTr("All"), qsTr("Info"), qsTr("Issues")]
                    currentIndex: root.logs.severityFilter
                    onSelected: function (index) {
                        root.logs.severityFilter = index;
                    }
                }

                ExoSearchField {
                    placeholderText: qsTr("Search category or message")
                    Layout.fillWidth: true
                    Layout.minimumWidth: 120
                    onSearchEdited: function (query) {
                        root.logs.searchQuery = query;
                    }
                }

                ExoCheckBox {
                    text: qsTr("Auto-scroll")
                    checked: root.logs.autoScroll
                    onToggledByUser: function (value) {
                        root.logs.autoScroll = value;
                    }
                }
            }

            RowLayout {
                spacing: ExoTheme.spacingSm
                Layout.alignment: root.width >= 900 ? Qt.AlignRight : Qt.AlignLeft

                // All three chromed. Quiet, they rendered as three text runs in a
                // toolbar that already contains a segmented control, a search
                // field and a checkbox — the only three items up there that did
                // not look like controls were the three that write to the
                // clipboard and to disk.
                ExoButton {
                    text: qsTr("Copy")
                    enabled: root.logs.canCopy
                    onClicked: root.logs.copyVisible()
                }

                ExoButton {
                    text: qsTr("Export…")
                    enabled: root.logs.canExport
                    onClicked: exportDialog.open()
                }

                ExoButton {
                    text: qsTr("Create support bundle")
                    Accessible.description: qsTr("Create a diagnostic package to share with support")
                    onClicked: bundleDialog.open()
                }
            }
        }

        ExoLogView {
            model: root.logs.model
            autoScroll: root.logs.autoScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 180
            onCopyRequested: function (firstSequence, lastSequence) {
                root.logs.copySequenceRange(firstSequence, lastSequence);
            }
            onCopyAllRequested: root.logs.copyVisible()
        }

        RowLayout {
            spacing: ExoTheme.spacingXs
            Layout.fillWidth: true

            Label {
                text: qsTr("Full session logs are written to")
                textFormat: Text.PlainText
                color: ExoTheme.textDim
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontCaption
                }
            }

            AbstractButton {
                id: folderLink

                implicitHeight: 16
                implicitWidth: folderLabel.implicitWidth
                hoverEnabled: true
                Accessible.role: Accessible.Link
                Accessible.name: qsTr("Open the log folder")
                ToolTip.text: root.logs.logFilePath
                ToolTip.visible: folderLink.hovered && root.logs.logFilePath !== ""
                ToolTip.delay: 400
                onClicked: root.logs.openLogFolder()

                contentItem: Label {
                    id: folderLabel

                    text: root.logs.logFolderPath + "."
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: folderLink.hovered ? ExoTheme.text : ExoTheme.accent
                    font {
                        family: ExoTheme.monoFamily
                        pixelSize: ExoTheme.fontCaption
                        underline: folderLink.hovered
                    }
                }
            }

            Item {
                Layout.fillWidth: true
            }
        }

        DiagnosticsSectionHeader {
            title: qsTr("STARTUP")
            Layout.fillWidth: true
        }

        Label {
            text: qsTr("No startup milestones have been recorded yet.")
            textFormat: Text.PlainText
            visible: root.logs.startupTrace.length === 0
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontCaption
            }
        }

        // Bounded height: the milestone list scrolls inside the card rather than
        // pushing the log surface off the page once it outgrows the space.
        ExoScrollView {
            id: startupScroll

            contentWidth: availableWidth
            clip: true
            visible: root.logs.startupTrace.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(150, startupTable.implicitHeight)

            ExoKeyValueTable {
                id: startupTable

                width: startupScroll.availableWidth
                rows: root.logs.startupTrace
                valueKey: "elapsed"
                valueRightAligned: true
            }
        }
    }
}

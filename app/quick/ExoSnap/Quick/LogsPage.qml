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

        DiagnosticsSectionHeader {
            title: qsTr("APPLICATION LOG")
            Layout.fillWidth: true
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

                ExoButton {
                    text: qsTr("Copy")
                    enabled: root.logs.canCopy
                    quiet: true
                    onClicked: root.logs.copyVisible()
                }

                ExoButton {
                    text: qsTr("Export…")
                    enabled: root.logs.canExport
                    quiet: true
                    onClicked: exportDialog.open()
                }

                ExoButton {
                    text: qsTr("Create support bundle")
                    quiet: true
                    Accessible.description: qsTr("Create a diagnostic package to share with support")
                    onClicked: bundleDialog.open()
                }
            }
        }

        Label {
            text: root.logs.statusText
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.textMuted
            Layout.fillWidth: true
            Layout.minimumHeight: 15
            font {
                family: ExoTheme.sansFamily
                pixelSize: 11
            }
        }

        ExoLogView {
            model: root.logs.model
            autoScroll: root.logs.autoScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 180
            onCopyRequested: function (first, last) {
                root.logs.copyRange(first, last);
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
                    pixelSize: 11
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
                        pixelSize: 11
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
                pixelSize: 11
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

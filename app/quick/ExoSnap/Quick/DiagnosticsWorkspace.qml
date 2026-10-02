import QtQuick
import QtQuick.Layouts

// The Diagnostics destination's workspace: the overview the destination opens
// on, and the full log surface reachable from its Reference section, Ctrl+4, a
// "Show in log" action or a failed recording's "View log".
//
// One workspace rather than two destinations: the log surface fills this
// destination rather than sitting in a small embedded table, and the four top
// tabs stay visible with Diagnostics selected either way. The overview is
// loaded eagerly because it is the everyday first content; the log view is
// loaded on first request and kept resident after that, so its filter, search
// and scroll position survive a trip through the overview.
Item {
    id: workspace

    required property DiagnosticsAdapter diagnostics
    required property DeviceAdapter device
    required property LogsAdapter logs
    // The section lives on the shell rather than here: C++ owns navigation
    // state, and the control channel reports the same property.
    required property ShellAdapter shell

    // Forwarded to the shell, which owns the one navigation edge. The overview
    // asks for "logs", the log view asks to go back.
    signal navigateToLogsRequested()
    signal navigateToSettingsRequested()
    signal backToOverviewRequested()

    readonly property int section: workspace.shell.diagnosticsSection
    readonly property bool overviewReady: overviewLoader.status === Loader.Ready
    readonly property bool logsViewReady: logsLoader.status === Loader.Ready

    objectName: "quickDiagnosticsWorkspace"

    function ensureLogsLoaded(): void {
        if (logsLoader.status !== Loader.Null)
            return;
        logsLoader.setSource(Qt.resolvedUrl("LogsPage.qml"), {
            logs: workspace.logs,
            embedded: true
        });
    }

    function focusLogs(): void {
        if (workspace.section === ShellAdapter.DiagnosticsLogs && logsLoader.item)
            logsLoader.item.focusPrimary();
    }

    Component.onCompleted: {
        overviewLoader.setSource(Qt.resolvedUrl("DiagnosticsPage.qml"), {
            diagnostics: workspace.diagnostics,
            device: workspace.device
        });
        if (workspace.section === ShellAdapter.DiagnosticsLogs)
            workspace.ensureLogsLoaded();
    }

    onSectionChanged: {
        if (workspace.section === ShellAdapter.DiagnosticsLogs) {
            workspace.ensureLogsLoaded();
            Qt.callLater(workspace.focusLogs);
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: workspace.section

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Loader {
                id: overviewLoader

                anchors.fill: parent
                asynchronous: true
            }

            Connections {
                target: overviewLoader.item
                ignoreUnknownSignals: true

                function onNavigateToLogsRequested(): void {
                    workspace.navigateToLogsRequested();
                }

                function onNavigateToSettingsRequested(): void {
                    workspace.navigateToSettingsRequested();
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Loader {
                id: logsLoader

                anchors.fill: parent
                asynchronous: true

                onStatusChanged: {
                    if (logsLoader.status === Loader.Ready && workspace.section === ShellAdapter.DiagnosticsLogs)
                        Qt.callLater(workspace.focusLogs);
                }
            }

            Connections {
                target: logsLoader.item
                ignoreUnknownSignals: true

                function onBackRequested(): void {
                    workspace.backToOverviewRequested();
                }
            }
        }
    }
}

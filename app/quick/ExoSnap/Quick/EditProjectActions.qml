pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as Native

EditActionButton {
    id: root
    required property EditSessionAdapter session
    required property Item dialogParent
    readonly property bool shortcutsAvailable: visible && !session.projectBusy && !projectDiscard.visible
        && !recoveryChoice.visible && !projectFailure.visible && !openProjectFile.visible && !saveProjectFile.visible
    implicitWidth: 32
    panelAccess: true

    function openMenu(): void { projectMenu.open(); }
    Component.onCompleted: {
        session.initializeProjects();
        if (session.recoverableProjects.length > 0)
            recoveryChoice.open();
    }
    component ProjectDialog: Dialog {
        id: panel
        anchors.centerIn: parent
        width: Math.min((parent?.width ?? 440) - 32, 440)
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        padding: ExoTheme.spacingLg
        palette.window: ExoTheme.surfaceRaised
        palette.windowText: ExoTheme.text
        palette.text: ExoTheme.text
        palette.button: ExoTheme.surface
        palette.buttonText: ExoTheme.text
        palette.base: ExoTheme.surface
        palette.highlight: ExoTheme.accent
        palette.highlightedText: ExoTheme.accentInk
        Overlay.modal: Rectangle { color: ExoTheme.overlayScrim }
        background: Rectangle {
            color: ExoTheme.surfaceRaised
            border.width: 1
            border.color: ExoTheme.lineStrong
            radius: ExoTheme.radiusLg
        }
        header: Label {
            text: panel.title
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: ExoTheme.text
            leftPadding: ExoTheme.spacingLg
            rightPadding: ExoTheme.spacingLg
            topPadding: ExoTheme.spacingLg
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontSectionTitle
            font.weight: Font.DemiBold
        }
    }
    component ActionFooter: Flow {
        padding: ExoTheme.spacingMd
        spacing: ExoTheme.spacingXs
        layoutDirection: Qt.RightToLeft
    }
    property bool continueAfterSave: false
    property string projectAction: ""
    property var projectDiscardRevision: 0
    property url projectTarget
    property string recoveryTarget: ""
    function performProjectAction(discard: bool): void {
        const action = projectAction;
        projectAction = "";
        if (action === "new")
            session.newProject(projectDiscardRevision, discard);
        else if (action === "open")
            session.openProject(projectTarget, projectDiscardRevision, discard);
        else if (action === "recover")
            session.recoverProject(recoveryTarget, projectDiscardRevision, discard);
    }
    function confirmProjectDiscard(): void {
        projectDiscardRevision = session.closeGuardRevision();
        projectDiscard.open();
    }
    function requestProjectAction(action: string): void {
        if (session.projectBusy || !session.preflightPendingDraft())
            return;
        projectAction = action;
        if (session.projectDirty)
            confirmProjectDiscard();
        else
            performProjectAction(false);
    }
    function saveCurrentProject(): void {
        if (session.projectPath.length > 0)
            session.saveCurrentProject();
        else
            saveProjectFile.open();
    }
    objectName: "editProjectAccess"
    text: root.session.projectDirty ? qsTr("Project (unsaved changes)") : qsTr("Project")
    glyph: ExoGlyph.Layers
    expanded: projectMenu.opened
    onClicked: projectMenu.open()
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        width: 6
        height: 6
        radius: 3
        color: ExoTheme.accent
        visible: root.session.projectDirty || root.session.recoverableProjects.length > 0
    }
    ExoMenu {
        id: projectMenu
        x: root.width
        width: Math.min(root.dialogParent.width - root.width - 16, contentWidth + 2 * padding)
        ExoMenuItem {
            enabled: false
            text: root.session.projectPath.length > 0
                ? root.session.projectPath.split("/").pop() : qsTr("Untitled project")
        }
        ExoMenuItem { text: qsTr("New project"); enabled: !root.session.projectBusy; onTriggered: root.requestProjectAction("new") }
        ExoMenuItem { text: qsTr("Open project…"); enabled: !root.session.projectBusy; onTriggered: openProjectFile.open() }
        ExoMenuItem { text: qsTr("Save project"); enabled: !root.session.projectBusy; onTriggered: root.saveCurrentProject() }
        ExoMenuItem { text: qsTr("Save project as…"); enabled: !root.session.projectBusy; onTriggered: saveProjectFile.open() }
        ExoMenuItem {
            text: qsTr("Recover project…")
            visible: root.session.recoverableProjects.length > 0
            enabled: !root.session.projectBusy
            onTriggered: recoveryChoice.open()
        }
    }
    Shortcut { sequences: [StandardKey.New]; enabled: root.shortcutsAvailable; onActivated: root.requestProjectAction("new") }
    Shortcut { sequences: [StandardKey.Open]; enabled: root.shortcutsAvailable; onActivated: openProjectFile.open() }
    Shortcut { sequences: [StandardKey.Save]; enabled: root.shortcutsAvailable; onActivated: root.saveCurrentProject() }
    Shortcut { sequences: [StandardKey.SaveAs]; enabled: root.shortcutsAvailable; onActivated: saveProjectFile.open() }
    Native.FileDialog {
        id: openProjectFile
        title: qsTr("Open project")
        nameFilters: [qsTr("ExoSnap projects (*.exoproj)")]
        onAccepted: {
            root.projectTarget = selectedFile;
            root.requestProjectAction("open");
        }
    }
    Native.FileDialog {
        id: saveProjectFile
        title: qsTr("Save project as")
        fileMode: Native.FileDialog.SaveFile
        defaultSuffix: "exoproj"
        nameFilters: [qsTr("ExoSnap projects (*.exoproj)")]
        onAccepted: root.session.saveProject(selectedFile)
        onRejected: {
            root.continueAfterSave = false;
            root.session.preserveProjectRecovery();
        }
    }
    ProjectDialog {
        parent: root.dialogParent
        id: projectDiscard
        title: qsTr("Save changes before continuing?")
        contentItem: Label {
            text: root.session.projectPath.length ? root.session.projectPath.split("/").pop() : qsTr("Untitled project")
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: ExoTheme.textSecondary
            font.family: ExoTheme.sansFamily
        }
        footer: ActionFooter {
            ExoButton { id: cancelDiscard; objectName: "editProjectCancel"; compact: true; text: qsTr("Cancel"); onClicked: projectDiscard.reject() }
            ExoButton {
                compact: true
                objectName: "editProjectDiscard"
                text: qsTr("Discard")
                tone: "destructive"
                onClicked: { projectDiscard.close(); root.performProjectAction(true); }
            }
            ExoButton { compact: true; text: qsTr("Save"); tone: "primary"; onClicked: projectDiscard.accept() }
        }
        onAccepted: {
            root.continueAfterSave = true;
            root.saveCurrentProject();
        }
        onRejected: { root.projectAction = ""; root.continueAfterSave = false; }
        onOpened: cancelDiscard.forceActiveFocus(Qt.PopupFocusReason)
    }
    ProjectDialog {
        parent: root.dialogParent
        id: recoveryChoice
        title: qsTr("Recover an editor project")
        footer: ActionFooter {
            ExoButton { id: cancelRecovery; compact: true; text: qsTr("Cancel"); onClicked: recoveryChoice.reject() }
            ExoButton { compact: true; text: qsTr("Open"); tone: "primary"; onClicked: recoveryChoice.accept() }
        }
        onOpened: cancelRecovery.forceActiveFocus(Qt.PopupFocusReason)
        contentItem: ComboBox {
            id: recoveries
            model: root.session.recoverableProjects.map(path => ({file: path, label: path.split("/").pop().substring(0, 19)}))
            textRole: "label"
            valueRole: "file"
        }
        onAccepted: {
            root.recoveryTarget = recoveries.currentValue;
            root.requestProjectAction("recover");
        }
    }
    ProjectDialog {
        parent: root.dialogParent
        id: projectFailure
        title: qsTr("Editor project")
        footer: ActionFooter {
            ExoButton { id: dismissFailure; compact: true; text: qsTr("OK"); onClicked: projectFailure.accept() }
        }
        onOpened: dismissFailure.forceActiveFocus(Qt.PopupFocusReason)
        property string message: ""
        contentItem: Label { textFormat: Text.PlainText; text: projectFailure.message; wrapMode: Text.Wrap; color: ExoTheme.text }
    }
    Connections {
        target: root.session
        function onProjectSaved(): void {
            if (!root.continueAfterSave)
                return;
            root.continueAfterSave = false;
            if (root.session.projectDirty)
                root.confirmProjectDiscard();
            else
                root.performProjectAction(false);
        }
        function onProjectError(message: string): void {
            root.continueAfterSave = false;
            projectFailure.message = message;
            projectFailure.open();
        }
    }
}

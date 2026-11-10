pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

// The right-click menu of one Record scene item.
//
// The host owns the layout: the source's registered node-menu entries come
// first, then the host's own stacking and visibility operations, then Remove.
// Every request carries the item, runtime generation and edit token captured
// when the menu opened, so the adapter rejects it if the scene moved on while
// the menu was open.
ExoMenu {
    id: root
    objectName: "sceneItemContextMenu"

    required property SettingsAdapter settings
    property var target: ({})
    property var entries: []

    function openFor(state: var, at: point): void {
        root.target = state;
        root.entries = root.settings.sceneItemMenuEntries(state.sourceId, state.itemId);
        root.popup(at.x, at.y);
    }

    // Opens the submenu of one source entry, as a click on its row would.
    function openEntry(index: int): bool {
        const row = root.itemAt(index) as MenuItem;
        if (!row || !row.subMenu)
            return false;
        row.triggered();
        return row.subMenu.opened;
    }

    function command(name: string): bool {
        return root.settings.applySceneItemCommand(root.target.sourceId, root.target.itemId, root.target.runtime,
                                                   root.target.editRevision, name);
    }

    function dispatch(entry: var, choice: var): bool {
        return root.settings.dispatchVisualAction(entry.sourceId, entry.itemId, entry.runtime, entry.action, true,
                                                  choice === undefined ? null : choice);
    }

    Instantiator {
        model: root.entries

        delegate: ExoMenu {
            id: choiceMenu

            required property var modelData
            required property int index

            title: modelData.label
            valueText: modelData.valueLabel ?? ""
            enabled: modelData.enabled === true

            Instantiator {
                model: choiceMenu.modelData.choices ?? []

                delegate: ExoMenuItem {
                    required property var modelData

                    text: modelData.label
                    enabled: modelData.selectable !== false
                    current: modelData.value === choiceMenu.modelData.value
                    onTriggered: root.dispatch(choiceMenu.modelData, modelData.value)
                }

                onObjectAdded: (index, object) => choiceMenu.insertItem(index, object as MenuItem)
                onObjectRemoved: (index, object) => choiceMenu.removeItem(object as MenuItem)
            }
        }

        onObjectAdded: (index, object) => root.insertMenu(index, object as Menu)
        onObjectRemoved: (index, object) => root.removeMenu(object as Menu)
    }

    MenuSeparator {
        visible: root.entries.length > 0
        height: visible ? implicitHeight : 0
    }

    ExoMenuItem {
        objectName: "sceneItemBringToFront"
        text: qsTr("Bring to front")
        enabled: root.target.editable === true && root.target.canBringToFront === true
        onTriggered: root.command("front")
    }

    ExoMenuItem {
        objectName: "sceneItemSendToBack"
        text: qsTr("Send to back")
        enabled: root.target.editable === true && root.target.canSendToBack === true
        onTriggered: root.command("back")
    }

    ExoMenuItem {
        objectName: "sceneItemVisibility"
        text: root.target.outputVisible === false ? qsTr("Show in recording") : qsTr("Hide from recording")
        enabled: root.target.editable === true
        onTriggered: root.command(root.target.outputVisible === false ? "show" : "hide")
    }

    MenuSeparator {}

    ExoMenuItem {
        objectName: "sceneItemRemove"
        text: qsTr("Remove")
        enabled: root.target.editable === true
        onTriggered: root.command("remove")
    }
}

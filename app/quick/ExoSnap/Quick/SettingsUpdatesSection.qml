import QtQuick
import QtQuick.Layouts

ExoCard {
    id: root

    required property SettingsAdapter settings
    required property bool stacked

    title: qsTr("Updates")
    subtitle: root.settings.updateStatusText

    ExoSettingRow {
        label: qsTr("Update channel")
        stacked: root.stacked
        Layout.fillWidth: true

        ExoSelect {
            options: root.settings.updateChannelOptions
            value: root.settings.updateChannel
            enabled: !root.settings.controlsLocked
            Layout.fillWidth: true
            Accessible.name: qsTr("Update channel")
            onValueActivated: value => root.settings.updateChannel = value
        }
    }

    ExoSettingRow {
        label: qsTr("Check for updates automatically")
        hint: qsTr("Checks GitHub Releases when ExoSnap starts")
        stacked: root.stacked
        controlWidth: ExoTheme.controlSlotSwitch
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.autoUpdateCheck
            Accessible.name: qsTr("Check for updates automatically")
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            onToggledByUser: value => root.settings.autoUpdateCheck = value
        }
    }

    Text {
        visible: root.settings.updateManaged
        text: root.settings.updateManagerText
        color: ExoTheme.text
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }

    Text {
        visible: root.settings.updateManaged
        text: root.settings.updateManagerHint
        color: ExoTheme.textSecondary
        Layout.fillWidth: true
        wrapMode: Text.Wrap
    }

    Text {
        visible: root.settings.updateManagerCommand.length > 0
        text: root.settings.updateManagerCommand
        color: ExoTheme.text
        Layout.fillWidth: true
        wrapMode: Text.WrapAnywhere
    }

    RowLayout {
        spacing: ExoTheme.spacingSm
        Layout.fillWidth: true

        ExoButton {
            text: root.settings.updateActionText
            enabled: root.settings.updateActionEnabled
            onClicked: root.settings.updateAvailable
                       ? root.settings.runUpdatePrimaryAction()
                       : root.settings.checkForUpdates()
        }

        ExoButton {
            text: qsTr("Copy command")
            quiet: true
            visible: root.settings.updateManagerCommand.length > 0
            onClicked: root.settings.copyUpdateCommand()
        }

        // Names the version, as the spec requires: "What's new" alone says nothing
        // about which release it is about, and the card right above it is already
        // showing one.
        ExoButton {
            objectName: "settingsWhatsNewLink"
            text: qsTr("See what's new in v%1").arg(root.settings.updateAvailableVersion)
            quiet: true
            visible: root.settings.whatsNewAvailable
            onClicked: root.settings.showWhatsNew()
        }

        Item {
            Layout.fillWidth: true
        }
    }
}

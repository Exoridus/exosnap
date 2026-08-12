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
        hint: qsTr("Contacts GitHub Releases on start · off by default")
        stacked: root.stacked
        controlWidth: 60
        Layout.fillWidth: true

        ExoSwitch {
            checked: root.settings.autoUpdateCheck
            Accessible.name: qsTr("Check for updates automatically")
            onToggledByUser: value => root.settings.autoUpdateCheck = value
        }
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
            text: qsTr("What's new")
            quiet: true
            visible: root.settings.whatsNewAvailable
            onClicked: root.settings.showWhatsNew()
        }

        Item {
            Layout.fillWidth: true
        }
    }
}

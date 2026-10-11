import QtQuick
import QtQuick.Layouts

AudioRoutingPanel {
    id: root
    required property SettingsAdapter settings
    required property bool stacked
    routing: root.settings.audioRouting
    microphoneDeviceOptions: root.settings.microphoneDeviceOptions
    activeFocusOnTab: true
    function focusControl(target: int): var { return root; }
}

pragma ComponentBehavior: Bound
import QtQuick

EditDraftField {
    id: root
    property alias gainDb: root.value
    property bool track: false
    property bool mixed: false
    property var targetIds: []
    context: ({track: root.track, targets: root.targetIds.slice(), mixed: root.mixed})
    formatValue: (value, context) => context.mixed ? qsTr("Mixed") : root.session.formatGainValue(value)
    parseValue: (text, base, context) => root.session.parseGainValue(text, context.track)
}

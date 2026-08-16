import QtQuick
import QtQuick.Layouts

// The card shell the Expert taxonomy uses for a one-line statement plus its
// affordance: elevation status and its badge, the logs redirect and its button.
// Both were written out by hand with the same fill, hairline, radius, inner
// margins and row spacing, so a token change reached one and not the other.
// Children are declared inline and land in the row.
Rectangle {
    id: root

    default property alias content: contentRow.data

    implicitHeight: contentRow.implicitHeight + 2 * ExoTheme.spacingMd
    color: ExoTheme.surface
    border.width: 1
    border.color: ExoTheme.line
    radius: ExoTheme.radiusMd

    RowLayout {
        id: contentRow

        spacing: ExoTheme.spacingMd
        anchors {
            fill: parent
            topMargin: ExoTheme.spacingMd
            bottomMargin: ExoTheme.spacingMd
            leftMargin: ExoTheme.spacingLg
            rightMargin: ExoTheme.spacingLg
        }
    }
}

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// One toast card — the transient glance at the hub record (product-spec §9).
// The window that stacks, positions and times a set of these (the Quick
// counterpart of app/ui/overlay/NotificationToastWindow) is explicitly out of
// scope here and built by someone else; this file only renders one card's
// content, so both that window and, e.g., a --visual-test harness render
// byte-identical output for the same data.
//
// Layout follows the spec's toast rules exactly:
//  - a dismiss ✕ is always present (NotificationToastWindow::ToastHit's
//    is_dismiss target exists independently of action count);
//  - with exactly one action the whole card is clickable, marked with a
//    trailing "›" (cardIsAction below);
//  - with two actions each gets its own named (quiet) button;
//  - the body wraps up to six lines and ellipsizes beyond that — the hub,
//    not the toast, is where the untruncated text lives;
//  - a countdown bar renders only for a TIMED toast (`standing: false`),
//    matching NotificationManager::IsStanding()/DismissIntervalMs().
Rectangle {
    id: root

    required property string title
    required property string body
    // "success" | "caution" | "error" | "info" — notifications::AdvisoryStatusForType.
    required property string tone
    // [{action:int, label:string}, ...] — the `actions` role from
    // NotificationEntryModel, 0 to 2 entries.
    required property var actions
    // 0..1 remaining-dwell fraction, driven by the toast window from
    // NotificationManager::ShownAtMs()/DismissIntervalMs(). Meaningless
    // (and hidden) while standing is true.
    property real remainingFraction: 1
    property bool standing: false

    // action is the int from the clicked entry in `actions` (or the sole one,
    // for a card-wide click). The toast window forwards this to whatever owns
    // dispatch — this card never acts on it itself.
    signal actionRequested(int action)
    signal dismissRequested()

    readonly property color toneColor: root.tone === "success" ? ExoTheme.success
                                     : root.tone === "caution" ? ExoTheme.warning
                                     : root.tone === "error" ? ExoTheme.error
                                     : ExoTheme.accent
    readonly property bool cardIsAction: root.actions.length === 1

    width: 340
    implicitHeight: column.implicitHeight + 2 * ExoTheme.spacingMd
    color: ExoTheme.surfaceRaised
    border.width: 1
    // Deliberately the quiet line token: a toast is transient and informational,
    // and a strong outline made an ordinary "recording saved" read heavier than
    // the surfaces the user actually has to answer.
    border.color: ExoTheme.line
    radius: ExoTheme.radiusLg

    Accessible.role: Accessible.AlertMessage
    Accessible.name: root.body === "" ? root.title : root.title + ". " + root.body

    // Whole-card click target for the single-action case. Declared before the
    // header's own Dismiss/action controls in z-order (children are hit-
    // tested before their parent's own handlers), so those still take the
    // press first.
    TapHandler {
        enabled: root.cardIsAction
        onTapped: root.actionRequested(root.actions[0].action)
    }

    Rectangle {
        width: 3
        radius: 1.5
        color: root.toneColor
        anchors {
            top: parent.top
            bottom: parent.bottom
            left: parent.left
        }
    }

    ColumnLayout {
        id: column

        spacing: ExoTheme.spacingXs
        anchors {
            fill: parent
            margins: ExoTheme.spacingMd
            leftMargin: ExoTheme.spacingMd + 6
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: ExoTheme.spacingSm

            // Severity, said once more where the eye actually lands. The edge bar
            // marks the card; this marks the sentence.
            Rectangle {
                radius: 4
                color: root.toneColor
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 5
            }

            Label {
                text: root.title
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                    weight: Font.DemiBold
                }
            }

            ExoChevron {
                direction: 270
                visible: root.cardIsAction
                tone: ExoTheme.textMuted
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 12
                Layout.preferredHeight: 12
            }

            ExoGlyph {
                kind: ExoGlyph.Close
                color: dismissHover.hovered ? ExoTheme.text : ExoTheme.textMuted
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 12
                Layout.preferredHeight: 12

                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Dismiss notification")

                HoverHandler {
                    id: dismissHover
                }

                TapHandler {
                    onTapped: root.dismissRequested()
                }
            }
        }

        Label {
            text: root.body
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            visible: root.body !== ""
            color: ExoTheme.textSecondary
            maximumLineCount: 6
            elide: Text.ElideRight
            Layout.fillWidth: true
            font {
                family: ExoTheme.sansFamily
                pixelSize: ExoTheme.fontSecondary
            }
        }

        RowLayout {
            visible: root.actions.length === 2
            Layout.fillWidth: true
            Layout.topMargin: ExoTheme.spacingXs
            spacing: ExoTheme.spacingSm

            Item {
                Layout.fillWidth: true
            }

            Repeater {
                model: root.actions.length === 2 ? root.actions : []

                ExoButton {
                    id: actionButton

                    required property var modelData

                    text: actionButton.modelData.label
                    quiet: true
                    onClicked: root.actionRequested(actionButton.modelData.action)
                }
            }
        }

        Rectangle {
            visible: !root.standing
            Layout.fillWidth: true
            Layout.topMargin: ExoTheme.spacingXs
            implicitHeight: 2
            radius: 1
            color: ExoTheme.line

            Rectangle {
                height: parent.height
                radius: parent.radius
                color: root.toneColor
                width: parent.width * Math.max(0, Math.min(1, root.remainingFraction))
            }
        }
    }
}

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Release notes, as ONE always-expanded scrolling document, newest first
// (product-spec, "What's new (shipped)"). No collapse, no per-release
// disclosure: a changelog the user has to open six times is a changelog nobody
// reads, and the notes are the whole reason the surface is up.
//
// Two entry points, one surface, and the ONLY difference is which notes arrive
// plus whether the suppress tick is offered — see WhatsNewAdapter. Nothing here
// decides either.
//
// An in-window layer over the current page, exactly like recovery, the crash
// prompt and the recording-error report: same ExoOverlayCard, same scrim, same
// focus ring, same `contentTopInset` keeping it out of the shell's title band.
// It is deliberately NOT a nav destination and NOT a second window — the same
// argument ADR 0022 makes for the Edit surface.
ExoOverlayCard {
    id: root

    required property WhatsNewAdapter whatsNew

    objectName: "quickWhatsNewOverlay"
    subtitle: qsTr("RELEASE NOTES")
    // "none": this surface reports, it does not ask. A severity hero here would
    // dress a changelog as something the user has to resolve.
    severity: "none"
    title: qsTr("What's new")
    hint: root.whatsNew.postUpdateMode
          ? qsTr("You're now up to date. Here's what changed.")
          : qsTr("Everything shipped on this channel, newest first.")
    // Post-update only. The tick is what makes the auto-show optional; the
    // pre-update link is never gated by it, so offering it there would suggest
    // the link itself can be turned off.
    persistentVisible: root.whatsNew.postUpdateMode
    onDismissed: root.whatsNew.dismiss()

    Repeater {
        model: root.whatsNew.notes

        delegate: ColumnLayout {
            id: note

            required property int index
            required property var modelData

            spacing: ExoTheme.spacingSm
            Layout.fillWidth: true

            // Between releases, never above the first: the card's own title is
            // the top edge of the document.
            Rectangle {
                color: ExoTheme.line
                visible: note.index > 0
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                Layout.bottomMargin: ExoTheme.spacingXs
            }

            // Mono, like every other version string in the product (About's
            // metadata rows, the crash summary): a version is an identifier, not
            // prose.
            Label {
                objectName: "whatsNewNoteVersion"
                text: "v" + note.modelData.version
                textFormat: Text.PlainText
                color: ExoTheme.text
                Layout.fillWidth: true
                font {
                    family: ExoTheme.monoFamily
                    pixelSize: ExoTheme.fontSectionTitle
                    weight: Font.DemiBold
                }
            }

            // The GitHub release body verbatim, rendered as Markdown — headings,
            // lists and links come out as the author wrote them. Links are opened
            // through the adapter rather than by Qt's own handler, so every URL
            // this product opens goes through one place.
            Label {
                objectName: "whatsNewNoteBody"
                text: note.modelData.body !== "" ? note.modelData.body : qsTr("No release notes.")
                textFormat: Text.MarkdownText
                wrapMode: Text.WordWrap
                color: ExoTheme.textSecondary
                // No linkColor here: it only reaches Text's StyledText parser, and
                // this document is Markdown. QuickThemeTokens puts the accent in the
                // application palette's Link role, which is what the QTextDocument
                // path reads.
                Layout.fillWidth: true
                font {
                    family: ExoTheme.sansFamily
                    pixelSize: ExoTheme.fontBody
                }
                onLinkActivated: link => root.whatsNew.openUrl(link)
            }
        }
    }

    persistent: [
        ExoCheckBox {
            objectName: "whatsNewShowAfterUpdates"
            text: qsTr("Show release notes after updates")
            checked: root.whatsNew.showAfterUpdates
            onToggled: root.whatsNew.showAfterUpdates = checked
        }
    ]

    actions: [
        ExoButton {
            objectName: "whatsNewAllReleasesButton"
            text: qsTr("All releases")
            quiet: true
            leadingGlyph: ExoGlyph.ExternalLink
            onClicked: root.whatsNew.openAllReleases()
        },
        Item {
            Layout.fillWidth: true
        },
        // The one action, and it commits nothing: closing is the whole answer.
        // "Got it" after an update acknowledges what just happened; "Close" is
        // what a list the user opened themselves wants.
        ExoButton {
            objectName: "whatsNewCloseButton"
            text: root.whatsNew.postUpdateMode ? qsTr("Got it") : qsTr("Close")
            tone: "primary"
            onClicked: root.whatsNew.dismiss()
        }
    ]
}

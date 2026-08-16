pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    required property AboutViewModelAdapter aboutViewModel

    ExoScrollView {
        id: scrollView

        contentWidth: availableWidth
        // The card below is centred and its labels word-wrap, so its height is a
        // function of its width. Without a fixed gutter that closes a loop
        // through the scrollbar -- see ExoScrollView.
        reserveScrollBarGutters: true
        anchors.fill: parent

        Item {
            width: scrollView.availableWidth
            // Deliberately root.height -- neither scrollView.availableHeight nor
            // scrollView.height. This value becomes the ScrollView's contentHeight
            // and therefore its implicitHeight, so reading any of the ScrollView's
            // own geometry here makes the ScrollView's implicit size depend on its
            // assigned size. Inside the shell's StackLayout that is a recursive
            // rearrange, which surfaces as the binding loop this page used to log
            // on every launch. root.height is handed down by the layout and cannot
            // be influenced from in here, so the "at least one viewport tall, so a
            // short card stays centred" rule costs nothing.
            // The paddings are subtracted explicitly rather than by reading
            // availableHeight: with reserveScrollBarGutters they are fixed
            // constants (the bar's own implicit size), so this is the viewport
            // height without reintroducing a dependency on the content. Leaving
            // them in would make the content exactly one gutter taller than the
            // viewport and the page would always scroll by 12 px.
            implicitHeight: Math.max(root.height - scrollView.topPadding - scrollView.bottomPadding,
                                     card.implicitHeight + 2 * ExoTheme.spacing2Xl)

            Rectangle {
                id: card

                width: Math.min(520, parent.width - 2 * ExoTheme.spacing2Xl)
                implicitHeight: content.implicitHeight + 48
                color: ExoTheme.surface
                border.width: 1
                border.color: ExoTheme.line
                radius: ExoTheme.radiusLg
                anchors.centerIn: parent

                ColumnLayout {
                    id: content

                    spacing: ExoTheme.spacingMd
                    anchors.fill: parent
                    anchors.margins: ExoTheme.pagePadding

                    RowLayout {
                        spacing: ExoTheme.spacingLg
                        Layout.fillWidth: true

                        ExoBrandMark {
                            Layout.preferredWidth: 48
                            Layout.preferredHeight: 48
                            Accessible.ignored: true
                        }

                        ColumnLayout {
                            spacing: ExoTheme.spacingXs
                            Layout.fillWidth: true

                            Row {
                                spacing: 0

                                Label {
                                    text: "exo"  // the wordmark is never translated; see AppShell
                                    textFormat: Text.PlainText
                                    color: ExoTheme.text
                                    font {
                                        family: ExoTheme.sansFamily
                                        pixelSize: ExoTheme.fontValueLarge
                                        weight: Font.DemiBold
                                    }
                                }

                                Label {
                                    text: "snap"
                                    textFormat: Text.PlainText
                                    color: ExoTheme.accent
                                    font {
                                        family: ExoTheme.sansFamily
                                        pixelSize: ExoTheme.fontValueLarge
                                        weight: Font.DemiBold
                                    }
                                }
                            }

                            Label {
                                text: qsTr("Version %1 · for Windows").arg(root.aboutViewModel.version)
                                textFormat: Text.PlainText
                                color: ExoTheme.textDim
                                Layout.fillWidth: true
                                font {
                                    family: ExoTheme.monoFamily
                                    pixelSize: ExoTheme.fontSecondary
                                }
                            }
                        }
                    }

                    Label {
                        text: root.aboutViewModel.description
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: ExoTheme.textSecondary
                        Layout.fillWidth: true
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontBody
                        }
                    }

                    Rectangle {
                        implicitHeight: metadata.implicitHeight + 12
                        color: ExoTheme.surfaceRaised
                        radius: ExoTheme.radiusMd
                        Layout.fillWidth: true

                        ColumnLayout {
                            id: metadata

                            spacing: 0
                            anchors {
                                right: parent.right
                                left: parent.left
                                verticalCenter: parent.verticalCenter
                                margins: ExoTheme.spacingLg
                            }

                            AboutMetadataRow {
                                label: qsTr("Version")
                                value: root.aboutViewModel.version
                                Layout.fillWidth: true
                            }

                            AboutMetadataRow {
                                label: qsTr("Commit")
                                value: root.aboutViewModel.commitShort
                                linkEnabled: root.aboutViewModel.commitAvailable
                                Layout.fillWidth: true
                                onLinkActivated: root.aboutViewModel.openCommit()
                            }

                            AboutMetadataRow {
                                label: qsTr("Built")
                                value: root.aboutViewModel.builtDisplay
                                Layout.fillWidth: true
                            }

                            AboutMetadataRow {
                                label: qsTr("Install")
                                value: root.aboutViewModel.installMode
                                Layout.fillWidth: true
                            }

                            AboutMetadataRow {
                                label: qsTr("Channel")
                                value: root.aboutViewModel.channel
                                Layout.fillWidth: true
                            }

                            AboutMetadataRow {
                                label: qsTr("Author")
                                value: root.aboutViewModel.author
                                linkEnabled: true
                                Layout.fillWidth: true
                                onLinkActivated: root.aboutViewModel.openAuthor()
                            }
                        }
                    }

                    // Build provenance, as badges rather than three stacked
                    // banners. Each is a fact about this binary, not a warning the
                    // user has to act on, and three full-width amber boxes made an
                    // ordinary developer build look like a page of problems.
                    Flow {
                        spacing: ExoTheme.spacingSm
                        visible: root.aboutViewModel.unofficialBuild
                                 || root.aboutViewModel.debugBuild
                                 || root.aboutViewModel.dirtySourceTree
                        Layout.fillWidth: true

                        ExoBadge {
                            text: qsTr("Unofficial build")
                            tone: "notice"
                            visible: root.aboutViewModel.unofficialBuild
                        }

                        ExoBadge {
                            text: qsTr("Debug build")
                            tone: "notice"
                            visible: root.aboutViewModel.debugBuild
                        }

                        ExoBadge {
                            text: qsTr("Dirty source tree")
                            tone: "notice"
                            visible: root.aboutViewModel.dirtySourceTree
                        }
                    }

                    RowLayout {
                        spacing: ExoTheme.spacingSm
                        Layout.fillWidth: true

                        // One row of three peers, so all three are chromed. Two of
                        // them were quiet and one was not, which made the middle
                        // one look like the recommended action when the three are
                        // equals.
                        ExoButton {
                            text: qsTr("GitHub")
                            onClicked: root.aboutViewModel.openGitHub()
                        }

                        ExoButton {
                            enabled: !root.aboutViewModel.copying
                            text: root.aboutViewModel.copying ? qsTr("Copying…") : qsTr("Copy details")
                            onClicked: root.aboutViewModel.copyDetails()
                        }

                        ExoButton {
                            text: qsTr("Release notes")
                            onClicked: root.aboutViewModel.openReleaseNotes()
                        }

                        Item {
                            Layout.fillWidth: true
                        }
                    }

                    Label {
                        text: root.aboutViewModel.copyStatusText
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: ExoTheme.textMuted
                        visible: text.length > 0
                        Layout.fillWidth: true
                        font {
                            family: ExoTheme.sansFamily
                            pixelSize: ExoTheme.fontSecondary
                        }
                    }
                }
            }
        }
    }
}

pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ExoScrollView {
    id: root
    objectName: "editInspector"
    required property EditSessionAdapter session
    required property Item canvasFocusItem
    readonly property int mode: session.selectionMode
    readonly property var clipDetails: session.selectedClipDetails
    readonly property var junction: session.selectedJunctionDetails
    readonly property var range: session.editRange
    readonly property var scopeRange: session.rangeScopeReviewActive ? session.rangeGestureCandidate : range
    readonly property var scopeTrackIds: scopeRange.trackIds || []
    readonly property var track: session.tracks.find(row => row.id === session.activeTrack) || ({})
    readonly property bool oneClip: mode === EditSessionAdapter.ClipSelection && session.selectedClipIds.length === 1
    property real crossfadeDurationUs: 500000
    function focusField(field: string): void {
        const editor = field === "sourceIn" ? sourceInRow.editor : field === "sourceOut" ? sourceOutRow.editor : null;
        if (editor && editor.enabled) {
            editor.forceActiveFocus(Qt.OtherFocusReason);
            root.revealFocusedControl(editor);
        }
    }
    contentWidth: availableWidth
    clip: true
    rightPadding: ExoTheme.spacingMd
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    function revealFocusedControl(item: Item): void {
        if (!item || !root.flickable)
            return;
        let ancestor = item;
        while (ancestor && ancestor !== root)
            ancestor = ancestor.parent;
        if (ancestor !== root)
            return;
        const point = item.mapToItem(root.flickable, 0, 0);
        if (point.y < 0 || point.y + item.height > root.flickable.height)
            root.revealItem(item);
    }
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged(): void { root.revealFocusedControl(root.Window.window?.activeFocusItem ?? null); }
    }
    Keys.onEscapePressed: event => {
        if (!session.rangeScopeReviewActive)
            return;
        session.cancelRangeGesture();
        canvasFocusItem.forceActiveFocus();
        event.accepted = true;
    }

    function changeRangeScope(id: var, included: bool): void {
        const tracks = scopeTrackIds.filter(track => track !== id);
        if (included)
            tracks.push(id);
        session.previewRangeScope(tracks);
    }

    component Section: ColumnLayout {
        id: section
        required property string title
        Layout.fillWidth: true
        spacing: ExoTheme.spacingSm
        Label {
            Layout.fillWidth: true
            text: section.title
            color: ExoTheme.text
            font.family: ExoTheme.sansFamily
            font.pixelSize: ExoTheme.fontBody
            font.weight: Font.DemiBold
            Accessible.role: Accessible.Heading
        }
    }
    component TimeRow: ExoSettingRow {
        id: row
        property alias editor: editor
        required property string field
        required property real timeUs
        property var targetId: 0
        property string fieldObjectName: ""
        property string unavailableReason: ""
        property bool usesSamples: false
        readonly property bool locked: root.mode === EditSessionAdapter.ClipSelection ? root.session.trackLocked(root.clipDetails.trackId || 0) : root.mode === EditSessionAdapter.JunctionSelection ? root.session.trackLocked(root.junction.trackId || 0) : false
        stacked: true
        Layout.fillWidth: true
        function refreshPrecision(): void {
            if (root.session)
                usesSamples = root.session.timeValueUsesSamples(targetId, field);
        }
        onTargetIdChanged: refreshPrecision()
        onFieldChanged: refreshPrecision()
        Component.onCompleted: refreshPrecision()
        Connections {
            target: root.session
            function onWorkspaceChanged(): void { row.refreshPrecision(); }
            function onSelectionChanged(): void { row.refreshPrecision(); }
            function onInteractionSettingsChanged(): void { row.refreshPrecision(); }
        }
        EditTimeField {
            id: editor
            objectName: row.fieldObjectName
            Layout.fillWidth: true
            session: root.session
            canvasFocusItem: root.canvasFocusItem
            targetId: row.targetId
            valueUs: row.timeUs
            sampleContext: row.usesSamples
            enabled: row.unavailableReason.length === 0 && !row.locked && !(root.mode === EditSessionAdapter.RangeSelection && root.session.rangeScopeReviewActive)
            Accessible.name: row.label
            Accessible.description: row.locked ? qsTr("This track is locked.") : root.mode === EditSessionAdapter.RangeSelection && root.session.rangeScopeReviewActive ? qsTr("Apply or cancel the track scope review before changing boundaries.") : row.unavailableReason.length > 0 ? row.unavailableReason : qsTr("%1. %2").arg(formatDescription).arg(sampleContext ? qsTr("48 kHz sample precision") : qsTr("Project frame precision"))
            applyValue: (value, capturedTarget) => root.session.applyTimeValue(capturedTarget, row.field, value)
        }
    }
    component BiasField: EditDraftField {
        Layout.fillWidth: true
        session: root.session
        canvasFocusItem: root.canvasFocusItem
        formatValue: value => String(Math.round(value * 10000) / 100)
        parseValue: text => root.session.parseCurveBiasValue(text)
        Accessible.description: errorText.length > 0 ? errorText : qsTr("Curve bias from -100% to 100%")
    }
    component FadeShape: ColumnLayout {
        id: shape
        required property bool incoming
        readonly property int curve: (incoming ? root.clipDetails.fadeInCurve : root.clipDetails.fadeOutCurve) ?? 0
        readonly property real bias: (incoming ? root.clipDetails.fadeInBias : root.clipDetails.fadeOutBias) ?? 0
        readonly property bool owned: (incoming ? root.clipDetails.fadeInOwned : root.clipDetails.fadeOutOwned) ?? false
        readonly property string targetLabel: root.clipDetails.video
            ? (incoming ? qsTr("Video fade in curve") : qsTr("Video fade out curve"))
            : (incoming ? qsTr("Audio fade in curve") : qsTr("Audio fade out curve"))
        Layout.fillWidth: true
        ExoSettingRow {
            label: shape.targetLabel
            stacked: true
            Layout.fillWidth: true
            ExoSelect {
                objectName: shape.incoming ? "editDetailsFadeInCurve" : "editDetailsFadeOutCurve"
                Layout.fillWidth: true
                options: [{value: 0, label: qsTr("Linear"), selectable: true}, {value: 1, label: qsTr("Smooth"), selectable: true}, {value: 2, label: qsTr("Bias"), selectable: true}]
                value: shape.curve
                enabled: !shape.owned && !root.session.trackLocked(root.clipDetails.trackId || 0)
                Accessible.name: shape.targetLabel
                Accessible.description: shape.owned ? qsTr("The junction owns this fade.") : !enabled ? qsTr("This track is locked.") : ""
                onValueActivated: value => root.session.setClipFadeCurve(root.clipDetails.id, shape.incoming, value, shape.bias)
            }
        }
        ExoSettingRow {
            label: qsTr("Curve bias")
            stacked: true
            Layout.fillWidth: true
            visible: shape.curve === 2
            BiasField {
                objectName: shape.incoming ? "editDetailsFadeInBias" : "editDetailsFadeOutBias"
                Layout.fillWidth: true
                targetId: root.clipDetails.id ?? 0
                value: shape.bias
                context: ({curve: shape.curve, incoming: shape.incoming})
                enabled: !shape.owned && !root.session.trackLocked(root.clipDetails.trackId || 0)
                Accessible.name: qsTr("%1 bias").arg(shape.targetLabel)
                applyValue: (value, target, context) => root.session.setClipFadeCurve(target, context.incoming, context.curve, value)
            }
        }
    }
    ColumnLayout {
        width: root.availableWidth
        spacing: ExoTheme.spacingLg
        Section {
            title: qsTr("Transition")
            visible: root.mode === EditSessionAdapter.ClipSelection && root.session.selectedClip !== 0
            ExoSettingRow {
                label: qsTr("Crossfade duration")
                stacked: true
                Layout.fillWidth: true
                EditTimeField {
                    id: crossfadeDuration
                    objectName: "editInspectorCrossfadeDuration"
                    Layout.fillWidth: true
                    session: root.session
                    canvasFocusItem: root.canvasFocusItem
                    targetId: root.session.selectedClip
                    valueUs: root.crossfadeDurationUs
                    sampleContext: root.session.timeValueUsesSamples(targetId, "fadeIn")
                    function refreshGrid(): void {
                        sampleContext = root.session.timeValueUsesSamples(targetId, "fadeIn");
                    }
                    Connections {
                        target: root.session
                        function onWorkspaceChanged(): void { crossfadeDuration.refreshGrid(); }
                        function onSelectionChanged(): void { crossfadeDuration.refreshGrid(); }
                        function onInteractionSettingsChanged(): void { crossfadeDuration.refreshGrid(); }
                    }
                    Accessible.name: qsTr("Crossfade duration")
                    applyValue: value => {
                        if (value <= 0)
                            return false;
                        root.crossfadeDurationUs = value;
                        return true;
                    }
                }
            }
            ExoButton {
                objectName: "editInspectorCrossfadeAtCut"
                Layout.fillWidth: true
                text: qsTr("Crossfade to next clip")
                onClicked: if (root.session.previewCrossfadeAtCut(root.session.selectedClip, root.crossfadeDurationUs)) root.canvasFocusItem.forceActiveFocus()
            }
        }
        Section {
            title: qsTr("Clip")
            visible: root.oneClip
            Label {
                Layout.fillWidth: true
                text: root.clipDetails.name || ""
                wrapMode: Text.Wrap
                color: ExoTheme.textSecondary
            }
            TimeRow { label: qsTr("Timeline start"); field: "clipStart"; fieldObjectName: "editDetailsClipStart"; timeUs: root.clipDetails.startUs ?? 0; targetId: root.clipDetails.id ?? 0 }
            TimeRow { id: sourceInRow; label: qsTr("Source in"); field: "sourceIn"; fieldObjectName: "editDetailsSourceIn"; timeUs: root.clipDetails.sourceInUs ?? 0; targetId: root.clipDetails.id ?? 0 }
            TimeRow { id: sourceOutRow; label: qsTr("Source out"); field: "sourceOut"; fieldObjectName: "editDetailsSourceOut"; timeUs: root.clipDetails.sourceOutUs ?? 0; targetId: root.clipDetails.id ?? 0 }
            TimeRow { label: qsTr("Duration"); field: "duration"; timeUs: root.clipDetails.durationUs ?? 0; targetId: root.clipDetails.id ?? 0 }
            TimeRow {
                label: root.clipDetails.video ? qsTr("Video fade in") : qsTr("Audio fade in")
                field: "fadeIn"; fieldObjectName: "editDetailsFadeIn"
                timeUs: root.clipDetails.fadeInUs ?? 0; targetId: root.clipDetails.id ?? 0
                unavailableReason: root.clipDetails.fadeInOwned ? qsTr("The incoming junction owns this fade.") : ""
            }
            FadeShape { incoming: true }
            TimeRow {
                label: root.clipDetails.video ? qsTr("Video fade out") : qsTr("Audio fade out")
                field: "fadeOut"; fieldObjectName: "editDetailsFadeOut"
                timeUs: root.clipDetails.fadeOutUs ?? 0; targetId: root.clipDetails.id ?? 0
                unavailableReason: root.clipDetails.fadeOutOwned ? qsTr("The outgoing junction owns this fade.") : ""
            }
            FadeShape { incoming: false }
            ExoSettingRow {
                label: qsTr("Clip gain"); stacked: true; Layout.fillWidth: true
                visible: !root.clipDetails.video
                EditGainField {
                    objectName: "editDetailsClipGain"
                    Layout.fillWidth: true
                    session: root.session
                    canvasFocusItem: root.canvasFocusItem
                    targetId: root.clipDetails.id ?? 0
                    gainDb: root.clipDetails.gainDb ?? 0
                    enabled: !root.session.trackLocked(root.clipDetails.trackId || 0)
                    Accessible.name: qsTr("Clip gain in decibels")
                    applyValue: (value, target) => root.session.setClipGain(target, value)
                }
            }
        }
        Section {
            title: qsTr("Clips")
            visible: root.mode === EditSessionAdapter.ClipSelection && !root.oneClip
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 selected clips").arg(root.session.selectedClipIds.length)
                color: ExoTheme.textSecondary
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 audio, %2 video; %3 editable timing targets").arg(root.session.selectedClipSummary.audioCount || 0).arg(root.session.selectedClipSummary.videoCount || 0).arg(root.session.selectedClipSummary.timingEligibleCount || 0)
                color: ExoTheme.textSecondary
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: root.session.selectedClipSummary.timingMixed ? qsTr("Timing: mixed values") : qsTr("Duration: %1").arg(root.session.formatTimeValue(root.session.selectedClipSummary.durationUs || 0, 1))
                color: ExoTheme.textSecondary
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                visible: (root.session.selectedClipSummary.audioCount || 0) > 0
                text: (root.session.selectedClipSummary.gainMixed ? qsTr("Gain: mixed values") : qsTr("Gain: %1 dB").arg(root.session.selectedClipSummary.gainDb ?? 0)) + qsTr(" (%1 compatible targets)").arg(root.session.selectedClipSummary.gainEligibleCount || 0)
                color: ExoTheme.textSecondary
                wrapMode: Text.Wrap
            }
            ExoSettingRow {
                label: qsTr("Audio gain (%1 clips)").arg(root.session.selectedClipSummary.audioCount || 0)
                stacked: true
                Layout.fillWidth: true
                visible: (root.session.selectedClipSummary.audioCount || 0) > 0
                EditGainField {
                    objectName: "editDetailsMultiGain"
                    Layout.fillWidth: true
                    session: root.session
                    canvasFocusItem: root.canvasFocusItem
                    targetIds: root.session.selectedClipSummary.audioIds || []
                    gainDb: root.session.selectedClipSummary.gainDb ?? 0
                    mixed: root.session.selectedClipSummary.gainMixed ?? false
                    enabled: (root.session.selectedClipSummary.gainEligibleCount || 0) === (root.session.selectedClipSummary.audioCount || 0)
                    Accessible.name: qsTr("Selected audio clip gain in decibels")
                    Accessible.description: enabled ? qsTr("Applies only to the named audio clips.") : qsTr("A selected audio track is locked.")
                    applyValue: (value, target, context) => root.session.setClipGains(context.targets, value)
                }
            }
        }
        Section {
            title: qsTr("Junction")
            visible: root.mode === EditSessionAdapter.JunctionSelection
            TimeRow { label: qsTr("Start"); field: "junctionStart"; fieldObjectName: "editDetailsJunctionStart"; timeUs: root.junction.startUs ?? 0; targetId: root.junction.id ?? 0; visible: root.junction.kind !== "hardCut" }
            TimeRow { label: qsTr("End"); field: "junctionEnd"; fieldObjectName: "editDetailsJunctionEnd"; timeUs: root.junction.endUs ?? 0; targetId: root.junction.id ?? 0; visible: root.junction.kind !== "hardCut" }
            TimeRow { label: qsTr("Duration"); field: "junctionDuration"; timeUs: root.junction.durationUs ?? 0; targetId: root.junction.id ?? 0; visible: root.junction.kind !== "hardCut" }
            TimeRow { label: qsTr("Cut"); field: "junctionCut"; visible: root.junction.kind === "hardCut"; timeUs: root.junction.cutUs ?? 0; targetId: root.junction.id ?? 0 }
            ExoSettingRow {
                label: qsTr("Curve"); stacked: true; Layout.fillWidth: true
                visible: root.junction.kind !== "hardCut"
                ExoSelect {
                    Layout.fillWidth: true
                    options: [{value: 0, label: qsTr("Linear"), selectable: true}, {value: 1, label: qsTr("Smooth"), selectable: true}, {value: 2, label: qsTr("Bias"), selectable: true}]
                    value: root.junction.curve ?? 0
                    enabled: !root.session.trackLocked(root.junction.trackId || 0)
                    Accessible.name: qsTr("Junction curve")
                    onValueActivated: value => root.session.setJunctionCurve(value, root.junction.bias || 0)
                }
            }
            ExoSettingRow {
                label: qsTr("Curve bias"); stacked: true; Layout.fillWidth: true
                visible: root.junction.kind !== "hardCut" && root.junction.curve === 2
                BiasField {
                    objectName: "editDetailsJunctionBias"
                    Layout.fillWidth: true
                    targetId: root.junction.id ?? 0
                    value: root.junction.bias ?? 0
                    context: ({curve: root.junction.curve})
                    enabled: !root.session.trackLocked(root.junction.trackId || 0)
                    Accessible.name: qsTr("Junction curve bias")
                    applyValue: (value, target, context) => root.session.setJunctionCurveFor(target, context.curve, value)
                }
            }
            ExoSettingRow {
                label: qsTr("Audio gain law"); stacked: true; Layout.fillWidth: true
                visible: !root.junction.video && root.junction.kind !== "hardCut"
                ExoSelect {
                    Layout.fillWidth: true
                    options: [{value: 0, label: qsTr("Equal Gain"), selectable: true}, {value: 1, label: qsTr("Equal Power"), selectable: true}]
                    value: root.junction.audioLaw ?? 0
                    enabled: !root.session.trackLocked(root.junction.trackId || 0)
                    Accessible.name: qsTr("Junction audio gain law")
                    onValueActivated: value => root.session.setJunctionAudioLaw(value)
                }
            }
        }
        Section {
            title: qsTr("Range")
            visible: root.mode === EditSessionAdapter.RangeSelection
            Label {
                Layout.fillWidth: true
                text: qsTr("%1 scoped tracks").arg(root.scopeTrackIds.length)
                color: ExoTheme.textSecondary
            }
            Repeater {
                model: root.session.tracks.length
                delegate: ExoCheckBox {
                    id: scopeChoice
                    required property int index
                    readonly property var track: root.session.tracks[index] || ({})
                    readonly property var trackId: track.id || 0
                    objectName: "editRangeScope" + trackId
                    Layout.fillWidth: true
                    text: track.name || ""
                    checked: root.scopeTrackIds.includes(trackId)
                    onToggledByUser: value => {
                        root.changeRangeScope(trackId, value);
                        scopeChoice.checked = Qt.binding(() => root.scopeTrackIds.includes(scopeChoice.trackId));
                    }
                }
            }
            ExoButton {
                objectName: "editIncludeLinkedRangeTracks"
                Layout.fillWidth: true
                text: qsTr("Include linked tracks")
                onClicked: root.session.includeLinkedRangeTracks()
            }
            Label {
                objectName: "editRangeScopePreview"
                Layout.fillWidth: true
                visible: root.session.rangeScopeReviewActive
                text: qsTr("Nearest boundaries: %1 s to %2 s")
                    .arg(root.session.formatTimeValue(root.scopeRange.startUs || 0, 1))
                    .arg(root.session.formatTimeValue(root.scopeRange.endUs || 0, 1))
                    + (root.session.rangeGestureReason.length ? "\n" + root.session.rangeGestureReason : "")
                color: root.session.rangeGestureValid ? ExoTheme.textSecondary : ExoTheme.errorText
                wrapMode: Text.Wrap
                Accessible.name: text
            }
            RowLayout {
                Layout.fillWidth: true
                visible: root.session.rangeScopeReviewActive
                ExoButton {
                    objectName: "editRangeScopeApply"
                    Layout.fillWidth: true
                    text: qsTr("Apply scope")
                    enabled: root.session.rangeGestureValid
                    Accessible.description: enabled ? "" : root.session.rangeGestureReason
                    onClicked: if (root.session.commitRangeGesture()) root.canvasFocusItem.forceActiveFocus()
                }
                ExoButton {
                    objectName: "editRangeScopeCancel"
                    Layout.fillWidth: true
                    text: qsTr("Cancel")
                    onClicked: {
                        root.session.cancelRangeGesture();
                        root.canvasFocusItem.forceActiveFocus();
                    }
                }
            }
            TimeRow { label: qsTr("Start"); field: "rangeStart"; fieldObjectName: "editDetailsRangeStart"; timeUs: root.range.startUs ?? 0 }
            TimeRow { label: qsTr("End"); field: "rangeEnd"; timeUs: root.range.endUs ?? 0 }
            TimeRow { label: qsTr("Duration"); field: "rangeDuration"; timeUs: root.range.durationUs ?? 0 }
            ExoSettingRow {
                label: qsTr("Range gain change")
                stacked: true
                Layout.fillWidth: true
                EditGainField {
                    objectName: "editDetailsRangeGain"
                    Layout.fillWidth: true
                    session: root.session
                    canvasFocusItem: root.canvasFocusItem
                    gainDb: 0
                    enabled: !root.session.rangeScopeReviewActive
                    Accessible.name: qsTr("Audio gain change in the selected range, in decibels")
                    Accessible.description: qsTr("Adds this gain to every scoped audio clip in the range. The entire change is refused if any affected clip cannot be edited.")
                    applyValue: value => root.session.applyRangeGain(value)
                }
            }
            ExoButton {
                objectName: "editRemoveTimeOnAllTracks"
                Layout.fillWidth: true
                text: qsTr("Remove time on all tracks")
                enabled: !root.session.rangeScopeReviewActive
                onClicked: if (root.session.previewRemoveTimeOnAllTracks()) root.canvasFocusItem.forceActiveFocus()
            }
        }
        Section {
            title: qsTr("Track")
            visible: root.mode === EditSessionAdapter.TrackSelection
            Label { Layout.fillWidth: true; text: root.track.name || ""; color: ExoTheme.text; wrapMode: Text.Wrap }
            ExoCheckBox {
                Layout.fillWidth: true
                text: qsTr("Enabled")
                checked: root.track.enabled ?? true
                enabled: !root.track.locked
                onToggledByUser: value => root.session.setTrackEnabled(root.track.id, value)
            }
            ExoCheckBox {
                Layout.fillWidth: true
                text: qsTr("Locked")
                checked: root.track.locked ?? false
                onToggledByUser: value => root.session.setTrackLocked(root.track.id, value)
            }
            ExoSettingRow {
                label: qsTr("Track gain"); stacked: true; Layout.fillWidth: true
                visible: !root.track.video
                EditGainField {
                    objectName: "editDetailsTrackGain"
                    Layout.fillWidth: true
                    session: root.session
                    canvasFocusItem: root.canvasFocusItem
                    targetId: root.track.id ?? 0
                    track: true
                    gainDb: root.track.gainDb ?? 0
                    enabled: !root.track.locked
                    Accessible.name: qsTr("Track gain in decibels")
                    applyValue: (value, target) => root.session.setTrackGain(target, value)
                }
            }
        }
        Section {
            title: qsTr("Timeline")
            visible: root.mode === EditSessionAdapter.NoSelection || root.mode === EditSessionAdapter.GapSelection || root.mode === EditSessionAdapter.SpacerSelection
            ExoCheckBox {
                objectName: "editFixedTimelineEnd"
                Layout.fillWidth: true
                text: qsTr("Fixed timeline end")
                checked: root.session.fixedTimelineEnd
                onToggledByUser: value => {
                    if (value) root.session.applyTimeValue(0, "timelineEnd", root.session.durationUs);
                    else root.session.setFixedTimelineEnd(false, 0);
                }
            }
            TimeRow { label: qsTr("Timeline end"); field: "timelineEnd"; fieldObjectName: "editTimelineEnd"; timeUs: root.session.durationUs; enabled: root.session.fixedTimelineEnd }
            ExoButton {
                objectName: "editCloseGap"
                Layout.fillWidth: true
                visible: root.mode === EditSessionAdapter.GapSelection
                text: qsTr("Close Gap")
                onClicked: root.session.previewCloseGap()
            }
        }
    }
}

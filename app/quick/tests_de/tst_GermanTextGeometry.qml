import QtQuick
import QtTest
import ExoSnap.Quick.TestControls

TestCase {
    id: testCase
    name: "GermanTextGeometry"
    when: windowShown
    width: 860
    height: 700
    visible: true

    Component {
        id: noticeComponent
        ExoNotice { width: 360; tone: "success"; text: qsTranslate("SettingsAppearanceSection", "Restart ExoSnap to apply the new language.") }
    }
    Component {
        id: buttonComponent
        ExoButton { width: 190; text: qsTranslate("SettingsAppearanceSection", "Language") }
    }
    Component {
        id: tileComponent
        ExoStatusTile {
            width: 200
            title: qsTranslate("RecordPage", "Recording")
            value: "60 FPS"
            sub: qsTranslate("SettingsAppearanceSection", "Appearance")
        }
    }

    Component {
        id: recordingOverlayComponent
        OverlayRecording {
            overlayActive: false
            diagnosticsActive: true
            showFps: true
            showDrop: true
            showDrift: true
            showOutputSize: false
            showDiagnosticsSize: false
            showMutedSources: false
            fpsText: "60"
            dropText: "0"
            driftText: "+1 ms"
            outputSizeText: "42 MB"
        }
    }
    Component {
        id: quickControlsComponent
        OverlayQuickControlPill { overlayActive: false }
    }

    readonly property var audioResources: ({
        physical: [{
            identity: "capture-00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
            metrics: [{label: "Readers", value: "2", available: true, reason: ""}]
        }],
        sources: [{
            identity: "source-1",
            metrics: [{label: "Processing", value: "1234567890123456789012345678901234567890 ms", available: true, reason: ""}]
        }],
        tracks: [{
            identity: "track-1",
            metrics: [{label: "Limiter", value: "", available: false, reason: "No measured limiter activity is available."}]
        }],
        clipJobs: [{
            identity: "clip-1",
            metrics: [{label: "Copy budget", value: "17 ms", available: true, reason: ""}]
        }],
        unavailable: [{label: "Index commit lag", value: "", available: false, reason: "No committed index has been observed."}]
    })

    Component {
        id: audioResourcesComponent
        DiagnosticsResourcesCard { resources: testCase.audioResources }
    }

    function checkText(item) {
        for (let i = 0; i < item.children.length; ++i) {
            const child = item.children[i];
            if (child.visible && child.contentWidth !== undefined && child.text !== undefined
                    && String(child.text).length > 0) {
                verify(child.width > 0 && child.height > 0, child.text + ": collapsed label");
                if (child.elide === Text.ElideNone)
                    verify(child.contentWidth <= child.width + 1, child.text + ": clipped label");
            }
            checkText(child);
        }
    }

    function namedDescendants(item, name, found) {
        if (item.objectName === name)
            found.push(item);
        for (let i = 0; i < item.children.length; ++i)
            namedDescendants(item.children[i], name, found);
    }

    function visibleTextHasGeometry(item) {
        if (item.visible && item.contentWidth !== undefined && item.text !== undefined
                && String(item.text).length > 0 && (item.width <= 0 || item.height <= 0))
            return false;
        for (let i = 0; i < item.children.length; ++i) {
            if (!visibleTextHasGeometry(item.children[i]))
                return false;
        }
        return true;
    }

    function test_german_resource_is_active() {
        compare(qsTranslate("SettingsAppearanceSection", "Language"), "Sprache");
        compare(qsTranslate("SettingsAppearanceSection", "Appearance"), "Darstellung");
        compare(qsTranslate("UnknownDiagnostic", "nvenc_submit_failed"), "nvenc_submit_failed");
    }

    function test_german_geometry() {
        const factories = [noticeComponent, buttonComponent, tileComponent];
        for (let i = 0; i < factories.length; ++i) {
            const item = createTemporaryObject(factories[i], testCase);
            verify(item !== null);
            checkText(item);
            if (i === 0)
                verify(item.Accessible.name.indexOf("Erfolg. ") === 0);
        }
    }
    function test_german_overlay_geometry_and_accessibility() {
        const recording = createTemporaryObject(recordingOverlayComponent, testCase);
        verify(recording !== null);
        verify(recording.title.indexOf("Aufnahme") >= 0);
        verify(recording.width > 0 && recording.width <= 860);
        compare(recording.diagnosticsTokens.length, 3);
        const controls = createTemporaryObject(quickControlsComponent, testCase);
        verify(controls !== null);
        verify(controls.width > 0 && controls.width <= 860);
        const grip = findChild(controls, "quickControlGrip");
        verify(grip !== null);
        verify(grip.Accessible.name.indexOf("Schnellsteuerung") >= 0);
        verify(grip.Accessible.description.indexOf("Ziehen") >= 0);
    }

    function test_audio_resources_geometry_and_unavailable_reasons() {
        const widths = [860, 1100, 1600];
        for (let i = 0; i < widths.length; ++i) {
            const card = createTemporaryObject(audioResourcesComponent, testCase, {width: widths[i]});
            verify(card !== null);
            verify(card.visible);
            tryVerify(function () { return visibleTextHasGeometry(card); }, 1000,
                      "audio resource labels did not settle at " + widths[i]);
            checkText(card);

            const rows = [];
            namedDescendants(card, "diagnosticsAudioResourceRow", rows);
            compare(rows.length, 4);
            for (let j = 0; j < rows.length; ++j)
                verify(rows[j].width <= widths[i], "resource row exceeds " + widths[i]);

            const categories = [];
            namedDescendants(card, "diagnosticsAudioResourceCategory", categories);
            compare(categories.length, 4);

            const identities = [];
            namedDescendants(card, "diagnosticsAudioResourceIdentity", identities);
            compare(identities.length, 4);
            verify(identities.some(function (identity) {
                return identity.text === testCase.audioResources.physical[0].identity;
            }));

            const values = [];
            namedDescendants(card, "diagnosticsAudioResourceValue", values);
            compare(values.length, 5);
            compare(values.filter(function (value) { return value.text === "n/a"; }).length, 2);

            const reasons = [];
            namedDescendants(card, "diagnosticsAudioResourceReason", reasons);
            compare(reasons.filter(function (reason) { return reason.visible; }).length, 2);
            verify(reasons.some(function (reason) { return reason.text === "No measured limiter activity is available."; }));
            verify(reasons.some(function (reason) { return reason.text === "No committed index has been observed."; }));
        }
    }
}

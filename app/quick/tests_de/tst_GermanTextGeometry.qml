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
}

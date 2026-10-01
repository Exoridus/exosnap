import QtQuick
import QtTest
import ExoSnap.Quick.TestControls

TestCase {
    id: root
    name: "RecordPreviewChrome"
    when: windowShown
    width: 860
    height: 500
    visible: true
    Component {
        id: chromeComponent
        RecordPreviewChrome {
            width: 820
            height: 440
            formatText: "2560×1440 · AV1 · 60 CFR"
            sourceText: "Screen: Display 1"
            sourceGlyph: ExoGlyph.Display
            sourceEnabled: true
        }
    }
    SignalSpy { id: activated; signalName: "sourceRequested" }
    function test_source_states_data() {
        return [
            { tag: "none", text: "Select source" },
            { tag: "display", text: "Screen: Display 1" },
            { tag: "window", text: "App: Google Chrome" },
            { tag: "selecting", text: "Selecting region…" },
            { tag: "region", text: "Region: 1920×1080" },
            { tag: "long", text: "App: " + "Long window title ".repeat(30) }
        ];
    }
    function test_source_states(data) {
        const chrome = createTemporaryObject(chromeComponent, root, {sourceText: data.text});
        verify(chrome);
        const button = findChild(chrome, "previewSourceButton");
        compare(button.Accessible.name, data.text);
        verify(button.width > 0 && button.x + button.width <= chrome.width);
        button.forceActiveFocus();
        tryCompare(button, "activeFocus", true);
        activated.target = chrome;
        activated.clear();
        keyClick(Qt.Key_Space);
        compare(activated.count, 1);
    }
    function test_narrow_geometry_preserves_chrome() {
        const chrome = createTemporaryObject(chromeComponent, root, {width: 400});
        const button = findChild(chrome, "previewSourceButton");
        verify(button.width > 0 && button.x + button.width <= chrome.width);
        verify(findChild(chrome, "previewContractText").width > 0);
    }
    function test_metrics_require_a_live_measurement() {
        const chrome = createTemporaryObject(chromeComponent, root);
        const metrics = findChild(chrome, "previewLiveMetrics");
        compare(metrics.visible, false);
        chrome.metricsVisible = true;
        tryCompare(metrics, "visible", true);
        chrome.metricsVisible = false;
        tryCompare(metrics, "visible", false);
        verify(findChild(chrome, "previewContractText").visible);
    }
    Component {
        id: confidenceComponent
        SourceConfidence { indicators: [] }
    }
    function test_status_is_accessible_without_keyboard_stops() {
        const view = createTemporaryObject(confidenceComponent, root, {indicators: [
            {key: "speaker", included: true, tone: "success", description: "Application audio active; system audio muted"},
            {key: "microphone", included: false, tone: "neutral", description: "Microphone muted"},
            {key: "webcam", included: true, tone: "warning", description: "Camera recovering"}
        ]});
        const mic = findChild(view, "confidence-microphone");
        verify(mic);
        compare(mic.Accessible.name, "Microphone muted");
        compare(mic.activeFocusOnTab, false);
    }
}

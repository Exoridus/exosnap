import QtQuick
import QtTest

import ".."

Item {
    id: root
    width: 640
    height: 480

    Component {
        id: overlayComponent

        PreviewMetricsOverlay {
            property bool ownerExpanded: false

            width: 600
            height: 400
            expanded: ownerExpanded
            frameReady: true
            presentationRate: 60.0
            sourceDeliveryRate: 30.0
            frameTimeP95Ms: 16.8
            frameTimeP99Ms: 17.2
            accentColor: "#63e6be"
            surfaceColor: "#e6151517"
            textColor: "#f5f5f5"
            secondaryTextColor: "#b0b0b0"
            sansFamily: "Arial"
            monoFamily: "Consolas"
            onToggled: requested => ownerExpanded = requested
        }
    }

    SignalSpy {
        id: toggledSpy
        signalName: "toggled"
    }

    TestCase {
        name: "PreviewMetricsOverlayTests"
        when: windowShown

        function test_collapsedByDefault() {
            let overlay = createTemporaryObject(overlayComponent, root)
            verify(!!overlay, "Component exists")
            let metricsPanel = findChild(overlay, "previewMetricsOverlay")
            verify(!!metricsPanel, "Object exists")
            compare(overlay.expanded, false)
            compare(metricsPanel.visible, false)
        }

        function test_buttonTogglesSceneOverlay() {
            let overlay = createTemporaryObject(overlayComponent, root)
            verify(!!overlay, "Component exists")
            let button = findChild(overlay, "previewOverlayButton")
            verify(!!button, "Object exists")
            let metricsPanel = findChild(overlay, "previewMetricsOverlay")
            verify(!!metricsPanel, "Object exists")
            toggledSpy.target = overlay
            toggledSpy.clear()
            mouseClick(button)
            tryCompare(overlay, "expanded", true)
            tryCompare(metricsPanel, "visible", true)
            tryCompare(toggledSpy, "count", 1)
            overlay.ownerExpanded = false
            tryCompare(overlay, "expanded", false)
            compare(metricsPanel.visible, false)
        }
    }
}

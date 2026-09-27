import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-606. The diagnostics section's token list is keyed on the CONTENT
// POLICY, not on the measured values.
//
// It used to build an array of object literals carrying each token's resolved
// text, so the array depended on fpsText/dropText/driftText — properties the
// diagnostics callback moves roughly four times a second while recording. A
// `var` property compares by identity, so every one of those was a model
// assignment: QQmlDelegateModel tore down and rebuilt two to four Rows and six
// to twelve Texts, each with a fresh font-metric layout, on the same GUI
// thread as the DXGI preview.
//
// OverlayDiagnostics.qml was absorbed into OverlayRecording.qml (one window,
// two independently-gated sections); this test now targets that component
// directly. The window is never shown here: `visible` is gated on
// CaptureExclusion.granted, which is false in a test, and none of what is
// asserted needs it on screen — the delegates exist either way, which is
// exactly the cost this item is about.
TestCase {
    id: testCase

    name: "OverlayRecordingDiagnosticsTokens"
    when: windowShown
    width: 400
    height: 200
    visible: true

    Component {
        id: overlayComponent

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

    // The token delegates are children of the row alongside the Repeater itself
    // and the two muted glyphs, so they are reached through the Repeater.
    function tokens(overlay) {
        return findChild(overlay, "overlayTokenRepeater");
    }

    function test_the_configured_tokens_are_the_labels_in_reading_order() {
        let overlay = createTemporaryObject(overlayComponent, testCase);
        verify(overlay);

        compare(overlay.diagnosticsTokens.length, 3);
        compare(overlay.diagnosticsTokens[0], "fps");
        compare(overlay.diagnosticsTokens[1], "drop");
        compare(overlay.diagnosticsTokens[2], "drift");
    }

    function test_a_measured_value_moving_keeps_the_same_delegates() {
        let overlay = createTemporaryObject(overlayComponent, testCase);
        verify(overlay);
        let row = tokens(overlay);
        verify(row);

        let fpsDelegate = row.itemAt(0);
        let dropDelegate = row.itemAt(1);
        verify(fpsDelegate);
        verify(dropDelegate);

        overlay.fpsText = "58";
        overlay.dropText = "3";
        overlay.driftText = "-2 ms";

        // Same delegate objects — the row was updated, not rebuilt.
        compare(row.itemAt(0), fpsDelegate);
        compare(row.itemAt(1), dropDelegate);
        // And the values actually followed.
        tryCompare(fpsDelegate, "resolvedValue", "58");
        compare(dropDelegate.resolvedValue, "3");
    }

    function test_an_unmeasured_token_reads_as_an_em_dash_not_a_zero() {
        let overlay = createTemporaryObject(overlayComponent, testCase);
        verify(overlay);
        let row = tokens(overlay);
        verify(row);

        overlay.fpsText = "";

        tryCompare(row.itemAt(0), "resolvedValue", overlay.unavailable);
    }

    // Zero dropped frames is the one measured all-good state the pill reports in
    // green; any other count stays neutral.
    function test_only_a_measured_zero_drop_is_good() {
        let overlay = createTemporaryObject(overlayComponent, testCase);
        verify(overlay);
        let row = tokens(overlay);
        verify(row);

        compare(row.itemAt(1).good, true);
        compare(row.itemAt(0).good, false);

        overlay.dropText = "4";
        tryCompare(row.itemAt(1), "good", false);

        overlay.dropText = "";
        tryCompare(row.itemAt(1), "good", false);
    }

    // The content policy is the one thing that MAY restructure the row.
    function test_a_content_policy_change_restructures_the_row() {
        let overlay = createTemporaryObject(overlayComponent, testCase);
        verify(overlay);
        let row = tokens(overlay);
        verify(row);
        compare(row.count, 3);

        overlay.showFps = false;
        tryCompare(row, "count", 2);
        // The first token never carries the interpunct separator, whichever token
        // it happens to be.
        compare(row.itemAt(0).index, 0);
        compare(row.itemAt(0).modelData, "drop");
    }

    // The recording section and the diagnostics section are gated
    // independently — merging the two windows into one must not couple their
    // settings. "Recording off, diagnostics on" is the same independence the
    // two separate windows used to have.
    function test_recording_off_diagnostics_on_shows_only_diagnostics() {
        let overlay = createTemporaryObject(overlayComponent, testCase);
        verify(overlay);

        compare(overlay.overlayActive, false);
        compare(overlay.diagnosticsActive, true);
        // `visible` itself is gated on CaptureExclusion.granted, which is
        // false in this headless test; diagnosticsContentPresent is the part
        // of that gate this test can actually observe.
        compare(overlay.diagnosticsContentPresent, true);

        let row = tokens(overlay);
        verify(row);
        compare(row.count, 3);
    }

    // The output-size field is shared between the recording section's own
    // toggle and the diagnostics preset's "size" element: one value, rendered
    // once, whichever setting (or both) asks for it. It never becomes a fourth
    // diagnostics token.
    function test_output_size_is_one_shared_value_not_a_diagnostics_token() {
        let overlay = createTemporaryObject(overlayComponent, testCase);
        verify(overlay);

        compare(overlay.showSize, false);
        overlay.showDiagnosticsSize = true;
        compare(overlay.showSize, true);

        let row = tokens(overlay);
        verify(row);
        // Still just fps/drop/drift — "size" is never in this list.
        compare(row.count, 3);
        compare(overlay.diagnosticsTokens.indexOf("size"), -1);

        overlay.showDiagnosticsSize = false;
        overlay.showOutputSize = true;
        compare(overlay.showSize, true);
    }

    // ── Source-rect containment ──────────────────────────────────────────────
    //
    // The product contract: the pill lies entirely inside monitorGeometry
    // (bound to OverlayAdapter::recordedSourceGeometry in Main.qml, whatever
    // that resolves to for the active capture mode). It must never fall back
    // to the monitor just because it does not fit -- see clampedPosition.

    function verifyContained(overlay, source) {
        verify(overlay.x >= source.x, "pill left edge is left of the source rect");
        verify(overlay.y >= source.y, "pill top edge is above the source rect");
        verify(overlay.x + overlay.width <= source.x + source.width,
               "pill right edge (" + (overlay.x + overlay.width) + ") is right of the source rect ("
               + (source.x + source.width) + ")");
        verify(overlay.y + overlay.height <= source.y + source.height,
               "pill bottom edge is below the source rect");
    }

    // Measures the pill's own implicit size against a generous source rect
    // first, rather than guessing at pixel widths that drift with font
    // metrics, theme spacing or which content flags a test enables --
    // exactly the numbers a hand-picked literal would silently go stale
    // against.
    function measureWidth(properties) {
        let probe = createTemporaryObject(overlayComponent, testCase,
            Object.assign({monitorGeometry: Qt.rect(0, 0, 4000, 4000)}, properties));
        verify(probe);
        return Qt.size(probe.width, probe.height);
    }

    function test_pill_stays_within_a_small_region_source_rect() {
        // Minimal content: no diagnostics, so this is the smallest the pill
        // gets, matching a genuinely small Region selection.
        const props = {overlayActive: true, diagnosticsActive: false};
        const size = measureWidth(props);
        const source = Qt.rect(300, 100, size.width + 24, size.height + 24);
        let overlay = createTemporaryObject(overlayComponent, testCase,
            Object.assign({monitorGeometry: source}, props));
        verify(overlay);
        verifyContained(overlay, source);
    }

    function test_pill_stays_within_a_small_window_source_rect() {
        const props = {
            overlayActive: true,
            diagnosticsActive: false,
            showSourceName: true,
            sourceNameText: "A long recorded window title that would normally push the pill wide"
        };
        const size = measureWidth(props);
        const source = Qt.rect(-50, 20, size.width + 24, size.height + 24);
        let overlay = createTemporaryObject(overlayComponent, testCase,
            Object.assign({monitorGeometry: source}, props));
        verify(overlay);
        verifyContained(overlay, source);
    }

    // Diagnostics ON widens the pill the most: this is the case the merge
    // made more likely to overflow a small source rect.
    function test_widest_pill_with_diagnostics_still_stays_within_the_source_rect() {
        const props = {overlayActive: true, diagnosticsActive: true, showOutputSize: true, outputSizeText: "1.2 GB"};
        const size = measureWidth(props);
        const source = Qt.rect(0, 0, size.width + 24, size.height + 24);
        let overlay = createTemporaryObject(overlayComponent, testCase,
            Object.assign({monitorGeometry: source}, props));
        verify(overlay);
        verifyContained(overlay, source);
    }

    // The pill is wider than the whole source rect: it cannot fit, so the
    // policy is to anchor at the source's own top-left rather than centre,
    // overflow, or fall back to some other rectangle.
    function test_pill_wider_than_the_source_rect_anchors_to_its_top_left() {
        const size = measureWidth({overlayActive: true, diagnosticsActive: true});
        const source = Qt.rect(500, 200, Math.floor(size.width / 4), Math.floor(size.height / 2));
        let overlay = createTemporaryObject(overlayComponent, testCase, {
            overlayActive: true,
            monitorGeometry: source
        });
        verify(overlay);
        verify(overlay.width > source.width, "this test needs a pill wider than the source rect to mean anything");
        compare(overlay.x, source.x);
        compare(overlay.y, source.y);
    }

    // A monitor left of or above the primary produces negative virtual-desktop
    // coordinates; the clamp arithmetic must not assume a positive origin.
    function test_pill_stays_within_a_source_rect_at_negative_origin() {
        const props = {overlayActive: true, diagnosticsActive: true};
        const size = measureWidth(props);
        const source = Qt.rect(-1920, -200, size.width + 24, size.height + 24);
        let overlay = createTemporaryObject(overlayComponent, testCase,
            Object.assign({monitorGeometry: source}, props));
        verify(overlay);
        verifyContained(overlay, source);
    }
}

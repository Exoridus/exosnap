import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-606. The diagnostics HUD's token list is keyed on the CONTENT POLICY, not
// on the measured values.
//
// It used to build an array of object literals carrying each token's resolved
// text, so the array depended on fpsText/dropText/driftText/sizeText — four
// properties the diagnostics callback moves roughly four times a second while
// recording. A `var` property compares by identity, so every one of those was a
// model assignment: QQmlDelegateModel tore down and rebuilt two to four Rows and
// six to twelve Texts, each with a fresh font-metric layout, on the same GUI
// thread as the DXGI preview.
//
// The overlay Window is never shown here: `visible` is gated on
// CaptureExclusion.granted, which is false in a test, and none of what is
// asserted needs it on screen — the delegates exist either way, which is exactly
// the cost this item is about.
TestCase {
    id: testCase

    name: "OverlayDiagnosticsTokens"
    when: windowShown
    width: 400
    height: 200
    visible: true

    Component {
        id: overlayComponent

        OverlayDiagnostics {
            overlayActive: true
            showFps: true
            showDrop: true
            showDrift: true
            showSize: false
            showMutedSources: false
            fpsText: "60"
            dropText: "0"
            driftText: "+1 ms"
            sizeText: "42 MB"
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

        compare(overlay.tokens.length, 3);
        compare(overlay.tokens[0], "fps");
        compare(overlay.tokens[1], "drop");
        compare(overlay.tokens[2], "drift");
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

        overlay.showSize = true;

        tryCompare(row, "count", 4);
        compare(overlay.tokens.length, 4);
        compare(row.itemAt(3).resolvedValue, "42 MB");

        overlay.showFps = false;
        tryCompare(row, "count", 3);
        // The first token never carries the interpunct separator, whichever token
        // it happens to be.
        compare(row.itemAt(0).index, 0);
        compare(row.itemAt(0).modelData, "drop");
    }
}

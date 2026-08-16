import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-507: a readiness tile said "blocked" and "caution" with the hue of a 1 px
// border and a background tint, which is nothing at all to a user with a
// colour-vision deficiency. These pin the glyph and the spoken severity, and
// that a clear tile stays quiet rather than growing a fourth badge.
TestCase {
    id: testCase

    name: "ExoStatusTile"
    when: windowShown
    width: 320
    height: 200
    visible: true

    Component {
        id: tileComponent

        ExoStatusTile {
            width: 210
            title: "Disk"
            value: "412 GB"
            sub: "free on D:"
        }
    }

    function test_severity_has_a_glyph_data() {
        return [
            { tag: "blocker", tone: "blocker", glyph: ExoGlyph.Close, word: "Blocked" },
            { tag: "notice", tone: "notice", glyph: ExoGlyph.Warning, word: "Caution" }
        ];
    }

    function test_severity_has_a_glyph(data) {
        let tile = createTemporaryObject(tileComponent, testCase, { tone: data.tone });
        verify(tile);
        compare(tile.toneGlyph, data.glyph);
        compare(tile.severityText, data.word);
        verify(tile.Accessible.name.indexOf(data.word) === 0,
               "accessible name '" + tile.Accessible.name + "' must lead with the severity");
        verify(tile.Accessible.name.indexOf(tile.value) >= 0, "the measurement is still there");
    }

    function test_a_neutral_tile_draws_no_glyph_and_says_no_severity() {
        // The dashboard is mostly neutral tiles; a glyph on every one of them
        // would say nothing and cost the row its calm.
        let tile = createTemporaryObject(tileComponent, testCase);
        verify(tile);
        compare(tile.tone, "neutral");
        compare(tile.toneGlyph, ExoGlyph.Invalid);
        compare(tile.severityText, "");
        compare(tile.Accessible.name.indexOf("Disk"), 0);
    }

    function test_the_ok_glyph_still_works_and_now_speaks() {
        // `showOkGlyph` is the Readiness tile once everything passes. It drew a
        // check before this item; what is new is that the check is now part of
        // the same severity vocabulary and is said out loud.
        let tile = createTemporaryObject(tileComponent, testCase, { showOkGlyph: true });
        verify(tile);
        compare(tile.toneGlyph, ExoGlyph.Check);
        compare(tile.severityText, "Ready");
    }

    function test_a_severity_glyph_outranks_the_ok_glyph() {
        // A blocked tile that also asked for the check must not draw the check:
        // one glyph slot, and severity owns it.
        let tile = createTemporaryObject(tileComponent, testCase, { tone: "blocker", showOkGlyph: true });
        verify(tile);
        compare(tile.toneGlyph, ExoGlyph.Close);
    }

    function test_the_glyph_takes_the_readable_semantic_rung() {
        let blocked = createTemporaryObject(tileComponent, testCase, { tone: "blocker" });
        verify(blocked);
        compare(blocked.toneGlyphColor, ExoTheme.errorText);

        let caution = createTemporaryObject(tileComponent, testCase, { tone: "notice" });
        verify(caution);
        compare(caution.toneGlyphColor, ExoTheme.warningText);
    }
}

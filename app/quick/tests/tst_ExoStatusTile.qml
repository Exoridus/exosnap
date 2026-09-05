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

    // rec.004: the colour of a number comes only from the check that owns it.
    // QML maps the tone C++ already computed to a theme colour and nothing else.
    function test_value_tone_picks_the_theme_colour_data() {
        return [
            { tag: "ok", tone: "ok", color: ExoTheme.success },
            { tag: "warn", tone: "warn", color: ExoTheme.warning },
            { tag: "critical", tone: "critical", color: ExoTheme.error },
            { tag: "neutral", tone: "neutral", color: ExoTheme.text }
        ];
    }

    function test_value_tone_picks_the_theme_colour(data) {
        let tile = createTemporaryObject(tileComponent, testCase, { valueTone: data.tone });
        verify(tile);
        compare(tile.valueToneColor, data.color);
    }

    function test_an_empty_series_hides_the_sparkline() {
        let tile = createTemporaryObject(tileComponent, testCase);
        verify(tile);
        let spark = findChild(tile, "statusTileSparkline");
        verify(spark);
        compare(spark.visible, false);

        tile.series = [1, 2, 3];
        compare(spark.visible, true);
    }

    function test_head_badge_is_visible_only_when_set() {
        let tile = createTemporaryObject(tileComponent, testCase);
        verify(tile);
        let badge = findChild(tile, "statusTileHeadBadge");
        verify(badge);
        compare(badge.visible, false);

        tile.headBadge = "NVENC";
        compare(badge.visible, true);
        compare(badge.text, "NVENC");
    }

    function test_codec_chips_mark_the_selected_one_in_accent() {
        let tile = createTemporaryObject(tileComponent, testCase, {
            chips: [
                { text: "H.264", state: "available" },
                { text: "HEVC", state: "unavailable" },
                { text: "AV1", state: "selected" }
            ]
        });
        verify(tile);
        let repeater = findChild(tile, "statusTileChipsRepeater");
        verify(repeater);
        compare(repeater.count, 3);
        compare(repeater.itemAt(0).border.color, ExoTheme.line);
        compare(repeater.itemAt(1).unavailable, true);
        compare(repeater.itemAt(2).selected, true);
        compare(repeater.itemAt(2).border.color, ExoTheme.accent);
    }

    function test_a_tinted_fragment_of_sub_carries_its_own_tone() {
        let tile = createTemporaryObject(tileComponent, testCase, {
            sub: "Target 60 fps · jitter 9.2 ms",
            subTinted: "jitter 9.2 ms",
            subTone: "warn"
        });
        verify(tile);
        verify(tile.subDisplay.indexOf("<font") >= 0, "the tinted fragment is wrapped in a colour span");
        verify(tile.subDisplay.indexOf("jitter 9.2 ms") >= 0);
    }
}

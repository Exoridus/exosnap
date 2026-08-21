import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// The theme's font resources, one assertion per shipped face.
//
// A FontLoader registers exactly one file, and a weight with no registered face
// is silently resolved from whichever face IS registered — same outline, same
// metrics, no error anywhere. The product asks for Medium, DemiBold and Bold in
// more than fifty places, so "the file ships" and "the weight renders" are two
// different facts and only the second one matters.
//
// Deliberately NOT asserted through text metrics: real faces of one family may
// legitimately share advance widths, so a width comparison would test the
// typeface's design rather than this application's wiring.
TestCase {
    id: testCase

    name: "ExoThemeFonts"
    when: windowShown

    function test_every_shipped_sans_face_is_registered() {
        compare(ExoTheme.sansFont.status, FontLoader.Ready, "Regular");
        compare(ExoTheme.sansMediumFont.status, FontLoader.Ready, "Medium");
        compare(ExoTheme.sansSemiBoldFont.status, FontLoader.Ready, "SemiBold");
        compare(ExoTheme.sansBoldFont.status, FontLoader.Ready, "Bold");
    }

    function test_every_shipped_mono_face_is_registered() {
        compare(ExoTheme.monoFont.status, FontLoader.Ready, "Regular");
        compare(ExoTheme.monoMediumFont.status, FontLoader.Ready, "Medium");
    }

    // The faces have to land in ONE family, because that is what a `weight:`
    // request selects within. Four families named after their weight would load
    // just as successfully and leave every weight request unfulfilled.
    function test_the_faces_register_into_one_family_each() {
        compare(ExoTheme.sansMediumFont.name, ExoTheme.sansFont.name);
        compare(ExoTheme.sansSemiBoldFont.name, ExoTheme.sansFont.name);
        compare(ExoTheme.sansBoldFont.name, ExoTheme.sansFont.name);
        compare(ExoTheme.monoMediumFont.name, ExoTheme.monoFont.name);
    }

    // What the rest of the product actually reads.
    function test_the_theme_reports_the_bundled_families() {
        compare(ExoTheme.sansFamily, ExoTheme.sansFont.name);
        compare(ExoTheme.monoFamily, ExoTheme.monoFont.name);
        verify(ExoTheme.sansFamily !== "Segoe UI", "sans fell back to the system font");
        verify(ExoTheme.monoFamily !== "Consolas", "mono fell back to the system font");
    }
}

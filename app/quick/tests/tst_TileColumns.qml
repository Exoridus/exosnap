import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// Tile rows are always full: four when four fit, otherwise two, never three
// (which would leave a ragged 3 + 1 row).
TestCase {
    name: "TileColumns"

    function test_snaps_to_four_or_two_data() {
        return [
            { tag: "1160 -> 4", width: 1160, expected: 4 },
            { tag: "796 fits 3, snaps to 2", width: 796, expected: 2 },
            { tag: "430 -> 2", width: 430, expected: 2 },
            { tag: "200 -> 2, never 1", width: 200, expected: 2 }
        ];
    }

    function test_snaps_to_four_or_two(data) {
        compare(ExoTheme.tileColumns(data.width, 210, 12), data.expected);
    }
}

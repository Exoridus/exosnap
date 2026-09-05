import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

TestCase {
    id: testCase

    name: "ExoSparkline"
    when: windowShown
    width: 200
    height: 60
    visible: true

    Component {
        id: sparklineComponent

        ExoSparkline {
            width: 160
            height: 24
        }
    }

    // The line is only a trend if the highest sample is drawn above the lowest.
    // A point count says nothing about that: it holds for a mapping that puts
    // every point off the tile.
    function test_the_highest_value_is_drawn_at_the_top_of_the_box() {
        let spark = createTemporaryObject(sparklineComponent, testCase, { values: [10, 40, 25] });
        verify(spark);
        // 10% headroom above the peak, and the floor of a positive series is 0.
        fuzzyCompare(spark.valueY(40), spark.height * (1 - 1 / 1.1), 0.5);
        compare(spark.valueY(0), spark.height);
        verify(spark.valueY(40) < spark.valueY(25), "the peak sits above the middle sample");
        verify(spark.valueY(25) < spark.valueY(10), "the middle sample sits above the trough");
    }

    // A/V drift is signed, and a window that never reaches zero used to collapse
    // the scale onto a 1e-6 floor: every point landed millions of pixels below
    // the tile and the sparkline rendered blank.
    function test_a_negative_series_is_drawn_inside_the_box() {
        let spark = createTemporaryObject(sparklineComponent, testCase, { values: [-0.4, -0.6, -0.2] });
        verify(spark);
        let values = [-0.4, -0.6, -0.2];
        for (let i = 0; i < values.length; ++i) {
            let y = spark.valueY(values[i]);
            verify(y >= 0 && y <= spark.height, "y " + y + " for " + values[i] + " is outside the 24 px box");
        }
        verify(spark.valueY(-0.2) < spark.valueY(-0.6), "the least negative sample is the highest one");
    }

    function test_a_mixed_sign_series_keeps_its_shape() {
        let spark = createTemporaryObject(sparklineComponent, testCase, { values: [-2, 0, 3] });
        verify(spark);
        verify(spark.valueY(3) < spark.valueY(0), "zero is below the positive peak");
        verify(spark.valueY(0) < spark.valueY(-2), "the negative trough is the lowest point");
        compare(spark.valueY(-2), spark.height);
    }

    function test_budget_line_only_when_finite() {
        let withoutBudget = createTemporaryObject(sparklineComponent, testCase, { values: [1, 2, 3] });
        verify(withoutBudget);
        compare(withoutBudget.hasBudget, false);

        let withBudget = createTemporaryObject(sparklineComponent, testCase, { values: [1, 2, 3], budget: 16.67 });
        verify(withBudget);
        compare(withBudget.hasBudget, true);
    }
}

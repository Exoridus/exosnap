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
        }
    }

    function test_sixty_values_produce_sixty_points() {
        let values = [];
        for (let i = 0; i < 60; ++i)
            values.push(i % 10);

        let spark = createTemporaryObject(sparklineComponent, testCase, { values: values });
        verify(spark);
        compare(spark.pointCount, 60);
    }

    function test_no_values_draws_nothing() {
        let spark = createTemporaryObject(sparklineComponent, testCase);
        verify(spark);
        compare(spark.pointCount, 0);
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

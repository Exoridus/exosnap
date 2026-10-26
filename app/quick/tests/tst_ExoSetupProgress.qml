import QtQuick
import QtTest
import ExoSnap.Quick.TestControls

Item {
    id: root
    width: 240
    height: 40

    Component {
        id: progressComponent
        ExoSetupProgress {
            width: 200
            height: 4
            indeterminate: true
            progress: 0.38
        }
    }

    TestCase {
        name: "ExoSetupProgress"
        when: windowShown

        function test_unknown_progress_stays_still_without_implying_a_percentage() {
            const bar = createTemporaryObject(progressComponent, root)
            verify(bar !== null)
            const fill = bar.contentItem.children[0]
            compare(fill.x, 0)
            compare(fill.width, 0)
            wait(300)
            compare(fill.x, 0)
            compare(fill.width, 0)
        }

        function test_measured_progress_jumps_to_each_report_and_waits() {
            const bar = createTemporaryObject(progressComponent, root)
            verify(bar !== null)
            const fill = bar.contentItem.children[0]
            bar.indeterminate = false
            compare(fill.x, 0)
            compare(fill.width, bar.contentItem.width * 0.38)
            wait(300)
            compare(fill.width, bar.contentItem.width * 0.38)
            bar.progress = 0.72
            compare(fill.width, bar.contentItem.width * 0.72)
            bar.progress = 1
            compare(fill.x, 0)
            compare(fill.width, bar.contentItem.width)
        }
    }
}

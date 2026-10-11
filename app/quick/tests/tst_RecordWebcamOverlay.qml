import QtQuick
import QtTest
import ExoSnap.Quick.TestControls

TestCase {
    id: root
    name: "RecordWebcamOverlay"
    when: windowShown
    width: 440
    height: 340
    visible: true

    Component {
        id: stageComponent

        Item {
            id: stage

            property alias overlay: overlay

            width: 400
            height: 300

            RecordWebcamOverlay {
                id: overlay

                stage: stage
                editable: true
                modelRect: Qt.rect(0.3, 0.3, 0.4, 0.4)
                // Same round trip as the Record page: the request is stored and
                // comes back as the model rect.
                onRectRequested: rect => overlay.modelRect = rect
            }
        }
    }

    SignalSpy {
        id: requested
        signalName: "rectRequested"
    }

    function fuzzyRect(actual, expected, message) {
        fuzzyCompare(actual.x, expected.x, 0.002, message + " x")
        fuzzyCompare(actual.y, expected.y, 0.002, message + " y")
        fuzzyCompare(actual.width, expected.width, 0.002, message + " width")
        fuzzyCompare(actual.height, expected.height, 0.002, message + " height")
    }

    // Drags one corner grip by (dx, dy) stage pixels, in stage coordinates so
    // the gesture does not depend on where the overlay has moved to meanwhile.
    function dragCorner(stage, corner, dx, dy) {
        const overlay = stage.overlay
        const right = corner === 1 || corner === 2
        const bottom = corner >= 2
        const x = overlay.x + (right ? overlay.width - 4 : 4)
        const y = overlay.y + (bottom ? overlay.height - 4 : 4)
        mousePress(stage, x, y)
        mouseMove(stage, x + dx / 2, y + dy / 2)
        mouseMove(stage, x + dx, y + dy)
        mouseRelease(stage, x + dx, y + dy)
    }

    function test_each_corner_resizes_from_itself_and_keeps_the_opposite_one_data() {
        // dx/dy shrink the overlay by 40 px horizontally (0.1 of the stage).
        return [
            { tag: "top-left", corner: 0, dx: 40, dy: 40 },
            { tag: "top-right", corner: 1, dx: -40, dy: 40 },
            { tag: "bottom-right", corner: 2, dx: -40, dy: -40 },
            { tag: "bottom-left", corner: 3, dx: 40, dy: -40 }
        ]
    }

    function test_each_corner_resizes_from_itself_and_keeps_the_opposite_one(data) {
        const stage = createTemporaryObject(stageComponent, root)
        verify(stage)
        requested.target = stage.overlay
        requested.clear()
        // Copied: a rect property read is a live reference that would follow the
        // gesture and compare the result with itself.
        const draft = stage.overlay.draftRect
        const before = Qt.rect(draft.x, draft.y, draft.width, draft.height)

        dragCorner(stage, data.corner, data.dx, data.dy)

        compare(requested.count, 1, "one request per gesture")
        const after = requested.signalArguments[0][0]
        verify(after.width < before.width - 0.05,
               "the overlay shrank: " + JSON.stringify(before) + " -> " + JSON.stringify(after))
        const right = data.corner === 1 || data.corner === 2
        const bottom = data.corner >= 2
        if (right)
            fuzzyCompare(after.x, before.x, 0.002, "left edge stays")
        else
            fuzzyCompare(after.x + after.width, before.x + before.width, 0.002, "right edge stays")
        if (bottom)
            fuzzyCompare(after.y, before.y, 0.002, "top edge stays")
        else
            fuzzyCompare(after.y + after.height, before.y + before.height, 0.002, "bottom edge stays")
    }

    function test_a_released_resize_keeps_its_new_size() {
        // The size a gesture ends on is the one requested and the one shown
        // afterwards. Releasing the grip used to re-arm the model binding before
        // the request, so the old size was requested and the overlay snapped back.
        const stage = createTemporaryObject(stageComponent, root)
        verify(stage)
        requested.target = stage.overlay
        requested.clear()

        dragCorner(stage, 2, -60, -60)

        compare(requested.count, 1)
        const sent = requested.signalArguments[0][0]
        verify(sent.width < 0.4 - 0.1, "the shrunk size was requested")
        fuzzyRect(stage.overlay.draftRect, sent, "shown after release")
        fuzzyRect(stage.overlay.modelRect, sent, "stored after release")
    }

    function test_resizing_stops_at_the_minimum_size() {
        const stage = createTemporaryObject(stageComponent, root)
        verify(stage)
        requested.target = stage.overlay
        requested.clear()

        dragCorner(stage, 2, -300, -300)

        const sent = requested.signalArguments[0][0]
        fuzzyCompare(Math.min(sent.width, sent.height), 0.05, 0.002)
    }
}

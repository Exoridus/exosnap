import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-604. The six pipeline cards are delegates over a model with a stable
// per-stage identity, not a Repeater over a freshly built QVariantList.
//
// The old shape assigned a new list on every publication, which QQmlDelegateModel
// answers by destroying every delegate and building new ones; the profiler
// counted 432 rebuilt ExoPipelineStepCard subtrees in one auto-record trace, for
// six stages whose values move and whose identities never do.
//
// A ListModel stands in for the C++ PipelineStageModel here. What is asserted is
// the delegate contract both share: the five role names arrive in the card's five
// required properties, and a value that moves is a property update on a delegate
// that stays alive.
TestCase {
    id: testCase

    name: "PipelineFlow"
    when: windowShown
    width: 900
    height: 220
    visible: true

    Component {
        id: flowComponent

        Item {
            property alias stageModel: stageModel
            property alias flow: flow

            width: 900
            height: 220

            ListModel {
                id: stageModel

                ListElement {
                    title: "Capture"
                    status: "ok"
                    lane: "GPU"
                    value: "59.4 / 60.0 fps"
                    tip: "Frames arriving from the duplication."
                }
                ListElement {
                    title: "Convert"
                    status: "planned"
                    lane: "GPU"
                    value: "—"
                    tip: "Colour conversion."
                }
                ListElement {
                    title: "Encode"
                    status: "hotspot"
                    lane: "GPU (NVENC)"
                    value: "8.1 ms"
                    tip: "Encoder submit."
                }
            }

            ExoPipelineFlow {
                id: flow

                stages: stageModel
                width: parent.width
            }
        }
    }

    // The delegates are children of the Flow alongside the Repeater itself, so
    // they are reached through the Repeater rather than by child index.
    function cards(fixture) {
        return findChild(fixture.flow, "pipelineStageRepeater");
    }

    function test_role_names_reach_the_cards_required_properties() {
        let fixture = createTemporaryObject(flowComponent, testCase);
        verify(fixture);
        let repeater = cards(fixture);
        verify(repeater);

        compare(repeater.count, 3);
        compare(repeater.itemAt(0).title, "Capture");
        compare(repeater.itemAt(0).status, "ok");
        compare(repeater.itemAt(0).lane, "GPU");
        compare(repeater.itemAt(0).value, "59.4 / 60.0 fps");
        compare(repeater.itemAt(0).tip, "Frames arriving from the duplication.");
        compare(repeater.itemAt(2).title, "Encode");
        compare(repeater.itemAt(2).lane, "GPU (NVENC)");
    }

    // The live probe republishes at the diagnostics cadence with one changed
    // value. setProperty emits dataChanged without a reset, exactly like
    // PipelineStageModel::setStages() does for the same case.
    function test_a_changed_value_updates_the_same_card_object() {
        let fixture = createTemporaryObject(flowComponent, testCase);
        verify(fixture);
        let repeater = cards(fixture);
        verify(repeater);
        let capture = repeater.itemAt(0);

        fixture.stageModel.setProperty(0, "value", "58.1 / 60.0 fps");
        fixture.stageModel.setProperty(0, "status", "hotspot");

        tryCompare(capture, "value", "58.1 / 60.0 fps");
        compare(capture.status, "hotspot");
        // The same object, not a rebuilt one — this is the whole point of the item.
        compare(repeater.itemAt(0), capture);
    }

    function test_a_structural_change_still_reaches_the_flow() {
        let fixture = createTemporaryObject(flowComponent, testCase);
        verify(fixture);
        let repeater = cards(fixture);
        verify(repeater);

        fixture.stageModel.append({
            "title": "Mux",
            "status": "ok",
            "lane": "CPU",
            "value": "0.4 ms",
            "tip": "Container writer."
        });

        tryCompare(repeater, "count", 4);
        compare(repeater.itemAt(3).title, "Mux");
    }

    function test_every_card_takes_the_flows_computed_width() {
        let fixture = createTemporaryObject(flowComponent, testCase);
        verify(fixture);
        let repeater = cards(fixture);
        verify(repeater);

        for (let i = 0; i < repeater.count; ++i)
            compare(repeater.itemAt(i).width, fixture.flow.cardWidth);
    }
}

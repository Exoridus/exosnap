import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-411 (verify-first). Delegates whose model roles share a name with the
// component's own properties.
//
// ExoIssueCard, DeviceAdapterCard and DeviceCapabilityRow all declare
// properties called `title`, `label`, `active` — names their models also use as
// roles. Each is instantiated the same way: the delegate takes `required
// property var model` and assigns the roles across explicitly
// (`title: card.model.title`), rather than taking each role as its own required
// property, precisely because the two namespaces would collide.
//
// This pins down that the indirection actually tracks the model: a role updated
// with dataChanged and no reset has to reach a property of the same name on the
// delegate. It matters more since QCR-405 — the Diagnostics issue model now
// updates rows in place instead of resetting, which is exactly this path.
TestCase {
    id: testCase

    name: "DelegateRoleBinding"
    when: windowShown
    width: 300
    height: 200
    visible: true

    Component {
        id: repeaterComponent

        Item {
            property alias entries: entries
            property alias repeater: repeater

            ListModel {
                id: entries

                ListElement {
                    title: "Encoder falling behind"
                    measured: "62 fps"
                    active: true
                }
                ListElement {
                    title: "Disk is slow"
                    measured: "48 MB/s"
                    active: false
                }
            }

            Repeater {
                id: repeater

                model: entries

                Item {
                    id: delegateItem

                    required property var model

                    property string title: delegateItem.model.title
                    property string measured: delegateItem.model.measured
                    property bool active: delegateItem.model.active
                }
            }
        }
    }

    function test_roles_reach_same_named_delegate_properties() {
        let fixture = createTemporaryObject(repeaterComponent, testCase);
        verify(fixture);

        compare(fixture.repeater.count, 2);
        compare(fixture.repeater.itemAt(0).title, "Encoder falling behind");
        compare(fixture.repeater.itemAt(1).measured, "48 MB/s");
        compare(fixture.repeater.itemAt(0).active, true);
    }

    // setProperty on a ListModel emits dataChanged for that row without a
    // reset — the same signal DiagnosticIssueModel now sends for a measurement
    // that moved.
    function test_a_role_changed_without_a_reset_updates_the_delegate() {
        let fixture = createTemporaryObject(repeaterComponent, testCase);
        verify(fixture);
        let first = fixture.repeater.itemAt(0);

        fixture.entries.setProperty(0, "measured", "31 fps");

        tryCompare(first, "measured", "31 fps");
        // The same delegate object, not a rebuilt one: local state survives.
        compare(fixture.repeater.itemAt(0), first);
    }

    function test_every_overlapping_name_updates_independently() {
        let fixture = createTemporaryObject(repeaterComponent, testCase);
        verify(fixture);
        let second = fixture.repeater.itemAt(1);

        fixture.entries.setProperty(1, "title", "Disk is very slow");
        fixture.entries.setProperty(1, "active", true);

        tryCompare(second, "title", "Disk is very slow");
        tryCompare(second, "active", true);
        compare(second.measured, "48 MB/s");
    }
}

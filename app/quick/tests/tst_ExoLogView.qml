import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-404. A log selection identifies ENTRIES, not row positions.
//
// The real model is a filter proxy over a bounded history that evicts from the
// front, so rows move under the selection constantly: an append, an eviction, a
// severity filter, a search. A ListModel stands in for it here — every role the
// delegate requires is present, and nothing asserted below is about where the
// rows come from.
TestCase {
    id: testCase

    name: "ExoLogView"
    when: windowShown
    width: 600
    height: 300
    visible: true

    Component {
        id: viewComponent

        ExoLogView {
            property alias entries: logEntries

            width: 600
            height: 300

            model: ListModel {
                id: logEntries
            }
        }
    }

    function focusList(view) {
        for (let i = 0; i < view.children.length; ++i) {
            if (view.children[i].objectName === "logList") {
                view.children[i].forceActiveFocus();
                return view.children[i];
            }
        }
        return null;
    }

    function appendEntry(view, sequence, message) {
        view.entries.append({
            "sequence": sequence,
            "timestampText": "2026-08-16T10:00:0" + (sequence % 10),
            "severityKey": "info",
            "severityLabel": "INFO",
            "category": "engine",
            "message": message
        });
    }

    function makeView(sequences) {
        let view = createTemporaryObject(viewComponent, testCase);
        verify(view);
        for (let i = 0; i < sequences.length; ++i)
            appendEntry(view, sequences[i], "entry " + sequences[i]);
        return view;
    }

    function test_clicking_a_row_selects_that_entry() {
        let view = makeView([10, 11, 12]);

        view.selectRow(11, false);

        compare(view.hasSelection, true);
        compare(view.selectionLowSequence, 11);
        compare(view.selectionHighSequence, 11);
    }

    function test_shift_click_extends_from_the_anchor() {
        let view = makeView([10, 11, 12, 13]);

        view.selectRow(11, false);
        view.selectRow(13, true);

        compare(view.selectionLowSequence, 11);
        compare(view.selectionHighSequence, 13);

        // Extending backwards past the anchor is still one span, in the order
        // the two ends actually are.
        view.selectRow(10, true);
        compare(view.selectionLowSequence, 10);
        compare(view.selectionHighSequence, 11);
    }

    // The case the index-based selection got wrong: the history evicts from the
    // front, every surviving row moves up by one, and index 1 is a different
    // entry than the one that was clicked.
    function test_selection_stays_on_its_entry_through_front_eviction() {
        let view = makeView([10, 11, 12, 13]);
        view.selectRow(12, false);

        view.entries.remove(0);

        compare(view.selectionLowSequence, 12);
        compare(view.selectionHighSequence, 12);
    }

    function test_selection_is_unmoved_by_an_append() {
        let view = makeView([10, 11]);
        view.selectRow(10, false);

        appendEntry(view, 12, "entry 12");
        appendEntry(view, 13, "entry 13");

        compare(view.selectionLowSequence, 10);
        compare(view.selectionHighSequence, 10);
    }

    // An emptied view holds no selection. The real Clear goes through
    // LogEntryModel::clear(), which is a model reset; a QML ListModel removes
    // its rows one signal at a time instead, so this drives the other of the two
    // paths that lead to an empty view. waitForRendering first, because a
    // ListView only recomputes `count` when it lays out — without it the view
    // never observes the rows that were just added, let alone their removal.
    function test_emptying_the_model_drops_a_select_all() {
        let view = makeView([10, 11, 12]);
        verify(focusList(view));
        waitForRendering(view);
        keyClick(Qt.Key_A, Qt.ControlModifier);
        compare(view.selectionIsAll, true);

        view.entries.clear();
        waitForRendering(view);

        tryCompare(view, "selectionIsAll", false);
        compare(view.hasSelection, false);
    }

    // A range selection over an emptied view matches nothing, and picks nothing
    // up when the log fills again: sequences are never reused.
    function test_a_range_selection_matches_nothing_after_the_log_is_emptied() {
        let view = makeView([10, 11, 12]);
        view.selectRow(11, false);
        compare(view.hasSelection, true);

        view.entries.clear();
        appendEntry(view, 40, "entry 40");
        waitForRendering(view);

        compare(view.selectionLowSequence, 11);
        compare(view.selectionHighSequence, 11);
        verify(view.selectionHighSequence < 40);
    }

    // Ctrl+A means "everything visible", which has to keep meaning that after a
    // filter change — a span captured from the rows that were visible then
    // would not.
    function test_select_all_survives_a_filter_change() {
        let view = makeView([10, 11, 12]);

        verify(focusList(view));
        keyClick(Qt.Key_A, Qt.ControlModifier);
        compare(view.selectionIsAll, true);

        // The filter dropped one entry and admitted a newer one.
        view.entries.remove(1);
        appendEntry(view, 20, "entry 20");

        compare(view.selectionIsAll, true);
        compare(view.hasSelection, true);
    }

    function test_select_all_is_dropped_when_a_row_is_clicked() {
        let view = makeView([10, 11, 12]);
        verify(focusList(view));
        keyClick(Qt.Key_A, Qt.ControlModifier);

        view.selectRow(11, false);

        compare(view.selectionIsAll, false);
        compare(view.selectionLowSequence, 11);
    }

    function test_copy_reports_the_selected_sequences() {
        let view = makeView([10, 11, 12, 13]);
        let copied = null;
        view.copyRequested.connect(function (first, last) {
            copied = [first, last];
        });

        view.selectRow(11, false);
        view.selectRow(13, true);
        verify(focusList(view));
        keyClick(Qt.Key_C, Qt.ControlModifier);

        verify(copied !== null);
        compare(copied[0], 11);
        compare(copied[1], 13);
    }

    function test_copy_without_a_selection_copies_everything() {
        let view = makeView([10, 11]);
        let all = 0;
        view.copyAllRequested.connect(function () {
            ++all;
        });

        verify(focusList(view));
        keyClick(Qt.Key_C, Qt.ControlModifier);
        compare(all, 1);

        // Select-all is the same request: "everything visible" is not a span,
        // and the adapter already has a verb for it.
        keyClick(Qt.Key_A, Qt.ControlModifier);
        keyClick(Qt.Key_C, Qt.ControlModifier);
        compare(all, 2);
    }
}

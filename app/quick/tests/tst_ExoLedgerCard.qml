import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

TestCase {
    id: testCase

    name: "ExoLedgerCard"
    when: windowShown
    width: 420
    height: 240
    visible: true

    Component {
        id: cardComponent

        ExoLedgerCard {
            width: 400
            entryId: "rec.001"
            title: "Judder: present cadence is irregular"
            summary: "Present jitter above 8 ms while capturing at a constant 60 fps."
            active: false
            count: 3
            firstSeenText: "15:43:12"
            lastSeenText: "15:53:05"
            worstText: "11.4 ms"
            budgetText: "8 ms"
            totalActiveText: "6.5 s"
        }
    }

    function test_the_collapsed_row_is_44_px_and_speaks_its_severity() {
        let card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        compare(card.expanded, false);
        compare(card.height, 44);
        verify(card.Accessible.name.indexOf("Warning.") === 0,
               "accessible name '" + card.Accessible.name + "' must lead with the severity");
    }

    function test_active_fills_amber_quiet_stays_plain_data() {
        return [
            { tag: "active", active: true, color: ExoTheme.warningSurface },
            { tag: "quiet", active: false, color: ExoTheme.surface }
        ];
    }

    function test_active_fills_amber_quiet_stays_plain(data) {
        let card = createTemporaryObject(cardComponent, testCase, { active: data.active });
        verify(card);
        compare(card.color, data.color);
    }

    function test_clicking_the_row_toggles_expanded() {
        let card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        mouseClick(card, 20, 20);
        compare(card.expanded, true);
        // The layout re-measures on the next polish pass, not synchronously
        // with the click.
        tryVerify(function () {
            return card.height > 44;
        }, 1000, "the expanded card must grow past the collapsed row");

        mouseClick(card, 20, 20);
        compare(card.expanded, false);
    }

    function test_occurrences_render_one_link_each_and_open_at_their_time() {
        let card = createTemporaryObject(cardComponent, testCase, {
            expanded: true,
            occurrences: [
                { startMs: 151000, text: "02:31" },
                { startMs: 468000, text: "07:48" },
                { startMs: 714000, text: "11:54" }
            ]
        });
        verify(card);

        let links = findLinks(card);
        compare(links.length, 3);
        compare(links[0].text, "02:31");

        let openedAt = -1;
        card.openAtRequested.connect(function (startMs) { openedAt = startMs; });
        mouseClick(links[1], 5, 5);
        compare(openedAt, 468000);
    }

    // findChild only ever returns the first match, so occurrence links (one
    // per occurrence, same objectName) are collected by hand.
    function findLinks(card) {
        let matches = [];
        collect(card, matches);
        return matches;
    }

    function collect(item, matches) {
        for (let i = 0; i < item.children.length; ++i) {
            let child = item.children[i];
            if (child.objectName === "ledgerOccurrenceLink")
                matches.push(child);
            collect(child, matches);
        }
    }
}

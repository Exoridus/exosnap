import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

TestCase {
    id: testCase

    name: "ExoLastSessionCard"
    when: windowShown
    width: 900
    height: 700
    visible: true

    Component {
        id: cardComponent

        ExoLastSessionCard {
            width: 860
            session: {
                "headerText": "Recording saved · 2 problems observed",
                "fileName": "exosnap-2026-09-05-154112.mkv",
                "durationMs": 754000,
                "mediaDurationMs": 750000,
                "facts": [
                    { "label": "Frames dropped", "value": "0", "valueTone": "ok", "sub": "45 240 captured" },
                    { "label": "Achieved", "value": "59.98 fps", "valueTone": "neutral", "sub": "target 60 fps" },
                    { "label": "A/V drift", "value": "+0.4 ms", "valueTone": "neutral", "sub": "peak 1.7 ms" },
                    { "label": "File", "value": "Valid", "valueTone": "ok", "sub": "MKV · AV1 · Opus · 1.9 GB" }
                ],
                "marks": [],
                "ledgerEntries": [
                    {
                        "entryId": "rec.001",
                        "title": "Judder: present cadence is irregular",
                        "summary": "Present jitter above 8 ms.",
                        "active": false,
                        "count": 3,
                        "firstSeenText": "15:43:12",
                        "lastSeenText": "15:53:05",
                        "worstText": "11.4 ms",
                        "budgetText": "8 ms",
                        "totalActiveText": "6.5 s"
                    },
                    {
                        "entryId": "rec.gpu.contention",
                        "title": "Encoder is running late",
                        "summary": "The encoder needs more than its frame budget.",
                        "active": false,
                        "count": 1,
                        "firstSeenText": "15:52:40",
                        "lastSeenText": "15:52:40",
                        "worstText": "13.9 ms",
                        "budgetText": "16.67 ms",
                        "totalActiveText": "0 s"
                    }
                ]
            }
        }
    }

    function test_header_names_the_file_and_the_problem_count() {
        let card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        let header = findChild(card, "lastSessionHeader");
        verify(header);
        compare(header.text, "Recording saved · 2 problems observed");
    }

    function test_exactly_four_facts_render() {
        let card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        let facts = findChild(card, "lastSessionFacts");
        verify(facts);
        let repeater = findRepeater(facts);
        verify(repeater);
        compare(repeater.count, 4);
    }

    function test_the_narrow_caller_gets_two_columns() {
        let card = createTemporaryObject(cardComponent, testCase, { columns: 2 });
        verify(card);
        let facts = findChild(card, "lastSessionFacts");
        verify(facts);
        compare(facts.columns, 2);
    }

    function test_worst_first_only_the_first_ledger_card_starts_expanded() {
        let card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        let ledger = findChild(card, "lastSessionLedger");
        verify(ledger);
        let cards = findLedgerCards(ledger);
        compare(cards.length, 2);
        compare(cards[0].expanded, true);
        compare(cards[1].expanded, false);
    }

    // The outer frame is given the layout's height, and the layout sits inside it
    // at cardPadding on every side. Sizing the frame to the bare content height
    // compressed the column below its implicit height and clipped the action row
    // off the bottom.
    function test_the_frame_is_tall_enough_for_its_own_padding() {
        let card = createTemporaryObject(cardComponent, testCase);
        verify(card);
        let actions = findChild(card, "lastSessionActions");
        verify(actions);
        let frame = card.children[0];
        verify(frame);
        verify(actions.y + actions.height <= frame.height - ExoTheme.cardPadding + 1,
               "the action row (" + (actions.y + actions.height) + ") is clipped by the frame ("
               + frame.height + ")");
    }

    function findRepeater(item) {
        for (let i = 0; i < item.children.length; ++i) {
            if (item.children[i].count !== undefined)
                return item.children[i];
        }
        return null;
    }

    function findLedgerCards(item) {
        let matches = [];
        for (let i = 0; i < item.children.length; ++i) {
            let child = item.children[i];
            if (child.entryId !== undefined)
                matches.push(child);
        }
        return matches;
    }
}

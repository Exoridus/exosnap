import QtQuick
import QtTest

import ExoSnap.Quick.TestControls

// QCR-508: the banner's tone reached the user through a border colour and a
// ground tint and nothing else. These pin the two non-colour channels — the
// glyph and the accessible name — and that they agree with each other.
TestCase {
    id: testCase

    name: "ExoNotice"
    when: windowShown
    width: 400
    height: 160
    visible: true

    Component {
        id: noticeComponent

        ExoNotice {
            width: 360
            text: "The output folder is not writable."
        }
    }

    function test_each_tone_carries_a_glyph_and_a_word_data() {
        return [
            { tag: "warning", tone: "warning", glyph: ExoGlyph.Warning, word: "Warning" },
            { tag: "error", tone: "error", glyph: ExoGlyph.Close, word: "Error" },
            { tag: "success", tone: "success", glyph: ExoGlyph.Check, word: "Success" },
            { tag: "info", tone: "info", glyph: ExoGlyph.Info, word: "Information" }
        ];
    }

    function test_each_tone_carries_a_glyph_and_a_word(data) {
        let notice = createTemporaryObject(noticeComponent, testCase, { tone: data.tone });
        verify(notice);
        compare(notice._glyph, data.glyph);
        verify(notice.Accessible.name.indexOf(data.word) === 0,
               "accessible name '" + notice.Accessible.name + "' must start with '" + data.word + "'");
        verify(notice.Accessible.name.indexOf(notice.text) >= 0, "the sentence itself is still there");
    }

    function test_the_default_tone_is_still_warning() {
        // Callers that never named a tone keep the banner they had.
        let notice = createTemporaryObject(noticeComponent, testCase);
        verify(notice);
        compare(notice.tone, "warning");
        compare(notice._glyph, ExoGlyph.Warning);
    }

    function test_the_glyph_takes_the_readable_semantic_rung() {
        // QCR-502: the glyph sits INSIDE the tinted ground, which is the darkest
        // surface the tone is drawn on. Asserted against the token rather than a
        // literal, so a palette change moves both. Whether that rung is far
        // enough from the ground is the C++ contrast gate's job — it is the one
        // that can iterate both appearances.
        let notice = createTemporaryObject(noticeComponent, testCase, { tone: "warning" });
        verify(notice);
        compare(notice._glyphInk, ExoTheme.warningText);

        let failure = createTemporaryObject(noticeComponent, testCase, { tone: "error" });
        verify(failure);
        compare(failure._glyphInk, ExoTheme.errorText);
    }
}

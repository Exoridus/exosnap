import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

Item {
    id: root
    width: 860
    height: 600

    Component {
        id: pageComponent
        EditPage {
            width: root.width
            height: root.height
            session: testPageSession
            timeline: testPageTimeline
            player: testPagePlayer
            exporter: testPageExporter
            recordings: testPageRecordings
        }
    }

    TestCase {
        name: "EditPage"
        when: windowShown

        function make(properties) {
            const page = createTemporaryObject(pageComponent, root, properties || {});
            verify(!!page, "Component exists");
            return page;
        }

        function child(page, name) {
            const item = findChild(page, name);
            verify(!!item, "Object exists");
            return item;
        }

        function test_icon_actions_data() {
            return [
                { tag: "jump", objectName: "editJumpStart", label: qsTr("Jump to start"), shortcut: "Home", enabled: false },
                { tag: "back", objectName: "editSeekBackward", label: qsTr("Back one second"), shortcut: "Shift+Left", enabled: false },
                { tag: "play", objectName: "editTransportPlay", label: qsTr("Play"), shortcut: "Space", enabled: false },
                { tag: "undo", objectName: "editUndo", label: qsTr("Undo"), shortcut: "Ctrl+Z", enabled: false },
                { tag: "redo", objectName: "editRedo", label: qsTr("Redo"), shortcut: "Ctrl+Y", enabled: false },
                { tag: "split", objectName: "editSplit", label: qsTr("Split"), shortcut: "Ctrl+B", enabled: false },
                { tag: "delete", objectName: "editDelete", label: qsTr("Delete"), shortcut: "Delete", enabled: false },
                { tag: "ripple", objectName: "editRippleDelete", label: qsTr("Ripple delete"), shortcut: "Shift+Delete", enabled: false },
                { tag: "snap", objectName: "editSnapping", label: qsTr("Snapping"), shortcut: "", enabled: true },
                { tag: "export", objectName: "editExport", label: qsTr("Export"), shortcut: "", enabled: false }
            ];
        }

        function test_icon_actions(data) {
            const page = make();
            const action = child(page, data.objectName);
            compare(action.Accessible.name, data.label);
            compare(action.enabled, data.enabled);
            verify(action.glyph !== ExoGlyph.Invalid);
            compare(action.width, action.height);
            verify(action.ToolTip.text.indexOf(data.label) >= 0);
            if (data.shortcut.length > 0)
                verify(action.ToolTip.text.indexOf(data.shortcut) >= 0);
            const position = action.mapToItem(page, 0, 0);
            verify(position.x >= 0 && position.x + action.width <= page.width);
            verify(position.y >= 0 && position.y + action.height <= page.height);
        }

        function test_empty_sources_and_keyboard_tabs() {
            const page = make();
            const history = child(page, "editHistoryTab");
            const media = child(page, "editMediaTab");
            const transitions = child(page, "editTransitionsTab");
            const title = child(page, "editSourceEmptyTitle");
            const help = child(page, "editSourceEmptyHelp");
            const add = child(page, "editAddMedia");
            compare(title.text, qsTr("No recordings yet"));
            compare(help.text, qsTr("Recent recordings appear here automatically."));
            verify(title.visible);
            compare(add.visible, false);

            history.forceActiveFocus(Qt.TabFocusReason);
            verify(history.visualFocus);
            keyClick(Qt.Key_Tab);
            tryCompare(media, "activeFocus", true);
            keyClick(Qt.Key_Space);
            tryCompare(title, "text", qsTr("Drop media here"));
            compare(add.visible, true);
            verify(add.Accessible.name.length > 0);
            verify(add.glyph !== ExoGlyph.Invalid);
            keyClick(Qt.Key_Tab);
            tryCompare(transitions, "activeFocus", true);
            keyClick(Qt.Key_Space);
            tryCompare(title, "text", qsTr("Transitions will appear here when available."));
            compare(add.visible, false);
            compare(child(page, "editSourceItems").count, 0);
            keyClick(Qt.Key_Right);
            tryCompare(history, "activeFocus", true);
            compare(history.visualFocus, true);
            compare(title.text, qsTr("No recordings yet"));
            keyClick(Qt.Key_Left);
            tryCompare(transitions, "activeFocus", true);
            compare(transitions.visualFocus, true);
            compare(transitions.Accessible.role, Accessible.PageTab);
            compare(title.text, qsTr("Transitions will appear here when available."));
        }

        function test_snapping_keyboard_toggle() {
            const page = make();
            const snap = child(page, "editSnapping");
            const timeline = child(page, "editClipTimeline");
            snap.forceActiveFocus(Qt.TabFocusReason);
            compare(snap.checked, true);
            keyClick(Qt.Key_Space);
            compare(timeline.snapping, false);
            compare(snap.checked, false);
            keyClick(Qt.Key_Space);
            compare(timeline.snapping, true);
            compare(snap.checked, true);
        }

        function test_hidden_resident_page_retains_source_and_zoom() {
            const page = make();
            const media = child(page, "editMediaTab");
            const zoom = child(page, "editZoom");
            const timeline = child(page, "editClipTimeline");
            media.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            zoom.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Right);
            verify(timeline.pixelsPerSecond > 40);
            const zoomBefore = timeline.pixelsPerSecond;
            page.visible = false;
            page.visible = true;
            compare(timeline.pixelsPerSecond, zoomBefore);
            compare(child(page, "editSourceEmptyTitle").text, qsTr("Drop media here"));
        }

        function test_transport_and_undo_actions() {
            while (testSession.canUndo)
                testSession.undo();
            testSession.appendAsset(testSession.media[0].id);
            const page = make({ session: testSession, timeline: testTimeline, player: testPlayer });
            verify(testSession.durationMs > 0);
            compare(testPlayer.clipOpen, false);
            compare(child(page, "editTransportPlay").enabled, false);
            page.player = testTransportPlayer;
            compare(testTransportPlayer.clipOpen, true);
            compare(child(page, "editTransportPlay").enabled, true);
            page.player = testPlayer;
            const backward = child(page, "editSeekBackward");
            const jump = child(page, "editJumpStart");
            testSession.requestSeek(5000);
            backward.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            compare(testSession.positionMs, 4000);
            jump.forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            compare(testSession.positionMs, 0);
            compare(jump.enabled, false);
            compare(backward.enabled, false);

            child(page, "editUndo").forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            compare(testSession.durationMs, 0);
            child(page, "editRedo").forceActiveFocus(Qt.TabFocusReason);
            keyClick(Qt.Key_Space);
            compare(testSession.durationMs, 100000);
            while (testSession.canUndo)
                testSession.undo();
        }
    }
}

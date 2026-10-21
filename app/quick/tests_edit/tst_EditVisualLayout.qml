import QtQuick
import QtQuick.Controls
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: tests
    name: "EditVisualLayout"
    when: windowShown
    width: 1920
    height: 1080
    visible: true

    Component {
        id: pageComponent
        EditPage {
            width: tests.width
            height: tests.height
            session: testSession
            timeline: testTimeline
            player: testPlayer
            exporter: testPageExporter
            recordings: testPageRecordings
            shell: testPageShell
            mediaPinPreferred: false
            detailsPinPreferred: false
            Rectangle { anchors.fill: parent; z: -1; color: ExoTheme.background }
        }
    }
    function luminance(color) {
        const channels = [color.r, color.g, color.b].map(value =>
            value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4));
        return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2];
    }
    function contrast(first, second) {
        const a = luminance(first);
        const b = luminance(second);
        return (Math.max(a, b) + 0.05) / (Math.min(a, b) + 0.05);
    }
    function child(parent, name) {
        const item = findChild(parent, name);
        verify(!!item, name);
        return item;
    }
    function init() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
        testSession.appendAsset(testSession.media[0].id);
    }
    function cleanup() {
        testSession.cancelClipDrag();
        while (testSession.canUndo) testSession.undo();
        verify(testEditHarness.setVisualAppearance("dark"));
    }
    function test_composited_clip_text_and_essential_marks_data() {
        return [{tag: "dark", appearance: "dark"}, {tag: "light", appearance: "light"}];
    }
    function test_composited_clip_text_and_essential_marks(data) {
        verify(testEditHarness.setVisualAppearance(data.appearance));
        const page = createTemporaryObject(pageComponent, tests);
        const timeline = child(page, "editClipTimeline");
        timeline.view.setVisibleInterval(0, 10000000);
        waitForRendering(page);
        const rows = testSession.visibleClipsUs(0, 10000000);
        for (const row of rows) {
            const clip = child(page, "editClip" + row.id);
            const header = child(clip, "editClipHeader" + row.id);
            const label = child(clip, "editClipName" + row.id);
            for (const colour of ["#ffffff", "#000000", "#ff0000", "#00ff00", "#0000ff", "#ffff00"]) {
                testSession.trackLayout.setTrackColor(row.trackId, colour);
                testSession.clearSelection();
                verify(contrast(label.color, header.color) >= 4.5, "Unselected name " + colour);
                testSession.selectClip(row.id);
                verify(contrast(label.color, header.color) >= 4.5, "Selected name " + colour);
                verify(contrast(ExoTheme.accent, clip.color) >= 3, "Essential marks " + colour);
                verify(contrast(ExoTheme.textMuted, clip.color) >= 3, "Waveform marks " + colour);
                compare(clip.opacity, 1);
            }
        }
    }
    function test_harness_rejects_missing_targets_and_checks_routed_panels() {
        const page = createTemporaryObject(pageComponent, tests);
        verify(!page.applyHarnessView({popup: "misspelled"}));
        verify(!page.applyHarnessView({popup: false}));
        verify(!page.applyHarnessView({popup: "crossfade"}));
        verify(!page.applyHarnessView({popup: "source-audio"}));
        verify(!page.applyHarnessView({sourceTab: 3}));
        verify(!page.applyHarnessView({viewFromUs: 5000, viewToUs: 1000}));
        verify(page.applyHarnessView({fit: true}));
        testPageShell.editSourceTabRequested(1);
        testPageShell.editPanelRequested(0, true, false);
        verify(page.applyHarnessView({media: "open", sourceTab: 1}));
        compare(page.sourceTab, 1);
        verify(page.mediaOpen);
        verify(!page.applyHarnessView({sourceTab: 0}));
    }
    function test_fonts_are_the_shipped_faces() {
        for (const font of [ExoTheme.sansFont, ExoTheme.sansMediumFont, ExoTheme.sansSemiBoldFont, ExoTheme.sansBoldFont, ExoTheme.monoFont, ExoTheme.monoMediumFont])
            compare(font.status, FontLoader.Ready);
        verify(ExoTheme.sansFamily !== "Segoe UI");
        verify(ExoTheme.monoFamily !== "Consolas");
    }
    function test_initial_preview_placeholder_uses_the_loaded_catalogue() {
        compare(testPlayer.placeholderText, testVisualLanguage === "de" ? "Vorschau nicht verfügbar" : "Preview unavailable");
    }
    function test_responsive_editor_data() {
        const cases = [];
        for (const appearance of ["dark", "light"])
            for (const width of [860, 1280, 1600, 1920])
                for (const height of [600, 1080])
                    cases.push({tag: appearance + "-" + width + "x" + height, appearance: appearance, width: width, height: height});
        return cases;
    }
    function test_responsive_editor(data) {
        verify(testEditHarness.setVisualAppearance(data.appearance));
        tests.width = data.width;
        tests.height = data.height;
        for (const track of testSession.tracks)
            verify(testSession.setTrackName(track.id, "Aufnahmespur mit besonders langer deutscher Bezeichnung für vollständige Bildschirmaufnahmen"));
        const page = createTemporaryObject(pageComponent, tests);
        verify(!!page);
        page.applyHarnessView({fit: true});
        waitForRendering(page);
        for (const name of ["editViewerStage", "editTimelineZone", "editTimelineHeader", "editTimelineFooter", "editSelectionStatus"]) {
            const item = child(page, name);
            const point = item.mapToItem(page, 0, 0);
            verify(point.x >= -1 && point.x + item.width <= page.width + 1, name + " horizontal bounds");
            verify(point.y >= -1 && point.y + item.height <= page.height + 1, name + " vertical bounds: " + point.y + " + " + item.height + " / " + page.height);
        }
        verify(child(page, "editViewerStage").height >= 160);
        for (const name of ["editTransportPlay", "editMediaAccess", "editDetailsAccess", "editExport", "editZoomIn", "editZoomOut"]) {
            const control = child(page, name);
            verify(control.Accessible.name.length > 0, name + " accessible name");
            if (!control.visible) continue;
            const point = control.mapToItem(page, 0, 0);
            verify(point.x >= 0 && point.x + control.width <= page.width + 1, name + " horizontal bounds");
            verify(point.y >= 0 && point.y + control.height <= page.height + 1, name + " vertical bounds: " + point.y + " + " + control.height + " / " + page.height);
        }
        child(page, "editDetailsAccess").forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        tryCompare(page, "detailsOpen", true);
        verify(child(page, "editDetailsPanel").visible);
        if (testVisualLanguage === "de")
            compare(child(page, "editMediaAccess").Accessible.name, "Medien");
        if (testVisualDirectory.length > 0) {
            let saved = false;
            const file = testVisualDirectory + "/editor-" + data.tag + "-dpr" + testScale + ".png";
            verify(page.grabToImage(result => { saved = result.saveToFile(file); },
                Qt.size(Math.round(page.width * Number(testScale)), Math.round(page.height * Number(testScale)))));
            tryVerify(() => saved, 5000, file);
        }
        keyClick(Qt.Key_Escape);
        tryCompare(page, "detailsOpen", false);
        verify(child(page, "editDetailsAccess").activeFocus);
    }
}

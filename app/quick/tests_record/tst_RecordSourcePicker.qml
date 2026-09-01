pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import ExoSnap.Quick.RecordPickerTestControls

// The source picker's behavioural contract: three tabs, a Windows tab that
// reports its count, filters by search and scrolls visibly when it overflows,
// full-card selection confirmed by Enter or a double click, a fixed footer with
// Cancel and the named confirm action, an accessible refresh control, and
// cards that reflow from two columns to one at the narrow layout. The Region
// tab lists Draw custom first, then the aspect presets, and confirms into
// region mode through the adapter's preset boundary.
TestCase {
    id: testCase

    name: "RecordSourcePicker"
    when: windowShown
    width: 860
    height: 700
    visible: true

    Component {
        id: pageComponent

        Item {
            id: page

            property alias picker: picker

            width: 860
            height: 700

            RecordSourcePicker {
                id: picker

                recordViewModel: recordDriver.adapter
            }
        }
    }

    SignalSpy {
        id: selectSpy
        signalName: "selectTargetRequested"
    }

    SignalSpy {
        id: presetSpy
        signalName: "regionPresetRequested"
    }

    SignalSpy {
        id: stillSpy
        signalName: "visibleTargetIdentitiesChanged"
    }

    function init() {
        recordDriver.seedTargets(2, ["Claude Design - Brave", "Task Manager"]);
        selectSpy.target = recordDriver.adapter;
        presetSpy.target = recordDriver.adapter;
        stillSpy.target = recordDriver.adapter;
        selectSpy.clear();
        presetSpy.clear();
        stillSpy.clear();
    }

    function lastPublished() {
        return stillSpy.count === 0 ? [] : stillSpy.signalArguments[stillSpy.count - 1][0];
    }

    function makePage() {
        let page = createTemporaryObject(pageComponent, testCase, {});
        verify(page, "Component exists");
        page.picker.open();
        waitForRendering(page);
        return page;
    }

    // TestCase.findChild follows QObject parents, but view delegates are only
    // on the visual tree, so the lookup walks childItems instead.
    function findVisual(obj, name) {
        if (obj.objectName === name)
            return obj;
        const kids = obj.children !== undefined ? obj.children : [];
        for (let i = 0; i < kids.length; ++i) {
            const found = findVisual(kids[i], name);
            if (found)
                return found;
        }
        return null;
    }

    function pick(page, name) {
        const item = findVisual(page.picker.contentItem, name);
        verify(!!item, "Object exists");
        return item;
    }

    function showTab(page, index) {
        pick(page, "tabs").selected(index);
        waitForRendering(page);
    }

    function test_three_tabs_switch_pages() {
        let page = makePage();
        const tabs = pick(page, "tabs");
        compare(tabs.options.length, 3);
        compare(tabs.options[0], "Displays");
        compare(tabs.options[1], "Windows");
        compare(tabs.options[2], "Region");
        compare(page.picker.currentTab, 0);
        verify(pick(page, "displaysPage").visible);

        showTab(page, 1);
        verify(!pick(page, "displaysPage").visible);
        verify(pick(page, "windowsPage").visible);

        showTab(page, 2);
        verify(!pick(page, "windowsPage").visible);
        verify(pick(page, "regionPage").visible);
        compare(page.picker.currentTab, 2);
    }

    // ExoSearchField is a frameless rectangle over its inner TextField, so the
    // typed input has to go to that field -- what a click or Tab lands on.
    function focusInnerTextField(container) {
        for (let i = 0; i < container.children.length; ++i) {
            const child = container.children[i];
            if (child.toString().indexOf("TextField") !== -1) {
                child.forceActiveFocus();
                return child;
            }
            const found = focusInnerTextField(child);
            if (found)
                return found;
        }
        return null;
    }

    function test_windows_tab_reports_count_and_search_filters() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager", "Steam Library", "Notepad", "Terminal"]);
        showTab(page, 1);
        const count = pick(page, "windowsCount");
        compare(count.text, "5 windows");
        const grid = pick(page, "windowsGrid");
        compare(grid.count, 5);

        const search = pick(page, "windowSearch");
        verify(!!focusInnerTextField(search), "Object exists");
        keyClick(Qt.Key_T);
        keyClick(Qt.Key_A);
        keyClick(Qt.Key_S);
        keyClick(Qt.Key_K);
        tryCompare(grid, "count", 1);
        compare(count.text, "1 window");
    }

    function test_window_list_shows_a_scrollbar_only_when_it_overflows() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Window 1", "Window 2", "Window 3", "Window 4", "Window 5", "Window 6",
            "Window 7", "Window 8", "Window 9", "Window 10", "Window 11", "Window 12", "Window 13", "Window 14",
            "Window 15"]);
        showTab(page, 1);
        const grid = pick(page, "windowsGrid");
        const bar = findChild(page.picker, "windowsScrollBar");
        verify(!!bar, "Object exists");
        tryCompare(grid, "count", 15);
        tryCompare(bar.contentItem, "visible", true);
        verify(bar.size < 1.0, "an overflowing list reports a fractional scroll extent");

        recordDriver.seedTargets(1, ["Brave", "Task Manager"]);
        waitForRendering(page);
        tryCompare(grid, "count", 2);
        tryCompare(bar.contentItem, "visible", false);
    }

    function test_card_click_selects_and_footer_confirm_commits() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager"]);
        showTab(page, 1);
        const card = pick(page, "targetCard-window:100");
        verify(!card.pending);

        mouseClick(card);
        waitForRendering(page);
        verify(card.pending, "clicking a card selects it");
        verify(pick(page, "confirmButton").enabled);

        mouseClick(pick(page, "confirmButton"));
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][0], 1);
        compare(selectSpy.signalArguments[0][1], 1);
        tryCompare(page.picker, "opened", false);
    }

    function test_enter_confirms_the_focused_card() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager"]);
        showTab(page, 1);
        const card = pick(page, "targetCard-window:100");

        card.forceActiveFocus();
        keyClick(Qt.Key_Return);
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][0], 1);
        compare(selectSpy.signalArguments[0][1], 1);
        tryCompare(page.picker, "opened", false);
    }

    function test_double_click_confirms() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager"]);
        showTab(page, 1);
        const card = pick(page, "targetCard-window:100");

        mouseDoubleClickSequence(card);
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][0], 1);
        compare(selectSpy.signalArguments[0][1], 1);
        tryCompare(page.picker, "opened", false);
    }

    function test_cancel_closes_without_committing() {
        let page = makePage();
        mouseClick(pick(page, "cancelButton"));
        tryCompare(page.picker, "opened", false);
        compare(selectSpy.count, 0);
        compare(presetSpy.count, 0);
    }

    function test_open_publishes_the_visible_targets_in_layout_order() {
        let page = makePage();
        // The Displays tab is the one on screen, so only its two cards are
        // published -- the windows behind the second tab cost nothing until the
        // user goes there.
        tryVerify(() => lastPublished().length === 2);
        compare(lastPublished(), ["display:1", "display:2"]);

        page.picker.currentTab = 1;
        tryVerify(() => lastPublished()[0] === "window:100");
        compare(lastPublished(), ["window:100", "window:101"]);

        // A closed picker captures nothing at all.
        page.picker.close();
        tryVerify(() => lastPublished().length === 0);
    }

    function test_a_stale_still_keeps_its_image_and_its_geometry() {
        let page = makePage();
        recordDriver.deliverStill("display:2", "image://capture-target/display-2/1");
        // Re-picked after every change: republishing the option rows rebuilds
        // the delegates, so a card captured once is a stale object handle.
        const state = () => pick(page, "targetCard-display:2").modelData.thumbnailState;
        const box = pick(page, "displaysGrid").cellHeight;
        tryVerify(() => state() === "ready");

        recordDriver.failStill("display:2");
        tryVerify(() => state() === "stale");
        // The still survives the loss: nothing reverts to the placeholder glyph,
        // and the card does not resize under it.
        compare(pick(page, "targetCard-display:2").modelData.thumbnailSource,
                "image://capture-target/display-2/1");
        compare(pick(page, "displaysGrid").cellHeight, box);
    }

    function test_cards_reflow_from_two_columns_to_one() {
        let page = makePage();
        recordDriver.seedTargets(2, ["Claude Design - Brave"]);
        const grid = pick(page, "displaysGrid");
        compare(page.picker.pickerColumns, 2);
        verify(grid.cellWidth < grid.width / 2 + 1, "two columns split the grid width");

        compare(page.picker.columnsForWidth(520), 2);
        compare(page.picker.columnsForWidth(519), 1);
        compare(page.picker.columnsForWidth(400), 1);
    }

    function test_region_tab_lists_draw_custom_first_then_the_aspect_presets() {
        let page = makePage();
        showTab(page, 2);
        const keys = page.picker.regionPresetRows.map(row => row.key);
        compare(keys.length, 5);
        compare(keys[0], "custom");
        compare(keys[1], "16:9");
        compare(keys[2], "9:16");
        compare(keys[3], "1:1");
        compare(keys[4], "4:5");

        verify(pick(page, "presetCard-custom").emphasized);
        verify(!pick(page, "presetCard-16:9").emphasized);
        verify(pick(page, "regionCaption").text.includes("Region on Display 1"));
    }

    function test_region_preset_confirm_enters_region_mode_with_the_preset() {
        let page = makePage();
        showTab(page, 2);
        const card = pick(page, "presetCard-9:16");

        mouseClick(card);
        waitForRendering(page);
        verify(card.pendingPreset, "clicking a preset selects it");

        mouseClick(pick(page, "confirmButton"));
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][0], 0);
        compare(selectSpy.signalArguments[0][1], 2);
        tryCompare(presetSpy, "count", 1);
        compare(presetSpy.signalArguments[0][0], "9:16");
        tryCompare(page.picker, "opened", false);
    }

    function test_draw_custom_confirms_the_custom_preset_key() {
        let page = makePage();
        showTab(page, 2);
        mouseClick(pick(page, "presetCard-custom"));
        waitForRendering(page);

        mouseClick(pick(page, "confirmButton"));
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][1], 2);
        tryCompare(presetSpy, "count", 1);
        compare(presetSpy.signalArguments[0][0], "custom");
        tryCompare(page.picker, "opened", false);
    }
}

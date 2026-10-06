pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import ExoSnap.Quick.RecordPickerTestControls

// The source picker's behavioural contract: three tabs, a Windows tab with
// search, count and scroll, full-card selection confirmed by Enter or a double
// click, a fixed footer with Cancel and the named confirm action, cards that
// reflow across the 1-4 column breakpoints, and a pending selection carried by
// source identity rather than the list index. The Region tab lists Draw custom
// first, then the aspect presets, and confirms into region mode through the
// adapter's preset boundary.
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
                hostPage: page
            }
        }
    }

    Component {
        id: regionSelectorComponent

        Item {
            property alias overlay: regionOverlay
            width: 800
            height: 600

            Rectangle {
                anchors.fill: parent
                color: "white"
            }

            RegionSelectionOverlay {
                id: regionOverlay
                recordViewModel: recordDriver.adapter
                sourcePixelSize: Qt.size(1920, 1080)
                anchors.fill: parent
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
        const title = pick(page, "pickerTitle");
        verify(tabs.mapToItem(page.picker.contentItem, 0, 0).x >
               title.mapToItem(page.picker.contentItem, title.width, 0).x);
        const header = pick(page, "pickerHeader");
        verify(tabs.mapToItem(header, tabs.width, 0).x < header.width);
        verify(header.width > page.picker.contentItem.width * 0.9);
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

    function test_windows_tab_reports_count_and_search() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager", "Steam Library", "Notepad", "Terminal"]);
        showTab(page, 1);
        const count = pick(page, "windowsCount");
        compare(count.text, "Windows: 5");
        const card = pick(page, "targetCard-window:100");
        compare(card.primaryLabel, "Claude Design");
        compare(card.secondaryLabel, "Brave");
        const thumbnail = findVisual(card, "targetThumbnail");
        verify(thumbnail.width <= card.width);
        compare(Math.round(thumbnail.width * 9 / 16), Math.round(thumbnail.height));
        compare(thumbnail.border.width, 0);
        const grid = pick(page, "windowsGrid");
        compare(grid.count, 5);
        compare(grid.height % grid.cellHeight, 0);

        verify(findVisual(page.picker.contentItem, "windowSearch"));
        compare(page.picker.windowRows.length, 5);
    }

    // The search narrows the grid and the count, but the dialog's own height is
    // reserved from the unfiltered list: typing must not make the picker jump.
    function test_window_search_filters_by_title_and_app() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager", "Steam Library"]);
        showTab(page, 1);
        const grid = pick(page, "windowsGrid");
        const heightBefore = page.picker.height;

        page.picker.windowQuery = "task";
        tryCompare(grid, "count", 1);
        compare(pick(page, "targetCard-window:101").primaryLabel, "Task Manager");
        compare(pick(page, "windowsCount").text, "Windows: 1");
        compare(page.picker.height, heightBefore);

        page.picker.windowQuery = "brave";
        tryCompare(grid, "count", 1);
        compare(pick(page, "targetCard-window:100").secondaryLabel, "Brave");

        page.picker.windowQuery = "no such window";
        tryCompare(grid, "count", 0);
        compare(pick(page, "windowsEmptyState").text, "No windows match your search.");
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

    // The pending choice follows the source's identity, not the list index.
    // A rescan that inserts a window before it must leave the same card
    // selected and commit to the source that was clicked.
    function test_pending_selection_survives_a_list_insert() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager"]);
        showTab(page, 1);
        mouseClick(pick(page, "targetCard-window:101"));
        waitForRendering(page);
        verify(pick(page, "targetCard-window:101").pending);

        recordDriver.seedWindowTargets([
            { id: 99, label: "New Window - Fresh" },
            { id: 100, label: "Claude Design - Brave" },
            { id: 101, label: "Task Manager" }
        ]);
        waitForRendering(page);
        tryVerify(() => pick(page, "targetCard-window:101") !== null);
        verify(pick(page, "targetCard-window:101").pending, "the identical source stays selected");
        verify(pick(page, "confirmButton").enabled);

        mouseClick(pick(page, "confirmButton"));
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][0], 2, "commits the re-resolved index");
        compare(selectSpy.signalArguments[0][1], 1);
    }

    // A source that disappears leaves no replacement selection: the confirm
    // action locks and the footer says why.
    function test_pending_selection_that_disappears_blocks_commit() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager"]);
        showTab(page, 1);
        mouseClick(pick(page, "targetCard-window:100"));
        waitForRendering(page);
        verify(pick(page, "confirmButton").enabled);

        recordDriver.seedWindowTargets([{ id: 101, label: "Task Manager" }]);
        waitForRendering(page);
        verify(!pick(page, "confirmButton").enabled, "a vanished source cannot be confirmed");
        compare(pick(page, "windowsCount").text, "Selected source is no longer available.");

        mouseClick(pick(page, "confirmButton"));
        waitForRendering(page);
        compare(selectSpy.count, 0);
    }

    // The confirm action belongs to the tab in front of the user: a display
    // picked earlier is not committed from the Windows tab.
    function test_tab_switch_does_not_commit_the_other_tabs_pending_selection() {
        let page = makePage();
        // init() seeded two displays and selected display 1.
        showTab(page, 1);
        verify(!pick(page, "confirmButton").enabled);
        mouseClick(pick(page, "confirmButton"));
        waitForRendering(page);
        compare(selectSpy.count, 0);

        mouseClick(pick(page, "targetCard-window:100"));
        waitForRendering(page);
        verify(pick(page, "confirmButton").enabled);
        mouseClick(pick(page, "confirmButton"));
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][0], 2);
        compare(selectSpy.signalArguments[0][1], 1);
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
        const card = pick(page, "targetCard-display:2");
        recordDriver.deliverStill("display:2", "image://capture-target/display-2/1");
        const state = () => card.thumbnailState;
        tryVerify(() => pick(page, "targetCard-display:2") === card);
        const box = pick(page, "displaysGrid").cellHeight;
        tryVerify(() => state() === "ready");

        recordDriver.failStill("display:2");
        tryVerify(() => state() === "stale");
        // The still survives the loss: nothing reverts to the placeholder glyph,
        // and the card does not resize under it.
        compare(card.thumbnailSource, "image://capture-target/display-2/1");
        verify(pick(page, "targetCard-display:2") === card);
        compare(findVisual(card, "targetThumbnailImage").fillMode, Image.PreserveAspectFit);
        compare(pick(page, "displaysGrid").cellHeight, box);
    }

    function test_columns_follow_the_one_to_four_column_breakpoints() {
        let page = makePage();
        recordDriver.seedTargets(2, ["Claude Design - Brave"]);
        const grid = pick(page, "displaysGrid");
        // 860x700 host: the dialog is 720 wide, 672 of content, so two columns.
        compare(page.picker.pickerColumns, 2);
        compare(page.picker.displayColumns, 2, "two displays use two columns");
        compare(grid.cellWidth, page.picker.displaysCardWidth + 16);
        compare(grid.width, 2 * page.picker.displaysCardWidth + 32);
        verify(page.picker.displaysCardWidth <= 420);

        compare(page.picker.columnsForWidth(300), 1);
        compare(page.picker.columnsForWidth(655), 1);
        compare(page.picker.columnsForWidth(656), 2);
        compare(page.picker.columnsForWidth(991), 2);
        compare(page.picker.columnsForWidth(992), 3);
        compare(page.picker.columnsForWidth(1327), 3);
        compare(page.picker.columnsForWidth(1328), 4);
        compare(page.picker.columnsForWidth(4000), 4);
    }

    // A single display must not stretch to the whole dialog: the card caps at
    // 420 and the grid centres inside the content area.
    function test_few_displays_keep_cards_compact_and_centred() {
        let page = makePage();
        recordDriver.seedTargets(1, []);
        waitForRendering(page);
        const grid = pick(page, "displaysGrid");
        compare(page.picker.displayColumns, 1);
        compare(page.picker.displaysCardWidth, 420);
        verify(grid.width < page.picker.gridContentWidth);
        compare(grid.x + page.picker.displaysCardWidth / 2, Math.round(grid.parent.width / 2));
        const card = pick(page, "targetCard-display:1");
        compare(card.primaryLabel, "Display 1");
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

    function test_region_selector_uses_monitor_pixels_and_keeps_preset() {
        const selectorHost = createTemporaryObject(regionSelectorComponent, testCase, {});
        verify(selectorHost);
        const selector = selectorHost.overlay;

        recordDriver.adapter.requestRegionPreset("16:9");
        verify(selector.selectionNormalized.width > 0);
        const preset = selector.selectionNormalized;
        const physicalAspect = preset.width * selector.sourcePixelSize.width
                               / (preset.height * selector.sourcePixelSize.height);
        verify(Math.abs(physicalAspect - 16 / 9) < 0.001);
        selector.visible = false;
        selector.visible = true;
        compare(selector.selectionNormalized, preset);

        recordDriver.adapter.requestRegionPreset("9:16");
        const portrait = selector.selectionNormalized;
        const portraitAspect = portrait.width * selector.sourcePixelSize.width
                               / (portrait.height * selector.sourcePixelSize.height);
        verify(Math.abs(portraitAspect - 9 / 16) < 0.001);

        selector.setSelectionEdges(0.99, 0.99, 0.99, 0.99);
        verify(selector.selectionNormalized.width >= 64 / 1920);
        verify(selector.selectionNormalized.height >= 64 / 1080);
        verify(selector.selectionNormalized.x + selector.selectionNormalized.width <= 1);
        verify(selector.selectionNormalized.y + selector.selectionNormalized.height <= 1);

        selector.setSelectionEdges(0.25, 0.25, 0.75, 0.75);
        waitForRendering(selector);
        const image = grabImage(selectorHost);
        compare(image.red(400, 300), 255);
        verify(image.red(10, 10) >= 120 && image.red(10, 10) <= 136);
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

    // The picker is a Popup, so it is reparented into the window's overlay and
    // does not go down with the scene subtree it was declared in. The shell
    // swaps destinations by switching a StackLayout child's visibility, and a
    // keyboard shortcut reaches that swap through the modal veil -- which left
    // the picker on screen over the destination the user had navigated to.
    function test_picker_closes_when_its_page_is_swapped_away() {
        let page = makePage();
        verify(page.picker.opened, "the picker opens");

        page.visible = false;
        tryCompare(page.picker, "opened", false,
                   1000, "the picker must not outlive the destination that owns it");
    }
}

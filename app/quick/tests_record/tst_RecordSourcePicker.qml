pragma ComponentBehavior: Bound

import QtQuick
import QtTest

import ExoSnap.Quick.RecordPickerTestControls

// The source picker separates source types into tabs. Selection stays
// pending until confirmation, and cards keep their identity across rescans.
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
        recordDriver.resetPickerState();
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

    function viewportTargets(page) {
        const list = pick(page, "sourceList");
        const top = list.contentY - list.originY;
        let y = 0;
        const result = [];
        for (const row of page.picker.pickerRows) {
            const height = page.picker.pickerRowHeight(row);
            if (row.kind === "cards" && y < top + list.height && y + height > top)
                for (const card of row.cards)
                    result.push(card.identity);
            y += height;
        }
        return result;
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
        tryVerify(() => findVisual(page.picker.contentItem, name) !== null);
        const item = findVisual(page.picker.contentItem, name);
        verify(!!item, "Object exists");
        return item;
    }

    function cardAt(page, targetIndex) {
        const row = recordDriver.adapter.targetOptions[targetIndex];
        verify(!!row, "Target row exists");
        tryVerify(() => findVisual(page.picker.contentItem, "targetCard-" + row.identity) !== null);
        return pick(page, "targetCard-" + row.identity);
    }

    function showTab(page, index) {
        pick(page, "tabs").selected(index);
        waitForRendering(page);
    }

    function test_tabs_only_contain_the_selected_source_type() {
        let page = makePage();
        const tabs = pick(page, "tabs");
        compare(tabs.options.length, 3);
        compare(tabs.options[0], "Displays");
        compare(tabs.options[1], "Windows");
        compare(tabs.options[2], "Region");
        const title = pick(page, "pickerTitle");
        verify(tabs.mapToItem(page.picker.contentItem, 0, 0).y >= title.mapToItem(page.picker.contentItem, 0, title.height).y);
        const header = pick(page, "pickerHeader");
        verify(tabs.mapToItem(header, tabs.width, 0).x < header.width);
        verify(header.width > page.picker.contentItem.width * 0.9);
        compare(page.picker.currentTab, 0);
        verify(pick(page, "sourceList").visible);
        verify(pick(page, "displaySectionLabel").visible);

        showTab(page, 1);
        verify(pick(page, "windowSectionLabel").visible);
        verify(page.picker.pickerRows.every(row => row.section === "window"));

        showTab(page, 2);
        verify(page.picker.pickerRows.every(row => row.section === "region"));
        verify(pick(page, "regionSectionLabel").visible);
        verify(pick(page, "presetCard-custom").visible);
        compare(page.picker.currentTab, 2);
    }

    function test_windows_tab_reports_count_and_search() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager", "Steam Library", "Notepad", "Terminal"]);
        showTab(page, 1);
        compare(page.picker.windowRows.length, 5);
        compare(page.picker.windowSearchMatchCount, 5);
        const card = cardAt(page, 1);
        compare(card.primaryLabel, "Claude Design");
        compare(card.secondaryLabel, "Brave");
        const thumbnail = findVisual(card, "targetThumbnail");
        verify(thumbnail.width <= card.width);
        compare(Math.round(thumbnail.width * 9 / 16), Math.round(thumbnail.height));
        compare(thumbnail.border.width, 0);
        const list = pick(page, "sourceList");
        compare(list.count, page.picker.pickerRows.length);
        verify(list.contentHeight > list.height);

        verify(findVisual(page.picker.contentItem, "windowSearch"));
        compare(page.picker.windowRows.length, 5);
    }

    // Search narrows only the window tab; the dialog stays in place.
    function test_window_search_filters_by_title_and_app() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager", "Steam Library"]);
        showTab(page, 1);
        const heightBefore = page.picker.height;

        page.picker.windowQuery = "task";
        waitForRendering(page);
        tryCompare(page.picker, "windowSearchMatchCount", 1);
        compare(page.picker.windowRows.length, 1);
        compare(cardAt(page, 2).primaryLabel, "Task Manager");
        compare(page.picker.height, heightBefore);

        page.picker.windowQuery = "brave";
        waitForRendering(page);
        tryCompare(page.picker, "windowSearchMatchCount", 1);
        compare(page.picker.windowRows.length, 1);
        compare(cardAt(page, 1).secondaryLabel, "Brave");

        page.picker.windowQuery = "no such window";
        tryCompare(page.picker, "windowSearchMatchCount", 0);
        compare(page.picker.windowRows.length, 0);
        compare(pick(page, "windowsEmptyState").text, "No windows match your search.");
    }

    function test_window_list_virtualizes_all_windows_and_scroll_extent_grows() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Window 1", "Window 2", "Window 3", "Window 4", "Window 5", "Window 6", "Window 7", "Window 8", "Window 9", "Window 10", "Window 11", "Window 12", "Window 13", "Window 14", "Window 15"]);
        showTab(page, 1);
        const list = pick(page, "sourceList");
        const bar = findChild(page.picker, "sourceScrollBar");
        verify(!!bar, "Object exists");
        compare(page.picker.windowRows.length, 15);
        tryCompare(bar.contentItem, "visible", true);
        verify(bar.size < 1.0, "an overflowing list reports a fractional scroll extent");
        const longExtent = list.contentHeight;

        recordDriver.seedTargets(1, ["Brave", "Task Manager"]);
        waitForRendering(page);
        compare(page.picker.windowRows.length, 2);
        verify(list.contentHeight < longExtent);
    }

    function test_zero_windows_keep_display_and_region_sections_available() {
        let page = makePage();
        recordDriver.seedTargets(1, []);
        showTab(page, 1);
        compare(page.picker.windowRows.length, 0);
        compare(pick(page, "windowsEmptyState").text, "No windows to capture.");
        showTab(page, 2);
        verify(pick(page, "presetCard-custom").visible);
    }

    function test_hidden_window_has_no_still_demand_until_revealed_in_viewport() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Window A", "Window B", "Window C"]);
        showTab(page, 1);
        const hiddenId = recordDriver.adapter.windowTargetOptions[1].identity;
        recordDriver.adapter.hideWindowInstance(hiddenId);
        tryCompare(page.picker, "hiddenWindowCount", 1);
        compare(page.picker.windowRows.length, 2);
        tryVerify(() => !lastPublished().includes(hiddenId));

        recordDriver.adapter.setShowHiddenWindows(true);
        tryVerify(() => page.picker.windowRows.length === 3);
        showTab(page, 1);
        tryVerify(() => lastPublished().includes(hiddenId));
        tryVerify(() => JSON.stringify(lastPublished()) === JSON.stringify(viewportTargets(page)));
    }

    function test_card_click_selects_and_footer_confirm_commits() {
        let page = makePage();
        recordDriver.seedTargets(1, ["Claude Design - Brave", "Task Manager"]);
        showTab(page, 1);
        const card = cardAt(page, 1);
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
        const card = cardAt(page, 1);

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
        const card = cardAt(page, 1);

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
        mouseClick(cardAt(page, 2));
        waitForRendering(page);
        verify(cardAt(page, 2).pending);

        recordDriver.seedWindowTargets([
            {
                id: 99,
                label: "New Window - Fresh"
            },
            {
                id: 100,
                label: "Claude Design - Brave"
            },
            {
                id: 101,
                label: "Task Manager"
            }
        ]);
        waitForRendering(page);
        tryVerify(() => cardAt(page, 2) !== null);
        verify(cardAt(page, 2).pending, "the identical source stays selected");
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
        mouseClick(cardAt(page, 1));
        waitForRendering(page);
        verify(pick(page, "confirmButton").enabled);

        recordDriver.seedWindowTargets([
            {
                id: 101,
                label: "Task Manager"
            }
        ]);
        waitForRendering(page);
        verify(!pick(page, "confirmButton").enabled, "a vanished source cannot be confirmed");
        compare(pick(page, "windowsCount").text, "Selected source is no longer available.");

        mouseClick(pick(page, "confirmButton"));
        waitForRendering(page);
        compare(selectSpy.count, 0);
    }

    // Switching tabs restores the pending choice when returning.
    function test_tab_restores_its_pending_identity() {
        let page = makePage();
        // init() seeded two displays and selected display 1.
        const display = cardAt(page, 0);
        mouseClick(display);
        waitForRendering(page);
        compare(page.picker.pendingIdentity, "display:1");
        showTab(page, 1);
        compare(page.picker.pendingIdentity, "");
        showTab(page, 0);
        compare(page.picker.pendingIdentity, "display:1");
        verify(pick(page, "confirmButton").enabled);
        mouseClick(pick(page, "confirmButton"));
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][0], 0);
        compare(selectSpy.signalArguments[0][1], 0);
    }

    function test_open_publishes_the_visible_targets_in_layout_order() {
        let page = makePage();
        tryVerify(() => lastPublished().length > 0);
        tryVerify(() => JSON.stringify(lastPublished()) === JSON.stringify(viewportTargets(page)));

        showTab(page, 1);
        tryVerify(() => lastPublished()[0] === recordDriver.adapter.windowTargetOptions[0].identity);
        tryVerify(() => JSON.stringify(lastPublished()) === JSON.stringify(viewportTargets(page)));

        const names = [];
        for (let i = 0; i < 100; ++i)
            names.push("Window " + i);
        recordDriver.seedTargets(1, names);
        waitForRendering(page);
        tryVerify(() => lastPublished().length < 100);
        compare(page.picker.windowRows.length, 100);
        tryVerify(() => JSON.stringify(lastPublished()) === JSON.stringify(viewportTargets(page)));

        // A closed picker captures nothing at all.
        page.picker.close();
        tryVerify(() => lastPublished().length === 0);
    }

    function test_a_stale_still_keeps_its_image_and_its_geometry() {
        let page = makePage();
        const card = cardAt(page, 1);
        recordDriver.deliverStill("display:2", "image://capture-target/display-2/1");
        const state = () => card.thumbnailState;
        tryVerify(() => cardAt(page, 1) === card);
        const cardHeight = card.height;
        tryVerify(() => state() === "ready");
        const image = findVisual(card, "targetThumbnailImage");
        tryCompare(image, "status", Image.Ready);
        verify(image.sourceSize.width > 0 && image.sourceSize.height > 0);
        tryCompare(page.picker, "opened", true);
        const scene = page.Window.window.contentItem;
        const point = image.mapToItem(scene, image.width / 2, image.height / 2);
        tryVerify(() => grabImage(scene).red(Math.floor(point.x), Math.floor(point.y)) === 72);

        recordDriver.failStill("display:2");
        tryVerify(() => state() === "stale");
        // The still survives the loss: nothing reverts to the placeholder glyph,
        // and the card does not resize under it.
        verify(card.thumbnailSource.startsWith("image://capture-target/display-2/"));
        verify(cardAt(page, 1) === card);
        compare(findVisual(card, "targetThumbnailImage").fillMode, Image.PreserveAspectFit);
        compare(card.height, cardHeight);
    }

    function test_columns_follow_the_one_to_four_column_breakpoints() {
        let page = makePage();
        recordDriver.seedTargets(2, ["Claude Design - Brave"]);
        // 860x700 host: the dialog is 720 wide, 672 of content, so two columns.
        compare(page.picker.pickerColumns, 2);
        compare(page.picker.displayColumns, 2, "two displays use two columns");
        const first = cardAt(page, 0);
        const second = cardAt(page, 1);
        compare(first.width, page.picker.displaysCardWidth);
        compare(second.x - first.x, page.picker.displaysCardWidth + page.picker.gridGap);
        verify(page.picker.displaysCardWidth <= 260);

        compare(page.picker.columnsForWidth(300), 1);
        compare(page.picker.columnsForWidth(475), 1);
        compare(page.picker.columnsForWidth(476), 2);
        compare(page.picker.columnsForWidth(721), 2);
        compare(page.picker.columnsForWidth(722), 3);
        compare(page.picker.columnsForWidth(967), 3);
        compare(page.picker.columnsForWidth(968), 4);
        compare(page.picker.columnsForWidth(4000), 4);
    }

    // A single display stays compact and centered in the shared list.
    function test_few_displays_keep_cards_compact_and_centred() {
        let page = makePage();
        recordDriver.seedTargets(1, []);
        waitForRendering(page);
        compare(page.picker.displayColumns, 1);
        compare(page.picker.displaysCardWidth, 260);
        const card = cardAt(page, 0);
        compare(card.width, 260);
        verify(card.parent.x + card.x > 0);
        compare(Math.round(card.parent.x + card.width / 2), Math.round(card.parent.parent.width / 2));
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
        const physicalAspect = preset.width * selector.sourcePixelSize.width / (preset.height * selector.sourcePixelSize.height);
        verify(Math.abs(physicalAspect - 16 / 9) < 0.001);
        selector.visible = false;
        selector.visible = true;
        compare(selector.selectionNormalized, preset);

        recordDriver.adapter.requestRegionPreset("9:16");
        const portrait = selector.selectionNormalized;
        const portraitAspect = portrait.width * selector.sourcePixelSize.width / (portrait.height * selector.sourcePixelSize.height);
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
        tryCompare(page.picker, "opened", false, 1000, "the picker must not outlive the destination that owns it");
    }
    function test_pending_window_lifetime_is_not_rebound_after_hwnd_or_pid_reuse() {
        let page = makePage();
        recordDriver.seedWindowTargets([
            {
                id: 100,
                label: "Document - Editor",
                pid: 10,
                creation: 20,
                generation: 1
            }
        ]);
        showTab(page, 1);
        mouseClick(cardAt(page, 0));
        const identity = page.picker.pendingIdentity;
        recordDriver.seedWindowTargets([
            {
                id: 100,
                label: "Document - Editor",
                pid: 10,
                creation: 20,
                generation: 2
            }
        ]);
        waitForRendering(page);
        verify(page.picker.pendingIdentity === identity);
        verify(!page.picker.confirmEnabled);
        verify(!recordDriver.adapter.confirmTarget(identity, 1));
        compare(selectSpy.count, 0);
        recordDriver.seedWindowTargets([
            {
                id: 100,
                label: "Document - Editor",
                pid: 10,
                creation: 21,
                generation: 2
            }
        ]);
        verify(!recordDriver.adapter.confirmTarget(identity, 1));
        compare(selectSpy.count, 0);
    }
    function refreshWindows() {
        const rows = [];
        for (let i = 0; i < 24; ++i)
            rows.push({
                id: 100 + i,
                label: "Document " + i + " - Editor"
            });
        return rows;
    }

    function test_window_tab_restores_scroll_query_and_selection() {
        recordDriver.seedWindowTargets(refreshWindows());
        const page = makePage();
        compare(page.picker.currentTab, 1);
        page.picker.windowQuery = "Document";
        const list = pick(page, "sourceList");
        list.positionViewAtIndex(5, ListView.Beginning);
        waitForRendering(page);
        const row = page.picker.pickerRows[5].cards[0];
        page.picker.selectCard(row, 1);
        const scroll = list.contentY - list.originY;
        verify(scroll > 0);
        showTab(page, 0);
        verify(!pick(page, "windowSearchRow").visible);
        showTab(page, 1);
        compare(page.picker.windowQuery, "Document");
        compare(page.picker.pendingIdentity, row.identity);
        compare(list.contentY - list.originY, scroll);
    }

    function test_region_uses_the_display_tab_pending_choice() {
        const page = makePage();
        page.picker.selectCard(recordDriver.adapter.displayTargetOptions[1], 0);
        showTab(page, 2);
        compare(page.picker.regionAnchorRow().identity, "display:2");
        mouseClick(pick(page, "presetCard-16:9"));
        mouseClick(pick(page, "confirmButton"));
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][0], 1);
    }

    function test_inactive_window_tab_preserves_its_source_anchor() {
        const rows = refreshWindows();
        recordDriver.seedWindowTargets(rows);
        const page = makePage();
        const list = pick(page, "sourceList");
        list.positionViewAtIndex(5, ListView.Beginning);
        waitForRendering(page);
        const identity = page.picker.pickerRows[5].cards[0].identity;
        const offset = pick(page, "targetCard-" + identity).mapToItem(list, 0, 0).y;
        showTab(page, 0);
        rows.splice(0, 4);
        recordDriver.seedWindowTargets(rows);
        showTab(page, 1);
        compare(pick(page, "targetCard-" + identity).mapToItem(list, 0, 0).y, offset);
    }

    function test_structural_removal_keeps_an_active_flick_moving() {
        const rows = refreshWindows();
        recordDriver.seedWindowTargets(rows);
        const page = makePage();
        const list = pick(page, "sourceList");
        list.positionViewAtIndex(5, ListView.Beginning);
        waitForRendering(page);
        list.flick(0, -900);
        tryCompare(list, "moving", true);
        rows.splice(0, 2);
        recordDriver.seedWindowTargets(rows);
        waitForRendering(page);
        verify(list.moving, "source removal must not cancel an ongoing flick");
    }

    function test_structural_removal_during_scrollbar_drag_does_not_restore() {
        const rows = refreshWindows();
        recordDriver.seedWindowTargets(rows);
        const page = makePage();
        const list = pick(page, "sourceList");
        const bar = findChild(page.picker, "sourceScrollBar");
        list.positionViewAtIndex(5, ListView.Beginning);
        waitForRendering(page);
        bar.pressed = true;
        const scroll = list.contentY;
        rows.splice(0, 2);
        recordDriver.seedWindowTargets(rows);
        waitForRendering(page);
        compare(list.contentY, scroll);
        bar.pressed = false;
    }

    function test_saved_hidden_rules_remain_visible() {
        const page = makePage();
        recordDriver.adapter.addApplicationRule(recordDriver.adapter.windowTargetOptions[0].identity, true);
        compare(page.picker.hiddenWindowRules.length, 1);
        pick(page, "hiddenRulesButton").clicked();
        const popup = findChild(page.picker, "hiddenRulesPopup");
        tryCompare(popup, "opened", true);
        const id = page.picker.hiddenWindowRules[0].id;
        tryVerify(() => findVisual(popup.contentItem, "hiddenRule-" + id) !== null);
    }

    function test_scrolling_card_slot_cannot_inherit_focus_after_removal() {
        const rows = refreshWindows();
        recordDriver.seedWindowTargets(rows);
        const page = makePage();
        const list = pick(page, "sourceList");
        const bar = findChild(page.picker, "sourceScrollBar");
        list.positionViewAtIndex(5, ListView.Beginning);
        waitForRendering(page);
        const row = page.picker.pickerRows[5].cards[0];
        const identity = row.identity;
        const card = pick(page, "targetCard-" + row.identity);
        page.picker.selectCard(row, 1);
        card.forceActiveFocus(Qt.TabFocusReason);
        bar.pressed = true;
        rows.splice(0, 2);
        recordDriver.seedWindowTargets(rows);
        waitForRendering(page);
        verify(!card.activeFocus);
        bar.pressed = false;
        keyClick(Qt.Key_Return);
        compare(selectSpy.count, 0);
        compare(page.picker.pendingIdentity, identity);
    }

    function test_refresh_preserves_scrolled_source_and_keyboard_focus() {
        const rows = refreshWindows();
        recordDriver.seedWindowTargets(rows);
        const page = makePage();
        const list = pick(page, "sourceList");
        list.positionViewAtIndex(8, ListView.Beginning);
        waitForRendering(page);
        const identity = page.picker.pickerRows[8].cards[0].identity;
        const nativeId = rows[page.picker.pickerRows[8].cards[0].targetIndex].id;
        const card = pick(page, "targetCard-" + identity);
        card.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        const offset = card.mapToItem(list, 0, 0).y;
        const scroll = list.contentY;
        verify(scroll > 0);

        rows[0].label = "Renamed document - Editor";
        recordDriver.seedWindowTargets(rows);
        waitForRendering(page);
        tryCompare(list, "contentY", scroll);
        const refreshed = pick(page, "targetCard-" + identity);
        compare(refreshed.mapToItem(list, 0, 0).y, offset);
        verify(refreshed.activeFocus, "refresh retains the focused lifetime identity");
        compare(page.picker.pendingIdentity, identity);
        tryVerify(() => JSON.stringify(lastPublished()) === JSON.stringify(viewportTargets(page)));

        rows.unshift({
            id: 99,
            label: "Inserted window - Editor"
        });
        recordDriver.seedWindowTargets(rows);
        waitForRendering(page);
        compare(page.picker.windowRows[page.picker.windowRows.length - 1].title, "Inserted window");
        tryVerify(() => findVisual(page.picker.contentItem, "targetCard-" + identity) !== null && findVisual(page.picker.contentItem, "targetCard-" + identity).mapToItem(list, 0, 0).y === offset, 5000, "focused source keeps viewport offset " + offset + "; contentY=" + list.contentY + ", originY=" + list.originY);
        verify(pick(page, "targetCard-" + identity).activeFocus);
        compare(page.picker.pendingIdentity, identity);

        rows.splice(0, 3);
        recordDriver.seedWindowTargets(rows);
        waitForRendering(page);
        compare(pick(page, "targetCard-" + identity).mapToItem(list, 0, 0).y, offset);
        compare(page.picker.pendingIdentity, identity);
        keyClick(Qt.Key_Return);
        tryCompare(selectSpy, "count", 1);
        compare(selectSpy.signalArguments[0][0], rows.findIndex(row => row.id === nativeId));
    }

    function test_unavailable_card_cannot_confirm_previous_pending_source() {
        const page = makePage();
        mouseClick(cardAt(page, 0));
        const identity = page.picker.pendingIdentity;
        verify(identity !== "");
        page.picker.confirmCard({
            available: false,
            identity: "",
            targetIndex: 1,
            kind: "window"
        }, 1);
        compare(selectSpy.count, 0);
        verify(page.picker.opened);
        compare(page.picker.pendingIdentity, identity);
    }
    function test_selected_target_and_region_have_independent_focus_markers() {
        const page = makePage();
        const card = cardAt(page, 0);
        mouseClick(card);
        card.forceActiveFocus(Qt.TabFocusReason);
        const targetFocus = findVisual(card, "cardFocusRing");
        verify(targetFocus !== null, "target exposes keyboard focus independently of selection");
        verify(targetFocus.visible);
        verify(card.pending);
        showTab(page, 2);
        const preset = pick(page, "presetCard-custom");
        preset.forceActiveFocus(Qt.TabFocusReason);
        const regionFocus = findVisual(preset, "cardFocusRing");
        verify(regionFocus !== null, "region preset exposes keyboard focus");
        verify(regionFocus.visible);
    }
    function test_title_refresh_keeps_an_active_flick_moving() {
        const rows = refreshWindows();
        recordDriver.seedWindowTargets(rows);
        const page = makePage();
        const list = pick(page, "sourceList");
        list.positionViewAtIndex(5, ListView.Beginning);
        waitForRendering(page);
        list.flick(0, -900);
        tryCompare(list, "moving", true);
        const scroll = list.contentY;
        rows[0].label = "Renamed document - Editor";
        recordDriver.seedWindowTargets(rows);
        waitForRendering(page);
        verify(list.moving, "a title refresh does not cancel an active flick");
        verify(list.contentY >= scroll, "a refresh does not jump back while scrolling");
        list.cancelFlick();
    }
    function test_recycled_focused_window_does_not_receive_keyboard_confirmation() {
        recordDriver.seedWindowTargets([
            {
                id: 100,
                label: "Document - Editor",
                generation: 1
            }
        ]);
        const page = makePage();
        showTab(page, 1);
        const card = cardAt(page, 0);
        card.forceActiveFocus(Qt.TabFocusReason);
        keyClick(Qt.Key_Space);
        const identity = page.picker.pendingIdentity;
        recordDriver.seedWindowTargets([
            {
                id: 100,
                label: "Replacement - Editor",
                generation: 2
            }
        ]);
        waitForRendering(page);
        verify(!cardAt(page, 0).activeFocus, "a recycled lifetime does not inherit keyboard focus");
        keyClick(Qt.Key_Return);
        compare(selectSpy.count, 0);
        compare(page.picker.pendingIdentity, identity);
        verify(!page.picker.confirmEnabled);
    }
    function test_queued_structural_refreshes_keep_surviving_card_focus() {
        const rows = refreshWindows();
        recordDriver.seedWindowTargets(rows);
        const page = makePage();
        const list = pick(page, "sourceList");
        list.positionViewAtIndex(8, ListView.Beginning);
        waitForRendering(page);
        const identity = page.picker.pickerRows[8].cards[0].identity;
        const card = pick(page, "targetCard-" + identity);
        card.forceActiveFocus(Qt.TabFocusReason);
        const offset = card.mapToItem(list, 0, 0).y;
        rows.unshift({
            id: 99,
            label: "Inserted A - Editor"
        });
        recordDriver.seedWindowTargets(rows);
        rows.unshift({
            id: 98,
            label: "Inserted B - Editor"
        });
        recordDriver.seedWindowTargets(rows);
        waitForRendering(page);
        const refreshed = pick(page, "targetCard-" + identity);
        verify(refreshed.activeFocus, "a queued refresh batch keeps the original surviving lifetime focused");
        compare(refreshed.mapToItem(list, 0, 0).y, offset);
    }

    function test_user_focus_navigation_supersedes_queued_refresh_restore() {
        const rows = refreshWindows();
        recordDriver.seedWindowTargets(rows);
        const page = makePage();
        const list = pick(page, "sourceList");
        list.positionViewAtIndex(5, ListView.Beginning);
        waitForRendering(page);
        const identity = page.picker.pickerRows[5].cards[0].identity;
        pick(page, "targetCard-" + identity).forceActiveFocus(Qt.TabFocusReason);
        rows.unshift({
            id: 99,
            label: "Inserted window - Editor"
        });
        recordDriver.seedWindowTargets(rows);
        const search = pick(page, "windowSearch");
        search.forceActiveFocus(Qt.TabFocusReason);
        waitForRendering(page);
        verify(search.activeFocus, "a queued restore cannot steal focus after explicit navigation");
    }
}

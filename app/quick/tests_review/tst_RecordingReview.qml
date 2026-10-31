import QtQuick
import QtTest
import ExoSnap.Quick.EditTestControls

TestCase {
    id: testCase
    name: "RecordingReview"
    when: windowShown
    width: 860
    height: 500
    visible: true
    SignalSpy {
        id: editSpy
        target: testRecordingReview
        signalName: "editRequested"
    }
    SignalSpy {
        id: closeSpy
        signalName: "closeRequested"
    }

    function cleanup() {
        testEditHarness.setReviewPlaybackPlaying(false);
        testTransportPlayer.endScrub();
        testRecordingReview.close();
        testCase.forceActiveFocus(Qt.OtherFocusReason);
        mouseMove(testCase, 0, 0);
        editSpy.clear();
        closeSpy.target = null;
        closeSpy.clear();
        testEditHarness.setVisualAppearance("dark");
    }

    function test_lifecycle_and_actual_output_volume() {
        const result = testEditHarness.reviewLifecycleProbe();
        for (const key of ["opened", "silent", "held", "restored", "clamped", "closed", "residentUnchanged", "positioned", "appended", "scrubEnded", "recovered", "asyncPositioned", "cancelledImport"])
            compare(result[key], true, key);
    }

    Component {
        id: reviewComponent
        RecordingReview {
            width: testCase.width
            height: testCase.height
            session: testSession
            player: testTransportPlayer
            onCloseRequested: if (audioController)
                audioController.close()
        }
    }

    function test_single_transport_without_center_play() {
        const review = createTemporaryObject(reviewComponent, testCase);
        verify(review);
        verify(findChild(review, "reviewPlayPause"));
        compare(findChild(review, "reviewCenterPlay"), null);
        verify(findChild(review, "reviewSeek"));
        verify(findChild(review, "reviewAudio"));
    }

    function test_controls_overlay_video_and_close_explicitly() {
        const review = createTemporaryObject(reviewComponent, testCase);
        verify(review);
        const surface = findChild(review, "reviewSurface");
        const controls = findChild(review, "reviewControls");
        const close = findChild(review, "reviewClose");
        compare(surface.height, review.height);
        compare(controls.y + controls.height, surface.y + surface.height);
        verify(close.y < controls.y);
        verify(close.x > review.width / 2);
        closeSpy.target = review;
        mouseClick(close);
        compare(closeSpy.count, 1);
    }

    function test_idle_hides_playing_controls_and_motion_reveals() {
        const review = createTemporaryObject(reviewComponent, testCase);
        verify(review);
        review.forceActiveFocus();
        mouseMove(review, 20, 20);
        const controls = findChild(review, "reviewControls");
        const close = findChild(review, "reviewClose");
        wait(2600);
        compare(controls.opacity, 1, "paused controls stay visible");
        testEditHarness.setReviewPlaybackPlaying(true);
        compare(testTransportPlayer.playing, true);
        compare(review.controlsHeld, false, "idle playback has no focused, seeking or popup controls");
        compare(controls.opacity, 1);
        wait(2000);
        compare(controls.opacity, 1, "controls remain until the idle interval");
        tryCompare(controls, "opacity", 0, 1500);
        compare(close.opacity, 0);
        mouseMove(review, 40, 20);
        tryCompare(controls, "opacity", 1);
        compare(close.opacity, 1);
        testEditHarness.setReviewPlaybackPlaying(false);
        wait(2600);
        compare(controls.opacity, 1);
    }

    function test_keyboard_focus_and_scrubbing_keep_controls_visible() {
        const review = createTemporaryObject(reviewComponent, testCase);
        verify(review);
        review.forceActiveFocus();
        mouseMove(review, 20, 20);
        testEditHarness.setReviewPlaybackPlaying(true);
        const controls = findChild(review, "reviewControls");
        compare(testTransportPlayer.playing, true);
        compare(review.controlsHeld, false, "idle playback has no focused, seeking or popup controls");
        tryCompare(controls, "opacity", 0, 3500);
        const play = findChild(review, "reviewPlayPause");
        play.forceActiveFocus(Qt.TabFocusReason);
        tryCompare(controls, "opacity", 1);
        wait(2600);
        compare(controls.opacity, 1, "keyboard focus prevents idle hiding");
        review.forceActiveFocus();
        testTransportPlayer.beginScrub();
        compare(testTransportPlayer.scrubbing, true);
        wait(2600);
        compare(controls.opacity, 1, "seeking prevents idle hiding");
        testTransportPlayer.endScrub();
    }

    function test_open_audio_panel_keeps_playing_controls_visible() {
        const review = createTemporaryObject(reviewComponent, testCase);
        verify(review);
        review.forceActiveFocus();
        mouseMove(review, 20, 20);
        testEditHarness.setReviewPlaybackPlaying(true);
        const controls = findChild(review, "reviewControls");
        tryCompare(controls, "opacity", 0, 3500);
        review.showAudio();
        const panel = findChild(review, "reviewAudioPanel");
        tryCompare(panel, "opened", true);
        mouseMove(panel.background, 2, 2);
        wait(2600);
        compare(controls.opacity, 1);
        verify(panel.opened);
        panel.close();
        mouseMove(review, 20, 20);
        tryCompare(controls, "opacity", 0, 3500);
    }

    function test_audio_focus_opens_panel_and_escape_closes() {
        const review = createTemporaryObject(reviewComponent, testCase);
        verify(review);
        const audio = findChild(review, "reviewAudio");
        audio.forceActiveFocus();
        tryCompare(findChild(review, "reviewAudioPanel"), "opened", true);
        keyClick(Qt.Key_Escape);
        tryCompare(findChild(review, "reviewAudioPanel"), "opened", false);
    }

    function test_narrow_transport_stays_inside_preview() {
        const review = createTemporaryObject(reviewComponent, testCase, {
            width: 500
        });
        verify(review);
        const audio = findChild(review, "reviewAudio");
        verify(audio.x + audio.width <= review.width);
        verify(findChild(review, "reviewSeek").width > 0);
    }

    function test_audio_keyboard_can_leave_panel() {
        const review = createTemporaryObject(reviewComponent, testCase, {
            session: testRecordingReview.session,
            player: testRecordingReview.player,
            audioController: testRecordingReview
        });
        verify(review);
        verify(testEditHarness.prepareRecordingReviewVisual());
        findChild(review, "reviewAudio").forceActiveFocus();
        const panel = findChild(review, "reviewAudioPanel");
        tryCompare(panel, "opened", true);
        keyClick(Qt.Key_Down);
        keyClick(Qt.Key_Tab);
        keyClick(Qt.Key_Escape);
        tryCompare(panel, "opened", false);
        wait(50);
        compare(panel.opened, false);
    }

    function test_audio_panel_padding_keeps_hover_open() {
        const review = createTemporaryObject(reviewComponent, testCase, {
            session: testRecordingReview.session,
            player: testRecordingReview.player,
            audioController: testRecordingReview
        });
        verify(review);
        verify(testEditHarness.prepareRecordingReviewVisual());
        review.forceActiveFocus();
        const speaker = findChild(review, "reviewAudio");
        const panel = findChild(review, "reviewAudioPanel");
        mouseMove(speaker, speaker.width / 2, speaker.height / 2);
        tryCompare(panel, "opened", true);
        mouseMove(panel.background, 2, 2);
        wait(350);
        verify(panel.opened, "popup padding belongs to the hover surface");
        const volume = findChild(review, "reviewVolume");
        mouseMove(volume, volume.width / 2, volume.height / 2);
        wait(350);
        verify(panel.opened, "hovering the volume control keeps the panel open");
        mouseMove(review, 20, 20);
        tryCompare(panel, "opened", false);
    }

    function test_audio_track_padding_and_interaction_states_data() {
        return [{tag: "dark", appearance: "dark"}, {tag: "light", appearance: "light"}];
    }

    function test_audio_track_padding_and_interaction_states(data) {
        verify(testEditHarness.setVisualAppearance(data.appearance));
        const review = createTemporaryObject(reviewComponent, testCase, {
            session: testRecordingReview.session,
            player: testRecordingReview.player,
            audioController: testRecordingReview
        });
        verify(review);
        verify(testEditHarness.prepareRecordingReviewVisual());
        review.showAudio();
        const panel = findChild(review, "reviewAudioPanel");
        tryCompare(panel, "opened", true);
        mouseMove(panel.background, 2, 2);
        const tracks = testRecordingReview.player.monitoringTracks;
        const selected = visualChild(panel.contentItem, "reviewTrack-" + tracks.find(track => track.selected).id);
        const other = visualChild(panel.contentItem, "reviewTrack-" + tracks.find(track => !track.selected).id);
        waitForRendering(review);
        tryVerify(() => selected.contentItem.implicitHeight > 0);
        compare(selected.topPadding, 8);
        compare(selected.bottomPadding, 8);
        compare(selected.leftPadding, 12);
        compare(selected.rightPadding, 12);
        tryVerify(() => selected.height === selected.contentItem.implicitHeight + 16);
        compare(selected.contentItem.y, 8);
        const restingSelection = selected.background.color.toString();
        verify(restingSelection !== other.background.color.toString());
        mouseMove(other, other.width / 2, other.height / 2);
        tryCompare(other, "hovered", true);
        verify(restingSelection !== other.background.color.toString(), "hover does not mimic selection");
        mouseMove(selected, selected.width / 2, selected.height / 2);
        tryCompare(selected, "hovered", true);
        verify(restingSelection !== selected.background.color.toString(), "the selected row also responds to hover");
        selected.forceActiveFocus(Qt.TabFocusReason);
        tryCompare(selected, "visualFocus", true);
        compare(selected.background.border.width, 1);
        if (testVisualDirectory.length > 0) {
            waitForRendering(review);
            let saved = false;
            verify(review.Window.window.contentItem.grabToImage(result => {
                saved = result.saveToFile(testVisualDirectory + "/audio-selected-hover-focus-" + data.appearance + ".png");
            }));
            tryVerify(() => saved);
        }
        selected.enabled = false;
        mouseMove(panel.background, 2, 2);
        const label = selected.contentItem.children[0];
        compare(label.color, ExoTheme.textDim);
    }

    function test_appearance_data() {
        return [
            {
                tag: "dark-normal",
                appearance: "dark",
                width: 1520,
                height: 800
            },
            {
                tag: "light-normal",
                appearance: "light",
                width: 1520,
                height: 800
            },
            {
                tag: "dark-narrow",
                appearance: "dark",
                width: 780,
                height: 500
            },
            {
                tag: "light-narrow",
                appearance: "light",
                width: 780,
                height: 500
            }
        ];
    }

    function test_appearance(data) {
        verify(testEditHarness.setVisualAppearance(data.appearance));
        testCase.width = data.width;
        testCase.height = data.height;
        const review = createTemporaryObject(reviewComponent, testCase, {
            width: data.width,
            height: data.height,
            session: testRecordingReview.session,
            player: testRecordingReview.player,
            audioController: testRecordingReview
        });
        verify(review);
        verify(testEditHarness.prepareRecordingReviewVisual());
        tryCompare(testRecordingReview.player, "clipOpen", true, 10000);
        compare(testRecordingReview.player.monitoringTracks.length, 2);
        tryCompare(findChild(review, "reviewHarnessStill"), "status", Image.Ready);
        findChild(review, "reviewAudio").forceActiveFocus();
        const panel = findChild(review, "reviewAudioPanel");
        tryCompare(panel, "opened", true);
        verify(panel.x >= 0 && panel.x + panel.width <= review.width);
        verify(panel.y >= 0 && panel.y + panel.height <= review.height);
        const seek = findChild(review, "reviewSeek");
        compare(seek.background.height, 3);
        verify(seek.background.color !== ExoTheme.surfaceHover);
        const tracks = testRecordingReview.player.monitoringTracks;
        for (const track of tracks) {
            const button = visualChild(panel.contentItem, "reviewTrack-" + track.id);
            verify(button.height >= button.contentItem.implicitHeight + button.topPadding + button.bottomPadding);
        }
        waitForRendering(review);
        if (testVisualDirectory.length > 0) {
            let saved = false;
            const path = testVisualDirectory + "/review-" + data.tag + ".png";
            verify(review.Window.window.contentItem.grabToImage(result => {
                saved = result.saveToFile(path);
            }));
            tryVerify(() => saved, 5000, path);
        }
    }

    function visualChild(item, name) {
        if (item.objectName === name)
            return item;
        for (const child of item.children) {
            const found = visualChild(child, name);
            if (found)
                return found;
        }
        return null;
    }
}

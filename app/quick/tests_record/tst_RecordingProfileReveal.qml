import QtQuick
import QtTest

import ExoSnap.Quick.RecordPickerTestControls

TestCase {
    id: testCase
    name: "RecordingProfileReveal"
    when: windowShown
    width: 1200
    height: 800
    visible: true

    Component {
        id: inspectorComponent
        SettingsPage {
            width: 860
            height: 600
            settings: recordDriver.settings
        }
    }

    function findGroup(item) {
        if (!item || !item.visible)
            return null;
        if (item.Accessible.name === qsTr("Microphone post-processing"))
            return item;
        for (const child of item.children) {
            const result = findGroup(child);
            if (result)
                return result;
        }
        return null;
    }

    function findControl(item, name) {
        if (!item || !item.visible)
            return null;
        if (item.Accessible.name === name)
            return item;
        for (const child of item.children) {
            const result = findControl(child, name);
            if (result)
                return result;
        }
        return null;
    }

    function findSection(item, section) {
        const titles = ({
                format: qsTr("Recording format"),
                quality: qsTr("Video quality & timing"),
                audio: qsTr("Audio tracks"),
                microphone: qsTr("Microphone"),
                "audio-encoding": qsTr("Audio encoding"),
                output: qsTr("Output"),
                webcam: qsTr("Webcam")
            });
        if (!item || !item.visible)
            return null;
        if (section === "preset" ? typeof item.openHarnessDialog === "function" && item.stacked !== undefined : item.title === titles[section])
            return item;
        for (const child of item.children) {
            const result = findSection(child, section);
            if (result)
                return result;
        }
        return null;
    }

    function cleanup() {
        recordDriver.settings.expertMode = false;
        recordDriver.setRecordingContext(false);
        recordDriver.seedAudioSources(0, "");
    }

    function test_graph_microphone_target_reveals_audio_routing() {
        recordDriver.seedAudioSources(0, "");
        const inspector = createTemporaryObject(inspectorComponent, testCase);
        verify(!!inspector);
        waitForRendering(inspector);
        verify(!recordDriver.settings.audioRouting.legacy);
        compare(inspector.revealAutomationTarget("microphone"), 1);
        compare(inspector.revealAutomationTarget("unknown-section"), -1);
    }

    function test_details_follow_real_configure_hide_state_data() {
        return [
            {
                tag: "simple",
                expert: false
            },
            {
                tag: "expert",
                expert: true
            }
        ];
    }

    function test_details_follow_real_configure_hide_state(data) {
        recordDriver.seedLegacyAudio();
        recordDriver.settings.expertMode = data.expert;
        const inspector = createTemporaryObject(inspectorComponent, testCase);
        verify(!!inspector);
        waitForRendering(inspector);
        const group = findGroup(inspector);
        verify(!!group);
        verify(!group.open);
        inspector.prepareProfileVisual(true);
        tryCompare(group, "open", true);
        tryVerify(() => !!findControl(inspector, qsTr("Microphone high-pass filter")));
        tryVerify(() => inspector.revealAutomationTarget("microphone") === 1);
        group.forceActiveFocus();
        keyClick(Qt.Key_Space);
        tryCompare(group, "open", false);
        inspector.prepareProfileVisual(true);
        tryCompare(group, "open", true);
        inspector.prepareProfileVisual(false);
        tryCompare(group, "open", false);
        tryVerify(() => !findControl(inspector, qsTr("Microphone high-pass filter")));
        inspector.prepareProfileVisual(true);
        tryCompare(group, "open", true);
        inspector.prepareProfileVisual(false);
        tryCompare(group, "open", false);
    }

    function test_each_profile_section_reaches_the_viewport_data() {
        const rows = [];
        for (const expert of [false, true]) {
            for (const section of ["preset", "format", "quality", "audio", "microphone", "audio-encoding", "output", "webcam"])
                rows.push({
                    tag: (expert ? "expert-" : "simple-") + section,
                    expert: expert,
                    section: section
                });
        }
        return rows;
    }

    function test_each_profile_section_reaches_the_viewport(data) {
        recordDriver.seedLegacyAudio();
        recordDriver.settings.expertMode = data.expert;
        const inspector = createTemporaryObject(inspectorComponent, testCase);
        verify(!!inspector);
        waitForRendering(inspector);
        tryVerify(() => inspector.revealAutomationTarget(data.section) === 1);
        const section = findSection(inspector, data.section);
        verify(!!section, "the reported target is a real visible settings section");
        const position = section.mapToItem(inspector, 0, 0);
        verify(position.y >= 0 && position.y < inspector.height, "the section heading reached the inspector viewport");
    }
    function test_audio_source_details_follow_existing_opt_in_data() {
        return [
            {
                tag: "simple",
                expert: false
            },
            {
                tag: "expert",
                expert: true
            }
        ];
    }

    function test_audio_source_details_follow_existing_opt_in(data) {
        recordDriver.seedAudioSources(1, "Desktop ");
        recordDriver.settings.expertMode = data.expert;
        const inspector = createTemporaryObject(inspectorComponent, testCase);
        verify(!!inspector);
        waitForRendering(inspector);
        const name = qsTr("Channel mode for %1").arg("Desktop 0");
        verify(!findControl(inspector, name));
        inspector.prepareProfileVisual(true);
        const popup = findChild(inspector, "audioSourceDetails");
        tryCompare(popup, "opened", true);
        tryVerify(() => !!findControl(popup.contentItem, name));
        inspector.prepareProfileVisual(false);
        tryCompare(popup, "opened", false);
        compare(recordDriver.settings.audioRouting.sources.length, 1);
    }

    function test_empty_audio_details_do_not_create_a_source() {
        recordDriver.seedAudioSources(0, "");
        const inspector = createTemporaryObject(inspectorComponent, testCase);
        verify(!!inspector);
        waitForRendering(inspector);
        inspector.prepareProfileVisual(true);
        compare(recordDriver.settings.audioRouting.sources.length, 0);
        inspector.prepareProfileVisual(false);
        compare(recordDriver.settings.audioRouting.sources.length, 0);
    }
}

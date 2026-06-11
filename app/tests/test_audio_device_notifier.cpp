#include <gtest/gtest.h>

#include <QCoreApplication>

#include "services/AudioDeviceNotifier.h"

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Shared QCoreApplication — required for QTimer and QMetaObject::invokeMethod
// ---------------------------------------------------------------------------

QCoreApplication* EnsureApplication() {
    if (QCoreApplication::instance())
        return QCoreApplication::instance();
    static int argc = 1;
    static char app_name[] = "audio_device_notifier_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

// ---------------------------------------------------------------------------
// Signal recorder — avoids Qt6::Test / QSignalSpy dependency
// ---------------------------------------------------------------------------

struct SignalRecord {
    AudioDeviceSnapshot snapshot;
    DiscoveryReason reason = DiscoveryReason::Rescan;
};

struct SignalSink {
    std::vector<SignalRecord> calls;

    void connect(AudioDeviceNotifier* notifier) {
        QObject::connect(notifier, &AudioDeviceNotifier::snapshotChanged,
                         [this](const exosnap::AudioDeviceSnapshot& snap, exosnap::DiscoveryReason reason) {
                             calls.push_back({snap, reason});
                         });
    }

    int count() const {
        return static_cast<int>(calls.size());
    }

    const SignalRecord& last() const {
        return calls.back();
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static recorder_core::AudioInputDeviceInfo MakeDevice(std::string id, std::string name, bool is_default = false) {
    recorder_core::AudioInputDeviceInfo d;
    d.device_id = std::move(id);
    d.display_name = std::move(name);
    d.is_default = is_default;
    return d;
}

// Build a snapshot with one input and one output.
static AudioDeviceSnapshot MakeSnapshot(const std::string& input_id, const std::string& output_id,
                                        bool input_default = true, bool output_default = true) {
    AudioDeviceSnapshot snap;
    snap.inputs.push_back(MakeDevice(input_id, "Input " + input_id, input_default));
    snap.outputs.push_back(MakeDevice(output_id, "Output " + output_id, output_default));
    snap.default_input_id = input_default ? input_id : "";
    snap.default_output_id = output_default ? output_id : "";
    return snap;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class AudioDeviceNotifierTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    // Create a notifier in test mode (no COM registration).
    static std::unique_ptr<AudioDeviceNotifier> MakeTestNotifier(AudioDeviceNotifier::Enumerator enumerator,
                                                                 int debounce_ms = 0) {
        auto n = std::make_unique<AudioDeviceNotifier>();
        n->setEnumeratorForTest(std::move(enumerator));
        n->setDebounceIntervalMsForTest(debounce_ms);
        return n;
    }
};

// ---------------------------------------------------------------------------
// Test 1: Construct + destroy in test mode — no crash, no native registration
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, ConstructDestroyTestMode_NoCrash) {
    auto notifier = MakeTestNotifier([] { return AudioDeviceSnapshot{}; });
    // No start() called — just destroy
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 2: start() / stop() idempotent in test mode
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, StartStop_IdempotentInTestMode) {
    auto notifier = MakeTestNotifier([] { return AudioDeviceSnapshot{}; });

    notifier->start();
    notifier->start(); // second call is a no-op
    notifier->stop();
    notifier->stop(); // second call is a no-op
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 3: simulateNativeEvent → exactly one snapshotChanged with correct data
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, SimulateDeviceAdded_EmitsSnapshotChanged) {
    const AudioDeviceSnapshot v1 = MakeSnapshot("input-1", "output-1");

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();

    ASSERT_EQ(sink.count(), 1);
    EXPECT_EQ(sink.last().snapshot.inputs.size(), 1);
    EXPECT_EQ(sink.last().snapshot.inputs[0].device_id, "input-1");
    EXPECT_EQ(sink.last().snapshot.outputs.size(), 1);
    EXPECT_EQ(sink.last().snapshot.outputs[0].device_id, "output-1");
    EXPECT_EQ(sink.last().reason, DiscoveryReason::DeviceAdded);
}

// ---------------------------------------------------------------------------
// Test 4: Burst — 5 rapid events → exactly ONE snapshotChanged after flush
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, BurstEvents_CoalescedToOneEmit) {
    const AudioDeviceSnapshot v1 = MakeSnapshot("input-1", "output-1");

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    for (int i = 0; i < 5; ++i) {
        notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    }
    notifier->flushPendingForTest();

    EXPECT_EQ(sink.count(), 1);
}

// ---------------------------------------------------------------------------
// Test 5: DefaultChanged → snapshotChanged emitted with reason=DefaultChanged
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, DefaultDeviceChange_EmitsWithCorrectReason) {
    AudioDeviceSnapshot v1 = MakeSnapshot("input-1", "output-1");
    AudioDeviceSnapshot v2 = MakeSnapshot("input-2", "output-1"); // different default input

    int call_count = 0;
    auto notifier =
        MakeTestNotifier([&call_count, v1, v2]() -> AudioDeviceSnapshot { return (call_count++ == 0) ? v1 : v2; });

    SignalSink sink;
    sink.connect(notifier.get());

    // First event: snapshot changes empty→v1
    notifier->simulateNativeEvent(DiscoveryReason::DefaultChanged);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 1);

    // Second event: snapshot changes v1→v2, reason = DefaultChanged
    notifier->simulateNativeEvent(DiscoveryReason::DefaultChanged);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 2);

    EXPECT_EQ(sink.last().reason, DiscoveryReason::DefaultChanged);
    EXPECT_EQ(sink.last().snapshot.default_input_id, "input-2");
}

// ---------------------------------------------------------------------------
// Test 6: Identical snapshot → NO new snapshotChanged
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, IdenticalSnapshot_NoDuplicateEmit) {
    const AudioDeviceSnapshot v1 = MakeSnapshot("input-1", "output-1");

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    // First event: should emit (empty -> v1)
    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 1);

    // Second event with same snapshot: should NOT emit
    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    EXPECT_EQ(sink.count(), 1);
}

// ---------------------------------------------------------------------------
// Test 7: Simulate after stop() — no crash (QObject still alive, timer just stopped)
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, SimulateAfterStop_NoCrash) {
    const AudioDeviceSnapshot v1 = MakeSnapshot("input-1", "output-1");

    auto notifier = MakeTestNotifier([v1] { return v1; });

    notifier->start();
    notifier->stop();

    // After stop() the QObject still exists; simulate is safe (no COM re-entry).
    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 7b: Simulate on destroyed notifier — QPointer prevents UAF
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, SimulateOnDestroyedNotifier_NoUAF) {
    const AudioDeviceSnapshot v1 = MakeSnapshot("input-1", "output-1");

    // Queue an event, then destroy the notifier before the queued call runs.
    {
        auto notifier = MakeTestNotifier([v1] { return v1; });
        notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
        // notifier destroyed here — QPointer guard in the lambda prevents UAF
    }

    // Process the queued call; the lambda must check the QPointer and no-op.
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 8: Unsorted input → snapshot.inputs sorted by device_id
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, DeterministicOrdering_InputsSortedByDeviceId) {
    auto enumerator = []() -> AudioDeviceSnapshot {
        AudioDeviceSnapshot snap;
        snap.inputs.push_back(MakeDevice("zzz-device", "Z Device"));
        snap.inputs.push_back(MakeDevice("aaa-device", "A Device", true));
        snap.inputs.push_back(MakeDevice("mmm-device", "M Device"));
        snap.outputs.push_back(MakeDevice("out-1", "Out 1", true));
        snap.default_input_id = "aaa-device";
        snap.default_output_id = "out-1";
        return snap;
    };

    auto notifier = MakeTestNotifier(enumerator);
    notifier->simulateNativeEvent(DiscoveryReason::Startup);
    notifier->flushPendingForTest();

    const auto snap = notifier->currentSnapshot();
    ASSERT_EQ(snap.inputs.size(), 3);
    EXPECT_EQ(snap.inputs[0].device_id, "aaa-device");
    EXPECT_EQ(snap.inputs[1].device_id, "mmm-device");
    EXPECT_EQ(snap.inputs[2].device_id, "zzz-device");
}

// ---------------------------------------------------------------------------
// Test 9: rescan() → immediate synchronous emit when snapshot differs
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, Rescan_EmitsImmediatelyWhenSnapshotDiffers) {
    const AudioDeviceSnapshot v1 = MakeSnapshot("input-1", "output-1");

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    // rescan() is synchronous — no processEvents needed
    notifier->rescan();

    ASSERT_EQ(sink.count(), 1);
    EXPECT_EQ(sink.last().reason, DiscoveryReason::Rescan);
}

// ---------------------------------------------------------------------------
// Test 9b: rescan() with unchanged snapshot → no emit
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, Rescan_NoEmitWhenSnapshotUnchanged) {
    const AudioDeviceSnapshot v1 = MakeSnapshot("input-1", "output-1");

    auto notifier = MakeTestNotifier([v1] { return v1; });

    notifier->rescan(); // establishes v1 as last_snapshot_

    SignalSink sink;
    sink.connect(notifier.get());
    notifier->rescan(); // same snapshot — must not emit

    EXPECT_EQ(sink.count(), 0);
}

// ---------------------------------------------------------------------------
// Test 10: currentSnapshot() reflects last published snapshot
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, CurrentSnapshot_ReflectsLastPublished) {
    const AudioDeviceSnapshot v1 = MakeSnapshot("input-X", "output-Y");

    auto notifier = MakeTestNotifier([v1] { return v1; });

    notifier->rescan();

    const auto snap = notifier->currentSnapshot();
    ASSERT_EQ(snap.inputs.size(), 1);
    EXPECT_EQ(snap.inputs[0].device_id, "input-X");
    ASSERT_EQ(snap.outputs.size(), 1);
    EXPECT_EQ(snap.outputs[0].device_id, "output-Y");
    EXPECT_EQ(snap.default_input_id, "input-X");
    EXPECT_EQ(snap.default_output_id, "output-Y");
}

// ---------------------------------------------------------------------------
// Test 11: DeviceRemoved reason takes priority when coalescing burst
// ---------------------------------------------------------------------------

TEST_F(AudioDeviceNotifierTest, DeviceRemovedPriority_KeptDuringBurst) {
    const AudioDeviceSnapshot v1 = MakeSnapshot("input-1", "output-1");

    // Use a nonzero debounce so the timer does NOT fire between simulate calls.
    // We'll manually override it to 0 before flush.
    auto notifier = MakeTestNotifier([v1] { return v1; },
                                     10000); // 10 s debounce — won't fire during queuing

    SignalSink sink;
    sink.connect(notifier.get());

    // Queue three simulate calls without flushing — delivers scheduleRefresh 3x
    notifier->simulateNativeEvent(DiscoveryReason::PropertyChanged); // sets reason
    notifier->simulateNativeEvent(DiscoveryReason::DeviceRemoved);   // should override
    notifier->simulateNativeEvent(DiscoveryReason::PropertyChanged); // should NOT override DeviceRemoved

    // Deliver the queued invokeMethod calls (scheduleRefresh) without firing timer
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    // Now switch to 0ms debounce and flush to trigger refreshNow
    notifier->setDebounceIntervalMsForTest(0);
    notifier->flushPendingForTest();

    ASSERT_EQ(sink.count(), 1);
    EXPECT_EQ(sink.last().reason, DiscoveryReason::DeviceRemoved);
}

} // namespace
} // namespace exosnap

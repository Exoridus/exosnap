#include <gtest/gtest.h>

#include <QCoreApplication>

#include "services/WebcamDeviceNotifier.h"

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Shared QCoreApplication — required for QTimer and QMetaObject::invokeMethod
// ---------------------------------------------------------------------------

QCoreApplication* EnsureApplication() {
    if (QCoreApplication::instance())
        return QCoreApplication::instance();
    static int argc = 1;
    static char app_name[] = "webcam_device_notifier_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

// ---------------------------------------------------------------------------
// Signal recorder — avoids Qt6::Test / QSignalSpy dependency
// ---------------------------------------------------------------------------

struct SignalRecord {
    WebcamDeviceSnapshot snapshot;
    DiscoveryReason reason = DiscoveryReason::Rescan;
};

struct SignalSink {
    std::vector<SignalRecord> calls;

    void connect(WebcamDeviceNotifier* notifier) {
        QObject::connect(notifier, &WebcamDeviceNotifier::snapshotChanged,
                         [this](const exosnap::WebcamDeviceSnapshot& snap, exosnap::DiscoveryReason reason) {
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

static WebcamDeviceInfo MakeDevice(std::string id, std::string name) {
    WebcamDeviceInfo d;
    d.id = std::move(id);
    d.name = std::move(name);
    return d;
}

static WebcamDeviceSnapshot MakeSnapshot(std::vector<WebcamDeviceInfo> devs) {
    WebcamDeviceSnapshot snap;
    for (auto& d : devs) {
        snap.devices.push_back(std::move(d));
    }
    return snap;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class WebcamDeviceNotifierTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    static std::unique_ptr<WebcamDeviceNotifier> MakeTestNotifier(WebcamDeviceNotifier::Enumerator enumerator,
                                                                  int debounce_ms = 0) {
        auto n = std::make_unique<WebcamDeviceNotifier>();
        n->setEnumeratorForTest(std::move(enumerator));
        n->setDebounceIntervalMsForTest(debounce_ms);
        return n;
    }
};

// ---------------------------------------------------------------------------
// Test 1: Construct + destroy in test mode — no crash, no Win32 registration
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, ConstructDestroyTestMode_NoCrash) {
    auto notifier = MakeTestNotifier([] { return WebcamDeviceSnapshot{}; });
    // No start() called — just destroy
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 2: simulateNativeEvent(DeviceAdded) with injected enumerator V1 →
//         exactly 1 snapshotChanged with V1 devices, reason=DeviceAdded
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, SimulateDeviceAdded_EmitsSnapshotChanged) {
    const WebcamDeviceSnapshot v1 = MakeSnapshot({MakeDevice("cam-1", "Camera One")});

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();

    ASSERT_EQ(sink.count(), 1);
    ASSERT_EQ(sink.last().snapshot.devices.size(), 1);
    EXPECT_EQ(sink.last().snapshot.devices[0].id, "cam-1");
    EXPECT_EQ(sink.last().snapshot.devices[0].name, "Camera One");
    EXPECT_EQ(sink.last().reason, DiscoveryReason::DeviceAdded);
}

// ---------------------------------------------------------------------------
// Test 3: Burst — 5 rapid events → exactly ONE snapshotChanged after flush
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, BurstEvents_CoalescedToOneEmit) {
    const WebcamDeviceSnapshot v1 = MakeSnapshot({MakeDevice("cam-1", "Camera One")});

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
// Test 4: Identical snapshot → no second emit (dedup)
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, IdenticalSnapshot_NoDuplicateEmit) {
    const WebcamDeviceSnapshot v1 = MakeSnapshot({MakeDevice("cam-1", "Camera One")});

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    // First event: empty → v1, should emit
    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 1);

    // Second event: same snapshot, must NOT emit again
    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    EXPECT_EQ(sink.count(), 1);
}

// ---------------------------------------------------------------------------
// Test 5: Deterministic ordering — unsorted devices → sorted by id
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, DeterministicOrdering_DevicesSortedById) {
    auto enumerator = []() -> WebcamDeviceSnapshot {
        WebcamDeviceSnapshot snap;
        snap.devices.push_back(MakeDevice("zzz-cam", "Z Camera"));
        snap.devices.push_back(MakeDevice("aaa-cam", "A Camera"));
        snap.devices.push_back(MakeDevice("mmm-cam", "M Camera"));
        return snap;
    };

    auto notifier = MakeTestNotifier(enumerator);
    notifier->simulateNativeEvent(DiscoveryReason::Startup);
    notifier->flushPendingForTest();

    const auto snap = notifier->currentSnapshot();
    ASSERT_EQ(snap.devices.size(), 3);
    EXPECT_EQ(snap.devices[0].id, "aaa-cam");
    EXPECT_EQ(snap.devices[1].id, "mmm-cam");
    EXPECT_EQ(snap.devices[2].id, "zzz-cam");
}

// ---------------------------------------------------------------------------
// Test 6a: Simulate after stop() — no crash
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, SimulateAfterStop_NoCrash) {
    const WebcamDeviceSnapshot v1 = MakeSnapshot({MakeDevice("cam-1", "Camera One")});

    auto notifier = MakeTestNotifier([v1] { return v1; });

    notifier->start();
    notifier->stop();

    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 6b: Simulate on destroyed notifier — QPointer prevents UAF
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, SimulateOnDestroyedNotifier_NoUAF) {
    const WebcamDeviceSnapshot v1 = MakeSnapshot({MakeDevice("cam-1", "Camera One")});

    {
        auto notifier = MakeTestNotifier([v1] { return v1; });
        notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
        // notifier destroyed here — QPointer guard in the lambda prevents UAF
    }

    QCoreApplication::processEvents(QEventLoop::AllEvents);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 7: rescan() — immediate emit when changed; no emit when unchanged
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, Rescan_EmitsImmediatelyWhenSnapshotDiffers) {
    const WebcamDeviceSnapshot v1 = MakeSnapshot({MakeDevice("cam-1", "Camera One")});

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    // rescan() is synchronous — no processEvents needed
    notifier->rescan();

    ASSERT_EQ(sink.count(), 1);
    EXPECT_EQ(sink.last().reason, DiscoveryReason::Rescan);
}

TEST_F(WebcamDeviceNotifierTest, Rescan_NoEmitWhenSnapshotUnchanged) {
    const WebcamDeviceSnapshot v1 = MakeSnapshot({MakeDevice("cam-1", "Camera One")});

    auto notifier = MakeTestNotifier([v1] { return v1; });
    notifier->rescan(); // establishes v1 as last_snapshot_

    SignalSink sink;
    sink.connect(notifier.get());
    notifier->rescan(); // same snapshot — must not emit

    EXPECT_EQ(sink.count(), 0);
}

// ---------------------------------------------------------------------------
// Test 8: Device removed — enumerator returns empty → snapshotChanged with
//         empty devices, reason=DeviceRemoved
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, DeviceRemoved_EmitsEmptySnapshotWithCorrectReason) {
    const WebcamDeviceSnapshot v1 = MakeSnapshot({MakeDevice("cam-1", "Camera One")});
    const WebcamDeviceSnapshot empty;

    int call_count = 0;
    auto notifier = MakeTestNotifier(
        [&call_count, v1, empty]() -> WebcamDeviceSnapshot { return (call_count++ == 0) ? v1 : empty; });

    SignalSink sink;
    sink.connect(notifier.get());

    // First event: empty → v1
    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 1);

    // Second event: v1 → empty (device removed)
    notifier->simulateNativeEvent(DiscoveryReason::DeviceRemoved);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 2);

    EXPECT_EQ(sink.last().snapshot.devices.size(), 0);
    EXPECT_EQ(sink.last().reason, DiscoveryReason::DeviceRemoved);
}

// ---------------------------------------------------------------------------
// Test 9: DeviceRemoved reason takes priority when coalescing burst
// ---------------------------------------------------------------------------

TEST_F(WebcamDeviceNotifierTest, DeviceRemovedPriority_KeptDuringBurst) {
    const WebcamDeviceSnapshot v1 = MakeSnapshot({MakeDevice("cam-1", "Camera One")});

    // Use nonzero debounce so timer does not fire between simulate calls.
    auto notifier = MakeTestNotifier([v1] { return v1; }, 10000);

    SignalSink sink;
    sink.connect(notifier.get());

    notifier->simulateNativeEvent(DiscoveryReason::PropertyChanged);
    notifier->simulateNativeEvent(DiscoveryReason::DeviceRemoved);   // should win
    notifier->simulateNativeEvent(DiscoveryReason::PropertyChanged); // must NOT override DeviceRemoved

    // Deliver the queued invokeMethod calls without firing timer
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    // Switch to 0ms debounce and flush
    notifier->setDebounceIntervalMsForTest(0);
    notifier->flushPendingForTest();

    ASSERT_EQ(sink.count(), 1);
    EXPECT_EQ(sink.last().reason, DiscoveryReason::DeviceRemoved);
}

} // namespace
} // namespace exosnap

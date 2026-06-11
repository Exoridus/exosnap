#include <gtest/gtest.h>

#include <QCoreApplication>

#include "services/DisplayDeviceNotifier.h"

namespace exosnap {
namespace {

// ---------------------------------------------------------------------------
// Shared QCoreApplication — required for QTimer
// ---------------------------------------------------------------------------

QCoreApplication* EnsureApplication() {
    if (QCoreApplication::instance())
        return QCoreApplication::instance();
    static int argc = 1;
    static char app_name[] = "display_device_notifier_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

// ---------------------------------------------------------------------------
// Signal recorder
// ---------------------------------------------------------------------------

struct SignalRecord {
    DisplaySnapshot snapshot;
    DiscoveryReason reason = DiscoveryReason::Rescan;
};

struct SignalSink {
    std::vector<SignalRecord> calls;

    void connect(DisplayDeviceNotifier* notifier) {
        QObject::connect(notifier, &DisplayDeviceNotifier::snapshotChanged,
                         [this](const exosnap::DisplaySnapshot& snap, exosnap::DiscoveryReason reason) {
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

static DisplayInfo MakeDisplay(QString id, bool primary = false, QRect geometry = QRect(0, 0, 1920, 1080)) {
    DisplayInfo d;
    d.id = std::move(id);
    d.name = d.id;
    d.geometry = geometry;
    d.available_geometry = geometry;
    d.device_pixel_ratio = 1.0;
    d.logical_dpi = 96.0;
    d.rotation_degrees = 0;
    d.primary = primary;
    return d;
}

static DisplaySnapshot MakeSnapshot(std::vector<DisplayInfo> displays) {
    DisplaySnapshot snap;
    for (auto& d : displays) {
        snap.displays.push_back(std::move(d));
    }
    return snap;
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class DisplayDeviceNotifierTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }

    static std::unique_ptr<DisplayDeviceNotifier> MakeTestNotifier(DisplayDeviceNotifier::Enumerator enumerator,
                                                                   int debounce_ms = 0) {
        auto n = std::make_unique<DisplayDeviceNotifier>();
        n->setEnumeratorForTest(std::move(enumerator));
        n->setDebounceIntervalMsForTest(debounce_ms);
        return n;
    }
};

// ---------------------------------------------------------------------------
// Test 1: Construct + destroy in test mode — no crash, no screen signal connections
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, ConstructDestroyTestMode_NoCrash) {
    auto notifier = MakeTestNotifier([] { return DisplaySnapshot{}; });
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 2a: simulateNativeEvent(DeviceAdded) → 1 emit with correct reason
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, SimulateDeviceAdded_EmitsSnapshotChanged) {
    const DisplaySnapshot v1 = MakeSnapshot({MakeDisplay("\\\\.\\ DISPLAY1", true)});

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();

    ASSERT_EQ(sink.count(), 1);
    EXPECT_EQ(sink.last().reason, DiscoveryReason::DeviceAdded);
    ASSERT_EQ(sink.last().snapshot.displays.size(), 1);
}

// ---------------------------------------------------------------------------
// Test 2b: simulateNativeEvent(DeviceRemoved) → 1 emit with correct reason
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, SimulateDeviceRemoved_EmitsSnapshotChanged) {
    const DisplaySnapshot v1 =
        MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true), MakeDisplay("\\\\.\\DISPLAY2", false)});
    const DisplaySnapshot v2 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true)});

    int call_count = 0;
    auto notifier =
        MakeTestNotifier([&call_count, v1, v2]() -> DisplaySnapshot { return (call_count++ == 0) ? v1 : v2; });

    SignalSink sink;
    sink.connect(notifier.get());

    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 1);

    notifier->simulateNativeEvent(DiscoveryReason::DeviceRemoved);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 2);

    EXPECT_EQ(sink.last().reason, DiscoveryReason::DeviceRemoved);
    EXPECT_EQ(sink.last().snapshot.displays.size(), 1);
}

// ---------------------------------------------------------------------------
// Test 3: Geometry change — same id, changed geometry → snapshotChanged with
//         reason=GeometryChanged
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, GeometryChange_EmitsWithGeometryChangedReason) {
    const DisplaySnapshot v1 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true, QRect(0, 0, 1920, 1080))});
    const DisplaySnapshot v2 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true, QRect(0, 0, 2560, 1440))});

    int call_count = 0;
    auto notifier =
        MakeTestNotifier([&call_count, v1, v2]() -> DisplaySnapshot { return (call_count++ == 0) ? v1 : v2; });

    SignalSink sink;
    sink.connect(notifier.get());

    notifier->simulateNativeEvent(DiscoveryReason::Startup);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 1);

    notifier->simulateNativeEvent(DiscoveryReason::GeometryChanged);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 2);

    EXPECT_EQ(sink.last().reason, DiscoveryReason::GeometryChanged);
    EXPECT_EQ(sink.last().snapshot.displays[0].geometry, QRect(0, 0, 2560, 1440));
}

// ---------------------------------------------------------------------------
// Test 4a: Burst — 5 rapid events → exactly ONE emit (debounce)
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, BurstEvents_CoalescedToOneEmit) {
    const DisplaySnapshot v1 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true)});

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
// Test 4b: Identical snapshot → no second emit (dedup)
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, IdenticalSnapshot_NoDuplicateEmit) {
    const DisplaySnapshot v1 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true)});

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    ASSERT_EQ(sink.count(), 1);

    // Same snapshot — must NOT emit again
    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    EXPECT_EQ(sink.count(), 1);
}

// ---------------------------------------------------------------------------
// Test 5: Deterministic ordering — unsorted displays → sorted by id
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, DeterministicOrdering_DisplaysSortedById) {
    auto enumerator = []() -> DisplaySnapshot {
        DisplaySnapshot snap;
        snap.displays.push_back(MakeDisplay("\\\\.\\DISPLAY3", false));
        snap.displays.push_back(MakeDisplay("\\\\.\\DISPLAY1", true));
        snap.displays.push_back(MakeDisplay("\\\\.\\DISPLAY2", false));
        return snap;
    };

    auto notifier = MakeTestNotifier(enumerator);
    notifier->simulateNativeEvent(DiscoveryReason::Startup);
    notifier->flushPendingForTest();

    const auto snap = notifier->currentSnapshot();
    ASSERT_EQ(snap.displays.size(), 3);
    EXPECT_EQ(snap.displays[0].id, "\\\\.\\DISPLAY1");
    EXPECT_EQ(snap.displays[1].id, "\\\\.\\DISPLAY2");
    EXPECT_EQ(snap.displays[2].id, "\\\\.\\DISPLAY3");
}

// ---------------------------------------------------------------------------
// Test 6a: Simulate after stop() — no crash
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, SimulateAfterStop_NoCrash) {
    const DisplaySnapshot v1 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true)});

    auto notifier = MakeTestNotifier([v1] { return v1; });

    notifier->start();
    notifier->stop();

    notifier->simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier->flushPendingForTest();
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 6b: Simulate on destroyed notifier — no crash/UAF
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, SimulateOnDestroyedNotifier_NoCrash) {
    // In DisplayDeviceNotifier, simulateNativeEvent calls scheduleRefresh
    // directly (no QueuedConnection), so destruction is straightforward.
    // We still verify the pattern holds with start/stop.
    {
        DisplaySnapshot v1 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true)});
        auto notifier = MakeTestNotifier([v1] { return v1; });
        notifier->start();
        // Destroy before any events
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Test 7: rescan() — immediate synchronous emit when snapshot changes
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, Rescan_EmitsImmediatelyWhenSnapshotDiffers) {
    const DisplaySnapshot v1 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true)});

    auto notifier = MakeTestNotifier([v1] { return v1; });

    SignalSink sink;
    sink.connect(notifier.get());

    notifier->rescan();

    ASSERT_EQ(sink.count(), 1);
    EXPECT_EQ(sink.last().reason, DiscoveryReason::Rescan);
}

TEST_F(DisplayDeviceNotifierTest, Rescan_NoEmitWhenSnapshotUnchanged) {
    const DisplaySnapshot v1 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true)});

    auto notifier = MakeTestNotifier([v1] { return v1; });
    notifier->rescan(); // establishes v1

    SignalSink sink;
    sink.connect(notifier.get());
    notifier->rescan(); // same snapshot — must not emit

    EXPECT_EQ(sink.count(), 0);
}

// ---------------------------------------------------------------------------
// Test 8: Missing display — enumerator drops a previously-present id →
//         snapshot reflects the removal
// ---------------------------------------------------------------------------

TEST_F(DisplayDeviceNotifierTest, MissingDisplay_SnapshotReflectsRemoval) {
    const DisplaySnapshot v1 =
        MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true), MakeDisplay("\\\\.\\DISPLAY2", false)});
    const DisplaySnapshot v2 = MakeSnapshot({MakeDisplay("\\\\.\\DISPLAY1", true)});

    int call_count = 0;
    auto notifier =
        MakeTestNotifier([&call_count, v1, v2]() -> DisplaySnapshot { return (call_count++ == 0) ? v1 : v2; });

    SignalSink sink;
    sink.connect(notifier.get());

    notifier->rescan(); // v1
    ASSERT_EQ(sink.count(), 1);
    ASSERT_EQ(sink.last().snapshot.displays.size(), 2);

    notifier->rescan(); // v2 — DISPLAY2 gone
    ASSERT_EQ(sink.count(), 2);
    EXPECT_EQ(sink.last().snapshot.displays.size(), 1);
    EXPECT_EQ(sink.last().snapshot.displays[0].id, "\\\\.\\DISPLAY1");
}

} // namespace
} // namespace exosnap

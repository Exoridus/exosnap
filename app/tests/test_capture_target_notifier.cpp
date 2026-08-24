#include "services/CaptureTargetNotifier.h"

#include <QCoreApplication>

#include <gtest/gtest.h>

namespace exosnap {
namespace {

QCoreApplication* ensureApplication() {
    if (QCoreApplication::instance() != nullptr)
        return QCoreApplication::instance();
    static int argc = 1;
    static char app_name[] = "capture_target_notifier_tests";
    static char* argv[] = {app_name, nullptr};
    static QCoreApplication app(argc, argv);
    return &app;
}

exosnap::engine::CaptureTarget monitor(uintptr_t id, const char* description) {
    return {exosnap::engine::CaptureTarget::Kind::Monitor, id, description};
}

exosnap::engine::CaptureTarget window(uintptr_t id, const char* description) {
    return {exosnap::engine::CaptureTarget::Kind::Window, id, description};
}

TEST(CaptureTargetNotifierTest, RuntimeMonitorAndWindowChangesAreDeduplicatedAndReasoned) {
    ASSERT_NE(ensureApplication(), nullptr);
    CaptureTargetSnapshot current{{monitor(1, "Display 1")}};
    CaptureTargetNotifier notifier;
    notifier.setEnumeratorForTest([&current]() { return current; });
    notifier.setDebounceIntervalMsForTest(0);

    std::vector<std::pair<CaptureTargetSnapshot, DiscoveryReason>> changes;
    QObject::connect(&notifier, &CaptureTargetNotifier::snapshotChanged,
                     [&changes](const CaptureTargetSnapshot& snapshot, DiscoveryReason reason) {
                         changes.emplace_back(snapshot, reason);
                     });
    notifier.start();
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes.back().second, DiscoveryReason::Startup);

    current.targets.push_back(window(20, "Editor"));
    notifier.simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier.flushPendingForTest();
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes.back().first.targets.size(), 2u);
    EXPECT_EQ(changes.back().second, DiscoveryReason::DeviceAdded);

    notifier.simulateNativeEvent(DiscoveryReason::PropertyChanged);
    notifier.flushPendingForTest();
    EXPECT_EQ(changes.size(), 2u);

    current.targets = {monitor(2, "Display 2")};
    notifier.simulateNativeEvent(DiscoveryReason::DeviceRemoved);
    notifier.flushPendingForTest();
    ASSERT_EQ(changes.size(), 3u);
    EXPECT_EQ(changes.back().first.targets.front().native_id, 2);
    EXPECT_EQ(changes.back().second, DiscoveryReason::DeviceRemoved);
}

TEST(CaptureTargetNotifierTest, BurstUsesRemovalAsHighestPriorityReason) {
    ASSERT_NE(ensureApplication(), nullptr);
    CaptureTargetSnapshot current{{monitor(1, "Display 1")}};
    CaptureTargetNotifier notifier;
    notifier.setEnumeratorForTest([&current]() { return current; });
    notifier.setDebounceIntervalMsForTest(1000);
    notifier.start();

    DiscoveryReason last_reason = DiscoveryReason::Startup;
    int changes = 0;
    QObject::connect(&notifier, &CaptureTargetNotifier::snapshotChanged,
                     [&last_reason, &changes](const CaptureTargetSnapshot&, DiscoveryReason reason) {
                         last_reason = reason;
                         ++changes;
                     });
    current.targets.clear();
    notifier.simulateNativeEvent(DiscoveryReason::DeviceAdded);
    notifier.simulateNativeEvent(DiscoveryReason::DeviceRemoved);
    notifier.simulateNativeEvent(DiscoveryReason::PropertyChanged);
    notifier.flushPendingForTest();

    EXPECT_EQ(changes, 1);
    EXPECT_EQ(last_reason, DiscoveryReason::DeviceRemoved);
}

} // namespace
} // namespace exosnap

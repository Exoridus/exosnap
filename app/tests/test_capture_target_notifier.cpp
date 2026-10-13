#include "services/CaptureTargetNotifier.h"

#include <QCoreApplication>
#include <QDateTime>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

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

// A steady stream of window events must not postpone the refresh forever.
//
// Debouncing means restarting the timer on every event, and the desktop produces
// window lifecycle events continuously -- menus, tooltips, splash windows,
// background processes -- so an uncapped restart starves the enumeration and the
// target list keeps describing a desktop that no longer exists. MEASURED before
// the cap: a window created while the app was running stayed unselectable through
// the control channel, while the same window created BEFORE the app started was
// selectable on the first attempt.
TEST(CaptureTargetNotifierTest, AContinuousEventStreamCannotStarveTheRefresh) {
    ASSERT_NE(ensureApplication(), nullptr);
    CaptureTargetSnapshot current{{monitor(1, "Display 1")}};
    CaptureTargetNotifier notifier;
    notifier.setEnumeratorForTest([&current]() { return current; });
    notifier.setDebounceIntervalMsForTest(30);
    notifier.setMaxDeferralMsForTest(150);

    int refreshes = 0;
    QObject::connect(&notifier, &CaptureTargetNotifier::snapshotChanged,
                     [&refreshes](const CaptureTargetSnapshot&, DiscoveryReason) { ++refreshes; });
    notifier.start();
    refreshes = 0;

    // A window appears, and then the desktop keeps producing events faster than the
    // debounce interval for well over the cap.
    current = CaptureTargetSnapshot{{monitor(1, "Display 1"), window(2, "A window that just opened")}};
    const qint64 until = QDateTime::currentMSecsSinceEpoch() + 400;
    while (QDateTime::currentMSecsSinceEpoch() < until) {
        notifier.requestRefreshForTest(DiscoveryReason::DeviceAdded);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 60);

    EXPECT_GT(refreshes, 0) << "the refresh never ran while events kept arriving";
    EXPECT_EQ(notifier.currentSnapshot().targets.size(), 2u) << "the new window never reached the target list";
}

} // namespace
} // namespace exosnap

namespace exosnap {
namespace {
TEST(CaptureTargetNotifierTest, WindowLifetimeWitnessRejectsRecycledHandleWithinSameProcess) {
    ASSERT_NE(ensureApplication(), nullptr);
    CaptureTargetSnapshot current{{window(42, "Document")}};
    CaptureTargetNotifier notifier;
    notifier.setEnumeratorForTest([&current] { return current; });
    notifier.setWindowIdentityObserverForTest([](const auto& target) -> std::optional<WindowCaptureIdentity> {
        return WindowCaptureIdentity{target.native_id, 10, 20, 0, QStringLiteral("application")};
    });
    notifier.start();
    ASSERT_EQ(notifier.currentSnapshot().window_identities.size(), 1U);
    const auto original = notifier.currentSnapshot().window_identities.front();
    notifier.rescan();
    EXPECT_EQ(notifier.currentSnapshot().window_identities.front(), original);
    EXPECT_TRUE(notifier.isCurrent(original));
    notifier.simulateWindowLifecycleForTest(42, true);
    EXPECT_FALSE(notifier.isCurrent(original));
    notifier.simulateWindowLifecycleForTest(42, false);
    notifier.rescan();
    const auto replacement = notifier.currentSnapshot().window_identities.front();
    EXPECT_NE(original.stableIdentity(), replacement.stableIdentity());
    EXPECT_EQ(original.process_id, replacement.process_id);
    EXPECT_EQ(original.process_creation_time, replacement.process_creation_time);
    EXPECT_FALSE(notifier.isCurrent(original));
    EXPECT_TRUE(notifier.isCurrent(replacement));
    notifier.stop();
    EXPECT_FALSE(notifier.isCurrent(replacement));
}
TEST(CaptureTargetNotifierTest, ProcessLifetimeChangesAndUnavailableFactsFailClosed) {
    ASSERT_NE(ensureApplication(), nullptr);
    CaptureTargetNotifier notifier;
    notifier.setEnumeratorForTest([] { return CaptureTargetSnapshot{{window(42, "Document")}}; });
    uint64_t creation = 20;
    bool available = true;
    notifier.setWindowIdentityObserverForTest([&](const auto& target) -> std::optional<WindowCaptureIdentity> {
        if (!available)
            return std::nullopt;
        return WindowCaptureIdentity{target.native_id, 10, creation, 0, QStringLiteral("application")};
    });
    notifier.start();
    const auto original = notifier.currentSnapshot().window_identities.front().stableIdentity();
    ++creation;
    notifier.rescan();
    EXPECT_NE(notifier.currentSnapshot().window_identities.front().stableIdentity(), original);
    available = false;
    notifier.rescan();
    EXPECT_TRUE(notifier.currentSnapshot().window_identities.empty());
}

} // namespace
} // namespace exosnap

namespace exosnap {
TEST(CaptureTargetNotifierTest, NativeWindowWitnessRejectsResetWithoutLifecycleDeliveryAndCleansUp) {
    ASSERT_NE(ensureApplication(), nullptr);
    struct NativeWindow {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        const wchar_t* class_name = L"ExoSnapLifetimeWitnessTestWindow";
        HWND handle = nullptr;
        NativeWindow() {
            WNDCLASSW window_class{};
            window_class.lpfnWndProc = DefWindowProcW;
            window_class.hInstance = instance;
            window_class.lpszClassName = class_name;
            if (RegisterClassW(&window_class) != 0)
                handle = CreateWindowExW(0, class_name, L"Lifetime test", 0, 0, 0, 1, 1, nullptr, nullptr, instance,
                                         nullptr);
        }
        ~NativeWindow() {
            if (handle != nullptr)
                DestroyWindow(handle);
            UnregisterClassW(class_name, instance);
        }
    } native_window;
    ASSERT_NE(native_window.handle, nullptr);
    const uintptr_t native_id = reinterpret_cast<uintptr_t>(native_window.handle);
    CaptureTargetNotifier notifier;
    notifier.setEnumeratorForTest([native_id] { return CaptureTargetSnapshot{{window(native_id, "Lifetime test")}}; });
    notifier.start();
    ASSERT_EQ(notifier.currentSnapshot().window_identities.size(), 1U);
    const auto original = notifier.currentSnapshot().window_identities.front();
    EXPECT_TRUE(notifier.isCurrent(original));
    const auto validate_original = notifier.windowIdentityValidator(original);
    EXPECT_TRUE(validate_original());
    std::wstring property;
    EnumPropsExW(
        native_window.handle,
        [](HWND, LPWSTR name, HANDLE, ULONG_PTR data) -> int {
            if (!IS_INTRESOURCE(name) && std::wstring_view(name).starts_with(L"ExoSnap.WindowLifetime.")) {
                *reinterpret_cast<std::wstring*>(data) = name;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&property));
    ASSERT_FALSE(property.empty());
    ASSERT_NE(RemovePropW(native_window.handle, property.c_str()), nullptr);
    EXPECT_FALSE(notifier.isCurrent(original));
    EXPECT_FALSE(validate_original());
    notifier.rescan();
    ASSERT_EQ(notifier.currentSnapshot().window_identities.size(), 1U);
    const auto replacement = notifier.currentSnapshot().window_identities.front();
    EXPECT_NE(original.stableIdentity(), replacement.stableIdentity());
    EXPECT_TRUE(notifier.isCurrent(replacement));
    const auto validate_replacement = notifier.windowIdentityValidator(replacement);
    EXPECT_TRUE(validate_replacement());
    EXPECT_EQ(original.process_id, replacement.process_id);
    EXPECT_EQ(original.process_creation_time, replacement.process_creation_time);
    notifier.stop();
    EXPECT_FALSE(validate_replacement());
    EXPECT_EQ(GetPropW(native_window.handle, property.c_str()), nullptr);
}
} // namespace exosnap

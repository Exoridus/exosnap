#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QKeySequence>
#include <QString>
#include <array>
#include <map>

#include "services/GlobalHotkeyService.h"

namespace exosnap {
namespace {

// ── Fake registrar ──────────────────────────────────────────────────────────

struct FakeRegistrar : public IHotkeyRegistrar {
    struct Entry {
        unsigned int modifiers = 0;
        unsigned int vk = 0;
    };
    std::map<int, Entry> registrations;
    int fail_next_n = 0; // next N Register calls will return false
    int register_calls = 0;
    int unregister_calls = 0;

    bool Register(int id, unsigned int modifiers, unsigned int vk) override {
        ++register_calls;
        if (fail_next_n > 0) {
            --fail_next_n;
            return false;
        }
        registrations[id] = {modifiers, vk};
        return true;
    }

    void Unregister(int id) override {
        ++unregister_calls;
        registrations.erase(id);
    }

    bool IsRegistered(int id) const {
        return registrations.count(id) > 0;
    }
};

// ── QCoreApplication guard ──────────────────────────────────────────────────

class HotkeyServiceTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char name[] = "hotkey_service_tests";
            static char* argv[] = {name, nullptr};
            static QCoreApplication app(argc, argv);
        }
    }
};

// ── Tests ───────────────────────────────────────────────────────────────────

// 1. Default binding for ToggleRecording is Alt+F9; TogglePause is empty.
TEST_F(HotkeyServiceTest, DefaultBindingsAreCorrect) {
    EXPECT_EQ(GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording), QKeySequence(Qt::ALT | Qt::Key_F9));
    EXPECT_TRUE(GlobalHotkeyService::DefaultBinding(HotkeyAction::TogglePause).isEmpty());
}

// 2. Modifier-only sequence (Ctrl alone) is rejected.
TEST_F(HotkeyServiceTest, ModifierOnlyRejected) {
    const QKeySequence ctrl_only(Qt::ControlModifier | Qt::Key_Control);
    EXPECT_EQ(GlobalHotkeyService::ValidateSequence(ctrl_only), RebindError::ModifierOnly);
}

// 3. Alt+F4 (blocked combo) is rejected.
TEST_F(HotkeyServiceTest, BlockedComboAltF4Rejected) {
    const QKeySequence alt_f4(Qt::AltModifier | Qt::Key_F4);
    EXPECT_EQ(GlobalHotkeyService::ValidateSequence(alt_f4), RebindError::BlockedCombo);
}

// 4. Ctrl+Alt+Delete is rejected.
TEST_F(HotkeyServiceTest, BlockedComboCtrlAltDeleteRejected) {
    const QKeySequence ctrl_alt_del(Qt::ControlModifier | Qt::AltModifier | Qt::Key_Delete);
    EXPECT_EQ(GlobalHotkeyService::ValidateSequence(ctrl_alt_del), RebindError::BlockedCombo);
}

// 5. Valid sequence (Alt+F9) passes validation.
TEST_F(HotkeyServiceTest, ValidSequencePassesValidation) {
    EXPECT_EQ(GlobalHotkeyService::ValidateSequence(QKeySequence(Qt::ALT | Qt::Key_F9)), RebindError::None);
}

// 6. Internal conflict: assigning the same binding to two different actions is detected.
TEST_F(HotkeyServiceTest, InternalConflictDetected) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    const QKeySequence seq(Qt::ALT | Qt::Key_F9);
    // ToggleRecording already has Alt+F9 by default.
    RebindResult result = svc.TrySetBinding(HotkeyAction::TogglePause, seq);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, RebindError::InternalConflict);
    EXPECT_EQ(result.conflict_action, HotkeyAction::ToggleRecording);
}

// 7. Idempotent: assigning the same binding to the same action succeeds without re-registering.
TEST_F(HotkeyServiceTest, IdempotentRebindSucceedsWithoutReregistration) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    const int calls_before = reg.register_calls;
    RebindResult result = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F9));
    EXPECT_TRUE(result.success);
    // No new registration — it was already registered via SetRegistrar.
    EXPECT_EQ(reg.register_calls, calls_before);
}

// 8. Successful rebind unregisters old, registers new, updates binding.
TEST_F(HotkeyServiceTest, SuccessfulRebindUnregistersOldAndRegistersNew) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    const int id = GlobalHotkeyService::Win32IdForAction(HotkeyAction::ToggleRecording);
    EXPECT_TRUE(reg.IsRegistered(id)); // default Alt+F9 was registered by SetRegistrar

    const QKeySequence new_seq(Qt::ALT | Qt::Key_F8);
    RebindResult result = svc.TrySetBinding(HotkeyAction::ToggleRecording, new_seq);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(svc.GetBinding(HotkeyAction::ToggleRecording), new_seq);
    EXPECT_TRUE(reg.IsRegistered(id)); // new binding is registered
}

// 9. Failed registration (external conflict) restores the old binding.
TEST_F(HotkeyServiceTest, FailedRebindRestoresOldBinding) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    const QKeySequence original = svc.GetBinding(HotkeyAction::ToggleRecording);
    reg.fail_next_n = 1; // make the next Register call fail

    RebindResult result = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F8));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, RebindError::ExternalConflict);

    // Binding must be unchanged.
    EXPECT_EQ(svc.GetBinding(HotkeyAction::ToggleRecording), original);

    // Old binding must be re-registered.
    const int id = GlobalHotkeyService::Win32IdForAction(HotkeyAction::ToggleRecording);
    EXPECT_TRUE(reg.IsRegistered(id));
}

// 10. Persistence strings are only updated after successful binding.
TEST_F(HotkeyServiceTest, PersistenceNotUpdatedOnFailure) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    std::array<QString, 4> before{};
    svc.SaveToStrings(before);

    reg.fail_next_n = 1;
    [[maybe_unused]] auto r = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F8));

    std::array<QString, 4> after{};
    svc.SaveToStrings(after);

    EXPECT_EQ(before[0], after[0]); // ToggleRecording unchanged
}

// 11. Unset binding removes Win32 registration and clears the stored sequence.
TEST_F(HotkeyServiceTest, UnsetBindingRemovesRegistration) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    const int id = GlobalHotkeyService::Win32IdForAction(HotkeyAction::ToggleRecording);
    EXPECT_TRUE(reg.IsRegistered(id));

    svc.UnsetBinding(HotkeyAction::ToggleRecording);
    EXPECT_TRUE(svc.GetBinding(HotkeyAction::ToggleRecording).isEmpty());
    EXPECT_FALSE(reg.IsRegistered(id));
}

// 12. Reset to default restores the default binding.
TEST_F(HotkeyServiceTest, ResetToDefaultRestoresDefaultBinding) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    [[maybe_unused]] auto rb = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F8));
    EXPECT_NE(svc.GetBinding(HotkeyAction::ToggleRecording),
              GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording));

    RebindResult result = svc.ResetToDefault(HotkeyAction::ToggleRecording);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(svc.GetBinding(HotkeyAction::ToggleRecording),
              GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording));
}

// 13. Reset all restores all active bindings to defaults.
TEST_F(HotkeyServiceTest, ResetAllToDefaultsRestoresAll) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    [[maybe_unused]] auto r0 =
        svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::AltModifier | Qt::Key_F8));
    [[maybe_unused]] auto r1 =
        svc.TrySetBinding(HotkeyAction::TogglePause, QKeySequence(Qt::AltModifier | Qt::Key_F10));

    svc.ResetAllToDefaults();
    EXPECT_EQ(svc.GetBinding(HotkeyAction::ToggleRecording),
              GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording));
    EXPECT_EQ(svc.GetBinding(HotkeyAction::TogglePause),
              GlobalHotkeyService::DefaultBinding(HotkeyAction::TogglePause));
}

// 14. Invalid persisted string safely falls back to default.
TEST_F(HotkeyServiceTest, InvalidPersistedStringSafelyFallsToDefault) {
    GlobalHotkeyService svc;
    std::array<QString, 4> stored = {QStringLiteral("NOT_A_VALID_SEQUENCE"), QString(), QString(), QString()};
    svc.LoadFromStrings(stored);
    // Invalid string: should parse to empty via QKeySequence, then fall through to default.
    // QKeySequence::fromString returns empty for completely unknown strings.
    // (The actual behavior: if it can't parse it, isEmpty() is true → use default.)
    // So the binding should be the default.
    EXPECT_EQ(svc.GetBinding(HotkeyAction::ToggleRecording),
              GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording));
}

// 15. LoadFromStrings with valid portable-text binding is correctly restored.
TEST_F(HotkeyServiceTest, LoadFromStringsRestoresValidBinding) {
    GlobalHotkeyService svc;
    std::array<QString, 4> stored = {QStringLiteral("Ctrl+Shift+R"), QString(), QString(), QString()};
    svc.LoadFromStrings(stored);
    EXPECT_EQ(svc.GetBinding(HotkeyAction::ToggleRecording),
              QKeySequence::fromString(QStringLiteral("Ctrl+Shift+R"), QKeySequence::PortableText));
}

// 16. Win32 IDs are stable (ToggleRecording=1, TogglePause=2) for WM_HOTKEY dispatch.
TEST_F(HotkeyServiceTest, Win32IdsAreStable) {
    EXPECT_EQ(GlobalHotkeyService::Win32IdForAction(HotkeyAction::ToggleRecording), 1);
    EXPECT_EQ(GlobalHotkeyService::Win32IdForAction(HotkeyAction::TogglePause), 2);
}

// 17. bindingChanged signal is emitted on successful TrySetBinding.
TEST_F(HotkeyServiceTest, SignalEmittedOnSuccess) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    int signal_count = 0;
    QObject::connect(&svc, &GlobalHotkeyService::bindingChanged, &svc,
                     [&signal_count](HotkeyAction, QKeySequence) { ++signal_count; });

    [[maybe_unused]] auto rsig =
        svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::AltModifier | Qt::Key_F8));
    EXPECT_EQ(signal_count, 1);
}

// 18. bindingChanged signal is NOT emitted on failed TrySetBinding.
TEST_F(HotkeyServiceTest, SignalNotEmittedOnFailure) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    int signal_count = 0;
    QObject::connect(&svc, &GlobalHotkeyService::bindingChanged, &svc,
                     [&signal_count](HotkeyAction, QKeySequence) { ++signal_count; });

    reg.fail_next_n = 1;
    [[maybe_unused]] auto rfail =
        svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::AltModifier | Qt::Key_F8));
    EXPECT_EQ(signal_count, 0);
}

// 19. Resetting TogglePause to its default yields an empty (unset) binding.
//     Canonical Pause default is Unset per hotkeys-view.md: "all others unset".
TEST_F(HotkeyServiceTest, ResetPauseToDefaultIsEmpty) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    svc.SetRegistrar(&reg);

    // First set Pause to a non-empty binding.
    const QKeySequence alt_f10(Qt::AltModifier | Qt::Key_F10);
    RebindResult r = svc.TrySetBinding(HotkeyAction::TogglePause, alt_f10);
    ASSERT_TRUE(r.success);
    ASSERT_EQ(svc.GetBinding(HotkeyAction::TogglePause), alt_f10);

    // Reset must restore the canonical default, which is empty.
    RebindResult reset_r = svc.ResetToDefault(HotkeyAction::TogglePause);
    EXPECT_TRUE(reset_r.success);
    EXPECT_TRUE(svc.GetBinding(HotkeyAction::TogglePause).isEmpty());
}

// 20. An invalid persisted Pause string falls back to the default (empty / unset),
//     not to any specific key such as Alt+F10.
TEST_F(HotkeyServiceTest, InvalidPersistedPauseStringFallsToEmpty) {
    GlobalHotkeyService svc;
    std::array<QString, 4> stored = {QString(), QStringLiteral("NOT_VALID_SEQUENCE"), QString(), QString()};
    svc.LoadFromStrings(stored);
    EXPECT_TRUE(svc.GetBinding(HotkeyAction::TogglePause).isEmpty());
}

} // namespace
} // namespace exosnap

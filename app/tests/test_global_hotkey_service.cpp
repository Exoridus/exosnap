#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QKeySequence>
#include <QString>
#include <array>
#include <map>
#include <vector>

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

// 1. Every action ships with no default binding.
TEST_F(HotkeyServiceTest, DefaultBindingsAreCorrect) {
    EXPECT_TRUE(GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording).isEmpty());
    EXPECT_TRUE(GlobalHotkeyService::DefaultBinding(HotkeyAction::TogglePause).isEmpty());
    EXPECT_TRUE(GlobalHotkeyService::DefaultBinding(HotkeyAction::CaptureFrame).isEmpty());
    EXPECT_TRUE(GlobalHotkeyService::DefaultBinding(HotkeyAction::AddMarker).isEmpty());
    EXPECT_TRUE(GlobalHotkeyService::DefaultBinding(HotkeyAction::SplitRecording).isEmpty());
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
    (void)svc.SetRegistrar(&reg);

    const QKeySequence seq(Qt::ALT | Qt::Key_F9);
    [[maybe_unused]] auto setup = svc.TrySetBinding(HotkeyAction::ToggleRecording, seq);
    ASSERT_TRUE(setup.success);

    RebindResult result = svc.TrySetBinding(HotkeyAction::TogglePause, seq);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error, RebindError::InternalConflict);
    EXPECT_EQ(result.conflict_action, HotkeyAction::ToggleRecording);
}

// 7. Idempotent: assigning the same binding to the same action succeeds without re-registering.
TEST_F(HotkeyServiceTest, IdempotentRebindSucceedsWithoutReregistration) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    (void)svc.SetRegistrar(&reg);

    const QKeySequence seq(Qt::ALT | Qt::Key_F9);
    [[maybe_unused]] auto setup = svc.TrySetBinding(HotkeyAction::ToggleRecording, seq);
    ASSERT_TRUE(setup.success);

    const int calls_before = reg.register_calls;
    RebindResult result = svc.TrySetBinding(HotkeyAction::ToggleRecording, seq);
    EXPECT_TRUE(result.success);
    // No new registration — it was already registered by the setup call above.
    EXPECT_EQ(reg.register_calls, calls_before);
}

// 8. Successful rebind unregisters old, registers new, updates binding.
TEST_F(HotkeyServiceTest, SuccessfulRebindUnregistersOldAndRegistersNew) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    (void)svc.SetRegistrar(&reg);

    const int id = GlobalHotkeyService::Win32IdForAction(HotkeyAction::ToggleRecording);
    [[maybe_unused]] auto setup = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F9));
    ASSERT_TRUE(setup.success);
    EXPECT_TRUE(reg.IsRegistered(id)); // initial binding is registered

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
    (void)svc.SetRegistrar(&reg);

    const QKeySequence original(Qt::ALT | Qt::Key_F9);
    [[maybe_unused]] auto setup = svc.TrySetBinding(HotkeyAction::ToggleRecording, original);
    ASSERT_TRUE(setup.success);

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

// 9b. SetRegistrar reports which persisted bindings could not be registered
// (e.g. another app already holds the shortcut at startup) instead of
// silently swallowing the failure.
TEST_F(HotkeyServiceTest, SetRegistrarReportsFailedBindings) {
    GlobalHotkeyService svc;
    // No action ships with a default binding, so give one an explicit binding
    // before the registrar is attached — this is the pre-registrar path a
    // persisted custom binding takes at real startup, and it gives
    // SetRegistrar something non-empty to attempt registering below.
    const QKeySequence custom(Qt::ALT | Qt::Key_F9);
    [[maybe_unused]] auto setup = svc.TrySetBinding(HotkeyAction::ToggleRecording, custom);
    ASSERT_TRUE(setup.success);

    FakeRegistrar reg;
    reg.fail_next_n = 1; // make its Register() call fail to simulate an external conflict at startup

    const std::vector<HotkeyAction> failed = svc.SetRegistrar(&reg);
    ASSERT_EQ(failed.size(), 1u);
    EXPECT_EQ(failed[0], HotkeyAction::ToggleRecording);

    // The binding model itself is untouched — SetRegistrar only reports, it
    // never mutates state on failure.
    EXPECT_EQ(svc.GetBinding(HotkeyAction::ToggleRecording), custom);
}

// IsAtDefault distinguishes a still-default binding from one the user
// explicitly customized -- the settings UI uses this to decide whether an
// action's control should offer Reset (back to default) as distinct from
// Clear.
TEST_F(HotkeyServiceTest, IsAtDefaultReflectsCustomizationState) {
    GlobalHotkeyService svc;
    EXPECT_TRUE(svc.IsAtDefault(HotkeyAction::ToggleRecording)); // untouched: empty default
    EXPECT_TRUE(svc.IsAtDefault(HotkeyAction::TogglePause));     // untouched: empty default

    FakeRegistrar reg;
    (void)svc.SetRegistrar(&reg);
    [[maybe_unused]] auto r = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F8));
    ASSERT_TRUE(r.success);
    EXPECT_FALSE(svc.IsAtDefault(HotkeyAction::ToggleRecording)); // now customized away from empty

    [[maybe_unused]] auto reset = svc.ResetToDefault(HotkeyAction::ToggleRecording);
    EXPECT_TRUE(svc.IsAtDefault(HotkeyAction::ToggleRecording)); // back to default (empty)
}

// 9c. SetRegistrar reports nothing when every registration succeeds.
TEST_F(HotkeyServiceTest, SetRegistrarReportsNoFailuresOnSuccess) {
    GlobalHotkeyService svc;
    [[maybe_unused]] auto setup = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F9));
    ASSERT_TRUE(setup.success);

    FakeRegistrar reg;
    const std::vector<HotkeyAction> failed = svc.SetRegistrar(&reg);
    EXPECT_TRUE(failed.empty());
}

// 10. Persistence strings are only updated after successful binding.
TEST_F(HotkeyServiceTest, PersistenceNotUpdatedOnFailure) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    (void)svc.SetRegistrar(&reg);

    HotkeyBindings before{};
    svc.SaveToStrings(before);

    reg.fail_next_n = 1;
    [[maybe_unused]] auto r = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F8));

    HotkeyBindings after{};
    svc.SaveToStrings(after);

    EXPECT_EQ(before[0], after[0]); // ToggleRecording unchanged
}

// 11. Unset binding removes Win32 registration and clears the stored sequence.
TEST_F(HotkeyServiceTest, UnsetBindingRemovesRegistration) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    (void)svc.SetRegistrar(&reg);

    const int id = GlobalHotkeyService::Win32IdForAction(HotkeyAction::ToggleRecording);
    [[maybe_unused]] auto setup = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F9));
    ASSERT_TRUE(setup.success);
    ASSERT_TRUE(reg.IsRegistered(id));

    svc.UnsetBinding(HotkeyAction::ToggleRecording);
    EXPECT_TRUE(svc.GetBinding(HotkeyAction::ToggleRecording).isEmpty());
    EXPECT_FALSE(reg.IsRegistered(id));
}

// 12. Reset to default restores the default binding.
TEST_F(HotkeyServiceTest, ResetToDefaultRestoresDefaultBinding) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    (void)svc.SetRegistrar(&reg);

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
    (void)svc.SetRegistrar(&reg);

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
    HotkeyBindings stored = {QStringLiteral("NOT_A_VALID_SEQUENCE"), QString(), QString(), QString(), QString()};
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
    HotkeyBindings stored = {QStringLiteral("Ctrl+Shift+R"), QString(), QString(), QString(), QString()};
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
    (void)svc.SetRegistrar(&reg);

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
    (void)svc.SetRegistrar(&reg);

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
    (void)svc.SetRegistrar(&reg);

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
    HotkeyBindings stored = {QString(), QStringLiteral("NOT_VALID_SEQUENCE"), QString(), QString(), QString()};
    svc.LoadFromStrings(stored);
    EXPECT_TRUE(svc.GetBinding(HotkeyAction::TogglePause).isEmpty());
}

// 20b. An explicitly unset binding round-trips as unset — it must NOT fall back to
//      the action default on reload. The sentinel is what lets a shortcut the user
//      cleared (or that startup had to drop because another app already held it)
//      stay cleared across launches instead of silently reappearing.
TEST_F(HotkeyServiceTest, UnsetBindingRoundTripsAsUnsetNotDefault) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    (void)svc.SetRegistrar(&reg);

    [[maybe_unused]] auto setup = svc.TrySetBinding(HotkeyAction::ToggleRecording, QKeySequence(Qt::ALT | Qt::Key_F9));
    ASSERT_TRUE(setup.success);

    svc.UnsetBinding(HotkeyAction::ToggleRecording);
    ASSERT_TRUE(svc.GetBinding(HotkeyAction::ToggleRecording).isEmpty());

    HotkeyBindings saved{};
    svc.SaveToStrings(saved);
    // An explicitly-unset binding must persist as a non-empty sentinel, otherwise an
    // empty string would reload as the action's (now also empty) default and lose
    // the distinction between "never customized" and "customized, then cleared".
    EXPECT_FALSE(saved[0].trimmed().isEmpty());

    GlobalHotkeyService reloaded;
    reloaded.LoadFromStrings(saved);
    EXPECT_TRUE(reloaded.GetBinding(HotkeyAction::ToggleRecording).isEmpty());
}

// 20c. A genuinely empty persisted string (fresh settings, never saved) still yields
//      the action default — the sentinel is the ONLY "stay unset" marker, so first-run
//      defaults are preserved.
TEST_F(HotkeyServiceTest, EmptyPersistedStringStillYieldsDefault) {
    GlobalHotkeyService svc;
    HotkeyBindings stored = {QString(), QString(), QString(), QString(), QString()};
    svc.LoadFromStrings(stored);
    EXPECT_EQ(svc.GetBinding(HotkeyAction::ToggleRecording),
              GlobalHotkeyService::DefaultBinding(HotkeyAction::ToggleRecording));
}

// 21. SplitRecording is unset by default (no default binding per SPLIT-RECORDING-R1).
TEST_F(HotkeyServiceTest, SplitRecordingUnsetByDefault) {
    EXPECT_TRUE(GlobalHotkeyService::DefaultBinding(HotkeyAction::SplitRecording).isEmpty());
    GlobalHotkeyService svc;
    EXPECT_TRUE(svc.GetBinding(HotkeyAction::SplitRecording).isEmpty());
}

// 22. SplitRecording participates in conflict detection and rebinds on its own path.
TEST_F(HotkeyServiceTest, SplitRecordingRebindAndConflict) {
    GlobalHotkeyService svc;
    FakeRegistrar reg;
    (void)svc.SetRegistrar(&reg);

    const QKeySequence seq(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_S);
    RebindResult ok = svc.TrySetBinding(HotkeyAction::SplitRecording, seq);
    ASSERT_TRUE(ok.success);
    EXPECT_EQ(svc.GetBinding(HotkeyAction::SplitRecording), seq);
    EXPECT_TRUE(reg.IsRegistered(GlobalHotkeyService::Win32IdForAction(HotkeyAction::SplitRecording)));

    // Assigning the same binding to AddMarker must be reported as a conflict against SplitRecording.
    RebindResult conflict = svc.TrySetBinding(HotkeyAction::AddMarker, seq);
    EXPECT_FALSE(conflict.success);
    EXPECT_EQ(conflict.error, RebindError::InternalConflict);
    EXPECT_EQ(conflict.conflict_action, HotkeyAction::SplitRecording);
}

// 23. The action count and Win32 id for SplitRecording are stable.
TEST_F(HotkeyServiceTest, SplitRecordingActionIndexStable) {
    EXPECT_EQ(kHotkeyActionCount, 5);
    EXPECT_EQ(static_cast<int>(HotkeyAction::SplitRecording), 4);
    EXPECT_EQ(GlobalHotkeyService::Win32IdForAction(HotkeyAction::SplitRecording), 5);
}

} // namespace
} // namespace exosnap

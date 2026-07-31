#pragma once
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <array>
#include <vector>

namespace exosnap {

enum class HotkeyAction : int {
    ToggleRecording = 0,
    TogglePause = 1,
    CaptureFrame = 2,
    AddMarker = 3,
    SplitRecording = 4,
};

constexpr int kHotkeyActionCount = 5;

// Persistence vector for all action bindings, indexed by HotkeyAction. Sized by
// kHotkeyActionCount so adding an action keeps every call site in sync.
using HotkeyBindings = std::array<QString, static_cast<std::size_t>(kHotkeyActionCount)>;

enum class RebindError {
    None,
    ModifierOnly,     // no non-modifier key pressed
    BlockedCombo,     // e.g. Alt+F4, Ctrl+Alt+Del
    InternalConflict, // another ExoSnap action owns this binding
    ExternalConflict, // RegisterHotKey failed (Windows/other app)
    UnsupportedKey,   // key has no Win32 VK mapping
};

struct RebindResult {
    bool success = false;
    RebindError error = RebindError::None;
    // Filled when error == InternalConflict
    HotkeyAction conflict_action = HotkeyAction::ToggleRecording;
    QString error_message;
};

// Abstraction over Win32 RegisterHotKey / UnregisterHotKey.
// Tests provide a fake implementation; production uses Win32HotkeyRegistrar.
class IHotkeyRegistrar {
  public:
    virtual ~IHotkeyRegistrar() = default;
    virtual bool Register(int id, unsigned int modifiers, unsigned int vk) = 0;
    virtual void Unregister(int id) = 0;
};

// Not internally synchronized: every public method must be called from the
// thread that owns this QObject (the thread SetRegistrar's IHotkeyRegistrar is
// also driven from). This isn't an implementation shortcut — Win32
// RegisterHotKey/UnregisterHotKey deliver WM_HOTKEY to, and must be called
// from, the thread pumping the associated message loop, so cross-thread calls
// would be wrong even with a mutex around bindings_. Enforced by an assert
// (debug builds) at every public entry point rather than left implicit.
class GlobalHotkeyService : public QObject {
    Q_OBJECT
  public:
    explicit GlobalHotkeyService(QObject* parent = nullptr);

    // Called once the HWND is available; registers all current bindings.
    // Returns the actions whose persisted binding could not be (re-)registered
    // (e.g. already held by Windows or another application) so the caller can
    // surface it — this stays as UI/log-agnostic as TrySetBinding: it reports
    // structured failures instead of logging or notifying itself.
    [[nodiscard]] std::vector<HotkeyAction> SetRegistrar(IHotkeyRegistrar* registrar);

    // Attempt to set/change a binding.
    // Validates, checks conflicts, attempts Win32 registration with rollback.
    // Never persists on failure.
    [[nodiscard]] RebindResult TrySetBinding(HotkeyAction action, QKeySequence seq);

    // Remove binding for action. Always succeeds.
    void UnsetBinding(HotkeyAction action);

    // Reset one action to its default binding.
    [[nodiscard]] RebindResult ResetToDefault(HotkeyAction action);

    // Reset all active bindings to defaults.
    void ResetAllToDefaults();

    [[nodiscard]] QKeySequence GetBinding(HotkeyAction action) const;

    // True when action's current binding is exactly the shipped default (never
    // customized, or customized back to the same value -- either way it's the
    // combo ExoSnap ships out of the box, not a deliberate user choice of
    // something else). Used to decide whether a startup registration failure
    // is worth interrupting the user about, or just quiet environmental noise
    // (another app's own default claimed the same combo first).
    [[nodiscard]] bool IsAtDefault(HotkeyAction action) const;

    // Load from stored strings (e.g. AppSettingsStore).
    // Invalid or empty strings fall back to defaults.
    void LoadFromStrings(const HotkeyBindings& stored);

    // Write current bindings to stored strings.
    void SaveToStrings(HotkeyBindings& out) const;

    static QKeySequence DefaultBinding(HotkeyAction action);
    static QString ActionDisplayName(HotkeyAction action);
    static int Win32IdForAction(HotkeyAction action);

    // Validate a key sequence without attempting registration.
    static RebindError ValidateSequence(QKeySequence seq);

  signals:
    // Emitted only after a successful commit (set, unset, or reset).
    void bindingChanged(exosnap::HotkeyAction action, QKeySequence seq);

  private:
    void CommitBinding(HotkeyAction action, QKeySequence seq);
    RebindResult AttemptRegistration(HotkeyAction action, QKeySequence new_seq, QKeySequence old_seq);
    // Asserts the calling thread is this object's owning thread — see the class
    // comment above. No-op in release builds (matches Q_ASSERT).
    void AssertOwnerThread() const;

    IHotkeyRegistrar* registrar_ = nullptr;
    std::array<QKeySequence, static_cast<std::size_t>(kHotkeyActionCount)> bindings_{};
};

} // namespace exosnap

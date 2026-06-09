#pragma once
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <array>

namespace exosnap {

enum class HotkeyAction : int {
    ToggleRecording = 0,
    TogglePause = 1,
};

constexpr int kHotkeyActionCount = 2;

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

class GlobalHotkeyService : public QObject {
    Q_OBJECT
  public:
    explicit GlobalHotkeyService(QObject* parent = nullptr);

    // Called once the HWND is available; registers all current bindings.
    void SetRegistrar(IHotkeyRegistrar* registrar);

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

    // Load from stored strings (e.g. AppSettingsStore).
    // Invalid or empty strings fall back to defaults.
    void LoadFromStrings(const std::array<QString, 4>& stored);

    // Write current bindings to stored strings.
    void SaveToStrings(std::array<QString, 4>& out) const;

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

    IHotkeyRegistrar* registrar_ = nullptr;
    std::array<QKeySequence, kHotkeyActionCount> bindings_{};
};

} // namespace exosnap

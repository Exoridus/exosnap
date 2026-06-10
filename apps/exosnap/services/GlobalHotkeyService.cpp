#include "GlobalHotkeyService.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace exosnap {
namespace {

#if defined(Q_OS_WIN)
UINT QtModifiersToWin32(Qt::KeyboardModifiers mods) {
    UINT result = MOD_NOREPEAT;
    if (mods & Qt::AltModifier)
        result |= MOD_ALT;
    if (mods & Qt::ControlModifier)
        result |= MOD_CONTROL;
    if (mods & Qt::ShiftModifier)
        result |= MOD_SHIFT;
    if (mods & Qt::MetaModifier)
        result |= MOD_WIN;
    return result;
}

UINT QtKeyToVk(Qt::Key key) {
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return static_cast<UINT>(VK_F1 + (key - Qt::Key_F1));
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return static_cast<UINT>('A' + (key - Qt::Key_A));
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return static_cast<UINT>('0' + (key - Qt::Key_0));
    switch (key) {
    case Qt::Key_Space:
        return VK_SPACE;
    case Qt::Key_Tab:
        return VK_TAB;
    case Qt::Key_Return:
        return VK_RETURN;
    case Qt::Key_Escape:
        return VK_ESCAPE;
    case Qt::Key_Backspace:
        return VK_BACK;
    case Qt::Key_Delete:
        return VK_DELETE;
    case Qt::Key_Insert:
        return VK_INSERT;
    case Qt::Key_Home:
        return VK_HOME;
    case Qt::Key_End:
        return VK_END;
    case Qt::Key_PageUp:
        return VK_PRIOR;
    case Qt::Key_PageDown:
        return VK_NEXT;
    case Qt::Key_Left:
        return VK_LEFT;
    case Qt::Key_Right:
        return VK_RIGHT;
    case Qt::Key_Up:
        return VK_UP;
    case Qt::Key_Down:
        return VK_DOWN;
    case Qt::Key_Print:
        return VK_PRINT;
    case Qt::Key_Pause:
        return VK_PAUSE;
    case Qt::Key_CapsLock:
        return VK_CAPITAL;
    case Qt::Key_NumLock:
        return VK_NUMLOCK;
    case Qt::Key_ScrollLock:
        return VK_SCROLL;
    default:
        return 0;
    }
}
#endif

bool IsModifierKey(Qt::Key key) {
    switch (key) {
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Shift:
    case Qt::Key_Meta:
    case Qt::Key_AltGr:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
        return true;
    default:
        return false;
    }
}

bool IsBlockedCombo(Qt::KeyboardModifiers mods, Qt::Key key) {
    // Alt+F4 — close window
    if (mods == Qt::AltModifier && key == Qt::Key_F4)
        return true;
    // Ctrl+Alt+Delete — system reserved
    if (mods == (Qt::ControlModifier | Qt::AltModifier) && key == Qt::Key_Delete)
        return true;
    return false;
}

QString BuildConflictMessage(HotkeyAction conflicting, QKeySequence seq) {
    return QStringLiteral("%1 is already assigned to %2.")
        .arg(seq.toString(QKeySequence::NativeText), GlobalHotkeyService::ActionDisplayName(conflicting));
}

} // namespace

GlobalHotkeyService::GlobalHotkeyService(QObject* parent) : QObject(parent) {
    // Initialise defaults — registrar is not yet available.
    bindings_[0] = DefaultBinding(HotkeyAction::ToggleRecording);
    bindings_[1] = DefaultBinding(HotkeyAction::TogglePause);
    bindings_[2] = DefaultBinding(HotkeyAction::CaptureFrame);
    bindings_[3] = DefaultBinding(HotkeyAction::AddMarker);
    bindings_[4] = DefaultBinding(HotkeyAction::SplitRecording);
}

void GlobalHotkeyService::SetRegistrar(IHotkeyRegistrar* registrar) {
    registrar_ = registrar;
    if (!registrar_)
        return;
    // Register all current non-empty bindings now that we have a HWND.
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        const auto action = static_cast<HotkeyAction>(i);
        const QKeySequence& seq = bindings_[static_cast<std::size_t>(i)];
        if (seq.isEmpty())
            continue;
#if defined(Q_OS_WIN)
        QKeyCombination combo = seq[0];
        UINT vk = QtKeyToVk(combo.key());
        if (vk == 0)
            continue;
        registrar_->Register(Win32IdForAction(action), QtModifiersToWin32(combo.keyboardModifiers()), vk);
#endif
    }
}

RebindResult GlobalHotkeyService::TrySetBinding(HotkeyAction action, QKeySequence seq) {
    const int idx = static_cast<int>(action);
    if (idx < 0 || idx >= kHotkeyActionCount)
        return {false, RebindError::ExternalConflict, action, QStringLiteral("Unknown action")};

    // 1. Validate
    if (!seq.isEmpty()) {
        RebindError err = ValidateSequence(seq);
        if (err != RebindError::None) {
            QString msg;
            if (err == RebindError::ModifierOnly)
                msg = QStringLiteral("Press a key combination with at least one non-modifier key.");
            else if (err == RebindError::BlockedCombo)
                msg = QStringLiteral("This key combination is reserved and cannot be used.");
            else if (err == RebindError::UnsupportedKey)
                msg = QStringLiteral("This key is not supported for global shortcuts.");
            return {false, err, action, msg};
        }
    }

    // 2. Internal conflict check
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        if (i == idx)
            continue;
        if (!seq.isEmpty() && bindings_[static_cast<std::size_t>(i)] == seq) {
            auto conflicting = static_cast<HotkeyAction>(i);
            return {false, RebindError::InternalConflict, conflicting, BuildConflictMessage(conflicting, seq)};
        }
    }

    // 3. Idempotent: same binding as current
    if (bindings_[static_cast<std::size_t>(idx)] == seq) {
        return {true, RebindError::None, action, {}};
    }

    // 4. Attempt registration (with rollback on failure)
    QKeySequence old_seq = bindings_[static_cast<std::size_t>(idx)];
    RebindResult result = AttemptRegistration(action, seq, old_seq);
    if (!result.success)
        return result;

    // 5. Commit — update model and notify (persistence happens via signal in MainWindow)
    CommitBinding(action, seq);
    return {true, RebindError::None, action, {}};
}

void GlobalHotkeyService::UnsetBinding(HotkeyAction action) {
    const int idx = static_cast<int>(action);
    if (idx < 0 || idx >= kHotkeyActionCount)
        return;
    if (bindings_[static_cast<std::size_t>(idx)].isEmpty())
        return;

    if (registrar_) {
#if defined(Q_OS_WIN)
        registrar_->Unregister(Win32IdForAction(action));
#endif
    }
    CommitBinding(action, QKeySequence());
}

RebindResult GlobalHotkeyService::ResetToDefault(HotkeyAction action) {
    return TrySetBinding(action, DefaultBinding(action));
}

void GlobalHotkeyService::ResetAllToDefaults() {
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        [[maybe_unused]] auto r = ResetToDefault(static_cast<HotkeyAction>(i));
    }
}

QKeySequence GlobalHotkeyService::GetBinding(HotkeyAction action) const {
    const int idx = static_cast<int>(action);
    if (idx < 0 || idx >= kHotkeyActionCount)
        return {};
    return bindings_[static_cast<std::size_t>(idx)];
}

void GlobalHotkeyService::LoadFromStrings(const HotkeyBindings& stored) {
    // Load every registered action; empty stored strings fall back to defaults.
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        const HotkeyAction action = static_cast<HotkeyAction>(i);
        const QString& s = stored[static_cast<std::size_t>(i)];
        if (s.trimmed().isEmpty()) {
            bindings_[static_cast<std::size_t>(i)] = DefaultBinding(action);
            continue;
        }
        QKeySequence seq = QKeySequence::fromString(s, QKeySequence::PortableText);
        // Accept empty sequence (means "unset") only if original string was empty.
        // Validate non-empty parsed sequences; fall back to default on any error.
        if (!seq.isEmpty() && ValidateSequence(seq) == RebindError::None) {
            bindings_[static_cast<std::size_t>(i)] = seq;
        } else if (seq.isEmpty()) {
            // Unparseable string — fall back to default.
            bindings_[static_cast<std::size_t>(i)] = DefaultBinding(action);
        } else {
            // Parsed but invalid (e.g. unsupported key, blocked combo) — fall back to default.
            bindings_[static_cast<std::size_t>(i)] = DefaultBinding(action);
        }
    }
}

void GlobalHotkeyService::SaveToStrings(HotkeyBindings& out) const {
    for (int i = 0; i < kHotkeyActionCount; ++i) {
        out[static_cast<std::size_t>(i)] = bindings_[static_cast<std::size_t>(i)].toString(QKeySequence::PortableText);
    }
}

QKeySequence GlobalHotkeyService::DefaultBinding(HotkeyAction action) {
    switch (action) {
    case HotkeyAction::ToggleRecording:
        return QKeySequence(Qt::ALT | Qt::Key_F9);
    case HotkeyAction::TogglePause:
        return QKeySequence();
    case HotkeyAction::CaptureFrame:
        return QKeySequence(); // no default binding per spec
    case HotkeyAction::AddMarker:
        return QKeySequence(); // no default binding per spec
    case HotkeyAction::SplitRecording:
        return QKeySequence(); // unset by default per SPLIT-RECORDING-R1
    }
    return {};
}

QString GlobalHotkeyService::ActionDisplayName(HotkeyAction action) {
    switch (action) {
    case HotkeyAction::ToggleRecording:
        return QStringLiteral("Start / Stop recording");
    case HotkeyAction::TogglePause:
        return QStringLiteral("Pause / Resume");
    case HotkeyAction::CaptureFrame:
        return QStringLiteral("Capture frame");
    case HotkeyAction::AddMarker:
        return QStringLiteral("Add marker");
    case HotkeyAction::SplitRecording:
        return QStringLiteral("Split recording");
    }
    return QStringLiteral("Unknown");
}

int GlobalHotkeyService::Win32IdForAction(HotkeyAction action) {
    return static_cast<int>(action) + 1; // IDs are 1-based: ToggleRecording=1, TogglePause=2, CaptureFrame=3
}

RebindError GlobalHotkeyService::ValidateSequence(QKeySequence seq) {
    if (seq.isEmpty())
        return RebindError::None;
    QKeyCombination combo = seq[0];
    Qt::Key key = combo.key();
    Qt::KeyboardModifiers mods = combo.keyboardModifiers();

    if (IsModifierKey(key))
        return RebindError::ModifierOnly;
    if (IsBlockedCombo(mods, key))
        return RebindError::BlockedCombo;

#if defined(Q_OS_WIN)
    if (QtKeyToVk(key) == 0)
        return RebindError::UnsupportedKey;
#endif

    return RebindError::None;
}

void GlobalHotkeyService::CommitBinding(HotkeyAction action, QKeySequence seq) {
    bindings_[static_cast<std::size_t>(static_cast<int>(action))] = seq;
    emit bindingChanged(action, seq);
}

RebindResult GlobalHotkeyService::AttemptRegistration(HotkeyAction action, QKeySequence new_seq, QKeySequence old_seq) {
    if (!registrar_) {
        // No registrar yet (pre-show phase): accept without Win32 call.
        return {true, RebindError::None, action, {}};
    }

#if defined(Q_OS_WIN)
    const int win32_id = Win32IdForAction(action);

    // Always unregister the old binding first.
    registrar_->Unregister(win32_id);

    if (new_seq.isEmpty()) {
        // Unset path — no registration needed.
        return {true, RebindError::None, action, {}};
    }

    QKeyCombination combo = new_seq[0];
    UINT vk = QtKeyToVk(combo.key());
    if (vk == 0) {
        // Restore old binding.
        if (!old_seq.isEmpty()) {
            QKeyCombination old_combo = old_seq[0];
            UINT old_vk = QtKeyToVk(old_combo.key());
            if (old_vk != 0)
                registrar_->Register(win32_id, QtModifiersToWin32(old_combo.keyboardModifiers()), old_vk);
        }
        return {false, RebindError::UnsupportedKey, action,
                QStringLiteral("This key is not supported for global shortcuts.")};
    }

    if (!registrar_->Register(win32_id, QtModifiersToWin32(combo.keyboardModifiers()), vk)) {
        // Registration failed — restore old binding.
        if (!old_seq.isEmpty()) {
            QKeyCombination old_combo = old_seq[0];
            UINT old_vk = QtKeyToVk(old_combo.key());
            if (old_vk != 0)
                registrar_->Register(win32_id, QtModifiersToWin32(old_combo.keyboardModifiers()), old_vk);
        }
        return {false, RebindError::ExternalConflict, action,
                QStringLiteral("This shortcut is already used by Windows or another application.")};
    }
    return {true, RebindError::None, action, {}};
#else
    Q_UNUSED(new_seq)
    Q_UNUSED(old_seq)
    return {true, RebindError::None, action, {}};
#endif
}

} // namespace exosnap

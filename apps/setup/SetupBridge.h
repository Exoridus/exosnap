#pragma once

#include <cstdint>

namespace exosnap::setup {

enum class SetupAction : std::uint32_t { Install, Repair, Uninstall };
enum class SetupScope : std::uint32_t { User, Machine, Conflict };

[[nodiscard]] constexpr SetupScope ResolveSetupScope(bool machine_installed, bool user_installed, bool all_users) {
    if (machine_installed && user_installed)
        return SetupScope::Conflict;
    return machine_installed || (!user_installed && all_users) ? SetupScope::Machine : SetupScope::User;
}

enum class SetupEventKind : std::uint32_t {
    Detected,
    Applying,
    Progress,
    PackageBegin,
    PackageComplete,
    FilesInUse,
    Completed,
    Error,
};

struct SetupEvent {
    SetupEventKind kind;
    std::int32_t status = 0;
    std::uint32_t progress = 0;
    bool installed = false;
    bool restart_required = false;
    bool can_cancel = false;
    const wchar_t* text = nullptr;
    const wchar_t* version = nullptr;
    bool runtime_missing = false;
    bool data_cleanup_failed = false;
};

using SetupEventCallback = void (*)(void* context, const SetupEvent* event);

struct SetupHost {
    void* context;
    const wchar_t* version;
    const wchar_t* log_path;
    const wchar_t* install_path;
    const wchar_t* machine_install_path;
    const wchar_t* runtime_path;
    bool passive;
    bool uninstall_requested;
    bool repair_requested;
    bool scope_locked;
    bool all_users;
    bool scope_conflict;
    bool allow_data_cleanup;
    void (*subscribe)(void* context, SetupEventCallback callback, void* callback_context);
    void (*detect)(void* context);
    void (*apply)(void* context, SetupAction action, bool desktop_shortcut, bool remove_user_data, bool all_users,
                  std::uintptr_t window_handle);
    void (*cancel)(void* context);
    void (*quit)(void* context, std::uint32_t exit_code);
    void (*launch)(void* context, std::uintptr_t window_handle);
    void (*restart)(void* context);
};

using RunSetupUi = std::uint32_t (*)(const SetupHost* host);

} // namespace exosnap::setup

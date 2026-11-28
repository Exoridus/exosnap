#include <update/runtime_swap_lock.h>

#include <string>

namespace {

int ReportLaunchFailure(DWORD error) {
    wchar_t detail[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, detail,
                   static_cast<DWORD>(sizeof(detail) / sizeof(detail[0])), nullptr);
    const std::wstring message = L"ExoSnap could not start runtime\\exosnap.exe.\n\n" + std::wstring(detail) +
                                 L"\nWindows error: " + std::to_wstring(error);
    MessageBoxW(nullptr, message.c_str(), L"ExoSnap", MB_OK | MB_ICONERROR);
    return 1;
}

bool MissingPath(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int show_mode) {
    std::wstring target(32768, L'\0');
    const DWORD image_length = GetModuleFileNameW(nullptr, target.data(), static_cast<DWORD>(target.size()));
    if (image_length == 0 || image_length >= target.size())
        return ReportLaunchFailure(GetLastError());

    target.resize(image_length);
    const auto separator = target.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
        return ReportLaunchFailure(ERROR_BAD_PATHNAME);
    const std::wstring package_root = target.substr(0, separator);
    const std::wstring runtime = package_root + L"\\runtime";
    target = runtime + L"\\exosnap.exe";

    STARTUPINFOW startup{};
    STARTUPINFOW inherited{};
    GetStartupInfoW(&inherited);
    const bool inherit_stdio = (inherited.dwFlags & STARTF_USESTDHANDLES) != 0;
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = static_cast<WORD>(show_mode);
    if ((inherited.dwFlags & (STARTF_TITLEISAPPID | STARTF_TITLEISLINKNAME)) != 0) {
        startup.dwFlags |= inherited.dwFlags & (STARTF_TITLEISAPPID | STARTF_TITLEISLINKNAME | STARTF_PREVENTPINNING);
        startup.lpTitle = inherited.lpTitle;
    }
    if (inherit_stdio) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = inherited.hStdInput;
        startup.hStdOutput = inherited.hStdOutput;
        startup.hStdError = inherited.hStdError;
    }
    const auto launch_runtime = [&]() -> DWORD {
        std::wstring command = L"\"" + target + L"\"";
        if (*arguments != L'\0') {
            command += L' ';
            command += arguments;
        }
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(target.c_str(), command.data(), nullptr, nullptr, inherit_stdio, 0, nullptr, nullptr,
                            &startup, &child))
            return GetLastError();
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        return ERROR_SUCCESS;
    };

    const DWORD error = launch_runtime();
    if (error == ERROR_SUCCESS)
        return 0;
    if (!MissingPath(error))
        return ReportLaunchFailure(error);

    const std::wstring backup = runtime + L".old";
    const DWORD backup_app = GetFileAttributesW((backup + L"\\exosnap.exe").c_str());
    if (backup_app == INVALID_FILE_ATTRIBUTES || (backup_app & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return ReportLaunchFailure(error);

    DWORD retry_error = ERROR_SUCCESS;
    {
        const exosnap::update::RuntimeSwapLock swap_lock(package_root);
        if (!swap_lock)
            retry_error = GetLastError();
        else if (GetFileAttributesW(runtime.c_str()) == INVALID_FILE_ATTRIBUTES && MissingPath(GetLastError()) &&
                 !MoveFileExW(backup.c_str(), runtime.c_str(), MOVEFILE_WRITE_THROUGH))
            retry_error = GetLastError();
        else
            retry_error = launch_runtime();
    }
    return retry_error == ERROR_SUCCESS ? 0 : ReportLaunchFailure(retry_error);
}

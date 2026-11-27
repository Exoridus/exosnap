#include "diagnostics/PrivilegedDiagnosticsPath.h"

#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <memory>

namespace exosnap::diagnostics::privileged {
namespace {

struct LocalDelete {
    void operator()(void* memory) const {
        ::LocalFree(memory);
    }
};

bool IsWindowsTrustedSid(PSID sid) {
    if (::IsWellKnownSid(sid, WinLocalSystemSid) || ::IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
        return true;
    PSID installer_sid = nullptr;
    if (!::ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &installer_sid))
        return false;
    const std::unique_ptr<void, LocalDelete> installer(installer_sid);
    return ::EqualSid(sid, installer.get()) != FALSE;
}

bool HasProtectedAcl(HANDLE file) {
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR raw = nullptr;
    const DWORD result = ::GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                           &owner, nullptr, &acl, nullptr, &raw);
    const std::unique_ptr<void, LocalDelete> descriptor(raw);
    if (result != ERROR_SUCCESS || !owner || !acl || !IsWindowsTrustedSid(owner))
        return false;
    constexpr DWORD mutable_rights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD |
                                     FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE |
                                     GENERIC_ALL;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!::GetAce(acl, i, &entry))
            return false;
        const auto* header = static_cast<ACE_HEADER*>(entry);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0 || header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            return false;
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if ((ace->Mask & mutable_rights) != 0 && !IsWindowsTrustedSid(&ace->SidStart))
            return false;
    }
    return true;
}

Handle OpenProtectedPath(const std::wstring& path, bool directory) {
    Handle file(::CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!file || !::GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory || !HasProtectedAcl(file.get()))
        return {};
    return file;
}

} // namespace

std::optional<ProtectedHelper> OpenProtectedHelper() {
    PWSTR raw = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, KF_FLAG_DEFAULT_PATH, nullptr, &raw);
    const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> folder(raw, &::CoTaskMemFree);
    if (FAILED(hr) || !folder)
        return std::nullopt;
    const std::wstring program_files(folder.get());
    // The supported location is a direct, fixed-volume child of the system drive
    // root. Do not resolve an alias through an unverified directory hierarchy.
    if (program_files.size() < 4 || program_files[1] != L':' || program_files[2] != L'\\' ||
        program_files.find_first_of(L"\\/:", 3) != std::wstring::npos ||
        ::GetDriveTypeW(program_files.substr(0, 3).c_str()) != DRIVE_FIXED)
        return std::nullopt;
    ProtectedHelper target;
    target.directory = program_files + L"\\" + kDirectoryName;
    target.executable = target.directory + L"\\" + kExecutableName;
    target.program_files_pin = OpenProtectedPath(program_files, true);
    target.directory_pin = OpenProtectedPath(target.directory, true);
    target.executable_pin = OpenProtectedPath(target.executable, false);
    if (!target.program_files_pin || !target.directory_pin || !target.executable_pin)
        return std::nullopt;
    return target;
}

} // namespace exosnap::diagnostics::privileged

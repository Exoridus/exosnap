#include "diagnostics/PrivilegedDiagnosticsPath.h"
#include "diagnostics/PrivilegedDiagnosticsIntegrity.h"

#include <aclapi.h>
#include <sddl.h>

#include <memory>
#include <string_view>
#include <update/release_key.h>
#include <vector>

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

bool HasProtectedAcl(HANDLE file, DWORD allowed_mutations) {
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
        if ((ace->Mask & (mutable_rights & ~allowed_mutations)) != 0 && !IsWindowsTrustedSid(&ace->SidStart))
            return false;
    }
    return true;
}

Handle OpenPinnedPath(const std::wstring& path, bool directory) {
    const DWORD access = READ_CONTROL | (directory ? FILE_READ_ATTRIBUTES : GENERIC_READ);
    Handle file(::CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!file || !::GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
        return {};
    return file;
}

Handle OpenProtectedPath(const std::wstring& path, bool directory, DWORD allowed_mutations = 0) {
    auto file = OpenPinnedPath(path, directory);
    if (!file || !HasProtectedAcl(file.get(), allowed_mutations))
        return {};
    return file;
}

std::wstring ProcessImagePath(HANDLE process, DWORD flags) {
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    if (!process || !::QueryFullProcessImageNameW(process, flags, image.data(), &size))
        return {};
    image.resize(size);
    return image;
}

std::wstring NativeFilePath(HANDLE file) {
    std::wstring path(32768, L'\0');
    const DWORD size = ::GetFinalPathNameByHandleW(file, path.data(), static_cast<DWORD>(path.size()), VOLUME_NAME_NT);
    if (size == 0 || size >= path.size())
        return {};
    path.resize(size);
    return path;
}

bool HasNoLoaderSidecars(const std::wstring& executable) {
    // Machine policy can honor .local redirection even for an embedded manifest.
    for (const auto suffix : {L".local", L".manifest", L".config"}) {
        if (::GetFileAttributesW((executable + suffix).c_str()) != INVALID_FILE_ATTRIBUTES ||
            ::GetLastError() != ERROR_FILE_NOT_FOUND)
            return false;
    }
    return true;
}

} // namespace

std::optional<ProtectedHelper> OpenSiblingHelper(HANDLE process, const wchar_t* expected_name) {
    const auto image = ProcessImagePath(process, 0);
    const auto leaf = image.find_last_of(L'\\');
    if (image.size() < 4 || image[1] != L':' || image[2] != L'\\' || leaf == image.npos || leaf < 3 ||
        _wcsicmp(image.c_str() + leaf + 1, expected_name) != 0)
        return std::nullopt;

    std::wstring path = image.substr(0, 3);
    if (::GetDriveTypeW(path.c_str()) != DRIVE_FIXED)
        return std::nullopt;
    ProtectedHelper location;
    // Creating another directory at the volume root cannot change any pinned component.
    auto root = OpenProtectedPath(path, true, FILE_ADD_SUBDIRECTORY);
    const std::wstring global_prefix = L"\\\\?\\GLOBALROOT\\GLOBAL??\\";
    auto global_root = OpenProtectedPath(global_prefix + path, true, FILE_ADD_SUBDIRECTORY);
    const auto native_root = root ? NativeFilePath(root.get()) : std::wstring{};
    if (!global_root || native_root.empty() || native_root != NativeFilePath(global_root.get()))
        return std::nullopt;
    location.directory_pins.push_back(std::move(root));
    for (size_t first = 3; first < leaf;) {
        const auto separator = image.find(L'\\', first);
        const auto component = std::wstring_view(image).substr(first, separator - first);
        if (component.empty() || component.back() == L'.' || component.back() == L' ' ||
            component.find_first_of(L"/:") != component.npos)
            return std::nullopt;
        path.append(component);
        auto directory = OpenProtectedPath(global_prefix + path, true);
        if (!directory)
            return std::nullopt;
        location.directory_pins.push_back(std::move(directory));
        first = separator + 1;
        if (separator < leaf)
            path.push_back(L'\\');
    }
    location.directory = path;
    location.executable = path + L"\\" + kExecutableName;
    location.executable_pin = OpenProtectedPath(global_prefix + location.executable, false);
    location.recorder_pin = OpenProtectedPath(global_prefix + path + L"\\" + kRecorderName, false);
    if (!location.executable_pin || !location.recorder_pin || !HasNoLoaderSidecars(global_prefix + location.executable))
        return std::nullopt;
    const auto expected =
        _wcsicmp(expected_name, kExecutableName) == 0 ? location.executable_pin.get() : location.recorder_pin.get();
    if (!ProcessImageMatchesFile(process, expected))
        return std::nullopt;
    return location;
}

bool ProcessImageMatchesFile(HANDLE process, HANDLE file) {
    // NTFS can distinguish images whose native paths differ only in case.
    const auto image = ProcessImagePath(process, PROCESS_NAME_NATIVE);
    return !image.empty() && image == NativeFilePath(file);
}

bool IsPairedRecorder(const ProtectedHelper& location, HANDLE process) {
    return ProcessImageMatchesFile(process, location.recorder_pin.get()) &&
           VerifyDiagnosticsImageFiles(location.directory, location.recorder_pin.get(), location.executable_pin.get(),
                                       update::kUpdatePublicKey);
}

} // namespace exosnap::diagnostics::privileged

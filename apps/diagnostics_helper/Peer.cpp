#include "Peer.h"
#include "FixtureStatus.h"
#include "ImagePath.h"
#include "NativeRead.h"

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <array>
#include <string_view>

namespace exosnap::diagnostics::ipc {
bool ApprovedPublisher(const std::wstring& image, const Handle& locked_file) {
    constexpr std::string_view expected = EXOSNAP_DIAGNOSTICS_PUBLISHER_SHA256;
    if (expected.size() != 64 || !locked_file)
        return false;
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = image.c_str();
    file.hFile = locked_file.get();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool accepted = false;
    const auto trust = WinVerifyTrust(nullptr, &policy, &data);
    ReportFixtureStatus("WinVerifyTrust", static_cast<DWORD>(trust));
    if (trust == ERROR_SUCCESS) {
        auto* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        auto* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        auto* leaf = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        std::array<BYTE, 32> hash{};
        DWORD length = static_cast<DWORD>(hash.size());
        if (leaf && leaf->pCert &&
            CertGetCertificateContextProperty(leaf->pCert, CERT_SHA256_HASH_PROP_ID, hash.data(), &length) &&
            length == hash.size()) {
            constexpr std::string_view digits = "0123456789abcdef";
            accepted = true;
            for (size_t i = 0; i < hash.size(); ++i) {
                if (digits[hash[i] >> 4] != expected[2 * i] || digits[hash[i] & 15u] != expected[2 * i + 1])
                    accepted = false;
            }
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &policy, &data);
    return accepted;
}

bool ApprovedClient(HANDLE process) {
    DWORD owner_session = 0;
    DWORD ours = 0;
    if (!ProcessIdToSessionId(GetProcessId(process), &owner_session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &ours) || owner_session != ours ||
        WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
        return false;
    const auto image = NativeProcessImagePath(process);
    const auto root = LocalVolumeRootEnd(image);
    const auto slash = image.find_last_of(L'\\');
    if (root == image.npos || slash == image.npos || _wcsicmp(image.c_str() + slash + 1, L"exosnap.exe") != 0)
        return false;
    // The recorder can live in a per-user directory. Hold each physical
    // ancestor against rename and reject reparse points before taking the next.
    std::array<Handle, 128> ancestors;
    size_t held = 0;
    for (size_t first = root + 1;;) {
        const auto next = image.find(L'\\', first);
        const bool leaf = next == image.npos;
        const auto component = std::wstring_view(image).substr(first, next == image.npos ? next : next - first);
        if (component.empty() || component == L"." || component == L".." || component.find(L':') != component.npos ||
            (!leaf && held == ancestors.size()))
            return false;
        auto locked = OpenNativeRead(std::wstring_view(image).substr(0, next), leaf);
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!locked || !GetFileInformationByHandleEx(locked.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == leaf)
            return false;
        if (leaf) {
            std::wstring physical(32768, L'\0');
            const auto length = GetFinalPathNameByHandleW(locked.get(), physical.data(), static_cast<DWORD>(physical.size()),
                                                          FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
            if (!length || length >= physical.size())
                return false;
            physical.resize(length);
            return _wcsicmp(physical.c_str(), image.c_str()) == 0 && NativeProcessImagePath(process) == image &&
                   ApprovedPublisher(GlobalLocalVolumePath(physical), locked) &&
                   WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        }
        ancestors[held++] = std::move(locked);
        first = next + 1;
    }
}
} // namespace exosnap::diagnostics::ipc

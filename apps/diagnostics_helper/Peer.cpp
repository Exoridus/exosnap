#include "Peer.h"
#include "ImagePath.h"

#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <array>
#include <string_view>

namespace exosnap::diagnostics::ipc {
bool ApprovedPublisher(const std::wstring& image, HANDLE locked_file) {
    constexpr std::string_view expected = EXOSNAP_DIAGNOSTICS_PUBLISHER_SHA256;
    if (expected.size() != 64 || !locked_file || locked_file == INVALID_HANDLE_VALUE)
        return false;
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = image.c_str();
    file.hFile = locked_file;
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
    if (WinVerifyTrust(nullptr, &policy, &data) == ERROR_SUCCESS) {
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
    const auto image = GlobalProcessImagePath(process);
    const auto slash = image.find_last_of(L'\\');
    if (slash == image.npos || _wcsicmp(image.c_str() + slash + 1, L"exosnap.exe") != 0)
        return false;
    Handle locked(CreateFileW(image.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    return locked && ApprovedPublisher(image, locked.get());
}
} // namespace exosnap::diagnostics::ipc

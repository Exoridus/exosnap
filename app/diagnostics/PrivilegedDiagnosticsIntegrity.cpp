#include "diagnostics/PrivilegedDiagnosticsIntegrity.h"
#include "diagnostics/PrivilegedDiagnosticsPipe.h"

#include <update/ed25519_verify.h>
#include <update/file_sha256.h>

#include <algorithm>
#include <array>

namespace exosnap::diagnostics::privileged {
namespace {

constexpr std::string_view kPairHeader = "EXOSNAP-DIAGNOSTICS-PAIR-V1\n";
constexpr size_t kHexDigestSize = 64;
constexpr size_t kDigestLineSize = kHexDigestSize + 1;
constexpr size_t kManifestSize = kPairHeader.size() + 2 * kDigestLineSize;

template <size_t Size> std::optional<std::array<uint8_t, Size>> ReadSignedPairFile(const std::wstring& path) {
    const Handle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    LARGE_INTEGER size{};
    if (!file || !::GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        !::GetFileSizeEx(file.get(), &size) || size.QuadPart != static_cast<LONGLONG>(Size))
        return std::nullopt;

    std::array<uint8_t, Size> bytes{};
    DWORD read = 0;
    if (!::ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
        return std::nullopt;
    return bytes;
}

bool IsLowerHex(std::string_view digest) {
    return std::ranges::all_of(
        digest, [](char digit) { return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'); });
}

} // namespace

std::optional<DiagnosticsImageDigests> VerifyDiagnosticsImageDigests(std::string_view manifest,
                                                                     std::span<const uint8_t> signature,
                                                                     std::span<const uint8_t, 32> official_key) {
    if (manifest.size() != kManifestSize || !manifest.starts_with(kPairHeader) ||
        manifest[kPairHeader.size() + kHexDigestSize] != '\n' || manifest.back() != '\n' || signature.size() != 64)
        return std::nullopt;
    if (!update::ed25519_verify(signature.data(), reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size(),
                                official_key.data()))
        return std::nullopt;

    const auto recorder = manifest.substr(kPairHeader.size(), kHexDigestSize);
    const auto helper = manifest.substr(kPairHeader.size() + kDigestLineSize, kHexDigestSize);
    if (!IsLowerHex(recorder) || !IsLowerHex(helper))
        return std::nullopt;
    return DiagnosticsImageDigests{std::string(recorder), std::string(helper)};
}

bool VerifyDiagnosticsImageFiles(std::wstring_view recorder_directory, void* recorder_file, void* helper_file,
                                 std::span<const uint8_t, 32> official_key) {
    const std::wstring directory(recorder_directory);
    const auto manifest = ReadSignedPairFile<kManifestSize>(directory + L"\\diagnostics-pair.txt");
    const auto signature = ReadSignedPairFile<64>(directory + L"\\diagnostics-pair.txt.sig");
    if (!manifest || !signature)
        return false;

    const std::string_view bytes(reinterpret_cast<const char*>(manifest->data()), manifest->size());
    const auto digests = VerifyDiagnosticsImageDigests(bytes, *signature, official_key);
    return digests && digests->recorder == update::Sha256HexOfHandle(recorder_file) &&
           digests->helper == update::Sha256HexOfHandle(helper_file);
}

} // namespace exosnap::diagnostics::privileged

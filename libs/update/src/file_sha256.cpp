#include <update/file_sha256.h>

// clang-format off
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <string>

namespace exosnap::update {
namespace {

template <class ReadBlock> std::string Sha256HexOfSource(ReadBlock&& read_block) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return {};

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    char buffer[65536];
    bool valid = true;
    for (;;) {
        size_t produced = 0;
        valid = read_block(buffer, sizeof(buffer), produced);
        if (!valid || produced == 0)
            break;
        valid = BCRYPT_SUCCESS(BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(produced), 0));
        if (!valid)
            break;
    }

    uint8_t digest[32]{};
    valid = valid && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!valid)
        return {};

    char hex[65]{};
    for (int index = 0; index < 32; ++index)
        snprintf(hex + index * 2, 3, "%02x", digest[index]);
    return std::string(hex);
}

} // namespace

std::string Sha256HexOfFile(const std::string& path) noexcept {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    return Sha256HexOfSource([&file](char* buffer, size_t capacity, size_t& produced) noexcept {
        file.read(buffer, static_cast<std::streamsize>(capacity));
        produced = static_cast<size_t>(file.gcount());
        return !file.bad();
    });
}

std::string Sha256HexOfHandle(void* file_handle) noexcept {
    const auto handle = static_cast<HANDLE>(file_handle);
    LARGE_INTEGER origin{};
    if (::SetFilePointerEx(handle, origin, nullptr, FILE_BEGIN) == 0)
        return {};
    return Sha256HexOfSource([handle](char* buffer, size_t capacity, size_t& produced) noexcept {
        DWORD read = 0;
        if (::ReadFile(handle, buffer, static_cast<DWORD>(capacity), &read, nullptr) == 0)
            return false;
        produced = static_cast<size_t>(read);
        return true;
    });
}

} // namespace exosnap::update

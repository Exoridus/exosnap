#include "MsixAcquisition.h"
#include "FixtureStatus.h"

#include <appxpackaging.h>
#include <sdkddkver.h>

#if defined(__IAppxDigestProvider_INTERFACE_DEFINED__) && defined(NTDDI_WIN11_GE)
#include <shlwapi.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>

#include <array>
#include <exception>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace exosnap::diagnostics::ipc {
namespace {
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Management::Deployment;

struct TaskString {
    wchar_t* value = nullptr;
    ~TaskString() {
        CoTaskMemFree(value);
    }
};

com_ptr<IStream> PackageStream(HANDLE source) {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(source, &length) || length.QuadPart <= 0 || length.QuadPart > 64 * 1024 * 1024 ||
        !SetFilePointerEx(source, {}, nullptr, FILE_BEGIN))
        throw hresult_error(E_INVALIDARG);
    com_ptr<IStream> stream;
    check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    std::array<unsigned char, 16384> block{};
    while (length.QuadPart > 0) {
        DWORD count = 0;
        check_bool(ReadFile(source, block.data(), static_cast<DWORD>(block.size()), &count, nullptr));
        ULONG written = 0;
        check_hresult(stream->Write(block.data(), count, &written));
        if (!count || count > length.QuadPart || written != count)
            throw hresult_error(E_FAIL);
        length.QuadPart -= count;
    }
    check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
    return stream;
}

std::vector<unsigned char> ReadFootprint(IAppxPackageReader* reader, APPX_FOOTPRINT_FILE_TYPE type) {
    com_ptr<IAppxFile> file;
    check_hresult(reader->GetFootprintFile(type, file.put()));
    UINT64 size = 0;
    check_hresult(file->GetSize(&size));
    if (size == 0 || size > 1024 * 1024)
        throw hresult_error(E_INVALIDARG);
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    com_ptr<IStream> stream;
    check_hresult(file->GetStream(stream.put()));
    ULONG received = 0;
    check_hresult(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &received));
    if (received != bytes.size())
        throw hresult_error(E_FAIL);
    return bytes;
}

template <typename Operation>
bool WaitForCompletion(const Operation& operation, HANDLE stop) {
    bool cancelled = false;
    while (operation.Status() == AsyncStatus::Started) {
        if (cancelled) {
            Sleep(100);
        } else if (WaitForSingleObject(stop, 100) != WAIT_TIMEOUT) {
            cancelled = true;
            try {
                operation.Cancel();
            } catch (const hresult_error& error) {
                ReportFixtureStatus("MSIX cancellation", static_cast<DWORD>(error.code().value));
            }
        }
    }
    return !cancelled && WaitForSingleObject(stop, 0) == WAIT_TIMEOUT;
}

bool Acquire(HANDLE source, const std::filesystem::path& source_path, HANDLE stop, const VerifyRegisteredImage& verify) {
    if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT)
        return false;
    if (!Metadata::ApiInformation::IsPropertyPresent(L"Windows.Management.Deployment.AddPackageOptions",
                                                     L"ExpectedDigests")) {
        ReportFixtureStatus("MSIX expected digest API", ERROR_NOT_SUPPORTED);
        return {};
    }
    const auto stream = PackageStream(source);
    if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT)
        return false;
    com_ptr<IAppxFactory> factory;
    check_hresult(CoCreateInstance(__uuidof(AppxFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())));
    com_ptr<IAppxPackageReader> reader;
    check_hresult(factory->CreatePackageReader(stream.get(), reader.put()));
    TaskString digest;
    check_hresult(reader.as<IAppxDigestProvider>()->GetDigest(&digest.value));
    com_ptr<IAppxManifestReader> manifest;
    check_hresult(reader->GetManifest(manifest.put()));
    com_ptr<IAppxManifestPackageId> identity;
    check_hresult(manifest->GetPackageId(identity.put()));
    TaskString name;
    TaskString publisher;
    TaskString full_name;
    APPX_PACKAGE_ARCHITECTURE architecture{};
    check_hresult(identity->GetName(&name.value));
    check_hresult(identity->GetPublisher(&publisher.value));
    check_hresult(identity->GetArchitecture(&architecture));
    check_hresult(identity->GetPackageFullName(&full_name.value));
    if (!digest.value || !*digest.value || !publisher.value || !name.value ||
        std::wstring_view(name.value) != L"Exoridus.ExoSnap.Diagnostics" ||
        architecture != APPX_PACKAGE_ARCHITECTURE_X64 || !full_name.value || !*full_name.value)
        return {};
    PackageManager manager;
    const auto expected_manifest = ReadFootprint(reader.get(), APPX_FOOTPRINT_FILE_TYPE_MANIFEST);
    const auto expected_blocks = ReadFootprint(reader.get(), APPX_FOOTPRINT_FILE_TYPE_BLOCKMAP);
    const auto resolve = [&]() -> bool {
        if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT)
            return false;
        const auto package = manager.FindPackageForUser(L"", full_name.value);
        if (!package || package.IsDevelopmentMode() || package.IsFramework() || package.IsResourcePackage() ||
            package.Id().FullName() != full_name.value)
            return {};
        const std::filesystem::path location(package.InstalledLocation().Path().c_str());
        if (_wcsicmp(location.filename().c_str(), full_name.value) ||
            _wcsicmp(location.parent_path().filename().c_str(), L"WindowsApps"))
            return {};
        if (!verify(location / L"exosnap-diagnostics.exe", expected_manifest, expected_blocks) ||
            WaitForSingleObject(stop, 0) != WAIT_TIMEOUT)
            return false;
        const auto integrity = package.VerifyContentIntegrityAsync();
        return WaitForCompletion(integrity, stop) && integrity.GetResults();
    };
    // Enumerate registrations rather than looking up an absent name (some builds throw on absence).
    for (const auto& package : manager.FindPackagesForUser(L"", name.value, publisher.value)) {
        if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT)
            return false;
        if (package.Id().FullName() == full_name.value)
            return resolve();
    }
    std::wstring url(32768, L'\0');
    DWORD count = static_cast<DWORD>(url.size());
    check_hresult(UrlCreateFromPathW(source_path.c_str(), url.data(), &count, 0));
    url.resize(count);
    const Uri uri(url);
    AddPackageOptions options;
    options.ExpectedDigests().Insert(uri, digest.value);
    if (WaitForSingleObject(stop, 0) != WAIT_TIMEOUT)
        return {};
    auto operation = manager.AddPackageByUriAsync(uri, options);
    if (!WaitForCompletion(operation, stop))
        return false;
    const auto result = operation.GetResults();
    check_hresult(result.ExtendedErrorCode());
    return resolve();
}
} // namespace

bool AcquireRegisteredMsix(HANDLE source, const std::filesystem::path& source_path, HANDLE stop,
                           const VerifyRegisteredImage& verify) {
    bool ready = false;
    // ShellExecute stays on the existing collector STA. This worker retains its
    // async operations and the caller's verified handles until completion or cancellation.
    try {
        std::thread deployment([&] {
            const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
            if (FAILED(initialized))
                return;
            try {
                ready = Acquire(source, source_path, stop, verify);
            } catch (const winrt::hresult_error& error) {
                ReportFixtureStatus("MSIX acquisition", static_cast<DWORD>(error.code().value));
            } catch (const std::exception&) {
                ReportFixtureStatus("MSIX acquisition", ERROR_GEN_FAILURE);
            }
            RoUninitialize();
        });
        deployment.join();
    } catch (const std::system_error&) {
        ReportFixtureStatus("MSIX worker creation", ERROR_NOT_ENOUGH_MEMORY);
    }
    return ready && WaitForSingleObject(stop, 0) == WAIT_TIMEOUT;
}

} // namespace exosnap::diagnostics::ipc
#else
namespace exosnap::diagnostics::ipc {
bool AcquireRegisteredMsix(HANDLE, const std::filesystem::path&, HANDLE, const VerifyRegisteredImage&) {
    return {};
}
} // namespace exosnap::diagnostics::ipc
#endif

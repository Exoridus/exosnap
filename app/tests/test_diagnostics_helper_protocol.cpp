#include "../../apps/diagnostics_helper/Protocol.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include "../../apps/diagnostics_helper/FileAcePolicy.h"
#include "../../apps/diagnostics_helper/RegistryAcePolicy.h"
#include "../../apps/diagnostics_helper/StartingGuardian.h"
#include <string>
#endif

namespace {
using namespace exosnap::diagnostics::ipc;

TEST(DiagnosticsHelperProtocol, BeginHasNoParameters) {
    Record r{};
    r.kind = Kind::Begin;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    r.value = 1;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.value = 0;
    r.related.back() = 1;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

TEST(DiagnosticsHelperProtocol, RejectsUnknownVersionsKindsAndLengths) {
    Record r{};
    r.kind = Kind::Lost;
    EXPECT_FALSE(Valid(r, sizeof(r) - 1));
    EXPECT_FALSE(Valid(r, sizeof(r) + 1));
    r.version++;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.version = kVersion;
    r.kind = static_cast<Kind>(99);
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

TEST(DiagnosticsHelperProtocol, BoundsPresentFieldsBeforeUse) {
    Record r{};
    r.kind = Kind::Present;
    r.value = 1;
    r.process = 12;
    r.related_count = 16;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    ++r.related_count;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.related_count = 0;
    r.flags = 8;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}
TEST(DiagnosticsHelperProtocol, KernelMetricsRejectPathsStaleSamplesAndOverlongNames) {
    Record r{};
    r.kind = Kind::Dpc;
    r.value = 10;
    r.window = 5;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    r.driver[0] = '/';
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.driver.fill('a');
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.driver.fill(0);
    r.process = 15001;
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.process = 0;
    r.window = 11;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

TEST(DiagnosticsHelperProtocol, DpcOnlyBeginCannotSpecifyProviderOrParameters) {
    Record r{};
    r.kind = Kind::BeginDpcOnly;
    EXPECT_TRUE(Valid(r, sizeof(r)));
    r.driver[0] = 'x';
    EXPECT_FALSE(Valid(r, sizeof(r)));
    r.driver[0] = 0;
    r.window = 3;
    EXPECT_FALSE(Valid(r, sizeof(r)));
}

#ifdef _WIN32
TEST(DiagnosticsHelperWindowsAcl, AcceptsOnlyReadOnlyConditionalAllows) {
    struct Entry {
        ACCESS_ALLOWED_CALLBACK_ACE ace{};
        std::array<DWORD, 4> space{};
    } entry;
    auto& header = entry.ace.Header;
    header.AceType = ACCESS_ALLOWED_CALLBACK_ACE_TYPE;
    header.AceSize = sizeof(entry);
    auto& sid = *reinterpret_cast<SID*>(&entry.ace.SidStart);
    sid.Revision = SID_REVISION;
    sid.SubAuthorityCount = 1;
    entry.ace.Mask = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
    EXPECT_EQ(InspectFileAce(header, 0), FileAceGrant::ReadOnly);
    for (const DWORD right : {static_cast<DWORD>(FILE_WRITE_DATA), static_cast<DWORD>(DELETE),
                              static_cast<DWORD>(WRITE_DAC), static_cast<DWORD>(FILE_DELETE_CHILD)}) {
        entry.ace.Mask = right;
        EXPECT_EQ(InspectFileAce(header, 0), FileAceGrant::Invalid);
    }
    entry.ace.Mask = FILE_GENERIC_READ;
    header.AceType = ACCESS_ALLOWED_OBJECT_ACE_TYPE;
    EXPECT_EQ(InspectFileAce(header, 0), FileAceGrant::Invalid);
    header.AceType = ACCESS_ALLOWED_CALLBACK_ACE_TYPE;
    header.AceSize = sizeof(ACE_HEADER);
    EXPECT_EQ(InspectFileAce(header, 0), FileAceGrant::Invalid);
    header.AceSize = sizeof(entry);
    sid.SubAuthorityCount = SID_MAX_SUB_AUTHORITIES;
    EXPECT_EQ(InspectFileAce(header, 0), FileAceGrant::Invalid);
}

TEST(DiagnosticsHelperWindowsAcl, RecognizesOnlyTheRegistryCreatorInheritanceTemplate) {
    struct Entry {
        ACCESS_ALLOWED_ACE ace{};
        std::array<DWORD, 4> space{};
    } entry;
    auto& header = entry.ace.Header;
    header.AceType = ACCESS_ALLOWED_ACE_TYPE;
    header.AceFlags = CONTAINER_INHERIT_ACE;
    header.AceSize = sizeof(entry);
    entry.ace.Mask = KEY_ALL_ACCESS;
    auto& sid = *reinterpret_cast<SID*>(&entry.ace.SidStart);
    sid.Revision = SID_REVISION;
    sid.IdentifierAuthority = SECURITY_CREATOR_SID_AUTHORITY;
    sid.SubAuthorityCount = 1;
    sid.SubAuthority[0] = SECURITY_CREATOR_OWNER_RID;
    EXPECT_TRUE(IsRegistryCreatorTemplate(header));
    for (const BYTE flags : {BYTE{0}, BYTE{OBJECT_INHERIT_ACE}, BYTE{INHERITED_ACE}}) {
        header.AceFlags = flags;
        EXPECT_FALSE(IsRegistryCreatorTemplate(header));
    }
    header.AceFlags = CONTAINER_INHERIT_ACE;
    entry.ace.Mask |= MAXIMUM_ALLOWED;
    EXPECT_FALSE(IsRegistryCreatorTemplate(header));
    entry.ace.Mask = KEY_ALL_ACCESS;
    header.AceType = ACCESS_ALLOWED_CALLBACK_ACE_TYPE;
    EXPECT_FALSE(IsRegistryCreatorTemplate(header));
    header.AceType = ACCESS_ALLOWED_ACE_TYPE;
    header.AceSize = sizeof(ACE_HEADER);
    EXPECT_FALSE(IsRegistryCreatorTemplate(header));
    header.AceSize = sizeof(entry);
    sid.SubAuthority[0] = SECURITY_CREATOR_OWNER_RIGHTS_RID;
    EXPECT_FALSE(IsRegistryCreatorTemplate(header));
    sid.IdentifierAuthority = SECURITY_WORLD_SID_AUTHORITY;
    sid.SubAuthority[0] = SECURITY_WORLD_RID;
    EXPECT_FALSE(IsRegistryCreatorTemplate(header));
}

TEST(DiagnosticsHelperGuardian, FailedHandoffReapsChildButSuccessfulHandoffTransfersIt) {
    std::wstring image(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
    ASSERT_GT(size, 0u);
    ASSERT_LT(size, image.size());
    image.resize(size);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    // Nothing runs in the child. This exercises failure before its entry point,
    // without elevation, UAC, desktop interaction or opening an ETW session.
    ASSERT_TRUE(CreateProcessW(image.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &child));
    Handle thread(child.hThread);
    StartingGuardian cleanup{Handle(child.hProcess)};
    HANDLE duplicated = nullptr;
    ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), cleanup.get(), GetCurrentProcess(), &duplicated, 0, FALSE,
                                DUPLICATE_SAME_ACCESS));
    Handle transferred;
    {
        StartingGuardian starting{Handle(duplicated)};
        transferred = starting.handoff();
    }
    EXPECT_EQ(WaitForSingleObject(cleanup.get(), 0), WAIT_TIMEOUT);
    {
        StartingGuardian failed(std::move(transferred));
    }
    EXPECT_EQ(WaitForSingleObject(cleanup.get(), 0), WAIT_OBJECT_0);
}
#endif

} // namespace

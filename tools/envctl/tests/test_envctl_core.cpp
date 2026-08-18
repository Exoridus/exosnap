// Vocabulary, capability classification, alias binding and journal durability.
// Everything here is pure: no display, no audio endpoint, no WinAPI.

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "env_alias.h"
#include "env_catalogue.h"
#include "env_display_mode.h"
#include "env_journal.h"
#include "env_types.h"
#include "temp_dir.h"

namespace exosnap::envctl {
namespace {

using exosnap::envctl::fakes::TempDir;

// ------------------------------------------------------------------ vocabulary

TEST(EnvctlTypes, CapabilityKeysRoundTrip) {
    for (const auto value :
         {CapabilityClass::Read, CapabilityClass::MutateSafe, CapabilityClass::MutateTestOnly, CapabilityClass::Human,
          CapabilityClass::Physical, CapabilityClass::Secure, CapabilityClass::Unavailable}) {
        const auto key = ToKey(value);
        const auto parsed = CapabilityClassFromKey(key);
        ASSERT_TRUE(parsed.has_value()) << key;
        EXPECT_EQ(*parsed, value);
    }
    EXPECT_EQ(ToKey(CapabilityClass::MutateSafe), "ENV_MUTATE_SAFE");
    EXPECT_EQ(ToKey(CapabilityClass::Human), "ENV_HUMAN");
    EXPECT_EQ(ToKey(CapabilityClass::Physical), "PHYSICAL");
}

TEST(EnvctlTypes, OnlyMutateClassesAreMutable) {
    EXPECT_TRUE(IsMutable(CapabilityClass::MutateSafe));
    EXPECT_TRUE(IsMutable(CapabilityClass::MutateTestOnly));
    EXPECT_FALSE(IsMutable(CapabilityClass::Read));
    EXPECT_FALSE(IsMutable(CapabilityClass::Human));
    EXPECT_FALSE(IsMutable(CapabilityClass::Physical));
    EXPECT_FALSE(IsMutable(CapabilityClass::Secure));
    EXPECT_FALSE(IsMutable(CapabilityClass::Unavailable));
}

TEST(EnvctlTypes, TransactionStateKeysRoundTrip) {
    for (const auto value :
         {TransactionState::Clean, TransactionState::Prepared, TransactionState::Mutating, TransactionState::Active,
          TransactionState::Restoring, TransactionState::Restored, TransactionState::RestorePending,
          TransactionState::RestorePendingDeviceUnavailable, TransactionState::RestoreFailed}) {
        const auto parsed = TransactionStateFromKey(ToKey(value));
        ASSERT_TRUE(parsed.has_value()) << ToKey(value);
        EXPECT_EQ(*parsed, value);
    }
    EXPECT_FALSE(IsDirty(TransactionState::Clean));
    EXPECT_FALSE(IsDirty(TransactionState::Restored));
    EXPECT_TRUE(IsDirty(TransactionState::Prepared));
    EXPECT_TRUE(IsDirty(TransactionState::Active));
    EXPECT_TRUE(IsDirty(TransactionState::RestoreFailed));
}

TEST(EnvctlTypes, PropertyKeyParsing) {
    const auto id = PropertyIdFromKey("display.main-hdr:hdr");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id->device_alias, "display.main-hdr");
    EXPECT_EQ(id->property, "hdr");
    EXPECT_EQ(id->Key(), "display.main-hdr:hdr");

    EXPECT_FALSE(PropertyIdFromKey("nocolon").has_value());
    EXPECT_FALSE(PropertyIdFromKey(":hdr").has_value());
    EXPECT_FALSE(PropertyIdFromKey("alias:").has_value());
    EXPECT_FALSE(PropertyIdFromKey("a:b:c").has_value());
}

// ------------------------------------------------------------------- catalogue

TEST(EnvctlCatalogue, EveryEntryDeclaresBothMechanisms) {
    ASSERT_FALSE(WindowsCapabilityCatalogue().empty());
    for (const auto& entry : WindowsCapabilityCatalogue()) {
        EXPECT_FALSE(entry.device_kind.empty());
        EXPECT_FALSE(entry.property.empty());
        EXPECT_FALSE(entry.read_mechanism.empty()) << entry.property;
        // A mutable property must name the setter it uses; a non-mutable one
        // must say why there is none. Either way this field is never blank.
        EXPECT_FALSE(entry.mutate_mechanism.empty()) << entry.property;
    }
}

TEST(EnvctlCatalogue, OnlyDocumentedSettersAreClassifiedMutable) {
    std::vector<std::string> mutable_properties;
    for (const auto& entry : WindowsCapabilityCatalogue()) {
        if (IsMutable(entry.capability)) {
            mutable_properties.push_back(entry.device_kind + ":" + entry.property);
        }
    }
    // The whole safety argument of this tool is that this list is short and
    // every member has a documented, reversible setter.
    EXPECT_EQ(mutable_properties, (std::vector<std::string>{"display:hdr", "display:refresh-hz"}));
}

TEST(EnvctlCatalogue, UndocumentedMechanismsAreHumanNotAutomated) {
    const auto find = [](const std::string& kind, const std::string& property) {
        for (const auto& entry : WindowsCapabilityCatalogue()) {
            if (entry.device_kind == kind && entry.property == property) {
                return entry;
            }
        }
        ADD_FAILURE() << "missing catalogue entry " << kind << ":" << property;
        return CatalogueEntry{};
    };

    EXPECT_EQ(find("display", "acm").capability, CapabilityClass::Human);
    EXPECT_EQ(find("display", "dpi-scale").capability, CapabilityClass::Human);
    EXPECT_EQ(find("audio-render", "default-roles").capability, CapabilityClass::Human);
    EXPECT_EQ(find("system", "apps-theme").capability, CapabilityClass::Human);
    EXPECT_EQ(find("audio-render", "endpoint-state").capability, CapabilityClass::Physical);
}

TEST(EnvctlCatalogue, DeviceFormatAndMixFormatAreDistinctProperties) {
    const auto entries = CatalogueForKind(device_kind::kAudioRender);
    const CatalogueEntry* device_format = nullptr;
    const CatalogueEntry* mix_format = nullptr;
    for (const auto& entry : entries) {
        if (entry.property == "device-format") {
            device_format = &entry;
        }
        if (entry.property == "mix-format") {
            mix_format = &entry;
        }
    }
    ASSERT_NE(device_format, nullptr);
    ASSERT_NE(mix_format, nullptr);
    EXPECT_NE(device_format->read_mechanism, mix_format->read_mechanism);
    EXPECT_NE(std::string::npos, device_format->read_mechanism.find("PKEY_AudioEngine_DeviceFormat"));
    EXPECT_NE(std::string::npos, mix_format->read_mechanism.find("GetMixFormat"));
}

TEST(EnvctlCatalogue, DescriptorsForAliasInstantiateTheKind) {
    const auto descriptors = DescriptorsForAlias("display.main-hdr", device_kind::kDisplay);
    ASSERT_FALSE(descriptors.empty());
    for (const auto& descriptor : descriptors) {
        EXPECT_EQ(descriptor.id.device_alias, "display.main-hdr");
    }
}

TEST(EnvctlCatalogue, RefreshRateNoteNamesTheNominalVersusReportedAsymmetry) {
    const CatalogueEntry* refresh = nullptr;
    for (const auto& entry : WindowsCapabilityCatalogue()) {
        if (entry.device_kind == device_kind::kDisplay && entry.property == "refresh-hz") {
            refresh = &entry;
        }
    }
    ASSERT_NE(refresh, nullptr);
    // `describe` is where a caller looks before writing a desired-state file, so
    // the trap has to be stated there: a nominal 60 is accepted and reads back
    // as 59, and the way out is `list-modes`, not a tolerance.
    EXPECT_NE(std::string::npos, refresh->note.find("EnumDisplaySettingsEx")) << refresh->note;
    EXPECT_NE(std::string::npos, refresh->note.find("59")) << refresh->note;
    EXPECT_NE(std::string::npos, refresh->note.find("list-modes")) << refresh->note;
    EXPECT_NE(std::string::npos, refresh->note.find("no tolerance")) << refresh->note;
}

// ---------------------------------------------------------------- display modes

DisplayModeFacts Mode(unsigned long width, unsigned long height, unsigned long hz) {
    return DisplayModeFacts{width, height, hz, 32, 0};
}

std::vector<unsigned long> RefreshRatesOf(const std::vector<DisplayModeFacts>& modes) {
    std::vector<unsigned long> rates;
    for (const auto& mode : modes) {
        rates.push_back(mode.refresh_hz);
    }
    return rates;
}

TEST(EnvctlDisplayMode, FingerprintRendersEveryCoupledField) {
    EXPECT_EQ(FormatModeFacts(DisplayModeFacts{2560, 1440, 144, 32, 0}), "2560x1440@144x32/0");
    EXPECT_EQ(FormatModeFacts(DisplayModeFacts{1080, 1920, 60, 32, 270}), "1080x1920@60x32/270");
}

TEST(EnvctlDisplayMode, GeometryComparisonIgnoresOnlyTheRefreshRate) {
    const DisplayModeFacts current{2560, 1440, 144, 32, 0};
    EXPECT_TRUE(SameGeometry(current, DisplayModeFacts{2560, 1440, 59, 32, 0}));
    EXPECT_FALSE(SameGeometry(current, DisplayModeFacts{1920, 1080, 144, 32, 0}));
    EXPECT_FALSE(SameGeometry(current, DisplayModeFacts{2560, 1440, 144, 16, 0}));
    EXPECT_FALSE(SameGeometry(current, DisplayModeFacts{2560, 1440, 144, 32, 90}));
}

TEST(EnvctlDisplayMode, RefreshRatesAreWindowsIntegersAndAreNeverRoundedToNominal) {
    // The whole point of the list: a panel that enumerates 59 is offered as 59.
    // Rounding it up to the nominal 60 would hand the caller a value the
    // read-back can never report, which is exactly the failure this feature
    // exists to prevent.
    const DisplayModeFacts current = Mode(2560, 1440, 144);
    const auto candidates =
        RefreshRateCandidates(current, {Mode(2560, 1440, 23), Mode(2560, 1440, 29), Mode(2560, 1440, 59),
                                        Mode(2560, 1440, 119), Mode(2560, 1440, 144)});
    EXPECT_EQ(RefreshRatesOf(candidates), (std::vector<unsigned long>{23, 29, 59, 119, 144}));
}

TEST(EnvctlDisplayMode, ARefreshRateChangeIsNeverOfferedAResolutionChange) {
    const DisplayModeFacts current{2560, 1440, 144, 32, 0};
    const auto candidates = RefreshRateCandidates(current, {
                                                               Mode(1920, 1080, 60), // smaller resolution
                                                               Mode(3840, 2160, 60), // larger resolution
                                                               DisplayModeFacts{2560, 1440, 60, 16, 0},  // other bpp
                                                               DisplayModeFacts{2560, 1440, 60, 32, 90}, // rotated
                                                               Mode(2560, 1440, 60),                     // keep
                                                               Mode(2560, 1440, 144),                    // keep
                                                           });
    ASSERT_EQ(candidates.size(), 2u);
    for (const auto& mode : candidates) {
        EXPECT_TRUE(SameGeometry(current, mode)) << FormatModeFacts(mode);
    }
}

TEST(EnvctlDisplayMode, RepeatedModesCollapseAndTheListIsSortedByRate) {
    // EnumDisplaySettingsEx repeats a mode once per dmDisplayFlags variant. The
    // duplicates carry no information a caller can act on.
    const DisplayModeFacts current = Mode(2560, 1440, 144);
    const auto candidates =
        RefreshRateCandidates(current, {Mode(2560, 1440, 144), Mode(2560, 1440, 60), Mode(2560, 1440, 144),
                                        Mode(2560, 1440, 60), Mode(2560, 1440, 100)});
    EXPECT_EQ(RefreshRatesOf(candidates), (std::vector<unsigned long>{60, 100, 144}));
}

TEST(EnvctlDisplayMode, CurrentIsNotInjectedWhenWindowsDoesNotEnumerateIt) {
    // An overclocked or custom mode can be current without being enumerated.
    // Adding it back would make "current is in modes" unfalsifiable and would
    // claim the display offers something it never offered.
    const DisplayModeFacts current = Mode(2560, 1440, 165);
    const auto candidates = RefreshRateCandidates(current, {Mode(2560, 1440, 60), Mode(2560, 1440, 144)});
    EXPECT_EQ(RefreshRatesOf(candidates), (std::vector<unsigned long>{60, 144}));
}

// ----------------------------------------------------------------------- alias

TEST(EnvctlAlias, UnboundAliasNamesTheExactCommandToRun) {
    const auto load = AliasProfile::LoadFromJsonText(R"({"schemaVersion":1,"aliases":{}})");
    ASSERT_TRUE(load.ok);
    const auto resolved = load.profile.Resolve({"display.main-hdr"});
    EXPECT_FALSE(resolved.ok);
    ASSERT_EQ(resolved.errors.size(), 1u);
    EXPECT_EQ(resolved.errors.front().code, AliasErrorCode::UnboundAlias);
    EXPECT_EQ(ToKey(resolved.errors.front().code), "unbound_alias");
    const auto& message = resolved.errors.front().message;
    EXPECT_NE(std::string::npos, message.find("bind-alias --alias display.main-hdr --stable-id"));
    EXPECT_NE(std::string::npos, message.find("resolve-aliases"));
}

TEST(EnvctlAlias, AmbiguousFriendlyNameSelectsNothing) {
    const std::vector<DeviceCandidate> candidates{
        {device_kind::kDisplay, R"(\\?\DISPLAY#DEL41A1#5&1&UID4353)", "DELL U2723QE", ""},
        {device_kind::kDisplay, R"(\\?\DISPLAY#DEL41A1#5&1&UID4354)", "DELL U2723QE", ""},
    };
    const auto outcome = BindByFriendlyName("display.main-hdr", device_kind::kDisplay, "DELL U2723QE", candidates,
                                            "2026-08-17T10:00:00Z");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.error.code, AliasErrorCode::AmbiguousDevice);
    EXPECT_EQ(ToKey(outcome.error.code), "ambiguous_device");
    EXPECT_TRUE(outcome.binding.stable_id.empty());
    EXPECT_NE(std::string::npos, outcome.error.message.find("UID4353"));
    EXPECT_NE(std::string::npos, outcome.error.message.find("UID4354"));
}

TEST(EnvctlAlias, StableIdMatchingMoreThanOneDeviceIsAmbiguous) {
    const auto load = AliasProfile::LoadFromJsonText(R"({
        "schemaVersion": 1,
        "aliases": {
          "display.main-hdr": {"kind":"display","stableId":"SAME","friendlyName":"Panel","boundAtUtc":"x"}
        }})");
    ASSERT_TRUE(load.ok) << load.error.message;
    const std::vector<DeviceCandidate> candidates{
        {device_kind::kDisplay, "SAME", "Panel", ""},
        {device_kind::kDisplay, "SAME", "Panel", ""},
    };
    const auto reports = load.profile.ValidateAgainstInventory(candidates);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.front().status, BindingStatus::Ambiguous);
    EXPECT_EQ(ToKey(reports.front().status), "ambiguous_device");
}

TEST(EnvctlAlias, FriendlyNameIsDisplayOnlyAndDriftIsNotAFailure) {
    const auto load = AliasProfile::LoadFromJsonText(R"({
        "schemaVersion": 1,
        "aliases": {
          "display.main-hdr": {"kind":"display","stableId":"ID-1","friendlyName":"Old Name","boundAtUtc":"x"}
        }})");
    ASSERT_TRUE(load.ok);
    const std::vector<DeviceCandidate> candidates{{device_kind::kDisplay, "ID-1", "New Name", ""}};
    const auto reports = load.profile.ValidateAgainstInventory(candidates);
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports.front().status, BindingStatus::FriendlyNameChanged);
    EXPECT_EQ(reports.front().current_friendly_name, "New Name");
    // Still resolvable: the stable id is the key, the name is decoration.
    const auto resolved = load.profile.Resolve({"display.main-hdr"});
    EXPECT_TRUE(resolved.ok);
}

TEST(EnvctlAlias, ProfileWithoutStableIdIsRejected) {
    const auto load = AliasProfile::LoadFromJsonText(R"({
        "schemaVersion": 1,
        "aliases": {"display.main-hdr": {"kind":"display","friendlyName":"Panel"}}})");
    EXPECT_FALSE(load.ok);
    EXPECT_EQ(load.error.code, AliasErrorCode::ProfileInvalid);
}

TEST(EnvctlAlias, MissingProfileIsNotAnError) {
    TempDir temp;
    const auto load = AliasProfile::LoadFromFile((temp.Path() / "absent.json").string());
    EXPECT_TRUE(load.ok);
    EXPECT_EQ(load.error.code, AliasErrorCode::ProfileMissing);
    EXPECT_TRUE(load.profile.Bindings().empty());
}

TEST(EnvctlAlias, BindByStableIdRoundTripsThroughTheProfileFile) {
    TempDir temp;
    const auto path = (temp.Path() / "profile.json").string();
    const std::vector<DeviceCandidate> candidates{{device_kind::kDisplay, "ID-1", "Panel", ""}};
    const auto outcome =
        BindByStableId("display.main-hdr", device_kind::kDisplay, "ID-1", candidates, "2026-08-17T10:00:00Z");
    ASSERT_TRUE(outcome.ok) << outcome.error.message;

    AliasProfile profile;
    profile.Upsert(outcome.binding);
    std::string error;
    ASSERT_TRUE(profile.SaveToFile(path, error)) << error;

    const auto reloaded = AliasProfile::LoadFromFile(path);
    ASSERT_TRUE(reloaded.ok) << reloaded.error.message;
    const auto binding = reloaded.profile.Find("display.main-hdr");
    ASSERT_TRUE(binding.has_value());
    EXPECT_EQ(binding->stable_id, "ID-1");
    EXPECT_EQ(binding->friendly_name, "Panel");
}

// --------------------------------------------------------------------- journal

Journal SampleJournal() {
    Journal journal;
    journal.transaction_id = "tx-1";
    journal.owner_pid = 4242;
    journal.run_id = "run-9";
    journal.machine = MachineHashFromSeed("seed");
    journal.scenario = "hdr-off";
    journal.state = TransactionState::Prepared;
    journal.original = {{"display.main-hdr:hdr", "on"}};
    journal.desired = {{"display.main-hdr:hdr", "off"}};
    journal.created_at = "2026-08-17T10:00:00Z";
    journal.updated_at = "2026-08-17T10:00:00Z";
    return journal;
}

TEST(EnvctlJournal, RoundTripsEveryField) {
    Journal journal = SampleJournal();
    journal.state = TransactionState::Mutating;
    journal.applied.push_back({"display.main-hdr:hdr", "on", "off", "2026-08-17T10:00:01Z"});

    std::string error;
    const auto parsed = Journal::FromJsonText(journal.ToJsonText(), error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->schema_version, 1);
    EXPECT_EQ(parsed->transaction_id, "tx-1");
    EXPECT_EQ(parsed->owner_pid, 4242);
    EXPECT_EQ(parsed->run_id, "run-9");
    EXPECT_EQ(parsed->machine, journal.machine);
    EXPECT_EQ(parsed->scenario, "hdr-off");
    EXPECT_EQ(parsed->state, TransactionState::Mutating);
    EXPECT_EQ(parsed->original.at("display.main-hdr:hdr"), "on");
    EXPECT_EQ(parsed->desired.at("display.main-hdr:hdr"), "off");
    ASSERT_EQ(parsed->applied.size(), 1u);
    EXPECT_EQ(parsed->applied.front().from, "on");
    EXPECT_EQ(parsed->applied.front().to, "off");
}

TEST(EnvctlJournal, WriteIsAtomicAndLeavesNoTemporary) {
    TempDir temp;
    const auto path = temp.Path() / "journal.json";

    std::string error;
    ASSERT_TRUE(WriteJournalAtomic(path, SampleJournal(), error)) << error;
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(path.string() + ".tmp")));

    // The update path must replace in place, not append or fail on an existing
    // destination -- this is executed after every verified mutation.
    Journal updated = SampleJournal();
    updated.state = TransactionState::Active;
    updated.applied.push_back({"display.main-hdr:hdr", "on", "off", "2026-08-17T10:00:02Z"});
    ASSERT_TRUE(WriteJournalAtomic(path, updated, error)) << error;
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(path.string() + ".tmp")));

    const auto reread = ReadJournal(path, error);
    ASSERT_TRUE(reread.has_value()) << error;
    EXPECT_EQ(reread->state, TransactionState::Active);
    ASSERT_EQ(reread->applied.size(), 1u);

    int documents = 0;
    std::ifstream stream(path, std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    for (const char character : contents) {
        if (character == '{') {
            ++documents;
            break;
        }
    }
    EXPECT_EQ(documents, 1);
}

TEST(EnvctlJournal, AbsentJournalReadsAsCleanNotAsAnError) {
    TempDir temp;
    std::string error;
    const auto journal = ReadJournal(temp.Path() / "nothing.json", error);
    EXPECT_FALSE(journal.has_value());
    EXPECT_TRUE(error.empty());
}

TEST(EnvctlJournal, CorruptJournalIsNeverReportedAsClean) {
    TempDir temp;
    const auto path = temp.Path() / "journal.json";
    {
        std::ofstream stream(path, std::ios::binary);
        stream << "{\"schemaVersion\": 1, \"state\": "; // truncated mid-write
    }
    std::string error;
    const auto journal = ReadJournal(path, error);
    EXPECT_FALSE(journal.has_value());
    EXPECT_FALSE(error.empty());
}

TEST(EnvctlJournal, JournalWithAWrongTypedFieldIsReportedNotThrown) {
    // `value<T>(key, fallback)` throws nlohmann type_error when the key EXISTS with
    // the wrong type -- "schemaVersion": "1" is a string, not a missing field, so the
    // fallback never applies. Uncaught, that aborted the one command whose job is to
    // clean up a mangled journal: recovery died on the file it came to fix.
    TempDir temp;
    const auto path = temp.Path() / "journal.json";
    {
        std::ofstream stream(path, std::ios::binary);
        stream << R"({"schemaVersion": "1", "state": "active", "ownerPid": "not-a-number"})";
    }
    std::string error;
    const auto journal = ReadJournal(path, error);
    EXPECT_FALSE(journal.has_value());
    EXPECT_FALSE(error.empty()) << "a malformed journal must never read as 'no journal'";
}

TEST(EnvctlJournal, AJournalPathThatCannotBeOpenedIsAnErrorNotACleanMachine) {
    // The contract the header states: "I could not read it" must never be answered as
    // "there is nothing owed", because every caller treats clean as a licence to
    // mutate. A directory is the portable way to make the open fail.
    //
    // The sibling case -- exists() ITSELF failing with an error_code, which used to be
    // folded into "no journal" by the same `||` -- has no portable way to be provoked
    // from a test, so it is guarded by the code and by this contract, not by a case.
    TempDir temp;
    const auto path = temp.Path() / "journal.json";
    std::filesystem::create_directories(path);
    std::string error;
    const auto journal = ReadJournal(path, error);
    EXPECT_FALSE(journal.has_value());
    EXPECT_FALSE(error.empty()) << "an unreadable journal path must not report a clean machine";
}

TEST(EnvctlJournal, DeleteFailureReturnsFalseWithAReason) {
    // This primitive always answered correctly; what changed is that a caller now
    // BRANCHES on it (RestoreInternal reports the failure as a warning instead of
    // discarding it), so its return value became load-bearing and is pinned here.
    // A non-empty directory is the portable way to make remove() refuse.
    TempDir temp;
    const auto path = temp.Path() / "busy";
    std::filesystem::create_directories(path / "child");
    std::string error;
    EXPECT_FALSE(DeleteJournal(path, error));
    EXPECT_FALSE(error.empty());
}

TEST(EnvctlJournal, MachineHashIsStableAndNotTheHostname) {
    const auto first = StableMachineHash();
    EXPECT_EQ(first, StableMachineHash());
    EXPECT_EQ(first.size(), 16u);
    for (const char character : first) {
        EXPECT_TRUE((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')) << first;
    }
    EXPECT_NE(MachineHashFromSeed("a"), MachineHashFromSeed("b"));
}

} // namespace
} // namespace exosnap::envctl

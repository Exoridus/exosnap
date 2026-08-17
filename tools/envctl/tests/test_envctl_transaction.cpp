// The transaction state machine, against a fake host that can lie.
//
// The assertions worth reading twice are the dishonest ones: a setter that
// returns success and changes nothing MUST fail the transaction, and a restore
// whose read-back disagrees MUST end in RestoreFailed rather than in a cheerful
// "restored".

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "env_journal.h"
#include "env_transaction.h"
#include "fake_provider.h"
#include "temp_dir.h"

namespace exosnap::envctl {
namespace {

using exosnap::envctl::fakes::ApplyBehaviour;
using exosnap::envctl::fakes::FakeProvider;
using exosnap::envctl::fakes::PropertySpec;
using exosnap::envctl::fakes::TempDir;

constexpr const char* kHdr = "display.main-hdr:hdr";
constexpr const char* kRefresh = "display.main-hdr:refresh-hz";
constexpr const char* kSecondHdr = "display.second:hdr";

TransactionConfig MakeConfig(const TempDir& temp, std::string scenario = "scenario-1") {
    TransactionConfig config;
    config.journal_path = temp.File("env-journal.json");
    config.transaction_id = "tx-fixed";
    config.run_id = "run-1";
    config.scenario = std::move(scenario);
    config.owner_pid = 1234;
    config.machine = MachineHashFromSeed("test-machine");
    config.clock = [] { return std::string("2026-08-17T10:00:00Z"); };
    return config;
}

FakeProvider MakeProvider() {
    FakeProvider provider;
    provider.SetProperty(kHdr, PropertySpec{"on", CapabilityClass::MutateSafe, ApplyBehaviour::Normal, {}, false});
    provider.SetProperty(kRefresh, PropertySpec{"144", CapabilityClass::MutateSafe, ApplyBehaviour::Normal, {}, false});
    return provider;
}

// --------------------------------------------------------------------- snapshot

TEST(EnvctlTransaction, SnapshotRecordsTheOriginalBeforeAnythingIsTouched) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    EnvironmentTransaction transaction(provider, MakeConfig(temp));

    const auto result = transaction.Begin({{kHdr, "off"}, {kRefresh, "60"}});
    ASSERT_TRUE(result.ok) << result.error;

    const auto& journal = transaction.JournalDocument();
    EXPECT_EQ(journal.original.at(kHdr), "on");
    EXPECT_EQ(journal.original.at(kRefresh), "144");
    EXPECT_EQ(journal.desired.at(kHdr), "off");
    EXPECT_EQ(journal.desired.at(kRefresh), "60");
    EXPECT_EQ(journal.machine, MachineHashFromSeed("test-machine"));
    EXPECT_EQ(journal.owner_pid, 1234);
}

TEST(EnvctlTransaction, JournalIsOnDiskBeforeTheFirstMutation) {
    TempDir temp;
    // The first apply asserts that a journal already exists on disk at the
    // moment the host is first asked to change anything.
    const auto journal_path = temp.File("env-journal.json");
    bool journal_existed_at_first_apply = false;

    class ObservingProvider : public FakeProvider {
      public:
        ObservingProvider(std::filesystem::path path, bool& flag) : path_(std::move(path)), flag_(flag) {
        }
        ApplyResult Apply(const PropertyId& id, const std::string& value) override {
            if (!seen_) {
                seen_ = true;
                flag_ = std::filesystem::exists(path_);
            }
            return FakeProvider::Apply(id, value);
        }

      private:
        std::filesystem::path path_;
        bool& flag_;
        bool seen_{false};
    };

    ObservingProvider observing(journal_path, journal_existed_at_first_apply);
    observing.SetProperty(kHdr, PropertySpec{"on", CapabilityClass::MutateSafe, ApplyBehaviour::Normal, {}, false});

    EnvironmentTransaction transaction(observing, MakeConfig(temp));
    ASSERT_TRUE(transaction.Begin({{kHdr, "off"}}).ok);
    EXPECT_TRUE(journal_existed_at_first_apply);
}

// ------------------------------------------------------------------------ apply

TEST(EnvctlTransaction, ApplySuccessReachesActiveAndJournalsEveryMutation) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    EnvironmentTransaction transaction(provider, MakeConfig(temp));

    ASSERT_TRUE(transaction.Begin({{kHdr, "off"}, {kRefresh, "60"}}).ok);
    EXPECT_EQ(transaction.State(), TransactionState::Active);
    EXPECT_EQ(provider.ValueOf(kHdr), "off");
    EXPECT_EQ(provider.ValueOf(kRefresh), "60");

    std::string error;
    const auto on_disk = ReadJournal(temp.File("env-journal.json"), error);
    ASSERT_TRUE(on_disk.has_value()) << error;
    EXPECT_EQ(on_disk->state, TransactionState::Active);
    ASSERT_EQ(on_disk->applied.size(), 2u);
    EXPECT_EQ(on_disk->applied.front().property, kHdr);
    EXPECT_EQ(on_disk->applied.front().from, "on");
    EXPECT_EQ(on_disk->applied.front().to, "off");
}

TEST(EnvctlTransaction, NoOpDesiredStateAppliesNothingAndJournalsNothing) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    EnvironmentTransaction transaction(provider, MakeConfig(temp));

    const auto result = transaction.Begin({{kHdr, "on"}, {kRefresh, "144"}});
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(transaction.State(), TransactionState::Active);
    EXPECT_TRUE(provider.ApplyCalls().empty());
    EXPECT_TRUE(transaction.JournalDocument().applied.empty());

    const auto restore = transaction.Restore();
    EXPECT_TRUE(restore.ok);
    EXPECT_EQ(restore.state, TransactionState::Restored);
    EXPECT_TRUE(provider.ApplyCalls().empty());
    EXPECT_FALSE(std::filesystem::exists(temp.File("env-journal.json")));
    EXPECT_TRUE(transaction.Evidence().Accepted());
}

TEST(EnvctlTransaction, ApplyRejectedRollsBackWhatWasAlreadyApplied) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    provider.SetBehaviour(kRefresh, ApplyBehaviour::Reject);
    EnvironmentTransaction transaction(provider, MakeConfig(temp));

    const auto result = transaction.Begin({{kHdr, "off"}, {kRefresh, "60"}});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, error_code::kApplyRejected);
    EXPECT_EQ(result.state, TransactionState::Restored);
    EXPECT_EQ(provider.ValueOf(kHdr), "on");
    EXPECT_EQ(provider.ValueOf(kRefresh), "144");
    EXPECT_FALSE(std::filesystem::exists(temp.File("env-journal.json")));
}

TEST(EnvctlTransaction, SetterThatClaimsSuccessButChangesNothingIsAFailure) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    // The exact real-world shape: DisplayConfigSetDeviceInfo returns ERROR_SUCCESS
    // and the panel stays where it was because a policy override clamped it.
    provider.SetBehaviour(kHdr, ApplyBehaviour::AcceptAndIgnore);
    EnvironmentTransaction transaction(provider, MakeConfig(temp));

    const auto result = transaction.Begin({{kHdr, "off"}});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, error_code::kVerifyMismatch);
    EXPECT_NE(std::string::npos, result.error.find("reads back 'on'"));
    EXPECT_EQ(result.state, TransactionState::Restored);
    EXPECT_EQ(provider.ValueOf(kHdr), "on");
    EXPECT_FALSE(std::filesystem::exists(temp.File("env-journal.json")));
}

TEST(EnvctlTransaction, SetterThatLandsOnADifferentValueIsAFailure) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    // A panel that substitutes the nearest supported refresh rate.
    provider.SetBehaviour(kRefresh, ApplyBehaviour::AcceptAndDistort, "59");
    EnvironmentTransaction transaction(provider, MakeConfig(temp));

    const auto result = transaction.Begin({{kRefresh, "60"}});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, error_code::kVerifyMismatch);
    // The rollback puts the ORIGINAL back, not the substituted value.
    EXPECT_EQ(provider.ValueOf(kRefresh), "144");
    EXPECT_EQ(result.state, TransactionState::Restored);
}

TEST(EnvctlTransaction, NonMutablePropertyIsRefusedAndLeavesNoJournal) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    provider.SetProperty("display.main-hdr:acm",
                         PropertySpec{"off", CapabilityClass::Human, ApplyBehaviour::Normal, {}, false});
    EnvironmentTransaction transaction(provider, MakeConfig(temp));

    const auto result = transaction.Begin({{"display.main-hdr:acm", "on"}});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, error_code::kNotMutable);
    EXPECT_NE(std::string::npos, result.error.find("ENV_HUMAN"));
    EXPECT_TRUE(provider.ApplyCalls().empty());
    EXPECT_FALSE(std::filesystem::exists(temp.File("env-journal.json")));
}

// ---------------------------------------------------------------------- restore

TEST(EnvctlTransaction, RestorePutsTheExactOriginalBackInReverseOrder) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    EnvironmentTransaction transaction(provider, MakeConfig(temp));
    ASSERT_TRUE(transaction.Begin({{kHdr, "off"}, {kRefresh, "60"}}).ok);
    provider.ClearApplyCalls();

    const auto result = transaction.Restore();
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.state, TransactionState::Restored);
    EXPECT_EQ(provider.ValueOf(kHdr), "on");
    EXPECT_EQ(provider.ValueOf(kRefresh), "144");
    EXPECT_FALSE(std::filesystem::exists(temp.File("env-journal.json")));

    ASSERT_EQ(provider.ApplyCalls().size(), 2u);
    EXPECT_EQ(provider.ApplyCalls().front().first, kRefresh); // reverse of application order
    EXPECT_EQ(provider.ApplyCalls().back().first, kHdr);

    const auto evidence = transaction.Evidence();
    EXPECT_TRUE(evidence.Accepted());
    ASSERT_EQ(evidence.properties.size(), 2u);
    for (const auto& property : evidence.properties) {
        EXPECT_EQ(property.before, property.after_restore) << property.property;
        EXPECT_EQ(property.applied, property.requested) << property.property;
    }
}

TEST(EnvctlTransaction, RestoreSetterThatSucceedsButReadsBackDifferentlyIsTerminalRestoreFailed) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    EnvironmentTransaction transaction(provider, MakeConfig(temp));
    ASSERT_TRUE(transaction.Begin({{kHdr, "off"}}).ok);

    // Only the restore lies: the setter reports success and the value stays off.
    provider.SetBehaviour(kHdr, ApplyBehaviour::AcceptAndIgnore);

    const auto result = transaction.Restore();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, error_code::kRestoreVerifyMismatch);
    EXPECT_EQ(result.state, TransactionState::RestoreFailed);

    std::string error;
    const auto on_disk = ReadJournal(temp.File("env-journal.json"), error);
    ASSERT_TRUE(on_disk.has_value()) << error;
    EXPECT_EQ(on_disk->state, TransactionState::RestoreFailed);
    ASSERT_EQ(on_disk->applied.size(), 1u) << "the outstanding obligation must survive on disk";
    EXPECT_EQ(on_disk->applied.front().from, "on");

    const auto evidence = transaction.Evidence();
    EXPECT_FALSE(evidence.Accepted());
}

TEST(EnvctlTransaction, RestoreIsIdempotentAndSafeFromAFinally) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    EnvironmentTransaction transaction(provider, MakeConfig(temp));
    ASSERT_TRUE(transaction.Begin({{kHdr, "off"}}).ok);

    const auto first = transaction.Restore();
    ASSERT_TRUE(first.ok);
    provider.ClearApplyCalls();

    const auto second = transaction.Restore();
    EXPECT_TRUE(second.ok);
    EXPECT_EQ(second.state, TransactionState::Restored);
    EXPECT_TRUE(provider.ApplyCalls().empty()) << "a second Restore() must not touch the host again";

    const auto third = transaction.Restore();
    EXPECT_EQ(third.state, TransactionState::Restored);
    EXPECT_TRUE(provider.ApplyCalls().empty());
}

TEST(EnvctlTransaction, RestoreAfterAFailedBeginIsANoOp) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    provider.SetBehaviour(kHdr, ApplyBehaviour::Reject);
    EnvironmentTransaction transaction(provider, MakeConfig(temp));

    EXPECT_FALSE(transaction.Begin({{kHdr, "off"}}).ok);
    provider.ClearApplyCalls();
    const auto restore = transaction.Restore();
    EXPECT_TRUE(restore.ok);
    EXPECT_TRUE(provider.ApplyCalls().empty());
}

// ------------------------------------------------------- device gone at restore

TEST(EnvctlTransaction, DeviceGoneAtRestoreTimeTouchesNoOtherDevice) {
    TempDir temp;
    FakeProvider provider;
    provider.SetProperty(kHdr, PropertySpec{"on", CapabilityClass::MutateSafe, ApplyBehaviour::Normal, {}, false});
    provider.SetProperty(kSecondHdr,
                         PropertySpec{"off", CapabilityClass::MutateSafe, ApplyBehaviour::Normal, {}, false});

    auto config = MakeConfig(temp);
    config.alias_lookup = [](const std::string& alias) -> std::optional<AliasBinding> {
        if (alias == "display.second") {
            return AliasBinding{alias, "display", R"(\\?\DISPLAY#DEL41A1#5&1&UID4354)", "DELL U2723QE",
                                "2026-08-01T00:00:00Z"};
        }
        return AliasBinding{alias, "display", R"(\\?\DISPLAY#DEL41A1#5&1&UID4353)", "DELL U2723QE",
                            "2026-08-01T00:00:00Z"};
    };

    EnvironmentTransaction transaction(provider, config);
    ASSERT_TRUE(transaction.Begin({{kHdr, "off"}, {kSecondHdr, "on"}}).ok);
    provider.ClearApplyCalls();

    provider.RemoveDevice("display.second");

    const auto result = transaction.Restore();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.state, TransactionState::RestorePendingDeviceUnavailable);
    EXPECT_EQ(result.error_code, error_code::kDeviceNotPresent);

    // The whole point: the still-present display was NOT restored halfway.
    EXPECT_TRUE(provider.ApplyCalls().empty());
    EXPECT_TRUE(provider.MutatedAliases().empty());
    EXPECT_EQ(provider.ValueOf(kHdr), "off");

    ASSERT_EQ(result.pending.size(), 2u);
    const DevicePendingRestore* missing = nullptr;
    for (const auto& entry : result.pending) {
        if (entry.alias == "display.second") {
            missing = &entry;
        }
    }
    ASSERT_NE(missing, nullptr);
    EXPECT_EQ(missing->stable_id, R"(\\?\DISPLAY#DEL41A1#5&1&UID4354)");
    EXPECT_EQ(missing->friendly_name, "DELL U2723QE");
    EXPECT_EQ(missing->original_value, "off");
    EXPECT_NE(std::string::npos, missing->remaining_action.find("Reattach"));
    EXPECT_NE(std::string::npos, missing->remaining_action.find("recover --journal"));

    std::string error;
    const auto on_disk = ReadJournal(temp.File("env-journal.json"), error);
    ASSERT_TRUE(on_disk.has_value()) << error;
    EXPECT_EQ(on_disk->state, TransactionState::RestorePendingDeviceUnavailable);
    EXPECT_EQ(on_disk->applied.size(), 2u);
}

// ------------------------------------------------------------- the startup gate

TEST(EnvctlRecovery, BeginRefusesWhileAJournalIsOnDisk) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    {
        EnvironmentTransaction first(provider, MakeConfig(temp));
        ASSERT_TRUE(first.Begin({{kHdr, "off"}}).ok);
    } // killed without restoring

    EnvironmentTransaction second(provider, MakeConfig(temp, "scenario-2"));
    const auto result = second.Begin({{kRefresh, "60"}});
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error_code, error_code::kDirtyJournal);
    EXPECT_NE(std::string::npos, result.error.find("recover --journal"));
    EXPECT_EQ(provider.ValueOf(kRefresh), "144") << "a blocked transaction must not have mutated anything";
}

TEST(EnvctlRecovery, CleanStartupAllowsMutation) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    const auto outcome = RecoverIfDirty(provider, MakeConfig(temp));
    EXPECT_FALSE(outcome.journal_present);
    EXPECT_FALSE(outcome.recovered);
    EXPECT_TRUE(outcome.mutation_allowed);
    EXPECT_EQ(outcome.state, TransactionState::Clean);
}

TEST(EnvctlRecovery, UnreadableJournalNeverAllowsMutation) {
    TempDir temp;
    FakeProvider provider = MakeProvider();
    {
        std::ofstream stream(temp.File("env-journal.json"), std::ios::binary);
        stream << "{\"schemaVersion\":1,\"state\":\"Prep";
    }
    const auto outcome = RecoverIfDirty(provider, MakeConfig(temp));
    EXPECT_TRUE(outcome.journal_present);
    EXPECT_FALSE(outcome.recovered);
    EXPECT_FALSE(outcome.mutation_allowed);
    EXPECT_EQ(outcome.error_code, error_code::kJournalReadFailed);
}

// Every dirty state a kill can leave behind. Each must restore the machine and
// only then release the mutation gate.
class DirtyStartupRecovery : public ::testing::TestWithParam<TransactionState> {};

TEST_P(DirtyStartupRecovery, RestoresAndOnlyThenUnblocksMutation) {
    TempDir temp;
    FakeProvider provider = MakeProvider();

    // Hand-build the on-disk journal for the state under test, and put the host
    // where a kill in that state would have left it.
    Journal journal;
    journal.transaction_id = "tx-killed";
    journal.owner_pid = 999;
    journal.run_id = "run-killed";
    journal.machine = MachineHashFromSeed("test-machine");
    journal.scenario = "killed";
    journal.state = GetParam();
    journal.original = {{kHdr, "on"}, {kRefresh, "144"}};
    journal.desired = {{kHdr, "off"}, {kRefresh, "60"}};
    journal.created_at = "2026-08-17T10:00:00Z";
    journal.updated_at = "2026-08-17T10:00:00Z";

    if (GetParam() != TransactionState::Prepared) {
        journal.applied.push_back({kHdr, "on", "off", "2026-08-17T10:00:01Z"});
        provider.SetValue(kHdr, "off");
    }
    if (GetParam() == TransactionState::Active || GetParam() == TransactionState::Restoring) {
        journal.applied.push_back({kRefresh, "144", "60", "2026-08-17T10:00:02Z"});
        provider.SetValue(kRefresh, "60");
    }

    std::string error;
    ASSERT_TRUE(WriteJournalAtomic(temp.File("env-journal.json"), journal, error)) << error;

    // Before recovery: a new mutating transaction is hard-blocked.
    {
        EnvironmentTransaction blocked(provider, MakeConfig(temp, "next"));
        const auto refused = blocked.Begin({{kHdr, "off"}});
        EXPECT_FALSE(refused.ok);
        EXPECT_EQ(refused.error_code, error_code::kDirtyJournal);
    }

    const auto outcome = RecoverIfDirty(provider, MakeConfig(temp));
    EXPECT_TRUE(outcome.journal_present);
    EXPECT_TRUE(outcome.recovered) << outcome.error;
    EXPECT_TRUE(outcome.mutation_allowed);
    EXPECT_EQ(outcome.state, TransactionState::Restored);
    EXPECT_EQ(provider.ValueOf(kHdr), "on");
    EXPECT_EQ(provider.ValueOf(kRefresh), "144");
    EXPECT_FALSE(std::filesystem::exists(temp.File("env-journal.json")));

    // After recovery: a new transaction is allowed again.
    EnvironmentTransaction next(provider, MakeConfig(temp, "next"));
    EXPECT_TRUE(next.Begin({{kHdr, "off"}}).ok);
    EXPECT_TRUE(next.Restore().ok);
}

INSTANTIATE_TEST_SUITE_P(AllDirtyStates, DirtyStartupRecovery,
                         ::testing::Values(TransactionState::Prepared, TransactionState::Mutating,
                                           TransactionState::Active, TransactionState::Restoring),
                         [](const ::testing::TestParamInfo<TransactionState>& info) {
                             return std::string(ToKey(info.param));
                         });

TEST(EnvctlRecovery, RecoveryThatCannotVerifyKeepsTheGateClosed) {
    TempDir temp;
    FakeProvider provider = MakeProvider();

    Journal journal;
    journal.transaction_id = "tx-killed";
    journal.run_id = "run-killed";
    journal.machine = MachineHashFromSeed("test-machine");
    journal.scenario = "killed";
    journal.state = TransactionState::Active;
    journal.original = {{kHdr, "on"}};
    journal.desired = {{kHdr, "off"}};
    journal.applied.push_back({kHdr, "on", "off", "2026-08-17T10:00:01Z"});
    journal.created_at = "2026-08-17T10:00:00Z";
    journal.updated_at = "2026-08-17T10:00:00Z";

    provider.SetValue(kHdr, "off");
    provider.SetBehaviour(kHdr, ApplyBehaviour::AcceptAndIgnore);

    std::string error;
    ASSERT_TRUE(WriteJournalAtomic(temp.File("env-journal.json"), journal, error)) << error;

    const auto outcome = RecoverIfDirty(provider, MakeConfig(temp));
    EXPECT_TRUE(outcome.journal_present);
    EXPECT_FALSE(outcome.recovered);
    EXPECT_FALSE(outcome.mutation_allowed);
    EXPECT_EQ(outcome.state, TransactionState::RestoreFailed);
    EXPECT_TRUE(std::filesystem::exists(temp.File("env-journal.json")));
}

TEST(EnvctlRecovery, RecoveryWithAMissingDeviceReportsWhatIsStillOwed) {
    TempDir temp;
    FakeProvider provider = MakeProvider();

    Journal journal;
    journal.transaction_id = "tx-killed";
    journal.run_id = "run-killed";
    journal.machine = MachineHashFromSeed("test-machine");
    journal.scenario = "killed";
    journal.state = TransactionState::Active;
    journal.original = {{kHdr, "on"}};
    journal.desired = {{kHdr, "off"}};
    journal.applied.push_back({kHdr, "on", "off", "2026-08-17T10:00:01Z"});
    journal.created_at = "2026-08-17T10:00:00Z";
    journal.updated_at = "2026-08-17T10:00:00Z";

    std::string error;
    ASSERT_TRUE(WriteJournalAtomic(temp.File("env-journal.json"), journal, error)) << error;
    provider.RemoveDevice("display.main-hdr");

    const auto outcome = RecoverIfDirty(provider, MakeConfig(temp));
    EXPECT_FALSE(outcome.recovered);
    EXPECT_FALSE(outcome.mutation_allowed);
    EXPECT_EQ(outcome.state, TransactionState::RestorePendingDeviceUnavailable);
    ASSERT_EQ(outcome.pending.size(), 1u);
    EXPECT_EQ(outcome.pending.front().property, kHdr);
    EXPECT_EQ(outcome.pending.front().original_value, "on");
    EXPECT_TRUE(provider.ApplyCalls().empty());
}

} // namespace
} // namespace exosnap::envctl

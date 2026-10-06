#include "services/UpdateService.h"

#include "ExoSnapBuildInfo.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace {

using exosnap::UpdateService;
using exosnap::update::DistributionContext;
using exosnap::update::DistributionOwner;
using exosnap::update::InstallMode;

std::string ReleaseFeed(const QString& version) {
    return QStringLiteral(R"([
        {"tag_name":"v%1","prerelease":false,"draft":false,"body":"Release notes for the offered version.",
         "html_url":"https://example.invalid/releases/v%1","assets":[
          {"name":"update-manifest.json","browser_download_url":"https://127.0.0.1:1/update-manifest.json"},
          {"name":"update-manifest.json.sig","browser_download_url":"https://127.0.0.1:1/update-manifest.json.sig"},
          {"name":"ExoSnap-%1-windows-x64.msi","browser_download_url":"https://127.0.0.1:1/package.msi"},
          {"name":"ExoSnap-%1-windows-x64-portable.zip","browser_download_url":"https://127.0.0.1:1/package.zip"}]}
    ])")
        .arg(version)
        .toStdString();
}

exosnap::update::UpdateCheckResult CheckFeed(const std::string& feed, const exosnap::update::CheckParams& params) {
    return exosnap::update::BuildCheckResult(feed, exosnap::update::LocateRelease(feed, params.channel), params);
}

std::optional<exosnap::update::UpdateCheckResult> CompleteCheck(UpdateService& service) {
    QEventLoop loop;
    std::optional<exosnap::update::UpdateCheckResult> result;
    const auto connection = QObject::connect(&service, &UpdateService::updateCheckComplete, &loop,
                                             [&](const exosnap::update::UpdateCheckResult& completed) {
                                                 result = completed;
                                                 loop.quit();
                                             });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    service.RequestUpdateCheck();
    loop.exec();
    QObject::disconnect(connection);
    return result;
}

class UpdateServiceDistributionTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        if (!QCoreApplication::instance()) {
            static int argc = 1;
            static char name[] = "update_service_distribution_tests";
            static char* argv[] = {name, nullptr};
            static QCoreApplication app(argc, argv);
        }
    }

    void SetUp() override {
        ASSERT_TRUE(scratch_.isValid());
        old_config_dir_ = qgetenv("EXOSNAP_CONFIG_DIR");
        config_override_was_set_ = qEnvironmentVariableIsSet("EXOSNAP_CONFIG_DIR");
        qputenv("EXOSNAP_CONFIG_DIR", scratch_.path().toUtf8());
    }

    void TearDown() override {
        if (config_override_was_set_)
            qputenv("EXOSNAP_CONFIG_DIR", old_config_dir_);
        else
            qunsetenv("EXOSNAP_CONFIG_DIR");
    }

    QTemporaryDir scratch_;
    QByteArray old_config_dir_;
    bool config_override_was_set_ = false;
};

class ManagedUpdateServiceTest : public UpdateServiceDistributionTest,
                                 public ::testing::WithParamInterface<DistributionContext> {};

TEST_P(ManagedUpdateServiceTest, LaunchUpdaterRefusesBeforeStagingOrChildLaunch) {
    for (const bool verify_reinstall : {false, true}) {
        UpdateService service(nullptr, GetParam());
        service.SetVerifyReinstallMode(verify_reinstall);
        std::vector<QString> errors;
        int launches = 0;
        QObject::connect(&service, &UpdateService::updateError, &service,
                         [&](exosnap::update::VerifyResult, const QString& detail) { errors.push_back(detail); });
        QObject::connect(&service, &UpdateService::updaterLaunched, &service, [&] { ++launches; });

        service.LaunchUpdater();

        ASSERT_EQ(errors.size(), 1u);
        EXPECT_TRUE(errors.front().contains(QStringLiteral("managed externally"))) << errors.front().toStdString();
        EXPECT_EQ(launches, 0);
        EXPECT_EQ(service.LastUpdaterLaunch().pid, 0);
        EXPECT_TRUE(service.LastUpdaterLaunch().staged_exe.isEmpty());
        EXPECT_TRUE(service.LastUpdaterLaunch().handoff_path.isEmpty());
        EXPECT_EQ(service.CurrentState().distribution, GetParam());
        EXPECT_FALSE(QFileInfo(QDir(scratch_.path()).filePath(QStringLiteral("updater"))).exists());
        EXPECT_FALSE(QFileInfo(QDir(scratch_.path()).filePath(QStringLiteral("update-transactions"))).exists());
    }
}

TEST_P(ManagedUpdateServiceTest, InstallerHandoffRefusesManagedOwnership) {
    UpdateService service(nullptr, GetParam());
    std::vector<QString> errors;
    int launches = 0;
    QObject::connect(&service, &UpdateService::updateError, &service,
                     [&](exosnap::update::VerifyResult, const QString& detail) { errors.push_back(detail); });
    QObject::connect(&service, &UpdateService::updaterLaunched, &service, [&] { ++launches; });

    service.HandoffToInstaller(QDir(scratch_.path()).filePath(QStringLiteral("absent-installer.msi")));

    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors.front().contains(QStringLiteral("managed externally"))) << errors.front().toStdString();
    EXPECT_EQ(launches, 0);
    EXPECT_EQ(service.LastUpdaterLaunch().pid, 0);
    EXPECT_FALSE(service.CurrentState().pending_restart);
}

TEST_P(ManagedUpdateServiceTest, DiscoveryPublishesOfferAndNotesWithoutPreparingArtifacts) {
    const std::string feed = ReleaseFeed(QStringLiteral("9999.0.0"));
    std::atomic<int> checks{0};
    UpdateService service(nullptr, GetParam(), nullptr, [&](const exosnap::update::CheckParams& params) {
        ++checks;
        return CheckFeed(feed, params);
    });
    std::vector<QString> errors;
    int launches = 0;
    QObject::connect(&service, &UpdateService::updateError, &service,
                     [&](exosnap::update::VerifyResult, const QString& detail) { errors.push_back(detail); });
    QObject::connect(&service, &UpdateService::updaterLaunched, &service, [&] { ++launches; });

    const auto result = CompleteCheck(service);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->update_available);
    EXPECT_FALSE(result->check_failed);
    EXPECT_EQ(result->available_version_raw, "9999.0.0");
    EXPECT_EQ(checks.load(), 1);
    EXPECT_EQ(service.CurrentState().distribution, GetParam());
    EXPECT_TRUE(service.CurrentState().update_available);
    EXPECT_FALSE(service.CurrentState().checking);
    ASSERT_EQ(service.LastGapNotes().size(), 1u);
    EXPECT_EQ(service.LastGapNotes().front().body_markdown, "Release notes for the offered version.");
    ASSERT_EQ(service.LastAllChannelNotes().size(), 1u);
    EXPECT_TRUE(service.LastPreparedUpdate().update_transaction_id.isEmpty());
    EXPECT_TRUE(service.LastPreparedUpdate().directory.isEmpty());
    EXPECT_TRUE(service.LastPreparedUpdate().manifest_path.isEmpty());
    EXPECT_TRUE(service.LastPreparedUpdate().manifest_signature_path.isEmpty());
    EXPECT_TRUE(service.LastPreparedUpdate().target_version.isEmpty());
    EXPECT_TRUE(service.LastPreparedUpdate().error.isEmpty());
    EXPECT_FALSE(QFileInfo(QDir(scratch_.path()).filePath(QStringLiteral("update-transactions"))).exists());
    EXPECT_FALSE(QFileInfo(QDir(scratch_.path()).filePath(QStringLiteral("updater"))).exists());
    EXPECT_EQ(service.LastUpdaterLaunch().pid, 0);
    service.LaunchUpdater();
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_TRUE(errors.front().contains(QStringLiteral("managed externally")));
    EXPECT_EQ(launches, 0);
    EXPECT_EQ(service.LastUpdaterLaunch().pid, 0);
}

TEST_P(ManagedUpdateServiceTest, MissingTrustAnchorStillSkipsPreparationAndVerificationReinstall) {
    const std::string feed = ReleaseFeed(QString::fromLatin1(exosnap::build::kVersion));
    UpdateService service(nullptr, GetParam(), nullptr, [&](const exosnap::update::CheckParams& params) {
        auto result = CheckFeed(feed, params);
        result.manifest_url.clear();
        result.manifest_signature_url.clear();
        return result;
    });
    service.SetVerifyReinstallMode(true);

    const auto result = CompleteCheck(service);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->update_available);
    EXPECT_TRUE(result->verification_reinstall);
    EXPECT_EQ(result->available_version_raw, exosnap::build::kVersion);
    EXPECT_TRUE(service.LastPreparedUpdate().error.isEmpty());
    EXPECT_TRUE(service.LastPreparedUpdate().target_version.isEmpty());
    EXPECT_FALSE(QFileInfo(QDir(scratch_.path()).filePath(QStringLiteral("update-transactions"))).exists());
}

TEST_P(ManagedUpdateServiceTest, FailedCheckPublishesFailureWithoutOfferingUpdate) {
    UpdateService service(nullptr, GetParam(), nullptr, [](const exosnap::update::CheckParams&) {
        exosnap::update::UpdateCheckResult result;
        result.check_failed = true;
        result.error_message = "Controlled feed failure";
        return result;
    });

    const auto result = CompleteCheck(service);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->check_failed);
    EXPECT_FALSE(result->update_available);
    EXPECT_EQ(service.CurrentState().last_error, "Controlled feed failure");
    EXPECT_FALSE(service.CurrentState().checking);
    EXPECT_TRUE(service.LastPreparedUpdate().error.isEmpty());
    EXPECT_EQ(service.CurrentState().distribution, GetParam());
}

TEST_P(ManagedUpdateServiceTest, InvalidAndOlderReleaseTagsDoNotOfferDowngrade) {
    for (const QString& version : {QStringLiteral("not-a-version"), QStringLiteral("0.0.0")}) {
        UpdateService service(nullptr, GetParam(), nullptr, [&](const exosnap::update::CheckParams& params) {
            return CheckFeed(ReleaseFeed(version), params);
        });

        const auto result = CompleteCheck(service);

        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(result->check_failed);
        EXPECT_FALSE(result->update_available);
        EXPECT_FALSE(service.CurrentState().update_available);
        EXPECT_TRUE(service.LastGapNotes().empty());
        EXPECT_TRUE(service.LastPreparedUpdate().error.isEmpty());
        EXPECT_TRUE(service.LastPreparedUpdate().target_version.isEmpty());
    }
}

TEST_F(UpdateServiceDistributionTest, DirectOfferPreparesAndRetainsExactVersionWhenTrustAnchorIsMissing) {
    const QString target = QStringLiteral("9999.0.0-rc.7");
    const std::string feed = ReleaseFeed(target);
    UpdateService service(nullptr, DistributionContext{InstallMode::Portable, DistributionOwner::Direct}, nullptr,
                          [&](const exosnap::update::CheckParams& params) {
                              auto result = CheckFeed(feed, params);
                              result.manifest_url.clear();
                              result.manifest_signature_url.clear();
                              return result;
                          });

    const auto result = CompleteCheck(service);

    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->update_available);
    EXPECT_EQ(result->available_version_raw, target.toStdString());
    EXPECT_EQ(service.LastPreparedUpdate().target_version, target);
    EXPECT_TRUE(service.LastPreparedUpdate().error.contains(QStringLiteral("no signed update manifest")));
    EXPECT_TRUE(service.LastPreparedUpdate().manifest_path.isEmpty());
    EXPECT_TRUE(service.LastPreparedUpdate().update_transaction_id.isEmpty());
    EXPECT_EQ(service.LastUpdaterLaunch().pid, 0);
}

INSTANTIATE_TEST_SUITE_P(
    ManagedOwners, ManagedUpdateServiceTest,
    ::testing::Values(DistributionContext{InstallMode::Installed, DistributionOwner::WinGet},
                      DistributionContext{InstallMode::Installed, DistributionOwner::Chocolatey},
                      DistributionContext{InstallMode::Portable, DistributionOwner::Scoop},
                      DistributionContext{InstallMode::Installed, DistributionOwner::UnknownManaged},
                      DistributionContext{InstallMode::Portable, DistributionOwner::WinGet},
                      DistributionContext{InstallMode::Portable, DistributionOwner::Chocolatey},
                      DistributionContext{InstallMode::Installed, DistributionOwner::Scoop},
                      DistributionContext{InstallMode::Portable, DistributionOwner::UnknownManaged}));

} // namespace

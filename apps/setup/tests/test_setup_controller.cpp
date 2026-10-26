#include <gtest/gtest.h>

#include "SetupController.h"

using namespace exosnap::setup;

namespace {

SetupEvent Detected(bool installed = false) {
    SetupEvent event{SetupEventKind::Detected};
    event.installed = installed;
    event.version = installed ? L"0.10.1" : nullptr;
    return event;
}

} // namespace

TEST(SetupControllerTest, FreshInstallHasFixedPathAndOptInShortcut) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    controller.receive(Detected());
    EXPECT_EQ(controller.page(), "install");
    EXPECT_TRUE(controller.decision());
    EXPECT_FALSE(controller.desktopShortcut());
    EXPECT_FALSE(controller.removeUserData());
    EXPECT_EQ(controller.primaryText(), "Install");
}

TEST(SetupControllerTest, ExistingInstallOpensManageAndKeepsDataByDefault) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    controller.receive(Detected(true));
    EXPECT_EQ(controller.page(), "manage");
    controller.chooseUninstall();
    EXPECT_EQ(controller.page(), "uninstall");
    EXPECT_FALSE(controller.removeUserData());
    EXPECT_EQ(controller.primaryText(), "Uninstall");
    controller.setRemoveUserData(true);
    EXPECT_EQ(controller.primaryText(), "Uninstall and delete data");
}

TEST(SetupControllerTest, OlderBundleOffersUpgradeWithBothVersions) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    SetupEvent event = Detected();
    event.version = L"0.10.1";
    controller.receive(event);
    EXPECT_EQ(controller.page(), "install");
    EXPECT_EQ(controller.headline(), "Update ExoSnap");
    EXPECT_EQ(controller.primaryText(), "Update");
    EXPECT_TRUE(controller.versionText().contains("0.10.1"));
    EXPECT_TRUE(controller.versionText().contains("0.11.0"));
}

TEST(SetupControllerTest, BackDiscardsDestructiveOptionAndRepairNeverDeletesData) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    controller.receive(Detected(true));
    controller.chooseUninstall();
    controller.setRemoveUserData(true);
    controller.activateSecondary();
    EXPECT_EQ(controller.page(), "manage");
    EXPECT_FALSE(controller.removeUserData());
    bool repair_requested = false;
    QObject::connect(&controller, &SetupController::applyRequested, [&](SetupAction action, bool, bool remove) {
        repair_requested = action == SetupAction::Repair;
        EXPECT_FALSE(remove);
    });
    controller.chooseRepair();
    EXPECT_TRUE(repair_requested);
}

TEST(SetupControllerTest, ProtectedApplyRefusesCloseAndOptionChanges) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    controller.receive(Detected());
    controller.activatePrimary();
    EXPECT_EQ(controller.page(), "applying");
    EXPECT_FALSE(controller.closeEnabled());
    EXPECT_FALSE(controller.requestClose());
    EXPECT_TRUE(controller.primaryText().isEmpty());
    controller.setDesktopShortcut(true);
    EXPECT_FALSE(controller.desktopShortcut());
}

TEST(SetupControllerTest, CancellationWaitsForBurnCompletionAndPreservesExitCode) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    controller.receive(Detected());
    controller.activatePrimary();
    SetupEvent event{SetupEventKind::Applying};
    event.can_cancel = true;
    controller.receive(event);
    int cancels = 0;
    QObject::connect(&controller, &SetupController::cancelRequested, [&] { ++cancels; });
    EXPECT_FALSE(controller.requestClose());
    EXPECT_TRUE(controller.cancelConfirmationVisible());
    EXPECT_EQ(cancels, 0);
    controller.confirmCancel();
    EXPECT_EQ(cancels, 1);
    EXPECT_EQ(controller.page(), "applying");
    EXPECT_FALSE(controller.closeEnabled());
    event = SetupEvent{SetupEventKind::Completed};
    event.status = static_cast<std::int32_t>(0x80070642U);
    controller.receive(event);
    EXPECT_EQ(controller.page(), "cancelled");
    std::uint32_t result = 0;
    QObject::connect(&controller, &SetupController::finished, [&](std::uint32_t code) { result = code; });
    EXPECT_TRUE(controller.requestClose());
    EXPECT_EQ(result, 1602U);
}

TEST(SetupControllerTest, ProgressComesFromBurnAndCannotExceedTheBar) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    controller.receive(Detected());
    controller.activatePrimary();
    EXPECT_TRUE(controller.indeterminate());
    SetupEvent event{SetupEventKind::Progress};
    event.progress = 48;
    controller.receive(event);
    EXPECT_FALSE(controller.indeterminate());
    EXPECT_DOUBLE_EQ(controller.progress(), 0.48);
    event.progress = 150;
    controller.receive(event);
    EXPECT_DOUBLE_EQ(controller.progress(), 1.0);
}

TEST(SetupControllerTest, ProtectedExecutionKeepsTheMeasuredBundleProgress) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    SetupEvent event{SetupEventKind::Progress};
    event.progress = 48;
    event.can_cancel = true;
    controller.receive(event);
    controller.receive(SetupEvent{SetupEventKind::Applying});
    EXPECT_FALSE(controller.closeEnabled());
    EXPECT_FALSE(controller.indeterminate());
    EXPECT_DOUBLE_EQ(controller.progress(), 0.48);
    EXPECT_EQ(controller.description(), "This step can't be interrupted.");
}

TEST(SetupControllerTest, RestartIsDistinctAndNeverAutomaticallyReboots) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    SetupEvent event{SetupEventKind::Completed};
    event.restart_required = true;
    controller.receive(event);
    EXPECT_EQ(controller.page(), "restart");
    EXPECT_EQ(controller.headline(), "Restart to finish");
    EXPECT_EQ(controller.primaryText(), "Restart now");
    EXPECT_EQ(controller.secondaryText(), "Later");
    std::uint32_t result = 0;
    QObject::connect(&controller, &SetupController::finished, [&](std::uint32_t code) { result = code; });
    controller.requestClose();
    EXPECT_EQ(result, 3010U);
}

TEST(SetupControllerTest, FailureDoesNotClaimSuccessfulInstallationOrRollback) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    SetupEvent event{SetupEventKind::Error};
    event.text = L"Windows Installer stopped with error 1603.";
    controller.receive(event);
    event = SetupEvent{SetupEventKind::Completed};
    event.status = static_cast<std::int32_t>(0x80070643U);
    controller.receive(event);
    EXPECT_EQ(controller.page(), "failure");
    EXPECT_EQ(controller.statusTone(), "error");
    EXPECT_EQ(controller.detail(), "Windows Installer stopped with error 1603.");
}

TEST(SetupControllerTest, PassiveCompletionReturnsEngineResultWithoutUserInput) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap", true);
    controller.receive(Detected());
    EXPECT_FALSE(controller.decision());
    EXPECT_TRUE(controller.primaryText().isEmpty());
    std::uint32_t result = 0;
    QObject::connect(&controller, &SetupController::finished, [&](std::uint32_t code) { result = code; });
    SetupEvent event{SetupEventKind::Completed};
    event.status = 1603;
    controller.receive(event);
    EXPECT_EQ(result, 1603U);
}

TEST(SetupControllerTest, FailureWithPendingRestartKeepsFailureAndItsExitCode) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    SetupEvent event{SetupEventKind::Completed};
    event.status = 1603;
    event.restart_required = true;
    controller.receive(event);
    EXPECT_EQ(controller.page(), "failure");
    EXPECT_TRUE(controller.description().contains("Restart Windows"));
    std::uint32_t result = 0;
    QObject::connect(&controller, &SetupController::finished, [&](std::uint32_t code) { result = code; });
    controller.requestClose();
    EXPECT_EQ(result, 1603U);
}

TEST(SetupControllerTest, PassiveMaintenanceNamesTheRealAction) {
    SetupController uninstall("0.11.0", "C:/Program Files/ExoSnap", true, true);
    uninstall.receive(Detected(true));
    EXPECT_EQ(uninstall.headline(), "Uninstalling ExoSnap");
    SetupController repair("0.11.0", "C:/Program Files/ExoSnap", true, false, true);
    repair.receive(Detected(true));
    EXPECT_EQ(repair.headline(), "Repairing ExoSnap");
}

TEST(SetupControllerTest, NonWin32HresultsAreNotTruncated) {
    for (bool passive : {false, true}) {
        SetupController controller("0.11.0", "C:/Program Files/ExoSnap", passive);
        std::uint32_t result = 0;
        QObject::connect(&controller, &SetupController::finished, [&](std::uint32_t code) { result = code; });
        SetupEvent event{SetupEventKind::Completed};
        event.status = static_cast<std::int32_t>(0x80004005U);
        controller.receive(event);
        if (!passive)
            controller.requestClose();
        EXPECT_EQ(result, 0x80004005U);
    }
}

TEST(SetupControllerTest, FilesInUseFailureExplainsHowToRecoverWithoutStoppingTheApp) {
    for (std::int32_t status : {1603, static_cast<std::int32_t>(0x80070642U)}) {
        SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
        SetupEvent event{SetupEventKind::FilesInUse};
        controller.receive(event);
        event = SetupEvent{SetupEventKind::Completed};
        event.status = status;
        controller.receive(event);
        EXPECT_EQ(controller.page(), "failure");
        EXPECT_TRUE(controller.description().contains("Close ExoSnap"));
    }
}

TEST(SetupControllerTest, LaunchFailureDoesNotMisrepresentTheInstallation) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    controller.receive(SetupEvent{SetupEventKind::Completed});
    SetupEvent event{SetupEventKind::Error};
    event.text = L"The program could not be started.";
    controller.receive(event);
    EXPECT_EQ(controller.page(), "success");
    EXPECT_TRUE(controller.description().contains("couldn't be started"));
    EXPECT_EQ(controller.statusTone(), "success");
}

TEST(SetupControllerTest, EnteringProtectedPhaseDismissesPendingCancellation) {
    SetupController controller("0.11.0", "C:/Program Files/ExoSnap");
    SetupEvent event{SetupEventKind::Applying};
    event.can_cancel = true;
    controller.receive(event);
    controller.requestClose();
    EXPECT_TRUE(controller.cancelConfirmationVisible());
    int cancellations = 0;
    QObject::connect(&controller, &SetupController::cancelRequested, [&] { ++cancellations; });
    event = SetupEvent{SetupEventKind::PackageBegin};
    controller.receive(event);
    EXPECT_FALSE(controller.cancelConfirmationVisible());
    controller.confirmCancel();
    EXPECT_EQ(cancellations, 0);
}

// Deterministic layout, state-presentation and accessibility contracts for the
// updater window.
//
// These are the invariants the pre-0.9 visual review found broken and which a
// screenshot alone cannot defend: a PNG shows that a string ended at a widget
// edge, but nothing fails when it starts doing so again. They are asserted on
// geometry and on the accessibility tree rather than on pixels, because that is
// what the contracts are actually about.
//
// No network, no install, no live updater process: the production window is
// driven by the production controller and measured offscreen.

#include <gtest/gtest.h>

#include <QAccessible>
#include <QAccessibleInterface>
#include <QAccessibleValueInterface>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QLabel>
#include <QString>
#include <QWidget>

#include "ElidingLabel.h"
#include "ProgressRing.h"
#include "StepListWidget.h"
#include "UpdaterController.h"
#include "UpdaterWindow.h"

using namespace exosnap::updater;

namespace {

constexpr QChar kEllipsis(0x2026);

// The longest input the shipping updater can actually receive: version strings
// come from UpdaterArgs (--from/--to), i.e. whatever the signed release manifest
// names. A pre-release + build-metadata semver is the realistic maximum.
constexpr char kLongFrom[] = "0.9.0-rc4+build.20260810.a5d55f1.windows-x64";
constexpr char kLongTo[] = "0.9.0-rc5+build.20260812.eba270a.windows-x64";

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "updater_text_layout";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

void Settle(QWidget& widget) {
    widget.move(-20000, -20000);
    widget.show();
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        widget.ensurePolished();
        widget.adjustSize();
    }
}

UpdaterUiState IdleState(const char* from = "0.9.0-rc4", const char* to = "0.9.0-rc5") {
    return UpdaterController(QString::fromLatin1(from), QString::fromLatin1(to)).state();
}

UpdaterUiState DownloadingState(const char* from = "0.9.0-rc4", const char* to = "0.9.0-rc5") {
    UpdaterController controller(QString::fromLatin1(from), QString::fromLatin1(to));
    controller.onStepStarted(UpStep::Download);
    controller.onDownloadProgress(38, 100);
    return controller.state();
}

UpdaterUiState LongMsiDetailState() {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    controller.onStepDone(UpStep::Download);
    controller.onStepDone(UpStep::CloseApp);
    controller.onFailure(FailureCase::MsiFailed,
                         QStringLiteral("1603 (fatal error during installation, ERROR_INSTALL_FAILURE)"));
    return controller.state();
}

UpdaterUiState ReinstallDownloadingState() {
    UpdaterController controller(QStringLiteral("0.9.0-rc5"), QStringLiteral("0.9.0-rc5"));
    controller.setVerificationReinstall(true);
    controller.onStepStarted(UpStep::Download);
    controller.onDownloadProgress(38, 100);
    return controller.state();
}

UpdaterUiState SuccessState() {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    controller.onAllDone();
    return controller.state();
}

const ElidingLabel* Eliding(const UpdaterWindow& window, const char* name) {
    return window.findChild<ElidingLabel*>(QString::fromLatin1(name));
}

// Every visible string in the window, in tree order. Used to assert that a
// statement is made once rather than that a particular widget carries it.
QStringList VisibleText(const UpdaterWindow& window) {
    QStringList out;
    for (const QLabel* label : window.findChildren<const QLabel*>()) {
        const auto* eliding = qobject_cast<const ElidingLabel*>(label);
        const QString text = eliding != nullptr ? eliding->fullText() : label->text();
        if (!text.isEmpty())
            out << text;
    }
    return out;
}

class UpdaterTextLayoutTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ── Long content ────────────────────────────────────────────────────────────

TEST_F(UpdaterTextLayoutTest, LongVersionsElideInTheMiddleAndStayInsideTheWindow) {
    UpdaterWindow window;
    window.render(DownloadingState(kLongFrom, kLongTo));
    Settle(window);

    for (const char* name : {"updaterFromVersionPill", "updaterToVersionPill"}) {
        const ElidingLabel* pill = Eliding(window, name);
        ASSERT_NE(pill, nullptr) << name;
        EXPECT_TRUE(pill->isElided()) << name;
        // Deliberate shortening, not a cut: an ellipsis, and it sits in the
        // MIDDLE so the build suffix that identifies the version survives.
        EXPECT_TRUE(pill->text().contains(kEllipsis)) << name;
        // The whole reason this pill elides in the middle: the tail is what
        // identifies WHICH build the version stands for, so it has to survive.
        // ElideRight would have dropped exactly that half.
        EXPECT_TRUE(pill->text().endsWith(pill->fullText().right(8))) << name << " -> " << pill->text().toStdString();
        EXPECT_TRUE(pill->text().startsWith(pill->fullText().left(8))) << name << " -> " << pill->text().toStdString();
        EXPECT_FALSE(pill->text().endsWith(kEllipsis)) << name;
        // Nothing is lost by shortening.
        EXPECT_EQ(pill->toolTip(), pill->fullText()) << name;
        EXPECT_EQ(pill->accessibleName(), pill->fullText()) << name;
        // The painted string fits its own box, and the box fits the window.
        EXPECT_LE(pill->fontMetrics().size(Qt::TextSingleLine, pill->text()).width(), pill->textAreaWidth()) << name;
        const QRect placed(pill->mapTo(&window, QPoint(0, 0)), pill->size());
        EXPECT_TRUE(window.rect().contains(placed)) << name;
    }

    EXPECT_EQ(Eliding(window, "updaterFromVersionPill")->fullText(), QString::fromLatin1(kLongFrom));
    EXPECT_EQ(Eliding(window, "updaterToVersionPill")->fullText(), QString::fromLatin1(kLongTo));
}

TEST_F(UpdaterTextLayoutTest, ShortVersionsAreNotShortenedAtAll) {
    UpdaterWindow window;
    window.render(DownloadingState());
    Settle(window);

    for (const char* name : {"updaterFromVersionPill", "updaterToVersionPill"}) {
        const ElidingLabel* pill = Eliding(window, name);
        ASSERT_NE(pill, nullptr) << name;
        EXPECT_FALSE(pill->isElided()) << name;
        EXPECT_FALSE(pill->text().contains(kEllipsis)) << name;
        // No tooltip on a label that shows everything: a hover hint repeating
        // what is already on screen is noise.
        EXPECT_TRUE(pill->toolTip().isEmpty()) << name;
    }
    EXPECT_EQ(Eliding(window, "updaterFromVersionPill")->text(), QStringLiteral("0.9.0-rc4"));
    EXPECT_EQ(Eliding(window, "updaterToVersionPill")->text(), QStringLiteral("0.9.0-rc5"));
}

TEST_F(UpdaterTextLayoutTest, LongMsiDetailElidesInsteadOfEndingMidToken) {
    UpdaterWindow window;
    window.render(LongMsiDetailState());
    Settle(window);

    const ElidingLabel* detail = Eliding(window, "updaterResultDetail");
    ASSERT_NE(detail, nullptr);
    EXPECT_TRUE(detail->isElided());
    // Prose reads front to back, so this one elides at the right — but it ends
    // in an ellipsis rather than in "...ERROR_I".
    EXPECT_TRUE(detail->text().endsWith(kEllipsis));
    EXPECT_EQ(detail->toolTip(), detail->fullText());
    EXPECT_TRUE(detail->fullText().contains(QStringLiteral("ERROR_INSTALL_FAILURE")));
    EXPECT_LE(detail->fontMetrics().size(Qt::TextSingleLine, detail->text()).width(), detail->textAreaWidth());
}

TEST_F(UpdaterTextLayoutTest, LongContentNeverPushesTheActionRowOutOfView) {
    UpdaterWindow window;
    window.render(LongMsiDetailState());
    Settle(window);

    auto* actions = window.findChild<QWidget*>(QStringLiteral("updaterActionRow"));
    ASSERT_NE(actions, nullptr);
    const QRect placed(actions->mapTo(&window, QPoint(0, 0)), actions->size());
    EXPECT_TRUE(window.rect().contains(placed));
    EXPECT_EQ(window.size(), QSize(520, 680)) << "the fixed window must not grow to fit pathological text";
}

// ── Pre-flight vs. measured progress ────────────────────────────────────────

TEST_F(UpdaterTextLayoutTest, PreFlightShowsNoPercentageAndOneLabelledIndicator) {
    UpdaterWindow window;
    window.render(IdleState());
    Settle(window);

    auto* ring = window.findChild<ProgressRing*>();
    ASSERT_NE(ring, nullptr);
    // Nothing has been measured, so no number is claimed.
    EXPECT_TRUE(ring->isIndeterminate());
    EXPECT_EQ(ring->value(), 0.0);

    // The spinner beside the ring is no longer an orphan: it labels a status
    // line, which is what every other state already had.
    const ElidingLabel* status = Eliding(window, "updaterStatusText");
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(status->fullText(), QStringLiteral("Preparing update…"));
}

// The success screen states the one thing the user needs — the app is coming
// back on its own — exactly once, in the state panel where every other state
// puts its safety line. It used to appear a second time as the action hint,
// where the surrounding states describe what the BUTTON does.
TEST_F(UpdaterTextLayoutTest, SuccessSaysTheAppIsRelaunchingOnlyOnce) {
    UpdaterWindow window;
    window.render(SuccessState());
    Settle(window);

    const QString relaunch = QStringLiteral("ExoSnap is starting automatically.");
    EXPECT_EQ(VisibleText(window).count(relaunch), 1);

    const ElidingLabel* safety = Eliding(window, "updaterWorkingSafety");
    ASSERT_NE(safety, nullptr);
    EXPECT_EQ(safety->fullText(), relaunch);

    const auto* hint = window.findChild<const QLabel*>(QStringLiteral("updaterActionHint"));
    ASSERT_NE(hint, nullptr);
    EXPECT_TRUE(hint->text().isEmpty()) << hint->text().toStdString();
}

TEST_F(UpdaterTextLayoutTest, MeasuredDownloadProgressIsDeterminate) {
    UpdaterWindow window;
    window.render(DownloadingState());
    Settle(window);

    auto* ring = window.findChild<ProgressRing*>();
    ASSERT_NE(ring, nullptr);
    EXPECT_FALSE(ring->isIndeterminate());
    EXPECT_GT(ring->value(), 0.0);
    // No geometry jump between the two: the ring keeps its box either way.
    EXPECT_EQ(ring->size(), QSize(120, 120));
}

// ── What kind of run this is ────────────────────────────────────────────────

TEST_F(UpdaterTextLayoutTest, EyebrowNamesTheRunAndTheTitleBarNeverChanges) {
    struct Case {
        UpdaterUiState state;
        const char* eyebrow;
    };
    const std::array<Case, 3> cases = {{
        {DownloadingState(), "UPDATING EXOSNAP"},
        {ReinstallDownloadingState(), "REINSTALLING EXOSNAP"},
        {LongMsiDetailState(), "EXOSNAP WAS NOT UPDATED"},
    }};

    for (const Case& scenario : cases) {
        UpdaterWindow window;
        window.render(scenario.state);
        Settle(window);

        auto* eyebrow = window.findChild<QLabel*>(QStringLiteral("updaterEyebrow"));
        ASSERT_NE(eyebrow, nullptr);
        EXPECT_EQ(eyebrow->text(), QString::fromLatin1(scenario.eyebrow));

        // ADR 0055: the role label is stable in every state, including a
        // verification reinstall and a terminal failure.
        auto* role = window.findChild<QLabel*>(QStringLiteral("updaterTitle"));
        ASSERT_NE(role, nullptr);
        EXPECT_EQ(role->text(), QStringLiteral("Updater"));
    }
}

TEST_F(UpdaterTextLayoutTest, TerminalFailureMovesTheEmphasisToTheInstalledVersion) {
    const QString accent = QStringLiteral("#9bd9d2"); // theme::mint()

    UpdaterWindow working;
    working.render(DownloadingState());
    Settle(working);
    // While the update is still going, the target is where the run is heading.
    EXPECT_TRUE(Eliding(working, "updaterToVersionPill")->styleSheet().contains(accent, Qt::CaseInsensitive));
    EXPECT_EQ(Eliding(working, "updaterToVersionPill")->font().weight(), QFont::DemiBold);
    EXPECT_EQ(Eliding(working, "updaterFromVersionPill")->font().weight(), QFont::Medium);

    UpdaterWindow failed;
    failed.render(LongMsiDetailState());
    Settle(failed);
    // After a terminal failure the target version is NOT what is installed, so
    // it must not keep the accent — that read as "0.9.0-rc5 is on the machine".
    EXPECT_FALSE(Eliding(failed, "updaterToVersionPill")->styleSheet().contains(accent, Qt::CaseInsensitive));
    EXPECT_EQ(Eliding(failed, "updaterToVersionPill")->font().weight(), QFont::Medium);
    EXPECT_EQ(Eliding(failed, "updaterFromVersionPill")->font().weight(), QFont::DemiBold);
}

TEST_F(UpdaterTextLayoutTest, SoftSuccessKeepsTheTargetEmphasisBecauseTheUpdateDidApply) {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    controller.onStepDone(UpStep::Download);
    controller.onStepDone(UpStep::CloseApp);
    controller.onStepDone(UpStep::Install);
    controller.onStepDone(UpStep::Verify);
    controller.onFailure(FailureCase::LaunchFailed, QString());

    UpdaterWindow window;
    window.render(controller.state());
    Settle(window);

    EXPECT_EQ(window.findChild<QLabel*>(QStringLiteral("updaterEyebrow"))->text(), QStringLiteral("UPDATING EXOSNAP"));
    EXPECT_TRUE(
        Eliding(window, "updaterToVersionPill")->styleSheet().contains(QStringLiteral("#9bd9d2"), Qt::CaseInsensitive));
}

// ── Accessibility ───────────────────────────────────────────────────────────

TEST_F(UpdaterTextLayoutTest, ProgressRingExposesProgressSemantics) {
    UpdaterWindow window;
    window.render(DownloadingState());
    Settle(window);

    auto* ring = window.findChild<ProgressRing*>();
    ASSERT_NE(ring, nullptr);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(ring);
    ASSERT_NE(iface, nullptr);
    EXPECT_EQ(iface->role(), QAccessible::ProgressBar);
    EXPECT_EQ(iface->text(QAccessible::Name), QStringLiteral("Update progress"));

    auto* value = iface->valueInterface();
    ASSERT_NE(value, nullptr);
    EXPECT_DOUBLE_EQ(value->minimumValue().toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(value->maximumValue().toDouble(), 100.0);
    EXPECT_GT(value->currentValue().toDouble(), 0.0);
    EXPECT_TRUE(iface->text(QAccessible::Description).endsWith(QStringLiteral("percent")));
}

TEST_F(UpdaterTextLayoutTest, ProgressRingSaysSoWhenThereIsNothingToMeasure) {
    UpdaterWindow window;
    window.render(IdleState());
    Settle(window);

    auto* ring = window.findChild<ProgressRing*>();
    ASSERT_NE(ring, nullptr);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(ring);
    ASSERT_NE(iface, nullptr);
    // "0 percent" would be the same lie the visible ring used to tell.
    EXPECT_TRUE(iface->text(QAccessible::Description).startsWith(QStringLiteral("Preparing update")));
}

TEST_F(UpdaterTextLayoutTest, StepListExposesEveryPhaseAndItsStatus) {
    UpdaterController controller(QStringLiteral("0.9.0-rc4"), QStringLiteral("0.9.0-rc5"));
    controller.onStepDone(UpStep::Download);
    controller.onStepStarted(UpStep::CloseApp);

    UpdaterWindow window;
    window.render(controller.state());
    Settle(window);

    auto* steps = window.findChild<StepListWidget*>();
    ASSERT_NE(steps, nullptr);
    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(steps);
    ASSERT_NE(iface, nullptr);
    EXPECT_EQ(iface->role(), QAccessible::List);
    EXPECT_EQ(iface->text(QAccessible::Name), QStringLiteral("Update steps"));

    // The glyph that carries each row's status on screen is painted, so the row
    // itself has to say it.
    QStringList names;
    for (const QWidget* row : steps->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
        names << row->accessibleName();
    EXPECT_TRUE(names.contains(QStringLiteral("Downloading update, done")))
        << names.join(QStringLiteral(" | ")).toStdString();
    EXPECT_TRUE(names.contains(QStringLiteral("Closing previous version, working")))
        << names.join(QStringLiteral(" | ")).toStdString();
    EXPECT_TRUE(names.contains(QStringLiteral("Launching ExoSnap, queued")))
        << names.join(QStringLiteral(" | ")).toStdString();
}

} // namespace

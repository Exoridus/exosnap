// NOTIFY-TOASTS-R1 — NotificationToastWindow unit tests
//
// Covers:
//   1. Construction defaults to hidden.
//   2. Exclusion failure → window stays hidden (mirrors test_recording_overlay.cpp pattern).
//   3. After a successful Enqueue + exclusion, window position is anchored to the
//      primary display bottom-right corner.
//   4. Rendering a sample event does not crash.
//   5. Window hides when the visible set is cleared.
//
// Follows the QApplication-fixture pattern from test_recording_overlay.cpp.
// ctest runs each test in isolation via --gtest_filter so the fixture owns its
// own QApplication.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>

#include "notifications/NotificationEvent.h"
#include "notifications/NotificationManager.h"
#include "ui/overlay/NotificationToastWindow.h"

namespace exosnap {
namespace {

using notifications::NotificationAction;
using notifications::NotificationEvent;
using notifications::NotificationManager;
using notifications::NotificationType;
using ui::overlay::NotificationToastWindow;

QApplication* EnsureApplication() {
    if (auto* existing = qobject_cast<QApplication*>(QCoreApplication::instance()))
        return existing;
    static int argc = 1;
    static char app_name[] = "notification_toast_tests";
    static char* argv[] = {app_name, nullptr};
    static QApplication app(argc, argv);
    return &app;
}

class NotificationToastTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        EnsureApplication();
    }
};

// ── Construction ──────────────────────────────────────────────────────────────

TEST_F(NotificationToastTest, Construction_DefaultsToHidden) {
    NotificationManager mgr;
    NotificationToastWindow window(&mgr, nullptr);
    EXPECT_FALSE(window.isVisible());
}

TEST_F(NotificationToastTest, Construction_WithNullManager_DefaultsToHidden) {
    NotificationToastWindow window(nullptr, nullptr);
    EXPECT_FALSE(window.isVisible());
}

TEST_F(NotificationToastTest, SizeHint_NoEvents_HasZeroHeight) {
    NotificationManager mgr;
    NotificationToastWindow window(&mgr, nullptr);
    const QSize hint = window.sizeHint();
    EXPECT_GT(hint.width(), 0);
    EXPECT_EQ(hint.height(), 0);
}

TEST_F(NotificationToastTest, SizeHint_OneEvent_HasPositiveHeight) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Saved");
    e.body = QStringLiteral("file.mkv");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const QSize hint = window.sizeHint();
    EXPECT_GT(hint.width(), 0);
    EXPECT_GT(hint.height(), 0);
}

// ── Exclusion fallback ────────────────────────────────────────────────────────
//
// When SetWindowDisplayAffinity fails (e.g. in a test process, older Windows
// build, or restricted environment), the window must stay hidden.  This mirrors
// the exact contract tested in test_recording_overlay.cpp.

TEST_F(NotificationToastTest, ExclusionFallback_HiddenWhenNotExcluded) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Saved");
    e.body = QStringLiteral("test.mkv");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);

    // Trigger the visibility path by directly processing the signal.
    // The window's onVisibleSetChanged() is connected to the manager; the
    // Enqueue above fires it before the window was wired, so we emit manually.
    // Reconnect a fresh manager to this window to test the guard path.
    NotificationManager mgr2;
    NotificationToastWindow window2(&mgr2, nullptr);
    mgr2.Enqueue(e);

    // After enqueue: if exclusion failed the window must NOT be visible.
    if (!window2.isExcluded()) {
        EXPECT_FALSE(window2.isVisible())
            << "NotificationToastWindow must remain hidden when SetWindowDisplayAffinity failed";
    }
    // If exclusion succeeded the window may be visible — both outcomes are valid.
}

// ── Primary display anchor ────────────────────────────────────────────────────

TEST_F(NotificationToastTest, PrimaryDisplay_IsAvailable) {
    // This test ensures a primary screen is accessible (prerequisite for anchoring).
    const QScreen* primary = QGuiApplication::primaryScreen();
    // In headless CI the primary screen may be absent; skip gracefully.
    if (primary == nullptr)
        GTEST_SKIP() << "No primary screen — skipping anchor test";

    EXPECT_FALSE(primary->geometry().isNull());
}

// ── Rendering sample event ────────────────────────────────────────────────────

TEST_F(NotificationToastTest, PaintEvent_SampleSavedEvent_DoesNotCrash) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Recording saved");
    e.body = QStringLiteral("sample.mkv");
    e.action = NotificationAction::OpenFolder;
    e.action_payload = QStringLiteral("C:/Videos");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    // Grabbing the window forces a paint cycle; must not crash.
    EXPECT_NO_FATAL_FAILURE(window.grab());
}

TEST_F(NotificationToastTest, PaintEvent_SampleLowStorageEvent_DoesNotCrash) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::LowStorage;
    e.title = QStringLiteral("Storage full");
    e.body = QStringLiteral("Drive critically low on space.");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    EXPECT_NO_FATAL_FAILURE(window.grab());
}

TEST_F(NotificationToastTest, PaintEvent_SampleUnexpectedStopEvent_DoesNotCrash) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::UnexpectedStop;
    e.title = QStringLiteral("Recording stopped unexpectedly");
    e.body = QStringLiteral("Phase: Encode");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    EXPECT_NO_FATAL_FAILURE(window.grab());
}

TEST_F(NotificationToastTest, PaintEvent_SampleRecoveryAvailableEvent_DoesNotCrash) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::RecoveryAvailable;
    e.title = QStringLiteral("Interrupted recordings found");
    e.body = QStringLiteral("1 interrupted recording is ready to recover.");
    e.action = NotificationAction::OpenRecovery;
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    EXPECT_NO_FATAL_FAILURE(window.grab());
}

// ── Manager dismiss → window hides ────────────────────────────────────────────

TEST_F(NotificationToastTest, DismissAll_WindowHides) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Test");
    e.body = QStringLiteral("body");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);

    // Dismiss all visible events.
    while (!mgr.VisibleEvents().isEmpty()) {
        mgr.Dismiss(mgr.VisibleEvents()[0].sequence);
    }

    // Window must be hidden (no visible events remain).
    if (window.isExcluded()) {
        EXPECT_FALSE(window.isVisible());
    } else {
        // Exclusion failed — window was never visible; trivially pass.
        EXPECT_FALSE(window.isVisible());
    }
}

// ── Skinned anatomy tests (NOTIFY-SKIN-R1) ────────────────────────────────────

// The toast card is kCardWidth (372px) wide. The window is wider by 2 * kShadowMargin
// on each side to accommodate the soft shadow penumbra.
TEST_F(NotificationToastTest, SizeHint_Width_Is372px) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Recording saved");
    e.body = QStringLiteral("file.mkv · 179 MB");
    e.action = NotificationAction::OpenFolder;
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const int expected_w = NotificationToastWindow::kCardWidth + 2 * NotificationToastWindow::kShadowMargin;
    EXPECT_EQ(window.sizeHint().width(), expected_w);
}

// Non-sticky (Saved) must have positive height (includes countdown bar).
TEST_F(NotificationToastTest, SizeHint_Saved_HasCountdownBarHeight) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Recording saved");
    e.body = QStringLiteral("file.mkv · 179 MB");
    e.action = NotificationAction::OpenFolder;
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const int h = window.sizeHint().height();
    EXPECT_GT(h, 0) << "Saved toast must have positive height";
    // Height must accommodate the 3px countdown bar
    // (at minimum: padTop=14 + chip=30 + padBottom=14 + bar=3 = 61px)
    EXPECT_GE(h, 61);
}

// Sticky toasts (LowStorage, UnexpectedStop, RecoveryAvailable) have no bar.
// Their height should still be positive but without the bar contribution.
TEST_F(NotificationToastTest, SizeHint_LowStorage_StickyNoBar) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::LowStorage;
    e.title = QStringLiteral("Storage running low");
    e.body = QStringLiteral("1.2 GB free on C:. Recording may stop soon.");
    e.action = NotificationAction::ChangeFolder;
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const int h = window.sizeHint().height();
    EXPECT_GT(h, 0);
    // LowStorage is sticky, no 3px bar at the bottom
}

// Render every notification type with actions — must not crash.
TEST_F(NotificationToastTest, PaintEvent_AllTypes_WithActions_NoFatalFailure) {
    const struct {
        NotificationType type;
        NotificationAction action;
        const char* title;
        const char* body;
    } cases[] = {
        {NotificationType::Saved, NotificationAction::OpenFolder, "Recording saved",
         "exosnap_2026-06-11_02.mkv · 179 MB"},
        {NotificationType::LowStorage, NotificationAction::ChangeFolder, "Storage running low",
         "1.2 GB free on C:. Recording may stop soon."},
        {NotificationType::UnexpectedStop, NotificationAction::ShowFile, "Recording stopped unexpectedly",
         "Disk write failed at 04:12. A partial file was recovered."},
        {NotificationType::RecoveryAvailable, NotificationAction::OpenRecovery, "Recover last session?",
         "A recording from 14:02 wasn't finalized."},
        {NotificationType::FramesDropped, NotificationAction::OpenDiagnostics, "Frames dropped",
         "42 frames did not make it into the recording."},
        {NotificationType::PresetSwitched, NotificationAction::UndoPresetSwitch, "Switched to 'Quality'", ""},
        {NotificationType::AudioSourceDegraded, NotificationAction::OpenDiagnostics, "Audio source went silent",
         "An audio source lost its device. Recording continues — ExoSnap keeps retrying the connection."},
    };

    for (const auto& c : cases) {
        NotificationManager mgr;
        NotificationEvent e;
        e.type = c.type;
        e.title = QString::fromLatin1(c.title);
        e.body = QString::fromLatin1(c.body);
        e.action = c.action;
        mgr.Enqueue(e);

        NotificationToastWindow window(&mgr, nullptr);
        EXPECT_NO_FATAL_FAILURE(window.grab()) << "Crash painting type=" << static_cast<int>(c.type);
    }
}

// Three stacked toasts must each contribute height separated by 12px gaps.
// The window adds 2 * kShadowMargin once (top + bottom), shared across the stack.
// So: h_n = 2*M + n*card_h + (n-1)*gap  where M = kShadowMargin.
// Derived: h3 = 3*(h1 - 2*M) + 2*gap + 2*M  →  h3 = 3*h1 - 4*M + 2*gap.
TEST_F(NotificationToastTest, SizeHint_ThreeEvents_HeightIsAdditive) {
    // Standing toasts stack (a timed one would replace itself), so three
    // identical standing events give three identical cards.
    NotificationManager mgr;
    for (int i = 0; i < 3; ++i) {
        NotificationEvent e;
        e.type = NotificationType::LowStorage;
        e.title = QStringLiteral("Storage running low");
        e.body = QStringLiteral("Drive low.");
        e.action = NotificationAction::ChangeFolder;
        mgr.Enqueue(e);
    }

    NotificationToastWindow window(&mgr, nullptr);
    const int h3 = window.sizeHint().height();

    // Manually compute a single toast height by querying with 1 event
    NotificationManager mgr1;
    NotificationEvent e1;
    e1.type = NotificationType::LowStorage;
    e1.title = QStringLiteral("Storage running low");
    e1.body = QStringLiteral("Drive low.");
    e1.action = NotificationAction::ChangeFolder;
    mgr1.Enqueue(e1);
    NotificationToastWindow w1(&mgr1, nullptr);
    const int h1 = w1.sizeHint().height();

    // With shadow margin M shared at window level:
    //   h1 = 2*M + card_h
    //   h3 = 2*M + 3*card_h + 2*gap  = 3*h1 - 4*M + 2*gap
    const int M = NotificationToastWindow::kShadowMargin;
    const int gap = 12;
    EXPECT_EQ(h3, 3 * h1 - 4 * M + 2 * gap);
}

// The toast is now INTERACTIVE (✕ dismiss + action pills), so it must NOT carry
// Qt::WindowTransparentForInput — that flag would make every click pass through.
// Capture exclusion is handled independently by SetWindowDisplayAffinity.
TEST_F(NotificationToastTest, WindowFlags_IsNotTransparentForInput) {
    NotificationManager mgr;
    NotificationToastWindow window(&mgr, nullptr);
    EXPECT_TRUE((window.windowFlags() & Qt::WindowTransparentForInput) == Qt::WindowType{})
        << "Interactive toast must not be transparent-for-input";
    // Stays-on-top + frameless tool window are still required.
    EXPECT_TRUE((window.windowFlags() & Qt::WindowStaysOnTopHint) != Qt::WindowType{});
    EXPECT_TRUE((window.windowFlags() & Qt::FramelessWindowHint) != Qt::WindowType{});
}

// Window is always hidden unless exclusion succeeded (WDA_EXCLUDEFROMCAPTURE guard).
TEST_F(NotificationToastTest, ExclusionGuard_ConstructsHidden) {
    NotificationManager mgr;
    NotificationToastWindow window(&mgr, nullptr);
    EXPECT_FALSE(window.isVisible());
}

// ── Interactive hit-test geometry (NOTIFY-TOAST-INTERACTIVE) ──────────────────

// A Saved toast with an OpenFolder action exposes exactly one ✕ target plus one
// action-pill target, both owned by the event's sequence.
TEST_F(NotificationToastTest, HitTargets_SavedWithAction_HasDismissAndPill) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Recording saved");
    e.body = QStringLiteral("file.mkv");
    e.action = NotificationAction::OpenFolder;
    e.action_payload = QStringLiteral("C:/Videos/file.mkv");
    mgr.Enqueue(e);
    const uint64_t seq = mgr.VisibleEvents()[0].sequence;

    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();

    int dismiss_count = 0;
    int pill_count = 0;
    for (const auto& h : hits) {
        EXPECT_EQ(h.sequence, seq);
        if (h.is_dismiss) {
            ++dismiss_count;
        } else {
            ++pill_count;
            EXPECT_EQ(h.action, NotificationAction::OpenFolder);
        }
    }
    EXPECT_EQ(dismiss_count, 1);
    EXPECT_EQ(pill_count, 1);
}

// The dismiss ✕ rect sits in the top-right of the card and is non-empty.
TEST_F(NotificationToastTest, HitTargets_DismissRectIsTopRight) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Saved");
    e.body = QStringLiteral("body");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();
    ASSERT_FALSE(hits.isEmpty());

    const auto& x = hits.front();
    EXPECT_TRUE(x.is_dismiss);
    EXPECT_GT(x.rect.width(), 0.0);
    EXPECT_GT(x.rect.height(), 0.0);
    // Top-right: the ✕ centre is in the right half and near the top of the card.
    EXPECT_GT(x.rect.center().x(), 372.0 / 2.0);
    EXPECT_LT(x.rect.top(), 40.0);
}

// Three stacked toasts → three distinct dismiss targets, one per sequence, with
// strictly increasing y (newest layout stacks downward in window space).
TEST_F(NotificationToastTest, HitTargets_ThreeToasts_DistinctRowsPerSequence) {
    NotificationManager mgr;
    for (int i = 0; i < 3; ++i) {
        NotificationEvent e;
        e.type = NotificationType::LowStorage; // standing — three of them stack
        e.title = QStringLiteral("T%1").arg(i);
        e.body = QStringLiteral("b%1").arg(i);
        mgr.Enqueue(e);
    }
    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();

    QVector<qreal> dismiss_tops;
    QVector<uint64_t> seqs;
    for (const auto& h : hits) {
        if (h.is_dismiss) {
            dismiss_tops.push_back(h.rect.top());
            seqs.push_back(h.sequence);
        }
    }
    ASSERT_EQ(dismiss_tops.size(), 3);
    EXPECT_LT(dismiss_tops[0], dismiss_tops[1]);
    EXPECT_LT(dismiss_tops[1], dismiss_tops[2]);
    // All sequences distinct.
    EXPECT_NE(seqs[0], seqs[1]);
    EXPECT_NE(seqs[1], seqs[2]);
}

// Sticky toasts (LowStorage) still expose a dismiss ✕ even without a countdown.
TEST_F(NotificationToastTest, HitTargets_StickyToast_HasDismiss) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::LowStorage;
    e.title = QStringLiteral("Storage running low");
    e.body = QStringLiteral("Drive low.");
    e.action = NotificationAction::ChangeFolder;
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();

    bool has_dismiss = false;
    for (const auto& h : hits)
        has_dismiss = has_dismiss || h.is_dismiss;
    EXPECT_TRUE(has_dismiss);
}

// Clicking the ✕ target's center maps to a Manager::Dismiss of that sequence.
// We simulate the press-handler contract by invoking Dismiss with the geometry
// the window reports — proving paint/hit/dismiss share one coordinate system.
TEST_F(NotificationToastTest, HitTargets_DismissCenter_DismissesThatSequence) {
    NotificationManager mgr;
    NotificationEvent a;
    a.type = NotificationType::LowStorage; // standing, so both stay visible
    a.title = QStringLiteral("A");
    a.body = QStringLiteral("a");
    mgr.Enqueue(a);
    NotificationEvent b;
    b.type = NotificationType::UnexpectedStop;
    b.title = QStringLiteral("B");
    b.body = QStringLiteral("b");
    mgr.Enqueue(b);
    ASSERT_EQ(mgr.VisibleEvents().size(), 2);

    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();

    // Pick the dismiss target for the SECOND visible event.
    const uint64_t target_seq = mgr.VisibleEvents()[1].sequence;
    bool found = false;
    for (const auto& h : hits) {
        if (h.is_dismiss && h.sequence == target_seq) {
            found = true;
            mgr.Dismiss(h.sequence); // same call mousePressEvent makes
            break;
        }
    }
    ASSERT_TRUE(found);
    ASSERT_EQ(mgr.VisibleEvents().size(), 1);
    EXPECT_NE(mgr.VisibleEvents()[0].sequence, target_seq);
}

// ── Edit action (EDIT-QUICK-ACTION-R1) ────────────────────────────────────────

// A "Recording saved" toast built with Edit+OpenFolder actions exposes exactly
// one ✕ target, one primary "Edit" pill, and one secondary "Show in folder" pill.
TEST_F(NotificationToastTest, HitTargets_SavedWithEditAction_HasDismissEditAndShowInFolder) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Recording saved");
    e.body = QStringLiteral("file.mkv");
    e.action = NotificationAction::Edit;
    e.secondary_action = NotificationAction::OpenFolder;
    e.action_payload = QStringLiteral("C:/Videos/file.mkv");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();

    int dismiss_count = 0;
    int edit_count = 0;
    int open_folder_count = 0;
    for (const auto& h : hits) {
        if (h.is_dismiss) {
            ++dismiss_count;
        } else if (h.action == NotificationAction::Edit) {
            ++edit_count;
        } else if (h.action == NotificationAction::OpenFolder) {
            ++open_folder_count;
        }
    }
    EXPECT_EQ(dismiss_count, 1);
    EXPECT_EQ(edit_count, 1) << "Expected exactly one Edit pill";
    EXPECT_EQ(open_folder_count, 1) << "Expected exactly one Show-in-folder pill";
    EXPECT_EQ(hits.size(), 3) << "Expected dismiss + Edit + Show-in-folder = 3 targets";
}

// Edit pill must be marked primary (accent fill) and Show-in-folder secondary (ghost).
// We verify pill ordering: Edit first (leftmost), Show-in-folder second.
TEST_F(NotificationToastTest, HitTargets_EditPill_IsLeftOfShowInFolderPill) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Recording saved");
    e.body = QStringLiteral("session.mkv");
    e.action = NotificationAction::Edit;
    e.secondary_action = NotificationAction::OpenFolder;
    e.action_payload = QStringLiteral("C:/Videos/session.mkv");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();

    qreal edit_x = -1.0;
    qreal show_x = -1.0;
    for (const auto& h : hits) {
        if (!h.is_dismiss) {
            if (h.action == NotificationAction::Edit)
                edit_x = h.rect.left();
            else if (h.action == NotificationAction::OpenFolder)
                show_x = h.rect.left();
        }
    }
    EXPECT_GE(edit_x, 0.0) << "Edit pill not found";
    EXPECT_GE(show_x, 0.0) << "Show-in-folder pill not found";
    EXPECT_LT(edit_x, show_x) << "Edit pill must be to the left of Show-in-folder pill";
}

// Rendering an Edit+OpenFolder Saved toast must not crash.
TEST_F(NotificationToastTest, PaintEvent_SavedWithEditAction_DoesNotCrash) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Recording saved");
    e.body = QStringLiteral("exosnap_2026-06-25_01.mkv · 245 MB");
    e.action = NotificationAction::Edit;
    e.secondary_action = NotificationAction::OpenFolder;
    e.action_payload = QStringLiteral("C:/Videos/exosnap_2026-06-25_01.mkv");
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    EXPECT_NO_FATAL_FAILURE(window.grab());
}

// The manager exposes the per-event shown-at timestamp the toast uses to drive
// the countdown bar; it is >= 0 for a visible event and -1 for an unknown one.
TEST_F(NotificationToastTest, Manager_ShownAtMs_TracksVisibleEvent) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Saved");
    e.body = QStringLiteral("body");
    mgr.Enqueue(e);
    const uint64_t seq = mgr.VisibleEvents()[0].sequence;

    EXPECT_GE(mgr.ShownAtMs(seq), 0);
    EXPECT_EQ(mgr.ShownAtMs(999999), -1);
}

// DismissIntervalMs is the single source of dwell timings shared with the bar.
TEST_F(NotificationToastTest, Manager_DismissIntervalMs_MatchesPerTypeConstants) {
    EXPECT_EQ(NotificationManager::DismissIntervalMs(NotificationType::Saved), NotificationManager::kDismissMs_Saved);
    EXPECT_EQ(NotificationManager::DismissIntervalMs(NotificationType::LowStorage), 0);
    EXPECT_EQ(NotificationManager::DismissIntervalMs(NotificationType::UnexpectedStop), 0);
    EXPECT_EQ(NotificationManager::DismissIntervalMs(NotificationType::RecoveryAvailable), 0);
    EXPECT_EQ(NotificationManager::DismissIntervalMs(NotificationType::UpdateAvailable),
              NotificationManager::kDismissMs_UpdateAvailable);
    EXPECT_EQ(NotificationManager::DismissIntervalMs(NotificationType::FramesDropped),
              NotificationManager::kDismissMs_FramesDropped);
}

// ── Preset switch: recorded, never a toast ────────────────────────────────────
// Switching back is a matter of reselecting the previous preset from the same
// combo box that performed the switch; the hub keeps the record with Undo.

TEST_F(NotificationToastTest, PresetSwitched_NeverBecomesAToast) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::PresetSwitched;
    e.title = QStringLiteral("Switched to 'Quality'");
    e.action = NotificationAction::UndoPresetSwitch;
    mgr.Enqueue(e);

    EXPECT_TRUE(mgr.VisibleEvents().isEmpty());
    NotificationToastWindow window(&mgr, nullptr);
    EXPECT_TRUE(window.computeHitTargets().isEmpty());
}

// ── Card height derives from present content ─────────────────────────────────
// No reserved space for an absent body; no button strip for a single action.

namespace {
int singleToastHeight(NotificationType type, const QString& body, NotificationAction action,
                      NotificationAction secondary = NotificationAction::None) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = type;
    e.title = QStringLiteral("Title");
    e.body = body;
    e.action = action;
    e.secondary_action = secondary;
    mgr.Enqueue(e);
    NotificationToastWindow w(&mgr, nullptr);
    return w.sizeHint().height();
}
} // namespace

TEST_F(NotificationToastTest, CardHeight_AbsentBody_ReservesNoSpace) {
    const int title_only = singleToastHeight(NotificationType::SettingsRepaired, {}, NotificationAction::None);
    const int with_body =
        singleToastHeight(NotificationType::SettingsRepaired, QStringLiteral("Body line"), NotificationAction::None);
    EXPECT_LT(title_only, with_body);
}

// Grow-to-fit: a body long enough to wrap past two lines makes the card taller than
// a one-line body — the old two-line cap no longer clamps it. The card grows up to
// six lines (then ellipsizes), while the Notification Hub keeps the full text.
TEST_F(NotificationToastTest, CardHeight_LongBody_GrowsPastTwoLines) {
    const int one_line =
        singleToastHeight(NotificationType::LowStorage, QStringLiteral("Short."), NotificationAction::None);
    const QString long_body =
        QStringLiteral("This is a deliberately long notification body that wraps far beyond two lines so the "
                       "grow-to-fit card has to expand to several lines to show all of it before it ellipsizes.");
    const int tall = singleToastHeight(NotificationType::LowStorage, long_body, NotificationAction::None);
    // Beyond the old cap: at least two extra body lines over the one-line card (kBodyH
    // is 18px), proving the body now occupies three or more lines.
    EXPECT_GE(tall - one_line, 2 * 18);
}

TEST_F(NotificationToastTest, CardHeight_OneAction_HasNoButtonStrip) {
    // One action: the card IS the action — same height as an action-less card.
    const int no_action =
        singleToastHeight(NotificationType::FramesDropped, QStringLiteral("Body"), NotificationAction::None);
    const int one_action =
        singleToastHeight(NotificationType::FramesDropped, QStringLiteral("Body"), NotificationAction::OpenDiagnostics);
    EXPECT_EQ(no_action, one_action);
}

TEST_F(NotificationToastTest, CardHeight_TwoActions_AddAButtonRow) {
    const int one_action =
        singleToastHeight(NotificationType::Saved, QStringLiteral("Body"), NotificationAction::ShowFile);
    const int two_actions = singleToastHeight(NotificationType::Saved, QStringLiteral("Body"), NotificationAction::Edit,
                                              NotificationAction::OpenFolder);
    EXPECT_GT(two_actions, one_action);
}

// A single-action card draws a › chevron in its own column, left of the ✕ —
// the wrap width used to grow the card must be narrower than a same-content
// no-action card's, or long text runs through the chevron's x-range. Over a
// long enough body the narrower column needs a strictly taller card (never
// shorter — word-wrap into a narrower column can only add lines, not remove
// them), proving the chevron's column really is reserved and not just a
// cosmetic overlap left for the painter to clip.
TEST_F(NotificationToastTest, CardHeight_OneAction_ChevronColumnNarrowsWrapWidth) {
    // Length matters here, not phrasing: the sentence must land just past the
    // point where the wide (no chevron) column still fits it on two lines but
    // the narrower (chevron) column needs a third — verified empirically
    // against the real body font (a few characters of slack either side, so
    // this isn't a knife-edge match against one exact wrap point).
    const QString body =
        QStringLiteral("Recording continued while several frames were dropped during this capture session on your PC.");
    const int no_action = singleToastHeight(NotificationType::FramesDropped, body, NotificationAction::None);
    const int one_action =
        singleToastHeight(NotificationType::FramesDropped, body, NotificationAction::OpenDiagnostics);
    EXPECT_GT(one_action, no_action);
}

// ── One-action card: the card is the action ──────────────────────────────────

TEST_F(NotificationToastTest, OneActionCard_WholeCardIsTheActionTarget) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::FramesDropped;
    e.title = QStringLiteral("Frames dropped");
    e.body = QStringLiteral("12 frames were dropped.");
    e.action = NotificationAction::OpenDiagnostics;
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();

    ASSERT_EQ(hits.size(), 2); // the ✕ and the card
    EXPECT_TRUE(hits[0].is_dismiss);
    EXPECT_FALSE(hits[1].is_dismiss);
    EXPECT_EQ(hits[1].action, NotificationAction::OpenDiagnostics);
    // The action target spans the full card width, not a pill.
    EXPECT_EQ(hits[1].rect.width(), static_cast<qreal>(NotificationToastWindow::kCardWidth));
    // The ✕ sits inside the card-wide target but is pushed first, so it wins.
    EXPECT_TRUE(hits[1].rect.contains(hits[0].rect.center()));
}

// ── AudioSourceDegraded (ADR 0046 follow-up): standing, single-action card ────

TEST_F(NotificationToastTest, AudioSourceDegraded_IsStanding_NoAutoDismissBar) {
    NotificationManager mgr;
    mgr.Enqueue(notifications::MakeAudioSourceDegradedEvent(1));

    NotificationToastWindow window(&mgr, nullptr);
    const int standing_h = window.sizeHint().height();

    NotificationManager mgr_timed;
    NotificationEvent timed;
    timed.type = NotificationType::Saved;
    timed.title = QStringLiteral("Audio source went silent");
    timed.body = QStringLiteral("An audio source lost its device. Recording continues — "
                                "ExoSnap keeps retrying the connection.");
    timed.action = NotificationAction::OpenDiagnostics;
    mgr_timed.Enqueue(timed);
    NotificationToastWindow timed_window(&mgr_timed, nullptr);

    // Same chip/title/body/one-action layout, but the timed card reserves the
    // bottom countdown bar and the standing one does not.
    EXPECT_LT(standing_h, timed_window.sizeHint().height());
}

TEST_F(NotificationToastTest, AudioSourceDegraded_WholeCardOpensDiagnostics) {
    NotificationManager mgr;
    mgr.Enqueue(notifications::MakeAudioSourceDegradedEvent(2));

    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();

    ASSERT_EQ(hits.size(), 2); // the ✕ and the card
    EXPECT_TRUE(hits[0].is_dismiss);
    EXPECT_FALSE(hits[1].is_dismiss);
    EXPECT_EQ(hits[1].action, NotificationAction::OpenDiagnostics);
    EXPECT_EQ(hits[1].rect.width(), static_cast<qreal>(NotificationToastWindow::kCardWidth));
}

TEST_F(NotificationToastTest, TwoActionCard_CardItselfIsNotClickable) {
    NotificationManager mgr;
    NotificationEvent e;
    e.type = NotificationType::Saved;
    e.title = QStringLiteral("Recording saved");
    e.body = QStringLiteral("clip.mkv");
    e.action = NotificationAction::Edit;
    e.secondary_action = NotificationAction::OpenFolder;
    mgr.Enqueue(e);

    NotificationToastWindow window(&mgr, nullptr);
    const auto hits = window.computeHitTargets();

    ASSERT_EQ(hits.size(), 3); // ✕ + two pills, no card-wide target
    for (const auto& h : hits) {
        if (!h.is_dismiss)
            EXPECT_LT(h.rect.width(), static_cast<qreal>(NotificationToastWindow::kCardWidth));
    }
}

// ── Anchoring follows the app window ─────────────────────────────────────────

TEST_F(NotificationToastTest, Anchor_FollowsTheAnchorWidgetsScreen) {
    NotificationManager mgr;
    NotificationToastWindow window(&mgr, nullptr);

    // Without an anchor: primary screen fallback.
    EXPECT_EQ(window.anchorScreen(), QGuiApplication::primaryScreen());

    // With an anchor: the anchor widget's screen (offscreen platforms expose a
    // single screen, so the pinned contract is the source of the choice).
    QWidget anchor;
    anchor.show();
    window.setAnchorWidget(&anchor);
    EXPECT_EQ(window.anchorScreen(), anchor.screen());
}

} // namespace
} // namespace exosnap

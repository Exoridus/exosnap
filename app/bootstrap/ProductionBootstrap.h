#pragma once

// ProductionBootstrap.h -- the process-level startup/shutdown contract every
// ExoSnap frontend has to run, independent of which UI toolkit draws the window.
//
// The Qt Quick migration gave the product a second executable entry point. The
// duties collected here are not UI concerns -- DLL-search hardening, the updater
// self-heal, crash capture, the single-instance guard and the startup clock are
// properties of "an ExoSnap process", not of a particular window class. Left
// duplicated per entry point they drift silently, and a frontend that is merely
// *missing* one of them looks perfectly healthy right up to the launch where it
// matters: a stranded install tree after an interrupted swap, a second instance
// fighting over the global hotkeys, a crash that never reaches the next-launch
// dialog because no session sidecar was ever written.
//
// Framework boundary: QtCore/QtGui only. No QApplication, no QWidget, no window
// or page types. The frontend owns its window; this owns the process. Anything
// that needs a window handle takes it as an opaque native handle.
//
// Deliberately NOT wrapped here: exosnap::services::ParseRelaunchArgs and
// exosnap::services::HasVerifyUpdateReinstallRequest. Both are already pure,
// UI-agnostic argv parsers; a forwarding wrapper would only add a second name
// for the same thing. Both frontends call them directly. What *is* here is the
// part that cannot live in a pure parser: executing the relaunch, which has to
// hand the single-instance mutex over to the elevated successor.

#include <QIcon>
#include <QString>
#include <QStringList>

#include <string>

namespace exosnap::bootstrap {

// Exact top-level window title both frontends give their main window
// (MainWindow::setWindowTitle, Main.qml `title:`). Three independent places key
// off this literal: the single-instance activation below, the updater's
// close/handoff window lookup (apps/updater), and swap_engine's
// FindTopLevelWindowForProcess. A frontend that renames its window silently
// turns "activate the running instance" into "exit quietly and look broken",
// and turns the updater's handoff into a timeout -- hence one shared constant
// rather than a literal per call site.
inline constexpr const wchar_t* kAppWindowTitle = L"ExoSnap";

// QCoreApplication::applicationName. The settings/log/crash directories are
// derived from it, so the two frontends must agree or they read different
// configuration on the same machine.
inline constexpr const char* kApplicationName = "ExoSnap";

// ---------------------------------------------------------------------------
// Phase 1 -- before the Q*Application object exists.
// ---------------------------------------------------------------------------

struct PreAppResult {
    // StartupClock reading taken at the true start of main(). Logged later (the
    // log sink needs QCoreApplication), but captured here so the number stays
    // start->milestone latency rather than latency-to-whenever-logging-worked.
    qint64 main_start_ms = 0;
};

// Hardens the loader search path, self-heals an interrupted update swap and
// starts the process-global startup clock. Must be the first statement of
// main(): the DLL hardening has to precede any LoadLibrary the Qt platform
// plugin triggers, and the swap repair has to precede anything that resolves a
// path inside the install directory.
[[nodiscard]] PreAppResult RunPreApplicationPhase();

// ---------------------------------------------------------------------------
// Phase 2 -- immediately after the Q*Application constructor returns.
// ---------------------------------------------------------------------------

struct PostAppResult {
    qint64 qapplication_created_ms = 0;
};

// Reads the startup clock at the moment the application object became usable.
// Kept separate from InitializeLogging() on purpose: anything a frontend does
// between the constructor and bringing the log sink up (harness config
// isolation, graphics-API selection) would otherwise be folded into the
// reported construction cost.
[[nodiscard]] PostAppResult MarkApplicationConstructed();

// Sets the shared application identity. Call once, before anything resolves a
// configuration or log path.
void ApplyApplicationMetadata();

// Points EXOSNAP_CONFIG_DIR at a scratch directory named after `harness_id`
// (e.g. "visual-test" -> <temp>/exosnap-visual-test), unless the variable is
// already set. A harness runs the *real* application, and the real application
// persists its live configuration while it runs; aimed at the developer's own
// config directory it overwrites their settings with the scenario's synthetic
// ones. Isolation is therefore unconditional for a harness run rather than an
// opt-in environment variable -- forgetting the variable is exactly the mistake
// that destroys data. Returns false when an explicit EXOSNAP_CONFIG_DIR was
// already in force and nothing was changed.
bool IsolateHarnessConfigDir(const QString& harness_id);

// Moves the QML bytecode cache under EXOSNAP_CONFIG_DIR whenever that variable
// is in force, so an "isolated" run really is isolated.
//
// EXOSNAP_CONFIG_DIR redirects everything ExoSnap itself persists, but the QML
// engine's disk cache is not ExoSnap's to place: Qt resolves it from
// QStandardPaths::CacheLocation, which reads the real per-user folder through
// SHGetKnownFolderPath and therefore ignores both EXOSNAP_CONFIG_DIR and a
// redirected %LOCALAPPDATA%. So a run launched with an isolated config dir still
// wrote %LOCALAPPDATA%\ExoSnap\cache\qmlcache\*.qmlc into the real user's tree.
//
// This did not exist while the frontend was Qt Widgets, and it is what the
// release packaging smoke means by "isolation breach": it snapshots
// %LOCALAPPDATA%\ExoSnap around the launch of the packaged executable and
// asserts nothing under it changed.
//
// Qt reads QML_DISK_CACHE_PATH before the first QQmlEngine is constructed, so
// this must be called before one exists. An explicit QML_DISK_CACHE_PATH from
// the caller is left alone. Returns true when the variable was set here.
bool AlignQmlDiskCacheWithConfigDir();

// Loads the branded application icon from the Qt resource system, logs why it
// failed if it did, and installs it as the process-wide window icon. Returns
// the icon so a frontend can reapply it to its own window; the returned icon is
// null when the resource could not be loaded.
[[nodiscard]] QIcon InstallApplicationIcon();

// Stamps the EXE's Win32 icon resources onto a native top-level window via
// WM_SETICON. The Qt-level window icon only covers what Qt itself draws; the
// taskbar and Alt-Tab read the native ICON_SMALL/ICON_BIG slots, which stay
// empty unless set here. `native_window_handle` is an HWND.
void ApplyNativeWindowIcons(void* native_window_handle);

// ---------------------------------------------------------------------------
// Phase 3 -- the process-scoped services, owned for the lifetime of main().
// ---------------------------------------------------------------------------

struct BootstrapOptions {
    // Harness runs must stay out of the single-instance guard entirely. Beyond
    // a second instance exiting immediately, the guard calls SetForegroundWindow
    // on the running window -- a harness would yank focus off whatever the
    // developer is doing, which CLAUDE.md rules out.
    bool suppress_single_instance = false;

    // Empty means kAppWindowTitle. Only worth overriding if a frontend ships a
    // deliberately different top-level title.
    QString single_instance_window_title;
};

enum class SingleInstanceOutcome {
    Acquired,       // this process owns the guard; carry on
    AlreadyRunning, // another instance was activated; exit 0 without a window
    Suppressed,     // guard skipped by option (harness) or non-Windows build
};

enum class SentryTestEventOutcome {
    NotRequested,
    SentExitNow, // the event was sent and shutdown already ran; return 0
};

// RAII: the destructor performs the normal-shutdown sequence (detach the engine
// log sink, mark a clean exit, flush crash capture, run a pending elevated
// relaunch). Construct it in main() and let scope exit do the work, so an early
// `return` out of a harness path cannot skip it.
class ProductionBootstrap {
  public:
    explicit ProductionBootstrap(BootstrapOptions options = {});
    ~ProductionBootstrap();

    ProductionBootstrap(const ProductionBootstrap&) = delete;
    ProductionBootstrap& operator=(const ProductionBootstrap&) = delete;
    ProductionBootstrap(ProductionBootstrap&&) = delete;
    ProductionBootstrap& operator=(ProductionBootstrap&&) = delete;

    // Brings up the app log sink and the engine->AppLog bridge, then emits the
    // two startup milestones captured in phases 1 and 2. Call as early after the
    // application object as possible so those milestones land in the physical
    // log file. Idempotent.
    void InitializeLogging(const PreAppResult& pre_app, const PostAppResult& post_app);

    // Records one further startup milestone in both the perf log and the
    // StartupTrace table (frontend-specific ones: theme-applied, first-paint).
    // No-op before InitializeLogging().
    void RecordStartupMilestone(const QString& label, qint64 elapsed_ms);

    // Takes the named single-instance mutex, or activates the already-running
    // instance's window and reports AlreadyRunning. The mutex handle is kept so
    // an elevated relaunch can hand it to its successor.
    [[nodiscard]] SingleInstanceOutcome AcquireSingleInstance();

    // Resolves + creates the crash directory and starts the capture engine
    // (ADR 0017). BeginSession is deliberately NOT called here: the frontend
    // must first read the previous session's crash context, which starting a
    // new session would overwrite.
    void InitializeCrashCapture();

    // Empty when the crash directory could not be resolved, i.e. crash capture
    // is disabled for this run.
    [[nodiscard]] const std::string& crashDir() const noexcept {
        return crash_dir_;
    }

    // EXOSNAP_SENTRY_TEST_EVENT=1 sends one diagnostic event and asks the caller
    // to exit. Harmless in production (the variable is unset) and inert in any
    // build without a compiled-in DSN.
    [[nodiscard]] SentryTestEventOutcome RunSentryTestEventIfRequested();

    // ADR 0033: arm an elevated relaunch to run once the event loop has ended.
    // Doing it at shutdown rather than on the spot means the current instance
    // has already released its files and its single-instance claim before the
    // successor asks for them.
    void requestElevatedRelaunch(const QStringList& args);
    [[nodiscard]] bool elevatedRelaunchRequested() const noexcept {
        return elevated_relaunch_requested_;
    }

    // Releases the single-instance mutex and relaunches via ShellExecuteEx
    // ("runas", UAC). A UAC decline is a normal, graceful outcome -- stay
    // non-elevated, no retry loop. No-op when nothing was requested.
    void RunPendingElevatedRelaunch();

    // The normal-shutdown sequence. Idempotent; the destructor calls it.
    void Shutdown();

  private:
    void ReleaseSingleInstance();

    BootstrapOptions options_;
    std::string crash_dir_;
    void* single_instance_mutex_ = nullptr; // HANDLE; void* keeps windows.h out of this header
    QStringList elevated_relaunch_args_;
    bool elevated_relaunch_requested_ = false;
    bool logging_initialized_ = false;
    bool crash_capture_attempted_ = false;
    bool shut_down_ = false;
};

} // namespace exosnap::bootstrap

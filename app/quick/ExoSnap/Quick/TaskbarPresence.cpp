#include "TaskbarPresence.h"

#include <QGuiApplication>
#include <QStyleHints>

#include "diagnostics/AppLog.h"

#include <QCoreApplication>
#include <QString>

#if defined(Q_OS_WIN)
#include <windows.h>
// After windows.h, which shobjidl_core.h depends on.
#include <shobjidl_core.h>

#include "exosnap_resource.h"
#endif

namespace exosnap::quick {

namespace {

constexpr qint32 kSucceeded = 0;

[[nodiscard]] bool Succeeded(qint32 hr) noexcept {
    return hr >= 0;
}

[[nodiscard]] QString HrText(qint32 hr) {
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

#if defined(Q_OS_WIN)

// The button icons and the badges are HICONs out of the executable's own
// resources. LR_SHARED is what makes a heartbeat affordable: the OS caches one
// handle per (module, id, size) tuple, so a swap allocates nothing and the
// handles must not be destroyed.
[[nodiscard]] HICON SharedIcon(int resource_id, int size) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (instance == nullptr)
        return nullptr;
    return static_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(resource_id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR | LR_SHARED));
}

// True when Windows is drawing its chrome light. The SYSTEM appearance, not the
// application's: the thumbnail strip belongs to the taskbar, so the ground these
// glyphs sit on follows Windows even when the product is set to dark.
[[nodiscard]] bool SystemChromeIsLight() {
    return QGuiApplication::styleHints() != nullptr &&
           QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light;
}

[[nodiscard]] QString ThumbTooltip(ShellAction action) {
    switch (action) {
    case ShellAction::Start:
        return QCoreApplication::translate("TaskbarPresence", "Start recording");
    case ShellAction::Pause:
        return QCoreApplication::translate("TaskbarPresence", "Pause recording");
    case ShellAction::Resume:
        return QCoreApplication::translate("TaskbarPresence", "Resume recording");
    case ShellAction::Stop:
        return QCoreApplication::translate("TaskbarPresence", "Stop recording");
    case ShellAction::OpenOutputFolder:
        return QCoreApplication::translate("TaskbarPresence", "Open output folder");
    case ShellAction::None:
        break;
    }
    return {};
}

[[nodiscard]] TBPFLAG ProgressFlag(TaskbarProgressState state) noexcept {
    switch (state) {
    case TaskbarProgressState::Normal:
        return TBPF_NORMAL;
    case TaskbarProgressState::Indeterminate:
        return TBPF_INDETERMINATE;
    case TaskbarProgressState::Error:
        return TBPF_ERROR;
    case TaskbarProgressState::NoProgress:
        break;
    }
    return TBPF_NOPROGRESS;
}

void FillThumbButton(THUMBBUTTON& out, const ThumbButtonSpec& spec, int icon_size) {
    out.dwMask = static_cast<THUMBBUTTONMASK>(THB_ICON | THB_TOOLTIP | THB_FLAGS);
    out.iId = static_cast<UINT>(spec.command_id);
    out.hIcon = SharedIcon(ThumbIconResourceFor(spec.action, SystemChromeIsLight()), icon_size);

    const QString tip = ThumbTooltip(spec.action);
    // szTip is a fixed 260-wchar buffer, and lstrcpynW is the documented way to
    // fill it without running off the end.
    lstrcpynW(out.szTip, reinterpret_cast<const wchar_t*>(tip.utf16()),
              static_cast<int>(sizeof(out.szTip) / sizeof(out.szTip[0])));

    int flags = spec.enabled ? THBF_ENABLED : THBF_DISABLED;
    if (!spec.visible)
        flags |= THBF_HIDDEN;
    // THBF_DISMISSONCLICK is deliberately NOT set: the point of pausing from the
    // thumbnail is to watch the preview respond in the same flyout.
    out.dwFlags = static_cast<THUMBBUTTONFLAGS>(flags);
}

class WindowsTaskbarShell final : public TaskbarShell {
  public:
    ~WindowsTaskbarShell() override {
        if (taskbar_ != nullptr)
            taskbar_->Release();
    }

    qint32 initialize() override {
        // Always re-created. The one caller is a TaskbarButtonCreated
        // announcement, and a repeat of that means Explorer restarted -- the
        // interface held here is then a proxy to a process that is gone.
        if (taskbar_ != nullptr) {
            taskbar_->Release();
            taskbar_ = nullptr;
        }

        ITaskbarList3* taskbar = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar));
        if (FAILED(hr))
            return static_cast<qint32>(hr);

        // Documented as required before any other method on the interface.
        hr = taskbar->HrInit();
        if (FAILED(hr)) {
            taskbar->Release();
            return static_cast<qint32>(hr);
        }

        taskbar_ = taskbar;
        // GetSystemMetrics rather than a constant: the thumbnail toolbar draws at
        // the small-icon size, which follows the primary display's scaling.
        icon_size_ = GetSystemMetrics(SM_CXSMICON);
        if (icon_size_ <= 0)
            icon_size_ = 16;
        return kSucceeded;
    }

    qint32 addButtons(void* hwnd, const QVector<ThumbButtonSpec>& buttons) override {
        return withButtons(hwnd, buttons, /*add=*/true);
    }

    qint32 updateButtons(void* hwnd, const QVector<ThumbButtonSpec>& buttons) override {
        return withButtons(hwnd, buttons, /*add=*/false);
    }

    qint32 setProgressState(void* hwnd, TaskbarProgressState state) override {
        if (taskbar_ == nullptr)
            return static_cast<qint32>(E_POINTER);
        return static_cast<qint32>(taskbar_->SetProgressState(static_cast<HWND>(hwnd), ProgressFlag(state)));
    }

    qint32 setProgressValue(void* hwnd, quint64 completed, quint64 total) override {
        if (taskbar_ == nullptr)
            return static_cast<qint32>(E_POINTER);
        return static_cast<qint32>(taskbar_->SetProgressValue(static_cast<HWND>(hwnd), completed, total));
    }

  private:
    qint32 withButtons(void* hwnd, const QVector<ThumbButtonSpec>& buttons, bool add) {
        if (taskbar_ == nullptr)
            return static_cast<qint32>(E_POINTER);
        // Documented ceiling for a thumbnail toolbar. The product registers four.
        if (buttons.isEmpty() || buttons.size() > 7)
            return static_cast<qint32>(E_INVALIDARG);

        THUMBBUTTON native[7]{};
        for (int i = 0; i < buttons.size(); ++i)
            FillThumbButton(native[i], buttons[i], icon_size_);

        HWND window = static_cast<HWND>(hwnd);
        const auto count = static_cast<UINT>(buttons.size());
        const HRESULT hr = add ? taskbar_->ThumbBarAddButtons(window, count, native)
                               : taskbar_->ThumbBarUpdateButtons(window, count, native);
        return static_cast<qint32>(hr);
    }

    ITaskbarList3* taskbar_ = nullptr;
    int icon_size_ = 16;
};

#endif // Q_OS_WIN

} // namespace

TaskbarShell::~TaskbarShell() = default;

std::unique_ptr<TaskbarShell> MakePlatformTaskbarShell() {
#if defined(Q_OS_WIN)
    return std::make_unique<WindowsTaskbarShell>();
#else
    // No taskbar to integrate with. Nothing calls into a null shell, and the
    // product state is tracked either way.
    return {};
#endif
}

TaskbarPresence::TaskbarPresence(QObject* parent) : QObject(parent), shell_(MakePlatformTaskbarShell()) {
    // The glyph set is chosen per system appearance (see ThumbIconResource), and a
    // choice made once at registration would be wrong for the rest of the session
    // the moment Windows switches. The buttons cannot be re-ADDED -- that is once
    // per taskbar button -- so the existing set is updated in place with icons
    // resolved anew.
    if (QGuiApplication::styleHints() != nullptr) {
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
                         [this]() { refreshThumbIcons(); });
    }
}

int ThumbIconResourceFor(ShellAction action, bool light_chrome) noexcept {
    switch (action) {
    case ShellAction::Start:
        return light_chrome ? IDI_EXOSNAP_THUMB_RECORD_LIGHT : IDI_EXOSNAP_THUMB_RECORD;
    case ShellAction::Pause:
        return light_chrome ? IDI_EXOSNAP_THUMB_PAUSE_LIGHT : IDI_EXOSNAP_THUMB_PAUSE;
    case ShellAction::Resume:
        return light_chrome ? IDI_EXOSNAP_THUMB_RESUME_LIGHT : IDI_EXOSNAP_THUMB_RESUME;
    case ShellAction::Stop:
        return light_chrome ? IDI_EXOSNAP_THUMB_STOP_LIGHT : IDI_EXOSNAP_THUMB_STOP;
    case ShellAction::OpenOutputFolder:
        return light_chrome ? IDI_EXOSNAP_THUMB_FOLDER_LIGHT : IDI_EXOSNAP_THUMB_FOLDER;
    case ShellAction::None:
        break;
    }
    return light_chrome ? IDI_EXOSNAP_THUMB_RECORD_LIGHT : IDI_EXOSNAP_THUMB_RECORD;
}

void TaskbarPresence::refreshThumbIcons() {
    if (!ready_ || !shell_available_ || shell_ == nullptr || hwnd_ == nullptr || !buttons_registered_)
        return;
    // Nothing about the transport changed, so applyPresence() would do nothing:
    // its guard compares desired_ against applied_. The icons are what changed.
    reportResult("ThumbBarUpdateButtons (appearance)", shell_->updateButtons(hwnd_, ButtonsFor(desired_)));
}

TaskbarPresence::~TaskbarPresence() = default;

void TaskbarPresence::setShellForTest(std::unique_ptr<TaskbarShell> shell) {
    shell_ = std::move(shell);
    shell_available_ = false;
    buttons_registered_ = false;
    resetApplied();
}

void TaskbarPresence::setHandle(void* hwnd) {
    if (hwnd_ == hwnd)
        return;

    hwnd_ = hwnd;
    // A new taskbar button has been told nothing, whatever the old one had
    // applied, and Explorer will announce it separately. Anything sent in
    // between is accepted by COM and dropped.
    ready_ = false;
    shell_available_ = false;
    buttons_registered_ = false;
    resetApplied();
    // The bar belongs to the operation, not to the window: an export that is
    // still running has to reappear on the new button.
    progress_dirty_ = progress_.state() != TaskbarProgressState::NoProgress;

    diagnostics::AppLog::debug(QStringLiteral("shell"),
                               hwnd_ != nullptr ? QStringLiteral("taskbar presence awaiting shell readiness")
                                                : QStringLiteral("taskbar presence detached"));
}

void* TaskbarPresence::handle() const noexcept {
    return hwnd_;
}

void TaskbarPresence::notifyShellReady(void* hwnd) {
    // The message comes from a process-wide filter, and readiness is per-window.
    if (hwnd == nullptr || hwnd != hwnd_)
        return;

    const bool re_announced = ready_;
    ready_ = true;
    armShell();

    diagnostics::AppLog::info(
        QStringLiteral("shell"),
        QStringLiteral("taskbar %1 (buttons=%2)")
            .arg(re_announced ? QStringLiteral("re-announced") : QStringLiteral("ready"),
                 buttons_registered_ ? QStringLiteral("registered") : QStringLiteral("unavailable")));
}

void TaskbarPresence::armShell() {
    // Nothing is applied yet, whatever a previous Explorer instance was told.
    buttons_registered_ = false;
    shell_available_ = false;
    resetApplied();

    if (shell_ == nullptr || hwnd_ == nullptr)
        return;

    const qint32 init_hr = shell_->initialize();
    reportResult("taskbar init", init_hr);
    shell_available_ = Succeeded(init_hr);
    if (!shell_available_)
        return;

    // The full set goes up in one call: ThumbBarAddButtons is once per taskbar
    // button, and a button that was not registered can never be added later --
    // only hidden. So all three slots are registered up front and the state only
    // ever changes their visibility.
    const qint32 add_hr = shell_->addButtons(hwnd_, ButtonsFor(desired_));
    reportResult("ThumbBarAddButtons", add_hr);
    buttons_registered_ = Succeeded(add_hr);

    applied_ = desired_;
    applied_valid_ = true;

    // An operation that is still running owes the new taskbar button its state.
    progress_dirty_ = progress_dirty_ || progress_.state() != TaskbarProgressState::NoProgress;
    applyProgress();
}

bool TaskbarPresence::ready() const noexcept {
    return ready_;
}

void TaskbarPresence::setPresence(const ShellPresenceState& state) {
    desired_ = state;
    applyPresence();
}

bool TaskbarPresence::handleCommand(quint64 wparam) {
    const int notification = static_cast<int>((wparam >> 16) & 0xFFFFU);
    if (notification != kThumbButtonClickedNotification)
        return false;

    const int command_id = static_cast<int>(wparam & 0xFFFFU);
    ShellButton button{};
    if (!ShellButtonFromCommandId(command_id, button))
        return false;

    // Ours either way. A click Explorer delivered against a strip the session has
    // already left is consumed and does nothing, rather than falling through to
    // DefWindowProc as an unhandled command.
    const ShellAction action = ResolveShellCommand(command_id, desired_);
    if (action == ShellAction::None) {
        diagnostics::AppLog::debug(
            QStringLiteral("shell"),
            QStringLiteral("taskbar button %1 refused by the current state").arg(command_id, 0, 16));
        return true;
    }

    emit actionRequested(action);
    return true;
}

TaskbarProgressLease TaskbarPresence::acquireProgress(TaskbarProgressOwner owner) {
    const TaskbarProgressLease lease = progress_.acquire(owner);
    if (!lease.valid()) {
        // Observed rather than merged. Two producers in one bar is a bar that
        // jumps backwards, and the refused operation still reports in its own
        // surface.
        diagnostics::AppLog::info(QStringLiteral("shell"),
                                  QStringLiteral("taskbar progress already held; second producer publishes nothing"));
        return lease;
    }
    progress_dirty_ = true;
    applyProgress();
    return lease;
}

void TaskbarPresence::updateProgress(const TaskbarProgressLease& lease, double fraction) {
    if (!progress_.update(lease, fraction))
        return;
    progress_dirty_ = true;
    applyProgress();
}

void TaskbarPresence::setProgressIndeterminate(const TaskbarProgressLease& lease) {
    if (!progress_.setIndeterminate(lease))
        return;
    progress_dirty_ = true;
    applyProgress();
}

void TaskbarPresence::finishProgress(const TaskbarProgressLease& lease) {
    if (!progress_.finish(lease))
        return;
    progress_dirty_ = true;
    applyProgress();
}

void TaskbarPresence::failProgress(const TaskbarProgressLease& lease) {
    if (!progress_.fail(lease))
        return;
    progress_dirty_ = true;
    applyProgress();
}

void TaskbarPresence::cancelProgress(const TaskbarProgressLease& lease) {
    if (!progress_.cancel(lease))
        return;
    progress_dirty_ = true;
    applyProgress();
}

void TaskbarPresence::releaseProgress(const TaskbarProgressLease& lease) {
    if (!progress_.release(lease))
        return;
    progress_dirty_ = true;
    applyProgress();
}

const TaskbarProgressLedger& TaskbarPresence::progress() const noexcept {
    return progress_;
}

bool TaskbarPresence::buttonsRegistered() const noexcept {
    return buttons_registered_;
}

bool TaskbarPresence::shellAvailable() const noexcept {
    return shell_available_;
}

QVector<ThumbButtonSpec> TaskbarPresence::ButtonsFor(const ShellPresenceState& state) {
    // Fixed order and fixed ids. The set cannot change after registration, so the
    // slot a button occupies is part of its identity.
    const ShellButton buttons[] = {ShellButton::Record, ShellButton::PauseResume, ShellButton::Stop,
                                   ShellButton::OpenFolder};
    const int ids[] = {kShellButtonIdRecord, kShellButtonIdPauseResume, kShellButtonIdStop, kShellButtonIdOpenFolder};
    constexpr int kCount = 4;

    QVector<ThumbButtonSpec> specs;
    specs.reserve(kCount);
    for (int i = 0; i < kCount; ++i) {
        const ShellButtonAppearance appearance = ShellButtonFor(buttons[i], state);
        specs.push_back(ThumbButtonSpec{ids[i], appearance.action, appearance.visible, appearance.enabled});
    }
    return specs;
}

void TaskbarPresence::applyPresence() {
    if (!ready_ || !shell_available_ || shell_ == nullptr || hwnd_ == nullptr)
        return;

    const bool transport_changed = !applied_valid_ || applied_ != desired_;

    if (transport_changed && buttons_registered_)
        reportResult("ThumbBarUpdateButtons", shell_->updateButtons(hwnd_, ButtonsFor(desired_)));

    applied_ = desired_;
    applied_valid_ = true;
}

void TaskbarPresence::applyProgress() {
    if (!progress_dirty_)
        return;
    // Stays dirty on purpose: the operation is still running and the bar owes it
    // a state the moment there is a taskbar button to draw on.
    if (!ready_ || !shell_available_ || shell_ == nullptr || hwnd_ == nullptr)
        return;

    progress_dirty_ = false;
    const TaskbarProgressState state = progress_.state();
    reportResult("SetProgressState", shell_->setProgressState(hwnd_, state));
    if (state != TaskbarProgressState::Normal)
        return;

    // Whole percent out of 100: the ledger already gated the reports to that
    // resolution, and a bar a few hundred pixels wide shows no more.
    const auto completed = static_cast<quint64>(progress_.fraction() * 100.0 + 0.5);
    reportResult("SetProgressValue", shell_->setProgressValue(hwnd_, completed, 100));
}

void TaskbarPresence::resetApplied() {
    applied_ = ShellPresenceState{};
    applied_valid_ = false;
}

void TaskbarPresence::reportResult(const char* operation, qint32 hr) {
    if (Succeeded(hr)) {
        // A success after a refusal is worth a line; a run of successes is not.
        if (last_failed_operation_ != nullptr && last_failed_operation_ == operation) {
            last_failed_operation_ = nullptr;
            last_failed_hr_ = 0;
            diagnostics::AppLog::info(QStringLiteral("shell"),
                                      QStringLiteral("taskbar %1 recovered").arg(QString::fromLatin1(operation)));
        }
        return;
    }

    // Deduplicated: this runs on every state change, and a per-call line would
    // bury the first one under a recording's worth of repeats. A NEW cause is
    // reported.
    if (last_failed_operation_ == operation && last_failed_hr_ == hr)
        return;
    last_failed_operation_ = operation;
    last_failed_hr_ = hr;

    // Warning, not error: the shell refusing costs a button, never a capture.
    diagnostics::AppLog::warning(QStringLiteral("shell"),
                                 QStringLiteral("taskbar %1 refused (hr=%2) -- shell integration degraded")
                                     .arg(QString::fromLatin1(operation), HrText(hr)));
}

} // namespace exosnap::quick

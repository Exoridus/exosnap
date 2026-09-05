#include "services/CaptureTargetNotifier.h"

#include <QDateTime>

#include "diagnostics/AppLog.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QPointer>

#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace exosnap {
namespace {

std::mutex g_hook_mutex;
std::unordered_map<HWINEVENTHOOK, QPointer<CaptureTargetNotifier>> g_hook_owners;

bool targetEqual(const exosnap::engine::CaptureTarget& left, const exosnap::engine::CaptureTarget& right) noexcept {
    return left.kind == right.kind && left.native_id == right.native_id && left.description == right.description;
}

} // namespace

bool CaptureTargetSnapshot::operator==(const CaptureTargetSnapshot& other) const noexcept {
    return targets.size() == other.targets.size() &&
           std::equal(targets.begin(), targets.end(), other.targets.begin(), targetEqual);
}

bool CaptureTargetSnapshot::operator!=(const CaptureTargetSnapshot& other) const noexcept {
    return !(*this == other);
}

CaptureTargetNotifier::CaptureTargetNotifier(QObject* parent) : QObject(parent) {
    debounce_timer_.setSingleShot(true);
    debounce_timer_.setInterval(200);
    connect(&debounce_timer_, &QTimer::timeout, this, [this]() { refreshNow(pending_reason_); });
    connect(&display_notifier_, &DisplayDeviceNotifier::snapshotChanged, this,
            [this](const DisplaySnapshot&, DiscoveryReason reason) { scheduleRefresh(reason); });
}

CaptureTargetNotifier::~CaptureTargetNotifier() {
    stop();
}

void CaptureTargetNotifier::setEnumerator(Enumerator enumerator) {
    enumerator_ = std::move(enumerator);
}

void CaptureTargetNotifier::setEnumeratorForTest(Enumerator enumerator) {
    enumerator_ = std::move(enumerator);
    test_mode_ = true;
}

void CaptureTargetNotifier::setDebounceIntervalMsForTest(int milliseconds) {
    debounce_timer_.setInterval(milliseconds);
}

void CaptureTargetNotifier::simulateNativeEvent(DiscoveryReason reason) {
    scheduleRefresh(reason);
}

void CaptureTargetNotifier::flushPendingForTest() {
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    if (debounce_timer_.isActive()) {
        debounce_timer_.stop();
        refreshNow(pending_reason_);
    }
}

void CaptureTargetNotifier::requestRefreshForTest(DiscoveryReason reason) {
    scheduleRefresh(reason);
}

void CaptureTargetNotifier::setMaxDeferralMsForTest(qint64 milliseconds) {
    max_deferral_ms_ = milliseconds;
}

void CaptureTargetNotifier::start() {
    if (started_)
        return;
    started_ = true;
    if (!test_mode_) {
        display_notifier_.start();
        installWindowHook();
    }
    refreshNow(DiscoveryReason::Startup);
}

void CaptureTargetNotifier::stop() {
    if (!started_)
        return;
    started_ = false;
    debounce_timer_.stop();
    removeWindowHook();
    display_notifier_.stop();
}

void CaptureTargetNotifier::rescan() {
    refreshNow(DiscoveryReason::Rescan);
}

const CaptureTargetSnapshot& CaptureTargetNotifier::currentSnapshot() const noexcept {
    return last_snapshot_;
}

void CALLBACK CaptureTargetNotifier::WinEventCallback(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object_id,
                                                      LONG child_id, DWORD, DWORD) {
    if (window == nullptr || object_id != OBJID_WINDOW || child_id != CHILDID_SELF)
        return;

    QPointer<CaptureTargetNotifier> owner;
    {
        std::lock_guard lock(g_hook_mutex);
        const auto found = g_hook_owners.find(hook);
        if (found != g_hook_owners.end())
            owner = found->second;
    }
    if (owner == nullptr)
        return;

    const DiscoveryReason reason = event == EVENT_OBJECT_DESTROY || event == EVENT_OBJECT_HIDE
                                       ? DiscoveryReason::DeviceRemoved
                                       : DiscoveryReason::DeviceAdded;
    QMetaObject::invokeMethod(owner, [owner, reason]() {
        if (owner != nullptr)
            owner->scheduleRefresh(reason);
    });
}

void CaptureTargetNotifier::scheduleRefresh(DiscoveryReason reason) {
    if (!started_ && !test_mode_)
        return;
    if (!debounce_timer_.isActive() || reason == DiscoveryReason::DeviceRemoved)
        pending_reason_ = reason;

    if (!debounce_timer_.isActive()) {
        first_pending_at_ms_ = QDateTime::currentMSecsSinceEpoch();
        debounce_timer_.start();
        return;
    }

    // The deferral is CAPPED. Restarting the timer on every event is what
    // debouncing means, but the desktop produces window lifecycle events
    // continuously -- every menu, tooltip, splash and background process -- so an
    // uncapped restart could postpone the refresh indefinitely and the target list
    // would silently keep describing a desktop that no longer exists. MEASURED: a
    // window created while the app was running stayed unselectable through the
    // control channel, while the same window created BEFORE the app started was
    // selectable on the first attempt.
    //
    // Past the cap the timer is left alone rather than restarted, so it fires with
    // what it has and the next event opens a fresh window.
    const qint64 waited = QDateTime::currentMSecsSinceEpoch() - first_pending_at_ms_;
    if (waited < max_deferral_ms_)
        debounce_timer_.start();
}

void CaptureTargetNotifier::refreshNow(DiscoveryReason reason) {
    if (!enumerator_)
        return;
    CaptureTargetSnapshot fresh = enumerator_();
    if (fresh == last_snapshot_)
        return;
    last_snapshot_ = std::move(fresh);
    diagnostics::AppLog::info(QStringLiteral("CaptureTargetDiscovery"),
                              QStringLiteral("Snapshot changed — targets:%1 reason:%2")
                                  .arg(last_snapshot_.targets.size())
                                  .arg(QLatin1StringView(DiscoveryReasonName(reason))));
    emit snapshotChanged(last_snapshot_, reason);
}

void CaptureTargetNotifier::installWindowHook() {
    if (window_hook_ != nullptr)
        return;
    window_hook_ = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr, &WinEventCallback, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (window_hook_ == nullptr) {
        diagnostics::AppLog::warning(QStringLiteral("CaptureTargetDiscovery"),
                                     QStringLiteral("Window lifecycle hook could not be installed"));
        return;
    }
    std::lock_guard lock(g_hook_mutex);
    g_hook_owners.emplace(window_hook_, this);
}

void CaptureTargetNotifier::removeWindowHook() {
    if (window_hook_ == nullptr)
        return;
    {
        std::lock_guard lock(g_hook_mutex);
        g_hook_owners.erase(window_hook_);
    }
    UnhookWinEvent(window_hook_);
    window_hook_ = nullptr;
}

} // namespace exosnap

#include "services/CaptureTargetNotifier.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QThread>
#include <QUuid>

#include "diagnostics/AppLog.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QPointer>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

namespace exosnap {
namespace {

std::mutex g_hook_mutex;
std::unordered_map<HWINEVENTHOOK, QPointer<CaptureTargetNotifier>> g_hook_owners;

bool targetEqual(const exosnap::engine::CaptureTarget& left, const exosnap::engine::CaptureTarget& right) noexcept {
    return left.kind == right.kind && left.native_id == right.native_id && left.description == right.description;
}

std::optional<WindowCaptureIdentity> ObserveWindowIdentity(const engine::CaptureTarget& target) {
    const HWND window = reinterpret_cast<HWND>(target.native_id);
    DWORD process_id = 0;
    if (GetWindowThreadProcessId(window, &process_id) == 0 || process_id == 0)
        return std::nullopt;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr)
        return std::nullopt;
    FILETIME creation{}, exit{}, kernel{}, user{};
    DWORD exit_code = 0;
    std::wstring path(32768, L'\0');
    DWORD path_size = static_cast<DWORD>(path.size());
    const bool valid = GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE &&
                       GetExitCodeProcess(process, &exit_code) != FALSE && exit_code == STILL_ACTIVE &&
                       QueryFullProcessImageNameW(process, 0, path.data(), &path_size) != FALSE;
    CloseHandle(process);
    DWORD current_process = 0;
    if (!valid || GetWindowThreadProcessId(window, &current_process) == 0 || current_process != process_id)
        return std::nullopt;
    WindowCaptureIdentity identity;
    identity.native_id = target.native_id;
    identity.process_id = process_id;
    identity.process_creation_time = (static_cast<uint64_t>(creation.dwHighDateTime) << 32U) | creation.dwLowDateTime;
    identity.application_identity =
        QDir::cleanPath(QString::fromWCharArray(path.data(), static_cast<int>(path_size))).toCaseFolded();
    return identity;
}

} // namespace

QString WindowCaptureIdentity::stableIdentity() const {
    if (native_id == 0 || process_id == 0 || process_creation_time == 0 || window_generation == 0)
        return {};
    const QByteArray witness = QByteArray::number(static_cast<qulonglong>(native_id)) + ':' +
                               QByteArray::number(process_id) + ':' + QByteArray::number(process_creation_time) + ':' +
                               QByteArray::number(window_generation);
    return QStringLiteral("window:") +
           QString::fromLatin1(QCryptographicHash::hash(witness, QCryptographicHash::Sha256).toHex());
}
bool CaptureTargetSnapshot::operator==(const CaptureTargetSnapshot& other) const noexcept {
    return window_identities == other.window_identities && targets.size() == other.targets.size() &&
           std::equal(targets.begin(), targets.end(), other.targets.begin(), targetEqual);
}

bool CaptureTargetSnapshot::operator!=(const CaptureTargetSnapshot& other) const noexcept {
    return !(*this == other);
}

CaptureTargetNotifier::CaptureTargetNotifier(QObject* parent)
    : QObject(parent), window_identity_observer_([this](const auto& target) { return observeWindowIdentity(target); }),
      window_witness_property_(QStringLiteral("ExoSnap.WindowLifetime.") +
                               QUuid::createUuid().toString(QUuid::WithoutBraces)) {
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

void CaptureTargetNotifier::setWindowIdentityObserverForTest(WindowIdentityObserver observer) {
    window_identity_observer_ = std::move(observer);
    custom_window_identity_observer_ = true;
    test_mode_ = true;
}

void CaptureTargetNotifier::simulateWindowLifecycleForTest(uintptr_t native_id, bool destroyed) {
    noteWindowLifecycle(native_id, destroyed);
}

std::optional<WindowCaptureIdentity> CaptureTargetNotifier::observeWindowIdentity(const engine::CaptureTarget& target) {
    const HWND window = reinterpret_cast<HWND>(target.native_id);
    const auto property = reinterpret_cast<LPCWSTR>(window_witness_property_.utf16());
    const HANDLE before = GetPropW(window, property);
    auto identity = ObserveWindowIdentity(target);
    if (!identity || before != GetPropW(window, property))
        return std::nullopt;
    HANDLE witness = before;
    if (witness == nullptr) {
        witness = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(++next_window_generation_));
        if (witness == nullptr || SetPropW(window, property, witness) == FALSE)
            return std::nullopt;
        native_witnesses_[target.native_id] = reinterpret_cast<uintptr_t>(witness);
        const auto verified = ObserveWindowIdentity(target);
        if (!verified || verified->process_id != identity->process_id ||
            verified->process_creation_time != identity->process_creation_time || GetPropW(window, property) != witness)
            return std::nullopt;
    }
    identity->window_generation = reinterpret_cast<uintptr_t>(witness);
    return identity;
}
bool CaptureTargetNotifier::isCurrent(const WindowCaptureIdentity& identity) const {
    const auto found = window_generations_.find(identity.native_id);
    if (!started_ || identity.stableIdentity().isEmpty() || found == window_generations_.end() ||
        found->second != identity.window_generation ||
        std::find(last_snapshot_.window_identities.begin(), last_snapshot_.window_identities.end(), identity) ==
            last_snapshot_.window_identities.end())
        return false;
    if (custom_window_identity_observer_)
        return true;
    return windowIdentityValidator(identity)();
}
std::function<bool()> CaptureTargetNotifier::windowIdentityValidator(WindowCaptureIdentity identity) const {
    if (custom_window_identity_observer_)
        return [] { return false; };
    const QString property_name = window_witness_property_;
    const auto active = lifetime_active_;
    return [identity = std::move(identity), property_name, active] {
        if (!active->load(std::memory_order_acquire) || identity.stableIdentity().isEmpty())
            return false;
        const HWND window = reinterpret_cast<HWND>(identity.native_id);
        const auto property = reinterpret_cast<LPCWSTR>(property_name.utf16());
        const HANDLE witness = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(identity.window_generation));
        if (GetPropW(window, property) != witness)
            return false;
        const auto observed = ObserveWindowIdentity({engine::CaptureTarget::Kind::Window, identity.native_id, {}});
        return observed && observed->process_id == identity.process_id &&
               observed->process_creation_time == identity.process_creation_time &&
               GetPropW(window, property) == witness;
    };
}
void CaptureTargetNotifier::noteWindowLifecycle(uintptr_t native_id, bool destroyed) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (destroyed) {
        if (window_generations_.erase(native_id) == 0)
            return;
    } else {
        window_generations_[native_id] = ++next_window_generation_;
    }
    emit windowLifetimeInvalidated(static_cast<qulonglong>(native_id));
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
    lifetime_active_ = std::make_shared<std::atomic<bool>>(true);
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
    lifetime_active_->store(false, std::memory_order_release);
    started_ = false;
    if (!custom_window_identity_observer_) {
        const auto property = reinterpret_cast<LPCWSTR>(window_witness_property_.utf16());
        for (const auto& [native_id, generation] : native_witnesses_) {
            const HWND window = reinterpret_cast<HWND>(native_id);
            if (GetPropW(window, property) == reinterpret_cast<HANDLE>(static_cast<uintptr_t>(generation)))
                RemovePropW(window, property);
        }
    }
    native_witnesses_.clear();
    window_generations_.clear();
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

    // Out-of-context hooks run on the installing thread. Invalidate before the
    // deferred enumeration so queued confirmation cannot reuse a destroyed HWND.
    if (event == EVENT_OBJECT_DESTROY)
        owner->noteWindowLifecycle(reinterpret_cast<uintptr_t>(window), true);
    else if (event == EVENT_OBJECT_CREATE && GetAncestor(window, GA_ROOT) == window)
        owner->noteWindowLifecycle(reinterpret_cast<uintptr_t>(window), false);
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
    if (!started_ || !enumerator_)
        return;
    CaptureTargetSnapshot fresh = enumerator_();
    fresh.window_identities.clear();
    if (window_identity_observer_) {
        for (const auto& target : fresh.targets) {
            if (target.kind != engine::CaptureTarget::Kind::Window)
                continue;
            auto identity = window_identity_observer_(target);
            if (!identity)
                continue;
            // A native property witnesses the window lifetime even when WinEvent delivery is delayed.
            if (!custom_window_identity_observer_) {
                window_generations_[target.native_id] = identity->window_generation;
                fresh.window_identities.push_back(std::move(*identity));
                continue;
            }
            auto found = window_generations_.find(target.native_id);
            if (found == window_generations_.end())
                found = window_generations_.emplace(target.native_id, ++next_window_generation_).first;
            identity->window_generation = found->second;
            fresh.window_identities.push_back(std::move(*identity));
        }
    }
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

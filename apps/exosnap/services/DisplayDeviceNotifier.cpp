#include "services/DisplayDeviceNotifier.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>

#include <algorithm>

#include "diagnostics/AppLog.h"

namespace exosnap {

// ---------------------------------------------------------------------------
// DisplayInfo — comparison
// ---------------------------------------------------------------------------

bool DisplayInfo::operator==(const DisplayInfo& other) const noexcept {
    return id == other.id && name == other.name && geometry == other.geometry &&
           available_geometry == other.available_geometry &&
           qFuzzyCompare(device_pixel_ratio, other.device_pixel_ratio) &&
           qFuzzyCompare(logical_dpi, other.logical_dpi) && rotation_degrees == other.rotation_degrees &&
           primary == other.primary;
}

bool DisplayInfo::operator!=(const DisplayInfo& other) const noexcept {
    return !(*this == other);
}

// ---------------------------------------------------------------------------
// DisplaySnapshot — comparison
// ---------------------------------------------------------------------------

bool DisplaySnapshot::operator==(const DisplaySnapshot& other) const noexcept {
    if (displays.size() != other.displays.size()) {
        return false;
    }
    for (int i = 0; i < displays.size(); ++i) {
        if (displays[i] != other.displays[i]) {
            return false;
        }
    }
    return true;
}

bool DisplaySnapshot::operator!=(const DisplaySnapshot& other) const noexcept {
    return !(*this == other);
}

// ---------------------------------------------------------------------------
// Default enumerator (production) — builds from QGuiApplication::screens()
// ---------------------------------------------------------------------------

static DisplaySnapshot DefaultEnumerator() {
    DisplaySnapshot snap;
    QScreen* primary = QGuiApplication::primaryScreen();
    for (QScreen* screen : QGuiApplication::screens()) {
        DisplayInfo info;
        info.id = screen->name();
        // Use screen->manufacturer() + screen->model() if available (Qt 5.9+),
        // otherwise fall back to the GDI device name.
        const QString manuf = screen->manufacturer();
        const QString model = screen->model();
        if (!manuf.isEmpty() || !model.isEmpty()) {
            const QString combined = (manuf + QStringLiteral(" ") + model).trimmed();
            info.name = combined.isEmpty() ? screen->name() : combined;
        } else {
            info.name = screen->name();
        }
        info.geometry = screen->geometry();
        info.available_geometry = screen->availableGeometry();
        info.device_pixel_ratio = screen->devicePixelRatio();
        info.logical_dpi = screen->logicalDotsPerInch();
        // Derive rotation from screen orientation vs. native orientation.
        const Qt::ScreenOrientation orient = screen->orientation();
        const Qt::ScreenOrientation native = screen->nativeOrientation();
        info.rotation_degrees = static_cast<int>(screen->angleBetween(native, orient));
        info.primary = (screen == primary);
        snap.displays.push_back(std::move(info));
    }
    return snap;
}

// ---------------------------------------------------------------------------
// DisplayDeviceNotifier — construction / destruction
// ---------------------------------------------------------------------------

DisplayDeviceNotifier::DisplayDeviceNotifier(QObject* parent) : QObject(parent), enumerator_(&DefaultEnumerator) {
    debounce_timer_.setSingleShot(true);
    debounce_timer_.setInterval(200);
    connect(&debounce_timer_, &QTimer::timeout, this, [this]() { refreshNow(pending_reason_); });
}

DisplayDeviceNotifier::~DisplayDeviceNotifier() {
    stop();
}

// ---------------------------------------------------------------------------
// Test-support
// ---------------------------------------------------------------------------

void DisplayDeviceNotifier::setEnumeratorForTest(Enumerator enumerator) {
    enumerator_ = std::move(enumerator);
    test_mode_ = true;
}

void DisplayDeviceNotifier::setDebounceIntervalMsForTest(int ms) {
    debounce_timer_.setInterval(ms);
}

void DisplayDeviceNotifier::simulateNativeEvent(DiscoveryReason reason) {
    // Drive the same path that a real screen signal would follow.
    scheduleRefresh(reason);
}

void DisplayDeviceNotifier::flushPendingForTest() {
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    if (debounce_timer_.isActive()) {
        debounce_timer_.stop();
        refreshNow(pending_reason_);
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DisplayDeviceNotifier::start() {
    if (started_) {
        return;
    }

    started_ = true;

    if (test_mode_) {
        // Test mode: no screen signal connections, just mark as started.
        return;
    }

    QGuiApplication* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (!app) {
        diagnostics::AppLog::warning(QStringLiteral("DisplayDiscovery"),
                                     QStringLiteral("No QGuiApplication — display notifications unavailable"));
        return;
    }

    // Application-level signals.
    app_connections_.push_back(connect(app, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
        connectScreenSignals(screen);
        scheduleRefresh(DiscoveryReason::DeviceAdded);
    }));

    app_connections_.push_back(connect(app, &QGuiApplication::screenRemoved, this, [this](QScreen* /*screen*/) {
        // Per-screen connections for the removed screen auto-disconnect because
        // QScreen is being destroyed, but we rebuild the full set to stay clean.
        disconnectAllScreenSignals();
        for (QScreen* s : QGuiApplication::screens()) {
            connectScreenSignals(s);
        }
        scheduleRefresh(DiscoveryReason::DeviceRemoved);
    }));

    app_connections_.push_back(connect(app, &QGuiApplication::primaryScreenChanged, this, [this](QScreen* /*screen*/) {
        scheduleRefresh(DiscoveryReason::PropertyChanged);
    }));

    // Per-screen signals for all currently connected screens.
    for (QScreen* screen : QGuiApplication::screens()) {
        connectScreenSignals(screen);
    }
}

void DisplayDeviceNotifier::stop() {
    if (!started_) {
        return;
    }
    started_ = false;
    debounce_timer_.stop();

    // Disconnect application-level signals.
    for (const auto& conn : app_connections_) {
        QObject::disconnect(conn);
    }
    app_connections_.clear();

    // Disconnect per-screen signals.
    disconnectAllScreenSignals();
}

// ---------------------------------------------------------------------------
// Immediate synchronous refresh
// ---------------------------------------------------------------------------

void DisplayDeviceNotifier::rescan() {
    refreshNow(DiscoveryReason::Rescan);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

DisplaySnapshot DisplayDeviceNotifier::currentSnapshot() const {
    return last_snapshot_;
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void DisplayDeviceNotifier::scheduleRefresh(DiscoveryReason reason) {
    // DeviceRemoved takes priority over other reasons while debouncing.
    if (!debounce_timer_.isActive() || reason == DiscoveryReason::DeviceRemoved) {
        pending_reason_ = reason;
    }
    debounce_timer_.start();
}

void DisplayDeviceNotifier::refreshNow(DiscoveryReason reason) {
    const DisplaySnapshot fresh = buildSnapshot();
    if (fresh == last_snapshot_) {
        return;
    }
    last_snapshot_ = fresh;

    diagnostics::AppLog::info(QStringLiteral("DisplayDiscovery"),
                              QStringLiteral("Snapshot changed — displays:%1 reason:%2")
                                  .arg(fresh.displays.size())
                                  .arg(QLatin1StringView(DiscoveryReasonName(reason))));

    emit snapshotChanged(last_snapshot_, reason);
}

DisplaySnapshot DisplayDeviceNotifier::buildSnapshot() const {
    DisplaySnapshot snap = enumerator_();

    std::sort(snap.displays.begin(), snap.displays.end(),
              [](const DisplayInfo& a, const DisplayInfo& b) { return a.id < b.id; });

    return snap;
}

void DisplayDeviceNotifier::connectScreenSignals(QScreen* screen) {
    // Connect property-change signals using 'this' as context so they
    // auto-disconnect if 'this' is destroyed before the screen.
    screen_connections_.push_back(connect(screen, &QScreen::geometryChanged, this,
                                          [this](const QRect&) { scheduleRefresh(DiscoveryReason::GeometryChanged); }));

    screen_connections_.push_back(connect(screen, &QScreen::availableGeometryChanged, this,
                                          [this](const QRect&) { scheduleRefresh(DiscoveryReason::GeometryChanged); }));

    screen_connections_.push_back(connect(screen, &QScreen::logicalDotsPerInchChanged, this,
                                          [this](qreal) { scheduleRefresh(DiscoveryReason::PropertyChanged); }));

    screen_connections_.push_back(connect(screen, &QScreen::physicalDotsPerInchChanged, this,
                                          [this](qreal) { scheduleRefresh(DiscoveryReason::PropertyChanged); }));

    screen_connections_.push_back(connect(screen, &QScreen::orientationChanged, this, [this](Qt::ScreenOrientation) {
        scheduleRefresh(DiscoveryReason::PropertyChanged);
    }));
}

void DisplayDeviceNotifier::disconnectAllScreenSignals() {
    for (const auto& conn : screen_connections_) {
        QObject::disconnect(conn);
    }
    screen_connections_.clear();
}

} // namespace exosnap

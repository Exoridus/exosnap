#pragma once

#include "models/OverlayContentPolicy.h"
#include "services/ScreenPresentation.h"
#include "settings/AppSettingsStore.h"

#include <QObject>
#include <QRect>
#include <QtQmlIntegration/qqmlintegration.h>

#include <cstdint>
#include <functional>

namespace exosnap {

class RecordViewModel;

namespace quick {

// The narrow boundary for the capture-excluded overlay windows.
//
// WHAT IT OWNS
// ------------
// Exactly the two answers nothing else in the Quick frontend can give:
//
//   1. WHERE — the geometry of the monitor being recorded, in virtual-screen
//      coordinates. QML can reach `Screen`, but only the screen a window is
//      already on; which display the capture target resolves to is a C++ fact.
//   2. WHETHER — whether each overlay window belongs on screen at all, which is
//      a persisted setting AND a recording state AND, for the diagnostics HUD,
//      whether the configured content set is non-empty.
//
// WHAT IT DELIBERATELY DOES NOT OWN
// ---------------------------------
// The metric texts (RecordViewModelAdapter already publishes them) and the
// content flags (SettingsAdapter resolves them through the same
// models::OverlayContentPolicy). Mirroring either here would create a second
// value that can lag the first by one synchronize().
//
// Capture exclusion is not represented here either. It is a per-window
// correctness property enforced by the CaptureExclusion element inside each
// overlay, and it must remain independent of every "should this be visible"
// input above — that is what makes the fail-closed behaviour unconditional.
class OverlayAdapter : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("OverlayAdapter is provided by the application")

    // Empty when the capture target is not a monitor (a window or an unresolved
    // target). The overlays fall back to their own screen in that case, matching
    // the Widgets behaviour.
    Q_PROPERTY(QRect recordedMonitorGeometry READ recordedMonitorGeometry NOTIFY changed FINAL)
    // The actually-recorded rectangle, keyed off the active capture mode: the
    // region rect in Region mode, the live window rectangle in Window mode
    // (DWMWA_EXTENDED_FRAME_BOUNDS, re-resolved every synchronize() so a moved
    // or resized window keeps this attached), the full monitor rectangle in
    // Monitor mode. Empty when nothing is resolvable (no target, or a Window
    // target whose HWND cannot be queried) -- the recording/diagnostics pill
    // hides rather than falling back to some other monitor. The recording and
    // diagnostics pill binds to THIS, never to recordedMonitorGeometry: those
    // two disagree exactly in Window and Region mode, which is the geometry
    // bug this property exists to fix.
    Q_PROPERTY(QRect recordedSourceGeometry READ recordedSourceGeometry NOTIFY changed FINAL)
    // True only when the source rectangle above is a Region-mode crop, as
    // opposed to a Window-mode rectangle that also happens to be smaller than
    // its monitor. The quick-controls dock's "prefer below the region" default
    // placement applies to the first case only; a recorded window is not a
    // region and gets the plain bottom-of-monitor default instead.
    Q_PROPERTY(bool recordedSourceIsRegion READ recordedSourceIsRegion NOTIFY changed FINAL)
    Q_PROPERTY(int recordingState READ recordingState NOTIFY changed FINAL)

    Q_PROPERTY(bool recordingOverlayActive READ recordingOverlayActive NOTIFY changed FINAL)
    Q_PROPERTY(bool countdownOverlayActive READ countdownOverlayActive NOTIFY changed FINAL)
    Q_PROPERTY(bool diagnosticsOverlayActive READ diagnosticsOverlayActive NOTIFY changed FINAL)
    Q_PROPERTY(bool quickControlsActive READ quickControlsActive NOTIFY changed FINAL)

  public:
    // Mirrors models::RecordingOverlayState so QML can name the states instead
    // of comparing integers. The values are static_cast from the policy enum;
    // the order is asserted in OverlayAdapter.cpp.
    enum State { Hidden, Recording, Paused, Warning };
    Q_ENUM(State)

    explicit OverlayAdapter(const RecordViewModel* source = nullptr, QObject* parent = nullptr);

    void setSource(const RecordViewModel* source);
    void setAppSettings(const PersistedAppSettings& settings);
    // Recomputes everything and emits changed() if anything moved. Called from
    // the same place the Record surface is synchronized, so the overlays can
    // never show a state the window has already left.
    void synchronize();

    // The recorded monitor's presentation may have changed WITHOUT the target
    // changing: a resolution switch, a scale change, or a monitor being moved in
    // the desktop arrangement all keep the same HMONITOR. The cache below is
    // keyed on that handle alone, so nothing else would ever re-query it and the
    // overlays would stay pinned to the display's previous rectangle — off the
    // edge of a screen that got smaller, or in the middle of one that got
    // bigger. Called from the display/screen notifications, not from a timer:
    // the fast path exists because synchronize() runs several times a second.
    void invalidateMonitorGeometry();

    [[nodiscard]] QRect recordedMonitorGeometry() const noexcept;
    [[nodiscard]] QRect recordedSourceGeometry() const noexcept;
    [[nodiscard]] bool recordedSourceIsRegion() const noexcept;
    [[nodiscard]] int recordingState() const noexcept;
    [[nodiscard]] bool recordingOverlayActive() const noexcept;
    [[nodiscard]] bool countdownOverlayActive() const noexcept;
    [[nodiscard]] bool diagnosticsOverlayActive() const noexcept;
    [[nodiscard]] bool quickControlsActive() const noexcept;

    // Test seam for the one input that only a real desktop can produce. The
    // monitor rectangle comes from a Win32 query against an HMONITOR, so
    // "the same monitor now reports a different size" is otherwise only
    // reachable by physically changing a display.
    void setPresentationProviderForTesting(std::function<ScreenPresentation(std::uintptr_t)> provider);
    // Same reasoning, for the live window-rectangle query a Window-mode source
    // rect depends on.
    void setWindowRectProviderForTesting(std::function<WindowScreenRect(std::uintptr_t)> provider);

  signals:
    void changed();

  private:
    // Returns true when either cached rectangle changed. Reporting rather than
    // emitting keeps every notify for one synchronize() in a single signal, sent
    // after all state is written.
    [[nodiscard]] bool refreshMonitorGeometry();
    // Resolves recorded_source_geometry_ from the active capture mode. Must run
    // after refreshMonitorGeometry() within the same synchronize(): the
    // Monitor-mode fallback reads recorded_monitor_geometry_ from that call.
    // Unlike refreshMonitorGeometry(), not gated behind the same-HMONITOR fast
    // path: a Window-mode target can move within its hosting monitor, which
    // does not change the HMONITOR but must still move the source rect.
    [[nodiscard]] bool refreshSourceGeometry();

    const RecordViewModel* source_ = nullptr;
    PersistedAppSettings settings_;

    QRect recorded_monitor_geometry_;
    // The native id the cached geometry was resolved from, so a 4 Hz
    // synchronize() does not call into the monitor API on every tick. Zero means
    // "no monitor target", which is a distinct state from "not yet resolved" —
    // both produce an empty rect, and neither needs a re-query.
    std::uintptr_t geometry_native_id_ = 0;
    // Forces the next refresh past the same-monitor fast path. True initially so
    // the first synchronize() resolves a rectangle at all.
    bool geometry_dirty_ = true;
    std::function<ScreenPresentation(std::uintptr_t)> presentation_provider_;
    std::function<WindowScreenRect(std::uintptr_t)> window_rect_provider_;
    // The resolved recording/diagnostics-pill anchor; see recordedSourceGeometry().
    QRect recorded_source_geometry_;
    bool recorded_source_is_region_ = false;

    models::RecordingOverlayState state_ = models::RecordingOverlayState::Hidden;
    bool recording_overlay_active_ = false;
    bool countdown_overlay_active_ = false;
    bool diagnostics_overlay_active_ = false;
    bool quick_controls_active_ = false;
};

} // namespace quick
} // namespace exosnap

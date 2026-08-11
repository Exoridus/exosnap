#include "OverlayAdapter.h"

#include "diagnostics/AppLog.h"
#include "services/ScreenPresentation.h"
#include "viewmodels/RecordViewModel.h"

namespace exosnap::quick {

namespace {

// The QML-facing State enum must stay a faithful re-spelling of the policy
// enum — QML compares against OverlayAdapter.Recording, C++ produces
// models::RecordingOverlayState::Recording, and nothing at runtime would notice
// if the two drifted apart.
static_assert(static_cast<int>(models::RecordingOverlayState::Hidden) == OverlayAdapter::Hidden);
static_assert(static_cast<int>(models::RecordingOverlayState::Recording) == OverlayAdapter::Recording);
static_assert(static_cast<int>(models::RecordingOverlayState::Paused) == OverlayAdapter::Paused);
static_assert(static_cast<int>(models::RecordingOverlayState::Warning) == OverlayAdapter::Warning);

} // namespace

OverlayAdapter::OverlayAdapter(const RecordViewModel* source, QObject* parent) : QObject(parent), source_(source) {
    synchronize();
}

void OverlayAdapter::setSource(const RecordViewModel* source) {
    source_ = source;
    // Force a re-query: the new view model may point at a different target while
    // carrying the same native id value it happened to be cached against.
    geometry_native_id_ = 0;
    recorded_monitor_geometry_ = QRect();
    synchronize();
}

void OverlayAdapter::setAppSettings(const PersistedAppSettings& settings) {
    settings_ = settings;
    synchronize();
}

void OverlayAdapter::synchronize() {
    // Folded into `moved` below rather than emitting on its own: a signal sent
    // from here would run QML bindings against a half-updated adapter, with the
    // new geometry already readable and the new state not yet written.
    const bool geometry_moved = refreshMonitorGeometry();

    models::RecordingOverlayStateInputs inputs;
    bool countdown_running = false;
    if (source_ != nullptr) {
        inputs.recording = source_->state == UiRecordingState::Recording;
        // ArmedFromRecovery is visually a pause in the transport dock and is a
        // paused session in fact: a slice is pending and Resume starts the next
        // one. The HUD says the same thing rather than reading as stopped.
        inputs.paused =
            source_->state == UiRecordingState::Paused || source_->state == UiRecordingState::ArmedFromRecovery;
        inputs.failed = source_->state == UiRecordingState::Failed;
        inputs.dropped_frames = source_->dropped_frames;
        inputs.live_stats_available = source_->live_stats_available;
        countdown_running = source_->state == UiRecordingState::Countdown;
    }

    const models::RecordingOverlayState state = models::ResolveRecordingOverlayState(inputs);
    const bool capture_live = inputs.recording || inputs.paused;

    // Error is resolved, published and rendered by the HUD, but deliberately
    // does NOT activate the window. UiRecordingState::Failed is a RESTING state
    // -- it persists until the user starts another recording -- and this overlay
    // is click-through by construction, so an error pill would sit on the
    // desktop with no way to dismiss it. The failure already has an owner: the
    // modal recording-error surface inside the window. The state is kept in the
    // policy and in the QML because the HUD has to be able to show it the moment
    // a TRANSIENT error presence has a producer; today it has none, and
    // --overlay-visual-state is the only way to reach it.
    const bool recording_overlay_active =
        settings_.show_recording_overlay &&
        (state == models::RecordingOverlayState::Recording || state == models::RecordingOverlayState::Paused ||
         state == models::RecordingOverlayState::Warning);

    const models::DiagnosticsOverlayContent content = models::ResolveDiagnosticsOverlayContent(
        models::DiagnosticsOverlayPresetFromToken(settings_.diagnostics_overlay_preset),
        settings_.diagnostics_overlay_custom_elements);

    // An enabled overlay whose every token has been unticked would be an empty
    // pill floating over the recording. Treated as off rather than drawn empty.
    const bool diagnostics_overlay_active = settings_.show_diagnostics_overlay && capture_live && !content.IsEmpty();

    // Gated on the recording-overlay setting, matching the Widgets shell: the
    // countdown is the same "ExoSnap is about to be on your screen" presence,
    // and a user who turned that off does not expect a 3-2-1 ring either.
    const bool countdown_overlay_active = settings_.show_recording_overlay && countdown_running;

    const bool quick_controls_active = settings_.show_quick_controls && capture_live;

    const bool moved = geometry_moved || state_ != state || recording_overlay_active_ != recording_overlay_active ||
                       countdown_overlay_active_ != countdown_overlay_active ||
                       diagnostics_overlay_active_ != diagnostics_overlay_active ||
                       quick_controls_active_ != quick_controls_active;

    state_ = state;
    recording_overlay_active_ = recording_overlay_active;
    countdown_overlay_active_ = countdown_overlay_active;
    diagnostics_overlay_active_ = diagnostics_overlay_active;
    quick_controls_active_ = quick_controls_active;

    if (moved) {
        // Every transition of an on-screen surface, once. The overlays are the
        // one part of the product a screenshot cannot document -- they are
        // excluded from capture by design -- so the log is the record of what
        // was on the user's screen during a session.
        diagnostics::AppLog::info(QStringLiteral("overlay"),
                                  QStringLiteral("state=%1 recording=%2 diagnostics=%3 countdown=%4 controls=%5")
                                      .arg(static_cast<int>(state_))
                                      .arg(recording_overlay_active_)
                                      .arg(diagnostics_overlay_active_)
                                      .arg(countdown_overlay_active_)
                                      .arg(quick_controls_active_));
        emit changed();
    }
}

bool OverlayAdapter::refreshMonitorGeometry() {
    std::uintptr_t native_id = 0;
    if (source_ != nullptr) {
        const int index = source_->selected_target_index;
        if (index >= 0 && index < static_cast<int>(source_->targets.size())) {
            const recorder_core::CaptureTarget& target = source_->targets[static_cast<std::size_t>(index)];
            // Window targets have no monitor rectangle of their own here. The
            // Widgets overlays fall back to the primary screen in that case and
            // so do these -- following a window as it is dragged between
            // displays is a separate feature, not a port detail.
            if (target.kind == recorder_core::CaptureTarget::Kind::Monitor)
                native_id = target.native_id;
        }
    }

    if (native_id == geometry_native_id_)
        return false;

    geometry_native_id_ = native_id;

    QRect resolved;
    if (native_id != 0) {
        const ScreenPresentation meta = QueryScreenPresentation(native_id);
        if (meta.available && meta.width > 0 && meta.height > 0)
            resolved = QRect(meta.origin_x, meta.origin_y, meta.width, meta.height);
    }

    if (resolved == recorded_monitor_geometry_)
        return false;

    recorded_monitor_geometry_ = resolved;
    return true;
}

QRect OverlayAdapter::recordedMonitorGeometry() const noexcept {
    return recorded_monitor_geometry_;
}

int OverlayAdapter::recordingState() const noexcept {
    return static_cast<int>(state_);
}

bool OverlayAdapter::recordingOverlayActive() const noexcept {
    return recording_overlay_active_;
}

bool OverlayAdapter::countdownOverlayActive() const noexcept {
    return countdown_overlay_active_;
}

bool OverlayAdapter::diagnosticsOverlayActive() const noexcept {
    return diagnostics_overlay_active_;
}

bool OverlayAdapter::quickControlsActive() const noexcept {
    return quick_controls_active_;
}

} // namespace exosnap::quick

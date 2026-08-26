#include "RecordViewModelAdapter.h"

#include "viewmodels/RecordViewModel.h"

#include <QMetaObject>
#include <QMetaProperty>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace exosnap::quick {
namespace {

// An em dash, not a zero: a metric that has not been measured yet must not read
// as a measurement of nothing.
const QString kUnavailable = QStringLiteral("—");

QString wide(const std::wstring& value) {
    return QString::fromStdWString(value);
}

QString clockText(const QString& value) {
    const QStringList parts = value.split(QLatin1Char(':'));
    if (parts.size() == 2)
        return QStringLiteral("00:%1:%2").arg(parts.at(0).rightJustified(2, QLatin1Char('0')), parts.at(1));
    if (parts.size() == 3)
        return value;
    return QStringLiteral("00:00:00");
}

bool hasRow(const capability::AudioUiState& state, exosnap::engine::AudioSourceKind kind) {
    return std::any_of(state.source_rows.begin(), state.source_rows.end(), [kind](const auto& row) {
        return row.kind == kind || (kind == exosnap::engine::AudioSourceKind::Sys &&
                                    row.kind == exosnap::engine::AudioSourceKind::SystemOutput);
    });
}

} // namespace

RecordViewModelAdapter::RecordViewModelAdapter(const RecordViewModel* source, QObject* parent)
    : QObject(parent), source_(source) {
    // Which properties `changed()` speaks for, asked of moc rather than written
    // down. There are 49 of them today and the list is exactly the set a
    // Q_PROPERTY declaration puts in it, so a property added with
    // `NOTIFY changed` is covered the moment it is declared — a hand-maintained
    // list would go stale silently, and going stale here means a QML binding that
    // never updates.
    const QMetaObject* meta = &RecordViewModelAdapter::staticMetaObject;
    const int changed_signal = meta->indexOfSignal("changed()");
    for (int index = meta->propertyOffset(); index < meta->propertyCount(); ++index) {
        const QMetaProperty property = meta->property(index);
        if (property.hasNotifySignal() && property.notifySignalIndex() == changed_signal)
            changed_property_indices_.push_back(index);
    }
    synchronize();
}

QVariantList RecordViewModelAdapter::changedPropertySnapshot() const {
    QVariantList values;
    values.reserve(static_cast<qsizetype>(changed_property_indices_.size()));
    const QMetaObject* meta = &RecordViewModelAdapter::staticMetaObject;
    for (const int index : changed_property_indices_)
        values.push_back(meta->property(index).read(this));
    return values;
}

void RecordViewModelAdapter::publishChanged() {
    QVariantList snapshot = changedPropertySnapshot();
    if (snapshot == changed_property_values_)
        return;
    changed_property_values_ = std::move(snapshot);
    emit changed();
}

bool RecordViewModelAdapter::active() const noexcept {
    return active_;
}

void RecordViewModelAdapter::setActive(bool active) {
    if (active_ == active)
        return;
    active_ = active;
    emit activeChanged();
}

int RecordViewModelAdapter::state() const noexcept {
    return source_ != nullptr ? static_cast<int>(source_->state) : 0;
}

const QString& RecordViewModelAdapter::stateText() const noexcept {
    return state_text_;
}

// The tone names what the state MEANS, not what colour to draw — ExoStatusPill
// resolves it. Only three of them are semantic: `recording`, `error` and
// `success`. Everything else is a normal state and must not borrow caution
// amber: pausing a recording, counting down to one and writing one out are all
// things the product does on request, and a user who has been taught that amber
// means "something needs attention" learns nothing from seeing it four times a
// session. `warning` is left for a real warning to claim.
QString RecordViewModelAdapter::stateTone() const {
    if (source_ == nullptr)
        return QStringLiteral("neutral");
    switch (source_->state) {
    case UiRecordingState::Recording:
        return QStringLiteral("recording");
    // A paused session is still a session — visible at a glance, on the same
    // accent the Resume action beside it carries.
    case UiRecordingState::Paused:
        return QStringLiteral("paused");
    // Momentary transitions. They say what is happening and then stop; a quiet
    // neutral is the whole requirement.
    case UiRecordingState::Countdown:
    case UiRecordingState::Preparing:
    case UiRecordingState::RegionSelecting:
    case UiRecordingState::Stopping:
    case UiRecordingState::Saving:
        return QStringLiteral("busy");
    case UiRecordingState::Blocked:
    case UiRecordingState::Failed:
        return QStringLiteral("error");
    case UiRecordingState::Completed:
        return source_->last_succeeded ? QStringLiteral("success") : QStringLiteral("error");
    default:
        return QStringLiteral("neutral");
    }
}

QString RecordViewModelAdapter::capabilityText() const {
    return source_ != nullptr ? wide(source_->capability_status_text) : QString{};
}

QString RecordViewModelAdapter::elapsedText() const {
    return clockText(elapsed_text_);
}

const QString& RecordViewModelAdapter::outputSizeText() const noexcept {
    return output_size_text_;
}

QString RecordViewModelAdapter::bitrateText() const {
    if (source_ == nullptr || !source_->live_stats_available || source_->elapsed_seconds <= 0.0)
        return QStringLiteral("—");
    const double megabits = static_cast<double>(source_->video_bytes + source_->audio_bytes) * 8.0 / 1'000'000.0;
    return QStringLiteral("%1 Mb/s").arg(megabits / source_->elapsed_seconds, 0, 'f', 1);
}

const QString& RecordViewModelAdapter::capturedFpsText() const noexcept {
    return captured_fps_text_;
}

QString RecordViewModelAdapter::droppedFramesText() const {
    return source_ != nullptr && source_->live_stats_available ? QString::number(source_->dropped_frames)
                                                               : QStringLiteral("—");
}

QString RecordViewModelAdapter::driftText() const {
    return source_ != nullptr && source_->live_stats_available && source_->av_drift_available
               ? QStringLiteral("%1 ms").arg(source_->av_drift_ms, 0, 'f', 0)
               : QStringLiteral("—");
}

bool RecordViewModelAdapter::liveStatsAvailable() const noexcept {
    return live_stats_available_;
}

bool RecordViewModelAdapter::canStart() const noexcept {
    return source_ != nullptr && source_->CanStart() && !region_selection_needed_;
}

bool RecordViewModelAdapter::canStop() const noexcept {
    return source_ != nullptr && source_->CanStop();
}

bool RecordViewModelAdapter::canPause() const noexcept {
    return source_ != nullptr && source_->CanPause();
}

bool RecordViewModelAdapter::canResume() const noexcept {
    return source_ != nullptr && source_->CanResume();
}

bool RecordViewModelAdapter::canSelectSource() const noexcept {
    if (source_ == nullptr)
        return false;
    // QCR-V03: exhaustive on purpose, with no `default:`. The permissive answer is
    // the one that lets the user change what is being recorded, so a state added
    // later and forgotten here would fail OPEN — silently, at runtime. Without the
    // default label MSVC raises C4062 for the unhandled enumerator, and /W4 /WX
    // turns that into a build failure at the moment the state is introduced.
    switch (source_->state) {
    // The capture is committed for the session, or an overlay owns the picking.
    case UiRecordingState::Countdown:
    case UiRecordingState::Preparing:
    case UiRecordingState::RegionSelecting:
    case UiRecordingState::Recording:
    case UiRecordingState::Paused:
    case UiRecordingState::ArmedFromRecovery:
    case UiRecordingState::Stopping:
    case UiRecordingState::Saving:
        return false;
    // Nothing is in flight: the only question left is whether there is anything
    // to pick. Blocked and Failed are deliberately here — changing the source is
    // frequently the fix.
    case UiRecordingState::LoadingCapabilities:
    case UiRecordingState::Ready:
    case UiRecordingState::Blocked:
    case UiRecordingState::Completed:
    case UiRecordingState::Failed:
        return !source_->targets.empty();
    }
    return false;
}

bool RecordViewModelAdapter::recording() const noexcept {
    return source_ != nullptr && source_->state == UiRecordingState::Recording;
}

bool RecordViewModelAdapter::paused() const noexcept {
    return source_ != nullptr &&
           (source_->state == UiRecordingState::Paused || source_->state == UiRecordingState::ArmedFromRecovery);
}

bool RecordViewModelAdapter::countdownActive() const noexcept {
    return source_ != nullptr && source_->state == UiRecordingState::Countdown;
}

bool RecordViewModelAdapter::preparing() const noexcept {
    return source_ != nullptr && source_->state == UiRecordingState::Preparing;
}

bool RecordViewModelAdapter::finalizing() const noexcept {
    return source_ != nullptr &&
           (source_->state == UiRecordingState::Stopping || source_->state == UiRecordingState::Saving);
}

qreal RecordViewModelAdapter::savingProgress() const noexcept {
    return saving_progress_;
}

void RecordViewModelAdapter::setSavingProgress(float fraction) {
    // The coordinator posts -1 to mark the remux STARTING, before any packet has
    // been counted. Treating that as 0 % would put a bar at zero and leave it
    // there for however long the first packet takes; treating it as "unknown"
    // keeps the label honest until there is something to report.
    const qreal next = fraction < 0.0f ? -1.0 : static_cast<qreal>(std::clamp(fraction, 0.0f, 1.0f));
    if (qFuzzyCompare(next, saving_progress_))
        return;
    saving_progress_ = next;
    emit savingProgressChanged();
}

bool RecordViewModelAdapter::blocked() const noexcept {
    return source_ != nullptr && source_->state == UiRecordingState::Blocked;
}

bool RecordViewModelAdapter::failed() const noexcept {
    return source_ != nullptr && source_->state == UiRecordingState::Failed;
}

const QVariantList& RecordViewModelAdapter::targetOptions() const noexcept {
    return target_options_;
}

const QVariantList& RecordViewModelAdapter::displayTargetOptions() const noexcept {
    return display_target_options_;
}

const QVariantList& RecordViewModelAdapter::windowTargetOptions() const noexcept {
    return window_target_options_;
}

int RecordViewModelAdapter::selectedTargetIndex() const noexcept {
    return source_ != nullptr ? source_->selected_target_index : -1;
}

int RecordViewModelAdapter::captureMode() const noexcept {
    return source_ != nullptr ? static_cast<int>(source_->capture_mode) : 0;
}

const QString& RecordViewModelAdapter::sourceName() const noexcept {
    return source_name_;
}

const QString& RecordViewModelAdapter::sourceKindText() const noexcept {
    return source_kind_text_;
}

const QString& RecordViewModelAdapter::sourceDetailText() const noexcept {
    return source_detail_text_;
}

const QString& RecordViewModelAdapter::formatText() const noexcept {
    return format_text_;
}

QRectF RecordViewModelAdapter::normalizedSourceRect() const noexcept {
    return normalized_source_rect_;
}

bool RecordViewModelAdapter::regionSelectionNeeded() const noexcept {
    return region_selection_needed_;
}

bool RecordViewModelAdapter::systemAudioEnabled() const noexcept {
    return source_ != nullptr && source_->audio_ui_state.IsSysEnabled();
}

bool RecordViewModelAdapter::appAudioEnabled() const noexcept {
    return source_ != nullptr && source_->audio_ui_state.IsAppEnabled();
}

bool RecordViewModelAdapter::microphoneEnabled() const noexcept {
    return source_ != nullptr && source_->audio_ui_state.IsMicEnabled();
}

bool RecordViewModelAdapter::webcamEnabled() const noexcept {
    return webcam_enabled_;
}

bool RecordViewModelAdapter::appAudioVisible() const noexcept {
    return source_ != nullptr && source_->audio_ui_state.target_kind == capability::CaptureTargetKind::Window &&
           hasRow(source_->audio_ui_state, exosnap::engine::AudioSourceKind::App);
}

const QStringList& RecordViewModelAdapter::liveToggleableSources() const noexcept {
    return live_toggleable_sources_;
}

void RecordViewModelAdapter::setLiveToggleableSources(QStringList keys) {
    if (live_toggleable_sources_ == keys) {
        return;
    }
    live_toggleable_sources_ = std::move(keys);
    emit changed();
}

bool RecordViewModelAdapter::microphoneAvailable() const noexcept {
    return microphone_available_;
}

bool RecordViewModelAdapter::webcamAvailable() const noexcept {
    return webcam_available_;
}

bool RecordViewModelAdapter::webcamError() const noexcept {
    return webcam_enabled_ && !webcam_error_text_.isEmpty();
}

const QString& RecordViewModelAdapter::webcamErrorText() const noexcept {
    return webcam_error_text_;
}

const QString& RecordViewModelAdapter::webcamFrameSource() const noexcept {
    return webcam_frame_source_;
}

QRectF RecordViewModelAdapter::webcamOverlayRect() const noexcept {
    return webcam_overlay_rect_;
}

bool RecordViewModelAdapter::webcamOverlayEditable() const noexcept {
    return source_ != nullptr && IsWebcamOverlayEditable(source_->state);
}

bool RecordViewModelAdapter::webcamMirror() const noexcept {
    return webcam_mirror_;
}

double RecordViewModelAdapter::webcamOpacity() const noexcept {
    return webcam_opacity_;
}

double RecordViewModelAdapter::systemMeter() const noexcept {
    return system_meter_;
}

double RecordViewModelAdapter::appMeter() const noexcept {
    return app_meter_;
}

double RecordViewModelAdapter::microphoneMeter() const noexcept {
    return microphone_meter_;
}

int RecordViewModelAdapter::countdownSeconds() const noexcept {
    return countdown_seconds_;
}

int RecordViewModelAdapter::countdownRemaining() const noexcept {
    return countdown_remaining_;
}

double RecordViewModelAdapter::countdownProgress() const noexcept {
    return countdown_progress_;
}

bool RecordViewModelAdapter::captureFrameEnabled() const noexcept {
    return recording() || paused() ||
           (source_ != nullptr && source_->state == UiRecordingState::Ready && preview_frame_ready_);
}

bool RecordViewModelAdapter::splitEnabled() const noexcept {
    return split_enabled_ && (recording() || paused());
}

const QString& RecordViewModelAdapter::noticeText() const noexcept {
    return notice_text_;
}

const QString& RecordViewModelAdapter::noticeTone() const noexcept {
    return notice_tone_;
}

QString RecordViewModelAdapter::resultText() const {
    if (source_ == nullptr || !source_->HasResult())
        return {};
    if (source_->last_succeeded)
        return source_->result_destination_text.empty() ? wide(source_->result_status_text)
                                                        : wide(source_->result_destination_text);
    return source_->result_user_message.empty() ? wide(source_->result_error_detail)
                                                : wide(source_->result_user_message);
}

bool RecordViewModelAdapter::canOpenEditor() const noexcept {
    return source_ != nullptr && source_->last_succeeded && AllowsEditorEntry(source_->state) &&
           CanOpenInEditor(source_->current_completed_recording);
}

void RecordViewModelAdapter::setSource(const RecordViewModel* source) {
    source_ = source;
    // Revisions are per view model, so one source's stamp says nothing about the
    // next one's — a new source always rebuilds.
    target_options_revision_.reset();
    synchronize();
}

void RecordViewModelAdapter::setFormatText(QString text) {
    if (format_text_ == text)
        return;
    format_text_ = std::move(text);
    emit changed();
}

void RecordViewModelAdapter::setDeviceState(bool microphone_available, bool webcam_available, bool webcam_enabled,
                                            QString webcam_error) {
    if (microphone_available_ == microphone_available && webcam_available_ == webcam_available &&
        webcam_enabled_ == webcam_enabled && webcam_error_text_ == webcam_error) {
        return;
    }
    microphone_available_ = microphone_available;
    webcam_available_ = webcam_available;
    webcam_enabled_ = webcam_enabled;
    webcam_error_text_ = std::move(webcam_error);
    emit changed();
}

void RecordViewModelAdapter::setWebcamPresentation(QRectF overlay_rect, bool mirror, double opacity) {
    overlay_rect = overlay_rect.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    opacity = std::clamp(opacity, 0.0, 1.0);
    if (webcam_overlay_rect_ == overlay_rect && webcam_mirror_ == mirror && qFuzzyCompare(webcam_opacity_, opacity))
        return;
    webcam_overlay_rect_ = overlay_rect;
    webcam_mirror_ = mirror;
    webcam_opacity_ = opacity;
    emit changed();
}

void RecordViewModelAdapter::setWebcamFrameSource(QString source) {
    if (webcam_frame_source_ == source)
        return;
    webcam_frame_source_ = std::move(source);
    emit webcamFrameChanged();
}

void RecordViewModelAdapter::setCountdownState(int configured_seconds, int remaining_seconds, double progress) {
    progress = std::clamp(progress, 0.0, 1.0);
    // The progress comparison is what keeps this signalling at the coordinator's
    // tick rate rather than once a second. Comparing only the whole-second
    // fields would swallow nine of every ten updates, which is exactly the
    // rounding this property exists to avoid.
    if (countdown_seconds_ == configured_seconds && countdown_remaining_ == remaining_seconds &&
        qFuzzyCompare(countdown_progress_, progress)) {
        return;
    }
    countdown_seconds_ = configured_seconds;
    countdown_remaining_ = remaining_seconds;
    countdown_progress_ = progress;
    emit changed();
}

void RecordViewModelAdapter::setRegionState(QRectF normalized_rect, bool selection_needed) {
    normalized_rect = normalized_rect.normalized().intersected(QRectF(0.0, 0.0, 1.0, 1.0));
    if (normalized_rect.isEmpty())
        normalized_rect = QRectF(0.0, 0.0, 1.0, 1.0);
    if (normalized_source_rect_ == normalized_rect && region_selection_needed_ == selection_needed)
        return;
    normalized_source_rect_ = normalized_rect;
    region_selection_needed_ = selection_needed;
    rebuildPresentation();
    emit changed();
}

void RecordViewModelAdapter::setPreviewFrameReady(bool ready) {
    if (preview_frame_ready_ == ready)
        return;
    preview_frame_ready_ = ready;
    emit changed();
}

void RecordViewModelAdapter::setSplitEnabled(bool enabled) {
    if (split_enabled_ == enabled)
        return;
    split_enabled_ = enabled;
    emit changed();
}

void RecordViewModelAdapter::setMeters(double system, double app, double microphone) {
    system = std::clamp(system, 0.0, 1.0);
    app = std::clamp(app, 0.0, 1.0);
    microphone = std::clamp(microphone, 0.0, 1.0);
    if (qFuzzyCompare(system_meter_, system) && qFuzzyCompare(app_meter_, app) &&
        qFuzzyCompare(microphone_meter_, microphone)) {
        return;
    }
    system_meter_ = system;
    app_meter_ = app;
    microphone_meter_ = microphone;
    emit metersChanged();
}

void RecordViewModelAdapter::setNoticeText(QString text, QString tone) {
    if (notice_text_ == text && notice_tone_ == tone)
        return;
    notice_text_ = std::move(text);
    notice_tone_ = std::move(tone);
    emit changed();
}

void RecordViewModelAdapter::synchronize() {
    const QString state_text = source_ != nullptr ? wide(source_->state_text) : QString{};
    const QString elapsed_text = source_ != nullptr ? wide(source_->elapsed_text) : QString{};
    const QString output_size_text = source_ != nullptr ? wide(source_->output_size_text) : QString{};
    const bool live_stats_available = source_ != nullptr && source_->live_stats_available;
    const bool state_changed = state_text_ != state_text;
    const bool elapsed_changed = elapsed_text_ != elapsed_text;
    const bool output_changed = output_size_text_ != output_size_text;
    const bool stats_changed = live_stats_available_ != live_stats_available;

    state_text_ = state_text;
    elapsed_text_ = elapsed_text;
    output_size_text_ = output_size_text;
    live_stats_available_ = live_stats_available;
    updateCapturedFps();
    rebuildPresentation();

    if (state_changed)
        emit stateTextChanged();
    if (elapsed_changed)
        emit elapsedTextChanged();
    if (output_changed)
        emit outputSizeTextChanged();
    if (stats_changed)
        emit liveStatsAvailableChanged();

    // `changed()` used to fire here unconditionally, which is the one place in
    // this class that did not deduplicate — every other setter above compares
    // first. It reaches 49 bound properties (source name, target options, meter
    // context, webcam presentation, countdown, split state, notice text, the
    // whole transport enablement set), and this function runs on the engine's
    // stats callback at ~8 Hz while recording, at 10 Hz through a countdown, and
    // again on every preview frameReady. Almost none of those syncs moves
    // anything a `changed()` property reports.
    publishChanged();
}

void RecordViewModelAdapter::updateCapturedFps() {
    // Minimum delta window. The stats callback runs at roughly 264 ms, so this
    // is about two callbacks: short enough that a stall is visible within a
    // second, long enough that the figure does not flicker on callback jitter.
    constexpr double kWindowSeconds = 0.5;

    if (source_ == nullptr || !live_stats_available_) {
        // Between sessions there is nothing to measure. Clearing the anchor here
        // is what stops the next recording from computing its first reading
        // against the previous one's frame counter.
        fps_sample_seconds_ = -1.0;
        fps_sample_frames_ = 0;
        captured_fps_text_ = kUnavailable;
        return;
    }

    const double now_seconds = source_->elapsed_seconds;
    const quint64 now_frames = source_->frames_captured;

    if (fps_sample_seconds_ < 0.0 || now_seconds < fps_sample_seconds_ || now_frames < fps_sample_frames_) {
        // First sample of the session, or the counters went backwards (a split
        // starts a new file and a new count). Anchor and report nothing yet —
        // one point is not a rate.
        fps_sample_seconds_ = now_seconds;
        fps_sample_frames_ = now_frames;
        captured_fps_text_ = kUnavailable;
        return;
    }

    const double delta_seconds = now_seconds - fps_sample_seconds_;
    if (delta_seconds < kWindowSeconds)
        return; // Keep the previous reading until the window has filled.

    const double fps = static_cast<double>(now_frames - fps_sample_frames_) / delta_seconds;
    fps_sample_seconds_ = now_seconds;
    fps_sample_frames_ = now_frames;
    captured_fps_text_ = QString::number(fps, 'f', 0);
}

void RecordViewModelAdapter::rebuildPresentation() {
    source_name_ = QStringLiteral("No source selected");
    source_kind_text_ = QStringLiteral("SOURCE");
    source_detail_text_ = QStringLiteral("Choose a screen, window, or region.");
    if (source_ == nullptr) {
        target_options_revision_.reset();
        if (!target_options_.isEmpty()) {
            target_options_.clear();
            display_target_options_.clear();
            window_target_options_.clear();
            emit targetOptionsChanged();
        }
        return;
    }

    // The three option lists are rebuilt only when the capture-target vector was
    // actually replaced. Building them means a QVariantMap of three QStrings per
    // monitor AND per eligible top-level window, with each label produced by
    // TargetLabelFromCaptureTarget's string parsing — and it used to run on every
    // synchronize(), i.e. 8-10 times a second, only for the deep compare below to
    // throw the result away. The stamp is bumped by whoever assigns
    // RecordViewModel::targets; `nullopt` means "no list has been built for this
    // source yet", which is what makes a setSource() rebuild.
    if (!target_options_revision_.has_value() || *target_options_revision_ != source_->targets_revision) {
        target_options_revision_ = source_->targets_revision;
        QVariantList target_options;
        QVariantList display_target_options;
        QVariantList window_target_options;
        for (qsizetype index = 0; index < static_cast<qsizetype>(source_->targets.size()); ++index) {
            const auto& target = source_->targets[static_cast<std::size_t>(index)];
            const bool window = target.kind == exosnap::engine::CaptureTarget::Kind::Window;
            const QVariantMap option{
                {QStringLiteral("targetIndex"), index},
                {QStringLiteral("label"),
                 QString::fromStdString(RecordViewModel::TargetLabelFromCaptureTarget(target))},
                {QStringLiteral("kind"), window ? QStringLiteral("window") : QStringLiteral("display")},
            };
            target_options.push_back(option);
            (window ? window_target_options : display_target_options).push_back(option);
        }
        // The deep compare stays: a rescan that finds the identical set of
        // windows must not republish, because targetOptionsChanged is what makes
        // the source picker rebuild its list.
        if (target_options_ != target_options) {
            target_options_ = std::move(target_options);
            display_target_options_ = std::move(display_target_options);
            window_target_options_ = std::move(window_target_options);
            emit targetOptionsChanged();
        }
    }

    if (source_->capture_mode == CaptureMode::Region) {
        source_kind_text_ = QStringLiteral("REGION");
        source_name_ = region_selection_needed_ ? QStringLiteral("Select a region") : QStringLiteral("Screen region");
        if (source_->has_region && source_->region.IsValid()) {
            source_detail_text_ = QStringLiteral("%1 × %2 at %3, %4")
                                      .arg(source_->region.width)
                                      .arg(source_->region.height)
                                      .arg(source_->region.x)
                                      .arg(source_->region.y);
        } else {
            source_detail_text_ = QStringLiteral("Drag over the preview to choose the recorded area.");
        }
        return;
    }

    const int index = source_->selected_target_index;
    if (index < 0 || index >= static_cast<int>(source_->targets.size()))
        return;
    const auto& target = source_->targets[static_cast<std::size_t>(index)];
    source_name_ = QString::fromStdString(RecordViewModel::TargetLabelFromCaptureTarget(target));
    source_kind_text_ = target.kind == exosnap::engine::CaptureTarget::Kind::Window ? QStringLiteral("WINDOW")
                                                                                    : QStringLiteral("SCREEN");
    source_detail_text_ = QString::fromUtf8(target.description);
}

void RecordViewModelAdapter::requestStart() {
    emit startRequested();
}
void RecordViewModelAdapter::requestStop() {
    emit stopRequested();
}
void RecordViewModelAdapter::requestPause() {
    emit pauseRequested();
}
void RecordViewModelAdapter::requestResume() {
    emit resumeRequested();
}
void RecordViewModelAdapter::requestCaptureFrame() {
    emit captureFrameRequested();
}
void RecordViewModelAdapter::requestAddMarker() {
    emit addMarkerRequested();
}
void RecordViewModelAdapter::requestSplit() {
    emit splitRequested();
}
void RecordViewModelAdapter::requestSelectTarget(int target_index, int capture_mode) {
    emit selectTargetRequested(target_index, capture_mode);
}
void RecordViewModelAdapter::requestSelectRegion(QRectF normalized_rect) {
    emit selectRegionRequested(normalized_rect);
}
void RecordViewModelAdapter::requestToggleSource(const QString& key) {
    emit toggleSourceRequested(key);
}
void RecordViewModelAdapter::requestWebcamOverlayRect(QRectF normalized_rect) {
    emit webcamOverlayRectRequested(normalized_rect);
}
void RecordViewModelAdapter::requestCountdownSeconds(int seconds) {
    emit countdownSecondsRequested(seconds);
}
void RecordViewModelAdapter::requestOpenEditor() {
    emit openEditorRequested();
}
void RecordViewModelAdapter::clearNotice() {
    setNoticeText({});
}

} // namespace exosnap::quick

#include "EditExportAdapter.h"
#include <QCoreApplication>

#include "EditRenderText.h"
#include "EditSessionAdapter.h"

#include "models/EditTimelineModel.h"
#include "models/MarkerSidecar.h"
#include "services/AtomicFileOps.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QVariantMap>
#include <exosnap/engine/edit_timeline_export.h>
#include <exosnap/engine/edit_timeline_render.h>
#include <windows.h>

#include <exosnap/engine/mp4_remuxer.h>

#include <algorithm>
#include <limits>
#include <system_error>
#include <utility>

namespace exosnap::quick {
namespace {

QVariantMap option(const QString& value, const QString& label) {
    QVariantMap entry;
    entry.insert(QStringLiteral("value"), value);
    entry.insert(QStringLiteral("label"), label);
    // ExoSelect renders the capability owner's own verdict; both output choices
    // are always available here, so the reason line stays empty.
    entry.insert(QStringLiteral("selectable"), true);
    entry.insert(QStringLiteral("reason"), QString());
    return entry;
}

} // namespace

std::filesystem::path DefaultEditExportPath(const std::filesystem::path& output_directory, bool to_mp4) {
    return output_directory / (to_mp4 ? L"ExoSnap-export.mp4" : L"ExoSnap-export.mkv");
}
bool ShouldPublishExportProgress(float fraction, int last_published_percent) {
    const int percent = fraction <= 0.0f ? 0 : fraction >= 1.0f ? 100 : static_cast<int>(fraction * 100.0f);
    return percent != last_published_percent;
}

EditExportAdapter::EditExportAdapter(QObject* parent) : QObject(parent) {
    auto* timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this] {
        if (run_ && running()) {
            const int percent = run_->percent.load();
            if (percent >= 0 && percent != progress_percent_)
                publishProgress(percent);
        }
    });
    timer->start();
}

QVariantList EditExportAdapter::profileOptions() {
    QVariantList profiles{option(QStringLiteral("match"), tr("Match source"))};
    for (const auto& label :
         {tr("YouTube 1080p"), tr("YouTube 1440p"), tr("YouTube 4K"), tr("Archive / High quality")}) {
        auto profile = option(label, label);
        profile[QStringLiteral("selectable")] = false;
        profile[QStringLiteral("reason")] = tr("Only Match source is supported for render export.");
        profiles.push_back(profile);
    }
    return profiles;
}

void EditExportAdapter::setProfileKey(const QString& key) {
    if (key != QStringLiteral("match") || profile_key_ == key)
        return;
    profile_key_ = key;
    emit optionsChanged();
}

void EditExportAdapter::chooseDestination() {
    if (running())
        return;
    const QString directory =
        output_directory_ ? output_directory_() : QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    QString filter = container_key_ == QStringLiteral("mp4") ? tr("MP4 video (*.mp4)") : tr("Matroska video (*.mkv)");
    const QString path = QFileDialog::getSaveFileName(
        nullptr, tr("Export video"),
        QString::fromStdWString(DefaultEditExportPath(std::filesystem::path(directory.toStdWString()),
                                                      container_key_ == QStringLiteral("mp4"))
                                    .wstring()),
        tr("Matroska video (*.mkv);;MP4 video (*.mp4)"), &filter);
    if (path.isEmpty())
        return;
    setContainerKey(QFileInfo(path).suffix().toLower() == QStringLiteral("mp4") ? QStringLiteral("mp4")
                                                                                : QStringLiteral("mkv"));
    chosen_output_ = std::filesystem::path(path.toStdWString());
    overwrite_confirmed_ = QFileInfo::exists(path);
    startExport();
}

EditExportAdapter::~EditExportAdapter() {
    if (run_)
        run_->cancel.store(true);
}

void EditExportAdapter::setSession(EditSessionAdapter* session) {
    session_ = session;
    if (session_ == nullptr)
        return;
    connect(session_, &EditSessionAdapter::clipChanged, this, [this]() {
        // A different clip means the last run's result no longer describes it --
        // unless that run is still in flight, which must be found again as it
        // was when the surface is re-entered.
        if (!running())
            reset();
        emit optionsChanged();
    });
    connect(this, &EditExportAdapter::stateChanged, session_, [this]() { session_->setExportRunning(running()); });
}

int EditExportAdapter::stateValue() const noexcept {
    return static_cast<int>(state_);
}

EditExportAdapter::State EditExportAdapter::state() const noexcept {
    return state_;
}

bool EditExportAdapter::running() const noexcept {
    return state_ == State::Running || state_ == State::Cancelling;
}

int EditExportAdapter::progressPercent() const noexcept {
    return progress_percent_;
}

QString EditExportAdapter::outputPath() const {
    // Both getters are read by people, not by the filesystem: a path built from
    // a URL keeps its forward slashes, and the same folder then reads one way
    // here and another way in Settings.
    return QDir::toNativeSeparators(QString::fromStdWString(output_path_.wstring()));
}

QString EditExportAdapter::outputFileName() const {
    return QString::fromStdWString(output_path_.filename().wstring());
}

QString EditExportAdapter::outputFolder() const {
    return QDir::toNativeSeparators(QString::fromStdWString(output_path_.parent_path().wstring()));
}

const QString& EditExportAdapter::errorText() const noexcept {
    return error_text_;
}

bool EditExportAdapter::destinationFailure() const noexcept {
    return destination_failure_;
}

const QString& EditExportAdapter::containerKey() const noexcept {
    return container_key_;
}

void EditExportAdapter::setContainerKey(const QString& key) {
    const QString normalized = key == QStringLiteral("mp4") ? key : QStringLiteral("mkv");
    if (container_key_ == normalized)
        return;
    container_key_ = normalized;
    emit optionsChanged();
}

const QString& EditExportAdapter::saveModeKey() const noexcept {
    return save_mode_key_;
}

void EditExportAdapter::setSaveModeKey(const QString& key) {
    const QString normalized = key == QStringLiteral("overwrite") ? key : QStringLiteral("new");
    if (save_mode_key_ == normalized)
        return;
    save_mode_key_ = normalized;
    emit optionsChanged();
}

QVariantList EditExportAdapter::containerOptions() {
    return {option(QStringLiteral("mkv"), QStringLiteral("MKV")), option(QStringLiteral("mp4"), QStringLiteral("MP4"))};
}

QVariantList EditExportAdapter::saveModeOptions() {
    return {
        option(QStringLiteral("new"), QCoreApplication::translate("EditExportAdapter", "New file")),
        option(QStringLiteral("overwrite"), QCoreApplication::translate("EditExportAdapter", "Overwrite original"))};
}

bool EditExportAdapter::overwriteSelected() const {
    return save_mode_key_ == QStringLiteral("overwrite");
}

QString EditExportAdapter::destinationText() const {
    if (session_ && !session_->workspace().transitions().empty())
        return tr("Render Crossfade: SDR BT.709 video and PCM audio in Matroska. Choose an MKV destination.");
    if (overwriteSelected())
        return QCoreApplication::translate("EditExportAdapter",
                                           "Lossless stream copy\nReplaces the original recording");
    const QString extension = container_key_ == QStringLiteral("mp4") ? QStringLiteral("mp4") : QStringLiteral("mkv");
    return tr("Lossless stream copy. Choose a filename and folder (%1).").arg(extension);
}

// "Overwrite original" finishes with an atomic replace, so once it succeeds no
// copy of the original is left. The question has to come before the run starts,
// not as a report afterwards.
QString EditExportAdapter::overwritePrompt() const {
    const QString name = session_ != nullptr ? QFileInfo(session_->clipPath()).fileName() : QString();
    if (name.isEmpty())
        return QCoreApplication::translate("EditExportAdapter",
                                           "The original recording will be replaced by the exported result.\n"
                                           "The original cannot be recovered afterwards.");
    return QCoreApplication::translate("EditExportAdapter",
                                       "\xe2\x80\x9c%1\xe2\x80\x9d will be replaced by the exported result.\n"
                                       "The original cannot be recovered afterwards.")
        .arg(name);
}

bool EditExportAdapter::canExport() const noexcept {
    return !running() && state_ != State::Failed;
}

void EditExportAdapter::setState(State state) {
    if (state_ == state)
        return;
    state_ = state;
    emit stateChanged();
}

void EditExportAdapter::reset() {
    if (running())
        return;
    progress_percent_ = 0;
    error_text_.clear();
    destination_failure_ = false;
    emit progressChanged();
    emit resultChanged();
    setState(State::Options);
}

void EditExportAdapter::publishProgress(int percent) {
    if (progress_percent_ == percent)
        return;
    progress_percent_ = percent;
    emit progressChanged();
}

void EditExportAdapter::retry() {
    startExport();
}

void EditExportAdapter::retryInFolder(const QUrl& folder) {
    if (state_ != State::Failed || !destination_failure_ || !folder.isLocalFile() || output_path_.empty())
        return;
    retry_output_path_ = std::filesystem::path(folder.toLocalFile().toStdWString()) / output_path_.filename();
    startExport();
}

void EditExportAdapter::cancel() {
    if (state_ != State::Running)
        return;
    if (run_)
        run_->cancel.store(true);
    setState(State::Cancelling);
}

void EditExportAdapter::startExport() {
    if (running() || session_ == nullptr)
        return;

    error_text_.clear();
    destination_failure_ = false;
    progress_percent_ = 0;
    emit progressChanged();
    emit resultChanged();

    if (session_->workspace().clips().empty()) {
        error_text_ = QCoreApplication::translate("EditExportAdapter", "No edit master available for export.");
        emit resultChanged();
        setState(State::Failed);
        return;
    }

    const bool to_mp4 = container_key_ == QStringLiteral("mp4");
    const QString directory =
        output_directory_ ? output_directory_() : QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const auto suggested =
        std::filesystem::path(directory.toStdWString()) /
        (L"ExoSnap-" + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")).toStdWString() +
         (to_mp4 ? L".mp4" : L".mkv"));
    const std::filesystem::path output = retry_output_path_.value_or(chosen_output_.value_or(suggested));
    std::vector<engine::TimelineExportClip> recipe;
    engine::TimelineRenderSnapshot render_snapshot;
    render_snapshot.timeline = session_->workspace().timeline();
    for (const auto& asset : session_->workspace().assets())
        render_snapshot.sources.push_back({asset.id, std::filesystem::path(asset.path), {}});
    const auto render_config = render_config_ ? render_config_() : engine::RecorderConfig{};
    std::vector<RecordingMarker> timeline_markers;
    for (const auto& clip : session_->workspace().clips()) {
        const auto track = std::find_if(session_->workspace().tracks().begin(), session_->workspace().tracks().end(),
                                        [&clip](const auto& t) { return t.id == clip.track; });
        if (track->type != edit::TrackType::Video)
            continue;
        const auto* asset = session_->workspace().asset(clip.asset);
        if (asset->state != edit::AssetState::Available) {
            error_text_ = tr("The timeline contains unavailable media.");
            emit resultChanged();
            setState(State::Failed);
            return;
        }
        recipe.push_back({std::filesystem::path(asset->path), clip.source_in, clip.source_out, clip.start});
        for (const auto& marker : asset->markers) {
            const auto source_us = static_cast<int64_t>(marker.time_ms) * 1000;
            if (source_us < clip.source_in || source_us >= clip.source_out)
                continue;
            auto retimed = marker;
            retimed.time_ms = static_cast<uint64_t>((clip.start + source_us - clip.source_in) / 1000);
            timeline_markers.push_back(std::move(retimed));
        }
    }
    if (QFileInfo::exists(QString::fromStdWString(output.wstring())) && !overwrite_confirmed_) {
        error_text_ = tr("Choose a destination and confirm before replacing an existing file.");
        emit resultChanged();
        setState(State::Failed);
        return;
    }
    const bool replace_existing = overwrite_confirmed_;
    overwrite_confirmed_ = false;
    retry_output_path_.reset();
    MarkerExportPlan marker_plan = PlanMarkerSidecarForExport(output, timeline_markers);
    output_path_ = output;
    emit resultChanged();

    run_ = std::make_shared<RunState>();
    setState(State::Running);

    export_thread_ = QThread::create([run = run_, recipe = std::move(recipe),
                                      render_snapshot = std::move(render_snapshot), render_config, output, to_mp4,
                                      replace_existing, marker_plan = std::move(marker_plan)]() mutable {
        const auto temp_output = MakeDisposableSiblingStagingPath(output);

        auto progress_cb = [run](float fraction) -> bool {
            if (run->cancel.load())
                return false;
            const int percent = fraction <= 0.0f ? 0 : fraction >= 1.0f ? 100 : static_cast<int>(fraction * 100.0f);
            run->percent.store(percent);
            return true;
        };

        const auto copy_assessment = engine::AssessTimelineExport(recipe, to_mp4);
        if (!render_snapshot.timeline.crossfades.empty() ||
            copy_assessment.eligibility != engine::TimelineExportEligibility::StreamCopy)
            for (auto& source : render_snapshot.sources)
                source.metadata = engine::ProbeEditMedia(source.path);
        const auto plan = engine::ClassifyEditExport(render_snapshot, copy_assessment, to_mp4);
        exosnap::engine::RemuxResult result;
        switch (plan.path) {
        case engine::EditExportPath::StreamCopy:
            result = engine::ExportTimelineStreamCopy(recipe, temp_output, to_mp4, progress_cb);
            break;
        case engine::EditExportPath::Render:
            result = engine::RenderEditTimeline(render_snapshot, plan, render_config, temp_output, to_mp4, progress_cb);
            break;
        case engine::EditExportPath::Unsupported:
            result = engine::RemuxResult::Fail(-1, plan.reason);
            break;
        }
        bool ok = result.success;
        std::string error = result.message;

        if (ok) {
            std::error_code ec;
            if (replace_existing) {
                const auto error_code = AtomicReplaceInPlace(temp_output, output);
                if (error_code)
                    ec = std::error_code(static_cast<int>(error_code), std::system_category());
            } else {
                if (!MoveFileExW(temp_output.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
                    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
            }
            if (ec) {
                ok = false;
                error = "Failed to save output file: " + ec.message();
                if (const std::string left = exosnap::DescribeFailedStagingRemoval(temp_output); !left.empty())
                    error += " (" + left + ")";
            }
        } else {
            if (const std::string left = exosnap::DescribeFailedStagingRemoval(temp_output); !left.empty())
                error += " (" + left + ")";
        }

        if (ok)
            ApplyMarkerExportPlan(marker_plan);

        run->ok = ok;
        run->error = std::move(error);
    });
    connect(export_thread_, &QThread::finished, this, [this, run = run_, output] {
        export_thread_ = nullptr;
        finishRun(run->ok, QString::fromStdString(run->error), QString::fromStdWString(output.wstring()),
                  run->cancel.load());
        run_.reset();
    });
    connect(export_thread_, &QThread::finished, export_thread_, &QObject::deleteLater);
    export_thread_->start();
}

void EditExportAdapter::finishRun(bool ok, const QString& error, const QString& output_path, bool cancelled) {
    output_path_ = std::filesystem::path(output_path.toStdWString());

    if (cancelled && !ok) {
        // A cancelled run left nothing behind, so the panel returns to its
        // options rather than reporting a failure the user asked for.
        progress_percent_ = 0;
        error_text_.clear();
        emit progressChanged();
        emit resultChanged();
        setState(State::Options);
        return;
    }

    if (ok) {
        error_text_.clear();
        progress_percent_ = 100;
        emit progressChanged();
        emit resultChanged();
        setState(State::Done);
        emit exportCompleted(output_path);
        return;
    }

    error_text_ = error.isEmpty()
                      ? QCoreApplication::translate("EditExportAdapter", "Unknown error")
                      : EditExportAdapter::tr("Export could not complete: %1").arg(TranslateEditRenderReason(error));
    const QString lower_error = error_text_.toLower();
    destination_failure_ =
        lower_error.contains(QStringLiteral("save output")) || lower_error.contains(QStringLiteral("permission")) ||
        lower_error.contains(QStringLiteral("denied")) || lower_error.contains(QStringLiteral("disk")) ||
        lower_error.contains(QStringLiteral("space")) || lower_error.contains(QStringLiteral("directory")) ||
        lower_error.contains(QStringLiteral("path"));
    emit resultChanged();
    setState(State::Failed);
    emit exportFailed(error_text_);
}

void EditExportAdapter::applyVisualState(State state, int percent, const QString& output_path, const QString& error) {
    progress_percent_ = std::clamp(percent, 0, 100);
    output_path_ = std::filesystem::path(output_path.toStdWString());
    error_text_ = error;
    destination_failure_ = state == State::Failed;
    emit progressChanged();
    emit resultChanged();
    setState(state);
}

// The panel offers one folder action, and this is it: "explorer /select,<path>"
// opens the containing folder AND highlights the file, so it strictly contains
// what the separate "Open folder" action did.
void EditExportAdapter::revealFile() {
    const QString path = outputPath();
    if (path.isEmpty())
        return;
    // "explorer /select,<path>" highlights the file rather than opening it.
    QProcess::startDetached(QStringLiteral("explorer"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
}

} // namespace exosnap::quick

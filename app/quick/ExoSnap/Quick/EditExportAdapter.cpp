#include "EditExportAdapter.h"

#include "EditSessionAdapter.h"

#include "models/EditTimelineModel.h"
#include "models/MarkerSidecar.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QVariantMap>

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

std::filesystem::path DeriveExportOutputPath(const std::filesystem::path& original_output, bool overwrite,
                                             bool to_mp4) {
    if (overwrite)
        return original_output;
    const std::wstring extension = to_mp4 ? L".mp4" : L".mkv";
    return original_output.parent_path() / (original_output.stem().wstring() + L"_edit" + extension);
}

bool ShouldPublishExportProgress(float fraction, int last_published_percent) {
    const int percent = fraction <= 0.0f ? 0 : fraction >= 1.0f ? 100 : static_cast<int>(fraction * 100.0f);
    return percent != last_published_percent;
}

EditExportAdapter::EditExportAdapter(QObject* parent) : QObject(parent) {
}

EditExportAdapter::~EditExportAdapter() {
    // The run captures `this`, so it cannot outlive the adapter. Cancelling
    // first keeps the wait bounded by one remux packet rather than by the clip.
    export_cancel_.store(true);
    if (export_thread_.joinable())
        export_thread_.join();
}

void EditExportAdapter::setSession(EditSessionAdapter* session) {
    session_ = session;
    if (session_ == nullptr)
        return;
    connect(session_, &EditSessionAdapter::clipChanged, this, [this]() {
        // A different clip means the last run's result no longer describes it --
        // unless that run is still in flight, which must be found again as it
        // was when the surface is re-entered (ADR 0022).
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
    return {option(QStringLiteral("new"), QStringLiteral("New file")),
            option(QStringLiteral("overwrite"), QStringLiteral("Overwrite original"))};
}

bool EditExportAdapter::overwriteSelected() const {
    return save_mode_key_ == QStringLiteral("overwrite");
}

QString EditExportAdapter::destinationText() const {
    if (overwriteSelected())
        return QStringLiteral("Lossless stream copy\nReplaces the original recording");
    const QString extension = container_key_ == QStringLiteral("mp4") ? QStringLiteral("mp4") : QStringLiteral("mkv");
    return QStringLiteral("Lossless stream copy\nNew file beside the original (\xe2\x80\xa6_edit.%1)").arg(extension);
}

// "Overwrite original" finishes with an atomic replace, so once it succeeds no
// copy of the original is left. The question has to come before the run starts,
// not as a report afterwards.
QString EditExportAdapter::overwritePrompt() const {
    const QString name = session_ != nullptr ? QFileInfo(session_->clipPath()).fileName() : QString();
    if (name.isEmpty())
        return QStringLiteral("The original recording will be replaced by the exported result.\n"
                              "The original cannot be recovered afterwards.");
    return QStringLiteral("\xe2\x80\x9c%1\xe2\x80\x9d will be replaced by the exported result.\n"
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

// Cancel does NOT declare the run finished: the remux thread is still winding
// down and the join belongs to its own completion, not to the next run.
void EditExportAdapter::cancel() {
    if (state_ != State::Running)
        return;
    export_cancel_.store(true);
    setState(State::Cancelling);
}

void EditExportAdapter::startExport() {
    if (running() || session_ == nullptr)
        return;

    const EditContext& context = session_->editContext();
    error_text_.clear();
    destination_failure_ = false;
    progress_percent_ = 0;
    last_published_percent_ = -1;
    emit progressChanged();
    emit resultChanged();

    if (context.mkv_master_path.isEmpty()) {
        error_text_ = QStringLiteral("No edit master available for export.");
        emit resultChanged();
        setState(State::Failed);
        return;
    }

    const bool to_mp4 = container_key_ == QStringLiteral("mp4");
    const std::filesystem::path master(context.mkv_master_path.toStdWString());
    const std::filesystem::path output = retry_output_path_.value_or(
        DeriveExportOutputPath(std::filesystem::path(context.output_path.toStdWString()), overwriteSelected(), to_mp4));
    retry_output_path_.reset();

    exosnap::engine::TrimRange trim;
    trim.start_us = session_->trimStartUs();
    trim.end_us = session_->trimEndUs();

    // Markers ride along as a retimed JSON sidecar -- never as container
    // chapters. Planned here, on the GUI thread, so the export thread races
    // nothing.
    const qint64 window_start_ms = trim.start_us != exosnap::engine::TrimRange::kNoTimestamp ? trim.start_us / 1000 : 0;
    const qint64 window_end_ms = trim.end_us != exosnap::engine::TrimRange::kNoTimestamp
                                     ? trim.end_us / 1000
                                     : std::numeric_limits<qint64>::max();
    MarkerExportPlan marker_plan =
        PlanMarkerSidecarForExport(output, RetimeMarkersForTrim(session_->markers(), window_start_ms, window_end_ms));

    output_path_ = output;
    emit resultChanged();

    // A previous run is always joined by its own completion handler, so the
    // handle here is never joinable at this point. Kept as a defensive join
    // rather than an assert because a detached thread would outlive `this`.
    if (export_thread_.joinable())
        export_thread_.join();
    export_cancel_.store(false);
    setState(State::Running);

    export_thread_ = std::thread([this, master, output, to_mp4, trim, marker_plan = std::move(marker_plan)]() {
        std::filesystem::path temp_output = output;
        temp_output += L".tmp";

        auto progress_cb = [this](float fraction) -> bool {
            if (export_cancel_.load())
                return false;
            if (!ShouldPublishExportProgress(fraction, last_published_percent_))
                return true;
            const int percent = fraction <= 0.0f ? 0 : fraction >= 1.0f ? 100 : static_cast<int>(fraction * 100.0f);
            last_published_percent_ = percent;
            QMetaObject::invokeMethod(this, [this, percent]() { publishProgress(percent); }, Qt::QueuedConnection);
            return true;
        };

        exosnap::engine::RemuxResult result =
            to_mp4 ? exosnap::engine::RemuxToProgressiveMp4(master, temp_output, progress_cb, trim)
                   : exosnap::engine::RemuxToMkv(master, temp_output, progress_cb, trim);

        bool ok = result.success;
        std::string error = result.message;

        if (ok) {
            // Atomic replace: rename temp -> final (same volume = atomic on NTFS).
            std::error_code ec;
            std::filesystem::rename(temp_output, output, ec);
            if (ec) {
                ok = false;
                error = "Failed to save output file: " + ec.message();
                std::error_code remove_ec;
                std::filesystem::remove(temp_output, remove_ec);
            }
        } else {
            std::error_code remove_ec;
            std::filesystem::remove(temp_output, remove_ec);
        }

        if (ok)
            ApplyMarkerExportPlan(marker_plan);

        const bool cancelled = export_cancel_.load();
        QMetaObject::invokeMethod(
            this,
            [this, ok, error, output, cancelled]() {
                finishRun(ok, QString::fromStdString(error), QString::fromStdWString(output.wstring()), cancelled);
            },
            Qt::QueuedConnection);
    });
}

// Runs on the GUI thread once the export thread has posted its result. The join
// here is what keeps `exportRunning` honest: the flag only drops after the
// thread is actually gone, so a Retry can never land inside a join.
void EditExportAdapter::finishRun(bool ok, const QString& error, const QString& output_path, bool cancelled) {
    if (export_thread_.joinable())
        export_thread_.join();

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

    error_text_ = error.isEmpty() ? QStringLiteral("Unknown error") : error;
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

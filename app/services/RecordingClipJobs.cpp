#include "RecordingClipJobs.h"

#include "../diagnostics/AppLog.h"
#include "../diagnostics/DiskSpaceProvider.h"
#include "../diagnostics/DiskSpaceThresholds.h"
#include "../models/MarkerSidecar.h"
#include "AtomicFileOps.h"

#include <exosnap/engine/edit_timeline_export.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace exosnap {
namespace {

engine::EngineError ClipFailure(engine::EngineErrorCode code, std::string context) {
    engine::EngineError error;
    error.owner = engine::EngineModule::Clip;
    error.code = code;
    error.context = std::move(context);
    return error;
}

bool Terminal(RecordingClipPhase phase) {
    return phase == RecordingClipPhase::Completed || phase == RecordingClipPhase::Cancelled ||
           phase == RecordingClipPhase::Failed;
}

uint64_t SaturatingAdd(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

} // namespace

struct RecordingClipJobs::State {
    struct Job {
        RecordingClipJobSnapshot view;
        RecordingClipRequest request;
        std::optional<engine::ClipBoundaryPlan> plan;
        std::vector<RecordingArtifactRegistry::SourceLease> leases;
        std::vector<engine::ClipPinnedSource> sources;
        std::atomic<bool> cancelled{false};
        std::filesystem::path staging;
        std::filesystem::path staging_directory;
        bool owns_staging_file = false;
        bool storage_lost = false;
        std::chrono::steady_clock::time_point last_progress;
        std::chrono::steady_clock::time_point last_space_check;
    };
    struct Work {
        std::shared_ptr<Job> job;
        bool run = false;
    };

    RecordingArtifactRegistry& registry;
    ChangedCallback changed;
    PausePredicate pause_background;
    ReserveProvider primary_reserve;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::shared_ptr<Job>> jobs;
    std::deque<Work> work;
    bool closing = false;
    uint64_t next_generation = 1;
    std::jthread worker;
    std::shared_ptr<BackgroundStorageBudget> storage;

    State(RecordingArtifactRegistry& artifacts, ChangedCallback callback, PausePredicate pause, ReserveProvider reserve,
          std::shared_ptr<BackgroundStorageBudget> budget)
        : registry(artifacts), changed(std::move(callback)), pause_background(std::move(pause)),
          primary_reserve(std::move(reserve)),
          storage(budget ? std::move(budget) : std::make_shared<BackgroundStorageBudget>()) {
    }

    void Notify(const RecordingClipJobSnapshot& view) {
        if (changed)
            changed(view);
    }

    template <typename Mutation> void Update(Job& job, Mutation mutate, bool notify = true) {
        RecordingClipJobSnapshot snapshot;
        {
            std::lock_guard lock(mutex);
            mutate(job.view);
            if (notify)
                snapshot = job.view;
        }
        if (notify)
            Notify(snapshot);
    }

    void Fail(Job& job, engine::EngineError error) {
        if (job.storage_lost && !job.cancelled.load())
            error = ClipFailure(engine::EngineErrorCode::StorageInsufficient,
                                "Clip storage reservation is no longer available");
        error.job_id = job.view.job_id;
        error.generation = job.view.generation;
        Update(job, [&](auto& view) {
            view.phase = job.cancelled.load() ? RecordingClipPhase::Cancelled : RecordingClipPhase::Failed;
            if (view.phase == RecordingClipPhase::Cancelled)
                error.code = engine::EngineErrorCode::Cancelled;
            view.error = std::move(error);
            view.reserved_bytes = 0;
        });
    }

    bool Budget(Job& job, uint64_t bytes) {
        while (!job.cancelled.load()) {
            if (!pause_background || !pause_background())
                return storage->Acquire(bytes, &job.cancelled);
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::milliseconds(20), [&] { return closing || job.cancelled.load(); });
        }
        return false;
    }

    bool SpaceAvailable(Job& job) {
        uint64_t required = 0;
        {
            std::lock_guard lock(mutex);
            for (const auto& item : jobs)
                if (!Terminal(item->view.phase)) {
                    const auto remaining = item->view.reserved_bytes > item->view.staging_bytes_written
                                               ? item->view.reserved_bytes - item->view.staging_bytes_written
                                               : 0;
                    required = SaturatingAdd(required, remaining);
                }
        }
        required = SaturatingAdd(required, diagnostics::kHardStopFreeBytes);
        if (primary_reserve)
            required = SaturatingAdd(required, primary_reserve());
        diagnostics::Win32DiskSpaceProvider disk;
        const auto free = disk.FreeBytesForPath(job.request.destination.parent_path());
        const bool available = free && *free > required;
        job.storage_lost = !available;
        return available;
    }

    void ClearInputs(Job& job) {
        job.sources.clear();
        job.leases.clear();
        job.plan.reset();
        job.request = {};
    }

    void Plan(Job& job) {
        if (!Budget(job, 0)) {
            Fail(job, ClipFailure(engine::EngineErrorCode::Cancelled, "Clip planning cancelled"));
            return;
        }
        auto planned =
            engine::PlanClipBoundaries(job.request.snapshot, job.view.recording_id, job.request.requested_begin_r_ns,
                                       job.request.requested_end_r_ns, job.request.closed_segments,
                                       [&](uint64_t bytes) { return Budget(job, bytes); });
        if (!planned.succeeded()) {
            Fail(job, *planned.error());
            return;
        }
        job.plan = std::move(*planned.value());
        std::vector<std::shared_ptr<const engine::RecordingMediaDescriptor>> descriptors(job.plan->segments.size());
        uint64_t source_metadata_bytes = 0;
        bool metadata_budget_exceeded = false;
        const auto visited = engine::VisitRecordingClipDescriptors(
            job.request.snapshot,
            [&](const auto& descriptor) {
                for (size_t index = 0; index < job.plan->segments.size(); ++index) {
                    const auto& segment = job.plan->segments[index];
                    if (segment.segment_id == descriptor.segment_id &&
                        segment.writer_generation == descriptor.writer_generation &&
                        segment.descriptor_revision == descriptor.descriptor_revision) {
                        const auto bytes = engine::EstimateRecordingClipDescriptorBytes(descriptor);
                        source_metadata_bytes = SaturatingAdd(source_metadata_bytes, SaturatingAdd(bytes, bytes));
                        if (source_metadata_bytes > 2ULL * 1024 * 1024) {
                            metadata_budget_exceeded = true;
                            return false;
                        }
                        descriptors[index] = std::make_shared<const engine::RecordingMediaDescriptor>(descriptor);
                    }
                }
                return !job.cancelled.load();
            },
            [&](uint64_t bytes) { return Budget(job, bytes); });
        if (metadata_budget_exceeded) {
            Fail(job, ClipFailure(engine::EngineErrorCode::ResourceBudgetExceeded,
                                  "Clip source metadata exceeds its working-buffer budget"));
            return;
        }
        if (!visited.succeeded()) {
            Fail(job, *visited.error());
            return;
        }
        uint64_t reservation = 32ULL * 1024 * 1024;
        for (size_t index = 0; index < descriptors.size(); ++index) {
            const auto& descriptor = descriptors[index];
            const auto& segment = job.plan->segments[index];
            if (!descriptor) {
                Fail(job, ClipFailure(engine::EngineErrorCode::SourceIntegrityFailure,
                                      "Frozen source descriptor is missing"));
                return;
            }
            for (const auto& track : descriptor->audio_tracks)
                if (track.codec.codec_id != "A_OPUS" && track.codec.codec_id != "A_PCM/INT/LIT") {
                    Fail(job, ClipFailure(engine::EngineErrorCode::UnsupportedFormat,
                                          "This audio profile has no qualified recording clip decoder-start contract"));
                    return;
                }
            const RecordingArtifactKey key{descriptor->recording_id, descriptor->segment_id,
                                           descriptor->writer_generation, descriptor->descriptor_revision};
            const bool closed = std::any_of(job.request.closed_segments.begin(), job.request.closed_segments.end(),
                                            [&](const auto& seal) {
                                                return seal.segment_id == descriptor->segment_id &&
                                                       seal.writer_generation == descriptor->writer_generation;
                                            });
            auto lease = registry.AcquireSourceLease(key, descriptor->file_identity, {}, segment.readable_end_offset);
            if (!lease) {
                (void)registry.RegisterSource(*descriptor, !closed);
                lease = registry.AcquireSourceLease(key, descriptor->file_identity, {}, segment.readable_end_offset);
            }
            if (!lease) {
                Fail(job, ClipFailure(engine::EngineErrorCode::SourceIntegrityFailure,
                                      "Source identity could not be leased"));
                return;
            }
            job.sources.push_back(
                {descriptor, lease->handle(),
                 descriptor->audio_manifest ? std::optional(*descriptor->audio_manifest) : std::nullopt});
            job.leases.push_back(std::move(*lease));
            reservation = SaturatingAdd(reservation,
                                        SaturatingAdd(segment.readable_end_offset, segment.readable_end_offset / 10));
        }
        Update(
            job,
            [&](auto& view) {
                view.reserved_bytes = reservation;
                view.actual_begin_r_ns = job.plan->actual_begin_r_ns;
                view.actual_end_r_ns = job.plan->actual_end_r_ns;
                for (const auto& track : descriptors.front()->audio_tracks)
                    view.track_names.push_back(track.name);
            },
            false);
        if (job.cancelled.load() || !SpaceAvailable(job)) {
            Fail(job, ClipFailure(engine::EngineErrorCode::StorageInsufficient,
                                  "Clip would consume the recording storage reserve"));
            return;
        }
        Update(job, [&](auto& view) {
            if (job.cancelled.load()) {
                view.phase = RecordingClipPhase::Cancelled;
                view.reserved_bytes = 0;
            } else {
                view.phase = RecordingClipPhase::Review;
                view.review_reason =
                    QT_TRANSLATE_NOOP("RecordingClipAdapter",
                                      "MKV preserves every recorded track. Copy includes only proven closed GOPs; "
                                      "the recording continues independently.");
            }
        });
    }

    void Run(Job& job) {
        if (job.cancelled.load() || !SpaceAvailable(job)) {
            Fail(job, ClipFailure(engine::EngineErrorCode::StorageInsufficient,
                                  "Clip storage reservation is no longer available"));
            return;
        }
        const auto directory = MakeDisposableSiblingStagingPath(job.request.destination);
        if (!CreateDirectoryW(directory.c_str(), nullptr)) {
            auto error =
                ClipFailure(engine::EngineErrorCode::UnavailableSource, "Private clip staging could not be reserved");
            error.win32_code = GetLastError();
            Fail(job, std::move(error));
            return;
        }
        job.staging_directory = directory;
        job.staging = directory / L"clip.mkv";
        engine::ClipJobIoControl control;
        control.on_staging_created = [&] { job.owns_staging_file = true; };
        control.before_io = [&](engine::ClipJobIoPhase, uint64_t bytes) {
            if (!Budget(job, bytes))
                return false;
            const auto now = std::chrono::steady_clock::now();
            if (now - job.last_space_check >= std::chrono::seconds(1)) {
                job.last_space_check = now;
                if (!SpaceAvailable(job))
                    return false;
            }
            return !job.cancelled.load();
        };
        control.on_progress = [&](engine::ClipJobIoPhase phase, uint64_t read, uint64_t written) {
            const auto now = std::chrono::steady_clock::now();
            const bool notify = now - job.last_progress >= std::chrono::milliseconds(100);
            if (notify)
                job.last_progress = now;
            Update(
                job,
                [&](auto& view) {
                    view.source_bytes_read = read;
                    view.staging_bytes_written = written;
                    view.phase = job.cancelled.load()                        ? RecordingClipPhase::Cancelling
                                 : phase == engine::ClipJobIoPhase::Reading  ? RecordingClipPhase::Reading
                                 : phase == engine::ClipJobIoPhase::Remuxing ? RecordingClipPhase::Remuxing
                                                                             : RecordingClipPhase::Finalizing;
                },
                notify);
        };
        auto draft = engine::RunRecordingClipJob(*job.plan, job.request.snapshot, job.sources, job.staging,
                                                 &job.cancelled, &control);
        if (!draft.succeeded()) {
            Fail(job, *draft.error());
            return;
        }
        Update(job, [](auto& view) { view.phase = RecordingClipPhase::VerifyingMetadata; });
        const auto continue_hash = [&] { return !job.cancelled.load() && SpaceAvailable(job); };
        const engine::BackgroundIoAdmission metadata_io = [&](uint64_t bytes, const std::atomic<bool>*) {
            return Budget(job, bytes);
        };
        engine::AudioManifest1 manifest;
        if (draft.value()->output_manifest)
            manifest = *draft.value()->output_manifest;
        else {
            manifest.media_id = engine::NewId<engine::MediaId>();
            manifest.recording_id = job.view.recording_id;
            manifest.segment_id = engine::NewId<engine::SegmentId>();
            for (const auto& source : job.sources)
                manifest.derived_from_media_ids.push_back(source.descriptor->media_id);
        }
        std::vector<RecordingMarker> markers;
        for (const auto& marker : job.request.markers) {
            if (marker.time_ms > static_cast<uint64_t>(INT64_MAX) / 1000000)
                continue;
            const auto marker_r_ns = static_cast<int64_t>(marker.time_ms * 1000000);
            if (marker_r_ns >= job.plan->actual_begin_r_ns && marker_r_ns < job.plan->actual_end_r_ns) {
                auto rebased = marker;
                rebased.time_ms = static_cast<uint64_t>(marker_r_ns - job.plan->actual_begin_r_ns) / 1000000;
                markers.push_back(std::move(rebased));
            }
        }
        engine::MediaIdentitySidecarReferences references;
        if (!markers.empty()) {
            const auto marker_path = DeriveMarkerSidecarPath(job.staging);
            const auto marker_bytes =
                SerializeMarkerSidecar(markers, QString::fromStdWString(job.request.destination.filename().wstring()))
                    .toJson(QJsonDocument::Indented);
            if (marker_bytes.size() > 2 * 1024 * 1024 || !Budget(job, static_cast<uint64_t>(marker_bytes.size()))) {
                Fail(job, ClipFailure(engine::EngineErrorCode::InvalidConfig,
                                      "Clip marker metadata exceeds its I/O budget"));
                return;
            }
            if (!WriteMarkerSidecar(marker_path, markers,
                                    QString::fromStdWString(job.request.destination.filename().wstring()))) {
                Fail(job, ClipFailure(engine::EngineErrorCode::MetadataIncomplete, "Clip markers could not be staged"));
                return;
            }
            references.marker_refs.push_back(DeriveMarkerSidecarPath(job.request.destination).filename().string());
        }
        QJsonArray ranges;
        for (const auto& source_range : draft.value()->source_ranges) {
            QJsonObject range;
            range.insert(QStringLiteral("recording_id"),
                         QString::fromStdString(engine::ToUuid(source_range.recording_id)));
            range.insert(QStringLiteral("segment_id"), QString::fromStdString(engine::ToUuid(source_range.segment_id)));
            range.insert(QStringLiteral("media_id"), QString::fromStdString(engine::ToUuid(source_range.media_id)));
            range.insert(QStringLiteral("begin_r_ns"), QString::number(source_range.begin_r_ns));
            range.insert(QStringLiteral("end_r_ns"), QString::number(source_range.end_r_ns));
            ranges.append(range);
        }
        references.source_ranges_json = QJsonDocument(ranges).toJson(QJsonDocument::Compact).toStdString();
        const auto metadata_written = engine::WriteMediaIdentitySidecar(job.staging, manifest, continue_hash,
                                                                        references, metadata_io, &job.cancelled);
        if (!metadata_written.succeeded()) {
            Fail(job, *metadata_written.error());
            return;
        }
        const auto verified = engine::ProbeEditMedia(job.staging, continue_hash, metadata_io, &job.cancelled);
        if (!verified.error.empty() || !verified.content_identity_valid ||
            verified.audio_metadata.status != engine::AudioMetadataStatus::Verified ||
            !verified.audio_metadata.manifest || verified.audio_metadata.manifest->media_id != manifest.media_id ||
            verified.audio_metadata.actual_tracks.size() != manifest.tracks.size() ||
            verified.audio_metadata.references.source_ranges_json != references.source_ranges_json ||
            verified.audio_metadata.references.marker_refs != references.marker_refs) {
            Fail(job, ClipFailure(engine::EngineErrorCode::MetadataIncomplete, "Staged clip metadata did not verify"));
            return;
        }
        if (job.cancelled.load() || !SpaceAvailable(job)) {
            Fail(job, ClipFailure(engine::EngineErrorCode::StorageInsufficient,
                                  "Clip publication cancelled or lacks storage reserve"));
            return;
        }
        bool may_publish = false;
        Update(job, [&](auto& view) {
            if (!job.cancelled.load()) {
                view.phase = RecordingClipPhase::Publishing;
                may_publish = true;
            }
        });
        if (!may_publish) {
            Fail(job, ClipFailure(engine::EngineErrorCode::Cancelled, "Clip publication cancelled"));
            return;
        }
        const auto published = PublishMediaAndIdentitySidecar(job.staging, job.request.destination, false);
        if (published != ERROR_SUCCESS) {
            auto error = ClipFailure(engine::EngineErrorCode::MetadataIncomplete,
                                     "Clip publication failed; recording remains intact");
            error.win32_code = published;
            Fail(job, std::move(error));
            return;
        }
        CompletedRecording completed;
        completed.file_path = QString::fromStdWString(job.request.destination.wstring());
        completed.display_name = QString::fromStdWString(job.request.destination.filename().wstring());
        completed.file_size_bytes = static_cast<qint64>(verified.file_size);
        completed.duration_seconds = static_cast<double>(job.plan->actual_end_r_ns - job.plan->actual_begin_r_ns) / 1e9;
        completed.output_width = job.sources.front().descriptor->video_width;
        completed.output_height = job.sources.front().descriptor->video_height;
        completed.frame_rate_num = job.sources.front().descriptor->frame_rate_num;
        completed.frame_rate_den = job.sources.front().descriptor->frame_rate_den;
        completed.container = engine::Container::Matroska;
        completed.cfr = job.request.cfr;
        const auto& video_codec = job.sources.front().descriptor->video.codec_id;
        completed.video_codec = video_codec == "V_MPEG4/ISO/AVC"    ? engine::VideoCodec::H264
                                : video_codec == "V_MPEGH/ISO/HEVC" ? engine::VideoCodec::Hevc
                                                                    : engine::VideoCodec::Av1;
        if (!manifest.tracks.empty())
            completed.audio_codec = manifest.tracks.front().container.codec.codec_id == "A_OPUS"
                                        ? engine::AudioCodec::Opus
                                        : engine::AudioCodec::Pcm;
        completed.completed_at = QDateTime::currentDateTimeUtc();
        completed.succeeded = true;
        completed.markers = std::move(markers);
        if (!completed.markers.empty())
            completed.marker_sidecar_path =
                QString::fromStdWString(DeriveMarkerSidecarPath(job.request.destination).wstring());
        Update(job, [&](auto& view) {
            view.phase = RecordingClipPhase::Completed;
            view.asset_id = manifest.media_id;
            view.completed = std::move(completed);
            view.reserved_bytes = 0;
        });
    }

    void Loop() {
        for (;;) {
            Work next;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return closing || !work.empty(); });
                if (work.empty()) {
                    if (closing)
                        return;
                    continue;
                }
                next = std::move(work.front());
                work.pop_front();
            }
            auto& job = *next.job;
            try {
                if (job.cancelled.load())
                    Fail(job, ClipFailure(engine::EngineErrorCode::Cancelled, "Clip cancelled"));
                else if (next.run)
                    Run(job);
                else
                    Plan(job);
            } catch (const std::exception& exception) {
                Fail(job, ClipFailure(engine::EngineErrorCode::SourceIntegrityFailure, exception.what()));
            } catch (...) {
                Fail(job, ClipFailure(engine::EngineErrorCode::SourceIntegrityFailure, "Unexpected clip failure"));
            }
            bool terminal = false;
            {
                std::lock_guard lock(mutex);
                terminal = Terminal(job.view.phase);
            }
            if (terminal) {
                if (job.owns_staging_file) {
                    const auto cleanup = DescribeFailedStagingRemoval(job.staging);
                    if (!cleanup.empty())
                        diagnostics::AppLog::warning(QStringLiteral("clip"), QString::fromStdString(cleanup));
                }
                if (!job.staging_directory.empty() && !RemoveDirectoryW(job.staging_directory.c_str()))
                    diagnostics::AppLog::warning(
                        QStringLiteral("clip"),
                        QStringLiteral("Private clip staging remains: %1 (error %2)")
                            .arg(QString::fromStdWString(job.staging_directory.filename().wstring()))
                            .arg(GetLastError()));
                ClearInputs(job);
            }
        }
    }
};

RecordingClipJobs::RecordingClipJobs(RecordingArtifactRegistry& registry, ChangedCallback changed,
                                     PausePredicate pause_background, ReserveProvider primary_reserve,
                                     std::shared_ptr<BackgroundStorageBudget> storage)
    : state_(std::make_shared<State>(registry, std::move(changed), std::move(pause_background),
                                     std::move(primary_reserve), std::move(storage))) {
    state_->worker = std::jthread([state = state_] { state->Loop(); });
}

RecordingClipJobs::~RecordingClipJobs() {
    Shutdown();
}

engine::Outcome<engine::JobId> RecordingClipJobs::RequestPlan(RecordingClipRequest request) {
    if (request.snapshot.token.recording_id == engine::RecordingId{} || request.requested_begin_r_ns < 0 ||
        request.requested_end_r_ns <= request.requested_begin_r_ns || request.destination.empty())
        return ClipFailure(engine::EngineErrorCode::InvalidConfig, "Invalid recording clip request");
    auto job = std::make_shared<State::Job>();
    RecordingClipJobSnapshot view;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closing)
            return ClipFailure(engine::EngineErrorCode::Cancelled, "Clip service is shutting down");
        size_t outstanding = 0;
        for (const auto& existing : state_->jobs)
            if (!Terminal(existing->view.phase)) {
                ++outstanding;
                if (existing->view.recording_id == request.snapshot.token.recording_id &&
                    existing->view.requested_begin_r_ns == request.requested_begin_r_ns &&
                    existing->view.requested_end_r_ns == request.requested_end_r_ns)
                    return existing->view.job_id;
            }
        if (outstanding >= 3 || state_->next_generation == UINT64_MAX) {
            auto error = ClipFailure(engine::EngineErrorCode::ResourceBudgetExceeded,
                                     "One active clip and two waiting clips are already reserved");
            error.retryability = engine::Retryability::Transient;
            return error;
        }
        if (state_->jobs.size() >= 64)
            std::erase_if(state_->jobs, [](const auto& existing) { return Terminal(existing->view.phase); });
        job->view.job_id = engine::NewId<engine::JobId>();
        job->view.generation = state_->next_generation++;
        job->view.recording_id = request.snapshot.token.recording_id;
        job->view.requested_begin_r_ns = request.requested_begin_r_ns;
        job->view.requested_end_r_ns = request.requested_end_r_ns;
        job->view.requested_duration_seconds = request.requested_duration_seconds;
        job->request = std::move(request);
        view = job->view;
        state_->jobs.push_back(job);
        state_->work.push_back({job, false});
    }
    state_->Notify(view);
    state_->wake.notify_all();
    return view.job_id;
}

bool RecordingClipJobs::Confirm(engine::JobId id, uint64_t generation) {
    RecordingClipJobSnapshot view;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = std::find_if(state_->jobs.begin(), state_->jobs.end(), [&](const auto& job) {
            return job->view.job_id == id && job->view.generation == generation;
        });
        if (state_->closing || found == state_->jobs.end() || (*found)->view.phase != RecordingClipPhase::Review ||
            (*found)->cancelled.load())
            return false;
        (*found)->view.phase = RecordingClipPhase::Queued;
        view = (*found)->view;
        state_->work.push_back({*found, true});
    }
    state_->Notify(view);
    state_->wake.notify_all();
    return true;
}

bool RecordingClipJobs::Cancel(engine::JobId id, uint64_t generation) {
    RecordingClipJobSnapshot view;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = std::find_if(state_->jobs.begin(), state_->jobs.end(), [&](const auto& job) {
            return job->view.job_id == id && job->view.generation == generation;
        });
        if (found == state_->jobs.end() || Terminal((*found)->view.phase) ||
            (*found)->view.phase == RecordingClipPhase::Publishing)
            return false;
        const bool review = (*found)->view.phase == RecordingClipPhase::Review;
        (*found)->cancelled.store(true);
        (*found)->view.phase = RecordingClipPhase::Cancelling;
        view = (*found)->view;
        if (review)
            state_->work.push_back({*found, false});
    }
    state_->Notify(view);
    state_->wake.notify_all();
    return true;
}

std::vector<RecordingClipJobSnapshot> RecordingClipJobs::Snapshots() const {
    std::vector<RecordingClipJobSnapshot> snapshots;
    std::lock_guard lock(state_->mutex);
    for (const auto& job : state_->jobs)
        snapshots.push_back(job->view);
    return snapshots;
}

void RecordingClipJobs::BeginShutdown() {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->closing)
            return;
        state_->closing = true;
        for (const auto& job : state_->jobs) {
            job->cancelled.store(true);
            if (job->view.phase == RecordingClipPhase::Review)
                state_->work.push_back({job, false});
        }
    }
    state_->wake.notify_all();
}

void RecordingClipJobs::Shutdown() {
    BeginShutdown();
    if (state_->worker.joinable())
        state_->worker.join();
}

} // namespace exosnap

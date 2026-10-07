#include "EditPlayerWorker.h"
#include <exosnap/engine/timeline_audio_queue.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

namespace exosnap::quick {
namespace {

// Fallback tick, used only until a clip is open (or when its container declares
// no usable frame rate).
constexpr int kWorkerFallbackTickMs = 16;

const edit::Clip* EvaluatedClip(const edit::Workspace& workspace, int64_t time_us) {
    const auto evaluation = engine::EvaluateTimeline(workspace.timeline(), time_us);
    return evaluation.error.empty() && !evaluation.video.empty() ? workspace.clip(evaluation.video.front().clip)
                                                                 : nullptr;
}

} // namespace

EditPlayerWorker::EditPlayerWorker(std::shared_ptr<EditPlayerFrameSink> sink) : sink_(std::move(sink)) {
}

EditPlayerWorker::~EditPlayerWorker() {
    close();
}

void EditPlayerWorker::open(const QString& master_path, qint64 duration_ms, double screen_hz) {
    close();
    duration_ms_ = duration_ms;
    screen_hz_ = screen_hz;
    session_ = std::make_unique<exosnap::engine::EditPlayerSession>();
    std::string error;
    if (!session_->Open(std::filesystem::path(master_path.toStdWString()), error)) {
        session_.reset();
        emit openFinished(false, QString::fromStdString(error));
        return;
    }
    // Delivered straight from the decode/seek thread into the scene-graph item,
    // with no UI-thread hop: the item's mailbox is thread-safe by design, and a
    // per-frame hop would undo the point of the GPU path.
    auto sink = sink_;
    session_->SetOnFrameReady([sink](exosnap::engine::RawDecodedVideoFrame frame) { sink->deliver(std::move(frame)); });
    session_->SetVolume(static_cast<float>(volume_));
    ensureTimer();
    tick_timer_->setInterval(EditPreviewTickMsFor(session_->VideoFrameRate(), screen_hz_));
    // Poster frame: show the clip's first frame instead of the placeholder while
    // the user is still reviewing. Publishing the clock matters here too -- a
    // second clip opened over a first would otherwise leave the gate holding the
    // previous clip's value and drop this very frame.
    session_->SeekTo(0);
    syncClock();
    emit openFinished(true, {});
}

void EditPlayerWorker::close() {
    if (render_audio_)
        render_audio_->Shutdown();
    render_audio_.reset();
    render_reader_.reset();
    if (tick_timer_ != nullptr)
        tick_timer_->stop();
    if (session_) {
        session_->SetOnFrameReady({});
        session_->Close();
        session_.reset();
    }
    position_ms_ = 0;
    sink_->publishClock(-1);
}

void EditPlayerWorker::play(qint64 from_ms) {
    if (timeline_mode_) {
        seekTimeline(from_ms >= workspace_.duration() / 1000 ? 0 : from_ms, true);
        return;
    }
    if (!session_)
        return;
    position_ms_ = from_ms;
    ensureTimer();
    tick_timer_->setInterval(EditPreviewTickMsFor(session_->VideoFrameRate(), screen_hz_));
    elapsed_.restart();
    tick_timer_->start();
    // Continuous decode is only engaged when there is audio to pace it against;
    // a silent clip is driven entirely by the per-tick SeekTo fallback below,
    // since continuous decode would race through the file unthrottled for frames
    // nothing consumes.
    if (session_->HasAudioStream())
        session_->Play(position_ms_ * 1000);
    syncClock();
}

void EditPlayerWorker::pause() {
    timeline_playing_ = false;
    if (tick_timer_ != nullptr)
        tick_timer_->stop();
    if (render_audio_)
        render_audio_->Stop();
    if (!session_)
        return;
    session_->Pause();
    syncClock();
}

void EditPlayerWorker::seek(qint64 position_ms) {
    if (timeline_mode_) {
        seekTimeline(position_ms, false);
        return;
    }
    position_ms_ = position_ms;
    if (!session_)
        return;
    session_->SeekTo(position_ms * 1000);
    // A seek result is the frame the user asked for: never gate it.
    syncClock();
}

void EditPlayerWorker::setScreenRefreshHz(double screen_hz) {
    screen_hz_ = screen_hz;
    if (tick_timer_ != nullptr && session_)
        tick_timer_->setInterval(EditPreviewTickMsFor(session_->VideoFrameRate(), screen_hz_));
}

void EditPlayerWorker::ensureTimer() {
    if (tick_timer_ != nullptr)
        return;
    tick_timer_ = new QTimer(this);
    // Coarse timers are allowed a 5% slop, which at 144 fps (6 ms) is most of a
    // frame.
    tick_timer_->setTimerType(Qt::PreciseTimer);
    tick_timer_->setInterval(kWorkerFallbackTickMs);
    connect(tick_timer_, &QTimer::timeout, this, &EditPlayerWorker::onTick);
}

void EditPlayerWorker::syncClock() {
    sink_->publishClock(session_ ? session_->ClockSnapshotUs() : -1);
}

void EditPlayerWorker::onTick() {
    if (render_mode_) {
        tickRenderTimeline();
        return;
    }
    if (timeline_mode_) {
        if (!timeline_playing_)
            return;
        const auto* clip = workspace_.clip(active_clip_);
        if (clip && session_ && session_->HasAudioStream()) {
            position_ms_ = (clip->start - clip->source_in) / 1000 + session_->CurrentPositionMs();
            syncClock();
        } else {
            position_ms_ += elapsed_.restart();
            if (clip && session_) {
                session_->SeekTo(position_ms_ * 1000 - clip->start + clip->source_in);
                syncClock();
            }
        }
        const auto* next = EvaluatedClip(workspace_, position_ms_ * 1000);
        if ((next ? next->id : 0) != active_clip_)
            seekTimeline(position_ms_, true);
        position_ms_ = std::min<qint64>(position_ms_, workspace_.duration() / 1000);
        emit positionAdvanced(position_ms_);
        if (position_ms_ >= workspace_.duration() / 1000) {
            pause();
            emit reachedEnd();
        }
        return;
    }
    if (!session_)
        return;
    const qint64 total = std::max<qint64>(duration_ms_, 0);
    if (session_->HasAudioStream()) {
        // Audio is the pacing AND position source of truth while it exists.
        position_ms_ = std::clamp<qint64>(session_->CurrentPositionMs(), 0, total);
        sink_->publishClock(session_->ClockSnapshotUs());
    } else {
        position_ms_ = std::clamp<qint64>(position_ms_ + elapsed_.restart(), 0, total);
        session_->SeekTo(position_ms_ * 1000);
        syncClock();
    }
    emit positionAdvanced(position_ms_);
    if (total > 0 && position_ms_ >= total) {
        tick_timer_->stop();
        session_->Pause();
        syncClock();
        emit reachedEnd();
    }
}

void EditPlayerWorker::setTimeline(edit::Workspace workspace) {
    if (timeline_mode_ && workspace.clips() == workspace_.clips())
        return;
    pause();
    close();
    sink_->clear();
    workspace_ = std::move(workspace);
    timeline_mode_ = true;
    active_clip_ = 0;
    render_mode_ = !workspace_.transitions().empty();
    if (render_mode_) {
        engine::TimelineRenderSnapshot snapshot;
        snapshot.timeline = workspace_.timeline();
        for (const auto& asset : workspace_.assets()) {
            const std::filesystem::path path(asset.path);
            snapshot.sources.push_back({asset.id, path, engine::ProbeEditMedia(path)});
        }
        render_plan_ =
            engine::ClassifyEditExport(snapshot, {engine::TimelineExportEligibility::RenderRequired, {}}, false);
        std::string error;
        render_reader_ = std::make_unique<engine::EditTimelineReader>();
        if (!render_reader_->Open(std::move(snapshot), render_plan_, error)) {
            render_reader_.reset();
            emit openFinished(false, QString::fromStdString(error));
            return;
        }
        emit openFinished(true, {});
    }
    seekTimeline(workspace_.playhead() / 1000, false);
    if (!EvaluatedClip(workspace_, workspace_.playhead()))
        emit openFinished(workspace_.duration() > 0, {});
}

void EditPlayerWorker::setVolume(double volume) {
    volume_ = volume;
    if (session_)
        session_->SetVolume(static_cast<float>(volume));
}

void EditPlayerWorker::seekTimeline(qint64 position_ms, bool resume) {
    if (render_mode_) {
        if (!render_reader_)
            return;
        if (render_audio_)
            render_audio_->Stop();
        render_reader_->ResetAudio();
        position_ms_ = std::clamp<qint64>(position_ms, 0, workspace_.duration() / 1000);
        render_origin_ms_ = position_ms_;
        audio_sample_ = position_ms_ * 48;
        timeline_playing_ = resume;
        if (!renderTimeline(position_ms_))
            return;
        ensureTimer();
        tick_timer_->setInterval(
            EditPreviewTickMsFor(static_cast<double>(render_plan_.fps_num) / render_plan_.fps_den, screen_hz_));
        elapsed_.restart();
        if (resume) {
            const bool has_audio =
                std::any_of(workspace_.clips().begin(), workspace_.clips().end(), [this](const auto& c) {
                    const auto* asset = workspace_.asset(c.asset);
                    return asset && asset->audio;
                });
            if (has_audio && !render_audio_) {
                render_audio_ = std::make_unique<engine::WasapiAudioRenderer>();
                std::string error;
                if (!render_audio_->Init(error)) {
                    render_audio_.reset();
                    timeline_playing_ = false;
                    emit openFinished(false, QString::fromStdString(error));
                    return;
                }
            }
            if (render_audio_) {
                std::vector<float> initial;
                std::string error;
                const auto count =
                    static_cast<uint32_t>(std::min<int64_t>(9600, workspace_.duration() * 48 / 1000 - audio_sample_));
                if (!render_reader_->AudioAt(audio_sample_, count, initial, error)) {
                    pause();
                    emit openFinished(false, QString::fromStdString(error));
                    return;
                }
                for (auto& sample : initial)
                    sample *= static_cast<float>(volume_);
                render_audio_->Start(initial, true);
                if (!render_audio_->Running()) {
                    pause();
                    emit openFinished(false, tr("The audio output could not start."));
                    return;
                }
                audio_sample_ += count;
            }
            tickRenderTimeline();
            tick_timer_->start();
        } else
            tick_timer_->stop();
        return;
    }
    const auto* clip = EvaluatedClip(workspace_, position_ms * 1000);
    const auto* asset = clip ? workspace_.asset(clip->asset) : nullptr;
    const auto id = clip ? clip->id : 0;
    if (id != active_clip_ || (clip && !session_)) {
        close();
        sink_->clear();
        if (asset && asset->state == edit::AssetState::Available)
            open(QString::fromStdWString(asset->path), asset->duration / 1000, screen_hz_);
        else
            emit openFinished(workspace_.duration() > 0, asset ? tr("Media unavailable") : QString());
        active_clip_ = id;
    }
    position_ms_ = position_ms;
    if (session_ && clip) {
        const auto source_us = position_ms * 1000 - clip->start + clip->source_in;
        session_->SeekTo(source_us);
        if (resume && session_->HasAudioStream())
            session_->Play(source_us);
        syncClock();
    }
    timeline_playing_ = resume;
    ensureTimer();
    elapsed_.restart();
    if (resume)
        tick_timer_->start();
    else
        tick_timer_->stop();
}

bool EditPlayerWorker::renderTimeline(qint64 position_ms) {
    if (!render_reader_)
        return false;
    engine::TimelineVideoFrame frame;
    std::string error;
    if (!render_reader_->VideoAt(position_ms * 1000, frame, error)) {
        sink_->clear();
        emit openFinished(false, QString::fromStdString(error));
        pause();
        return false;
    }
    sink_->publishClock(-1);
    sink_->deliverTimeline(std::move(frame), request_generation_);
    return true;
}

void EditPlayerWorker::tickRenderTimeline() {
    if (!timeline_playing_ || !render_reader_)
        return;
    position_ms_ =
        render_origin_ms_ +
        (render_audio_ ? static_cast<qint64>(render_audio_->FramesPlayed() * 1000 / render_audio_->SampleRate())
                       : elapsed_.elapsed());
    position_ms_ = std::min<qint64>(position_ms_, workspace_.duration() / 1000);
    if (render_audio_) {
        if (!render_audio_->Running()) {
            pause();
            emit openFinished(false, tr("The audio output stopped unexpectedly."));
            return;
        }
        const auto consumed = render_audio_->TimelineConsumedFrames();
        const int64_t origin = render_origin_ms_ * 48;
        audio_sample_ = std::max(audio_sample_, origin + static_cast<int64_t>(consumed));
        const int64_t through =
            origin + static_cast<int64_t>(engine::TimelineAudioRenderThrough(
                         static_cast<uint64_t>(workspace_.duration() * 48 / 1000 - origin), consumed));
        std::vector<float> samples;
        std::string error;
        while (audio_sample_ < through) {
            const auto count = static_cast<uint32_t>(std::min<int64_t>(9600, through - audio_sample_));
            if (!render_reader_->AudioAt(audio_sample_, count, samples, error)) {
                pause();
                emit openFinished(false, QString::fromStdString(error));
                return;
            }
            for (auto& sample : samples)
                sample *= static_cast<float>(volume_);
            render_audio_->PushTimelineSamples(static_cast<uint64_t>(audio_sample_ - render_origin_ms_ * 48), samples);
            audio_sample_ += count;
        }
    }
    if (!renderTimeline(position_ms_))
        return;
    emit positionAdvanced(position_ms_);
    if (position_ms_ >= workspace_.duration() / 1000) {
        pause();
        emit reachedEnd();
    }
}

} // namespace exosnap::quick

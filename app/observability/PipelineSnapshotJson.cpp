#include "observability/PipelineSnapshotJson.h"

#include "observability/ObservabilityJson.h"
#include "observability/ProtocolNames.h"

#include <QJsonArray>

namespace exosnap::observability {
namespace {

using recorder_core::MetricAvailability;
using recorder_core::RecordingDiagnosticsSnapshot;

QJsonObject SummaryJson(const RecordingDiagnosticsSnapshot& s) {
    QJsonObject json;
    json.insert(QStringLiteral("valid"), s.valid);
    json.insert(QStringLiteral("sessionGeneration"), Count(s.session_generation));
    json.insert(QStringLiteral("lifecycle"), LifecycleName(s.lifecycle));
    json.insert(QStringLiteral("elapsedSeconds"), Metric(s.elapsed_seconds, s.valid));
    json.insert(QStringLiteral("health"), QString::fromLatin1(recorder_core::ToString(s.health)));
    json.insert(QStringLiteral("bottleneck"), QString::fromLatin1(recorder_core::ToString(s.bottleneck)));
    json.insert(QStringLiteral("bottleneckReason"), TextOrNull(s.bottleneck_reason));
    return json;
}

QJsonObject CaptureJson(const recorder_core::CaptureDiagnostics& c) {
    QJsonObject json;
    json.insert(QStringLiteral("targetFps"), c.target_fps);
    json.insert(QStringLiteral("actualFps"), c.actual_fps);
    json.insert(QStringLiteral("framesCaptured"), Count(c.frames_captured));
    json.insert(QStringLiteral("framesEmitted"), Count(c.frames_emitted));

    // The drop taxonomy, with the engine's own aggregate alongside the parts.
    // `problemDrops` is what every user-facing drop surface reads; the four
    // categories are here so a diagnosis can say WHICH kind was lost, and
    // `totalDrops` so nobody has to re-add them and get it wrong.
    json.insert(QStringLiteral("problemDrops"), Count(c.frames_dropped_problem()));
    json.insert(QStringLiteral("totalDrops"), Count(c.frames_dropped_total()));
    json.insert(QStringLiteral("coalescedDrops"), Count(c.frames_dropped_coalesced));
    json.insert(QStringLiteral("cfrDrops"), Count(c.frames_dropped_cfr));
    json.insert(QStringLiteral("backpressureDrops"), Count(c.frames_dropped_backpressure));
    json.insert(QStringLiteral("processingFailures"), Count(c.frames_dropped_processing_failure));

    json.insert(QStringLiteral("duplicates"), Count(c.frames_duplicated));
    json.insert(QStringLiteral("sourceLoss"), c.source_loss);
    json.insert(QStringLiteral("sourceType"), CaptureSourceTypeName(c.source_type));

    // Only observed on VFR: on a CFR session the interval IS the target, so
    // reporting it as a measurement would be reporting the setting back.
    json.insert(QStringLiteral("frameIntervalMs"), Metric(c.frame_interval_ms, c.interval_observed));
    json.insert(QStringLiteral("frameIntervalAvailability"), AvailabilityKey(c.interval_observed));
    return json;
}

// Source present cadence + present mode. Two DIFFERENT providers with two
// different availability stories, deliberately kept apart:
//
//   cadence    -- DXGI Output Duplication only, from LastPresentTime deltas.
//                 Structurally absent under WGC (window/region capture), which
//                 exposes no present timestamp at all -- hence `unsupported`
//                 rather than `unavailable` for a window source.
//   mode       -- PresentMon ETW, elevation- and opt-in-gated. The pipeline
//                 snapshot cannot tell WHY it is off; environment.snapshot can,
//                 and says so there.
QJsonObject SourcePresentationJson(const recorder_core::CaptureDiagnostics& c) {
    const bool cadence_available = IsAvailable(c.present_cadence_availability);
    const bool wgc_source = c.source_type == recorder_core::CaptureSourceType::Window ||
                            c.source_type == recorder_core::CaptureSourceType::Region;

    QJsonObject json;
    json.insert(QStringLiteral("presentIntervalMs"), Metric(c.source_present_interval_ms, cadence_available));
    json.insert(QStringLiteral("presentJitterMs"), Metric(c.source_present_jitter_ms, cadence_available));
    json.insert(QStringLiteral("coalesceRatio"), Metric(c.source_coalesce_ratio, cadence_available));
    json.insert(QStringLiteral("cadenceAvailability"), cadence_available ? QString::fromLatin1(availability::kAvailable)
                                                       : wgc_source ? QString::fromLatin1(availability::kUnsupported)
                                                                    : QString::fromLatin1(availability::kUnavailable));

    const bool mode_available = IsAvailable(c.present_mode_availability);
    json.insert(QStringLiteral("presentMode"),
                mode_available ? QJsonValue(PresentModeName(c.source_present_mode)) : QJsonValue(QJsonValue::Null));
    json.insert(QStringLiteral("tearing"),
                mode_available ? QJsonValue(c.source_tearing) : QJsonValue(QJsonValue::Null));
    json.insert(QStringLiteral("modeAvailability"), AvailabilityKey(c.present_mode_availability));
    return json;
}

QJsonObject CaptureTimingJson(const recorder_core::CaptureDiagnostics& c) {
    QJsonObject json;
    json.insert(QStringLiteral("acquireLatestMs"), Metric(c.acquire_latest_ms, c.acquire_availability));
    json.insert(QStringLiteral("acquireAverageMs"), Metric(c.acquire_average_ms, c.acquire_availability));
    json.insert(QStringLiteral("acquirePeakMs"), Metric(c.acquire_peak_ms, c.acquire_availability));
    json.insert(QStringLiteral("availability"), AvailabilityKey(c.acquire_availability));
    // Stated, not implied: this brackets the backend acquire + copy on the CPU.
    json.insert(QStringLiteral("semantics"), QStringLiteral("cpuAcquireAndCopy"));
    return json;
}

QJsonObject CompositorJson(const recorder_core::CompositorDiagnostics& c) {
    QJsonObject json;
    json.insert(QStringLiteral("active"), c.active);
    json.insert(QStringLiteral("latestMs"), Metric(c.latest_ms, c.active));
    json.insert(QStringLiteral("averageMs"), Metric(c.average_ms, c.active));
    json.insert(QStringLiteral("peakMs"), Metric(c.peak_ms, c.active));
    json.insert(QStringLiteral("framesComposed"), Count(c.frames_composed));

    json.insert(QStringLiteral("vpBltLatestMs"), Metric(c.vpblt_latest_ms, c.vpblt_availability));
    json.insert(QStringLiteral("vpBltAverageMs"), Metric(c.vpblt_average_ms, c.vpblt_availability));
    json.insert(QStringLiteral("vpBltPeakMs"), Metric(c.vpblt_peak_ms, c.vpblt_availability));
    json.insert(QStringLiteral("vpBltAvailability"), AvailabilityKey(c.vpblt_availability));

    json.insert(QStringLiteral("overlayOmitted"), c.overlay_omitted);
    // The one thing a reader must not get wrong about these numbers. There is no
    // GPU timestamp query in this pipeline, so nothing here is GPU execution
    // time, and no field may ever be renamed to suggest that it is.
    json.insert(QStringLiteral("timingSemantics"), QStringLiteral("cpuSubmission"));
    return json;
}

QJsonObject EncoderJson(const recorder_core::EncoderDiagnostics& e) {
    QJsonObject json;
    const bool sampled = e.frames_encoded > 0;
    json.insert(QStringLiteral("latestMs"), Metric(e.latest_ms, sampled));
    json.insert(QStringLiteral("averageMs"), Metric(e.average_ms, sampled));
    json.insert(QStringLiteral("peakMs"), Metric(e.peak_ms, sampled));
    json.insert(QStringLiteral("p50Ms"), Metric(e.p50_ms, sampled));
    json.insert(QStringLiteral("p99Ms"), Metric(e.p99_ms, sampled));
    json.insert(QStringLiteral("availability"),
                QString::fromLatin1(sampled ? availability::kAvailable : availability::kUnavailable));
    json.insert(QStringLiteral("outputFps"), e.output_fps);
    json.insert(QStringLiteral("framesSubmitted"), Count(e.frames_submitted));
    json.insert(QStringLiteral("framesEncoded"), Count(e.frames_encoded));
    json.insert(QStringLiteral("backlog"), Count(e.backlog));
    json.insert(QStringLiteral("forcedKeyframes"), Count(e.forced_keyframes));
    json.insert(QStringLiteral("timestampMismatches"), Count(e.output_ts_mismatches));
    json.insert(QStringLiteral("keyframeMismatches"), Count(e.keyframe_prediction_mismatches));
    json.insert(QStringLiteral("codec"), ui::videoCodecLabel(e.codec));
    json.insert(QStringLiteral("width"), static_cast<int>(e.width));
    json.insert(QStringLiteral("height"), static_cast<int>(e.height));
    json.insert(QStringLiteral("cfr"), e.cfr);
    // Submit -> bitstream-ready, per frame. Named so nobody reads it as the time
    // the GPU spent encoding.
    json.insert(QStringLiteral("timingSemantics"), QStringLiteral("submitToBitstreamReady"));
    return json;
}

// The authoritative answer to "what is actually running in the encoder". Emitted
// only when the encoder was configured: an EncoderInitInfo with valid == false is
// a session that failed before configure, and its defaults are not a measurement.
QJsonObject EncoderInitJson(const recorder_core::EncoderInitInfo& init) {
    QJsonObject json;
    json.insert(QStringLiteral("valid"), init.valid);
    if (!init.valid)
        return json;
    json.insert(QStringLiteral("codec"), ui::videoCodecLabel(init.codec));
    json.insert(QStringLiteral("preset"), EncoderPresetName(init.preset));
    json.insert(QStringLiteral("rateControl"), RateControlName(init.rc_mode));
    json.insert(QStringLiteral("targetBitrateKbps"), static_cast<double>(init.target_bitrate_kbps));
    json.insert(QStringLiteral("maxBitrateKbps"), static_cast<double>(init.max_bitrate_kbps));
    json.insert(QStringLiteral("cq"), static_cast<double>(init.cq));
    json.insert(QStringLiteral("gopLength"), static_cast<double>(init.gop_length));
    json.insert(QStringLiteral("bframes"), static_cast<double>(init.bframes));
    json.insert(QStringLiteral("lookaheadFrames"), static_cast<double>(init.lookahead_frames));
    json.insert(QStringLiteral("temporalAQ"), init.temporal_aq);
    json.insert(QStringLiteral("spatialAQ"), init.spatial_aq);
    json.insert(QStringLiteral("bitDepth"), BitDepthValue(init.bit_depth));
    json.insert(QStringLiteral("chroma"), ChromaName(init.chroma));
    json.insert(QStringLiteral("colorRange"), ColorRangeName(init.color_full_range));
    json.insert(QStringLiteral("hdrMode"), HdrModeName(init.hdr_mode));
    return json;
}

QJsonObject VideoTimingJson(const recorder_core::VideoTimingDiagnostics& t) {
    QJsonObject json;
    json.insert(QStringLiteral("tickP50Ms"), Metric(t.tick_p50_ms, t.availability));
    json.insert(QStringLiteral("tickP99Ms"), Metric(t.tick_p99_ms, t.availability));
    json.insert(QStringLiteral("tickPeakMs"), Metric(t.tick_peak_ms, t.availability));
    // The frame budget is 1000 / target fps and is known from configuration, not
    // from a sample -- it is the one value in this group that is real before the
    // first frame. Diagnostics reads it from here rather than recomputing it.
    json.insert(QStringLiteral("budgetMs"), Metric(t.budget_ms, t.budget_ms > 0.0));
    json.insert(QStringLiteral("availability"), AvailabilityKey(t.availability));
    json.insert(QStringLiteral("semantics"), QStringLiteral("wholeVideoTick"));
    return json;
}

QJsonArray ResamplerDrainJson(const recorder_core::AudioDiagnostics& a) {
    QJsonArray tracks;
    for (std::size_t i = 0; i < a.resampler_drain_recorded.size(); ++i) {
        // A track that never reached its drain leaves the counters at 0, and 0
        // there is not a measurement. Only recorded tracks are reported at all.
        if (!a.resampler_drain_recorded[i])
            continue;
        QJsonObject track;
        track.insert(QStringLiteral("track"), static_cast<int>(i));
        track.insert(QStringLiteral("drainedFrames"), Count(a.resampler_drained_frames[i]));
        track.insert(QStringLiteral("undrainedFrames"), Count(a.resampler_undrained_frames[i]));
        tracks.append(track);
    }
    return tracks;
}

QJsonObject AudioJson(const recorder_core::AudioDiagnostics& a) {
    QJsonObject json;
    json.insert(QStringLiteral("active"), a.active);
    json.insert(QStringLiteral("packetsEncoded"), Count(a.packets_encoded));
    json.insert(QStringLiteral("bytesEncoded"), Count(a.bytes_encoded));
    json.insert(QStringLiteral("queueDepth"), static_cast<double>(a.queue_depth));
    json.insert(QStringLiteral("queuePeak"), static_cast<double>(a.queue_peak));
    json.insert(QStringLiteral("discontinuities"),
                Metric(static_cast<double>(a.discontinuities), a.discontinuity_availability));
    json.insert(QStringLiteral("discontinuityAvailability"), AvailabilityKey(a.discontinuity_availability));
    json.insert(QStringLiteral("sampleRate"), static_cast<double>(a.sample_rate));
    json.insert(QStringLiteral("channels"), static_cast<double>(a.channels));
    json.insert(QStringLiteral("codec"), ui::audioCodecLabel(a.codec));
    json.insert(QStringLiteral("trackCount"), static_cast<double>(a.track_count));
    json.insert(QStringLiteral("degradedSources"), static_cast<double>(a.degraded_sources));
    json.insert(QStringLiteral("sourceDegraded"), a.source_degraded);
    // Latched post-flight fact: at least one source was lost at SOME point, even
    // if all of them are healthy again now. Distinct from sourceDegraded, and the
    // difference is the whole value of it.
    json.insert(QStringLiteral("sourceDegradedOccurred"), a.source_degraded_occurred);
    // Terminal-snapshot only; an empty array means no track has drained yet, not
    // that nothing was dropped.
    json.insert(QStringLiteral("resamplerDrain"), ResamplerDrainJson(a));
    return json;
}

// The three A/V facts that must never be collapsed into one number:
//   raw       -- measured device-clock vs QPC drift, BEFORE compensation
//   residual  -- what is left after clock slaving, i.e. what lands in the file
//   skew      -- accumulated |video duration - audio duration|, a starving encoder
QJsonObject AvTimingJson(const recorder_core::RecordingDiagnosticsSnapshot& s) {
    const bool drift_available = IsAvailable(s.av_drift_availability);
    QJsonObject json;
    json.insert(QStringLiteral("avDriftMs"), Metric(s.av_drift_ms, drift_available));
    json.insert(QStringLiteral("avDriftAvailability"), AvailabilityKey(s.av_drift_availability));
    json.insert(QStringLiteral("rawAvDriftMs"), Metric(s.av_drift_raw_ms, drift_available));
    json.insert(QStringLiteral("peakAvDriftMs"), Metric(s.peak_av_drift_ms, s.peak_av_drift_availability));
    json.insert(QStringLiteral("peakAvailability"), AvailabilityKey(s.peak_av_drift_availability));
    json.insert(QStringLiteral("clockSlavingActive"), s.clock_slaving_active);
    json.insert(QStringLiteral("clockSlavingPpm"), Metric(s.clock_slaving_ppm, drift_available));
    json.insert(QStringLiteral("durationSkewMs"), Metric(s.duration_skew_ms, s.duration_skew_availability));
    json.insert(QStringLiteral("durationSkewAvailability"), AvailabilityKey(s.duration_skew_availability));
    return json;
}

QJsonObject QueueJson(const recorder_core::QueueDiagnostics& q) {
    const bool available = IsAvailable(q.availability);
    QJsonObject json;
    json.insert(QStringLiteral("currentDepth"), Metric(static_cast<double>(q.current_depth), available));
    json.insert(QStringLiteral("peakDepth"), Metric(static_cast<double>(q.peak_depth), available));
    json.insert(QStringLiteral("capacity"), static_cast<double>(q.capacity));
    json.insert(QStringLiteral("bounded"), q.bounded);
    json.insert(QStringLiteral("droppedItems"), Count(q.dropped_items));
    json.insert(QStringLiteral("availability"), AvailabilityKey(q.availability));
    return json;
}

QJsonObject MuxJson(const recorder_core::MuxDiagnostics& m) {
    const bool available = IsAvailable(m.availability);
    QJsonObject json;
    json.insert(QStringLiteral("packetsProcessed"), Count(m.packets_processed));
    json.insert(QStringLiteral("bytesWritten"), Count(m.bytes_written));
    json.insert(QStringLiteral("throughputMiBs"), m.throughput_mib_s);

    json.insert(QStringLiteral("latestWriteMs"), Metric(m.latest_write_ms, available));
    json.insert(QStringLiteral("averageWriteMs"), Metric(m.average_write_ms, available));
    json.insert(QStringLiteral("peakWriteMs"), Metric(m.peak_write_ms, available));

    // Segment-local streaming Matroska reorder window. Unavailable for MP4, which
    // is what `availability` reports.
    json.insert(QStringLiteral("reorderPackets"), Metric(static_cast<double>(m.reorder_packets), available));
    json.insert(QStringLiteral("reorderPacketsPeak"), Metric(static_cast<double>(m.reorder_packets_peak), available));
    json.insert(QStringLiteral("reorderBytes"), Metric(static_cast<double>(m.reorder_bytes), available));
    json.insert(QStringLiteral("reorderBytesPeak"), Metric(static_cast<double>(m.reorder_bytes_peak), available));

    json.insert(QStringLiteral("currentSegment"), static_cast<double>(m.current_segment_index));
    json.insert(QStringLiteral("segmentCount"), static_cast<double>(m.segment_count));
    json.insert(QStringLiteral("splitTransitions"), Count(m.split_transitions));
    json.insert(QStringLiteral("finalizations"), Count(m.finalizations));
    json.insert(QStringLiteral("latestFinalizeMs"), Metric(m.latest_finalize_ms, m.finalizations > 0));
    json.insert(QStringLiteral("failures"), Count(m.failures));

    json.insert(QStringLiteral("processLatestMs"), Metric(m.process_latest_ms, m.process_availability));
    json.insert(QStringLiteral("processAverageMs"), Metric(m.process_average_ms, m.process_availability));
    json.insert(QStringLiteral("processPeakMs"), Metric(m.process_peak_ms, m.process_availability));
    json.insert(QStringLiteral("processAvailability"), AvailabilityKey(m.process_availability));

    json.insert(QStringLiteral("availability"), AvailabilityKey(m.availability));
    return json;
}

QJsonObject DiskJson(const recorder_core::RecordingDiagnosticsSnapshot& s) {
    const recorder_core::DiskDiagnostics& d = s.disk;
    const bool latency_available = IsAvailable(d.latency_availability);
    QJsonObject json;
    json.insert(QStringLiteral("bytesWritten"), Count(d.bytes_written));
    json.insert(QStringLiteral("throughputMiBs"), d.throughput_mib_s);
    json.insert(QStringLiteral("latestWriteMs"), Metric(d.latest_write_ms, latency_available));
    json.insert(QStringLiteral("averageWriteMs"), Metric(d.average_write_ms, latency_available));
    json.insert(QStringLiteral("peakWriteMs"), Metric(d.peak_write_ms, latency_available));
    json.insert(QStringLiteral("availability"), AvailabilityKey(d.latency_availability));
    // Drive/root only -- the engine already scrubs the path, and the full output
    // path is not needed to diagnose a slow or full volume.
    json.insert(QStringLiteral("outputTarget"), TextOrNull(d.output_target));
    json.insert(QStringLiteral("writeFailures"), Count(d.write_failures));
    // Negative means unavailable (throughput or free space unknown), which is a
    // real answer and must not serialize as "0 seconds left".
    json.insert(QStringLiteral("fillEtaSeconds"), Metric(s.disk_fill_eta_seconds, s.disk_fill_eta_seconds >= 0.0));
    json.insert(QStringLiteral("latencySemantics"), QStringLiteral("bufferedWriteCall"));
    return json;
}

QJsonObject SplitJson(const recorder_core::SplitDiagnostics& sp) {
    QJsonObject json;
    json.insert(QStringLiteral("supported"), sp.split_supported);
    json.insert(QStringLiteral("currentSegment"), static_cast<double>(sp.current_segment));
    json.insert(QStringLiteral("completedSegments"), static_cast<double>(sp.completed_segments));
    json.insert(QStringLiteral("pending"), sp.split_pending);
    json.insert(QStringLiteral("lastTrigger"), SplitTriggerName(sp.last_trigger));
    json.insert(QStringLiteral("lastFinalizeMs"), Metric(sp.last_finalize_ms, sp.completed_segments > 0));
    json.insert(QStringLiteral("failures"), Count(sp.split_failures));
    json.insert(QStringLiteral("secondsUntilAutoSplit"),
                Metric(sp.seconds_until_auto_split, sp.seconds_until_auto_split >= 0.0));
    json.insert(QStringLiteral("availability"), AvailabilityKey(sp.availability));
    return json;
}

QJsonObject RetainedFramesJson(const recorder_core::RecordingDiagnosticsSnapshot& s) {
    QJsonObject json;
    json.insert(QStringLiteral("screenGenerationChanges"), Count(s.screen_generation_changes));
    json.insert(QStringLiteral("webcamGenerationChanges"), Count(s.webcam_generation_changes));
    json.insert(QStringLiteral("cursorOnlyEventsIgnored"), Count(s.cursor_only_capture_events_ignored));
    json.insert(QStringLiteral("phaseRingCursorOnlyEventsIgnored"), Count(s.phase_ring_cursor_only_events_ignored));
    json.insert(QStringLiteral("fullCompositions"), Count(s.full_compositions));
    json.insert(QStringLiteral("reusedYuvFrames"), Count(s.reused_yuv_frames));
    json.insert(QStringLiteral("yuvSlotCopies"), Count(s.yuv_slot_copies));
    json.insert(QStringLiteral("yuvSlotCopiesSkipped"), Count(s.yuv_slot_copies_skipped));
    return json;
}

} // namespace

QJsonObject PipelineSnapshotToJson(const RecordingDiagnosticsSnapshot& snapshot) {
    QJsonObject json = SummaryJson(snapshot);
    if (!snapshot.valid) {
        // Nothing has been measured. Emitting the measurement groups here would
        // hand a reader a complete, entirely zero pipeline -- which reads as a
        // healthy recording that dropped nothing, not as an idle process.
        return json;
    }

    json.insert(QStringLiteral("capture"), CaptureJson(snapshot.capture));
    json.insert(QStringLiteral("sourcePresentation"), SourcePresentationJson(snapshot.capture));
    json.insert(QStringLiteral("captureTiming"), CaptureTimingJson(snapshot.capture));
    json.insert(QStringLiteral("compositor"), CompositorJson(snapshot.compositor));
    json.insert(QStringLiteral("encoder"), EncoderJson(snapshot.video_encoder));
    json.insert(QStringLiteral("encoderInit"), EncoderInitJson(snapshot.encoder_init));
    json.insert(QStringLiteral("videoTiming"), VideoTimingJson(snapshot.video_timing));
    json.insert(QStringLiteral("audio"), AudioJson(snapshot.audio));
    json.insert(QStringLiteral("avTiming"), AvTimingJson(snapshot));
    json.insert(QStringLiteral("videoQueue"), QueueJson(snapshot.video_queue));
    json.insert(QStringLiteral("audioQueue"), QueueJson(snapshot.audio_queue));
    json.insert(QStringLiteral("mux"), MuxJson(snapshot.mux));
    json.insert(QStringLiteral("disk"), DiskJson(snapshot));
    json.insert(QStringLiteral("split"), SplitJson(snapshot.split));
    json.insert(QStringLiteral("retainedFrames"), RetainedFramesJson(snapshot));
    return json;
}

} // namespace exosnap::observability

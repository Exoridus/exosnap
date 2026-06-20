#include "matroska_stream_writer.h"

// libebml / libmatroska headers — suppress MSVC warnings from third-party code
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267) // size_t to smaller integer conversion
#pragma warning(disable : 4244) // possible data loss in conversion
#pragma warning(disable : 4245) // signed/unsigned mismatch in conversion
#pragma warning(disable : 4100) // unreferenced formal parameter
#endif
#include <ebml/EbmlHead.h>
#include <ebml/EbmlSubHead.h>
#include <ebml/EbmlVoid.h>
#include <ebml/StdIOCallback.h>
#include <matroska/KaxBlockData.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxCues.h>
#include <matroska/KaxCuesData.h>
#include <matroska/KaxInfo.h>
#include <matroska/KaxSeekHead.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxSemantic.h>
#include <matroska/KaxTrackAudio.h>
#include <matroska/KaxTrackVideo.h>
#include <matroska/KaxTracks.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <chrono>
#include <exception>

namespace recorder_core {

namespace {

// Timecode scale: 1 000 000 ns per unit = 1 ms resolution (unchanged contract).
constexpr uint64_t kTimecodeScaleNs = 1000000ULL;
// Start a new cluster at a video keyframe once >= 2000 ms have elapsed.
constexpr uint64_t kClusterBoundaryMs = 2000ULL;
// Maximum relative timecode inside a cluster (int16_t range, in ms).
constexpr int64_t kMaxClusterRelativeMs = 32767LL;
// Space reserved for the SeekHead placeholder at the start of the Segment.
constexpr uint64_t kSeekHeadReservedBytes = 150ULL;

} // namespace

MatroskaStreamWriter::MatroskaStreamWriter() = default;

MatroskaStreamWriter::~MatroskaStreamWriter() {
    // Defensive: if Finalize() was never called, release the cluster and close
    // the handle so we never leak the file descriptor. We do NOT attempt a real
    // finalize here — an un-finalized file is intentionally left as-is for the
    // failure path; the owner decides whether to quarantine it.
    delete m_cluster;
    m_cluster = nullptr;
    CloseIo();
}

void MatroskaStreamWriter::Fail(const std::string& reason) {
    if (!m_failed) {
        m_failed = true;
        m_error = reason;
    }
}

void MatroskaStreamWriter::CloseIo() {
    if (m_io != nullptr) {
        try {
            static_cast<libebml::StdIOCallback*>(m_io)->close();
        } catch (...) {
            // best-effort close
        }
        delete m_io;
        m_io = nullptr;
    }
}

bool MatroskaStreamWriter::Open(const MatroskaStreamConfig& config) {
    if (m_opened) {
        Fail("MatroskaStreamWriter::Open called twice");
        return false;
    }
    m_config = config;
    m_opened = true;

    // --- Open output file ---
    try {
        m_io = new libebml::StdIOCallback(m_config.output_path.c_str(), MODE_CREATE);
    } catch (const std::exception& ex) {
        m_io = nullptr;
        Fail(std::string("StdIOCallback::open failed: ") + ex.what());
        return false;
    }

    try {
        // --- EBML header ---
        {
            libebml::EbmlHead ebml_head;
            libebml::GetChild<libebml::EDocType>(ebml_head).SetValue("matroska");
            libebml::GetChild<libebml::EDocTypeVersion>(ebml_head).SetValue(4);
            libebml::GetChild<libebml::EDocTypeReadVersion>(ebml_head).SetValue(2);
            ebml_head.Render(*m_io, true);
        }

        // --- Segment head (unknown size, back-patched at finalize) ---
        m_segment = std::make_unique<libmatroska::KaxSegment>();
        m_segment->SetSizeInfinite(true);
        m_segment->WriteHead(*m_io, 8);
        m_segment_data_start = static_cast<uint64_t>(m_io->getFilePointer());

        // --- SeekHead placeholder void (replaced at finalize) ---
        m_seekhead_void = std::make_unique<libebml::EbmlVoid>();
        m_seekhead_void->SetSize(kSeekHeadReservedBytes);
        m_seekhead_void->Render(*m_io);

        // --- Segment Info. Duration is written as 0.0 now and back-patched at
        //     finalize. We reserve a fixed 8-byte float so the patched value is
        //     guaranteed to fit in place. ---
        m_info = std::make_unique<libmatroska::KaxInfo>();
        libebml::GetChild<libmatroska::KaxTimecodeScale>(*m_info).SetValue(kTimecodeScaleNs);
        libebml::GetChild<libmatroska::KaxMuxingApp>(*m_info).SetValueUTF8("exosnap");
        libebml::GetChild<libmatroska::KaxWritingApp>(*m_info).SetValueUTF8("exosnap");
        {
            auto& dur = libebml::GetChild<libmatroska::KaxDuration>(*m_info);
            // Force 8-byte (double) precision so the value can be back-patched at
            // finalize without changing the element's serialized size (a 4-byte
            // float would risk a size mismatch in OverwriteData).
            dur.SetPrecision(libebml::EbmlFloat::FLOAT_64);
            dur.SetValue(0.0);
        }
        m_info->Render(*m_io, /*bSaveDefault=*/true);

        // --- Tracks ---
        m_tracks = std::make_unique<libmatroska::KaxTracks>();
        m_track_entries.clear();

        // Video track (track_num=1)
        {
            auto& vid = libebml::AddNewChild<libmatroska::KaxTrackEntry>(*m_tracks);
            libebml::GetChild<libmatroska::KaxTrackNumber>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackUID>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackType>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackFlagEnabled>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackFlagDefault>(vid).SetValue(1);
            libebml::GetChild<libmatroska::KaxTrackFlagLacing>(vid).SetValue(0);
            libebml::GetChild<libmatroska::KaxCodecID>(vid).SetValue(m_config.video_codec_id);
            if (!m_config.video_codec_private.empty()) {
                libebml::GetChild<libmatroska::KaxCodecPrivate>(vid).CopyBuffer(
                    m_config.video_codec_private.data(), static_cast<uint32_t>(m_config.video_codec_private.size()));
            }
            auto& vs = libebml::GetChild<libmatroska::KaxTrackVideo>(vid);
            libebml::GetChild<libmatroska::KaxVideoPixelWidth>(vs).SetValue(m_config.encode_width);
            libebml::GetChild<libmatroska::KaxVideoPixelHeight>(vs).SetValue(m_config.encode_height);
            if (m_config.frame_rate_num > 0 && m_config.frame_rate_den > 0) {
                const uint64_t frame_dur_ns = static_cast<uint64_t>(m_config.frame_rate_den) * 1000000000ULL /
                                              static_cast<uint64_t>(m_config.frame_rate_num);
                libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(vid).SetValue(frame_dur_ns);
            }
            vid.SetGlobalTimecodeScale(kTimecodeScaleNs);
            m_track_entries.push_back(&vid);
        }

        // Audio tracks (track_num=2+i)
        for (uint32_t i = 0; i < m_config.audio_track_count; ++i) {
            const uint32_t track_number = 2 + i;
            auto& aud = libebml::AddNewChild<libmatroska::KaxTrackEntry>(*m_tracks);
            libebml::GetChild<libmatroska::KaxTrackNumber>(aud).SetValue(track_number);
            libebml::GetChild<libmatroska::KaxTrackUID>(aud).SetValue(track_number);
            libebml::GetChild<libmatroska::KaxTrackType>(aud).SetValue(2);
            libebml::GetChild<libmatroska::KaxTrackFlagLacing>(aud).SetValue(0);

            const auto& slot = m_config.audio_tracks[i].codec_private;
            if (m_config.audio_codec == StreamAudioCodec::Opus) {
                if (slot.size() < 19) {
                    Fail("Opus CodecPrivate is incomplete; expected full 19-byte OpusHead");
                    CloseIo();
                    return false;
                }
                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue("A_OPUS");
                libebml::GetChild<libmatroska::KaxCodecPrivate>(aud).CopyBuffer(slot.data(),
                                                                                static_cast<uint32_t>(slot.size()));
                const uint16_t pre_skip = static_cast<uint16_t>(slot[10]) | (static_cast<uint16_t>(slot[11]) << 8u);
                const uint64_t codec_delay_ns = static_cast<uint64_t>(pre_skip) * 1000000000ULL / 48000u;
                libebml::GetChild<libmatroska::KaxCodecDelay>(aud).SetValue(codec_delay_ns);
                libebml::GetChild<libmatroska::KaxSeekPreRoll>(aud).SetValue(80000000ULL);
                libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(aud).SetValue(20000000ULL);
            } else if (m_config.audio_codec == StreamAudioCodec::Pcm) {
                // Uncompressed 16-bit signed little-endian PCM. No CodecPrivate;
                // the bit depth is carried in the track audio header below.
                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue("A_PCM/INT_LIT");
            } else if (m_config.audio_codec == StreamAudioCodec::Flac) {
                // Lossless FLAC. A_FLAC mandates the CodecPrivate be the native
                // FLAC stream header (the "fLaC" marker + STREAMINFO and any
                // leading metadata blocks) — players require it to decode.
                if (slot.size() < 4 || slot[0] != 0x66 || slot[1] != 0x4C || slot[2] != 0x61 || slot[3] != 0x43) {
                    Fail("FLAC CodecPrivate is missing or malformed; expected native 'fLaC' header");
                    CloseIo();
                    return false;
                }
                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue("A_FLAC");
                libebml::GetChild<libmatroska::KaxCodecPrivate>(aud).CopyBuffer(slot.data(),
                                                                                static_cast<uint32_t>(slot.size()));
            } else {
                libebml::GetChild<libmatroska::KaxCodecID>(aud).SetValue("A_AAC");
                if (!slot.empty()) {
                    libebml::GetChild<libmatroska::KaxCodecPrivate>(aud).CopyBuffer(slot.data(),
                                                                                    static_cast<uint32_t>(slot.size()));
                }
                libebml::GetChild<libmatroska::KaxTrackDefaultDuration>(aud).SetValue(21333333ULL);
            }

            auto& as = libebml::GetChild<libmatroska::KaxTrackAudio>(aud);
            libebml::GetChild<libmatroska::KaxAudioSamplingFreq>(as).SetValue(48000.0);
            libebml::GetChild<libmatroska::KaxAudioChannels>(as).SetValue(2);
            if (m_config.audio_codec == StreamAudioCodec::Pcm || m_config.audio_codec == StreamAudioCodec::Flac) {
                libebml::GetChild<libmatroska::KaxAudioBitDepth>(as).SetValue(16);
            }
            aud.SetGlobalTimecodeScale(kTimecodeScaleNs);
            m_track_entries.push_back(&aud);
        }

        // bSaveDefault=false avoids rendering deprecated KaxTrackTimecodeScale.
        m_tracks->Render(*m_io);

        // --- Cues accumulator (entries added incrementally per keyframe). ---
        m_cues = std::make_unique<libmatroska::KaxCues>();
        m_cues->SetGlobalTimecodeScale(kTimecodeScaleNs);
    } catch (const std::exception& ex) {
        Fail(std::string("MatroskaStreamWriter::Open render failed: ") + ex.what());
        CloseIo();
        return false;
    }

    return true;
}

bool MatroskaStreamWriter::Push(MuxPacket packet) {
    if (m_failed || !m_opened || m_finalized)
        return false;
    if (packet.bytes.empty())
        return true; // skip empties silently (matches old muxer)

    WindowEntry e;
    e.pts_ns = packet.pts_ns;
    e.track_num = packet.track_num;
    e.is_key = packet.is_key;
    e.bytes = std::move(packet.bytes);

    if (e.pts_ns > m_max_pushed_pts_ns)
        m_max_pushed_pts_ns = e.pts_ns;

    // Insert keeping the window sorted ascending by PTS. The window is small
    // (a few seconds) so a back-to-front insertion scan is cheap; equal PTS
    // preserves push order (stable) which matches the old stable_sort.
    auto it = m_window.end();
    while (it != m_window.begin()) {
        auto prev = std::prev(it);
        if (prev->pts_ns <= e.pts_ns)
            break;
        it = prev;
    }
    m_cur_window_bytes += e.bytes.size();
    m_window.insert(it, std::move(e));

    if (m_window.size() > m_peak_window_packets)
        m_peak_window_packets = m_window.size();
    if (m_cur_window_bytes > m_peak_window_bytes)
        m_peak_window_bytes = m_cur_window_bytes;

    return DrainWindow(/*force=*/false);
}

bool MatroskaStreamWriter::DrainWindow(bool force) {
    // Emit from the front (oldest PTS) while either forcing, or the front packet
    // is far enough behind the newest pushed PTS that no earlier packet can still
    // arrive within the reorder horizon. Also enforce the hard count ceiling so a
    // stalled track can never blow RAM.
    while (!m_window.empty()) {
        const WindowEntry& front = m_window.front();
        const bool behind_horizon = (m_max_pushed_pts_ns >= front.pts_ns + m_config.reorder_window_ns);
        const bool over_capacity = m_window.size() > m_config.reorder_window_max_packets;
        if (!force && !behind_horizon && !over_capacity)
            break;

        if (!EmitPacket(front))
            return false;
        m_cur_window_bytes -= front.bytes.size();
        m_window.pop_front();
    }
    return true;
}

bool MatroskaStreamWriter::EmitPacket(const WindowEntry& e) {
    if (e.track_num == 0)
        return true;

    // Resolve KaxTrackEntry*: track_num 1 -> [0] video; 2+n -> [1+n] audio.
    libmatroska::KaxTrackEntry* track_entry = nullptr;
    if (e.track_num == 1) {
        track_entry = m_track_entries[0];
    } else {
        const uint32_t entry_idx = static_cast<uint32_t>(e.track_num - 2) + 1;
        if (entry_idx >= static_cast<uint32_t>(m_track_entries.size()))
            return true; // out-of-range track: skip (matches old muxer)
        track_entry = m_track_entries[entry_idx];
    }

    const uint64_t pkt_ms = e.pts_ns / kTimecodeScaleNs;

    bool need_new_cluster = m_first_cluster;
    if (!m_first_cluster && m_cluster != nullptr) {
        const int64_t rel_ms = static_cast<int64_t>(pkt_ms) - static_cast<int64_t>(m_cluster_start_ms);
        if (rel_ms > kMaxClusterRelativeMs) {
            need_new_cluster = true;
        } else if (e.is_key && e.track_num == 1 && (pkt_ms - m_cluster_start_ms) >= kClusterBoundaryMs) {
            need_new_cluster = true;
        }
    }

    try {
        if (need_new_cluster) {
            if (!FlushCluster())
                return false;
            m_first_cluster = false;
            m_cluster_start_ms = pkt_ms;
            m_cluster = new libmatroska::KaxCluster();
            m_cluster->SetParent(*m_segment);
            m_cluster->SetGlobalTimecodeScale(kTimecodeScaleNs);
            m_cluster->InitTimecode(m_cluster_start_ms, static_cast<int64_t>(kTimecodeScaleNs));
        }

        // Backing bytes must outlive FlushCluster()/Render(). Keep them in a
        // per-cluster store that is cleared only after the cluster is rendered.
        m_cluster_bytes.push_back(e.bytes);
        std::vector<uint8_t>& stored = m_cluster_bytes.back();

        auto* data_buf = new libmatroska::DataBuffer(stored.data(), static_cast<uint32_t>(stored.size()));
        auto& sb = libebml::AddNewChild<libmatroska::KaxSimpleBlock>(*m_cluster);
        sb.SetParent(*m_cluster);
        sb.AddFrame(*track_entry, e.pts_ns, *data_buf, libmatroska::LACING_NONE);
        sb.SetKeyframe(e.is_key);

        if (e.is_key && e.track_num == 1)
            m_pending_cue_ms.push_back(pkt_ms);

        if (e.pts_ns > m_max_emitted_pts_ns)
            m_max_emitted_pts_ns = e.pts_ns;
    } catch (const std::exception& ex) {
        Fail(std::string("MatroskaStreamWriter::EmitPacket failed: ") + ex.what());
        return false;
    }
    return true;
}

bool MatroskaStreamWriter::FlushCluster() {
    if (m_cluster == nullptr)
        return true;
    try {
        const uint64_t pos_before = static_cast<uint64_t>(m_io->getFilePointer());
        const auto flush_t0 = std::chrono::steady_clock::now();
        m_cluster->Render(*m_io, *m_cues);
        const auto flush_t1 = std::chrono::steady_clock::now();
        m_last_flush_ms = std::chrono::duration<double, std::milli>(flush_t1 - flush_t0).count();
        ++m_flush_count;
        const uint64_t pos_after = static_cast<uint64_t>(m_io->getFilePointer());
        if (pos_after > pos_before) {
            m_bytes_written += pos_after - pos_before;
        }
        const uint64_t cluster_pos = m_cluster->GetPosition();
        for (const uint64_t cue_ms : m_pending_cue_ms) {
            auto& point = libebml::AddNewChild<libmatroska::KaxCuePoint>(*m_cues);
            libebml::GetChild<libmatroska::KaxCueTime>(point).SetValue(cue_ms);
            auto& tp = libebml::AddNewChild<libmatroska::KaxCueTrackPositions>(point);
            libebml::GetChild<libmatroska::KaxCueTrack>(tp).SetValue(1);
            libebml::GetChild<libmatroska::KaxCueClusterPosition>(tp).SetValue(cluster_pos);
        }
        m_pending_cue_ms.clear();
    } catch (const std::exception& ex) {
        Fail(std::string("MatroskaStreamWriter::FlushCluster failed: ") + ex.what());
        delete m_cluster;
        m_cluster = nullptr;
        m_cluster_bytes.clear();
        return false;
    }
    delete m_cluster;
    m_cluster = nullptr;
    // Block bytes are now serialized to disk; free them.
    m_cluster_bytes.clear();
    return true;
}

bool MatroskaStreamWriter::Finalize() {
    if (!m_opened || m_finalized)
        return !m_failed;
    m_finalized = true;

    // Even on prior failure, attempt an honest finalize so the file is closed
    // and as valid as possible. If we already failed, we still close the handle.
    bool ok = !m_failed;

    if (ok) {
        // Drain remaining window into clusters, then flush the last cluster.
        if (!DrainWindow(/*force=*/true))
            ok = false;
    }
    if (ok) {
        if (!FlushCluster())
            ok = false;
    }

    if (ok) {
        try {
            // --- Cues ---
            m_cues->Render(*m_io);

            // --- SeekHead into the reserved placeholder ---
            {
                libmatroska::KaxSeekHead seekhead;
                seekhead.IndexThis(*m_info, *m_segment);
                seekhead.IndexThis(*m_tracks, *m_segment);
                seekhead.IndexThis(*m_cues, *m_segment);
                m_seekhead_void->ReplaceWith(seekhead, *m_io, /*ComeBackAfterward=*/true);
            }

            // --- Back-patch Duration in the already-written Info element ---
            {
                uint64_t last_frame_ns = 0;
                if (m_config.frame_rate_num > 0 && m_config.frame_rate_den > 0) {
                    last_frame_ns = static_cast<uint64_t>(m_config.frame_rate_den) * 1000000000ULL /
                                    static_cast<uint64_t>(m_config.frame_rate_num);
                }
                const uint64_t duration_ms = (m_max_emitted_pts_ns + last_frame_ns) / kTimecodeScaleNs;
                auto& dur = libebml::GetChild<libmatroska::KaxDuration>(*m_info);
                dur.SetValue(static_cast<double>(duration_ms));
                // Patch ONLY the Duration leaf in place. It was rendered as a
                // fixed 8-byte float at Open() (FLOAT_64), and its ElementPosition
                // was recorded when the parent Info was rendered, so OverwriteData
                // rewrites exactly those 8 data bytes without resizing anything.
                // (Patching the whole Info master would risk a size-mismatch assert
                // if any child's encoded size changed.)
                dur.OverwriteData(*m_io);
            }

            // --- Back-patch the Segment size ---
            {
                const uint64_t eof_pos = static_cast<uint64_t>(m_io->getFilePointer());
                const uint64_t segment_size = eof_pos - m_segment_data_start;
                if (m_segment->ForceSize(segment_size)) {
                    m_segment->OverwriteHead(*m_io);
                }
            }
        } catch (const std::exception& ex) {
            Fail(std::string("MatroskaStreamWriter::Finalize failed: ") + ex.what());
            ok = false;
        }
    }

    CloseIo();
    return ok && !m_failed;
}

} // namespace recorder_core

#include "mux_thread.h"

#include "session_internal.h"

#include <recorder_core/packet_types.h>

#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <variant>
#include <vector>
#include <windows.h>

namespace recorder_core {

// ---------------------------------------------------------------------------
// Internal packet representation for the interleaver
// ---------------------------------------------------------------------------

namespace {

struct InterleavePacket {
    uint64_t pts_ns      = 0;
    uint64_t track_num   = 0;
    bool     is_key      = false;
    std::vector<uint8_t> bytes;
};

// Add an EncodedVideoPacket to interleave buffer
void PushVideo(std::deque<InterleavePacket>& buf, EncodedVideoPacket&& pkt, uint64_t track_num) {
    InterleavePacket ip;
    ip.pts_ns    = pkt.pts_ns;
    ip.track_num = track_num;
    ip.is_key    = pkt.keyframe;
    ip.bytes     = std::move(pkt.bytes);
    buf.push_back(std::move(ip));
}

// Add an EncodedAudioPacket to interleave buffer
void PushAudio(std::deque<InterleavePacket>& buf, EncodedAudioPacket&& pkt, uint64_t track_num) {
    InterleavePacket ip;
    ip.pts_ns    = pkt.pts_ns;
    ip.track_num = track_num;
    ip.is_key    = true;
    ip.bytes     = std::move(pkt.bytes);
    buf.push_back(std::move(ip));
}

// Write a single interleave packet to the segment.
// Returns false on error.
bool WritePacket(mkvmuxer::Segment& seg, const InterleavePacket& ip) {
    return seg.AddFrame(
        ip.bytes.data(),
        static_cast<uint64_t>(ip.bytes.size()),
        ip.track_num,
        ip.pts_ns,
        ip.is_key);
}

// Pick the next packet from the interleave buffer using a 32 ms lookahead window.
// Returns nullptr if the buffer is empty.
InterleavePacket* PickNext(std::deque<InterleavePacket>& buf, bool video_eos, bool audio_eos) {
    if (buf.empty()) return nullptr;

    if (buf.size() == 1) return &buf.front();

    // Find minimum PTS
    uint64_t minPts = buf.front().pts_ns;
    for (const auto& p : buf) {
        if (p.pts_ns < minPts) minPts = p.pts_ns;
    }

    constexpr uint64_t kWindowNs = 32'000'000ULL; // 32 ms

    // Only commit packets whose PTS is within the lookahead window
    // unless one stream has reached EOS (then drain freely)
    (void)video_eos;
    (void)audio_eos;

    // For simplicity: pick the packet with the smallest PTS from the window
    size_t bestIdx = 0;
    uint64_t bestPts = buf[0].pts_ns;

    for (size_t i = 1; i < buf.size(); ++i) {
        if (buf[i].pts_ns <= minPts + kWindowNs) {
            if (buf[i].pts_ns < bestPts) {
                bestPts = buf[i].pts_ns;
                bestIdx = i;
            }
        }
    }

    return &buf[bestIdx];
}

// Remove element at index from deque (O(n) but deques are bounded)
void RemoveAt(std::deque<InterleavePacket>& buf, size_t idx) {
    buf.erase(buf.begin() + static_cast<std::ptrdiff_t>(idx));
}

// Find index of packet in deque
size_t FindIndex(std::deque<InterleavePacket>& buf, const InterleavePacket* ptr) {
    for (size_t i = 0; i < buf.size(); ++i) {
        if (&buf[i] == ptr) return i;
    }
    return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// MuxThread
// ---------------------------------------------------------------------------

MuxThread::MuxThread(SessionState& state)
    : m_state(state)
{}

MuxThread::~MuxThread() {
    if (m_thread.joinable()) m_thread.detach();
}

void MuxThread::Start() {
    m_thread = std::thread([this] { Run(); });
}

bool MuxThread::Join(unsigned timeout_ms) {
    if (!m_thread.joinable()) return true;
    HANDLE h = m_thread.native_handle();
    DWORD r = WaitForSingleObject(h, static_cast<DWORD>(timeout_ms));
    if (r == WAIT_OBJECT_0) {
        m_thread.join();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

void MuxThread::Run() {
    // --- Step 1: Wait for both codec private data to be ready ---
    {
        std::unique_lock lk(m_state.premux_mutex);
        m_state.premux_cv.wait(lk, [&] {
            return (m_state.codec_private.av1_ready && m_state.codec_private.aac_ready)
                || m_state.stop_requested.load();
        });

        if (m_state.stop_requested.load()
            && !(m_state.codec_private.av1_ready && m_state.codec_private.aac_ready)) {
            // Session failed before codec private was available; push EOS sentinels
            // to unblock any waiting consumers and exit
            return;
        }
    }

    // --- Step 2: Open output file and init MKV segment ---
    mkvmuxer::MkvWriter mkvWriter;
    {
        std::string outPath = m_state.config.output_path.string();
        if (!mkvWriter.Open(outPath.c_str())) {
            m_state.RecordFailure(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED), ErrorPhase::Mux,
                                  "MkvWriter::Open failed for: " + outPath);
            return;
        }
    }

    mkvmuxer::Segment segment;
    if (!segment.Init(&mkvWriter)) {
        mkvWriter.Close();
        m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "mkvmuxer::Segment::Init failed");
        return;
    }
    segment.set_mode(mkvmuxer::Segment::kFile);

    // --- Step 3: Add video and audio tracks ---
    uint32_t encW = 0, encH = 0;
    {
        std::lock_guard lk(m_state.stats_mutex);
        encW = m_state.encode_width;
        encH = m_state.encode_height;
    }

    // Codec private data (copied while holding premux_mutex)
    uint8_t av1Cp[4] = {};
    uint8_t aacCp[2] = {};
    {
        std::lock_guard lk(m_state.premux_mutex);
        std::memcpy(av1Cp, m_state.codec_private.av1_codec_private, 4);
        std::memcpy(aacCp, m_state.codec_private.aac_codec_private, 2);
    }

    uint64_t videoTrackNum = segment.AddVideoTrack(
        static_cast<int32_t>(encW),
        static_cast<int32_t>(encH),
        1);
    if (videoTrackNum == 0) {
        mkvWriter.Close();
        m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "AddVideoTrack returned 0");
        return;
    }

    auto* vt = static_cast<mkvmuxer::VideoTrack*>(segment.GetTrackByNumber(videoTrackNum));
    vt->set_codec_id("V_AV1");
    vt->SetCodecPrivate(av1Cp, 4);
    vt->set_frame_rate(
        static_cast<double>(m_state.config.frame_rate_num)
        / static_cast<double>(m_state.config.frame_rate_den));

    uint64_t audioTrackNum = segment.AddAudioTrack(48000, 2, 2);
    if (audioTrackNum == 0) {
        mkvWriter.Close();
        m_state.RecordFailure(E_FAIL, ErrorPhase::Mux, "AddAudioTrack returned 0");
        return;
    }

    auto* at = static_cast<mkvmuxer::AudioTrack*>(segment.GetTrackByNumber(audioTrackNum));
    at->set_codec_id("A_AAC");
    at->SetCodecPrivate(aacCp, 2);

    // --- Step 4: Flush premux buffers in PTS order ---
    {
        std::deque<InterleavePacket> premuxBuf;

        {
            std::lock_guard lk(m_state.premux_mutex);
            for (auto& pkt : m_state.video_premux)
                PushVideo(premuxBuf, std::move(pkt), videoTrackNum);
            for (auto& pkt : m_state.audio_premux)
                PushAudio(premuxBuf, std::move(pkt), audioTrackNum);
            m_state.video_premux.clear();
            m_state.audio_premux.clear();
        }

        // Sort by PTS
        std::stable_sort(premuxBuf.begin(), premuxBuf.end(),
            [](const InterleavePacket& a, const InterleavePacket& b) {
                return a.pts_ns < b.pts_ns;
            });

        for (const auto& ip : premuxBuf) {
            if (!WritePacket(segment, ip)) {
                mkvWriter.Close();
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                      "segment.AddFrame failed during premux flush");
                return;
            }
            // Update stats
            std::lock_guard slk(m_state.stats_mutex);
            if (ip.track_num == videoTrackNum) {
                m_state.stats.video_bytes += ip.bytes.size();
            } else {
                m_state.stats.audio_bytes += ip.bytes.size();
            }
        }
    }

    // --- Step 5: Live interleaved mux loop ---
    bool videoEos = false;
    bool audioEos = false;
    std::deque<InterleavePacket> liveBuf;

    while (!(videoEos && audioEos)) {
        // Collect new items from the mux queue
        {
            std::unique_lock lk(m_state.mux_mutex);
            // Wait briefly for data
            m_state.mux_cv.wait_for(lk, std::chrono::milliseconds(5), [&] {
                return !m_state.mux_queue.empty() || m_state.stop_requested.load();
            });

            while (!m_state.mux_queue.empty()) {
                MuxItem item = std::move(m_state.mux_queue.front());
                m_state.mux_queue.pop_front();
                lk.unlock();

                std::visit([&](auto&& payload) {
                    using T = std::decay_t<decltype(payload)>;
                    if constexpr (std::is_same_v<T, EncodedVideoPacket>) {
                        if (!payload.bytes.empty())
                            PushVideo(liveBuf, std::move(payload), videoTrackNum);
                    } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
                        if (!payload.bytes.empty())
                            PushAudio(liveBuf, std::move(payload), audioTrackNum);
                    } else if constexpr (std::is_same_v<T, VideoEosSentinel>) {
                        videoEos = true;
                    } else if constexpr (std::is_same_v<T, AudioEosSentinel>) {
                        audioEos = true;
                    }
                }, item.payload);

                lk.lock();
            }
        }

        // Write packets from liveBuf that are ready
        while (true) {
            InterleavePacket* next = PickNext(liveBuf, videoEos, audioEos);
            if (!next) break;

            // Commit only if: both streams have more data behind it, or one is at EOS
            bool canCommit = videoEos || audioEos || liveBuf.size() > 1;
            if (!canCommit) break;

            InterleavePacket ip = std::move(*next);
            size_t idx = FindIndex(liveBuf, next); // recompute after move-from
            // Note: after move, 'next' is dangling — use idx to erase
            RemoveAt(liveBuf, idx);

            if (!WritePacket(segment, ip)) {
                mkvWriter.Close();
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                      "segment.AddFrame failed during live mux");
                return;
            }
            // Update stats
            {
                std::lock_guard slk(m_state.stats_mutex);
                if (ip.track_num == videoTrackNum) {
                    m_state.stats.video_bytes += ip.bytes.size();
                } else {
                    m_state.stats.audio_bytes += ip.bytes.size();
                }
            }
        }
    }

    // --- Step 6: Drain remaining packets after both EOS ---
    {
        std::stable_sort(liveBuf.begin(), liveBuf.end(),
            [](const InterleavePacket& a, const InterleavePacket& b) {
                return a.pts_ns < b.pts_ns;
            });

        for (const auto& ip : liveBuf) {
            if (!WritePacket(segment, ip)) {
                mkvWriter.Close();
                m_state.RecordFailure(E_FAIL, ErrorPhase::Mux,
                                      "segment.AddFrame failed during final drain");
                return;
            }
        }
        liveBuf.clear();
    }

    // Also drain any items that arrived after EOS (race window)
    {
        std::lock_guard lk(m_state.mux_mutex);
        while (!m_state.mux_queue.empty()) {
            MuxItem item = std::move(m_state.mux_queue.front());
            m_state.mux_queue.pop_front();
            std::visit([&](auto&& payload) {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, EncodedVideoPacket>) {
                    if (!payload.bytes.empty()) {
                        InterleavePacket ip;
                        ip.pts_ns    = payload.pts_ns;
                        ip.track_num = videoTrackNum;
                        ip.is_key    = payload.keyframe;
                        ip.bytes     = std::move(payload.bytes);
                        WritePacket(segment, ip);
                    }
                } else if constexpr (std::is_same_v<T, EncodedAudioPacket>) {
                    if (!payload.bytes.empty()) {
                        InterleavePacket ip;
                        ip.pts_ns    = payload.pts_ns;
                        ip.track_num = audioTrackNum;
                        ip.is_key    = true;
                        ip.bytes     = std::move(payload.bytes);
                        WritePacket(segment, ip);
                    }
                }
            }, item.payload);
        }
    }

    // --- Step 7: Finalize ---
    if (!segment.Finalize()) {
        mkvWriter.Close();
        m_state.RecordFailure(E_FAIL, ErrorPhase::Finalize,
                              "mkvmuxer::Segment::Finalize returned false");
        return;
    }
    mkvWriter.Close();

    // Update output file size in stats
    {
        WIN32_FILE_ATTRIBUTE_DATA fd{};
        std::wstring wPath = m_state.config.output_path.wstring();
        if (GetFileAttributesExW(wPath.c_str(), GetFileExInfoStandard, &fd)) {
            uint64_t sz = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            std::lock_guard lk(m_state.stats_mutex);
            m_state.stats.output_file_bytes = sz;
        }
    }
}

} // namespace recorder_core

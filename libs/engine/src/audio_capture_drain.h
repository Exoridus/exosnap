#pragma once

#include <atomic>

namespace exosnap::engine {

// process_packet releases any acquired buffer before returning. Observe stop
// between packets even when the source stays ready, so capture cannot starve
// teardown. A false result ends this drain without polling the source again.
template <typename Source, typename ProcessPacket>
void DrainAudioCapture(Source& source, const std::atomic<bool>& stop_requested, ProcessPacket&& process_packet) {
    while (!stop_requested.load() && source.PendingFrameCount() > 0) {
        if (!process_packet())
            break;
    }
}

} // namespace exosnap::engine

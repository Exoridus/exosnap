#pragma once

#include <exosnap/engine/packet_types.h>

#include <string>
#include <utility>
#include <vector>

namespace exosnap::engine {

// A failed encoder call can still transfer completed packets. Consume the
// batch before returning its status, retaining the encoder's error if the sink
// also fails. A rejected sink ends routing and consumes the remaining batch so
// a later drain cannot replay packets already accepted by the sink.
template <typename RoutePacket>
[[nodiscard]] bool RouteEncoderPackets(bool encoder_ok, std::vector<EncodedVideoPacket>& packets, std::string& error,
                                       RoutePacket route_packet) {
    bool routed = true;
    std::string route_error;
    for (auto& packet : packets) {
        if (!route_packet(std::move(packet), route_error)) {
            routed = false;
            break;
        }
    }
    packets.clear();
    if (encoder_ok && !routed)
        error = std::move(route_error);
    return encoder_ok && routed;
}

} // namespace exosnap::engine

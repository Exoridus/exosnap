#pragma once

// LiveVerifyProtocol.h -- the Live Verify channel's view of the shared control
// protocol.
//
// The wire format itself lives in libs/control (control/protocol.h) because the
// application and the updater speak the SAME envelope, error taxonomy and
// settle semantics; a second, separately-written protocol would be a second set
// of edge cases for a runner that drives both. Nothing about NDJSON framing,
// protocol negotiation or the error codes is ExoSnap-application-specific.
//
// What remains here is the naming: every existing include, test and call site in
// app/ says `exosnap::live_verify::...`, and the lift is meant to be
// behaviour-neutral -- so the names are re-exported rather than renamed. The
// contract documented in control/protocol.h is the contract this channel has.

#include <control/protocol.h>

namespace exosnap::live_verify {

namespace error_code = exosnap::control::error_code;

using exosnap::control::ErrorEnvelope;
using exosnap::control::ParsedRequest;
using exosnap::control::ParseFailure;
using exosnap::control::SuccessEnvelope;

using exosnap::control::kLatestProtocolVersion;
using exosnap::control::kMaxRequestBytes;
using exosnap::control::kMinimumProtocolVersion;

using exosnap::control::AllErrorCodes;
using exosnap::control::ErrorCodeForProtocol;
using exosnap::control::IsSupportedProtocol;
using exosnap::control::MakeErrorResponse;
using exosnap::control::MakeEvent;
using exosnap::control::MakeSuccessResponse;
using exosnap::control::ParseRequest;
using exosnap::control::SerializeLine;

} // namespace exosnap::live_verify

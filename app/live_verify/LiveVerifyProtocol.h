#pragma once

// LiveVerifyProtocol.h -- the wire format of the Live Verify control channel.
//
// One JSON object per line (NDJSON) over a local named pipe. Deliberately
// inspectable: a release-acceptance transcript is evidence, and evidence a human
// cannot read is evidence nobody audits. Nothing here touches Win32, Qt Quick or
// the application state -- the parser and the serializer are pure so the whole
// error surface (malformed JSON, wrong protocol version, oversized frame,
// missing id) is provable without a pipe, a window or a GPU.
//
// Shapes, protocol 1:
//   request   {"protocol":1,"id":"42","command":"record.pause","params":{}}
//   response  {"protocol":1,"id":"42","ok":true,"result":{...}}
//   error     {"protocol":1,"id":"42","ok":false,"error":{"code":"...","message":"..."}}
//   event     {"protocol":1,"event":"record.stateChanged","data":{...}}
//
// Protocol 2 keeps every one of those fields and adds four, all of them for the
// same purpose -- letting a runner assert a postcondition instead of sleeping:
//
//   response  ... "stateRevision":185, "settled":true, "state":{...}
//   error     ... "error":{..., "requires":{...}, "actual":{...}}, "stateRevision":185
//
// Both versions are answered. A protocol-1 request gets a byte-identical
// protocol-1 response to the one it got before protocol 2 existed: the new
// fields are absent, and the three error codes protocol 2 introduced are folded
// back onto `command_failed`, which is the only code a v1 client knows for a
// refused intent. The v2-only commands are not in a v1 client's registry at all,
// so asking for one under protocol 1 is `unknown_command` rather than a
// half-understood answer.
//
// `id` is echoed verbatim and is opaque to the server. Events carry no id: they
// are unsolicited, and a client correlates them by name plus payload.

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace exosnap::live_verify {

// The oldest and newest envelope this build answers. A client that asks for
// anything outside the range is rejected before any command runs, because a
// partially-understood control channel is worse than no control channel.
//
// The range exists rather than a single version because the runner, the
// acceptance suite and this server are three artifacts that do not have to be
// rebuilt in the same minute. It is NOT a promise to keep widening: protocol 1
// stays because its contract is already written down and already exercised, not
// because compatibility is a goal of its own.
inline constexpr int kMinimumProtocolVersion = 1;
inline constexpr int kLatestProtocolVersion = 2;

[[nodiscard]] constexpr bool IsSupportedProtocol(int version) noexcept {
    return version >= kMinimumProtocolVersion && version <= kLatestProtocolVersion;
}

// Largest accepted request line. A control channel needs kilobytes; anything
// past this is a client defect or an attempt to make the server allocate.
inline constexpr int kMaxRequestBytes = 64 * 1024;

// Stable, machine-readable error codes. The runner branches on these; the
// message is for a human reading the transcript and is never parsed.
namespace error_code {
inline constexpr const char* kMalformedRequest = "malformed_request";
inline constexpr const char* kRequestTooLarge = "request_too_large";
inline constexpr const char* kProtocolVersionMismatch = "protocol_version_mismatch";
inline constexpr const char* kRunIdMismatch = "run_id_mismatch";
inline constexpr const char* kHandshakeRequired = "handshake_required";
inline constexpr const char* kAlreadyHandshaken = "already_handshaken";
inline constexpr const char* kUnknownCommand = "unknown_command";
inline constexpr const char* kInvalidParams = "invalid_params";
// Protocol 1's single code for every refused intent. Protocol 2 splits it into
// the three below; a protocol-1 response still carries this one, so the v1
// contract is unchanged.
inline constexpr const char* kCommandFailed = "command_failed";
// [protocol 2] The command exists but the current product state is the wrong
// one for it: edit.seek with no session open, record.pause while Ready. A
// runner reads this as "my test drove the app into the wrong state".
inline constexpr const char* kInvalidState = "invalid_state";
// [protocol 2] The state would be right, and a product rule refuses anyway:
// record.start under an open recovery/crash/recording-error surface, or with a
// diagnostics blocker standing. A runner reads this as "the product said no",
// which is a result, not a test defect.
inline constexpr const char* kBlocked = "blocked";
// [protocol 2] The attempt ran and did not achieve its postcondition -- a reveal
// target that exists but could not be brought into view, a window that vanished
// between selection and access.
inline constexpr const char* kOperationFailed = "operation_failed";
inline constexpr const char* kUnavailable = "unavailable";
inline constexpr const char* kDispatchTimeout = "dispatch_timeout";
} // namespace error_code

// Every code this build can answer with, sorted. Reported by system.hello and
// ipc.describe so a client never has to discover a code by being surprised.
[[nodiscard]] QStringList AllErrorCodes();

// The three protocol-2 codes have no protocol-1 spelling. A v1 client is told
// `command_failed`, which is exactly what it was told before those codes
// existed -- the REFUSAL is still reported, only its category is coarser.
[[nodiscard]] QString ErrorCodeForProtocol(const QString& code, int protocol);

struct ParsedRequest {
    int protocol = kMinimumProtocolVersion;
    QString id;
    QString command;
    QJsonObject params;
    // [protocol 2] `includeState: true` on the request. Off by default so the
    // ordinary response stays small; a runner that wants the whole product
    // state in the same round trip asks for it.
    bool include_state = false;
};

struct ParseFailure {
    // Empty when the line was so malformed that no id could be recovered. The
    // server still answers -- a client that never gets a response for a request
    // it believes it sent cannot tell a hang from a rejection.
    QString id;
    // The version to answer the failure in. A version mismatch is answered in
    // the newest version this build speaks, because by definition there is no
    // agreed one.
    int protocol = kMinimumProtocolVersion;
    QString code;
    QString message;
};

// Parses one NDJSON line. Returns false and fills `failure` on any defect:
// non-object JSON, absent/unsupported protocol version, missing or non-string
// command, non-object params. `id` is recovered whenever the line was at least
// valid JSON with a string/number id, so the failure can still be correlated.
[[nodiscard]] bool ParseRequest(const QByteArray& line, ParsedRequest* out, ParseFailure* failure);

// A success, with the protocol-2 additions expressed as options rather than as
// sentinel values: a field that is absent means "this version does not carry
// it", which is not the same statement as "its value is zero".
struct SuccessEnvelope {
    int protocol = kMinimumProtocolVersion;
    QString id;
    QJsonObject result;
    std::optional<std::uint64_t> state_revision;
    std::optional<bool> settled;
    std::optional<QJsonObject> state;
};

struct ErrorEnvelope {
    int protocol = kMinimumProtocolVersion;
    QString id;
    QString code;
    QString message;
    // The precondition that was not met, and what was actually observed, in the
    // same shape. This is what stops a runner from parsing `message` to answer
    // "why was it refused" -- the single most common reason a control protocol
    // ends up with clients that break on a wording change.
    QJsonObject requirements;
    QJsonObject actual;
    std::optional<std::uint64_t> state_revision;
};

[[nodiscard]] QJsonObject MakeSuccessResponse(const SuccessEnvelope& envelope);
[[nodiscard]] QJsonObject MakeErrorResponse(const ErrorEnvelope& envelope);
[[nodiscard]] QJsonObject MakeEvent(int protocol, const QString& name, QJsonObject data,
                                    std::optional<std::uint64_t> state_revision = std::nullopt);

// Compact JSON plus a trailing '\n'. The only place a frame boundary is
// decided, so a serializer that ever emitted indented JSON would break every
// reader at once rather than subtly.
[[nodiscard]] QByteArray SerializeLine(const QJsonObject& object);

} // namespace exosnap::live_verify

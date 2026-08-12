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
// Shapes:
//   request   {"protocol":1,"id":"42","command":"record.pause","params":{}}
//   response  {"protocol":1,"id":"42","ok":true,"result":{...}}
//   error     {"protocol":1,"id":"42","ok":false,"error":{"code":"...","message":"..."}}
//   event     {"protocol":1,"event":"record.stateChanged","data":{...}}
//
// `id` is echoed verbatim and is opaque to the server. Events carry no id: they
// are unsolicited, and a client correlates them by name plus payload.

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace exosnap::live_verify {

// Bumped only for an incompatible change. A client that sends a different
// version is rejected before any command runs, because a partially-understood
// control channel is worse than no control channel.
inline constexpr int kProtocolVersion = 1;

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
inline constexpr const char* kCommandFailed = "command_failed";
inline constexpr const char* kUnavailable = "unavailable";
inline constexpr const char* kDispatchTimeout = "dispatch_timeout";
} // namespace error_code

struct ParsedRequest {
    QString id;
    QString command;
    QJsonObject params;
};

struct ParseFailure {
    // Empty when the line was so malformed that no id could be recovered. The
    // server still answers -- a client that never gets a response for a request
    // it believes it sent cannot tell a hang from a rejection.
    QString id;
    QString code;
    QString message;
};

// Parses one NDJSON line. Returns false and fills `failure` on any defect:
// non-object JSON, wrong/absent protocol version, missing or non-string
// command, non-object params. `id` is recovered whenever the line was at least
// valid JSON with a string/number id, so the failure can still be correlated.
[[nodiscard]] bool ParseRequest(const QByteArray& line, ParsedRequest* out, ParseFailure* failure);

[[nodiscard]] QJsonObject MakeSuccessResponse(const QString& id, QJsonObject result);
[[nodiscard]] QJsonObject MakeErrorResponse(const QString& id, const QString& code, const QString& message);
[[nodiscard]] QJsonObject MakeEvent(const QString& name, QJsonObject data);

// Compact JSON plus a trailing '\n'. The only place a frame boundary is
// decided, so a serializer that ever emitted indented JSON would break every
// reader at once rather than subtly.
[[nodiscard]] QByteArray SerializeLine(const QJsonObject& object);

} // namespace exosnap::live_verify
